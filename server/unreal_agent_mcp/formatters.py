"""Compact, deterministic text renderers. Every function here exists to turn
plugin JSON into the smallest useful text for Claude.

Conventions (see skill/references/graph-format.md):
  N12 Branch              node line: id, kind/member, flags
    then -> N14           exec links are printed at the SOURCE node
    Condition <- N9.Ret   data links are printed at the CONSUMER node
    Duration = 0.5        unlinked input with a non-default value
"""
from typing import Any, Dict, Iterable, List, Optional

EXEC_IN = "execute"
EXEC_OUT = "then"


def short(path: str) -> str:
    return path.rsplit("/", 1)[-1] if path else ""


def clip(text: str, max_len: int = 80) -> str:
    text = str(text).replace("\n", " ")
    return text if len(text) <= max_len else text[: max_len - 1] + "…"


def kv_line(props: Dict[str, Any], max_items: int = 10, max_len: int = 90) -> str:
    items = list(props.items())
    parts = [f"{key}={clip(value, max_len)}" for key, value in items[:max_items]]
    if len(items) > max_items:
        parts.append(f"(+{len(items) - max_items} more)")
    return " ".join(parts)


# ---------------------------------------------------------------- errors

def format_plugin_error(exc) -> str:
    lines = ["FAILED", f"Operation: {exc.cmd}" if exc.cmd else "Operation: (plugin)", f"Reason: {exc.message}"]
    for key, value in (exc.details or {}).items():
        lines.append(f"{key}: {clip(value, 200)}")
    if exc.code == "UNSUPPORTED":
        cap = (exc.details or {}).get("capability", "")
        lines.append(f"Unsupported operation. Missing capability registered: {cap or 'unknown'}")
    return "\n".join(lines)


def format_unavailable(exc, ctx) -> str:
    return "\n".join([
        "EDITOR UNAVAILABLE",
        clip(str(exc), 300),
        f"Project: {ctx.config.project_file or ctx.config.project_dir}",
        "Fix: open the project in Unreal Editor with the ClaudeBlueprintAgent plugin enabled, then retry.",
        "Offline tools still work: search (cached index), session_info, get_capabilities (cached).",
    ])


# ---------------------------------------------------------------- discovery

def format_search(query: str, results: List[dict], ctx, coverage: Dict[str, int], cursor: Optional[str], total: int) -> str:
    if not results:
        return f'Search "{query}": 0 results (index {coverage.get("total", 0)} blueprints, {coverage.get("deep", 0)} deep-indexed)'
    lines = [f'Search "{query}": {total} results']
    for item in results:
        aid = ctx.aid(item["path"])
        if item["type"] == "asset":
            parent = f" < {item['parent']}" if item.get("parent") else ""
            cls = item.get("class", "")
            cls = {"Blueprint": "BP", "WidgetBlueprint": "WBP"}.get(cls, cls)
            lines.append(f"{aid} {item['name']} [{cls}{parent}]")
        else:
            detail = item.get("detail", item["name"])
            lines.append(f"{aid} {item.get('asset_name')}: {item['type']} {detail}")
    if cursor:
        lines.append(f"next_cursor: {cursor}")
    if coverage.get("deep", 0) < coverage.get("total", 0):
        lines.append(f"(index: {coverage['total']} blueprints, {coverage['deep']} deep-indexed; run index_project(deep=true) for full function/variable coverage)")
    return "\n".join(lines)


def format_xref(kind: str, name: str, hits: List[dict], ctx, coverage: Dict[str, int], limit: int = 30) -> str:
    if not hits:
        cov = f" ({coverage.get('deep', 0)}/{coverage.get('total', 0)} deep-indexed)"
        return f"{kind} {name}: none found{cov}"
    lines = [f"{kind} {name}: {len(hits)}"]
    for hit in hits[:limit]:
        detail = hit.get("call") or hit.get("ref") or hit.get("detail") or ""
        lines.append(f"{ctx.aid(hit['path'])} {hit['name']}  {detail}".rstrip())
    if len(hits) > limit:
        lines.append(f"(+{len(hits) - limit} more; pass limit= to see them)")
    return "\n".join(lines)


