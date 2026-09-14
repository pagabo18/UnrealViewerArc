"""Progressive-disclosure inspection tools."""
import json

from . import P, schema, tool
from .common import graph_param, resolve_asset_or_fail
from ..formatters import (format_animations, format_components, format_found_nodes, format_graph, format_structure,
                          format_summary, format_widget, format_widget_tree)
from ..protocol import ToolError


def _raw(data) -> str:
    return json.dumps(data, ensure_ascii=False, separators=(",", ":"))


@tool("inspect_blueprint",
      "Inspect a Blueprint at a detail level. summary (<300 tokens): counts/parent/interfaces/refs. "
      "structure: functions with signatures, variables, events, components. components: components with changed properties.",
      schema({
          "asset": P("string", "A# id, package path (/Game/...) or unique asset name."),
          "detail": P("string", "summary (default) | structure | components"),
          "format": P("string", "compact (default) | raw (plugin JSON, debugging only)"),
      }, ["asset"]))
def inspect_blueprint(ctx, asset, detail=None, format=None):
    ctx.sync()
    path = resolve_asset_or_fail(ctx, asset)
    detail = (detail or "summary").lower()
    if detail == "summary":
        data = ctx.cached(path, "summary", "blueprint.summary", {"asset": path})
        return _raw(data) if format == "raw" else format_summary(data, ctx)
    if detail == "structure":
        data = ctx.cached(path, "structure", "blueprint.structure", {"asset": path})
        return _raw(data) if format == "raw" else format_structure(data, ctx)
    if detail == "components":
        data = ctx.cached(path, "components", "blueprint.components", {"asset": path, "properties": True})
        return _raw(data) if format == "raw" else format_components(data, ctx)
    raise ToolError("detail must be summary | structure | components")


@tool("inspect_graph",
      "Nodes of one graph in compact form (N# ids). Use graph='EventGraph' or a function name. For big graphs pass around=N# + depth "
      "(neighbourhood only) or query= to filter; results are paginated (offset). around with depth=0 shows one node with all pins.",
      schema({
          "asset": P("string", "A# id / path / name."),
          "graph": P("string", "Graph name (default EventGraph)."),
          "around": P("string", "Center node id (N#) for a neighbourhood window."),
          "depth": P("integer", "Hops from the center node (default 1)."),
          "query": P("string", "Only nodes whose kind/title/member/comment contains this."),
          "kind": P("string", "Only nodes of this kind (CallFunction, VariableSet, Event, Branch, ...)."),
          "offset": P("integer", "Pagination offset."),
          "limit": P("integer", "Max nodes (default 60)."),
          "format": P("string", "compact (default) | raw"),
      }, ["asset"]))
def inspect_graph(ctx, asset, graph=None, around=None, depth=None, query=None, kind=None, offset=None, limit=None, format=None):
    ctx.sync()
    path = resolve_asset_or_fail(ctx, asset)
    graph = graph_param(ctx, path, graph) or (ctx.session.nodes_for(path).graph_of.get(around.upper(), "") if around else "") or "EventGraph"
    params = {"asset": path, "graph": graph, "offset": int(offset or 0), "limit": int(limit or 60), "pins": True, "defaults": True}
    if around:
        try:
            params["around"] = ctx.session.node_ref_to_guid(path, around)
        except KeyError as exc:
            raise ToolError(str(exc))
        params["depth"] = int(depth if depth is not None else 1)
    if query:
        params["query"] = query
    if kind:
        params["kind"] = kind
    data = ctx.client.call("graph.inspect", params)
    if format == "raw":
        return _raw(data)
    detail_pins = bool(around) and int(depth or 0) == 0
    return format_graph(data, ctx, path, detail_pins=detail_pins)


@tool("find_nodes", "Search nodes across all graphs of a Blueprint by text and/or kind (e.g. query='Damage', kind='VariableSet').",
      schema({
          "asset": P("string", "A# id / path / name."),
          "query": P("string", "Substring matched against kind/title/member/comment."),
          "kind": P("string", "Node kind filter."),
          "graph": P("string", "Restrict to one graph."),
          "limit": P("integer", "Max results (default 30)."),
      }, ["asset"]))
def find_nodes(ctx, asset, query=None, kind=None, graph=None, limit=None):
    ctx.sync()
    path = resolve_asset_or_fail(ctx, asset)
    if not query and not kind:
        raise ToolError("Provide query and/or kind.")
    params = {"asset": path, "limit": int(limit or 30), "pins": False}
    if query:
        params["query"] = query
    if kind:
        params["kind"] = kind
    if graph:
        params["graph"] = graph_param(ctx, path, graph)
    data = ctx.client.call("graph.find_nodes", params)
    return format_found_nodes(data, ctx, path)


@tool("inspect_widget_tree", "Widget hierarchy of a Widget Blueprint with style fingerprints (S#) so repeated styles are listed once.",
      schema({
          "asset": P("string", "A# id / path / name of the Widget Blueprint."),
          "root": P("string", "Only the subtree under this widget."),
          "styles": P("boolean", "Include style fingerprints (default true)."),
          "format": P("string", "compact (default) | raw"),
      }, ["asset"]))
def inspect_widget_tree(ctx, asset, root=None, styles=None, format=None):
    ctx.sync()
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path, "styles": True if styles is None else bool(styles)}
    if root:
        params["root"] = root
    key = f"tree:{root or ''}:{params['styles']}"
    data = ctx.cached(path, key, "widget.tree", params)
    return _raw(data) if format == "raw" else format_widget_tree(data, ctx)


@tool("inspect_widget", "One widget's non-default properties, slot settings, children, available/bound events.",
      schema({
          "asset": P("string", "Widget Blueprint (A# / path / name)."),
          "widget": P("string", "Widget name, e.g. BTN_Settings."),
          "all": P("boolean", "Include properties equal to class defaults (verbose)."),
          "format": P("string", "compact (default) | raw"),
      }, ["asset", "widget"]))
def inspect_widget(ctx, asset, widget, all=None, format=None):
    ctx.sync()
    path = resolve_asset_or_fail(ctx, asset)
    data = ctx.client.call("widget.inspect", {"asset": path, "widget": widget, "all": bool(all)})
    return _raw(data) if format == "raw" else format_widget(data, ctx)


@tool("inspect_animations", "List widget animations with bound widgets/tracks (read-only).",
      schema({"asset": P("string", "Widget Blueprint."), "animation": P("string", "Only this animation.")}, ["asset"]))
def inspect_animations(ctx, asset, animation=None):
    ctx.sync()
    path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": path}
    if animation:
        params["animation"] = animation
    return format_animations(ctx.client.call("widget.animations", params), ctx)
