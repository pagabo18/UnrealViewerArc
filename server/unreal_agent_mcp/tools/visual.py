"""Visual validation: widget previews (layout rectangles + PNG) and pixel diffs."""
from . import P, schema, tool
from .common import as_list, resolve_asset_or_fail
from ..formatters import format_layout_check, format_preview


@tool("preview_widget",
      "Render a Widget Blueprint off-screen: returns every widget's rectangle (x,y,w,h) and optionally a PNG path. "
      "Use it after layout/UMG changes; check=[names] prints sizes and vertical gaps for those widgets.",
      schema({
          "asset": P("string", "Widget Blueprint."),
          "width": P("integer", "Render width (default 1920)."),
          "height": P("integer", "Render height (default 1080)."),
          "image": P("boolean", "Write a PNG (default true)."),
          "path": P("string", "Output PNG path."),
          "check": P("array", "Widget names to report sizes/gaps for (visual validation without images).", items={"type": "string"}),
      }, ["asset"]))
def preview_widget(ctx, asset, width=None, height=None, image=None, path=None, check=None):
    asset_path = resolve_asset_or_fail(ctx, asset)
    params = {"asset": asset_path, "width": int(width or 1920), "height": int(height or 1080), "image": True if image is None else bool(image)}
    if path:
        params["path"] = path
    result = ctx.client.call("widget.preview", params, timeout=300)
    text = format_preview(result, ctx)
    names = as_list(check)
    if names:
        text += "\nCheck:\n" + "\n".join(format_layout_check(result.get("layout", []), names))
    return text


@tool("screenshot_diff", "Pixel diff between two PNGs (e.g. previews before/after): percent changed and bounding box.",
      schema({"a": P("string", "First PNG path."), "b": P("string", "Second PNG path."), "threshold": P("integer", "Per-channel tolerance 0-255 (default 12).")}, ["a", "b"]))
def screenshot_diff(ctx, a, b, threshold=None):
    params = {"a": a, "b": b}
    if threshold is not None:
        params["threshold"] = int(threshold)
    result = ctx.client.call("widget.preview_diff", params)
    if result.get("size_mismatch"):
        return f"Size mismatch: {result.get('width')}x{result.get('height')} vs {result.get('width_b')}x{result.get('height_b')}"
    line = f"Visual diff: {result.get('diff_percent', 0)}% ({result.get('changed_pixels', 0)} px)"
    box = result.get("bbox")
    if box:
        line += f"\nchanged region: {box['x']},{box['y']} {box['w']}x{box['h']}"
    else:
        line += "\nno differences"
    return line