# ---------------------------------------------------------------- blueprint

def format_summary(data: dict, ctx) -> str:
    path = data.get("asset", "")
    lines = [f"{ctx.aid(path)} {data.get('name')} ({data.get('kind')}) parent {data.get('parent')}"]
    counts = []
    for key in ("functions", "variables", "events", "event_graphs", "macros", "components", "widgets", "animations", "nodes"):
        if key in data and data[key] not in (0, None):
            counts.append(f"{key.replace('event_graphs', 'graphs')} {data[key]}")
    lines.append("  ".join(counts))
    if data.get("interfaces"):
        lines.append("interfaces: " + ", ".join(data["interfaces"]))
    if data.get("referencers"):
        lines.append("referencers: " + ", ".join(short(p) for p in data["referencers"][:8]) + (f" (+{len(data['referencers']) - 8})" if len(data["referencers"]) > 8 else ""))
    if data.get("dependencies"):
        deps = [short(p) for p in data["dependencies"] if not p.startswith("/Engine")]
        if deps:
            lines.append("uses: " + ", ".join(deps[:8]) + (f" (+{len(deps) - 8})" if len(deps) > 8 else ""))
    status = data.get("status", "")
    flags = [status] if status and status != "OK" else []
    if data.get("dirty"):
        flags.append("unsaved")
    if flags:
        lines.append("status: " + ", ".join(flags))
    return "\n".join(lines)


def _signature(item: dict) -> str:
    inputs = ", ".join(item.get("inputs", []))
    outputs = item.get("outputs", [])
    out = ""
    if outputs:
        out = " -> " + (outputs[0].split(":", 1)[1] if len(outputs) == 1 and ":" in outputs[0] else ", ".join(outputs))
    flags = " [pure]" if item.get("pure") else ""
    return f"{item.get('name')}({inputs}){out}{flags}"


def format_structure(data: dict, ctx) -> str:
    path = data.get("asset", "")
    lines = [f"{ctx.aid(path)} {data.get('name')} ({data.get('kind')}) parent {data.get('parent')}"]
    functions = data.get("functions", [])
    if functions:
        lines.append(f"Functions ({len(functions)}):")
        for item in functions:
            iface = f" <{item['interface']}>" if item.get("interface") else ""
            lines.append(f"  {_signature(item)}{iface}  [{item.get('nodes', 0)}n]")
    graphs = data.get("event_graphs", [])
    if graphs:
        lines.append("Graphs: " + ", ".join(f"{g['name']} [{g.get('nodes', 0)}n]" for g in graphs))
    macros = data.get("macros", [])
    if macros:
        lines.append("Macros: " + ", ".join(f"{g['name']} [{g.get('nodes', 0)}n]" for g in macros))
    events = data.get("events", [])
    if events:
        lines.append("Events: " + ", ".join(events))
    variables = data.get("variables", [])
    if variables:
        lines.append(f"Variables ({len(variables)}):")
        for var in variables:
            flags = []
            if var.get("category"):
                flags.append(f"[{var['category']}]")
            if var.get("replicated"):
                flags.append("rep" + (f"({var['rep_notify']})" if var.get("rep_notify") else ""))
            if var.get("instance_editable"):
                flags.append("editable")
            if var.get("expose_on_spawn"):
                flags.append("spawn")
            if var.get("read_only"):
                flags.append("readonly")
            default = f" = {clip(var['default'], 60)}" if var.get("default") not in (None, "") else ""
            lines.append(f"  {var['name']}:{var['type']}{default} {' '.join(flags)}".rstrip())
    components = (data.get("components") or {}).get("components", [])
    if components:
        lines.append(f"Components ({len(components)}):")
        for comp in components:
            parent = f" < {comp['parent']}" if comp.get("parent") else ""
            native = " (native)" if comp.get("native") else ""
            lines.append(f"  {comp['name']}:{comp['class']}{parent}{native}")
    if data.get("interfaces"):
        lines.append("Interfaces: " + ", ".join(data["interfaces"]))
    if data.get("delegates"):
        lines.append("Delegates: " + ", ".join(data["delegates"]))
    if data.get("referencers"):
        lines.append(f"Referencers ({len(data['referencers'])}): " + ", ".join(short(p) for p in data["referencers"][:10]))
    return "\n".join(lines)


