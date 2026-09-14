#!/usr/bin/env python3
"""Cross-platform installer core for claude-unreal-blueprint-agent.

Invoked by scripts/install.* / update.* / uninstall.* wrappers, or directly:
    python scripts/lib/installer.py install [--project <path>] [--engine] [--scope project|user] [--build] [--yes]
    python scripts/lib/installer.py update
    python scripts/lib/installer.py uninstall [--purge]
    python scripts/lib/installer.py doctor

Everything is detected; nothing is hard-coded (no user paths, drive letters,
project names). State lives in ~/.unreal-agent/installs.json.
"""
import argparse
import glob
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PLUGIN_NAME = "ClaudeBlueprintAgent"
SKILL_NAME = "unreal-blueprint-agent"
MCP_NAME = "unreal-agent"
STATE_DIR = os.path.join(os.path.expanduser("~"), ".unreal-agent")
STATE_FILE = os.path.join(STATE_DIR, "installs.json")


def read_version():
    try:
        with open(os.path.join(REPO_ROOT, "VERSION"), "r", encoding="utf-8") as handle:
            return handle.read().strip()
    except OSError:
        return "0.0.0"


def git_sha():
    try:
        return subprocess.check_output(["git", "-C", REPO_ROOT, "rev-parse", "--short", "HEAD"], stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:  # noqa: BLE001
        return ""


# ---------------------------------------------------------------- output

class Log:
    quiet = False

    @staticmethod
    def step(msg):
        print(f"==> {msg}")

    @staticmethod
    def info(msg):
        print(f"    {msg}")

    @staticmethod
    def warn(msg):
        print(f"    ! {msg}")

    @staticmethod
    def ok(msg):
        print(f"    + {msg}")


def confirm(question, default=True, yes=False):
    if yes:
        return default
    suffix = " [Y/n] " if default else " [y/N] "
    try:
        answer = input(question + suffix).strip().lower()
    except EOFError:
        return default
    if not answer:
        return default
    return answer in ("y", "yes", "s", "si", "sí")


def choose(question, options, yes=False):
    if not options:
        return None
    if len(options) == 1 or yes:
        return options[0]
    print(question)
    for index, option in enumerate(options, start=1):
        print(f"  {index}) {option}")
    while True:
        try:
            answer = input(f"Select [1-{len(options)}]: ").strip()
        except EOFError:
            return options[0]
        if answer.isdigit() and 1 <= int(answer) <= len(options):
            return options[int(answer) - 1]


# ---------------------------------------------------------------- detection

def is_windows():
    return platform.system() == "Windows"


def is_mac():
    return platform.system() == "Darwin"


def find_claude_cli():
    for name in ("claude", "claude.cmd", "claude.exe"):
        path = shutil.which(name)
        if path:
            return path
    return None


def claude_home():
    return os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(os.path.expanduser("~"), ".claude")


def find_engine_installs():
    """Returns {version: engine_root} for launcher and source builds."""
    found = {}
    if is_windows():
        try:
            import winreg  # type: ignore
            for hive, key in ((winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\EpicGames\Unreal Engine"), (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Epic Games\Unreal Engine\Builds")):
                try:
                    with winreg.OpenKey(hive, key) as root:
                        index = 0
                        while True:
                            try:
                                sub = winreg.EnumKey(root, index)
                            except OSError:
                                break
                            index += 1
                            try:
                                with winreg.OpenKey(root, sub) as subkey:
                                    value, _ = winreg.QueryValueEx(subkey, "InstalledDirectory")
                                    if os.path.isdir(value):
                                        found[sub] = value
                            except OSError:
                                # Builds key stores GUID -> path as values, not subkeys
                                pass
                        # source builds: values under Builds
                        index = 0
                        while True:
                            try:
                                name, value, _ = winreg.EnumValue(root, index)
                            except OSError:
                                break
                            index += 1
                            if isinstance(value, str) and os.path.isdir(value):
                                found[name] = value
                except OSError:
                    continue
        except ImportError:
            pass
        launcher = os.path.join(os.environ.get("ProgramData", r"C:\ProgramData"), "Epic", "UnrealEngineLauncher", "LauncherInstalled.dat")
        if os.path.isfile(launcher):
            try:
                with open(launcher, "r", encoding="utf-8") as handle:
                    for item in json.load(handle).get("InstallationList", []):
                        app = item.get("AppName", "")
                        if app.startswith("UE_") and os.path.isdir(item.get("InstallLocation", "")):
                            found[app[3:]] = item["InstallLocation"]
            except (OSError, ValueError):
                pass
        for root in (os.environ.get("ProgramFiles", r"C:\Program Files"), r"D:\Program Files", r"C:\Epic Games", r"D:\Epic Games"):
            for path in glob.glob(os.path.join(root, "Epic Games", "UE_*")) + glob.glob(os.path.join(root, "UE_*")):
                found.setdefault(os.path.basename(path)[3:], path)
    elif is_mac():
        for path in glob.glob("/Users/Shared/Epic Games/UE_*"):
            found[os.path.basename(path)[3:]] = path
    else:
        for path in glob.glob(os.path.expanduser("~/UnrealEngine*")) + glob.glob("/opt/UnrealEngine*") + glob.glob(os.path.expanduser("~/Epic/UE_*")):
            found[os.path.basename(path)] = path
    # Source builds registered by the editor (Install.ini) on mac/linux
    install_ini = os.path.expanduser("~/.config/Epic/UnrealEngine/Install.ini") if not is_mac() else os.path.expanduser("~/Library/Application Support/Epic/UnrealEngine/Install.ini")
    if os.path.isfile(install_ini):
        try:
            with open(install_ini, "r", encoding="utf-8") as handle:
                for line in handle:
                    if "=" in line and not line.startswith("["):
                        key, value = line.strip().split("=", 1)
                        if os.path.isdir(value):
                            found[key] = value
        except OSError:
            pass
    # Only keep real engine roots
    return {version: path for version, path in found.items() if os.path.isdir(os.path.join(path, "Engine"))}


def engine_version_of(engine_root):
    version_file = os.path.join(engine_root, "Engine", "Build", "Build.version")
    try:
        with open(version_file, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        return f"{data.get('MajorVersion', 5)}.{data.get('MinorVersion', 0)}.{data.get('PatchVersion', 0)}"
    except (OSError, ValueError):
        return "unknown"


def engine_for_project(uproject, engines):
    try:
        with open(uproject, "r", encoding="utf-8") as handle:
            association = json.load(handle).get("EngineAssociation", "")
    except (OSError, ValueError):
        association = ""
    if association in engines:
        return engines[association]
    # "5.5" may be stored as "5.5" while the launcher lists "5.5"; also try prefix match
    for version, path in engines.items():
        if association and (version == association or version.startswith(association) or engine_version_of(path).startswith(association)):
            return path
    # source build relative path (EngineAssociation empty -> engine two dirs up)
    if not association:
        candidate = os.path.abspath(os.path.join(os.path.dirname(uproject), "..", ".."))
        if os.path.isdir(os.path.join(candidate, "Engine")):
            return candidate
    return None


def find_uprojects(start_dirs):
    results = []
    for base in start_dirs:
        if not base or not os.path.isdir(base):
            continue
        for path in glob.glob(os.path.join(base, "*.uproject")):
            results.append(os.path.abspath(path))
        for path in glob.glob(os.path.join(base, "*", "*.uproject")):
            results.append(os.path.abspath(path))
    return sorted(dict.fromkeys(results))


def find_python(engine_root=None):
    """Interpreter for the MCP server: current one, else python3/py, else UE's bundled Python."""
    if sys.executable and os.path.isfile(sys.executable):
        return sys.executable
    for name in ("python3", "python", "py"):
        path = shutil.which(name)
        if path:
            return path
    if engine_root:
        for sub in ("Win64/python.exe", "Mac/bin/python3", "Linux/bin/python3"):
            candidate = os.path.join(engine_root, "Engine", "Binaries", "ThirdParty", "Python3", *sub.split("/"))
            if os.path.isfile(candidate):
                return candidate
    return None


# ---------------------------------------------------------------- state

def load_state():
    try:
        with open(STATE_FILE, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return {"installs": []}


def save_state(state):
    os.makedirs(STATE_DIR, exist_ok=True)
    with open(STATE_FILE, "w", encoding="utf-8") as handle:
        json.dump(state, handle, indent=2)


def upsert_install(state, record):
    installs = [i for i in state.get("installs", []) if i.get("project") != record.get("project") or i.get("mode") != record.get("mode")]
    installs.append(record)
    state["installs"] = installs


# ---------------------------------------------------------------- steps

def copy_tree(src, dst):
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns("Binaries", "Intermediate", "__pycache__", ".DS_Store"))


def stamp(path, extra=None):
    data = {"version": read_version(), "sha": git_sha(), "installed": time.strftime("%Y-%m-%dT%H:%M:%S"), "repo": REPO_ROOT}
    if extra:
        data.update(extra)
    with open(os.path.join(path, ".claude-agent-install.json"), "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2)


def read_stamp(path):
    try:
        with open(os.path.join(path, ".claude-agent-install.json"), "r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return None


def install_plugin(uproject, engine_root, engine_install):
    src = os.path.join(REPO_ROOT, "unreal-plugin", PLUGIN_NAME)
    if engine_install:
        if not engine_root:
            raise SystemExit("Engine install requested but no engine found for this project.")
        dst = os.path.join(engine_root, "Engine", "Plugins", "Marketplace", PLUGIN_NAME)
    else:
        dst = os.path.join(os.path.dirname(uproject), "Plugins", PLUGIN_NAME)
    copy_tree(src, dst)
    stamp(dst, {"mode": "engine" if engine_install else "project"})
    Log.ok(f"plugin copied to {dst}")
    return dst


def enable_plugin_in_uproject(uproject, enabled=True):
    with open(uproject, "r", encoding="utf-8") as handle:
        text = handle.read()
    data = json.loads(text)
    plugins = data.setdefault("Plugins", [])
    entry = next((p for p in plugins if p.get("Name") == PLUGIN_NAME), None)
    if enabled:
        if entry is None:
            plugins.append({"Name": PLUGIN_NAME, "Enabled": True})
        else:
            entry["Enabled"] = True
    else:
        if entry is not None:
            plugins.remove(entry)
        if not plugins:
            data.pop("Plugins", None)
    with open(uproject, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent="\t")
        handle.write("\n")
    Log.ok(f"{'enabled' if enabled else 'removed'} {PLUGIN_NAME} in {os.path.basename(uproject)}")


def install_skill(scope_dir=None):
    dst = scope_dir or os.path.join(claude_home(), "skills", SKILL_NAME)
    copy_tree(os.path.join(REPO_ROOT, "skill"), dst)
    stamp(dst)
    Log.ok(f"skill installed to {dst}")
    return dst


def mcp_entry(python_exe, uproject):
    return {"command": python_exe, "args": [os.path.join(REPO_ROOT, "server", "run_server.py"), "--project", uproject]}


def configure_mcp(uproject, python_exe, scope):
    entry = mcp_entry(python_exe, uproject)
    if scope == "user":
        cli = find_claude_cli()
        if cli:
            subprocess.run([cli, "mcp", "remove", "-s", "user", MCP_NAME], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            result = subprocess.run([cli, "mcp", "add", "-s", "user", MCP_NAME, "--", entry["command"]] + entry["args"], capture_output=True, text=True)
            if result.returncode == 0:
                Log.ok(f"MCP server '{MCP_NAME}' registered (user scope via claude CLI)")
                return "user"
            Log.warn(f"claude mcp add failed: {result.stderr.strip()[:200]}")
        config_path = os.path.join(os.path.expanduser("~"), ".claude.json")
        data = {}
        if os.path.isfile(config_path):
            with open(config_path, "r", encoding="utf-8") as handle:
                data = json.load(handle)
        data.setdefault("mcpServers", {})[MCP_NAME] = entry
        with open(config_path, "w", encoding="utf-8") as handle:
            json.dump(data, handle, indent=2)
        Log.ok(f"MCP server '{MCP_NAME}' written to {config_path}")
        return "user"
    # project scope: <Project>/.mcp.json (shared with the team through source control)
    mcp_path = os.path.join(os.path.dirname(uproject), ".mcp.json")
    data = {}
    if os.path.isfile(mcp_path):
        try:
            with open(mcp_path, "r", encoding="utf-8") as handle:
                data = json.load(handle)
        except ValueError:
            data = {}
    data.setdefault("mcpServers", {})[MCP_NAME] = entry
    with open(mcp_path, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2)
        handle.write("\n")
    Log.ok(f"MCP server '{MCP_NAME}' written to {mcp_path}")
    return "project"


def remove_mcp(uproject, scope):
    if scope == "user":
        cli = find_claude_cli()
        if cli:
            subprocess.run([cli, "mcp", "remove", "-s", "user", MCP_NAME], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        config_path = os.path.join(os.path.expanduser("~"), ".claude.json")
        if os.path.isfile(config_path):
            try:
                with open(config_path, "r", encoding="utf-8") as handle:
                    data = json.load(handle)
                if MCP_NAME in data.get("mcpServers", {}):
                    del data["mcpServers"][MCP_NAME]
                    with open(config_path, "w", encoding="utf-8") as handle:
                        json.dump(data, handle, indent=2)
            except ValueError:
                pass
        return
    mcp_path = os.path.join(os.path.dirname(uproject), ".mcp.json")
    if os.path.isfile(mcp_path):
        try:
            with open(mcp_path, "r", encoding="utf-8") as handle:
                data = json.load(handle)
            data.get("mcpServers", {}).pop(MCP_NAME, None)
            if data.get("mcpServers"):
                with open(mcp_path, "w", encoding="utf-8") as handle:
                    json.dump(data, handle, indent=2)
                    handle.write("\n")
            else:
                os.remove(mcp_path)
        except ValueError:
            pass
    Log.ok("MCP configuration removed")


def write_project_config(uproject):
    agent_dir = os.path.join(os.path.dirname(uproject), ".unreal-agent")
    os.makedirs(agent_dir, exist_ok=True)
    config_path = os.path.join(agent_dir, "config.json")
    if not os.path.isfile(config_path):
        shutil.copyfile(os.path.join(REPO_ROOT, "unreal-agent.config.json"), config_path)
        Log.ok(f"project config created at {config_path}")
    gitignore = os.path.join(agent_dir, ".gitignore")
    if not os.path.isfile(gitignore):
        with open(gitignore, "w", encoding="utf-8") as handle:
            handle.write("cache/\n")
    return config_path


def build_plugin(uproject, engine_root):
    if not engine_root:
        Log.warn("no engine found; skipping build (the editor will offer to compile the plugin on first launch)")
        return False
    batch = os.path.join(engine_root, "Engine", "Build", "BatchFiles")
    if is_windows():
        script = [os.path.join(batch, "Build.bat")]
    elif is_mac():
        script = [os.path.join(batch, "Mac", "Build.sh")]
    else:
        script = [os.path.join(batch, "Linux", "Build.sh")]
    if not os.path.isfile(script[0]):
        Log.warn(f"build script not found: {script[0]}")
        return False
    project_name = os.path.splitext(os.path.basename(uproject))[0]
    # Blueprint-only projects build the stock editor target with -Project; C++ projects build <Project>Editor.
    has_source = os.path.isdir(os.path.join(os.path.dirname(uproject), "Source"))
    target = f"{project_name}Editor" if has_source else "UnrealEditor"
    plat = "Win64" if is_windows() else ("Mac" if is_mac() else "Linux")
    cmd = script + [target, plat, "Development", f"-Project={uproject}", "-WaitMutex", "-NoHotReload"]
    Log.step("Building plugin (this can take several minutes)...")
    Log.info(" ".join(cmd))
    result = subprocess.run(cmd)
    if result.returncode != 0:
        Log.warn("build failed; open the project in the editor and accept the rebuild prompt, or check Visual Studio/Xcode setup")
        return False
    Log.ok("plugin built")
    return True


# ---------------------------------------------------------------- commands

def cmd_install(args):
    Log.step(f"claude-unreal-blueprint-agent {read_version()} installer ({platform.system()})")
    cli = find_claude_cli()
    Log.info(f"Claude Code CLI: {cli or 'not found (skill/MCP files will still be written)'}")
    Log.info(f"Claude home: {claude_home()}")

    engines = find_engine_installs()
    if engines:
        for version, path in sorted(engines.items()):
            Log.info(f"Unreal Engine {version} ({engine_version_of(path)}): {path}")
    else:
        Log.warn("no Unreal Engine installation detected (you can still install; the editor compiles the plugin on launch)")

    uproject = args.project
    if uproject and os.path.isdir(uproject):
        candidates = find_uprojects([uproject])
        uproject = candidates[0] if len(candidates) == 1 else choose("Select the Unreal project:", candidates, args.yes)
    if not uproject:
        candidates = find_uprojects([os.getcwd(), os.path.dirname(os.getcwd()), os.path.join(os.path.expanduser("~"), "Documents", "Unreal Projects"), os.path.join(os.path.expanduser("~"), "Unreal Projects")])
        uproject = choose("Select the Unreal project:", candidates, args.yes)
    if not uproject or not os.path.isfile(uproject):
        raise SystemExit("No .uproject found. Re-run with --project <path-to.uproject>.")
    uproject = os.path.abspath(uproject)
    Log.step(f"Project: {uproject}")

    engine_root = engine_for_project(uproject, engines)
    Log.info(f"Engine for project: {engine_root or 'unknown'}")
    python_exe = args.python or find_python(engine_root)
    if not python_exe:
        raise SystemExit("No Python interpreter found. Install Python 3.9+ or pass --python <path>.")
    Log.info(f"Python for MCP server: {python_exe}")

    mode = "engine" if args.engine else "project"
    scope = args.scope
    if not args.yes:
        Log.info(f"Plan: plugin -> {mode} install, skill -> {os.path.join(claude_home(), 'skills', SKILL_NAME)}, MCP -> {scope} scope")
        if not confirm("Proceed?", True, args.yes):
            raise SystemExit("Aborted.")

    Log.step("Installing plugin")
    plugin_dir = install_plugin(uproject, engine_root, mode == "engine")
    enable_plugin_in_uproject(uproject, True)
    Log.step("Installing skill")
    skill_dir = install_skill()
    Log.step("Configuring MCP")
    mcp_scope = configure_mcp(uproject, python_exe, scope)
    Log.step("Project configuration")
    write_project_config(uproject)
    built = False
    if args.build:
        built = build_plugin(uproject, engine_root)

    state = load_state()
    upsert_install(state, {"project": uproject, "mode": mode, "plugin_dir": plugin_dir, "skill_dir": skill_dir, "mcp_scope": mcp_scope, "python": python_exe,
                           "engine": engine_root, "version": read_version(), "sha": git_sha(), "installed": time.strftime("%Y-%m-%dT%H:%M:%S"), "repo": REPO_ROOT})
    save_state(state)

    Log.step("Done")
    print("""
Next steps:
  1. Open the project in Unreal Editor. If asked to rebuild the ClaudeBlueprintAgent module, accept (needs Visual Studio / Xcode / clang).
  2. Check the Output Log for: "Claude Blueprint Agent listening on http://127.0.0.1:<port>/rpc".
  3. Start Claude Code in the project folder (project-scope MCP) or anywhere (user scope) and ask, e.g.:
       "Revisa WBP_MainMenu y agrega un boton Credits igual a los demas."
  4. First session in a project: run the index once ("index the project deep") for full function/variable search coverage.
""")
    return 0


def cmd_update(args):
    Log.step("Updating repository")
    if os.path.isdir(os.path.join(REPO_ROOT, ".git")) and not args.no_pull:
        result = subprocess.run(["git", "-C", REPO_ROOT, "pull", "--ff-only"], capture_output=True, text=True)
        Log.info(result.stdout.strip() or result.stderr.strip())
    state = load_state()
    if not state.get("installs"):
        raise SystemExit("Nothing installed yet. Run install first.")
    version = read_version()
    for record in state["installs"]:
        uproject = record["project"]
        if not os.path.isfile(uproject):
            Log.warn(f"project missing, skipping: {uproject}")
            continue
        Log.step(f"Updating {os.path.basename(uproject)}")
        plugin_stamp = read_stamp(record["plugin_dir"]) or {}
        if plugin_stamp.get("sha") != git_sha() or plugin_stamp.get("version") != version or not os.path.isdir(record["plugin_dir"]):
            engines = find_engine_installs()
            install_plugin(uproject, engine_for_project(uproject, engines), record.get("mode") == "engine")
            if args.build:
                build_plugin(uproject, engine_for_project(uproject, engines))
        else:
            Log.info("plugin already up to date")
        install_skill(record.get("skill_dir"))
        configure_mcp(uproject, record.get("python") or find_python(record.get("engine")), record.get("mcp_scope", "project"))
        record["version"] = version
        record["sha"] = git_sha()
        record["updated"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    save_state(state)
    Log.step("Update complete. Restart Unreal Editor and Claude Code.")
    return 0


def cmd_uninstall(args):
    state = load_state()
    installs = state.get("installs", [])
    if args.project:
        installs = [i for i in installs if os.path.abspath(i["project"]) == os.path.abspath(args.project)]
    if not installs:
        Log.warn("no recorded installations; trying to clean the given project anyway" if args.project else "no recorded installations")
        if args.project and os.path.isfile(args.project):
            installs = [{"project": os.path.abspath(args.project), "plugin_dir": os.path.join(os.path.dirname(os.path.abspath(args.project)), "Plugins", PLUGIN_NAME), "mode": "project", "mcp_scope": "project", "skill_dir": os.path.join(claude_home(), "skills", SKILL_NAME)}]
        else:
            return 0
    for record in installs:
        uproject = record["project"]
        Log.step(f"Uninstalling from {os.path.basename(uproject)}")
        if os.path.isdir(record.get("plugin_dir", "")):
            shutil.rmtree(record["plugin_dir"])
            Log.ok(f"removed {record['plugin_dir']}")
            parent = os.path.dirname(record["plugin_dir"])
            if os.path.basename(parent) == "Plugins" and os.path.isdir(parent) and not os.listdir(parent):
                os.rmdir(parent)
        if os.path.isfile(uproject):
            enable_plugin_in_uproject(uproject, False)
            remove_mcp(uproject, record.get("mcp_scope", "project"))
            agent_dir = os.path.join(os.path.dirname(uproject), ".unreal-agent")
            if args.purge and os.path.isdir(agent_dir):
                shutil.rmtree(agent_dir)
                Log.ok("removed .unreal-agent (purge)")
            else:
                Log.info(".unreal-agent/ kept (project conventions/config); use --purge to delete")
    remaining = [i for i in state.get("installs", []) if i not in installs]
    if not remaining:
        skill_dir = os.path.join(claude_home(), "skills", SKILL_NAME)
        if os.path.isdir(skill_dir):
            shutil.rmtree(skill_dir)
            Log.ok(f"removed skill {skill_dir}")
    state["installs"] = remaining
    save_state(state)
    Log.step("Uninstall complete. No project assets were touched.")
    return 0


def cmd_doctor(args):
    Log.step("Doctor")
    Log.info(f"repo: {REPO_ROOT} ({read_version()} {git_sha()})")
    Log.info(f"python: {sys.executable} {platform.python_version()}")
    Log.info(f"claude CLI: {find_claude_cli() or 'not found'}")
    engines = find_engine_installs()
    Log.info(f"engines: {', '.join(f'{v} -> {p}' for v, p in engines.items()) or 'none'}")
    state = load_state()
    for record in state.get("installs", []):
        uproject = record["project"]
        ok_project = os.path.isfile(uproject)
        plugin_ok = os.path.isfile(os.path.join(record.get("plugin_dir", ""), f"{PLUGIN_NAME}.uplugin"))
        skill_ok = os.path.isfile(os.path.join(record.get("skill_dir", ""), "SKILL.md"))
        endpoint = os.path.join(os.path.dirname(uproject), "Saved", "ClaudeAgent", "endpoint.json")
        editor = "running" if os.path.isfile(endpoint) else "not running / never started"
        Log.info(f"{os.path.basename(uproject)}: project {'ok' if ok_project else 'MISSING'}, plugin {'ok' if plugin_ok else 'MISSING'}, skill {'ok' if skill_ok else 'MISSING'}, editor {editor}")
        if os.path.isfile(endpoint):
            try:
                import urllib.request
                with open(endpoint, "r", encoding="utf-8") as handle:
                    info = json.load(handle)
                with urllib.request.urlopen(f"http://127.0.0.1:{info['port']}/health", timeout=3) as response:
                    Log.ok(f"plugin reachable: {response.read().decode('utf-8')[:120]}")
            except Exception as exc:  # noqa: BLE001
                Log.warn(f"plugin not reachable ({exc})")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="claude-unreal-blueprint-agent installer")
    sub = parser.add_subparsers(dest="command")
    p_install = sub.add_parser("install")
    p_install.add_argument("--project", help=".uproject file or its folder")
    p_install.add_argument("--engine", action="store_true", help="install the plugin into the engine (global) instead of the project")
    p_install.add_argument("--scope", choices=("project", "user"), default="project", help="MCP configuration scope (default project)")
    p_install.add_argument("--build", action="store_true", help="compile the plugin now with UnrealBuildTool")
    p_install.add_argument("--python", help="interpreter for the MCP server")
    p_install.add_argument("--yes", "-y", action="store_true", help="non-interactive")
    p_update = sub.add_parser("update")
    p_update.add_argument("--no-pull", action="store_true")
    p_update.add_argument("--build", action="store_true")
    p_uninstall = sub.add_parser("uninstall")
    p_uninstall.add_argument("--project")
    p_uninstall.add_argument("--purge", action="store_true", help="also delete <Project>/.unreal-agent")
    sub.add_parser("doctor")
    args = parser.parse_args(argv)
    if not args.command:
        parser.print_help()
        return 1
    return {"install": cmd_install, "update": cmd_update, "uninstall": cmd_uninstall, "doctor": cmd_doctor}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
