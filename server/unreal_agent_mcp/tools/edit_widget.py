"""Widget Blueprint edits: clone (style-preserving), add, remove, move, set, copy style, bind events."""
from . import P, schema, tool
from .common import as_dict, as_list, finish_edit, resolve_asset_or_fail
from ..formatters import node_label
from ..protocol import ToolError


@tool("clone_widget",
      "Deep-clone a widget subtree with its exact style/slot settings and insert it (insert_after / insert_before / parent+index). "
      "children={SourceChildName: {Text: 'Credits'}} overrides child properties; child names get the new suffix automatically (TXT_Settings -> TXT_Credits).",
      schema({
          "asset": P("string", "Widget Blueprint (A# / path / name)."),
          "source": P("string", "Widget to clone, e.g. BTN_Settings."),
          "new_name": P("string", "Name of the clone, e.g. BTN_Credits."),
          "insert_after": P("string", "Sibling to insert after."),
          "insert_before": P("string", "Sibling to insert before."),
          "parent": P("string", "Parent panel (with index)."),
          "index": P("integer", "Index under parent."),
          "properties": P("object", "Property overrides on the clone root."),
          "children": P("object", "Per-child overrides keyed by SOURCE child name; supports 'rename'."),
          "rename_children": P("boolean", "Rename children by suffix (default true)."),
          "compile": P("boolean", "Compile after the edit."),
      }, ["asset", "source", "new_name"]), mutating=True)
def clone_widget(ctx, asset, source, new_name, insert_after=None, insert_before=None, parent=None, index=None, properties=None, children=None, rename_children=None, compile=None):
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path, "source": source, "new_name": new_name}
    for key, value in (("insert_after", insert_after), ("insert_before", insert_before), ("parent", parent), ("index", index),
                       ("properties", as_dict(properties)), ("children", as_dict(children)), ("rename_children", rename_children)):
        if value is not None:
            params[key] = value
    result = ctx.client.call("widget.clone", params)
    lines = [f"+ Widget {result.get('widget')} ({result.get('class')}) in {result.get('parent')}[{result.get('index')}] cloned from {source}"]
    for entry in result.get("tree", [])[1:]:
        lines.append("  " + entry)
    if result.get("changed"):
        lines.append("  set " + ", ".join(result["changed"]))
    for failed in result.get("failed", []):
        lines.append("  ! " + failed)
    return finish_edit(ctx, "clone_widget", [path], lines, compile_now=compile)


@tool("add_widget", "Add a new widget (Button, TextBlock, Image, VerticalBox, WBP_Foo...) under a parent. Prefer clone_widget to match existing style.",
      schema({
          "asset": P("string", "Widget Blueprint."),
          "class": P("string", "Widget class: Button, TextBlock, Image, Border, SizeBox, VerticalBox, HorizontalBox, Overlay, CanvasPanel, or a WBP name/path."),
          "name": P("string", "Widget name."),
          "parent": P("string", "Parent panel name."),
          "index": P("integer", "Index under parent."),
          "insert_after": P("string", "Sibling to insert after."),
          "insert_before": P("string", "Sibling to insert before."),
          "properties": P("object", "Widget properties {path: value}."),
          "slot": P("object", "Slot properties {Padding: '(Top=8)', HorizontalAlignment: 'HAlign_Fill'}."),
          "var": P("boolean", "Is variable (default true for non-panels)."),
          "compile": P("boolean", "Compile after the edit."),
      }, ["asset", "class", "name"]), mutating=True)
def add_widget(ctx, asset, name, **kwargs):
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path, "class": kwargs["class"], "name": name}
    for key in ("parent", "index", "insert_after", "insert_before", "var"):
        if kwargs.get(key) is not None:
            params[key] = kwargs[key]
    for key in ("properties", "slot"):
        value = as_dict(kwargs.get(key))
        if value:
            params[key] = value
    result = ctx.client.call("widget.add", params)
    lines = [f"+ Widget {result.get('widget')} ({result.get('class')})" + (f" in {result.get('parent')}[{result.get('index')}]" if result.get("parent") else " as root")]
    if result.get("changed"):
        lines.append("  set " + ", ".join(result["changed"]))
    for failed in result.get("failed", []):
        lines.append("  ! " + failed)
    return finish_edit(ctx, "add_widget", [path], lines, compile_now=kwargs.get("compile"))


@tool("remove_widget", "Remove widgets and their subtrees.",
      schema({"asset": P("string", "Widget Blueprint."), "widgets": P("array", "Widget names.", items={"type": "string"}), "compile": P("boolean", "Compile after.")},
             ["asset", "widgets"]), mutating=True)
def remove_widget(ctx, asset, widgets, compile=None):
    path = resolve_asset_or_fail(ctx, asset)
    result = ctx.client.call("widget.remove", {"asset": path, "widgets": as_list(widgets)})
    return finish_edit(ctx, "remove_widget", [path], ["- Widget " + ", ".join(result.get("removed", []))], compile_now=compile)


@tool("move_widget", "Reparent or reorder a widget (parent+index, insert_after or insert_before).",
      schema({"asset": P("string", "Widget Blueprint."), "widget": P("string", "Widget name."), "parent": P("string", "New parent panel."),
              "index": P("integer", "Index under parent."), "insert_after": P("string", "Sibling."), "insert_before": P("string", "Sibling."),
              "compile": P("boolean", "Compile after.")}, ["asset", "widget"]), mutating=True)