def format_components(data: dict, ctx) -> str:
    lines = [f"{ctx.aid(data.get('asset', ''))} components:"]
    for comp in data.get("components", []):
        parent = f" < {comp['parent']}" if comp.get("parent") else ""
        native = " (native)" if comp.get("native") else ""
        lines.append(f"{comp['name']}:{comp['class']}{parent}{native}")
        if comp.get("props"):
            lines.append("  " + kv_line(comp["props"], 12))
    return "\n".join(lines)


# ---------------------------------------------------------------- graphs

def node_label(node: dict) -> str:
    kind = node.get("kind", "?")
    member = node.get("member", "")
    mclass = node.get("member_class", "")
    title = node.get("title", "")
    if kind == "CallFunction":
        label = f"{member or title}" + (f" ({mclass})" if mclass and mclass != "self" else "")
    elif kind == "CallParentFunction":
        label = f"Parent::{member}"
    elif kind == "VariableGet":
        label = f"Get {member}" + (f" ({mclass})" if mclass and mclass != "self" else "")
    elif kind == "VariableSet":
        label = f"Set {member}" + (f" ({mclass})" if mclass and mclass != "self" else "")
    elif kind == "Event":
        label = f"Event {member}"
    elif kind == "CustomEvent":
        label = f"CustomEvent {member}"
    elif kind == "ComponentBoundEvent":
        label = f"Event {member}"
    elif kind == "DynamicCast":
        label = f"Cast {member}"
    elif kind == "MacroInstance":
        label = f"Macro {member}"
    elif kind == "FunctionEntry":
        label = f"Entry {member}"
    elif kind == "FunctionResult":
        label = "Return"
    elif kind == "Comment":
        label = f'Comment "{clip(member, 60)}"'
    elif member:
        label = f"{kind} {member}"
    else:
        label = kind if not title or title.replace(" ", "") == kind else f"{kind} '{clip(title, 40)}'"
    flags = []
    if node.get("pure"):
        flags.append("pure")
    if node.get("latent"):
        flags.append("latent")
    if node.get("disabled"):
        flags.append("disabled")
    if flags:
        label += " [" + ",".join(flags) + "]"
    if node.get("comment"):
        label += f' "{clip(node["comment"], 50)}"'
    if node.get("error"):
        label += f" !ERR {clip(node['error'], 80)}"
    return label


def _pin_ref(nid: str, pin_name: str) -> str:
    if pin_name in (EXEC_IN, EXEC_OUT):
        return nid
    return f"{nid}.{pin_name}"


