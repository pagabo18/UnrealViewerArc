"""In-memory stand-in for the ClaudeBlueprintAgent editor plugin.

It speaks the same command/JSON contract as the C++ plugin (see docs/API.md)
so the Python server, formatters and token budgets can be tested without an
editor. It is deliberately simplified: no real Kismet schema, but pin/exec
semantics are close enough for the server's logic.
"""
import copy
import itertools
import time
import uuid
from typing import Any, Dict, List, Optional

from unreal_agent_mcp.bridge import PluginError


def guid() -> str:
    return uuid.uuid4().hex.upper()


def pin(name, direction, type_="exec", default=None):
    p = {"id": guid(), "name": name, "dir": direction, "type": type_}
    if default is not None:
        p["default"] = default
    return p


class FakePlugin:
    """Call with .call(cmd, params) exactly like PluginClient."""

    def __init__(self, project: Optional[dict] = None):
        self.project = project or make_menu_project()
        self.seq = 0
        self.changes: List[dict] = []
        self.undo_stack: List[dict] = []
        self.redo_stack: List[dict] = []
        self.calls: List[tuple] = []
        self.compiled: List[str] = []
        self.saved: List[str] = []
        self.calls_count = 0
        self.total_ms = 0.0
        self.last_seq = 0
        self.mtime_base = int(time.time())
        self.log_entries: List[dict] = []

    # PluginClient-compatible surface -------------------------------------
    def health(self):
        return {"ok": True, "engine": "5.5.4", "plugin": "0.1.0", "seq": self.seq}

    def is_available(self):
        return True

    def discover(self):
        return True

    def call(self, cmd: str, params: Optional[dict] = None, timeout=None) -> Dict[str, Any]:
        params = params or {}
        self.calls.append((cmd, params))
        self.calls_count += 1
        handler = getattr(self, "cmd_" + cmd.replace(".", "_"), None)
        if handler is None:
            raise PluginError("UNKNOWN_COMMAND", f"Unknown command '{cmd}'.", cmd=cmd)
        return handler(params)

    # helpers ---------------------------------------------------------------
    def _record(self, type_, asset, extra=""):
        self.seq += 1
        self.changes.append({"seq": self.seq, "type": type_, "asset": asset, "extra": extra})

    def _snapshot(self):
        self.undo_stack.append(copy.deepcopy(self.project))
        self.redo_stack.clear()

    def _bp(self, ref) -> dict:
        path = ref
        if "." in ref.rsplit("/", 1)[-1]:
            path = ref.rsplit(".", 1)[0]
        if path in self.project:
            return self.project[path]
        if not ref.startswith("/"):
            matches = [p for p, b in self.project.items() if b["name"].lower() == ref.lower()]
            if len(matches) == 1:
                return self.project[matches[0]]
            if len(matches) > 1:
                raise PluginError("AMBIGUOUS", f"Ambiguous asset name '{ref}'")
        raise PluginError("NOT_FOUND", f"Asset not found: {ref}")

    def _graph(self, bp, name):
        name = name or "EventGraph"
        for gname, graph in bp["graphs"].items():
            if gname.lower() == name.lower():
                return gname, graph
        raise PluginError("NOT_FOUND", f"Graph '{name}' not found in {bp['name']}.")

    def _node(self, bp, ref):
        for gname, graph in bp["graphs"].items():
            if ref.upper() in graph["nodes"]:
                return gname, graph, graph["nodes"][ref.upper()]
        raise PluginError("NOT_FOUND", f"Node {ref} not found.")

    def _pin(self, node, name, direction):
        if direction == "in" and (not name or name in ("exec", "in")):
            name = "execute"
        if direction == "out" and (not name or name in ("exec", "out")):
            name = "then"
        for p in node["pins"]:
            if p["dir"] == direction and p["name"].lower() == name.lower():
                return p
        if name in ("execute", "then"):
            execs = [p for p in node["pins"] if p["dir"] == direction and p["type"] == "exec"]
            if len(execs) == 1:
                return execs[0]
        raise PluginError("NOT_FOUND", f"Pin '{name}' not found on {node['title']}. Available: " + ", ".join(p['name'] for p in node['pins'] if p['dir'] == direction))

    def _link(self, from_node, from_pin, to_node, to_pin, force):
        existing = to_pin.get("links", [])
        if existing and not force:
            other = existing[0]
            raise PluginError("CONFLICT", f"Pin {to_pin['name']} already connected ({other['node']}.{other['pin']}).", {"existing": f"{other['node']}.{other['pin']}", "hint": "Pass force=true to replace the existing link."})
        replaced = None
        if existing:
            replaced = f"{existing[0]['node']}.{existing[0]['pin']}"
            for link in existing:
                src = self._find_node_any(link["node"])
                for p in src["pins"]:
                    p["links"] = [l for l in p.get("links", []) if not (l["node"] == to_node["guid"] and l["pin"] == to_pin["name"])]
            to_pin["links"] = []
        from_pin.setdefault("links", []).append({"node": to_node["guid"], "pin": to_pin["name"]})
        to_pin.setdefault("links", []).append({"node": from_node["guid"], "pin": from_pin["name"]})
        info = {"from": f"{from_node['guid']}.{from_pin['name']}", "to": f"{to_node['guid']}.{to_pin['name']}"}
        if replaced:
            info["replaced"] = replaced
        return info

    def _find_node_any(self, g):
        for bp in self.project.values():
            for graph in bp["graphs"].values():
                if g in graph["nodes"]:
                    return graph["nodes"][g]
        raise PluginError("NOT_FOUND", f"Node {g} not found.")

    def _unlink_all(self, node):
        for p in node["pins"]:
            for link in p.get("links", []):
                other = self._find_node_any(link["node"])
                for op in other["pins"]:
                    op["links"] = [l for l in op.get("links", []) if l["node"] != node["guid"]]
            p["links"] = []

    # system ---------------------------------------------------------------
    def cmd_system_ping(self, params):
        return {"project": "FakeProject", "engine": "5.5.4", "adapter": "UE55", "plugin": "0.1.0", "seq": self.seq, "pie": False, "sourceControl": "none", "git": True}

    def cmd_system_capabilities(self, params):
        return {"capabilities": {"Blueprint.ReadGraph": "supported", "Blueprint.EditGraph": "supported", "UMG.CloneWidget": "supported", "UMG.EditAnimation": "unsupported"}, "commands": ["system.ping"]}

    def cmd_system_changes(self, params):
        since = int(params.get("since", 0))
        items = [c for c in self.changes if c["seq"] > since]
        return {"changes": items[-int(params.get("limit", 200)):], "seq": self.seq, "truncated": False, "overflow": False}

    def cmd_system_log(self, params):
        return {"errors": len([e for e in self.log_entries if e["level"] == "error"]), "warnings": 0, "matched": len(self.log_entries), "entries": self.log_entries[-int(params.get("limit", 20)):], "seq": 0, "logFile": "/fake/Saved/Logs/Fake.log"}

    def cmd_system_undo(self, params):
        steps = int(params.get("steps", 1))
        done = 0
        titles = []
        for _ in range(steps):
            if not self.undo_stack:
                break
            self.redo_stack.append(copy.deepcopy(self.project))
            self.project = self.undo_stack.pop()
            done += 1
            titles.append("Claude: edit")
        for path in self.project:
            self._record("modified", path)
        return {"undone": done, "titles": titles, "undoStack": ["Claude: edit"] if self.undo_stack else []}

    def cmd_system_redo(self, params):
        done = 0
        for _ in range(int(params.get("steps", 1))):
            if not self.redo_stack:
                break
            self.undo_stack.append(copy.deepcopy(self.project))
            self.project = self.redo_stack.pop()
            done += 1
        return {"redone": done}

    # assets -----------------------------------------------------------------
    def cmd_assets_list(self, params):
        items = []
        contains = params.get("contains", "")
        for path, bp in sorted(self.project.items()):
            if contains and contains.lower() not in bp["name"].lower():
                continue
            item = {"path": path, "name": bp["name"], "class": "WidgetBlueprint" if bp["kind"] == "Widget" else "Blueprint", "parent": bp["parent"],
                    "interfaces": bp.get("interfaces", []), "bp_type": "Normal"}
            if params.get("mtime"):
                item["mtime"] = bp.get("mtime", self.mtime_base)
            if bp.get("dirty"):
                item["dirty"] = True
            items.append(item)
        return {"assets": items, "matched": len(items), "scanned": len(self.project), "truncated": False}

    def cmd_assets_info(self, params):
        bp = self._bp(params["asset"])
        path = bp["path"]
        refs = [p for p, other in self.project.items() if path in other.get("dependencies", [])]
        return {"path": path, "name": bp["name"], "class": "WidgetBlueprint" if bp["kind"] == "Widget" else "Blueprint", "parent": bp["parent"],
                "dependencies": bp.get("dependencies", []), "referencers": refs, "dirty": bool(bp.get("dirty"))}

    def cmd_assets_referencers(self, params):
        path = params["asset"]
        return {"asset": path, "referencers": [p for p, other in self.project.items() if path in other.get("dependencies", [])]}

    def cmd_assets_dependencies(self, params):
        bp = self._bp(params["asset"])
        return {"asset": bp["path"], "dependencies": bp.get("dependencies", [])}

    def cmd_assets_source_control(self, params):
        return {"provider": "none", "assets": [{"asset": a, "exists": True, "dirty": bool(self.project.get(a, {}).get("dirty"))} for a in params.get("assets", [])]}

    def cmd_assets_open(self, params):
        bp = self._bp(params["asset"])
        return {"asset": bp["path"], "name": bp["name"]}

    def cmd_assets_create_blueprint(self, params):
        path = params["path"]
        if path in self.project:
            raise PluginError("CONFLICT", f"Asset {path} already exists.")
        self._snapshot()
        widget = params.get("kind") == "widget" or params.get("parent", "").endswith("UserWidget")
        bp = make_blueprint(path, params.get("parent", "Actor"), kind="Widget" if widget else "Actor")
        if widget:
            bp["widgets"] = [{"name": "CanvasPanel", "class": "CanvasPanel", "parent": None, "props": {}, "slot": None}]
        self.project[path] = bp
        self._record("added", path)
        return {"asset": path, "name": bp["name"], "parent": params.get("parent", "Actor"), "widget": widget}

    # blueprint --------------------------------------------------------------
    def cmd_blueprint_summary(self, params):
        bp = self._bp(params["asset"])
        nodes = sum(len(g["nodes"]) for g in bp["graphs"].values())
        functions = [g for g, graph in bp["graphs"].items() if graph["kind"] == "function"]
        out = {"asset": bp["path"], "name": bp["name"], "kind": bp["kind"], "parent": bp["parent"], "parent_spec": "/Script/Engine." + bp["parent"],
               "functions": len(functions), "variables": len(bp["variables"]), "macros": 0, "event_graphs": 1, "events": len(self._events(bp)), "nodes": nodes,
               "interfaces": bp.get("interfaces", []), "referencers": self.cmd_assets_referencers({"asset": bp["path"]})["referencers"][:12],
               "dependencies": bp.get("dependencies", [])[:12], "status": "Error" if bp.get("error") else "OK", "dirty": bool(bp.get("dirty"))}
        if bp["kind"] == "Widget":
            out["widgets"] = len(bp.get("widgets", []))
            out["animations"] = len(bp.get("animations", []))
        out["components"] = len(bp.get("components", []))
        return out

    def _events(self, bp):
        events = []
        for node in bp["graphs"]["EventGraph"]["nodes"].values():
            if node["kind"] in ("Event", "CustomEvent", "ComponentBoundEvent"):
                events.append(node["member"] + (" (custom)" if node["kind"] == "CustomEvent" else ""))
        return sorted(events)

    def _signature(self, graph):
        return {"inputs": graph.get("inputs", []), "outputs": graph.get("outputs", []), "pure": graph.get("pure", False)}

    def cmd_blueprint_structure(self, params):
        bp = self._bp(params["asset"])
        functions = []
        for name, graph in bp["graphs"].items():
            if graph["kind"] == "function":
                functions.append({"name": name, "nodes": len(graph["nodes"]), **self._signature(graph)})
        variables = []
        for var in bp["variables"]:
            item = {"name": var["name"], "type": var["type"]}
            if var.get("category"):
                item["category"] = var["category"]
            if var.get("default") not in (None, ""):
                item["default"] = var["default"]
            if var.get("replicated"):
                item["replicated"] = True
            variables.append(item)
        return {"asset": bp["path"], "name": bp["name"], "kind": bp["kind"], "parent": bp["parent"], "functions": functions, "macros": [],
                "event_graphs": [{"name": "EventGraph", "kind": "event", "nodes": len(bp["graphs"]["EventGraph"]["nodes"])}],
                "events": self._events(bp), "variables": variables, "interfaces": bp.get("interfaces", []), "delegates": [],
                "components": {"components": bp.get("components", [])}, "referencers": self.cmd_assets_referencers({"asset": bp["path"]})["referencers"], "dependencies": bp.get("dependencies", [])}

    def cmd_blueprint_index_entry(self, params):
        bp = self._bp(params["asset"])
        calls, reads, writes = set(), set(), set()
        for graph in bp["graphs"].values():
            for node in graph["nodes"].values():
                if node["kind"] == "CallFunction":
                    calls.add(f"{node.get('member_class', 'self')}.{node['member']}")
                elif node["kind"] == "VariableGet":
                    reads.add(f"self.{node['member']}")
                elif node["kind"] == "VariableSet":
                    writes.add(f"self.{node['member']}")
        return {"asset": bp["path"], "name": bp["name"], "kind": bp["kind"], "parent": bp["parent"],
                "functions": [g for g, graph in bp["graphs"].items() if graph["kind"] == "function"], "macros": [],
                "variables": [f"{v['name']}:{v['type']}" for v in bp["variables"]], "events": self._events(bp), "interfaces": bp.get("interfaces", []),
                "components": [f"{c['name']}:{c['class']}" for c in bp.get("components", [])],
                "widgets": [f"{w['name']}:{w['class']}" for w in bp.get("widgets", [])], "calls": sorted(calls), "reads": sorted(reads), "writes": sorted(writes),
                "dependencies": bp.get("dependencies", []), "version": self.seq}

    def cmd_blueprint_components(self, params):
        bp = self._bp(params["asset"])
        return {"asset": bp["path"], "components": bp.get("components", [])}

    def cmd_blueprint_variable(self, params):
        bp = self._bp(params["asset"])
        op = params["op"].lower()
        name = params["name"]
        existing = next((v for v in bp["variables"] if v["name"] == name), None)
        out = {"asset": bp["path"], "name": bp["name"], "op": op, "variable": name}
        if op == "add":
            if existing:
                raise PluginError("CONFLICT", f"Variable '{name}' already exists.")
            self._snapshot()
            var = {"name": name, "type": params.get("type", "float"), "default": params.get("default", "")}
            bp["variables"].append(var)
            existing = var
            out["type"] = var["type"]
        elif not existing:
            raise PluginError("NOT_FOUND", f"Variable '{name}' not found on {bp['name']}.")
        else:
            self._snapshot()
        if op in ("remove", "delete"):
            bp["variables"].remove(existing)
        elif op == "rename":
            existing["name"] = params["new_name"]
            out["variable"] = params["new_name"]
        else:
            changed = []
            for key in ("category", "replicated", "default", "type", "instance_editable"):
                if key in params:
                    existing[key] = params[key]
                    changed.append(key)
            out["changed"] = changed
        bp["dirty"] = True
        self._record("modified", bp["path"], "variable." + op)
        return out

    def cmd_blueprint_function(self, params):
        bp = self._bp(params["asset"])
        op = params["op"].lower()
        name = params["name"]
        out = {"asset": bp["path"], "name": bp["name"], "op": op, "function": name}
        if op == "create":
            if name in bp["graphs"]:
                raise PluginError("CONFLICT", f"Function '{name}' already exists.")
            self._snapshot()
            entry = make_node("FunctionEntry", name, title=name)
            entry["pins"].append(pin("then", "out"))
            graph = {"kind": "function", "nodes": {entry["guid"]: entry}, "inputs": [], "outputs": []}
            bp["graphs"][name] = graph
            out["graph"] = name
        elif name not in bp["graphs"]:
            raise PluginError("NOT_FOUND", f"Function '{name}' not found on {bp['name']}.")
        else:
            self._snapshot()
            graph = bp["graphs"][name]
        if op in ("delete", "remove"):
            del bp["graphs"][name]
        elif op == "rename":
            bp["graphs"][params["new_name"]] = bp["graphs"].pop(name)
            out["function"] = params["new_name"]
        else:
            changed = []
            for spec in params.get("inputs", []):
                graph["inputs"].append(spec)
                pname, _, ptype = spec.partition(":")
                entry = next(n for n in graph["nodes"].values() if n["kind"] == "FunctionEntry")
                entry["pins"].append(pin(pname, "out", ptype))
                changed.append("+in " + pname)
            for spec in params.get("outputs", []):
                graph["outputs"].append(spec)
                changed.append("+out " + spec.split(":")[0])
            if "pure" in params:
                graph["pure"] = bool(params["pure"])
                changed.append("pure")
            out["changed"] = changed
            out["signature"] = self._signature(graph)
            out["entry_node"] = next(n["guid"] for n in graph["nodes"].values() if n["kind"] == "FunctionEntry")
        bp["dirty"] = True
        self._record("modified", bp["path"], "function." + op)
        return out

    def cmd_blueprint_interface(self, params):
        bp = self._bp(params["asset"])
        self._snapshot()
        name = params["interface"].rsplit("/", 1)[-1]
        if params["op"] == "add":
            bp.setdefault("interfaces", []).append(name)
        else:
            bp["interfaces"] = [i for i in bp.get("interfaces", []) if i != name]
        self._record("modified", bp["path"])
        return {"asset": bp["path"], "name": bp["name"], "op": params["op"], "interface": name}

    def cmd_blueprint_component(self, params):
        bp = self._bp(params["asset"])
        self._snapshot()
        op = params["op"]
        comps = bp.setdefault("components", [])
        out = {"asset": bp["path"], "name": bp["name"], "op": op, "component": params["name"]}
        if op == "add":
            comps.append({"name": params["name"], "class": params.get("class", "SceneComponent"), "parent": params.get("parent", ""), "props": params.get("properties", {})})
        elif op == "remove":
            bp["components"] = [c for c in comps if c["name"] != params["name"]]
        else:
            comp = next((c for c in comps if c["name"] == params["name"]), None)
            if not comp:
                raise PluginError("NOT_FOUND", "Component not found")
            comp.setdefault("props", {}).update(params.get("properties", {}))
            out["changed"] = list(params.get("properties", {}).keys())
        self._record("modified", bp["path"])
        return out

    def cmd_blueprint_compile(self, params):
        results = []
        ok = 0
        for ref in params.get("assets", []):
            bp = self._bp(ref)
            self.compiled.append(bp["path"])
            errors = bp.get("error_nodes", [])
            item = {"asset": bp["path"], "ok": not errors, "errors": len(errors), "warnings": 0, "status": "Error" if errors else "UpToDate", "messages": [],
                    "node_errors": [{"graph": e["graph"], "node": e["node"], "kind": e.get("kind", ""), "title": "", "level": "error", "text": e["text"]} for e in errors]}
            if params.get("validate"):
                item["validation"] = {"ok": True, "errors": [], "warnings": []}
            if params.get("save") and item["ok"]:
                item["saved"] = True
                bp["dirty"] = False
                self.saved.append(bp["path"])
                self._record("saved", bp["path"])
            self._record("compiled", bp["path"])
            if item["ok"]:
                ok += 1
            results.append(item)
        return {"results": results, "succeeded": ok, "total": len(results)}

    def cmd_blueprint_save(self, params):
        results = []
        for ref in params.get("assets", []):
            bp = self._bp(ref)
            was = bool(bp.get("dirty"))
            bp["dirty"] = False
            self.saved.append(bp["path"])
            results.append({"asset": bp["path"], "ok": True, "was_dirty": was})
            self._record("saved", bp["path"])
        return {"results": results, "saved": len(results), "total": len(results)}

    def cmd_blueprint_validate(self, params):
        results = [{"asset": self._bp(r)["path"], "ok": True, "errors": [], "warnings": [], "status": "OK"} for r in params.get("assets", [])]
        return {"results": results, "passed": len(results), "total": len(results)}

    def cmd_blueprint_reload(self, params):
        bp = self._bp(params["asset"])
        bp["dirty"] = False
        self._record("modified", bp["path"], "reloaded")
        return {"asset": bp["path"], "ok": True}

    # graph ------------------------------------------------------------------
    def _node_json(self, node, pins=True, defaults=True):
        out = {k: v for k, v in node.items() if k != "pins"}
        if pins:
            out["pins"] = []
            for p in node["pins"]:
                q = {"id": p["id"], "name": p["name"], "dir": p["dir"], "type": p["type"]}
                if defaults and p["dir"] == "in" and not p.get("links") and p.get("default") not in (None, ""):
                    q["default"] = p["default"]
                if p.get("links"):
                    q["links"] = list(p["links"])
                out["pins"].append(q)
        return out

    def cmd_graph_list(self, params):
        bp = self._bp(params["asset"])
        return {"asset": bp["path"], "name": bp["name"], "graphs": [{"name": g, "kind": graph["kind"], "nodes": len(graph["nodes"])} for g, graph in bp["graphs"].items()]}

    def cmd_graph_inspect(self, params):
        bp = self._bp(params["asset"])
        gname, graph = self._graph(bp, params.get("graph"))
        nodes = list(graph["nodes"].values())
        out = {"asset": bp["path"], "graph": gname, "kind": graph["kind"], "total_nodes": len(nodes)}
        if graph["kind"] == "function":
            out["signature"] = self._signature(graph)
        if params.get("around"):
            center = graph["nodes"].get(params["around"].upper())
            if not center:
                out["error"] = f"Node {params['around']} not found."
                out["nodes"] = []
                return out
            keep = {center["guid"]}
            frontier = [center]
            for _ in range(int(params.get("depth", 1))):
                nxt = []
                for n in frontier:
                    for p in n["pins"]:
                        for l in p.get("links", []):
                            if l["node"] not in keep and l["node"] in graph["nodes"]:
                                keep.add(l["node"])
                                nxt.append(graph["nodes"][l["node"]])
                frontier = nxt
            nodes = [n for n in nodes if n["guid"] in keep]
            out["around"] = params["around"]
            out["depth"] = int(params.get("depth", 1))
        query = params.get("query", "").lower()
        kind = params.get("kind", "")
        if query:
            nodes = [n for n in nodes if query in (n["title"] + n.get("member", "") + n["kind"] + n.get("comment", "")).lower()]
        if kind:
            nodes = [n for n in nodes if n["kind"].lower() == kind.lower()]
        nodes.sort(key=lambda n: (n["y"], n["x"], n["guid"]))
        offset = int(params.get("offset", 0))
        limit = int(params.get("limit", 200))
        page = nodes[offset:offset + limit]
        out["nodes"] = [self._node_json(n, params.get("pins", True), params.get("defaults", True)) for n in page]
        out["matched"] = len(nodes)
        out["offset"] = offset
        out["truncated"] = offset + len(page) < len(nodes)
        return out

    def cmd_graph_find_nodes(self, params):
        bp = self._bp(params["asset"])
        query = params.get("query", "").lower()
        kind = params.get("kind", "")
        items = []
        matched = 0
        for gname, graph in bp["graphs"].items():
            if params.get("graph") and gname.lower() != params["graph"].lower():
                continue
            for n in graph["nodes"].values():
                if query and query not in (n["title"] + n.get("member", "") + n["kind"]).lower():
                    continue
                if kind and n["kind"].lower() != kind.lower():
                    continue
                matched += 1
                if len(items) < int(params.get("limit", 50)):
                    item = self._node_json(n, params.get("pins", False))
                    item["graph"] = gname
                    items.append(item)
        return {"asset": bp["path"], "name": bp["name"], "nodes": items, "matched": matched, "truncated": matched > len(items)}

    def _create_node(self, bp, graph, params):
        t = params["type"].lower()
        if t in ("function_call", "call", "function", "print", "delay"):
            member = {"print": "PrintString", "delay": "Delay"}.get(t, params.get("function", "Func"))
            mclass = "KismetSystemLibrary" if t in ("print", "delay") else ("self" if "." not in member else member.split(".")[0])
            member = member.split(".")[-1]
            node = make_node("CallFunction", member, member_class=mclass, title=member)
            node["pins"] += [pin("execute", "in"), pin("then", "out")]
            if member == "PrintString":
                node["pins"].append(pin("InString", "in", "string", params.get("text", "Hello")))
            elif member == "Delay":
                node["pins"].append(pin("Duration", "in", "float", str(params.get("duration", 1.0))))
            else:
                if "." not in params.get("function", ""):
                    node["pins"].append(pin("self", "in", bp["name"]))
                sig = bp["graphs"].get(member)
                for spec in (sig or {}).get("inputs", []):
                    n, _, ty = spec.partition(":")
                    node["pins"].append(pin(n, "in", ty))
                for spec in (sig or {}).get("outputs", []):
                    n, _, ty = spec.partition(":")
                    node["pins"].append(pin(n, "out", ty))
            if params.get("pure"):
                node["pure"] = True
            return node
        if t in ("variable_get", "get", "variable_set", "set"):
            var = next((v for v in bp["variables"] if v["name"] == params.get("variable")), None)
            if not var and not params.get("class"):
                raise PluginError("NOT_FOUND", f"Variable '{params.get('variable')}' not found on {bp['name']}.")
            vtype = var["type"] if var else "float"
            if t in ("variable_get", "get"):
                node = make_node("VariableGet", params["variable"], member_class="self", title="Get " + params["variable"], pure=True)
                node["pins"].append(pin(params["variable"], "out", vtype))
            else:
                node = make_node("VariableSet", params["variable"], member_class="self", title="Set " + params["variable"])
                node["pins"] += [pin("execute", "in"), pin("then", "out"), pin(params["variable"], "in", vtype, var.get("default", "") if var else ""), pin("Output_Get", "out", vtype)]
            return node
        if t == "custom_event":
            node = make_node("CustomEvent", params.get("name", "Event"), title=params.get("name", "Event"))
            node["pins"].append(pin("then", "out"))
            for spec in params.get("inputs", []):
                n, _, ty = spec.partition(":")
                node["pins"].append(pin(n, "out", ty))
            return node
        if t == "event":
            node = make_node("Event", params.get("event", "BeginPlay"), member_class=bp["parent"], title="Event " + params.get("event", ""))
            node["pins"].append(pin("then", "out"))
            return node
        if t in ("branch", "if"):
            node = make_node("Branch", "", title="Branch")
            node["pins"] += [pin("execute", "in"), pin("Condition", "in", "bool", "false"), pin("True", "out"), pin("False", "out")]
            return node
        if t == "sequence":
            node = make_node("Sequence", "", title="Sequence")
            node["pins"].append(pin("execute", "in"))
            for i in range(int(params.get("outputs", 2))):
                node["pins"].append(pin(f"then_{i}", "out"))
            return node
        if t == "cast":
            node = make_node("DynamicCast", params.get("class", "Actor"), title="Cast To " + params.get("class", ""))
            node["pins"] += [pin("execute", "in"), pin("Object", "in", "Object"), pin("then", "out"), pin("CastFailed", "out"), pin("As" + params.get("class", ""), "out", params.get("class", "Actor"))]
            return node
        if t == "comment":
            node = make_node("Comment", params.get("text", "Comment"), title=params.get("text", ""))
            return node
        if t == "macro":
            node = make_node("MacroInstance", params.get("macro", "ForEachLoop"), title=params.get("macro", ""))
            node["pins"] += [pin("execute", "in"), pin("LoopBody", "out"), pin("Completed", "out"), pin("Array", "in", "[wildcard]"), pin("Array Element", "out", "wildcard"), pin("Array Index", "out", "int")]
            return node
        raise PluginError("UNSUPPORTED", f"Unknown node type '{t}'.", {"capability": "Blueprint.EditGraph"})

    def cmd_graph_add_node(self, params):
        bp = self._bp(params["asset"])
        gname, graph = self._graph(bp, params.get("graph"))
        self._snapshot()
        node = self._create_node(bp, graph, params)
        node["x"], node["y"] = 0, 200 * len(graph["nodes"])
        graph["nodes"][node["guid"]] = node
        connections = []
        failed = []
        for key, value in (params.get("pins") or {}).items():
            try:
                self._pin(node, key, "in")["default"] = value
            except PluginError as exc:
                failed.append(exc.message)
        exec_in = next((p for p in node["pins"] if p["dir"] == "in" and p["type"] == "exec"), None)
        exec_out = next((p for p in node["pins"] if p["dir"] == "out" and p["type"] == "exec"), None)
        if params.get("after") and exec_in:
            ng, pn = params["after"].partition(".")[0], params["after"].partition(".")[2]
            _, _, after_node = self._node(bp, ng)
            after_pin = self._pin(after_node, pn, "out")
            old = list(after_pin.get("links", []))
            if old and exec_out:
                for link in old:
                    tgt = graph["nodes"][link["node"]]
                    tgt_pin = self._pin(tgt, link["pin"], "in")
                    tgt_pin["links"] = [l for l in tgt_pin.get("links", []) if l["node"] != after_node["guid"]]
                after_pin["links"] = []
            connections.append(self._link(after_node, after_pin, node, exec_in, True))
            if exec_out:
                for link in old:
                    tgt = graph["nodes"][link["node"]]
                    connections.append(self._link(node, exec_out, tgt, self._pin(tgt, link["pin"], "in"), True))
        for key, ref in (params.get("connect") or {}).items():
            try:
                mine = next(p for p in node["pins"] if p["name"].lower() == key.lower())
                ng, _, pn = ref.partition(".")
                _, _, other = self._node(bp, ng)
                other_pin = self._pin(other, pn, "in" if mine["dir"] == "out" else "out")
                if mine["dir"] == "out":
                    connections.append(self._link(node, mine, other, other_pin, bool(params.get("force"))))
                else:
                    connections.append(self._link(other, other_pin, node, mine, bool(params.get("force"))))
            except (StopIteration, PluginError) as exc:
                failed.append(getattr(exc, "message", str(exc)))
        bp["dirty"] = True
        self._record("modified", bp["path"], "graph.add_node")
        out = self._node_json(node)
        out["graph"] = gname
        out["connections"] = connections
        if failed:
            out["failed"] = failed
        return out

    def cmd_graph_delete_nodes(self, params):
        bp = self._bp(params["asset"])
        self._snapshot()
        deleted, reconnected, failed = [], [], []
        for ref in params.get("nodes", []):
            try:
                gname, graph, node = self._node(bp, ref)
            except PluginError as exc:
                failed.append(exc.message)
                continue
            exec_in = next((p for p in node["pins"] if p["dir"] == "in" and p["type"] == "exec"), None)
            exec_out = next((p for p in node["pins"] if p["dir"] == "out" and p["type"] == "exec"), None)
            if params.get("reconnect", True) and exec_in and exec_out and exec_in.get("links") and len(exec_out.get("links", [])) == 1:
                down = graph["nodes"][exec_out["links"][0]["node"]]
                down_pin = self._pin(down, exec_out["links"][0]["pin"], "in")
                ups = list(exec_in["links"])
                self._unlink_all(node)
                for up in ups:
                    up_node = graph["nodes"][up["node"]]
                    self._link(up_node, self._pin(up_node, up["pin"], "out"), down, down_pin, True)
                    reconnected.append(f"{up_node['guid']} -> {down['guid']}")
            else:
                self._unlink_all(node)
            del graph["nodes"][node["guid"]]
            deleted.append(node["guid"])
        bp["dirty"] = True
        self._record("modified", bp["path"], "graph.delete")
        out = {"asset": bp["path"], "name": bp["name"], "deleted": deleted, "reconnected": reconnected}
        if failed:
            out["failed"] = failed
        return out

    def cmd_graph_connect(self, params):
        bp = self._bp(params["asset"])
        self._snapshot()
        done, failed = [], []
        for link in params.get("links", []):
            try:
                fg, _, fp = link["from"].partition(".")
                tg, _, tp = link["to"].partition(".")
                _, _, fnode = self._node(bp, fg)
                _, _, tnode = self._node(bp, tg)
                done.append(self._link(fnode, self._pin(fnode, fp, "out"), tnode, self._pin(tnode, tp, "in"), bool(params.get("force"))))
            except PluginError as exc:
                failed.append(exc.message + (" " + exc.details.get("hint", "") if exc.details else ""))
        bp["dirty"] = True
        self._record("modified", bp["path"], "graph.connect")
        out = {"asset": bp["path"], "name": bp["name"], "connected": done}
        if failed:
            out["failed"] = failed
        if not done and failed:
            raise PluginError("CONFLICT", failed[0], out)
        return out

    def cmd_graph_disconnect(self, params):
        bp = self._bp(params["asset"])
        self._snapshot()
        if params.get("pin"):
            g, _, pn = params["pin"].partition(".")
            _, _, node = self._node(bp, g)
            p = self._pin(node, pn, "in") if any(x["name"].lower() == pn.lower() and x["dir"] == "in" for x in node["pins"]) else self._pin(node, pn, "out")
            count = len(p.get("links", []))
            for link in list(p.get("links", [])):
                other = self._find_node_any(link["node"])
                for op in other["pins"]:
                    op["links"] = [l for l in op.get("links", []) if not (l["node"] == node["guid"] and l["pin"] == p["name"])]
            p["links"] = []
            self._record("modified", bp["path"])
            return {"asset": bp["path"], "broken": count}
        raise PluginError("BAD_REQUEST", "Provide 'pin' (break all) or 'from'+'to'.")

    def cmd_graph_set_pins(self, params):
        bp = self._bp(params["asset"])
        _, graph, node = self._node(bp, params["node"])
        self._snapshot()
        failed = []
        for key, value in (params.get("pins") or {}).items():
            try:
                self._pin(node, key, "in")["default"] = str(value)
            except PluginError as exc:
                failed.append(exc.message)
        if "comment" in params:
            node["comment"] = params["comment"]
        bp["dirty"] = True
        self._record("modified", bp["path"])
        out = self._node_json(node)
        if failed:
            out["failed"] = failed
        return out

    def cmd_graph_replace_node(self, params):
        bp = self._bp(params["asset"])
        gname, graph, old = self._node(bp, params["node"])
        self._snapshot()
        new = self._create_node(bp, graph, params)
        new["x"], new["y"] = old["x"], old["y"]
        graph["nodes"][new["guid"]] = new
        migrated, dropped = [], []
        for op in old["pins"]:
            for link in list(op.get("links", [])):
                np_ = next((p for p in new["pins"] if p["name"] == op["name"] and p["dir"] == op["dir"]), None) or next((p for p in new["pins"] if p["dir"] == op["dir"] and p["type"] == op["type"]), None)
                other = graph["nodes"][link["node"]]
                other_pin = self._pin(other, link["pin"], "in" if op["dir"] == "out" else "out")
                other_pin["links"] = [l for l in other_pin.get("links", []) if l["node"] != old["guid"]]
                if np_ is None:
                    dropped.append(f"{op['name']} -> {link['node']}.{link['pin']}")
                    continue
                if op["dir"] == "out":
                    self._link(new, np_, other, other_pin, True)
                else:
                    self._link(other, other_pin, new, np_, True)
                migrated.append(f"{op['name']} -> {link['node']}.{link['pin']}")
        del graph["nodes"][old["guid"]]
        bp["dirty"] = True
        self._record("modified", bp["path"])
        out = self._node_json(new)
        out["replaced"] = params["node"]
        out["migrated"] = migrated
        if dropped:
            out["dropped"] = dropped
        return out

    def cmd_graph_clone_nodes(self, params):
        bp = self._bp(params["asset"])
        self._snapshot()
        created = []
        gname = None
        for ref in params.get("nodes", []):
            gname, graph, node = self._node(bp, ref)
            clone = copy.deepcopy(node)
            clone["guid"] = guid()
            for p in clone["pins"]:
                p["id"] = guid()
                p["links"] = []
            clone["y"] += int(params.get("dy", 300))
            graph["nodes"][clone["guid"]] = clone
            created.append(self._node_json(clone, False))
        self._record("modified", bp["path"])
        return {"asset": bp["path"], "name": bp["name"], "graph": gname, "nodes": created, "internal_links": 0}

    def cmd_graph_local_variable(self, params):
        bp = self._bp(params["asset"])
        gname, graph = self._graph(bp, params.get("graph"))
        self._snapshot()
        graph.setdefault("locals", []).append(params["name"])
        self._record("modified", bp["path"])
        return {"asset": bp["path"], "graph": gname, "variable": params["name"], "type": params["type"]}

    # widgets ------------------------------------------------------------------
    def _widgets(self, bp):
        if bp["kind"] != "Widget":
            raise PluginError("BAD_REQUEST", f"{bp['name']} is not a Widget Blueprint.")
        return bp.setdefault("widgets", [])

    def _widget(self, bp, name):
        for w in self._widgets(bp):
            if w["name"].lower() == name.lower():
                return w
        raise PluginError("NOT_FOUND", f"Widget '{name}' not found in {bp['name']}.")

    def _children(self, bp, name):
        return [w for w in self._widgets(bp) if w.get("parent") == name]

    def _style_key(self, w):
        return (w["class"], tuple(sorted((w.get("props") or {}).items())), tuple(sorted((w.get("slot") or {}).items())))

    def cmd_widget_tree(self, params):
        bp = self._bp(params["asset"])
        widgets = self._widgets(bp)
        root = params.get("root") or next((w["name"] for w in widgets if w.get("parent") is None), None)
        items = []
        styles = {}
        style_ids = {}

        def walk(name, depth, index):
            w = self._widget(bp, name)
            item = {"name": w["name"], "class": w["class"], "depth": depth, "index": index}
            if w.get("parent"):
                item["parent"] = w["parent"]
            if w.get("var"):
                item["var"] = True
            if "Text" in (w.get("props") or {}):
                item["text"] = w["props"]["Text"]
            if params.get("styles", True) and w["class"] not in ("CanvasPanel", "VerticalBox", "HorizontalBox", "Overlay"):
                key = self._style_key(w)
                sid = style_ids.get(key)
                if sid is None:
                    sid = f"S{len(style_ids) + 1}"
                    style_ids[key] = sid
                    props = {k: v for k, v in (w.get("props") or {}).items() if k != "Text"}
                    props.update({"Slot." + k: v for k, v in (w.get("slot") or {}).items()})
                    styles[sid] = {"class": w["class"], "props": props, "members": []}
                styles[sid]["members"].append(w["name"])
                item["style"] = sid
            bound = [f"{e['event']}={e['node']}" for e in bp.get("bindings", []) if e["widget"] == w["name"]]
            if bound:
                item["events"] = bound
            items.append(item)
            for i, child in enumerate(self._children(bp, w["name"])):
                walk(child["name"], depth + 1, i)

        if root:
            walk(root, 0, 0)
        out = {"asset": bp["path"], "name": bp["name"], "parent": bp["parent"], "root": root, "widgets": items, "total": len(items), "animations": [a["name"] for a in bp.get("animations", [])]}
        if params.get("styles", True):
            out["styles"] = styles
        return out

    def cmd_widget_inspect(self, params):
        bp = self._bp(params["asset"])
        w = self._widget(bp, params["widget"])
        out = {"asset": bp["path"], "name": bp["name"], "widget": w["name"], "class": w["class"], "class_spec": "/Script/UMG." + w["class"], "var": bool(w.get("var")),
               "props": dict(w.get("props") or {}), "children": [c["name"] for c in self._children(bp, w["name"])],
               "events": ["OnClicked", "OnPressed", "OnReleased", "OnHovered", "OnUnhovered"] if w["class"] == "Button" else [],
               "bound": [f"{e['event']}={e['node']}" for e in bp.get("bindings", []) if e["widget"] == w["name"]]}
        if w.get("parent"):
            out["parent"] = w["parent"]
            out["index"] = [c["name"] for c in self._children(bp, w["parent"])].index(w["name"])
        if w.get("slot") is not None:
            out["slot"] = {"class": "VerticalBoxSlot", "props": dict(w["slot"])}
        return out

    def _insert(self, bp, params, widget, default_sibling=None):
        widgets = self._widgets(bp)
        after, before, parent = params.get("insert_after"), params.get("insert_before"), params.get("parent")
        if after or before:
            sib = self._widget(bp, after or before)
            parent = sib["parent"]
            siblings = [w for w in widgets if w.get("parent") == parent]
            idx = [s["name"] for s in siblings].index(sib["name"]) + (1 if after else 0)
        elif parent:
            self._widget(bp, parent)
            idx = params.get("index", -1)
        elif default_sibling:
            parent = default_sibling["parent"]
            siblings = [w for w in widgets if w.get("parent") == parent]
            idx = [s["name"] for s in siblings].index(default_sibling["name"]) + 1
        else:
            raise PluginError("BAD_REQUEST", "Provide parent (+index), insert_after or insert_before.")
        widget["parent"] = parent
        siblings = [w for w in widgets if w.get("parent") == parent and w is not widget]
        if idx is None or idx < 0 or idx > len(siblings):
            idx = len(siblings)
        # rebuild ordering: remove widget then insert at idx among siblings
        others = [w for w in widgets if w is not widget]
        pos = 0
        seen = 0
        for i, w in enumerate(others):
            if w.get("parent") == parent:
                if seen == idx:
                    pos = i
                    break
                seen += 1
        else:
            pos = len(others)
        others.insert(pos, widget)
        bp["widgets"] = others
        return parent, idx

    def cmd_widget_clone(self, params):
        bp = self._bp(params["asset"])
        src = self._widget(bp, params["source"])
        if any(w["name"].lower() == params["new_name"].lower() for w in self._widgets(bp)):
            raise PluginError("CONFLICT", f"Widget '{params['new_name']}' already exists.")
        self._snapshot()
        old_suffix = params["source"].split("_", 1)[-1]
        new_suffix = params["new_name"].split("_", 1)[-1]
        clone = copy.deepcopy(src)
        clone["name"] = params["new_name"]
        parent, idx = self._insert(bp, params, clone, src)
        tree = [f"{clone['name']}:{clone['class']}"]
        changed, failed = [], []
        for key, value in (params.get("properties") or {}).items():
            clone.setdefault("props", {})[key] = value
            changed.append(key)

        def clone_children(src_name, dst_name, depth):
            for child in list(self._children(bp, src_name)):
                if child.get("_cloned"):
                    continue
                c = copy.deepcopy(child)
                base = child["name"].replace(old_suffix, new_suffix) if params.get("rename_children", True) and old_suffix != new_suffix else child["name"] + "_1"
                c["name"] = base
                c["parent"] = dst_name
                c["_cloned"] = True
                overrides = (params.get("children") or {}).get(child["name"])
                if overrides:
                    for k, v in overrides.items():
                        if k == "rename":
                            c["name"] = v
                        else:
                            c.setdefault("props", {})[k] = v
                            changed.append(f"{c['name']}.{k}")
                bp["widgets"].append(c)
                tree.append(" " * depth + f"{c['name']}:{c['class']}")
                clone_children(child["name"], c["name"], depth + 1)

        clone_children(src["name"], clone["name"], 1)
        for w in bp["widgets"]:
            w.pop("_cloned", None)
        bp["dirty"] = True
        self._record("modified", bp["path"], "widget.clone")
        out = {"asset": bp["path"], "name": bp["name"], "widget": clone["name"], "class": clone["class"], "parent": parent, "index": idx, "tree": tree, "changed": changed}
        if failed:
            out["failed"] = failed
        return out

    def cmd_widget_add(self, params):
        bp = self._bp(params["asset"])
        if any(w["name"].lower() == params["name"].lower() for w in self._widgets(bp)):
            raise PluginError("CONFLICT", f"Widget '{params['name']}' already exists.")
        self._snapshot()
        widget = {"name": params["name"], "class": params["class"], "parent": None, "props": dict(params.get("properties") or {}), "slot": dict(params.get("slot") or {}) or None,
                  "var": params.get("var", params["class"] not in ("VerticalBox", "HorizontalBox", "CanvasPanel", "Overlay"))}
        if not self._widgets(bp) or params.get("root"):
            bp["widgets"].append(widget)
            parent, idx = None, 0
        else:
            bp["widgets"].append(widget)
            parent, idx = self._insert(bp, params, widget)
        bp["dirty"] = True
        self._record("modified", bp["path"], "widget.add")
        out = {"asset": bp["path"], "name": bp["name"], "widget": widget["name"], "class": widget["class"], "changed": list((params.get("properties") or {}).keys())}
        if parent:
            out["parent"] = parent
            out["index"] = idx
        return out

    def cmd_widget_remove(self, params):
        bp = self._bp(params["asset"])
        self._snapshot()
        removed = []
        for name in params.get("widgets", []):
            w = self._widget(bp, name)
            stack = [w]
            while stack:
                cur = stack.pop()
                removed.append(cur["name"])
                stack.extend(self._children(bp, cur["name"]))
        bp["widgets"] = [w for w in bp["widgets"] if w["name"] not in removed]
        bp["dirty"] = True
        self._record("modified", bp["path"], "widget.remove")
        return {"asset": bp["path"], "name": bp["name"], "removed": removed}

    def cmd_widget_move(self, params):
        bp = self._bp(params["asset"])
        w = self._widget(bp, params["widget"])
        self._snapshot()
        parent, idx = self._insert(bp, params, w)
        self._record("modified", bp["path"], "widget.move")
        return {"asset": bp["path"], "name": bp["name"], "widget": w["name"], "parent": parent, "index": idx}

    def cmd_widget_set(self, params):
        bp = self._bp(params["asset"])
        w = self._widget(bp, params["widget"])
        self._snapshot()
        changed, failed = [], []
        for key, value in (params.get("properties") or {}).items():
            if key.startswith("Nope"):
                failed.append(f"{key}: Property not found")
                continue
            w.setdefault("props", {})[key] = value
            changed.append(key)
        for key, value in (params.get("slot") or {}).items():
            w.setdefault("slot", {})[key] = value
            changed.append("Slot." + key)
        if "var" in params:
            w["var"] = bool(params["var"])
            changed.append("var")
        if params.get("rename"):
            old = w["name"]
            w["name"] = params["rename"]
            for c in bp["widgets"]:
                if c.get("parent") == old:
                    c["parent"] = w["name"]
            for b in bp.get("bindings", []):
                if b["widget"] == old:
                    b["widget"] = w["name"]
            changed.append("rename")
        bp["dirty"] = True
        self._record("modified", bp["path"], "widget.set")
        out = {"asset": bp["path"], "name": bp["name"], "widget": w["name"], "changed": changed}
        if failed:
            out["failed"] = failed
            if not changed:
                raise PluginError("BAD_REQUEST", failed[0], out)
        return out

    def cmd_widget_copy_style(self, params):
        bp = self._bp(params["asset"])
        src = self._widget(bp, params["source"])
        self._snapshot()
        applied, skipped = [], []
        for name in params.get("targets", []):
            t = self._widget(bp, name)
            if t["class"] != src["class"]:
                skipped.append(f"{name}: class mismatch")
                continue
            t["props"] = {k: v for k, v in (src.get("props") or {}).items() if k != "Text"} | {k: v for k, v in (t.get("props") or {}).items() if k == "Text"}
            if params.get("slot", True):
                t["slot"] = copy.deepcopy(src.get("slot"))
            applied.append(name)
        self._record("modified", bp["path"])
        out = {"asset": bp["path"], "applied": applied}
        if skipped:
            out["skipped"] = skipped
        return out

    def cmd_widget_bind_event(self, params):
        bp = self._bp(params["asset"])
        w = self._widget(bp, params["widget"])
        if w["class"] != "Button" and params["event"] == "OnClicked":
            raise PluginError("NOT_FOUND", f"Event '{params['event']}' not found on {w['class']}. Available: OnVisibilityChanged")
        self._snapshot()
        w["var"] = True
        existing = next((b for b in bp.get("bindings", []) if b["widget"] == w["name"] and b["event"] == params["event"]), None)
        graph = bp["graphs"]["EventGraph"]
        created = existing is None
        if created:
            node = make_node("ComponentBoundEvent", f"{w['name']}.{params['event']}", title=f"{params['event']} ({w['name']})")
            node["pins"].append(pin("then", "out"))
            graph["nodes"][node["guid"]] = node
            bp.setdefault("bindings", []).append({"widget": w["name"], "event": params["event"], "node": node["guid"]})
        else:
            node = graph["nodes"][existing["node"]]
        out = {"asset": bp["path"], "name": bp["name"], "widget": w["name"], "event": params["event"], "graph": "EventGraph", "node": node["guid"], "created": created}
        func = params.get("function")
        if func:
            if func not in bp["graphs"]:
                if params.get("create_function"):
                    entry = make_node("FunctionEntry", func, title=func)
                    bp["graphs"][func] = {"kind": "function", "nodes": {entry["guid"]: entry}, "inputs": [], "outputs": []}
                    out["function_created"] = func
                else:
                    out["function_error"] = f"Function '{func}' not found (self, parents, libraries). Use Class.Function."
            if func in bp["graphs"]:
                call = make_node("CallFunction", func, member_class="self", title=func)
                call["pins"] += [pin("execute", "in"), pin("then", "out"), pin("self", "in", bp["name"])]
                graph["nodes"][call["guid"]] = call
                then = self._pin(node, "then", "out")
                if then.get("links") and not params.get("force"):
                    out["function_error"] = "Event already has an exec target; pass force=true to insert before it."
                    del graph["nodes"][call["guid"]]
                else:
                    self._link(node, then, call, self._pin(call, "execute", "in"), True)
                    out["call_node"] = call["guid"]
                    out["function"] = func
        bp["dirty"] = True
        self._record("modified", bp["path"], "widget.bind_event")
        return out

    def cmd_widget_animations(self, params):
        bp = self._bp(params["asset"])
        anims = [{"name": a["name"], "seconds": a.get("seconds", 1.0), "bindings": a.get("bindings", [])} for a in bp.get("animations", []) if not params.get("animation") or a["name"] == params["animation"]]
        return {"asset": bp["path"], "animations": anims, "note": "Keyframe editing is not supported (capability UMG.EditAnimation)."}

    def cmd_widget_preview(self, params):
        bp = self._bp(params["asset"])
        widgets = self._widgets(bp)
        layout = []
        y = {None: 0}

        def walk(name, depth):
            w = self._widget(bp, name)
            kids = self._children(bp, w["name"])
            if w["class"] in ("CanvasPanel",):
                rect = (0, 0, params.get("width", 1920), params.get("height", 1080))
            elif w["class"] in ("VerticalBox",):
                rect = (760, 300, 400, 100 * len(kids))
            else:
                parent = self._widget(bp, w["parent"]) if w.get("parent") else None
                if parent and parent["class"] == "VerticalBox":
                    idx = [c["name"] for c in kids_of(parent["name"])].index(w["name"])
                    rect = (760, 300 + idx * 100, 400, 80)
                elif parent:
                    prect = rects[parent["name"]]
                    rect = (prect[0] + 10, prect[1] + 10, prect[2] - 20, prect[3] - 20)
                else:
                    rect = (0, 0, 100, 100)
            rects[w["name"]] = rect
            layout.append({"widget": w["name"], "class": w["class"], "depth": depth, "x": rect[0], "y": rect[1], "w": rect[2], "h": rect[3]})
            for k in kids:
                walk(k["name"], depth + 1)

        rects = {}
        kids_of = lambda n: self._children(bp, n)  # noqa: E731
        root = next((w["name"] for w in widgets if w.get("parent") is None), None)
        if root:
            walk(root, 0)
        out = {"asset": bp["path"], "name": bp["name"], "width": params.get("width", 1920), "height": params.get("height", 1080), "layout": layout}
        if params.get("image", True):
            out["image"] = params.get("path") or f"/fake/Saved/ClaudeAgent/Previews/{bp['name']}.png"
            out["bytes"] = 12345
        return out

    def cmd_widget_preview_diff(self, params):
        return {"width": 1920, "height": 1080, "changed_pixels": 4600, "diff_percent": 2.3, "bbox": {"x": 760, "y": 500, "w": 400, "h": 80}}

    # batch --------------------------------------------------------------------
    def cmd_batch(self, params):
        snapshot = copy.deepcopy(self.project)
        results = []
        ok = failed = 0
        aborted = False
        for i, op in enumerate(params.get("ops", [])):
            try:
                r = self.call(op["cmd"], op.get("params"))
                results.append({"index": i, "cmd": op["cmd"], "ok": True, "result": r})
                ok += 1
            except PluginError as exc:
                results.append({"index": i, "cmd": op["cmd"], "ok": False, "error": {"code": exc.code, "message": exc.message}})
                failed += 1
                if params.get("atomic", True) or params.get("stop_on_error", True):
                    aborted = True
                    break
        rolled = False
        if aborted and params.get("atomic", True):
            self.project = snapshot
            rolled = True
            for path in self.project:
                self._record("modified", path)
        return {"results": results, "succeeded": ok, "failed": failed, "total": len(params.get("ops", [])), "aborted": aborted, "rolled_back": rolled}


