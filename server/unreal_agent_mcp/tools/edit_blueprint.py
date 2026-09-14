"""Blueprint-level edits: variables, functions, interfaces, components, creation."""
from . import P, schema, tool
from .common import as_dict, as_list, finish_edit, resolve_asset_or_fail
from ..formatters import _signature, short


@tool("blueprint_variable",
      "Add/remove/rename/modify a member variable. Types: float,int,bool,string,name,text,Vector,Actor,class<Actor>,[float],{string:int},EMyEnum,soft<Texture2D>.",
      schema({
          "asset": P("string", "A# / path / name."),
          "op": P("string", "add | remove | rename | modify"),
          "name": P("string", "Variable name."),
          "type": P("string", "Type spec (add/modify)."),
          "default": P("string", "Default value as text."),
          "new_name": P("string", "For rename."),
          "category": P("string", "Category."),
          "tooltip": P("string", "Tooltip."),
          "replicated": P("boolean", "Replicate the variable."),
          "rep_notify": P("string", "RepNotify function name (implies replicated)."),
          "instance_editable": P("boolean", "Editable per instance."),
          "read_only": P("boolean", "Blueprint read-only."),
          "expose_on_spawn": P("boolean", "Expose on spawn."),
          "private": P("boolean", "Private."),
          "compile": P("boolean", "Compile after the edit (default: config autoCompile)."),
      }, ["asset", "op", "name"]), mutating=True)
def blueprint_variable(ctx, asset, op, name, type=None, default=None, new_name=None, category=None, tooltip=None, replicated=None,
                       rep_notify=None, instance_editable=None, read_only=None, expose_on_spawn=None, private=None, compile=None):
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path, "op": op, "name": name}
    for key, value in (("type", type), ("default", default), ("new_name", new_name), ("category", category), ("tooltip", tooltip),
                       ("replicated", replicated), ("rep_notify", rep_notify), ("instance_editable", instance_editable),
                       ("read_only", read_only), ("expose_on_spawn", expose_on_spawn), ("private", private)):
        if value is not None:
            params[key] = value
    result = ctx.client.call("blueprint.variable", params)
    sign = {"add": "+", "remove": "-", "delete": "-", "rename": "~", "modify": "~"}.get(op.lower(), "~")
    line = f"{sign} Variable {result.get('variable', name)}"
    if result.get("type"):
        line += f" : {result['type']}"
    if default is not None and op.lower() == "add":
        line += f" = {default}"
    if result.get("changed"):
        line += " (" + ", ".join(result["changed"]) + ")"
    return finish_edit(ctx, "blueprint_variable", [path], [line], compile_now=compile)


@tool("blueprint_function",
      "Create/delete/rename/modify a function. inputs/outputs as 'Name:type'. create also overrides parent functions/events of the same name.",
      schema({
          "asset": P("string", "A# / path / name."),
          "op": P("string", "create | delete | rename | modify"),
          "name": P("string", "Function name."),
          "inputs": P("array", "Pins to add, e.g. ['Amount:float'].", items={"type": "string"}),
          "outputs": P("array", "Return pins to add, e.g. ['Success:bool'].", items={"type": "string"}),
          "remove_pins": P("array", "Pin names to remove.", items={"type": "string"}),
          "pure": P("boolean", "Pure function."),
          "category": P("string", "Category."),
          "new_name": P("string", "For rename."),
          "access": P("string", "public | protected | private"),
          "compile": P("boolean", "Compile after the edit."),
      }, ["asset", "op", "name"]), mutating=True)