def format_graph(data: dict, ctx, path: str, detail_pins: bool = False) -> str:
    node_map = ctx.session.nodes_for(path)
    graph = data.get("graph", "")
    nodes = data.get("nodes", [])
    total = data.get("total_nodes", len(nodes))
    header = f"{ctx.aid(path)} {short(path)} graph {graph} ({data.get('kind', '')}, {total} nodes)"
    if data.get("around"):
        header += f" window around {node_map.id_for(data['around'])} depth {data.get('depth', 1)}"
    lines = [header]
    if data.get("error"):
        lines.append("error: " + data["error"])
    sig = data.get("signature")
    if sig:
        lines.append("signature: " + _signature({"name": graph, **sig}))
    # First pass: assign ids in display order so numbering is stable per session.
    ids = {}
    for node in nodes:
        nid = node_map.id_for(node["guid"], graph, node.get("kind", ""))
        ids[node["guid"]] = nid
        node_map.set_pins(nid, [pin["name"] for pin in node.get("pins", [])])
    shown = set(ids.values())
    for node in nodes:
        nid = ids[node["guid"]]
        lines.append(f"{nid} {node_label(node)}")
        for pin in node.get("pins", []):
            name = pin.get("name", "")
            is_exec = pin.get("type") == "exec"
            links = pin.get("links", [])
            if pin.get("dir") == "out":
                if is_exec and links:
                    targets = ", ".join(_pin_ref(node_map.id_for(l["node"], graph), l["pin"]) for l in links)
                    lines.append(f"  {name} -> {targets}")
                elif links and not is_exec:
                    outside = [l for l in links if node_map.id_for(l["node"], graph) not in shown]
                    if outside:
                        targets = ", ".join(_pin_ref(node_map.id_for(l["node"], graph), l["pin"]) for l in outside)
                        lines.append(f"  {name} -> {targets}")
                    elif detail_pins:
                        lines.append(f"  out {name}:{pin.get('type')}")
                elif detail_pins:
                    lines.append(f"  out {name}:{pin.get('type')}")
            else:
                if links:
                    if is_exec:
                        outside = [l for l in links if node_map.id_for(l["node"], graph) not in shown]
                        if outside:
                            lines.append(f"  {name} <- " + ", ".join(_pin_ref(node_map.id_for(l["node"], graph), l["pin"]) for l in outside))
                    else:
                        sources = ", ".join(_pin_ref(node_map.id_for(l["node"], graph), l["pin"]) for l in links)
                        lines.append(f"  {name} <- {sources}")
                elif "default" in pin and not is_exec:
                    lines.append(f"  {name} = {clip(pin['default'], 60)}")
                elif detail_pins and not is_exec:
                    lines.append(f"  in {name}:{pin.get('type')}")
    matched = data.get("matched", len(nodes))
    offset = data.get("offset", 0)
    if data.get("truncated") or offset:
        lines.append(f"nodes {offset + 1}-{offset + len(nodes)} of {matched} (offset={offset + len(nodes)} for more)")
    return "\n".join(lines)


def format_found_nodes(data: dict, ctx, path: str) -> str:
    node_map = ctx.session.nodes_for(path)
    nodes = data.get("nodes", [])
    lines = [f"{ctx.aid(path)} {short(path)}: {data.get('matched', len(nodes))} nodes"]
    for node in nodes:
        nid = node_map.id_for(node["guid"], node.get("graph", ""), node.get("kind", ""))
        lines.append(f"{nid} {node_label(node)}  [{node.get('graph', '')}]")
    if data.get("truncated"):
        lines.append("(truncated; refine query or use graph=)")
    return "\n".join(lines)


def format_node_created(node: dict, ctx, path: str, graph: str) -> List[str]:
    node_map = ctx.session.nodes_for(path)
    nid = node_map.id_for(node["guid"], graph, node.get("kind", ""))
    node_map.set_pins(nid, [pin["name"] for pin in node.get("pins", [])])
    lines = [f"+ {nid} {node_label(node)} [{graph}]"]
    for pin in node.get("pins", []):
        if pin.get("dir") == "in" and pin.get("type") != "exec" and not pin.get("hidden"):
            default = f" = {clip(pin['default'], 40)}" if "default" in pin else ""
            lines.append(f"  in {pin['name']}:{pin.get('type')}{default}")
        elif pin.get("dir") == "out" and not pin.get("hidden"):
            lines.append(f"  out {pin['name']}:{pin.get('type')}")
    for conn in node.get("connections", []):
        lines.append(f"  {_conn(conn, node_map, graph)}")
    return lines


def _conn(conn: dict, node_map, graph: str) -> str:
    def ref(text: str) -> str:
        guid, _, pin = text.partition(".")
        return _pin_ref(node_map.id_for(guid, graph), pin)
    extra = f" (replaced {conn['replaced']})" if conn.get("replaced") else ""
    return f"{ref(conn['from'])} -> {ref(conn['to'])}{extra}"


def format_connections(data: dict, ctx, path: str) -> str:
    node_map = ctx.session.nodes_for(path)
    lines = []
    for conn in data.get("connected", []):
        lines.append("+ " + _conn(conn, node_map, ""))
    for failed in data.get("failed", []):
        lines.append("! " + clip(failed, 200))
    return "\n".join(lines) if lines else "(no changes)"


# ---------------------------------------------------------------- widgets