# ---------------------------------------------------------------- fixtures

def make_node(kind, member, title="", member_class="", pure=False):
    node = {"guid": guid(), "kind": kind, "class": "K2Node_" + kind, "title": title or member, "member": member, "x": 0, "y": 0, "pins": []}
    if member_class:
        node["member_class"] = member_class
    if pure:
        node["pure"] = True
    return node


def make_blueprint(path, parent="Actor", kind="Actor"):
    name = path.rsplit("/", 1)[-1]
    begin = make_node("Event", "BeginPlay", member_class=parent, title="Event BeginPlay")
    begin["pins"].append(pin("then", "out"))
    return {"path": path, "name": name, "kind": kind, "parent": parent, "variables": [], "graphs": {"EventGraph": {"kind": "event", "nodes": {begin["guid"]: begin}}},
            "interfaces": [], "components": [], "dependencies": [], "mtime": 1700000000}


def link(a, a_pin, b, b_pin):
    pa = next(p for p in a["pins"] if p["name"] == a_pin)
    pb = next(p for p in b["pins"] if p["name"] == b_pin)
    pa.setdefault("links", []).append({"node": b["guid"], "pin": b_pin})
    pb.setdefault("links", []).append({"node": a["guid"], "pin": a_pin})


def make_player_blueprint(path="/Game/Characters/Player/BP_Player"):
    bp = make_blueprint(path, "Character")
    bp["variables"] = [
        {"name": "Health", "type": "float", "default": "100", "category": "Stats", "replicated": True},
        {"name": "MaxHealth", "type": "float", "default": "100", "category": "Stats"},
        {"name": "HealthRegenRate", "type": "float", "default": "5", "category": "Stats"},
        {"name": "Inventory", "type": "BP_InventoryComponent", "category": "Components"},
    ]
    bp["components"] = [{"name": "Inventory", "class": "BP_InventoryComponent", "parent": ""}, {"name": "Mesh", "class": "SkeletalMeshComponent", "native": True}]
    # Function RegenHealth: Entry -> Get Health -> Add -> Set Health
    entry = make_node("FunctionEntry", "RegenHealth", title="RegenHealth")
    entry["pins"] += [pin("then", "out"), pin("DeltaTime", "out", "float")]
    get_h = make_node("VariableGet", "Health", member_class="self", title="Get Health", pure=True)
    get_h["pins"].append(pin("Health", "out", "float"))
    get_r = make_node("VariableGet", "HealthRegenRate", member_class="self", title="Get HealthRegenRate", pure=True)
    get_r["pins"].append(pin("HealthRegenRate", "out", "float"))
    mul = make_node("CallFunction", "Multiply_DoubleDouble", member_class="KismetMathLibrary", title="Multiply", pure=True)
    mul["pins"] += [pin("A", "in", "float"), pin("B", "in", "float"), pin("ReturnValue", "out", "float")]
    add = make_node("CallFunction", "Add_DoubleDouble", member_class="KismetMathLibrary", title="Add", pure=True)
    add["pins"] += [pin("A", "in", "float"), pin("B", "in", "float"), pin("ReturnValue", "out", "float")]
    get_max = make_node("VariableGet", "MaxHealth", member_class="self", title="Get MaxHealth", pure=True)
    get_max["pins"].append(pin("MaxHealth", "out", "float"))
    clamp = make_node("CallFunction", "FClamp", member_class="KismetMathLibrary", title="Clamp (float)", pure=True)
    clamp["pins"] += [pin("Value", "in", "float"), pin("Min", "in", "float", "0.0"), pin("Max", "in", "float"), pin("ReturnValue", "out", "float")]
    set_h = make_node("VariableSet", "Health", member_class="self", title="Set Health")
    set_h["pins"] += [pin("execute", "in"), pin("then", "out"), pin("Health", "in", "float"), pin("Output_Get", "out", "float")]
    for i, n in enumerate([entry, get_h, get_r, mul, add, get_max, clamp, set_h]):
        n["x"], n["y"] = i * 300, (i % 3) * 120
    link(entry, "then", set_h, "execute")
    link(get_r, "HealthRegenRate", mul, "A")
    link(entry, "DeltaTime", mul, "B")
    link(get_h, "Health", add, "A")
    link(mul, "ReturnValue", add, "B")
    link(add, "ReturnValue", clamp, "Value")
    link(get_max, "MaxHealth", clamp, "Max")
    link(clamp, "ReturnValue", set_h, "Health")
    bp["graphs"]["RegenHealth"] = {"kind": "function", "nodes": {n["guid"]: n for n in [entry, get_h, get_r, mul, add, get_max, clamp, set_h]}, "inputs": ["DeltaTime:float"], "outputs": []}
    # EventGraph: Tick -> RegenHealth
    tick = make_node("Event", "ReceiveTick", member_class="Actor", title="Event Tick")
    tick["pins"] += [pin("then", "out"), pin("DeltaSeconds", "out", "float")]
    call = make_node("CallFunction", "RegenHealth", member_class="self", title="Regen Health")
    call["pins"] += [pin("execute", "in"), pin("then", "out"), pin("DeltaTime", "in", "float")]
    link(tick, "then", call, "execute")
    link(tick, "DeltaSeconds", call, "DeltaTime")
    tick["y"], call["y"], call["x"] = 400, 400, 300
    bp["graphs"]["EventGraph"]["nodes"].update({tick["guid"]: tick, call["guid"]: call})
    bp["dependencies"] = ["/Game/Inventory/BP_InventoryComponent"]
    return bp


