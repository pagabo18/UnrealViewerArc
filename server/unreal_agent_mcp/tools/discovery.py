"""search / xref / index / working set tools."""
from typing import Optional

from . import P, schema, tool
from .common import as_list, resolve_asset_or_fail
from ..bridge import PluginError, PluginUnavailable
from ..formatters import format_search, format_xref, short
from ..protocol import ToolError


def _refresh(ctx, force: bool = False):
    try:
        ctx.sync()
        return ctx.index.refresh_shallow(force=force)
    except PluginUnavailable:
        if not ctx.index.assets:
            raise
        return {"offline": True}


@tool("search",
      "Find Blueprints, Widget Blueprints, functions, variables, widgets or events by name. Returns short asset ids (A#). "
      "Start here instead of inspecting assets blindly.",
      schema({
          "query": P("string", "Name fragment, e.g. 'Inventory', 'AddItem', 'BTN_Settings'."),
          "kind": P("string", "asset | function | variable | widget | event | component | any (default any)."),
          "limit": P("integer", "Max results (default 20)."),
          "cursor": P("string", "next_cursor from a previous call to page further."),
          "path": P("string", "Restrict to a content folder, e.g. /Game/UI."),
      }, ["query"]), needs_editor=False)
def search(ctx, query, kind=None, limit=None, cursor=None, path=None):
    limit = int(limit or ctx.config.max_results)
    if cursor:
        entry = ctx.session.take_cursor(cursor)
        if not entry:
            raise ToolError(f"Unknown cursor {cursor}.")
        _kind, items, offset, page = entry
        chunk = items[offset:offset + page]
        next_cursor = ctx.session.make_cursor("search", items, offset + page, page) if offset + page < len(items) else None
        return format_search(query, chunk, ctx, ctx.index.deep_coverage(), next_cursor, len(items))
    _refresh(ctx)
    kind = (kind or "any").lower()
    if kind != "asset" and ctx.config.auto_deep_index:
        # Deep-index assets whose names look related first (bounded), so function/variable hits are found.
        try:
            ctx.index.ensure_deep(ctx.index.candidates_for(query))
        except PluginUnavailable:
            pass
    results = ctx.index.search(query, kind, limit=0, paths_filter=path)
    chunk = results[:limit]
    next_cursor = ctx.session.make_cursor("search", results, limit, limit) if len(results) > limit else None
    return format_search(query, chunk, ctx, ctx.index.deep_coverage(), next_cursor, len(results))


@tool("who_calls", "Blueprints that call a function (by name, or Class.Function).",
      schema({"function": P("string", "Function name, e.g. StartCombat or BP_Inventory.AddItem."), "limit": P("integer", "Max rows (default 30).")}, ["function"]), needs_editor=False)
def who_calls(ctx, function, limit=None):
    _refresh(ctx)
    _ensure_all_deep(ctx)
    return format_xref("who_calls", function, ctx.index.who_calls(function), ctx, ctx.index.deep_coverage(), int(limit or 30))


@tool("who_reads", "Blueprints that read (Get) a variable.",
      schema({"variable": P("string", "Variable name, e.g. CurrentHealth."), "limit": P("integer", "Max rows (default 30).")}, ["variable"]), needs_editor=False)
def who_reads(ctx, variable, limit=None):
    _refresh(ctx)
    _ensure_all_deep(ctx)
    return format_xref("who_reads", variable, ctx.index.who_accesses(variable, "reads"), ctx, ctx.index.deep_coverage(), int(limit or 30))


@tool("who_writes", "Blueprints that write (Set) a variable.",
      schema({"variable": P("string", "Variable name."), "limit": P("integer", "Max rows (default 30).")}, ["variable"]), needs_editor=False)
def who_writes(ctx, variable, limit=None):
    _refresh(ctx)
    _ensure_all_deep(ctx)
    return format_xref("who_writes", variable, ctx.index.who_accesses(variable, "writes"), ctx, ctx.index.deep_coverage(), int(limit or 30))


@tool("find_references", "Assets that reference an asset (Asset Registry referencers + dependencies).",
      schema({"asset": P("string", "A# id, package path or unique name.")}, ["asset"]))
def find_references(ctx, asset):
    path = resolve_asset_or_fail(ctx, asset)
    info = ctx.client.call("assets.info", {"asset": path})
    lines = [f"{ctx.label(path)} ({info.get('class')})"]
    refs = info.get("referencers", [])
    deps = [d for d in info.get("dependencies", []) if not d.startswith("/Engine")]
    lines.append(f"referenced by ({len(refs)}): " + ", ".join(f"{ctx.aid(p)} {short(p)}" for p in refs[:30]))
    lines.append(f"depends on ({len(deps)}): " + ", ".join(f"{ctx.aid(p)} {short(p)}" for p in deps[:30]))
    return "\n".join(lines)