def format_widget_tree(data: dict, ctx) -> str:
    path = data.get("asset", "")
    widgets = data.get("widgets", [])
    lines = [f"{ctx.aid(path)} {data.get('name')} (UserWidget < {data.get('parent')}) {data.get('total', len(widgets))} widgets"]
    # Determine last-child flags per depth for connectors.
    last_flags = []
    for index, widget in enumerate(widgets):
        depth = widget.get("depth", 0)
        is_last = True
        for other in widgets[index + 1:]:
            if other.get("depth", 0) < depth:
                break
            if other.get("depth", 0) == depth:
                is_last = False
                break
        last_flags.append(is_last)
    stack: List[bool] = []
    for index, widget in enumerate(widgets):
        depth = widget.get("depth", 0)
        stack = stack[:max(depth - 1, 0)]
        prefix = ""
        if depth > 0:
            prefix = "".join("   " if flag else "│  " for flag in stack) + ("└─ " if last_flags[index] else "├─ ")
            stack.append(last_flags[index])
        label = f"{widget['name']} {widget['class']}"
        if widget.get("text"):
            label += f' "{clip(widget["text"], 40)}"'
        if widget.get("style"):
            label += f" {widget['style']}"
        if widget.get("var"):
            label += " var"
        if widget.get("visibility"):
            label += f" ({widget['visibility']})"
        if widget.get("events"):
            label += " events:" + ",".join(e.split("=", 1)[0] for e in widget["events"])
        lines.append(prefix + label)
    styles = data.get("styles") or {}
    if styles:
        lines.append("Styles:")
        for sid, style in styles.items():
            members = style.get("members", [])
            lines.append(f"{sid} {style.get('class')} ({len(members)}): " + (kv_line(style.get("props", {}), 8, 70) or "(defaults)"))
    if data.get("animations"):
        lines.append("Animations: " + ", ".join(data["animations"]))
    return "\n".join(lines)


def format_widget(data: dict, ctx) -> str:
    path = data.get("asset", "")
    header = f"{ctx.aid(path)} {data.get('widget')} {data.get('class')}"
    if data.get("parent"):
        header += f"  parent {data['parent']}[{data.get('index', 0)}]"
    if data.get("var"):
        header += " var"
    lines = [header]
    props = data.get("props") or {}
    lines.append("props: " + (kv_line(props, 40, 120) or "(all defaults)"))
    slot = data.get("slot")
    if slot:
        lines.append(f"slot {slot.get('class')}: " + (kv_line(slot.get("props", {}), 20, 120) or "(defaults)"))
    if data.get("children"):
        lines.append("children: " + ", ".join(data["children"]))
    if data.get("events"):
        lines.append("events: " + ", ".join(data["events"]))
    if data.get("bound"):
        lines.append("bound: " + ", ".join(data["bound"]))
    return "\n".join(lines)


def format_animations(data: dict, ctx) -> str:
    lines = [f"{ctx.aid(data.get('asset', ''))} animations: {len(data.get('animations', []))}"]
    for anim in data.get("animations", []):
        lines.append(f"{anim['name']} ({anim.get('seconds', 0):.2f}s)")
        for binding in anim.get("bindings", []):
            lines.append(f"  {binding['widget']}: " + ", ".join(binding.get("tracks", [])))
    return "\n".join(lines)


def format_preview(data: dict, ctx) -> str:
    path = data.get("asset", "")
    lines = [f"Preview {ctx.aid(path)} {short(path)} {data.get('width')}x{data.get('height')}" + (f" -> {data['image']}" if data.get("image") else "")]
    for item in data.get("layout", []):
        hidden = " hidden" if item.get("hidden") else ""
        lines.append(f"{' ' * item.get('depth', 0)}{item['widget']} {item['x']},{item['y']} {item['w']}x{item['h']}{hidden}")
    return "\n".join(lines)


def format_layout_check(layout: List[dict], names: Iterable[str]) -> List[str]:
    """Spacing/size report for a set of siblings (visual validation without images)."""
    wanted = [item for item in layout if item["widget"] in set(names)]
    wanted.sort(key=lambda i: (i["y"], i["x"]))
    lines = []
    prev = None
    for item in wanted:
        gap = "" if prev is None else f" gap {item['y'] - (prev['y'] + prev['h'])}px"
        lines.append(f"{item['widget']}: {item['w']}x{item['h']} at {item['x']},{item['y']}{gap}")
        prev = item
    return lines


