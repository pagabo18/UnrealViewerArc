# Install

## Requirements
- Unreal Engine 5.5 / 5.6 / 5.7 (launcher or source build) with a C++ toolchain able to build plugins (Visual Studio 2022 on Windows, Xcode on macOS, clang on Linux). Blueprint-only projects are fine: the editor compiles the plugin on first launch.
- Claude Code (CLI or desktop). The skill is installed to `~/.claude/skills`.
- Python 3.9+ for the MCP server (system Python, `py` launcher, or the Python bundled with Unreal at `Engine/Binaries/ThirdParty/Python3`). No packages are installed.
- Git.

## Windows
```powershell
git clone https://github.com/pagabo18/UnrealViewerArc claude-unreal-blueprint-agent
cd claude-unreal-blueprint-agent
.\scripts\install.cmd
```
Options (`install.ps1`): `-Project <path.uproject>` `-Engine` (global plugin) `-Scope user` (MCP for all projects) `-Build` (compile now with UBT) `-Yes` (non-interactive) `-Python <exe>`.

## macOS / Linux
```bash
./scripts/install.sh --project /path/MyGame.uproject [--engine] [--scope user] [--build] [--yes]
```

## What happens
1. Detects Claude Code, engine installs (registry, launcher manifest, common folders, `Install.ini`), `.uproject` (argument, current folder, parents, `Documents/Unreal Projects`).
2. Copies `unreal-plugin/ClaudeBlueprintAgent` → `<Project>/Plugins/ClaudeBlueprintAgent` (or `<Engine>/Engine/Plugins/Marketplace` with `--engine`) and enables it in the `.uproject`.
3. Copies `skill/` → `~/.claude/skills/unreal-blueprint-agent`.
4. Writes the MCP server entry `unreal-agent` to `<Project>/.mcp.json` (project scope, shareable with the team) or to the user scope via `claude mcp add` / `~/.claude.json`.
5. Creates `<Project>/.unreal-agent/config.json` (from `unreal-agent.config.json`) and records the install in `~/.unreal-agent/installs.json`.

## Then
1. Open the project in Unreal. Accept the rebuild prompt for `ClaudeBlueprintAgent` if shown.
2. Output Log shows `Claude Blueprint Agent listening on http://127.0.0.1:8766/rpc` and `Saved/ClaudeAgent/endpoint.json` is written (port + per-session token used by the MCP server).
3. Run Claude Code in the project folder (project scope) → `/mcp` lists `unreal-agent`. Ask Claude to work on Blueprints. On the first session say "index the project" for full function/variable search coverage (one-time, persisted).

## Update / uninstall / diagnose
- `git pull` + `scripts\update.cmd` (`./scripts/update.sh`): re-copies plugin/skill when the version or commit changed, refreshes MCP config. Add `-Build` to compile immediately.
- `scripts\uninstall.cmd` (`./scripts/uninstall.sh`): removes the plugin folder, the `.uproject` entry, the MCP config and the skill. Project assets are never touched; `.unreal-agent/` is kept unless `--purge`.
- `scripts\doctor.ps1` (`./scripts/doctor.sh`): shows detected engines, installs and whether the plugin is reachable.

See `docs/INSTALLATION.md` for verification steps and `docs/TROUBLESHOOTING.md` for common problems.
