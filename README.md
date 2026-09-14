# claude-unreal-blueprint-agent

Give Claude capabilities, not context.

A portable, versioned Claude skill + Unreal Editor plugin + MCP server that lets Claude **inspect and edit Blueprints and Widget Blueprints directly inside a running Unreal Editor** — search assets, read graphs, add/connect nodes, edit variables/functions/components, clone widgets while preserving their exact style, bind events, compile, validate, save, undo, and render previews — while consuming as few tokens as possible.

```
Claude ──> Skill (how to work) ──> MCP tools (Python, zero deps) ──> HTTP localhost ──> ClaudeBlueprintAgent plugin (C++, editor-only) ──> Unreal Editor APIs
```

The plugin is the source of truth: every change goes through regular editor APIs with transactions (undo works), `Modify()`, compilation and the normal save pipeline. No `.uasset` is touched binarily.

## Why it is token-efficient

| Principle | How |
|---|---|
| Progressive disclosure | `search` → `inspect_blueprint(summary)` (< 300 tokens) → `structure` → `inspect_graph(around=N#, depth=1)`; nothing is dumped by default |
| Server-side filtering | queries, kinds, neighbourhood windows, pagination run inside the plugin/server |
| Short ids | `A12` assets, `N14` nodes, `N14.P2` pins, `WS7` working sets, `S3` styles, `CS12` changesets |
| Compact formats | deterministic text graphs (`then -> N5`, `Condition <- N9.ReturnValue`), tree-drawn widget hierarchies, style fingerprints listed once |
| Diffs, not states | every edit returns a changeset diff + compile result |
| Session cache & index | persisted incremental project index (Asset Registry + lazy deep entries), invalidated by editor change events |
| Macro operations | `clone_widget`, `bind_widget_event`, `add_node(after=...)`, `replace_node`, `batch` (one transaction, atomic rollback) |
| Lazy documentation | a small `SKILL.md`; workflows/references loaded only when relevant |

Measured (synthetic 500-Blueprint project): find a function 452 tokens, add a styled "Credits" button end-to-end 851 tokens, mirror a Health→Stamina pattern 428 tokens. See `docs/TOKEN_BENCHMARK.md`.

## Install (Windows first; macOS/Linux supported)

```powershell
git clone https://github.com/pagabo18/UnrealViewerArc claude-unreal-blueprint-agent
cd claude-unreal-blueprint-agent
.\scripts\install.cmd            # or: .\scripts\install.ps1 -Project "C:\Path\MyGame.uproject"
```

```bash
./scripts/install.sh --project /path/MyGame.uproject     # macOS / Linux
```

The installer detects Claude Code, Unreal Engine installations, the `.uproject`, a Python 3.9+ interpreter (falls back to the one bundled with Unreal), copies the plugin into `<Project>/Plugins/` (project install, the safe default; `--engine` for a global install), enables it in the `.uproject`, installs the skill into `~/.claude/skills/unreal-blueprint-agent`, writes the MCP configuration (`<Project>/.mcp.json`, or `--scope user`), and creates `<Project>/.unreal-agent/config.json`. Then open the project in Unreal (accept the module rebuild prompt on first launch — needs Visual Studio/Xcode), start Claude Code and ask:

> Revisa WBP_MainMenu y agrega un botón Credits igual a los demás.

Update: `git pull` then `scripts\update.cmd` (or `./scripts/update.sh`). Uninstall: `scripts\uninstall.cmd` (keeps project assets). Health check: `scripts\doctor.ps1`.

Details: `INSTALL.md`, `docs/INSTALLATION.md`, `docs/TROUBLESHOOTING.md`.

## What Claude can do

- **Discovery**: `search`, `who_calls`, `who_reads`, `who_writes`, `find_references`, `index_project`, `working_set`
- **Inspection**: `inspect_blueprint` (summary/structure/components), `inspect_graph` (windows, queries, pagination), `find_nodes`, `inspect_widget_tree` (with style fingerprints), `inspect_widget`, `inspect_animations`
- **Blueprint edits**: variables (type/default/category/replication/flags), functions (signature, pure, access, overrides), interfaces, components, new Blueprints
- **Graph edits**: add nodes of 20+ kinds (function calls, variables, events, custom events, branches, sequences, casts, macros, structs, switches, spawn/create widget, delays, prints, raw node classes), insert after/before in exec chains, connect/disconnect with conflict detection, pin defaults, replace with link migration, clone, delete with exec bridging, local variables
- **Widget edits**: deep clone preserving style + slot settings (children renamed by suffix, per-child overrides), add/remove/move/reorder, set nested properties (`Font.Size`, `WidgetStyle.Normal...`), copy style, bind delegates (OnClicked…) and create the handler function
- **Build**: compile (errors mapped to `N#` nodes), validate (DataValidation), save (source-control aware), undo/redo, reload
- **Visual**: off-screen widget preview → layout rectangles + PNG, spacing/size checks, pixel diff
- **Safety**: transactions, atomic batches, capability registry (`get_capabilities`), compact error responses, logs kept on disk

Unsupported today (registered in `capabilities.json`): timeline creation, widget animation keyframe editing, level/actor-instance editing.

## Compatibility

Unreal Engine 5.5, 5.6, 5.7 (adapter layer in `unreal-plugin/.../Adapters`; see `docs/COMPATIBILITY.md`). Claude Code on Windows/macOS/Linux; Python 3.9+ (Unreal's bundled Python works). No third-party Python packages.

## Repository layout

```
skill/            Claude skill (SKILL.md + lazy workflows/references)
unreal-plugin/    ClaudeBlueprintAgent editor plugin (C++)
server/           MCP server (Python, stdlib only)
scripts/          install / update / uninstall / doctor (Windows, macOS, Linux)
docs/             human documentation (architecture, API, capabilities, tokens, troubleshooting)
tests/            pytest (server, installer, token budgets), editor automation tests runner, e2e, benchmark
capabilities.json unreal-agent.config.json VERSION CHANGELOG.md
```

## Status

v0.1.0 — first functional release. The plugin was written against the UE 5.5–5.7 editor APIs and reviewed carefully, but this repository was authored in an environment without an Unreal Engine build, so the editor automation tests (`tests/unreal/`) and the live e2e tests must be run on a machine with the engine after the first install (see `docs/INSTALLATION.md` → "Verify"). Please report compile issues with the engine version and the failing line.

License: MIT.