def blueprint_function(ctx, asset, op, name, inputs=None, outputs=None, remove_pins=None, pure=None, category=None, new_name=None, access=None, compile=None):
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path, "op": op, "name": name}
    for key, value in (("inputs", as_list(inputs) or None), ("outputs", as_list(outputs) or None), ("remove_pins", as_list(remove_pins) or None),
                       ("pure", pure), ("category", category), ("new_name", new_name), ("access", access)):
        if value is not None:
            params[key] = value
    result = ctx.client.call("blueprint.function", params)
    sign = {"create": "+", "delete": "-", "remove": "-"}.get(op.lower(), "~")
    lines = []
    if result.get("event_node"):
        node_map = ctx.session.nodes_for(path)
        nid = node_map.id_for(result["event_node"], result.get("graph", "EventGraph"), "Event")
        lines.append(f"+ {nid} Event {name} (override) [{result.get('graph')}]")
    else:
        sig = result.get("signature")
        lines.append(f"{sign} Function " + (_signature({"name": result.get("function", name), **sig}) if sig else result.get("function", name)))
        if result.get("entry_node"):
            node_map = ctx.session.nodes_for(path)
            nid = node_map.id_for(result["entry_node"], result.get("graph", name), "FunctionEntry")
            lines.append(f"  entry {nid} (graph {result.get('graph', name)})")
        if result.get("changed"):
            lines.append("  " + ", ".join(result["changed"]))
    return finish_edit(ctx, "blueprint_function", [path], lines, compile_now=compile)


@tool("blueprint_interface", "Add or remove an implemented Blueprint interface.",
      schema({"asset": P("string", "A# / path / name."), "op": P("string", "add | remove"), "interface": P("string", "Interface class (BPI_Foo, /Game/..., or native name).")},
             ["asset", "op", "interface"]), mutating=True)
def blueprint_interface(ctx, asset, op, interface):
    path = resolve_asset_or_fail(ctx, asset)
    result = ctx.client.call("blueprint.interface", {"asset": path, "op": op, "interface": interface})
    sign = "+" if op.lower() == "add" else "-"
    return finish_edit(ctx, "blueprint_interface", [path], [f"{sign} Interface {result.get('interface', interface)}"])


@tool("blueprint_component", "Add/remove/modify a component of an Actor Blueprint (class, parent attachment, properties).",
      schema({
          "asset": P("string", "A# / path / name."),
          "op": P("string", "add | remove | modify"),
          "name": P("string", "Component (variable) name."),
          "class": P("string", "Component class for add, e.g. StaticMeshComponent."),
          "parent": P("string", "Attach parent component name."),
          "properties": P("object", "Property path -> value, e.g. {'RelativeLocation': '(X=0,Y=0,Z=50)'}."),
          "compile": P("boolean", "Compile after the edit."),
      }, ["asset", "op", "name"]), mutating=True)
def blueprint_component(ctx, asset, op, name, **kwargs):
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path, "op": op, "name": name}
    if kwargs.get("class"):
        params["class"] = kwargs["class"]
    if kwargs.get("parent") is not None:
        params["parent"] = kwargs["parent"]
    props = as_dict(kwargs.get("properties"))
    if props:
        params["properties"] = props
    result = ctx.client.call("blueprint.component", params)
    sign = {"add": "+", "remove": "-"}.get(op.lower(), "~")
    lines = [f"{sign} Component {name}" + (f" : {kwargs['class']}" if kwargs.get("class") else "") + (f" < {kwargs['parent']}" if kwargs.get("parent") else "")]
    if result.get("changed"):
        lines.append("  set " + ", ".join(result["changed"]))
    for failed in result.get("failed", []):
        lines.append("  ! " + failed)
    return finish_edit(ctx, "blueprint_component", [path], lines, compile_now=kwargs.get("compile"))


@tool("create_blueprint", "Create a new Blueprint or Widget Blueprint asset. Prefer reusing existing assets (search first).",
      schema({
          "path": P("string", "Package path, e.g. /Game/UI/WBP_Credits."),
          "parent": P("string", "Parent class: Actor, ActorComponent, UserWidget, a BP name or /Game path (default Actor)."),
          "kind": P("string", "auto | blueprint | widget"),
          "root": P("string", "Root panel class for widgets (default CanvasPanel)."),
      }, ["path"]), mutating=True)
def create_blueprint(ctx, path, parent=None, kind=None, root=None):
    params = {"path": path}
    if parent:
        params["parent"] = parent
    if kind:
        params["kind"] = kind
    if root:
        params["root"] = root
    result = ctx.client.call("assets.create_blueprint", params)
    created = result.get("asset", path)
    ctx.index.last_refresh = 0.0
    ctx.session.add_to_working_set([created])
    return finish_edit(ctx, "create_blueprint", [created], [f"+ {'Widget ' if result.get('widget') else ''}Blueprint {short(created)} < {result.get('parent')}"], compile_now=False)
