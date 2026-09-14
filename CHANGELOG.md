# Changelog

All notable changes to this project are documented here. Semantic Versioning.

## [0.1.0] - 2026-09-14
### Added
- Unreal Editor plugin `ClaudeBlueprintAgent` (UE 5.5–5.7): localhost HTTP bridge with per-session token, command registry, change tracker, log capture, engine-version adapter layer.
- Commands: asset registry search/refs/source control/creation; Blueprint summary/structure/index entries; variables, functions, interfaces, components; graph inspection with neighbourhood windows, node creation (20+ kinds), delete with exec bridging, connect/disconnect with conflict detection, pin defaults, replace with link migration, clone; widget tree with style fingerprints, widget inspection, deep clone, add/remove/move/set/copy style, event binding, animation listing, off-screen preview with layout rectangles, PNG diff; compile/validate/save/reload; undo/redo; atomic batch.
- Zero-dependency Python MCP server: stdio JSON-RPC, endpoint discovery, short ids (A#/N#/WS#/CS#), incremental persisted project index with cross references and convention inference, compact formatters, 48 tools.
- Claude skill with lazy-loaded workflows (blueprint editing, widget editing, debugging, visual validation) and references.
- Installer/update/uninstall/doctor for Windows (PowerShell/cmd), macOS and Linux (bash) with automatic detection of Claude Code, engines, projects and Python.
- Tests: pytest suite (protocol, session, index, formatters, flows, token budgets, installer), editor automation tests, live e2e test, token benchmark.
- Documentation: README, INSTALL, architecture, API, capabilities, installation, troubleshooting, token optimization, compatibility, benchmark.