# ---------------------------------------------------------------- build

def format_compile(data: dict, ctx) -> str:
    results = data.get("results", [])
    lines = [f"Compile {data.get('succeeded', 0)}/{data.get('total', len(results))} OK"]
    for item in results:
        path = item.get("asset", "")
        label = f"{ctx.aid(path)} {short(path)}" if path.startswith("/") else path
        if not item.get("ok"):
            lines.append(f"{label}: ERROR {item.get('errors', '?')} errors {item.get('warnings', 0)} warnings" + (f" ({item['error']})" if item.get("error") else ""))
        else:
            warn = item.get("warnings", 0)
            lines.append(f"{label}: OK" + (f" ({warn} warnings)" if warn else ""))
        node_map = ctx.session.nodes_for(path) if path.startswith("/") else None
        for err in item.get("node_errors", [])[:12]:
            nid = node_map.id_for(err["node"], err.get("graph", ""), err.get("kind", "")) if node_map else err["node"]
            lines.append(f"  {nid} [{err.get('graph')}] {err.get('level')}: {clip(err.get('text', ''), 160)}")
        shown_texts = {clip(e.get("text", ""), 160) for e in item.get("node_errors", [])}
        for msg in item.get("messages", []):
            text = clip(msg.get("text", ""), 160)
            if not any(text.startswith(s[:40]) for s in shown_texts if s):
                lines.append(f"  {msg.get('level')}: {text}")
        if "saved" in item:
            lines.append("  saved" if item["saved"] else f"  save FAILED {item.get('save_error', '')}")
        validation = item.get("validation")
        if validation:
            if validation.get("ok"):
                lines.append("  validation: passed" + (f" ({len(validation.get('warnings', []))} warnings)" if validation.get("warnings") else ""))
            else:
                lines.append("  validation: FAILED")
                for err in validation.get("errors", [])[:8]:
                    lines.append("    " + clip(err, 160))
    return "\n".join(lines)


def format_save(data: dict, ctx) -> str:
    lines = [f"Saved {data.get('saved', 0)}/{data.get('total', 0)}"]
    for item in data.get("results", []):
        path = item.get("asset", "")
        status = "saved" if item.get("ok") else f"FAILED {item.get('error', '')}"
        if item.get("ok") and not item.get("was_dirty"):
            status = "unchanged"
        lines.append(f"{ctx.aid(path) if path.startswith('/') else ''} {short(path)}: {status}".strip())
    return "\n".join(lines)


def format_validate(data: dict, ctx) -> str:
    lines = [f"Validation {data.get('passed', 0)}/{data.get('total', 0)} passed"]
    for item in data.get("results", []):
        path = item.get("asset", "")
        label = f"{ctx.aid(path)} {short(path)}" if path.startswith("/") else path
        if item.get("ok"):
            lines.append(f"{label}: passed" + (f" ({len(item.get('warnings', []))} warnings)" if item.get("warnings") else ""))
        else:
            lines.append(f"{label}: FAILED" + (f" ({item['error']})" if item.get("error") else ""))
            for err in item.get("errors", [])[:8]:
                lines.append("  " + clip(err, 160))
    return "\n".join(lines)


def format_changeset(cs: dict, ctx, body_lines: List[str]) -> str:
    assets = ", ".join(ctx.label(p) for p in cs.get("assets", []))
    lines = [f"ChangeSet {cs['id']} ({cs['tool']}) {assets}"]
    lines.extend(body_lines)
    return "\n".join(lines)


def format_log(data: dict) -> str:
    lines = [f"{data.get('errors', 0)} errors, {data.get('warnings', 0)} warnings (matched {data.get('matched', 0)})"]
    for entry in data.get("entries", []):
        lines.append(f"[{entry.get('level')}] {entry.get('category')}: {clip(entry.get('message', ''), 200)}")
    lines.append(f"full log: {data.get('logFile', '')}")
    return "\n".join(lines)
