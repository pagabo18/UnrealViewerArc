# claude-unreal-blueprint-agent — contributor notes

- Three parts: `unreal-plugin/` (C++ editor plugin, source of truth), `server/` (zero-dependency Python MCP server), `skill/` (Claude behaviour). Keep the plugin API contract in `docs/API.md` and the fake plugin (`tests/server/fake_plugin.py`) in sync when adding or changing commands.
- Run `python -m pytest tests -q` before committing; token budgets in `tests/server/test_token_budgets.py` are part of the contract. Regenerate `docs/TOKEN_BENCHMARK.md` with `python tests/benchmark/benchmark.py` when formatters change.
- Engine-version differences go only in `unreal-plugin/.../Private/Adapters/` (`AgentCompat.h` is the only file with `#if` on engine versions).
- Tool output must stay compact: ids (`A#`, `N#`, `S#`), diffs instead of states, pagination instead of truncation. New tools need a formatter and a test.
- No hard-coded user paths, project names, drive letters or engine paths anywhere (guarded by `tests/installer/test_installer.py::test_no_hardcoded_paths_in_repo`).
- Version bumps: `VERSION`, `.uplugin` VersionName, `Build.cs` define, `server/unreal_agent_mcp/__init__.py`, `pyproject.toml`, `capabilities.json`, `CHANGELOG.md` (see `docs/RELEASING.md`).