def _ensure_all_deep(ctx):
    pending = [p for p, e in ctx.index.assets.items() if not e.get("deep")]
    if pending and ctx.config.auto_deep_index:
        try:
            ctx.index.deep_index(pending, max_count=ctx.config.max_auto_deep_index)
        except PluginUnavailable:
            pass


@tool("index_project", "Build/refresh the project index. deep=true also indexes functions/variables/calls of every Blueprint (needed for who_calls/who_reads coverage).",
      schema({
          "deep": P("boolean", "Deep-index all Blueprints (loads them once; incremental afterwards)."),
          "paths": P("array", "Limit deep indexing to these package paths / A# ids.", items={"type": "string"}),
          "force": P("boolean", "Rebuild shallow index even if fresh."),
      }))
def index_project(ctx, deep=None, paths=None, force=None):
    stats = _refresh(ctx, force=bool(force))
    lines = [f"Index: {len(ctx.index.assets)} blueprints (changed {stats.get('changed', 0)}, removed {stats.get('removed', 0)})"]
    if deep or paths:
        targets = [resolve_asset_or_fail(ctx, p) for p in as_list(paths)] if paths else list(ctx.index.assets.keys())
        result = ctx.index.deep_index(targets, max_count=None)
        lines.append(f"deep-indexed {result['indexed']} (failed {result['failed']})")
    cov = ctx.index.deep_coverage()
    lines.append(f"coverage: {cov['deep']}/{cov['total']} deep")
    conventions = ctx.index.conventions(recompute=True)
    if conventions.get("prefixes"):
        lines.append("conventions: " + " ".join(f"{k}={v}" for k, v in list(conventions["prefixes"].items())[:10]))
    return "\n".join(lines)


@tool("working_set", "Create/extend/show the working set (WS#) of assets for the current task. compile_blueprint accepts WS ids.",
      schema({
          "assets": P("array", "A# ids, paths or names to add.", items={"type": "string"}),
          "op": P("string", "create | add | show | clear (default: create if assets given, else show)."),
          "ws": P("string", "Working set id to modify (default current)."),
      }), needs_editor=False)
def working_set(ctx, assets=None, op=None, ws=None):
    op = (op or ("create" if assets else "show")).lower()
    if op == "clear":
        ctx.session.working_sets.clear()
        ctx.session.current_ws = None
        return "Working sets cleared."
    if op in ("create", "add"):
        paths = [resolve_asset_or_fail(ctx, a) for a in as_list(assets)]
        if not paths:
            raise ToolError("assets required.")
        ws_id = ctx.session.create_working_set(paths) if op == "create" else ctx.session.add_to_working_set(paths, ws)
    else:
        ws_id = (ws or ctx.session.current_ws or "").upper()
    if not ws_id or ws_id not in ctx.session.working_sets:
        return "No working set. Use working_set(assets=[...])."
    lines = [f"{ws_id}" + (" (current)" if ws_id == ctx.session.current_ws else "")]
    for path in ctx.session.working_sets[ws_id]:
        lines.append(f"{ctx.label(path)}")
    return "\n".join(lines)


@tool("session_info", "Session memory: working sets, id counts, changesets, project, editor status.", schema({}), needs_editor=False)
def session_info(ctx):
    summary = ctx.session.summary()
    lines = [f"Project: {ctx.config.project_name} ({ctx.config.project_dir})"]
    try:
        health = ctx.client.health()
        lines.append(f"Editor: connected (engine {health.get('engine')}, plugin {health.get('plugin')})")
    except PluginUnavailable:
        lines.append("Editor: NOT connected")
    cov = ctx.index.deep_coverage()
    lines.append(f"Index: {cov['total']} blueprints, {cov['deep']} deep")
    lines.append(f"Assets referenced: {summary['assets']}  changesets: {summary['changesets']}")
    for ws_id, ids in summary["working_sets"].items():
        lines.append(f"{ws_id}: " + ", ".join(f"{i} {short(ctx.session.path_for(i))}" for i in ids))
    for cs in ctx.session.changesets[-5:]:
        lines.append(f"{cs['id']} {cs['tool']}: " + ", ".join(short(a) for a in cs['assets']))
    lines.append(f"Plugin calls: {ctx.client.calls} ({ctx.client.total_ms:.0f} ms)")
    return "\n".join(lines)
