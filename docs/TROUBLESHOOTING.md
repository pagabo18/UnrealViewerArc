# Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Tools answer `EDITOR UNAVAILABLE` | The editor is not running, the plugin is disabled, or the port differs. Open the project, check Output Log for `LogClaudeAgent`, run `scripts\doctor.ps1`. The server re-reads `Saved/ClaudeAgent/endpoint.json` automatically. |
| Installer says Python not found / `python.exe: no se encontró Python` | Windows ships a Microsoft Store alias for `python.exe` that is not a real interpreter. Install Python (`winget install Python.Python.3.12`, open a new terminal) or pass `-Python "<UE>\Engine\Binaries\ThirdParty\Python3\Win64\python.exe"`. The bootstrap skips the Store stub automatically since 0.1.0. |
| `/mcp` does not list `unreal-agent` | Project scope: start Claude Code inside the project folder and accept the `.mcp.json` trust prompt. User scope: `claude mcp list`. Re-run the installer. |
| Editor asks to rebuild modules and fails | Install Visual Studio 2022 with "Game development with C++" (Windows) / Xcode (macOS). Check `Saved/Logs/UnrealBuildTool`. For Blueprint-only teams, use a prebuilt engine install (`docs/INSTALLATION.md`). |
| Compile errors in the plugin on a newer engine | The adapter layer isolates version differences: see `docs/COMPATIBILITY.md`, adjust `Adapters/UnrealAdapterBase.cpp` or `AgentCompat.h`, and open an issue with the engine version + error. |
| Port 8766 in use | Set `plugin.port` in `<Project>/.unreal-agent/config.json` or env `CLAUDE_AGENT_PORT`; run `ClaudeAgent.Restart` in the editor console. |
| `UNAUTHORIZED` | Token rotated (editor restarted) — the server retries once after re-reading the endpoint file; if you set `CLAUDE_AGENT_TOKEN` manually, keep it in sync with `plugin.token`. |
| Search does not find a function/variable | Deep index coverage: run `index_project(deep=true)` once (persisted). `search` prints coverage when partial. |
| Edits succeeded but the editor UI does not refresh | Graph editors refresh on `NotifyGraphChanged`; the Widget designer refreshes on structural modification. Click the tab to force a redraw, or reopen the asset. |
| Compile OK but Blueprint shows "dirty" | Saving is explicit (`autoSave=false`). Ask Claude to save, or set `autoSave: true`. |
| Widget preview fails with `does not compile` | Fix compile errors first; the preview needs a valid generated class. Construct/PreConstruct run in design-time mode (`IsDesignTime()` true). |
| Undo undid the wrong thing | Every agent edit is a named transaction (`Claude: …`); check `system.transactions` (tool `plugin_raw`) before undoing. |
| Large projects: first search is slow | Deep indexing loads each Blueprint once; the index is persisted and refreshed incrementally afterwards. Limit with `maxAutoDeepIndex` / `contentRoots`. |
| PIE running | Commands still work but editing during PIE is discouraged; the `system.ping` result reports `pie: true`. |

Logs: editor `Saved/Logs/<Project>.log` (category `LogClaudeAgent`); MCP server logs to stderr (Claude Code shows them in the MCP panel; `--log-level DEBUG`).
