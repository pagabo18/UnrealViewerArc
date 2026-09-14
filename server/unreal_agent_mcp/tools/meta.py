"""Capabilities, logs, batch, source control, misc."""
import json

from . import P, schema, tool
from .common import as_list, finish_edit, resolve_asset_or_fail
from ..bridge import PluginUnavailable
from ..formatters import format_log, short
from ..protocol import ToolError


@tool("get_capabilities", "Capability registry of the connected plugin (supported | experimental | unsupported). Check before advanced operations.",
      schema({}), needs_editor=False)
def get_capabilities(ctx):
    try:
        caps = ctx.get_capabilities()
        engine = ctx.client.health().get("engine", "?")
    except PluginUnavailable:
        return "EDITOR UNAVAILABLE (capabilities unknown until the editor is connected)"
    lines = [f"Plugin capabilities (engine {engine}):"]
    for key, status in sorted(caps.items()):
        lines.append(f"{key} = {status}")
    return "\n".join(lines)


@tool("get_log", "Captured editor errors/warnings (counts + last entries). Full log stays on disk.",
      schema({"level": P("string", "error (default) | warning | all"), "category": P("string", "Log category filter, e.g. LogBlueprint."),
              "contains": P("string", "Substring filter."), "limit": P("integer", "Max entries (default 15)."), "clear": P("boolean", "Clear the buffer after reading.")}))
def get_log(ctx, level=None, category=None, contains=None, limit=None, clear=None):
    params = {"level": level or "error", "limit": int(limit or 15)}
    if category:
        params["category"] = category
    if contains:
        params["contains"] = contains
    if clear:
        params["clear"] = True
    return format_log(ctx.client.call("system.log", params))


@tool("batch",
      "Run several plugin operations in ONE editor transaction (one undo step). atomic=true (default) rolls everything back if an op fails. "
      "ops: [{tool: 'add_node', args: {...}}, ...] using the same tool names/args as the individual tools (N#/A# ids allowed). "
      "Do not include compile/save inside atomic batches; compile afterwards.",
      schema({
          "ops": P("array", "Operations [{tool, args}].", items={"type": "object"}),
          "atomic": P("boolean", "Roll back on failure (default true)."),
          "title": P("string", "Transaction title."),
          "compile": P("boolean", "Compile touched assets afterwards (default config autoCompile)."),
      }, ["ops"]), mutating=True)
def batch(ctx, ops, atomic=None, title=None, compile=None):
    from . import build_registry  # local import to avoid cycles
    from .. import tools as tools_pkg
    registry = tools_pkg.ToolRegistry(list(tools_pkg._REGISTRY))
    plugin_ops = []
    touched = []
    summaries = []
    for index, op in enumerate(ops or []):
        if not isinstance(op, dict) or "tool" not in op:
            raise ToolError(f"op {index} must be {{tool, args}}.")
        name = op["tool"]
        args = op.get("args") or {}
        translated = _translate(ctx, name, args)
        if translated is None:
            raise ToolError(f"op {index}: tool '{name}' cannot run inside a batch (use it directly).")
        cmd, params, asset, summary = translated
        plugin_ops.append({"cmd": cmd, "params": params})
        if asset:
            touched.append(asset)
        summaries.append(summary)
    params = {"ops": plugin_ops, "atomic": True if atomic is None else bool(atomic)}
    if title:
        params["title"] = title
    result = ctx.client.call("batch", params, timeout=600)
    batch_id = f"B{next(ctx.session.batch_counter)}"
    lines = [f"Batch {batch_id}: {result.get('succeeded', 0)}/{result.get('total', 0)} ok" + (" ROLLED BACK" if result.get("rolled_back") else "")]
    for item, summary in zip(result.get("results", []), summaries):
        if item.get("ok"):
            lines.append(f"  ok {summary}")
            _learn_ids(ctx, item.get("result") or {}, touched)
        else:
            err = item.get("error") or {}
            lines.append(f"  FAILED {summary}: {err.get('message', '')}")
    touched = list(dict.fromkeys(touched))
    if result.get("rolled_back") or result.get("succeeded", 0) == 0:
        for path in touched:
            ctx.session.invalidate(path)
        return "\n".join(lines)
    return finish_edit(ctx, "batch", touched, lines[1:], compile_now=compile).replace("ChangeSet", lines[0] + "\nChangeSet", 1)


def _learn_ids(ctx, result: dict, touched):
    """Register node ids for nodes created inside a batch so follow-up calls can use N#."""
    asset = result.get("asset")
    if not asset:
        return
    node_map = ctx.session.nodes_for(asset)
    if result.get("guid"):
        node_map.id_for(result["guid"], result.get("graph", ""), result.get("kind", ""))
        node_map.set_pins(node_map.id_for(result["guid"]), [p["name"] for p in result.get("pins", [])])
    if result.get("node"):
        node_map.id_for(result["node"], result.get("graph", ""), "ComponentBoundEvent")


