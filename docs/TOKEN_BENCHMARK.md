# Token benchmark (synthetic project, fake plugin)

Measured with `tests/benchmark/benchmark.py` on the in-memory fake plugin (500 Blueprints, realistic naming, 0-39 node event graphs).
Token counts are estimates (~3.6 chars/token, identifier-heavy). Real editor numbers vary with project content; latency here excludes editor time.

Metric: **tokens per successful task** (sum of all tool outputs), tool calls per task, plugin round-trips.

| Task | Tool calls | Plugin calls | Tokens | Target | Status | Wall time |
|---|---|---|---|---|---|---|
| Search: find AddItem in 500 Blueprints | 2 | 503 | 452 | < 2000 | ok | 0.046s |
| Inspect: BP_Player summary + structure + one function graph | 3 | 7 | 321 | < 1500 | ok | 0.008s |
| Widget: add Credits button to WBP_MainMenu (clone + bind + compile + preview) | 7 | 510 | 851 | < 5000 | ok | 0.471s |
| Blueprint: stamina regen mirroring Health (batch + nodes + compile) | 7 | 513 | 428 | < 6000 | ok | 1.213s |
| Debug: inventory stopped working (search + refs + compile + validate + log) | 6 | 506 | 345 | < 2500 | ok | 0.032s |
| Big graph page: 39-node EventGraph, one page | 1 | 2 | 266 | < 2000 | ok | 0.0s |

## Per-call breakdown

**Search: find AddItem in 500 Blueprints**: search=21, who_calls=431
**Inspect: BP_Player summary + structure + one function graph**: inspect_blueprint=41, inspect_blueprint=120, inspect_graph=160
**Widget: add Credits button to WBP_MainMenu (clone + bind + compile + preview)**: search=20, inspect_widget_tree=444, inspect_widget=131, clone_widget=49, bind_widget_event=48, compile_blueprint=13, preview_widget=146
**Blueprint: stamina regen mirroring Health (batch + nodes + compile)**: search=19, inspect_blueprint=120, inspect_graph=160, batch=45, add_node=30, add_node=41, compile_blueprint=13
**Debug: inventory stopped working (search + refs + compile + validate + log)**: search=79, find_references=183, working_set=17, compile_blueprint=20, validate_assets=24, get_log=22
**Big graph page: 39-node EventGraph, one page**: inspect_graph=266

## Notes

- Deep indexing 500 Blueprints happened inside the first `search` (one-time cost, persisted to `.unreal-agent/cache/index.json`); later sessions skip it.
- Tool schemas (~10k tokens for 48 tools) are loaded by the MCP client, not counted here; Claude Code defers MCP tool schemas by default.
- Editor blocking time: every plugin command runs on the game thread; typical commands take 1-50 ms, `blueprint.compile` 50-2000 ms per asset, `widget.preview` 30-200 ms, deep-indexing loads each Blueprint once (10-200 ms each).
- Re-run after changing formatters; budgets are enforced by `tests/server/test_token_budgets.py`.
