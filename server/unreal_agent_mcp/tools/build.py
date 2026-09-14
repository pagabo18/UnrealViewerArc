"""Compile / save / validate / undo."""
from . import P, schema, tool
from .common import as_list
from ..formatters import format_compile, format_save, format_validate
from ..protocol import ToolError


def _targets(ctx, assets):
    refs = as_list(assets)
    if not refs:
        ws = ctx.session.current_ws
        if not ws:
            raise ToolError("No assets given and no working set.")
        return list(ctx.session.working_sets[ws])
    try:
        return ctx.resolve_assets(refs)
    except ValueError as exc:
        raise ToolError(str(exc))


@tool("compile_blueprint", "Compile Blueprints (A# ids, paths, names or a WS# working set). Optionally validate and save. Returns errors with N# node ids.",
      schema({
          "assets": P("array", "Assets or WS#. Default: current working set.", items={"type": "string"}),
          "save": P("boolean", "Save after a successful compile."),
          "validate": P("boolean", "Run DataValidation."),
      }), mutating=True)
def compile_blueprint(ctx, assets=None, save=None, validate=None):
    paths = _targets(ctx, assets)
    result = ctx.client.call("blueprint.compile", {"assets": paths, "save": bool(save), "validate": bool(validate)})
    for path in paths:
        ctx.session.invalidate(path)
    ctx.index.apply_changes([{"asset": p, "type": "modified"} for p in paths])
    return format_compile(result, ctx)


@tool("save_assets", "Save assets through the editor pipeline (source-control aware).",
      schema({"assets": P("array", "Assets or WS#. Default: current working set.", items={"type": "string"}),
              "only_if_dirty": P("boolean", "Skip clean assets (default true).")}), mutating=True)
def save_assets(ctx, assets=None, only_if_dirty=None):
    paths = _targets(ctx, assets)
    params = {"assets": paths}
    if only_if_dirty is not None:
        params["only_if_dirty"] = bool(only_if_dirty)
    return format_save(ctx.client.call("blueprint.save", params), ctx)


@tool("validate_assets", "Run the editor's DataValidation on assets.",
      schema({"assets": P("array", "Assets or WS#. Default: current working set.", items={"type": "string"})}))
def validate_assets(ctx, assets=None):
    paths = _targets(ctx, assets)
    return format_validate(ctx.client.call("blueprint.validate", {"assets": paths}), ctx)


@tool("undo", "Undo the last N editor transactions (every edit tool is one transaction).",
      schema({"steps": P("integer", "How many (default 1).")}), mutating=True)
def undo(ctx, steps=None):
    result = ctx.client.call("system.undo", {"steps": int(steps or 1)})
    ctx.session.cache.clear()
    ctx.index.last_refresh = 0.0
    lines = [f"Undone {result.get('undone', 0)}: " + ", ".join(result.get("titles", []))]
    if result.get("undoStack"):
        lines.append("next undo: " + result["undoStack"][0])
    return "\n".join(lines)


@tool("redo", "Redo N undone transactions.", schema({"steps": P("integer", "How many (default 1).")}), mutating=True)
def redo(ctx, steps=None):
    result = ctx.client.call("system.redo", {"steps": int(steps or 1)})
    ctx.session.cache.clear()
    return f"Redone {result.get('redone', 0)}"


@tool("reload_asset", "Reload an asset from disk, discarding unsaved in-memory changes.",
      schema({"asset": P("string", "A# / path / name.")}, ["asset"]), mutating=True)
def reload_asset(ctx, asset):
    try:
        path = ctx.resolve_asset(asset)
    except ValueError as exc:
        raise ToolError(str(exc))
    result = ctx.client.call("blueprint.reload", {"asset": path})
    ctx.session.invalidate(path)
    ctx.session.nodes.pop(path, None)
    return f"{ctx.label(path)}: " + ("reloaded" if result.get("ok") else "reload failed")
