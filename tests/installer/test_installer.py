"""Installer tests run in a temporary HOME with a fake Unreal project (no engine needed)."""
import json
import os
import shutil

import pytest

import installer


@pytest.fixture
def sandbox(tmp_path, monkeypatch):
    home = tmp_path / "home"
    home.mkdir()
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("USERPROFILE", str(home))
    monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(home / ".claude"))
    monkeypatch.setattr(installer, "STATE_DIR", str(home / ".unreal-agent"))
    monkeypatch.setattr(installer, "STATE_FILE", str(home / ".unreal-agent" / "installs.json"))
    monkeypatch.setattr(installer, "find_claude_cli", lambda: None)
    monkeypatch.setattr(installer, "find_engine_installs", lambda: {})
    project = tmp_path / "MyGame"
    (project / "Content").mkdir(parents=True)
    uproject = project / "MyGame.uproject"
    uproject.write_text(json.dumps({"FileVersion": 3, "EngineAssociation": "5.5", "Plugins": [{"Name": "Other", "Enabled": True}]}, indent="\t"), encoding="utf-8")
    return {"home": home, "project": project, "uproject": uproject}


def test_install_project_mode(sandbox, capsys):
    rc = installer.main(["install", "--project", str(sandbox["uproject"]), "--yes"])
    assert rc == 0
    plugin_dir = sandbox["project"] / "Plugins" / installer.PLUGIN_NAME
    assert (plugin_dir / f"{installer.PLUGIN_NAME}.uplugin").is_file()
    assert (plugin_dir / "Source" / installer.PLUGIN_NAME / f"{installer.PLUGIN_NAME}.Build.cs").is_file()
    assert (plugin_dir / ".claude-agent-install.json").is_file()
    data = json.loads(sandbox["uproject"].read_text(encoding="utf-8"))
    assert {"Name": installer.PLUGIN_NAME, "Enabled": True} in data["Plugins"]
    assert {"Name": "Other", "Enabled": True} in data["Plugins"]
    skill = sandbox["home"] / ".claude" / "skills" / installer.SKILL_NAME / "SKILL.md"
    assert skill.is_file()
    assert (skill.parent / "workflows" / "widget-editing.md").is_file()
    mcp = json.loads((sandbox["project"] / ".mcp.json").read_text(encoding="utf-8"))
    entry = mcp["mcpServers"][installer.MCP_NAME]
    assert entry["args"][0].endswith(os.path.join("server", "run_server.py"))
    assert entry["args"][-1] == str(sandbox["uproject"])
    assert os.path.isfile(entry["command"])
    config = sandbox["project"] / ".unreal-agent" / "config.json"
    assert json.loads(config.read_text(encoding="utf-8"))["responseMode"] == "compact"
    state = json.loads((sandbox["home"] / ".unreal-agent" / "installs.json").read_text(encoding="utf-8"))
    assert state["installs"][0]["project"] == str(sandbox["uproject"])
    out = capsys.readouterr().out
    assert "Next steps" in out


def test_update_and_uninstall(sandbox):
    installer.main(["install", "--project", str(sandbox["uproject"]), "--yes"])
    # simulate an older install stamp -> update re-copies
    stamp_path = sandbox["project"] / "Plugins" / installer.PLUGIN_NAME / ".claude-agent-install.json"
    stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
    stamp["version"] = "0.0.1"
    stamp_path.write_text(json.dumps(stamp), encoding="utf-8")
    assert installer.main(["update", "--no-pull"]) == 0
    assert json.loads(stamp_path.read_text(encoding="utf-8"))["version"] == installer.read_version()
    assert installer.main(["uninstall"]) == 0
    assert not (sandbox["project"] / "Plugins" / installer.PLUGIN_NAME).exists()
    assert not (sandbox["project"] / ".mcp.json").exists()
    assert not (sandbox["home"] / ".claude" / "skills" / installer.SKILL_NAME).exists()
    data = json.loads(sandbox["uproject"].read_text(encoding="utf-8"))
    assert all(p["Name"] != installer.PLUGIN_NAME for p in data.get("Plugins", []))
    assert (sandbox["project"] / ".unreal-agent" / "config.json").is_file()  # kept unless --purge
    assert (sandbox["project"] / "Content").is_dir()


def test_uninstall_purge_and_user_scope(sandbox):
    installer.main(["install", "--project", str(sandbox["uproject"]), "--yes", "--scope", "user"])
    claude_json = sandbox["home"] / ".claude.json"
    assert installer.MCP_NAME in json.loads(claude_json.read_text(encoding="utf-8"))["mcpServers"]
    assert not (sandbox["project"] / ".mcp.json").exists()
    installer.main(["uninstall", "--purge"])
    assert installer.MCP_NAME not in json.loads(claude_json.read_text(encoding="utf-8")).get("mcpServers", {})
    assert not (sandbox["project"] / ".unreal-agent").exists()


def test_project_detection_from_directory(sandbox):
    rc = installer.main(["install", "--project", str(sandbox["project"]), "--yes"])
    assert rc == 0


def test_engine_association_matching(tmp_path):
    engine = tmp_path / "UE_5.5"
    (engine / "Engine" / "Build").mkdir(parents=True)
    (engine / "Engine" / "Build" / "Build.version").write_text(json.dumps({"MajorVersion": 5, "MinorVersion": 5, "PatchVersion": 4}), encoding="utf-8")
    uproject = tmp_path / "P.uproject"
    uproject.write_text(json.dumps({"EngineAssociation": "5.5"}), encoding="utf-8")
    assert installer.engine_for_project(str(uproject), {"5.5": str(engine)}) == str(engine)
    assert installer.engine_version_of(str(engine)) == "5.5.4"
    uproject.write_text(json.dumps({"EngineAssociation": "{ABC-GUID}"}), encoding="utf-8")
    assert installer.engine_for_project(str(uproject), {"{ABC-GUID}": str(engine)}) == str(engine)


def test_no_hardcoded_paths_in_repo():
    """Guard: the installable parts must not contain user-specific paths."""
    root = installer.REPO_ROOT
    bad = []
    for folder in ("server", "skill", "scripts", "unreal-plugin"):
        for dirpath, _, files in os.walk(os.path.join(root, folder)):
            for name in files:
                if name.endswith((".py", ".md", ".ps1", ".sh", ".cpp", ".h", ".cs", ".json", ".cmd")):
                    text = open(os.path.join(dirpath, name), "r", encoding="utf-8", errors="ignore").read()
                    for needle in ("C:\\Users\\", "/Users/gabriel", "/home/gabriel", "UnrealViewerArc/Content"):
                        if needle in text:
                            bad.append(f"{name}: {needle}")
    assert not bad, bad
