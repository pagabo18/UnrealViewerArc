"""Configuration: defaults <- <project>/.unreal-agent/config.json <- env <- CLI."""
import json
import logging
import os
from dataclasses import dataclass, field
from typing import List, Optional

log = logging.getLogger(__name__)

DEFAULTS = {
    "responseMode": "compact",      # compact | normal | debug
    "maxResults": 20,
    "cache": True,
    "autoCompile": True,
    "autoSave": False,
    "visualValidation": True,
    "autoDeepIndex": True,
    "maxAutoDeepIndex": 1000,
    "indexRefreshSeconds": 15,
    "contentRoots": ["/Game"],
    "plugin": {"port": 8766, "timeoutSeconds": 180},
}


def find_uproject(start: Optional[str]) -> Optional[str]:
    """Locate a .uproject from a file path, a directory, or by walking up from cwd."""
    candidates = []
    if start:
        start = os.path.abspath(os.path.expanduser(start))
        if os.path.isfile(start) and start.lower().endswith(".uproject"):
            return start
        candidates.append(start)
    else:
        candidates.append(os.getcwd())
    for base in candidates:
        current = base
        for _ in range(6):
            if os.path.isdir(current):
                for name in sorted(os.listdir(current)):
                    if name.lower().endswith(".uproject"):
                        return os.path.join(current, name)
            parent = os.path.dirname(current)
            if parent == current:
                break
            current = parent
    return None


@dataclass
class AgentConfig:
    project_file: Optional[str]
    project_dir: str
    response_mode: str = "compact"
    max_results: int = 20
    cache: bool = True
    auto_compile: bool = True
    auto_save: bool = False
    visual_validation: bool = True
    auto_deep_index: bool = True
    max_auto_deep_index: int = 1000
    index_refresh_seconds: int = 15
    content_roots: List[str] = field(default_factory=lambda: ["/Game"])
    port: Optional[int] = None
    token: Optional[str] = None
    timeout: int = 180
    raw: dict = field(default_factory=dict)

    @property
    def agent_dir(self) -> str:
        return os.path.join(self.project_dir, ".unreal-agent")

    @property
    def cache_dir(self) -> str:
        return os.path.join(self.agent_dir, "cache")

    @property
    def endpoint_file(self) -> str:
        return os.path.join(self.project_dir, "Saved", "ClaudeAgent", "endpoint.json")

    @property
    def project_name(self) -> str:
        if self.project_file:
            return os.path.splitext(os.path.basename(self.project_file))[0]
        return os.path.basename(self.project_dir)

    @classmethod
    def load(cls, project: Optional[str] = None, port: Optional[int] = None, token: Optional[str] = None) -> "AgentConfig":
        project = project or os.environ.get("CLAUDE_AGENT_PROJECT")
        uproject = find_uproject(project)
        if uproject:
            project_dir = os.path.dirname(uproject)
        else:
            project_dir = os.path.abspath(project) if project and os.path.isdir(project) else os.getcwd()
            log.warning("No .uproject found (searched from %s); using %s", project or os.getcwd(), project_dir)
        data = dict(DEFAULTS)
        config_path = os.path.join(project_dir, ".unreal-agent", "config.json")
        if os.path.isfile(config_path):
            try:
                with open(config_path, "r", encoding="utf-8") as handle:
                    loaded = json.load(handle)
                for key, value in loaded.items():
                    if key == "plugin" and isinstance(value, dict):
                        merged = dict(data["plugin"])
                        merged.update(value)
                        data["plugin"] = merged
                    else:
                        data[key] = value
            except (OSError, ValueError) as exc:
                log.warning("Could not read %s: %s", config_path, exc)
        env_port = os.environ.get("CLAUDE_AGENT_PORT")
        env_token = os.environ.get("CLAUDE_AGENT_TOKEN")
        plugin = data.get("plugin", {})
        return cls(
            project_file=uproject,
            project_dir=project_dir,
            response_mode=str(data.get("responseMode", "compact")),
            max_results=int(data.get("maxResults", 20)),
            cache=bool(data.get("cache", True)),
            auto_compile=bool(data.get("autoCompile", True)),
            auto_save=bool(data.get("autoSave", False)),
            visual_validation=bool(data.get("visualValidation", True)),
            auto_deep_index=bool(data.get("autoDeepIndex", True)),
            max_auto_deep_index=int(data.get("maxAutoDeepIndex", 1000)),
            index_refresh_seconds=int(data.get("indexRefreshSeconds", 15)),
            content_roots=list(data.get("contentRoots", ["/Game"])),
            port=port or (int(env_port) if env_port else None) or (int(plugin["port"]) if "port" in loaded_keys(config_path) else None),
            token=token or env_token or plugin.get("token"),
            timeout=int(plugin.get("timeoutSeconds", 180)),
            raw=data,
        )


def loaded_keys(config_path: str):
    """Keys explicitly present in the plugin section of the config file (to distinguish defaults)."""
    try:
        with open(config_path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        return set((data.get("plugin") or {}).keys())
    except (OSError, ValueError):
        return set()
