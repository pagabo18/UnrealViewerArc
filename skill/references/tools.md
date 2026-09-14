# Tool cheat sheet (unreal-agent MCP)

Discovery: `search(query, kind?, limit?, cursor?)` · `who_calls(function)` · `who_reads(variable)` · `who_writes(variable)` · `find_references(asset)` · `index_project(deep?)` · `working_set(assets, op?)` · `session_info()`
Inspect: `inspect_blueprint(asset, detail=summary|structure|components)` · `inspect_graph(asset, graph?, around?, depth?, query?, kind?, offset?)` · `find_nodes(asset, query?, kind?)` · `inspect_widget_tree(asset, root?)` · `inspect_widget(asset, widget, all?)` · `inspect_animations(asset)`
Blueprint edits: `blueprint_variable(asset, op, name, ...)` · `blueprint_function(asset, op, name, inputs?, outputs?, pure?)` · `blueprint_interface(asset, op, interface)` · `blueprint_component(asset, op, name, class?, parent?, properties?)` · `create_blueprint(path, parent, kind?)`
Graph edits: `add_node(asset, type, graph?, after?, before?, pins?, connect?)` · `delete_nodes(asset, nodes)` · `connect(asset, from, to | links)` · `disconnect(asset, pin | from,to)` · `set_pins(asset, node, pins)` · `replace_node(asset, node, type, ...)` · `clone_nodes(asset, nodes)` · `add_local_variable(asset, graph, name, type)`
Widget edits: `clone_widget(asset, source, new_name, insert_after?|parent?, children?)` · `add_widget(asset, class, name, parent?)` · `set_widget(asset, widget, properties?, slot?, rename?)` · `move_widget` · `remove_widget` · `copy_widget_style(asset, source, targets)` · `bind_widget_event(asset, widget, event, function?, create_function?)`
Build: `compile_blueprint(assets|WS#, save?, validate?)` · `save_assets` · `validate_assets` · `undo(steps)` · `redo` · `reload_asset`
Visual: `preview_widget(asset, check?, image?)` · `screenshot_diff(a, b)`
Meta: `get_capabilities()` · `get_log(level, contains?)` · `batch(ops=[{tool,args}], atomic?)` · `source_control_status(assets)` · `open_in_editor(asset)`

Ids: `A#` asset · `N#` node · `N#.Pin` / `N#.P#` pin · `WS#` working set · `CS#` changeset · `S#` widget style. Assets also accept `/Game/...` paths or unique names.
Every edit tool accepts `compile=false` to defer compilation (default follows config `autoCompile`).
