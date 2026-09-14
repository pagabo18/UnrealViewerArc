# Plugin API (HTTP) and MCP tools

## Transport
`POST http://127.0.0.1:<port>/rpc` with header `X-Agent-Token: <token>` (both from `<Project>/Saved/ClaudeAgent/endpoint.json`).

Request: `{"id": 1, "cmd": "blueprint.summary", "params": {"asset": "/Game/UI/WBP_MainMenu"}}`
Response: `{"ok": true, "result": {...}, "ms": 3.2, "seq": 118}` or `{"ok": false, "error": {"code": "NOT_FOUND", "message": "...", "details": {...}}}`
Error codes: `BAD_REQUEST`, `NOT_FOUND`, `AMBIGUOUS`, `UNSUPPORTED` (details.capability), `CONFLICT` (e.g. pin already connected; details.existing/hint), `COMPILE_FAILED`, `SAVE_FAILED`, `INTERNAL`, `UNKNOWN_COMMAND`, `UNAUTHORIZED` (HTTP 401).
`GET /health` → `{"ok":true,"project":...,"engine":...,"plugin":...,"seq":...}`.

Asset references accept a package path (`/Game/UI/WBP_MainMenu`), an object path, or a unique short name. Node references are GUIDs (the MCP server maps them to `N#`). Pin references are `GUID.PinName` (or `GUID` for the default exec pin).

## Commands
| Command | Mutating | Purpose / main params |
|---|---|---|
| `system.ping` | | project, engine, plugin version, seq, PIE, source control |
| `system.capabilities` | | capability registry + command list |
| `system.changes` | | `since`, `limit` → change events, `seq`, `overflow` |
| `system.log` | | `level`, `category`, `contains`, `since`, `limit`, `clear` |
| `system.undo` / `system.redo` / `system.transactions` | yes | editor undo stack |
| `system.config`, `system.gc` | | |
| `assets.list` | | `paths`, `class`, `contains`, `blueprints_only`, `mtime`, `modified_since`, `offset`, `limit` |
| `assets.info` / `assets.dependencies` / `assets.referencers` | | registry data |
| `assets.source_control` / `assets.checkout` | / yes | file states |
| `assets.open` | | open in editor |
| `assets.create_blueprint` | yes | `path`, `parent`, `kind` (auto/blueprint/widget), `root` |
| `blueprint.summary` / `structure` / `index_entry` / `components` | | detail levels |
| `blueprint.variable` | yes | `op` add/remove/rename/modify, `name`, `type`, `default`, `category`, `tooltip`, `replicated`, `rep_notify`, `instance_editable`, `read_only`, `expose_on_spawn`, `private` |
| `blueprint.function` | yes | `op` create/delete/rename/modify, `inputs`, `outputs`, `remove_pins`, `pure`, `category`, `tooltip`, `access`, `override` |
| `blueprint.interface` | yes | `op`, `interface` |
| `blueprint.component` | yes | `op`, `name`, `class`, `parent`, `properties` |
| `blueprint.compile` | yes | `assets`, `save`, `validate` → per-asset status, messages, `node_errors` |
| `blueprint.save` / `blueprint.validate` / `blueprint.reload` | yes/–/yes | |
| `graph.list` / `graph.inspect` / `graph.find_nodes` | | `graph`, `around`, `depth`, `query`, `kind`, `offset`, `limit`, `pins`, `hidden_pins`, `defaults` |
| `graph.add_node` | yes | `type` (+ type params), `after`, `before`, `pins`, `connect`, `comment`, `force`, `x`, `y` |
| `graph.delete_nodes` | yes | `nodes`, `reconnect` |
| `graph.connect` / `graph.disconnect` | yes | `from`, `to`, `links`, `force` / `pin` |
| `graph.set_pins` | yes | `node`, `pins`, `comment`, `x`, `y`, `enabled` |
| `graph.replace_node` / `graph.clone_nodes` / `graph.local_variable` | yes | |
| `widget.tree` / `widget.inspect` / `widget.animations` | | `root`, `styles`, `max_depth` / `widget`, `all` |
| `widget.clone` | yes | `source`, `new_name`, `insert_after`/`insert_before`/`parent`+`index`, `properties`, `children`, `rename_children` |
| `widget.add` / `widget.remove` / `widget.move` / `widget.set` / `widget.copy_style` | yes | |
| `widget.bind_event` | yes | `widget`, `event`, `function`, `create_function`, `force` |
| `widget.preview` / `widget.preview_diff` | | `width`, `height`, `image`, `path` → `layout`, `image` / `a`, `b`, `threshold` |
| `batch` | yes | `ops` [{cmd, params}], `atomic`, `stop_on_error`, `title` |

Node JSON: `{guid, kind, class, title, member, member_class, pure, latent, x, y, comment, error, pins:[{id, name, dir, type, default, links:[{node, pin}], hidden, adv, parent}]}`.

## MCP tools
See `skill/references/tools.md` for the cheat sheet; each tool's schema is in `server/unreal_agent_mcp/tools/*.py`. Tools return compact text (see `docs/TOKEN_OPTIMIZATION.md`); `format="raw"` returns the plugin JSON for debugging.