def make_main_menu(path="/Game/UI/WBP_MainMenu"):
    bp = make_blueprint(path, "UserWidget", kind="Widget")
    btn_style = {"WidgetStyle": "(Normal=(ImageSize=(X=420,Y=80),TintColor=(SpecifiedColor=(R=0.1,G=0.1,B=0.1,A=1))),Hovered=(TintColor=(SpecifiedColor=(R=0.3,G=0.3,B=0.3,A=1))))", "IsFocusable": "True"}
    txt_style = {"Font": "(FontObject=/Game/UI/Fonts/F_MainMenu,Size=32)", "ColorAndOpacity": "(SpecifiedColor=(R=1,G=1,B=1,A=1))", "Justification": "Center"}
    slot = {"Padding": "(Left=0,Top=12,Right=0,Bottom=12)", "HorizontalAlignment": "HAlign_Fill"}
    widgets = [
        {"name": "CanvasPanel", "class": "CanvasPanel", "parent": None, "props": {}, "slot": None},
        {"name": "IMG_Background", "class": "Image", "parent": "CanvasPanel", "props": {"Brush": "(ResourceObject=/Game/UI/T_MenuBG)"}, "slot": {"LayoutData": "(Anchors=(Maximum=(X=1,Y=1)))"}},
        {"name": "VB_Menu", "class": "VerticalBox", "parent": "CanvasPanel", "props": {}, "slot": {"LayoutData": "(Offsets=(Left=760,Top=300,Right=400,Bottom=400))"}},
        {"name": "TXT_Title", "class": "TextBlock", "parent": "VB_Menu", "props": {"Text": "MY GAME", "Font": "(FontObject=/Game/UI/Fonts/F_Title,Size=64)"}, "slot": dict(slot)},
    ]
    for label in ("Play", "Settings", "Exit"):
        widgets.append({"name": f"BTN_{label}", "class": "Button", "parent": "VB_Menu", "props": dict(btn_style), "slot": dict(slot), "var": True})
        widgets.append({"name": f"TXT_{label}", "class": "TextBlock", "parent": f"BTN_{label}", "props": {"Text": label.upper(), **txt_style}, "slot": {"Padding": "(Left=0,Top=0,Right=0,Bottom=0)"}})
    bp["widgets"] = widgets
    bp["animations"] = [{"name": "FadeIn", "seconds": 0.5, "bindings": [{"widget": "CanvasPanel", "tracks": ["Render Opacity (1 sections)"]}]}]
    bp["variables"] = [{"name": "bIsOpen", "type": "bool", "default": "false"}]
    for label in ("Play", "Settings", "Exit"):
        node = make_node("ComponentBoundEvent", f"BTN_{label}.OnClicked", title=f"OnClicked (BTN_{label})")
        node["pins"].append(pin("then", "out"))
        bp["graphs"]["EventGraph"]["nodes"][node["guid"]] = node
        bp.setdefault("bindings", []).append({"widget": f"BTN_{label}", "event": "OnClicked", "node": node["guid"]})
        fn = f"Open{label}"
        entry = make_node("FunctionEntry", fn, title=fn)
        bp["graphs"][fn] = {"kind": "function", "nodes": {entry["guid"]: entry}, "inputs": [], "outputs": []}
        call = make_node("CallFunction", fn, member_class="self", title=fn)
        call["pins"] += [pin("execute", "in"), pin("then", "out")]
        bp["graphs"]["EventGraph"]["nodes"][call["guid"]] = call
        link(node, "then", call, "execute")
    bp["dependencies"] = ["/Game/UI/Fonts/F_MainMenu", "/Game/UI/T_MenuBG", "/Game/UI/WBP_Settings"]
    return bp


