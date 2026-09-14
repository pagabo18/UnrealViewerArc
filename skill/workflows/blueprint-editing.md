# Workflow: Blueprint logic editing

Goal: change graphs/variables/functions while mirroring the project's own patterns.

## Steps
1. `search(query)` → pick the `A#`. If the user names a pattern ("like Health"), find where it lives: `inspect_blueprint(A#, detail=structure)` lists variables (with category/default/rep flags) and function signatures.
2. Read the reference pattern only: `inspect_graph(A#, graph="RegenHealth")` (function graphs are small; prefer them over the EventGraph). For the EventGraph use `find_nodes(A#, query="Health")` then `inspect_graph(A#, around=N#, depth=1)`.
3. Plan the mirror: same categories, naming (`Stamina`, `StaminaRegenRate`, `RegenStamina`), same node shapes (Get → Multiply → Add → Clamp → Set).
4. Apply with one `batch` when the ops are independent (variables, function creation), then node ops:
   - `blueprint_variable(op=add, name, type, default, category, replicated=...)`
   - `blueprint_function(op=create, name, inputs=["DeltaTime:float"], outputs=[...])` → returns the entry node `N#`
   - `add_node(graph=..., type=variable_get|variable_set|function_call|branch|..., after=N#)` — `after` splices into the exec chain
   - `connect(from="N3.ReturnValue", to="N4.Health")`; bare `N#` means the default exec pin
   - `set_pins(node=N#, pins={"Min": "0"})`
5. Wire the call site: `add_node(type=function_call, function="RegenStamina", after=<Tick node>)`.
6. `compile_blueprint(assets=[A#])`. Errors list `N#` ids → `inspect_graph(around=N#, depth=1)` and fix; never re-read the whole graph.
7. `save_assets` (or `compile_blueprint(save=true)`), then report.

## Tips
- Pin references: `N12.Condition`, `N12.P2` (alias from the last inspection), `N12` (exec).
- `add_node` with `connect={"A": "N7.ReturnValue"}` wires data pins at creation.
- Conflicting input link → tool fails with the existing link; pass `force=true` only when replacing is intended.
- `delete_nodes` bridges exec flow automatically; `replace_node` migrates links by pin name.
- Overriding parent events: `blueprint_function(op=create, name="ReceiveBeginPlay")` places the event node; `add_node(type=event, event=...)` also works.
- Components: `blueprint_component(op=add, name, class="StaticMeshComponent", parent="Root", properties={...})`.
- Unsupported: timelines (use a Delay/loop or ask the user), animation keyframes.
