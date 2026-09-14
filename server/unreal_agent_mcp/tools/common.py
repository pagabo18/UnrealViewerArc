"""Shared helpers for tool handlers."""
from typing import Any, Dict, List, Optional

from ..formatters import format_changeset, format_compile
from ..protocol import ToolError


def as_list(value) -> List[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [v.strip() for v in value.split(",") if v.strip()] if "," in value else [value]
    return [str(v) for v in value]


def as_dict(value) -> Optional[Dict[str, Any]]:
    if value is None:
        return None
    if isinstance(value, dict):
        return value
    raise ToolError(f"Expected an object, got {type(value).__name__}.")


def resolve_asset_or_fail(ctx, asset: str) -> str:
    try:
        return ctx.resolve_asset(asset)
    except ValueError as exc:
        raise ToolError(str(exc))


def finish_edit(ctx, tool: str, assets: List[str], body_lines: List[str], compile_now: Optional[bool] = None, save: Optional[bool] = None) -> str:
    """Records the changeset, optionally compiles/saves, and renders the diff."""
    cs_id = ctx.session.record_change(tool, assets, body_lines)
    cs = ctx.session.changesets[-1]
    text = format_changeset(cs, ctx, body_lines)
    do_compile = ctx.config.auto_compile if compile_now is None else compile_now
    do_save = ctx.config.auto_save if save is None else save
    if do_compile or do_save:
        result = ctx.client.call("blueprint.compile", {"assets": assets, "save": do_save, "validate": False})
        cs["compile"] = result
        text += "\n" + format_compile(result, ctx)
    ctx.index.apply_changes([{"asset": a, "type": "modified"} for a in assets])
    return text


def graph_param(ctx, path: str, graph: Optional[str]) -> str:
    """Accepts graph names; node ids (N#) also imply their graph."""
    if not graph:
        return ""
    node_map = ctx.session.nodes_for(path)
    if graph.upper() in node_map.graph_of:
        return node_map.graph_of[graph.upper()]
    return graph


def pin_refs(ctx, path: str, refs) -> List[str]:
    out = []
    for ref in as_list(refs):
        try:
            out.append(ctx.session.pin_ref_to_plugin(path, ref))
        except KeyError as exc:
            raise ToolError(str(exc))
    return out
