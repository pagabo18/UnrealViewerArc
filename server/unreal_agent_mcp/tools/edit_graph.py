"""Graph edits: nodes, links, pin defaults, macros (insert/replace/clone)."""
from . import P, schema, tool
from .common import as_dict, as_list, finish_edit, graph_param, pin_refs, resolve_asset_or_fail
from ..formatters import format_connections, format_node_created, node_label
from ..protocol import ToolError

NODE_TYPES = ("function_call (function=Class.Func|Func) | variable_get/variable_set (variable=, class=) | event (event=, class=) | "
              "custom_event (name=, inputs=['V:int']) | branch | sequence (outputs=) | cast (class=) | self | macro (macro=ForEachLoop) | "
              "reroute | comment (text=) | make_struct/break_struct (struct=) | switch_enum (enum=) | switch_string (cases=) | switch_int | "
              "spawn_actor (class=) | create_widget (class=) | parent_call (function=) | print (text=) | delay (duration=) | raw (class=K2Node_X)")


def _node_params(kwargs) -> dict:
    out = {}
    for key in ("function", "variable", "class", "event", "name", "inputs", "macro", "text", "struct", "enum", "cases", "duration", "outputs", "pure", "comment"):
        value = kwargs.get(key)
        if value is not None:
            out[key] = as_list(value) if key in ("inputs", "cases") else value
    return out


@tool("add_node",
      "Create a node in a graph. Optional after=N#[.pin] splices it into an exec chain (after.then -> new -> old target); before=N# likewise. "
      "pins={name:value} sets defaults; connect={myPin: 'N#.pin'} wires pins. Types: " + NODE_TYPES,
      schema({
          "asset": P("string", "A# / path / name."),
          "graph": P("string", "Graph name (default EventGraph, or inferred from after/before)."),
          "type": P("string", "Node type (see description)."),
          "function": P("string", "For function_call/parent_call: Func, Class.Func, BP_X.Func."),
          "variable": P("string", "For variable_get/set."),
          "class": P("string", "Class for cast/event/spawn_actor/create_widget/variable owner/raw."),
          "event": P("string", "Event name for type=event."),
          "name": P("string", "Name for custom_event."),
          "inputs": P("array", "custom_event inputs 'Name:type'.", items={"type": "string"}),
          "macro": P("string", "Macro name (ForEachLoop, DoOnce, Gate, IsValid, ...)."),
          "text": P("string", "print text / comment text."),
          "struct": P("string", "Struct for make/break_struct."),
          "enum": P("string", "Enum for switch_enum."),
          "cases": P("array", "Cases for switch_string.", items={"type": "string"}),
          "duration": P("number", "Delay duration."),
          "outputs": P("integer", "Sequence output count."),
          "pure": P("boolean", "Pure cast."),
          "after": P("string", "Insert after this node/pin (N# or N#.then)."),
          "before": P("string", "Insert before this node/pin (N# or N#.execute)."),
          "pins": P("object", "Pin defaults {pinName: value}."),
          "connect": P("object", "Connections {myPin: 'N#.otherPin'}."),
          "comment": P("string", "Node comment bubble."),
          "force": P("boolean", "Replace existing links on conflicting input pins."),
          "compile": P("boolean", "Compile after the edit."),
      }, ["asset", "type"]), mutating=True)
def add_node(ctx, asset, type, **kwargs):
    path = resolve_asset_or_fail(ctx, asset)
    after = kwargs.get("after")
    before = kwargs.get("before")
    graph = graph_param(ctx, path, kwargs.get("graph"))
    if not graph:
        for ref in (after, before):
            if ref:
                nid = ref.split(".")[0].split(":")[0].upper()
                graph = ctx.session.nodes_for(path).graph_of.get(nid, "")
                if graph:
                    break
    params = {"asset": path, "graph": graph or "EventGraph", "type": type}
    params.update(_node_params(kwargs))
    if after:
        params["after"] = pin_refs(ctx, path, after)[0]
    if before:
        params["before"] = pin_refs(ctx, path, before)[0]
    pins = as_dict(kwargs.get("pins"))
    if pins:
        params["pins"] = pins
    connect = as_dict(kwargs.get("connect"))
    if connect:
        params["connect"] = {key: pin_refs(ctx, path, value)[0] for key, value in connect.items()}
    if kwargs.get("force"):
        params["force"] = True
    result = ctx.client.call("graph.add_node", params)
    lines = format_node_created(result, ctx, path, result.get("graph", params["graph"]))
    for failed in result.get("failed", []):
        lines.append("  ! " + failed)
    return finish_edit(ctx, "add_node", [path], lines, compile_now=kwargs.get("compile"))


