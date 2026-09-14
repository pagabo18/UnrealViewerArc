# Token optimization

Design goal: maximum capability with minimum context. Correctness > safety > capability > token efficiency > latency.

## Techniques
1. **Progressive disclosure** — `search` (ids only) → `inspect_blueprint` summary (counts, parent, interfaces, refs) → `structure` (signatures/variables) → `inspect_graph` (nodes) → `around/depth` windows.
2. **Query-first** — `search(kind=function)`, `find_nodes(query, kind)`, `who_calls/reads/writes` answer without opening assets in context.
3. **Server-side filtering** — the plugin filters by neighbourhood, query, kind and pages results; the server never forwards 10k nodes.
4. **Short ids** — `A#`, `N#`, `N#.P#`, `WS#`, `CS#`, `S#` mapped internally to paths/GUIDs for the session.
5. **Compact formats** — one line per node, exec links at the source and data links at the consumer (each link printed once), defaults only when non-default, widget trees with box-drawing and style fingerprints listed once.
6. **Diffs and changesets** — edits return `+/-/~` lines and the compile result, never the whole asset.
7. **Session cache** — summaries/structures/trees cached per asset and invalidated by editor change events (`system.changes`), so re-inspection is free until something changes.
8. **Incremental index** — persisted on disk; shallow refresh uses Asset Registry + file mtimes; deep entries are refreshed only for changed assets.
9. **Macro operations** — `clone_widget` (one call instead of ~20), `add_node(after=…)`, `bind_widget_event(create_function=…)`, `replace_node`, `batch`.
10. **Lazy skill** — `SKILL.md` ≈ 900 tokens; workflows/references loaded on demand.
11. **Logs on disk** — only counts and filtered slices reach Claude.
12. **Visual validation by numbers** — layout rectangles and gap checks instead of images; PNG only on demand.

## Budgets (enforced by tests)
| Output | Budget |
|---|---|
| `inspect_blueprint` summary | < 300 tokens |
| `inspect_blueprint` structure | < 1000 tokens |
| `inspect_graph` page (≤ 60 nodes) | < 2000 tokens |
| Task: find a function in a 500-BP project | < 2000 tokens |
| Task: add a styled button to a menu | < 5000 tokens |
| Task: mirror a Health pattern for Stamina | < 6000 tokens |

Actual numbers: `docs/TOKEN_BENCHMARK.md`.

## Costs to keep in mind
- Tool schemas: ~10k tokens for 48 tools when a client loads all of them; Claude Code defers MCP tool schemas (tool search) so this is mostly not paid up-front.
- Deep indexing: one-time load of every Blueprint; persisted.

## Response modes
`responseMode` in `.unreal-agent/config.json`: `compact` (default), `normal` (same output; reserved for extra hints), `debug` (appends a token estimate to each response). Tools also accept `format="raw"` for plugin JSON.