def _translate(ctx, name: str, args: dict):
    """Maps a tool call to a plugin command for batching. Returns (cmd, params, asset_path, summary) or None."""
    from .common import pin_refs, graph_param, as_dict
    asset = args.get("asset")
    path = resolve_asset_or_fail(ctx, asset) if asset else None
    a = dict(args)
    if name == "blueprint_variable":
        p = {k: v for k, v in a.items() if k not in ("asset", "compile")}
        p["asset"] = path
        return "blueprint.variable", p, path, f"variable {a.get('op')} {a.get('name')}"
    if name == "blueprint_function":
        p = {k: v for k, v in a.items() if k not in ("asset", "compile")}
        p["asset"] = path
        return "blueprint.function", p, path, f"function {a.get('op')} {a.get('name')}"
    if name == "blueprint_component":
        p = {k: v for k, v in a.items() if k not in ("asset", "compile")}
        p["asset"] = path
        return "blueprint.component", p, path, f"component {a.get('op')} {a.get('name')}"
    if name == "add_node":
        p = {k: v for k, v in a.items() if k not in ("asset", "compile", "after", "before", "connect", "graph")}
        p["asset"] = path
        p["graph"] = graph_param(ctx, path, a.get("graph")) or "EventGraph"
        if a.get("after"):
            p["after"] = pin_refs(ctx, path, a["after"])[0]
        if a.get("before"):
            p["before"] = pin_refs(ctx, path, a["before"])[0]
        if a.get("connect"):
            p["connect"] = {k: pin_refs(ctx, path, v)[0] for k, v in as_dict(a["connect"]).items()}
        return "graph.add_node", p, path, f"add_node {a.get('type')} {a.get('function') or a.get('variable') or a.get('name') or ''}".rstrip()
    if name == "connect":
        pairs = a.get("links") or [[a.get("from"), a.get("to")]]
        links = [{"from": pin_refs(ctx, path, x[0])[0], "to": pin_refs(ctx, path, x[1])[0]} for x in pairs]
        p = {"asset": path, "links": links}
        if a.get("force"):
            p["force"] = True
        return "graph.connect", p, path, f"connect {len(links)} link(s)"
    if name == "set_pins":
        node = a.get("node")
        p = {"asset": path, "node": ctx.session.node_ref_to_guid(path, node), "pins": a.get("pins") or {}}
        return "graph.set_pins", p, path, f"set_pins {node}"
    if name == "delete_nodes":
        p = {"asset": path, "nodes": [ctx.session.node_ref_to_guid(path, n) for n in as_list(a.get("nodes"))]}
        return "graph.delete_nodes", p, path, f"delete {len(p['nodes'])} node(s)"
    if name in ("clone_widget", "add_widget", "remove_widget", "move_widget", "set_widget", "copy_widget_style", "bind_widget_event"):
        cmd = {"clone_widget": "widget.clone", "add_widget": "widget.add", "remove_widget": "widget.remove", "move_widget": "widget.move",
               "set_widget": "widget.set", "copy_widget_style": "widget.copy_style", "bind_widget_event": "widget.bind_event"}[name]
        p = {k: v for k, v in a.items() if k not in ("asset", "compile")}
        p["asset"] = path
        return cmd, p, path, f"{name} {a.get('widget') or a.get('new_name') or a.get('name') or a.get('source') or ''}".rstrip()
    return None


@tool("source_control_status", "Source control / dirty state of assets (Git or Perforce detection).",
      schema({"assets": P("array", "Assets (A#/paths/names).", items={"type": "string"})}, ["assets"]))
def source_control_status(ctx, assets):
    paths = [resolve_asset_or_fail(ctx, a) for a in as_list(assets)]
    result = ctx.client.call("assets.source_control", {"assets": paths})
    lines = [f"provider: {result.get('provider')}"]
    for item in result.get("assets", []):
        flags = [k for k in ("dirty", "checkedOut", "checkedOutOther", "modified") if item.get(k)]
        if item.get("controlled") is False:
            flags.append("not-controlled")
        if not item.get("exists", True):
            flags.append("new")
        lines.append(f"{ctx.aid(item['asset'])} {short(item['asset'])}: " + (", ".join(flags) or "clean"))
    return "\n".join(lines)


@tool("open_in_editor", "Open an asset in its Unreal editor window (for the user to look at).",
      schema({"asset": P("string", "A# / path / name.")}, ["asset"]))
def open_in_editor(ctx, asset):
    path = resolve_asset_or_fail(ctx, asset)
    ctx.client.call("assets.open", {"asset": path})
    return f"Opened {ctx.label(path)}"


@tool("plugin_raw", "Debug only: call a plugin command directly and return raw JSON.",
      schema({"cmd": P("string", "Command name, e.g. system.ping."), "params": P("object", "Parameters.")}, ["cmd"]))
def plugin_raw(ctx, cmd, params=None):
    return json.dumps(ctx.client.call(cmd, params or {}), ensure_ascii=False, separators=(",", ":"))
