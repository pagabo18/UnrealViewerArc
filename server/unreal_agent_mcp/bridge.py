"""HTTP client for the ClaudeBlueprintAgent editor plugin (stdlib only)."""
import json
import logging
import os
import time
import urllib.error
import urllib.request
from typing import Any, Dict, Optional

log = logging.getLogger(__name__)


class PluginError(Exception):
    """The plugin executed the command and reported a failure."""

    def __init__(self, code: str, message: str, details: Optional[dict] = None, cmd: str = ""):
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details or {}
        self.cmd = cmd


class PluginUnavailable(Exception):
    """The editor/plugin could not be reached."""


class PluginClient:
    def __init__(self, project_dir: str, endpoint_file: str, port: Optional[int] = None, token: Optional[str] = None, timeout: int = 180):
        self.project_dir = project_dir
        self.endpoint_file = endpoint_file
        self.fixed_port = port
        self.fixed_token = token
        self.timeout = timeout
        self.port: Optional[int] = port
        self.token: Optional[str] = token
        self.endpoint_info: Dict[str, Any] = {}
        self.last_seq = 0
        self.calls = 0
        self.total_ms = 0.0
        self._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # never go through proxies for localhost

    # ---- discovery -----------------------------------------------------
    def discover(self) -> bool:
        info = {}
        try:
            with open(self.endpoint_file, "r", encoding="utf-8") as handle:
                info = json.load(handle)
        except (OSError, ValueError):
            info = {}
        self.endpoint_info = info
        self.port = self.fixed_port or info.get("port") or self.port or 8766
        self.token = self.fixed_token or info.get("token") or self.token
        return bool(info) or self.fixed_port is not None

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port or 8766}"

    # ---- calls ---------------------------------------------------------
    def call(self, cmd: str, params: Optional[dict] = None, timeout: Optional[int] = None) -> Dict[str, Any]:
        if self.port is None or self.token is None:
            self.discover()
        payload = json.dumps({"id": self.calls + 1, "cmd": cmd, "params": params or {}}).encode("utf-8")
        attempts = 0
        while True:
            attempts += 1
            request = urllib.request.Request(self.url + "/rpc", data=payload, method="POST")
            request.add_header("Content-Type", "application/json")
            if self.token:
                request.add_header("X-Agent-Token", self.token)
            started = time.time()
            try:
                with self._opener.open(request, timeout=timeout or self.timeout) as response:
                    body = response.read().decode("utf-8")
                break
            except urllib.error.HTTPError as exc:
                body = exc.read().decode("utf-8", errors="replace")
                if exc.code == 401 and attempts == 1:
                    # Token rotated (editor restarted): re-read the endpoint file once.
                    self.discover()
                    continue
                raise PluginUnavailable(f"Plugin returned HTTP {exc.code}: {body[:200]}")
            except (urllib.error.URLError, ConnectionError, TimeoutError, OSError) as exc:
                if attempts == 1 and self.discover():
                    continue
                raise PluginUnavailable(
                    f"Unreal Editor not reachable at {self.url} ({exc}). Open the project in the editor with the "
                    f"ClaudeBlueprintAgent plugin enabled (endpoint file: {self.endpoint_file})."
                )
        elapsed = (time.time() - started) * 1000.0
        self.calls += 1
        self.total_ms += elapsed
        try:
            data = json.loads(body)
        except ValueError:
            raise PluginUnavailable(f"Plugin returned non-JSON for {cmd}: {body[:200]}")
        if "seq" in data:
            try:
                self.last_seq = int(data["seq"])
            except (TypeError, ValueError):
                pass
        if not data.get("ok"):
            error = data.get("error") or {}
            raise PluginError(error.get("code", "UNKNOWN"), error.get("message", "unknown error"), error.get("details"), cmd=cmd)
        log.debug("%s took %.1f ms", cmd, elapsed)
        return data.get("result") or {}

    def health(self) -> Dict[str, Any]:
        if self.port is None:
            self.discover()
        request = urllib.request.Request(self.url + "/health", method="GET")
        try:
            with self._opener.open(request, timeout=5) as response:
                return json.loads(response.read().decode("utf-8"))
        except Exception as exc:  # noqa: BLE001 - any failure means "down"
            raise PluginUnavailable(str(exc))

    def is_available(self) -> bool:
        try:
            self.health()
            return True
        except PluginUnavailable:
            return False
