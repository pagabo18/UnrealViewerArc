# Installation (detailed)

See `INSTALL.md` for the quick path. This page covers modes, verification and a clean-machine checklist.

## Modes
- **Project install (default)**: plugin in `<Project>/Plugins/ClaudeBlueprintAgent`, enabled in the `.uproject`. Safe, per project, can be committed with the project.
- **Engine install (`--engine`)**: plugin in `<Engine>/Engine/Plugins/Marketplace/ClaudeBlueprintAgent`, available to all projects of that engine (still enabled per project in the `.uproject`).
- **MCP scope**: `project` writes `<Project>/.mcp.json` (Claude Code asks to trust it the first time; share it with the team); `user` registers `unreal-agent` for every project via `claude mcp add -s user` (falls back to editing `~/.claude.json`). With user scope the server finds the project through `--project` in the args.

## Building the plugin
The plugin is C++ (editor-only). Options:
1. Let the editor build it: open the project → "Missing modules… rebuild?" → Yes. Requires Visual Studio 2022 (Desktop C++ workload) / Xcode / clang.
2. `install.ps1 -Build` runs UnrealBuildTool (`Build.bat UnrealEditor Win64 Development -Project=…` for Blueprint-only projects or `<Project>Editor` for C++ projects).
3. Package once with `RunUAT BuildPlugin -Plugin=…uplugin -Package=… -TargetPlatforms=Win64` and use `--engine` install so Blueprint-only teammates without a compiler can use prebuilt binaries.

## Verify
1. Output Log: `LogClaudeAgent: Claude Blueprint Agent listening on http://127.0.0.1:8766/rpc`.
2. `scripts\doctor.ps1` → `plugin reachable`.
3. In Claude Code: `/mcp` shows `unreal-agent` connected; ask "ping the Unreal editor" (uses `session_info`).
4. Editor automation tests: `tests\unreal\run_editor_tests.ps1 -Project <path.uproject>` (headless; creates and deletes assets under `/Game/__ClaudeAgentTests`).
5. Live e2e from Python: `set CLAUDE_AGENT_PROJECT=<path.uproject>` then `python -m pytest tests/e2e`.

## Clean-machine checklist (portable install test)
```
git clone <repo> && cd claude-unreal-blueprint-agent
scripts\install.cmd -Project "D:\Projects\Other\Other.uproject" -Yes
open project → rebuild → Output Log shows listening
claude → "Revisa BP_Player y dime qué variables tiene" → inspect works
claude → "Agrega una variable Stamina float = 100 en BP_Player" → edit + compile + diff
claude → "Guarda BP_Player" → saved
```
The repository's own installer tests (`python -m pytest tests/installer`) exercise clone → install → update → uninstall in a temporary folder without an engine.

## Files written
| Location | Content |
|---|---|
| `<Project>/Plugins/ClaudeBlueprintAgent/` | plugin sources + `.claude-agent-install.json` stamp |
| `<Project>/<Name>.uproject` | `Plugins: [{Name: ClaudeBlueprintAgent, Enabled: true}]` |
| `<Project>/.mcp.json` | `mcpServers.unreal-agent` (project scope) |
| `<Project>/.unreal-agent/config.json`, `conventions.json`, `cache/` | project config, learned conventions, index cache (ignored by git) |
| `<Project>/Saved/ClaudeAgent/endpoint.json`, `Previews/` | runtime endpoint/token (written by the editor), preview PNGs |
| `~/.claude/skills/unreal-blueprint-agent/` | the skill |
| `~/.unreal-agent/installs.json` | install records for update/uninstall |
