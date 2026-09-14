"""Project index: shallow entries from the Asset Registry (cheap, no loading)
plus deep entries (functions/variables/calls/reads/writes/widgets) fetched
lazily per Blueprint. Persisted under <project>/.unreal-agent/cache/ and
refreshed incrementally using file timestamps + editor change events."""
import json
import logging
import os
import re
import time
from collections import Counter
from typing import Any, Dict, Iterable, List, Optional

log = logging.getLogger(__name__)

INDEX_VERSION = 2
DEEP_FIELDS = ("functions", "variables", "events", "widgets", "components", "macros", "interfaces")


def _norm(text: str) -> str:
    return re.sub(r"[^a-z0-9]", "", text.lower())


def _score(name: str, query: str) -> int:
    """Higher is better. 0 = no match."""
    n, q = name.lower(), query.lower()
    if n == q:
        return 100
    if n.startswith(q):
        return 80
    if q in n:
        return 60
    nn, nq = _norm(name), _norm(query)
    if nq and nq in nn:
        return 50
    # all query words present (Inventory Slot -> WBP_InventorySlot)
    words = [w for w in re.split(r"[\s_]+", q) if w]
    if len(words) > 1 and all(w in n for w in words):
        return 40
    return 0


class ProjectIndex:
    def __init__(self, client, config, session):
        self.client = client
        self.config = config
        self.session = session
        self.path = os.path.join(config.cache_dir, "index.json")
        self.conventions_path = os.path.join(config.agent_dir, "conventions.json")
        self.assets: Dict[str, Dict[str, Any]] = {}
        self.last_refresh = 0.0
        self.last_seq = 0
        self.dirty = False
        self.load()

    # ---- persistence -------------------------------------------------------
    def load(self):
        try:
            with open(self.path, "r", encoding="utf-8") as handle:
                data = json.load(handle)
            if data.get("version") == INDEX_VERSION and data.get("project") == self.config.project_name:
                self.assets = data.get("assets", {})
                self.last_seq = int(data.get("seq", 0))
        except (OSError, ValueError):
            self.assets = {}

    def save(self):
        if not self.dirty:
            return
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            with open(self.path, "w", encoding="utf-8") as handle:
                json.dump({"version": INDEX_VERSION, "project": self.config.project_name, "saved": time.time(),
                           "seq": self.last_seq, "assets": self.assets}, handle)
            self.dirty = False
        except OSError as exc:
            log.warning("Could not save index: %s", exc)

    # ---- refresh -----------------------------------------------------------
    def refresh_shallow(self, force: bool = False) -> Dict[str, int]:
        """Pull the asset list (with mtimes) and drop deep data for changed assets."""
        now = time.time()
        if not force and self.assets and now - self.last_refresh < self.config.index_refresh_seconds:
            return {"scanned": len(self.assets), "changed": 0, "skipped": 1}
        result = self.client.call("assets.list", {"paths": self.config.content_roots, "blueprints_only": True, "mtime": True, "limit": 100000})
        seen = set()
        changed = 0
        for item in result.get("assets", []):
            path = item["path"]
            seen.add(path)
            existing = self.assets.get(path)
            entry = {
                "name": item.get("name", ""),
                "class": item.get("class", ""),
                "parent": item.get("parent", ""),
                "native_parent": item.get("native_parent", ""),
                "interfaces": item.get("interfaces", []),
                "bp_type": item.get("bp_type", ""),
                "mtime": item.get("mtime", 0),
                "dirty": bool(item.get("dirty")),
            }
            if existing:
                same = existing.get("mtime") == entry["mtime"] and not entry["dirty"] and not existing.get("dirty")
                if same and existing.get("deep"):
                    entry["deep"] = existing["deep"]
                    entry["deep_version"] = existing.get("deep_version")
                else:
                    changed += 1
            else:
                changed += 1
            self.assets[path] = entry
        removed = [path for path in self.assets if path not in seen]
        for path in removed:
            del self.assets[path]
        self.last_refresh = now
        self.dirty = self.dirty or changed > 0 or bool(removed)
        self.save()
        return {"scanned": len(seen), "changed": changed, "removed": len(removed)}

    def apply_changes(self, changes: Iterable[dict]):
        for change in changes:
            path = change.get("asset")
            if not path:
                continue
            entry = self.assets.get(path)
            if change.get("type") == "removed":
                self.assets.pop(path, None)
                self.dirty = True
            elif entry is not None:
                entry.pop("deep", None)
                entry["dirty"] = True
                self.dirty = True
            else:
                # new asset: force a shallow refresh next time
                self.last_refresh = 0.0

    def deep_index(self, paths: Iterable[str], max_count: Optional[int] = None) -> Dict[str, int]:
        done = 0
        failed = 0
        for path in paths:
            if max_count is not None and done >= max_count:
                break
            entry = self.assets.get(path)
            if entry is None:
                entry = self.assets.setdefault(path, {"name": path.rsplit("/", 1)[-1], "class": "Blueprint"})
            if entry.get("deep"):
                continue
            try:
                data = self.client.call("blueprint.index_entry", {"asset": path})
            except Exception as exc:  # noqa: BLE001
                log.debug("deep index failed for %s: %s", path, exc)
                failed += 1
                continue
            entry["deep"] = {key: data.get(key, []) for key in DEEP_FIELDS + ("calls", "reads", "writes", "dependencies")}
            entry["deep"]["kind"] = data.get("kind", "")
            entry["parent"] = data.get("parent", entry.get("parent", ""))
            entry["dirty"] = False
            entry["deep_version"] = data.get("version", 0)
            done += 1
            self.dirty = True
        self.save()
        return {"indexed": done, "failed": failed}

    def deep_coverage(self) -> Dict[str, int]:
        total = len(self.assets)
        deep = sum(1 for entry in self.assets.values() if entry.get("deep"))
        return {"total": total, "deep": deep}

    def ensure_deep(self, candidates: List[str], reason: str = "") -> int:
        """Lazily deep-index a bounded set of assets (auto mode)."""
        if not self.config.auto_deep_index:
            return 0
        pending = [path for path in candidates if not self.assets.get(path, {}).get("deep")]
        if not pending:
            return 0
        result = self.deep_index(pending, max_count=self.config.max_auto_deep_index)
        return result["indexed"]

    def candidates_for(self, query: str) -> List[str]:
        """Blueprints ordered by how likely they are to contain `query`: name hits,
        camel-case word hits in name/path, then everything else (bounded by config)."""
        words = [w.lower() for w in re.findall(r"[A-Z]?[a-z]+|[A-Z]+(?![a-z])|\d+", query) if len(w) > 2]
        q = query.lower()
        scored = []
        for path, entry in self.assets.items():
            if entry.get("deep"):
                continue
            name = entry.get("name", "").lower()
            score = 0
            if q in name:
                score = 3
            elif any(w in name for w in words):
                score = 2
            elif any(w in path.lower() for w in words):
                score = 1
            scored.append((-score, path))
        scored.sort()
        return [path for _, path in scored]

    # ---- queries -----------------------------------------------------------
    def blueprints(self) -> List[str]:
        return list(self.assets.keys())

    def resolve_name(self, name: str) -> List[str]:
        name_l = name.lower()
        return [path for path, entry in self.assets.items() if entry.get("name", "").lower() == name_l]

    def search(self, query: str, kind: str = "any", limit: int = 20, paths_filter: Optional[str] = None) -> List[Dict[str, Any]]:
        query = query.strip()
        kind = (kind or "any").lower()
        results: List[Dict[str, Any]] = []
        for path, entry in self.assets.items():
            if paths_filter and not path.startswith(paths_filter):
                continue
            name = entry.get("name", "")
            if kind in ("any", "asset", "blueprint", "widget_blueprint"):
                if kind == "widget_blueprint" and entry.get("class") != "WidgetBlueprint":
                    pass
                else:
                    score = _score(name, query) or (_score(path, query) // 2 if "/" in query else 0)
                    if score:
                        results.append({"score": score + 5, "type": "asset", "path": path, "name": name,
                                        "class": entry.get("class", ""), "parent": entry.get("parent", "")})
            deep = entry.get("deep")
            if not deep or kind == "asset":
                continue
            for field, label in (("functions", "function"), ("variables", "variable"), ("events", "event"),
                                 ("widgets", "widget"), ("components", "component"), ("macros", "macro")):
                if kind not in ("any", label, label + "s"):
                    continue
                for item in deep.get(field, []):
                    item_name = item.split(":", 1)[0]
                    score = _score(item_name, query)
                    if score:
                        results.append({"score": score, "type": label, "path": path, "name": item_name,
                                        "detail": item, "asset_name": name, "class": entry.get("class", "")})
        results.sort(key=lambda r: (-r["score"], r["type"] != "asset", r["name"], r["path"]))
        return results[:limit] if limit else results

    def who_calls(self, function: str) -> List[Dict[str, Any]]:
        target = function.lower()
        out = []
        for path, entry in self.assets.items():
            deep = entry.get("deep")
            if not deep:
                continue
            for call in deep.get("calls", []):
                cls, _, func = call.rpartition(".")
                if func.lower() == target or call.lower() == target:
                    out.append({"path": path, "name": entry.get("name"), "call": call})
        return out

    def who_accesses(self, variable: str, mode: str) -> List[Dict[str, Any]]:
        target = variable.lower()
        out = []
        for path, entry in self.assets.items():
            deep = entry.get("deep")
            if not deep:
                continue
            for ref in deep.get(mode, []):
                cls, _, var = ref.rpartition(".")
                if var.lower() == target or ref.lower() == target:
                    out.append({"path": path, "name": entry.get("name"), "ref": ref})
        return out

    def who_defines(self, name: str, field: str) -> List[Dict[str, Any]]:
        target = name.lower()
        out = []
        for path, entry in self.assets.items():
            deep = entry.get("deep")
            if not deep:
                continue
            for item in deep.get(field, []):
                if item.split(":", 1)[0].lower() == target:
                    out.append({"path": path, "name": entry.get("name"), "detail": item})
        return out

    # ---- conventions -------------------------------------------------------
    def conventions(self, recompute: bool = False) -> Dict[str, Any]:
        existing = None
        try:
            with open(self.conventions_path, "r", encoding="utf-8") as handle:
                existing = json.load(handle)
        except (OSError, ValueError):
            existing = None
        if existing and (existing.get("locked") or not recompute):
            return existing
        prefixes: Dict[str, Counter] = {}
        folders: Counter = Counter()
        for path, entry in self.assets.items():
            name = entry.get("name", "")
            cls = entry.get("class", "?")
            if "_" in name:
                prefixes.setdefault(cls, Counter())[name.split("_", 1)[0] + "_"] += 1
            folders[path.rsplit("/", 1)[0]] += 1
            deep = entry.get("deep") or {}
            for widget in deep.get("widgets", []):
                wname, _, wclass = widget.partition(":")
                if "_" in wname:
                    prefixes.setdefault("Widget." + wclass, Counter())[wname.split("_", 1)[0] + "_"] += 1
        result = {"locked": False, "generated": time.time(), "prefixes": {}, "folders": {}}
        for cls, counter in prefixes.items():
            prefix, count = counter.most_common(1)[0]
            total = sum(counter.values())
            if count >= 2 and count / total >= 0.5:
                result["prefixes"][cls] = prefix
        for folder, count in folders.most_common(12):
            result["folders"][folder] = count
        try:
            os.makedirs(os.path.dirname(self.conventions_path), exist_ok=True)
            with open(self.conventions_path, "w", encoding="utf-8") as handle:
                json.dump(result, handle, indent=2)
        except OSError:
            pass
        return result
