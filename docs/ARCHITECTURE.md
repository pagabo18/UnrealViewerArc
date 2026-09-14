# Architecture

```
┌──────────┐   skill (SKILL.md + lazy refs)    ┌─────────────────────┐   stdio JSON-RPC (MCP)   ┌────────────────────────┐   HTTP 127.0.0.1:8766   ┌──────────────────────────┐
│  Claude  │ ───────────────────────────────▶ │ Claude Code client  │ ◀──────────────────────▶ │ unreal_agent_mcp (py)  │ ◀─────────────────────▶ │ ClaudeBlueprintAgent (C++)│ ──▶ Editor APIs
└──────────┘                                   └─────────────────────┘                          └────────────────────────┘   X-Agent-Token          └──────────────────────────┘
```

## Components
- **skill/** — behaviour: workflow, rules, tool cheat sheet, formats. Kept small; workflows/references are read only when relevant.
- **server/unreal_agent_mcp/** — the "intelligent client":
  - `protocol.py` minimal MCP stdio server (initialize, tools/list, tools/call, ping).
  - `bridge.py` HTTP client; discovers `<Project>/Saved/ClaudeAgent/endpoint.json` (port + token), re-reads it when the editor restarts.
  - `session.py` session memory: `A#` asset ids, `N#` node ids ↔ GUIDs (+ `P#` pin aliases), working sets, changesets, cursors, cache keyed by asset and invalidated through the plugin's change sequence.
  - `index.py` project index: shallow entries from the Asset Registry (`assets.list` with file mtimes, no asset loading) + lazy deep entries (`blueprint.index_entry`: functions, variables, events, widgets, calls/reads/writes). Persisted at `<Project>/.unreal-agent/cache/index.json`; incremental via mtimes, dirty flags and editor change events; conventions inferred into `.unreal-agent/conventions.json`.
  - `formatters.py` compact text renderers (graph/widget/diff/compile formats).
  - `tools/` 48 MCP tools grouped by discovery, inspection, blueprint/graph/widget edits, build, visual, meta. Every edit records a changeset and returns a diff.
- **unreal-plugin/ClaudeBlueprintAgent/** — editor-only module:
  - `Core/AgentServer` HTTP endpoint (`/rpc`, `/health`) using the engine `HTTPServer` module; requests execute on the game thread; token from the endpoint file; bind address forced to 127.0.0.1 unless `allowRemote`.
  - `Core/AgentCommandRegistry` name → handler; `Commands/*` implement `system.*`, `assets.*`, `blueprint.*`, `graph.*`, `widget.*`, `batch`.
  - `Core/AgentChangeTracker` sequence-numbered change events (modified/compiled/saved/added/removed/renamed) from `OnObjectModified`, `OnBlueprintPreCompile`, `PackageSavedWithContext`, Asset Registry.
  - `Core/AgentLogCapture` ring buffer of warnings/errors (counts + slices; full log stays in `Saved/Logs`).
  - `Core/AgentResolver` loose identifiers → objects (asset path/name, graph, node GUID, pin name/id, function specs).
  - `Core/PinTypeUtils` compact type spec ↔ `FEdGraphPinType`; `Core/PropertyUtils` reflection read/write by dotted path, non-default detection, style hashing.
  - `Serialization/GraphSerializer` nodes/pins → JSON with neighbourhood/query/pagination filtering.
  - `Adapters/` `IUnrealAdapter` + `FUnrealAdapterBase` (UE 5.5) + `FUE56Adapter`/`FUE57Adapter`; `AgentCompat.h` is the only file with version `#if`s.
  - `Tests/` editor automation tests (`ClaudeBlueprintAgent.*`).

## Data flow of an edit
1. Claude calls `add_node(asset="A12", type="variable_set", variable="Stamina", after="N50")`.
2. Server resolves `A12` → `/Game/.../BP_Player`, `N50` → GUID, sends `graph.add_node`.
3. Plugin opens an `FScopedTransaction`, `Modify()`s the Blueprint/graph, creates the node, splices links through the K2 schema, marks the Blueprint modified, records a change event, returns the node JSON.
4. Server assigns `N52`, records changeset `CS4`, optionally compiles (`blueprint.compile`) and returns a diff:
   ```
   ChangeSet CS4 (add_node) A12 BP_Player
   + N52 Set Stamina [RegenStamina]
     N50 -> N52
   Compile 1/1 OK
   ```

## Safety model
- All mutations inside editor transactions (undo/redo via `system.undo`); batch = one transaction with automatic undo on failure.
- No binary asset manipulation; saving goes through `UEditorAssetLibrary::SaveLoadedAsset` (source control aware).
- The HTTP endpoint is local-only by default and requires the per-session token; the token lives in `Saved/ClaudeAgent/endpoint.json` (never committed).
- Capability registry prevents hallucinated features: unsupported operations return `UNSUPPORTED` with a capability id.

## Extensibility
- New plugin command: add a handler in `Commands/*.cpp`, register it in the module's `Register*Commands`.
- New tool: add a function with `@tool(...)` in `server/unreal_agent_mcp/tools/*.py` and a formatter if needed.
- New engine version: subclass `FUnrealAdapterBase`, override what changed, select it in `AgentAdapterFactory::Create`.
