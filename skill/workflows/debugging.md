# Workflow: Debugging ("the inventory stopped working")

1. Locate: `search("Inventory")` → candidate assets (component, widgets, data assets). `find_references(A#)` for who uses what.
2. Working set of the suspects: `working_set(assets=[...])`.
3. Cheap checks first:
   - `compile_blueprint(WS#)` → compile errors with `N#` ids.
   - `validate_assets(WS#)` → data validation.
   - `get_log(level=error, contains="Inventory")` → recent runtime/editor errors.
4. Inspect only what the errors point at: `inspect_graph(A#, around=N#, depth=1)`; `inspect_blueprint(A#, detail=structure)` for missing variables/functions/interfaces.
5. Cross-references narrow the blast radius: `who_calls("AddItem")`, `who_writes("Items")`, `who_reads("Items")`.
6. Fix with the smallest edit (`set_pins`, `connect`, `replace_node`, `blueprint_variable(op=modify)`), compile, validate.
7. If the cause is outside Blueprint scope (C++, data, level instances), report exactly what was verified and what remains.

Never delete or "clean up" nodes you have not understood. Prefer `undo` if a change made things worse.
