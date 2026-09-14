"""Everything a tool handler needs, plus the sync logic that keeps the session
cache and the index consistent with the editor."""
import logging
from typing import Any, Dict, List, Optional

from .bridge import PluginClient, PluginError, PluginUnavailable
from .index import ProjectIndex
from .session import Session
from .tokens import TokenMeter

log = logging.getLogger(__name__)


class AgentContext:
    def __init__(self, config, client: Optional[PluginClient] = None):
        self.config = config
        self.client = client or PluginClient(config.project_dir, config.endpoint_file, config.port, config.token, config.timeout)
        self.session = Session()
        self.index = ProjectIndex(self.client, config, self.session)
        self.meter = TokenMeter()
        self.capabilities: Dict[str, str] = {}

    # ---- sync --------------------------------------------------------------
    def sync(self):
        """Pull change events since the last sync and invalidate caches accordingly."""
        try:
            result = self.client.call("system.changes", {"since": self.session.seq, "limit": 500})
        except (PluginError, PluginUnavailable):
            return
        changes = result.get("changes", [])
        if result.get("overflow"):
            self.session.cache.clear()
            self.index.last_refresh = 0.0
        self.session.apply_changes(changes, result.get("seq", self.session.seq))
        self.index.apply_changes(changes)
        self.index.last_seq = self.session.seq

    # ---- assets ------------------------------------------------------------
    def resolve_asset(self, ref: str) -> str:
        """A# id, package path, object path or unique short name -> package path."""
        ref = (ref or "").strip()
        if not ref:
            raise ValueError("Missing asset reference.")
        if self.session.is_asset_id(ref):
            path = self.session.path_for(ref)
            if not path:
                raise ValueError(f"Unknown asset id {ref}. Use search() first.")
            return path
        if ref.startswith("/"):
            if "." in ref.rsplit("/", 1)[-1]:
                ref = ref.rsplit(".", 1)[0]
            return ref
        matches = self.index.resolve_name(ref)
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            raise ValueError(f"Ambiguous name '{ref}': " + ", ".join(matches[:6]))
        if not self.index.assets:
            try:
                self.index.refresh_shallow(force=True)
            except (PluginError, PluginUnavailable):
                pass
            matches = self.index.resolve_name(ref)
            if len(matches) == 1:
                return matches[0]
        # Let the plugin try (it can search the registry by name).
        info = self.client.call("assets.info", {"asset": ref})
        return info["path"]

    def resolve_assets(self, refs) -> List[str]:
        if isinstance(refs, str):
            refs = [refs]
        out = []
        for ref in refs or []:
            if self.session.is_asset_id(ref) and ref.upper().startswith("WS"):
                continue
            if ref.upper().startswith("WS") and ref.upper() in self.session.working_sets:
                out.extend(self.session.working_sets[ref.upper()])
            else:
                out.append(self.resolve_asset(ref))
        return list(dict.fromkeys(out))

    def aid(self, path: str) -> str:
        return self.session.asset_id(path)

    def label(self, path: str) -> str:
        """'A12 BP_Player'"""
        return f"{self.aid(path)} {path.rsplit('/', 1)[-1]}"

    # ---- cached plugin calls -------------------------------------------------
    def cached(self, path: str, key: str, cmd: str, params: dict):
        if self.config.cache:
            hit = self.session.cache_get(path, key)
            if hit is not None:
                return hit
        result = self.client.call(cmd, params)
        if self.config.cache:
            self.session.cache_put(path, key, result)
        return result

    def get_capabilities(self) -> Dict[str, str]:
        if not self.capabilities:
            self.capabilities = self.client.call("system.capabilities").get("capabilities", {})
        return self.capabilities