@tool("delete_nodes", "Delete nodes (N# ids). Exec flow through a deleted node is bridged automatically unless reconnect=false.",
      schema({
          "asset": P("string", "A# / path / name."),
          "nodes": P("array", "Node ids.", items={"type": "string"}),
          "reconnect": P("boolean", "Bridge exec links around deleted nodes (default true)."),
          "compile": P("boolean", "Compile after the edit."),
      }, ["asset", "nodes"]), mutating=True)
def delete_nodes(ctx, asset, nodes, reconnect=None, compile=None):
    path = resolve_asset_or_fail(ctx, asset)
    node_map = ctx.session.nodes_for(path)
    refs = as_list(nodes)
    guids = []
    for ref in refs:
        try:
            guids.append(ctx.session.node_ref_to_guid(path, ref))
        except KeyError as exc:
            raise ToolError(str(exc))
    params = {"asset": path, "nodes": guids}
    if reconnect is not None:
        params["reconnect"] = bool(reconnect)
    result = ctx.client.call("graph.delete_nodes", params)
    lines = []
    for guid in result.get("deleted", []):
        nid = node_map.id_for(guid)
        lines.append(f"- {nid} {node_map.kinds.get(nid, '')}".rstrip())
    for bridge in result.get("reconnected", []):
        a, _, b = bridge.partition(" -> ")
        lines.append(f"  {node_map.id_for(a)} -> {node_map.id_for(b)} (bridged)")
    for failed in result.get("failed", []):
        lines.append("  ! " + failed)
    return finish_edit(ctx, "delete_nodes", [path], lines, compile_now=compile)


@tool("connect", "Connect pins. from/to are 'N#.pin' (bare N# = default exec pin). Or links=[['N1.then','N2'], ...]. Conflicting input links fail unless force=true.",
      schema({
          "asset": P("string", "A# / path / name."),
          "from": P("string", "Source output pin 'N#.pin'."),
          "to": P("string", "Target input pin 'N#.pin'."),
          "links": P("array", "Multiple [from, to] pairs.", items={"type": "array", "items": {"type": "string"}}),
          "force": P("boolean", "Replace existing links."),
          "compile": P("boolean", "Compile after the edit."),
      }, ["asset"]), mutating=True)
def connect(ctx, asset, links=None, force=None, compile=None, **kwargs):
    path = resolve_asset_or_fail(ctx, asset)
    pairs = []
    if links:
        for item in links:
            if isinstance(item, dict):
                pairs.append((item.get("from"), item.get("to")))
            else:
                pairs.append((item[0], item[1]))
    elif kwargs.get("from") and kwargs.get("to"):
        pairs.append((kwargs["from"], kwargs["to"]))
    if not pairs:
        raise ToolError("Provide from+to or links.")
    params = {"asset": path, "links": [{"from": pin_refs(ctx, path, a)[0], "to": pin_refs(ctx, path, b)[0]} for a, b in pairs]}
    if force:
        params["force"] = True
    result = ctx.client.call("graph.connect", params)
    return finish_edit(ctx, "connect", [path], format_connections(result, ctx, path).split("\n"), compile_now=compile)


@tool("disconnect", "Break links: pin='N#.pin' breaks all links of that pin; from+to breaks one link.",
      schema({
          "asset": P("string", "A# / path / name."),
          "pin": P("string", "Pin whose links are all broken."),
          "from": P("string", "Source pin of a single link."),
          "to": P("string", "Target pin of a single link."),
          "compile": P("boolean", "Compile after the edit."),
      }, ["asset"]), mutating=True)
def disconnect(ctx, asset, pin=None, compile=None, **kwargs):
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path}
    if pin:
        params["pin"] = pin_refs(ctx, path, pin)[0]
    elif kwargs.get("from") and kwargs.get("to"):
        params["from"] = pin_refs(ctx, path, kwargs["from"])[0]
        params["to"] = pin_refs(ctx, path, kwargs["to"])[0]
    else:
        raise ToolError("Provide pin or from+to.")
    result = ctx.client.call("graph.disconnect", params)
    return finish_edit(ctx, "disconnect", [path], [f"- broke {result.get('broken', 0)} link(s) at {pin or kwargs.get('from')}"], compile_now=compile)


@tool("set_pins", "Set pin default values (and/or comment, position, enabled) on a node.",
      schema({
          "asset": P("string", "A# / path / name."),
          "node": P("string", "Node id N#."),
          "pins": P("object", "{pinName: value}. Objects/classes by path or name; enums by name; structs as '(X=1,Y=2)'."),
          "comment": P("string", "Node comment."),
          "enabled": P("boolean", "Enable/disable the node."),
          "compile": P("boolean", "Compile after the edit."),
      }, ["asset", "node"]), mutating=True)
