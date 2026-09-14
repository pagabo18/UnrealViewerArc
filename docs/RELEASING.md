# Releasing and the dedicated repository

## Dedicated repository
This tool was authored inside `pagabo18/UnrealViewerArc` on branch `claude/unreal-blueprint-agent-skill-5wrzn3`. To give it its own repository named `claude-unreal-blueprint-agent`, either:

1. **Rename the GitHub repository** (Settings → General → Repository name → `claude-unreal-blueprint-agent`) and merge the branch into `main`:
   ```bash
   git fetch origin
   git checkout -B main origin/claude/unreal-blueprint-agent-skill-5wrzn3
   git push -u origin main
   ```
2. **Or create a new repository** and push the branch there:
   ```bash
   gh repo create <owner>/claude-unreal-blueprint-agent --public --source=. --remote=agent --push   # or create it in the GitHub UI
   git push agent claude/unreal-blueprint-agent-skill-5wrzn3:main
   ```
Then update the clone URL in `README.md`/`INSTALL.md` and the `CreatedByURL`/`DocsURL` fields in `unreal-plugin/ClaudeBlueprintAgent/ClaudeBlueprintAgent.uplugin`.

## Tags and releases
`v0.1.0` is tagged locally on the release commit. Publish it with:
```bash
git push origin v0.1.0
gh release create v0.1.0 --title "v0.1.0" --notes-file CHANGELOG.md   # optional GitHub release
```

## Cutting a new version
1. Update `VERSION`, `unreal-plugin/.../ClaudeBlueprintAgent.uplugin` (`VersionName`), `Build.cs` (`CLAUDE_AGENT_PLUGIN_VERSION`), `server/unreal_agent_mcp/__init__.py`, `server/pyproject.toml`, `capabilities.json`.
2. Add a `CHANGELOG.md` entry; regenerate `docs/TOKEN_BENCHMARK.md` (`python tests/benchmark/benchmark.py`).
3. `python -m pytest tests` (server + installer), run the editor automation tests on a machine with the engine (`tests/unreal/run_editor_tests.*`), and the live e2e (`CLAUDE_AGENT_PROJECT=... pytest tests/e2e`).
4. Commit, tag `vX.Y.Z`, push branch and tag. Users update with `git pull` + `scripts/update.*`.