def make_inventory_component(path="/Game/Inventory/BP_InventoryComponent"):
    bp = make_blueprint(path, "ActorComponent", kind="Component")
    bp["variables"] = [{"name": "Items", "type": "[DA_Item]", "category": "Inventory"}, {"name": "MaxSlots", "type": "int", "default": "20"}]
    bp["interfaces"] = ["BPI_Inventory"]
    for fn, inputs, outputs in (("AddItem", ["Item:DA_Item"], ["Success:bool"]), ("RemoveItem", ["Item:DA_Item"], []), ("HasItem", ["Item:DA_Item"], ["Result:bool"])):
        entry = make_node("FunctionEntry", fn, title=fn)
        entry["pins"].append(pin("then", "out"))
        for spec in inputs:
            n, _, t = spec.partition(":")
            entry["pins"].append(pin(n, "out", t))
        bp["graphs"][fn] = {"kind": "function", "nodes": {entry["guid"]: entry}, "inputs": inputs, "outputs": outputs}
    return bp


def make_menu_project():
    project = {}
    for bp in (make_player_blueprint(), make_main_menu(), make_inventory_component()):
        project[bp["path"]] = bp
    wbp_inv = make_blueprint("/Game/UI/WBP_Inventory", "UserWidget", kind="Widget")
    wbp_inv["widgets"] = [{"name": "CanvasPanel", "class": "CanvasPanel", "parent": None, "props": {}, "slot": None}]
    wbp_inv["dependencies"] = ["/Game/Inventory/BP_InventoryComponent", "/Game/UI/WBP_InventorySlot"]
    wbp_slot = make_blueprint("/Game/UI/WBP_InventorySlot", "UserWidget", kind="Widget")
    wbp_slot["widgets"] = [{"name": "Overlay", "class": "Overlay", "parent": None, "props": {}, "slot": None}]
    project[wbp_inv["path"]] = wbp_inv
    project[wbp_slot["path"]] = wbp_slot
    return project