def set_pins(ctx, asset, node, pins=None, comment=None, enabled=None, compile=None):
    path = resolve_asset_or_fail(ctx, asset)
    node_map = ctx.session.nodes_for(path)
    try:
        guid = ctx.session.node_ref_to_guid(path, node)
    except KeyError as exc:
        raise ToolError(str(exc))
    params = {"asset": path, "node": guid}
    pins = as_dict(pins)
    if pins:
        params["pins"] = {node_map.pin_name(node.upper(), key): value for key, value in pins.items()}
    if comment is not None:
        params["comment"] = comment
    if enabled is not None:
        params["enabled"] = bool(enabled)
    result = ctx.client.call("graph.set_pins", params)
    nid = node_map.id_for(result["guid"])
    lines = [f"~ {nid} {node_label(result)}"]
    for pin in result.get("pins", []):
        if pins and pin.get("name") in params.get("pins", {}) and "default" in pin:
            lines.append(f"  {pin['name']} = {pin['default']}")
    for failed in result.get("failed", []):
        lines.append("  ! " + failed)
    return finish_edit(ctx, "set_pins", [path], lines, compile_now=compile)


@tool("replace_node", "Replace a node with a new one (same type params as add_node); links are migrated by pin name, then by type.",
      schema({
          "asset": P("string", "A# / path / name."),
          "node": P("string", "Node id N# to replace."),
          "type": P("string", "New node type (see add_node)."),
          "function": P("string", "Function for function_call."),
          "variable": P("string", "Variable for variable_get/set."),
          "class": P("string", "Class param."),
          "macro": P("string", "Macro name."),
          "text": P("string", "Text param."),
          "duration": P("number", "Delay duration."),
          "compile": P("boolean", "Compile after the edit."),
      }, ["asset", "node", "type"]), mutating=True)
def replace_node(ctx, asset, node, type, **kwargs):
    path = resolve_asset_or_fail(ctx, asset)
    node_map = ctx.session.nodes_for(path)
    try:
        guid = ctx.session.node_ref_to_guid(path, node)
    except KeyError as exc:
        raise ToolError(str(exc))
    params = {"asset": path, "node": guid, "type": type}
    params.update(_node_params(kwargs))
    result = ctx.client.call("graph.replace_node", params)
    graph = node_map.graph_of.get(node.upper(), "")
    lines = [f"- {node.upper()} (replaced)"]
    lines.extend(format_node_created(result, ctx, path, graph))
    if result.get("migrated"):
        lines.append("  migrated: " + ", ".join(result["migrated"][:8]))
    if result.get("dropped"):
        lines.append("  ! dropped: " + ", ".join(result["dropped"][:8]))
    return finish_edit(ctx, "replace_node", [path], lines, compile_now=kwargs.get("compile"))


@tool("clone_nodes", "Duplicate nodes (keeping links among them) with an offset, optionally into another graph.",
      schema({
          "asset": P("string", "A# / path / name."),
          "nodes": P("array", "Node ids.", items={"type": "string"}),
          "target_graph": P("string", "Destination graph (default same)."),
          "dy": P("integer", "Vertical offset (default 300)."),
          "compile": P("boolean", "Compile after the edit."),
      }, ["asset", "nodes"]), mutating=True)
def clone_nodes(ctx, asset, nodes, target_graph=None, dy=None, compile=None):
    path = resolve_asset_or_fail(ctx, asset)
    node_map = ctx.session.nodes_for(path)
    guids = []
    for ref in as_list(nodes):
        try:
            guids.append(ctx.session.node_ref_to_guid(path, ref))
        except KeyError as exc:
            raise ToolError(str(exc))
    params = {"asset": path, "nodes": guids}
    if target_graph:
        params["target_graph"] = target_graph
    if dy is not None:
        params["dy"] = int(dy)
    result = ctx.client.call("graph.clone_nodes", params)
    graph = result.get("graph", "")
    lines = []
    for node in result.get("nodes", []):
        nid = node_map.id_for(node["guid"], graph, node.get("kind", ""))
        lines.append(f"+ {nid} {node_label(node)} [{graph}]")
    lines.append(f"  internal links kept: {result.get('internal_links', 0)}")
    return finish_edit(ctx, "clone_nodes", [path], lines, compile_now=compile)


@tool("add_local_variable", "Add a local variable to a function graph.",
      schema({"asset": P("string", "A# / path / name."), "graph": P("string", "Function graph name."), "name": P("string", "Variable name."),
              "type": P("string", "Type spec."), "default": P("string", "Default value.")}, ["asset", "graph", "name", "type"]), mutating=True)
def add_local_variable(ctx, asset, graph, name, type, default=None):
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path, "graph": graph_param(ctx, path, graph), "name": name, "type": type}
    if default is not None:
        params["default"] = default
    result = ctx.client.call("graph.local_variable", params)
    return finish_edit(ctx, "add_local_variable", [path], [f"+ Local {result.get('variable')} : {result.get('type')} in {result.get('graph')}"])
