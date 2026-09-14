"""Session memory: short IDs, node maps, working sets, changesets, cursors and
an asset cache invalidated through the plugin's change sequence."""
import itertools
import re
import time
from typing import Any, Dict, List, Optional, Tuple

ASSET_ID = re.compile(r"^A\d+$")
NODE_ID = re.compile(r"^N\d+$")
WS_ID = re.compile(r"^WS\d+$")


class NodeMap:
    """Per-asset map between short node ids (N12) and editor GUIDs, plus pin aliases (P3)."""

    def __init__(self):
        self.counter = itertools.count(1)
        self.by_guid: Dict[str, str] = {}
        self.by_id: Dict[str, str] = {}
        self.graph_of: Dict[str, str] = {}
        self.pins: Dict[str, Dict[str, str]] = {}   # node id -> {P1: pin name}
        self.kinds: Dict[str, str] = {}

    def id_for(self, guid: str, graph: str = "", kind: str = "") -> str:
        guid = guid.upper()
        nid = self.by_guid.get(guid)
        if nid is None:
            nid = f"N{next(self.counter)}"
            self.by_guid[guid] = nid
            self.by_id[nid] = guid
        if graph:
            self.graph_of[nid] = graph
        if kind:
            self.kinds[nid] = kind
        return nid

    def guid_for(self, nid: str) -> Optional[str]:
        return self.by_id.get(nid.upper())

    def set_pins(self, nid: str, names: List[str]):
        self.pins[nid] = {f"P{index}": name for index, name in enumerate(names, start=1)}

    def pin_name(self, nid: str, alias: str) -> str:
        table = self.pins.get(nid, {})
        return table.get(alias.upper(), alias)


class Session:
    def __init__(self):
        self.asset_ids: Dict[str, str] = {}     # path -> A#
        self.asset_paths: Dict[str, str] = {}   # A# -> path
        self.asset_counter = itertools.count(1)
        self.nodes: Dict[str, NodeMap] = {}     # path -> NodeMap
        self.working_sets: Dict[str, List[str]] = {}
        self.ws_counter = itertools.count(1)
        self.current_ws: Optional[str] = None
        self.changesets: List[Dict[str, Any]] = []
        self.cs_counter = itertools.count(1)
        self.batch_counter = itertools.count(1)
        self.cursors: Dict[str, Tuple[str, list, int, int]] = {}
        self.cursor_counter = itertools.count(1)
        self.cache: Dict[str, Dict[str, Any]] = {}   # path -> {key: (seq, value)}
        self.seq = 0
        self.started = time.time()

    # ---- assets ---------------------------------------------------------
    def asset_id(self, path: str) -> str:
        aid = self.asset_ids.get(path)
        if aid is None:
            aid = f"A{next(self.asset_counter)}"
            self.asset_ids[path] = aid
            self.asset_paths[aid] = path
        return aid

    def path_for(self, aid: str) -> Optional[str]:
        return self.asset_paths.get(aid.upper())

    def is_asset_id(self, ref: str) -> bool:
        return bool(ASSET_ID.match(ref.strip().upper()))

    # ---- nodes ----------------------------------------------------------
    def nodes_for(self, path: str) -> NodeMap:
        node_map = self.nodes.get(path)
        if node_map is None:
            node_map = NodeMap()
            self.nodes[path] = node_map
        return node_map

    def node_ref_to_guid(self, path: str, ref: str) -> str:
        """'N12' -> guid; anything else (a guid) passes through."""
        ref = ref.strip()
        if NODE_ID.match(ref.upper()):
            guid = self.nodes_for(path).guid_for(ref)
            if guid is None:
                raise KeyError(f"Unknown node id {ref} for {path} (inspect the graph first).")
            return guid
        return ref

    def pin_ref_to_plugin(self, path: str, ref: str) -> str:
        """'N12.P2' / 'N12.Condition' / 'N12' -> 'GUID.PinName' / 'GUID'."""
        ref = ref.strip()
        node_part, sep, pin_part = ref.partition(".")
        if not sep:
            node_part, sep, pin_part = ref.partition(":")
        guid = self.node_ref_to_guid(path, node_part)
        if not sep:
            return guid
        nid = node_part.strip().upper()
        pin = self.nodes_for(path).pin_name(nid, pin_part) if NODE_ID.match(nid) else pin_part
        return f"{guid}.{pin}"

    # ---- working sets ---------------------------------------------------
    def create_working_set(self, paths: List[str]) -> str:
        ws = f"WS{next(self.ws_counter)}"
        self.working_sets[ws] = list(dict.fromkeys(paths))
        self.current_ws = ws
        return ws

    def add_to_working_set(self, paths: List[str], ws: Optional[str] = None) -> str:
        ws = ws or self.current_ws
        if ws is None or ws not in self.working_sets:
            return self.create_working_set(paths)
        for path in paths:
            if path not in self.working_sets[ws]:
                self.working_sets[ws].append(path)
        self.current_ws = ws
        return ws

    # ---- changesets -----------------------------------------------------
    def record_change(self, tool: str, assets: List[str], ops: List[str], extra: Optional[dict] = None) -> str:
        cs = f"CS{next(self.cs_counter)}"
        entry = {"id": cs, "tool": tool, "assets": list(dict.fromkeys(assets)), "ops": ops, "time": time.time()}
        if extra:
            entry.update(extra)
        self.changesets.append(entry)
        for path in assets:
            self.invalidate(path)
        return cs

    # ---- cursors --------------------------------------------------------
    def make_cursor(self, kind: str, items: list, offset: int, page: int) -> str:
        token = f"C{next(self.cursor_counter)}"
        self.cursors[token] = (kind, items, offset, page)
        return token

    def take_cursor(self, token: str):
        return self.cursors.get(token.strip().upper())

    # ---- cache ----------------------------------------------------------
    def cache_get(self, path: str, key: str):
        entry = self.cache.get(path, {}).get(key)
        return entry[1] if entry else None

    def cache_put(self, path: str, key: str, value):
        self.cache.setdefault(path, {})[key] = (self.seq, value)

    def invalidate(self, path: str):
        self.cache.pop(path, None)

    def apply_changes(self, changes: List[dict], seq: int):
        for change in changes:
            asset = change.get("asset")
            if asset:
                self.invalidate(asset)
                if change.get("type") in ("removed",):
                    self.nodes.pop(asset, None)
        self.seq = max(self.seq, int(seq or 0))

    def summary(self) -> Dict[str, Any]:
        return {
            "assets": len(self.asset_ids),
            "working_sets": {ws: [self.asset_id(p) for p in paths] for ws, paths in self.working_sets.items()},
            "current_ws": self.current_ws,
            "changesets": len(self.changesets),
            "cached": len(self.cache),
        }
