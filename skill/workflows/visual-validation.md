# Workflow: Visual validation

Use after layout/UMG changes, when the user asks, or when a change is visually risky. Not after every trivial edit.

1. `preview_widget(A#, check=[siblings...])` → rectangles for all widgets + size/gap report for the checked ones. This is usually enough: same width/height, consistent gaps, expected order.
2. If something is off, compare before/after images: `preview_widget(A#, path=".../before.png")` before editing, `preview_widget(A#, path=".../after.png")` after, then `screenshot_diff(a, b)` → percent + bounding box. Map the bbox to the layout rectangles to name the widget.
3. Only read the PNG (Read tool on the image path) when numbers cannot explain the difference.
4. Report: `Visual: BTN_Credits 400x80 at 760,520, gaps 12/12 px (matches siblings)`.

Editor viewport screenshots are experimental; prefer widget previews.