def move_widget(ctx, asset, widget, parent=None, index=None, insert_after=None, insert_before=None, compile=None):
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path, "widget": widget}
    for key, value in (("parent", parent), ("index", index), ("insert_after", insert_after), ("insert_before", insert_before)):
        if value is not None:
            params[key] = value
    result = ctx.client.call("widget.move", params)
    return finish_edit(ctx, "move_widget", [path], [f"~ Widget {widget} -> {result.get('parent')}[{result.get('index')}]"], compile_now=compile)


@tool("set_widget", "Set widget properties / slot properties, rename, or toggle the variable flag. Paths like 'WidgetStyle.Normal.TintColor' or 'Font.Size' work.",
      schema({
          "asset": P("string", "Widget Blueprint."),
          "widget": P("string", "Widget name."),
          "properties": P("object", "{path: value}. Text: 'Credits'; colors '(R=1,G=1,B=1,A=1)'; enums by name; assets by /Game path."),
          "slot": P("object", "Slot properties {Padding: '(Left=0,Top=8,Right=0,Bottom=8)'}."),
          "rename": P("string", "New widget name (updates variable references and bound events)."),
          "var": P("boolean", "Expose as variable."),
          "compile": P("boolean", "Compile after."),
      }, ["asset", "widget"]), mutating=True)
def set_widget(ctx, asset, widget, properties=None, slot=None, rename=None, var=None, compile=None):
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path, "widget": widget}
    props = as_dict(properties)
    if props:
        params["properties"] = props
    slot_props = as_dict(slot)
    if slot_props:
        params["slot"] = slot_props
    if rename:
        params["rename"] = rename
    if var is not None:
        params["var"] = bool(var)
    if len(params) == 2:
        raise ToolError("Nothing to set (properties/slot/rename/var).")
    result = ctx.client.call("widget.set", params)
    lines = [f"~ Widget {result.get('widget', widget)}: " + ", ".join(result.get("changed", []) or ["(no change)"])]
    for failed in result.get("failed", []):
        lines.append("  ! " + failed)
    return finish_edit(ctx, "set_widget", [path], lines, compile_now=compile)


@tool("copy_widget_style", "Copy style (non-content properties + slot settings) from one widget to others of the same class, recursively for matching children.",
      schema({"asset": P("string", "Widget Blueprint."), "source": P("string", "Source widget."), "targets": P("array", "Target widgets.", items={"type": "string"}),
              "slot": P("boolean", "Include slot settings (default true)."), "recursive": P("boolean", "Apply to matching children (default true).")},
             ["asset", "source", "targets"]), mutating=True)
def copy_widget_style(ctx, asset, source, targets, slot=None, recursive=None):
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path, "source": source, "targets": as_list(targets)}
    if slot is not None:
        params["slot"] = bool(slot)
    if recursive is not None:
        params["recursive"] = bool(recursive)
    result = ctx.client.call("widget.copy_style", params)
    lines = [f"~ style {source} -> " + ", ".join(result.get("applied", []))]
    for skipped in result.get("skipped", []):
        lines.append("  ! " + skipped)
    return finish_edit(ctx, "copy_widget_style", [path], lines)


@tool("bind_widget_event", "Create the bound event node for a widget delegate (OnClicked, OnHovered, OnTextChanged...) in the EventGraph; "
      "optionally call an existing function or create+call a new one (create_function=true).",
      schema({
          "asset": P("string", "Widget Blueprint."),
          "widget": P("string", "Widget name, e.g. BTN_Credits."),
          "event": P("string", "Delegate name, e.g. OnClicked."),
          "function": P("string", "Function to call from the event."),
          "create_function": P("boolean", "Create the function if missing."),
          "force": P("boolean", "Insert the call even if the event already has an exec target."),
          "compile": P("boolean", "Compile after (default config)."),
      }, ["asset", "widget", "event"]), mutating=True)
def bind_widget_event(ctx, asset, widget, event, function=None, create_function=None, force=None, compile=None):
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path, "widget": widget, "event": event}
    if function:
        params["function"] = function
    if create_function:
        params["create_function"] = True
    if force:
        params["force"] = True
    result = ctx.client.call("widget.bind_event", params)
    node_map = ctx.session.nodes_for(path)
    graph = result.get("graph", "EventGraph")
    nid = node_map.id_for(result["node"], graph, "ComponentBoundEvent")
    lines = [f"{'+' if result.get('created') else '='} {nid} Event {widget}.{event} [{graph}]" + ("" if result.get("created") else " (already existed)")]
    if result.get("function_created"):
        lines.append(f"+ Function {result['function_created']}()")
    if result.get("call_node"):
        cid = node_map.id_for(result["call_node"], graph, "CallFunction")
        lines.append(f"+ {cid} {result.get('function')} (self)")
        lines.append(f"  {nid} -> {cid}")
    if result.get("function_error"):
        lines.append("  ! " + result["function_error"])
    return finish_edit(ctx, "bind_widget_event", [path], lines, compile_now=compile)