def make_large_project(count=500):
    """500 blueprints with realistic naming; BP_Player/BP_InventoryComponent/WBP_MainMenu included."""
    project = make_menu_project()
    kinds = ["Actor", "Pawn", "ActorComponent", "UserWidget", "GameModeBase", "Object"]
    words = ["Door", "Chest", "Enemy", "Turret", "Pickup", "Spawner", "Trigger", "Elevator", "Light", "Camera", "Quest", "Dialogue", "Weapon", "Ammo", "Health"]
    for i in range(count - len(project)):
        kind = kinds[i % len(kinds)]
        word = words[i % len(words)]
        prefix = "WBP_" if kind == "UserWidget" else "BP_"
        folder = "/Game/UI" if kind == "UserWidget" else f"/Game/Gameplay/{word}"
        path = f"{folder}/{prefix}{word}{i:03d}"
        bp = make_blueprint(path, kind, kind="Widget" if kind == "UserWidget" else "Actor")
        bp["variables"] = [{"name": f"{word}Value", "type": "float", "default": "1"}, {"name": "bActive", "type": "bool", "default": "true"}]
        entry = make_node("FunctionEntry", f"Update{word}", title=f"Update{word}")
        bp["graphs"][f"Update{word}"] = {"kind": "function", "nodes": {entry["guid"]: entry}, "inputs": [], "outputs": []}
        if i % 7 == 0:
            call = make_node("CallFunction", "AddItem", member_class="BP_InventoryComponent", title="Add Item")
            call["pins"] += [pin("execute", "in"), pin("then", "out")]
            bp["graphs"]["EventGraph"]["nodes"][call["guid"]] = call
            bp["dependencies"].append("/Game/Inventory/BP_InventoryComponent")
        # pad the event graph to make graphs realistically large
        for j in range(i % 40):
            n = make_node("CallFunction", "PrintString", member_class="KismetSystemLibrary", title="Print String")
            n["pins"] += [pin("execute", "in"), pin("then", "out"), pin("InString", "in", "string", f"msg{j}")]
            n["y"] = 100 * j
            bp["graphs"]["EventGraph"]["nodes"][n["guid"]] = n
        if kind == "UserWidget":
            bp["widgets"] = [{"name": "CanvasPanel", "class": "CanvasPanel", "parent": None, "props": {}, "slot": None}]
        project[path] = bp
    return project
