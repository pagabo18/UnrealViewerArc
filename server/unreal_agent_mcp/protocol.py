"""Minimal MCP (Model Context Protocol) server over stdio: newline-delimited
JSON-RPC 2.0. Implements initialize, ping, tools/list and tools/call. No
third-party packages so it runs on any Python >= 3.9, including the one that
ships with Unreal Engine."""
import json
import logging
import sys
import traceback
from typing import Any, Dict, Optional

from . import PROTOCOL_VERSIONS, VERSION
from .bridge import PluginError, PluginUnavailable
from .formatters import format_plugin_error, format_unavailable

log = logging.getLogger(__name__)


class ToolError(Exception):
    """Raised by tool handlers for user-facing errors (rendered compactly)."""


class StdioServer:
    def __init__(self, registry, context, stdin=None, stdout=None):
        self.registry = registry
        self.context = context
        self.stdin = stdin or sys.stdin
        self.stdout = stdout or sys.stdout
        self.initialized = False

    # ---- transport ----------------------------------------------------------
    def serve_forever(self):
        for line in self.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except ValueError:
                self._write({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}})
                continue
            response = self.handle(message)
            if response is not None:
                self._write(response)

    def _write(self, message: Dict[str, Any]):
        self.stdout.write(json.dumps(message, ensure_ascii=False) + "\n")
        self.stdout.flush()

    # ---- dispatch -----------------------------------------------------------
    def handle(self, message: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        method = message.get("method")
        msg_id = message.get("id")
        params = message.get("params") or {}
        is_notification = "id" not in message
        try:
            if method == "initialize":
                result = self.initialize(params)
            elif method == "notifications/initialized":
                self.initialized = True
                return None
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                result = {"tools": [tool.to_mcp() for tool in self.registry.tools()]}
            elif method == "tools/call":
                result = self.call_tool(params)
            elif method in ("resources/list", "resources/templates/list"):
                result = {"resources": []} if method == "resources/list" else {"resourceTemplates": []}
            elif method == "prompts/list":
                result = {"prompts": []}
            elif method and method.startswith("notifications/"):
                return None
            else:
                if is_notification:
                    return None
                return {"jsonrpc": "2.0", "id": msg_id, "error": {"code": -32601, "message": f"Method not found: {method}"}}
        except Exception as exc:  # noqa: BLE001 - protocol-level failure
            log.error("Unhandled error in %s: %s\n%s", method, exc, traceback.format_exc())
            if is_notification:
                return None
            return {"jsonrpc": "2.0", "id": msg_id, "error": {"code": -32603, "message": str(exc)}}
        if is_notification:
            return None
        return {"jsonrpc": "2.0", "id": msg_id, "result": result}

    def initialize(self, params: Dict[str, Any]) -> Dict[str, Any]:
        requested = params.get("protocolVersion")
        version = requested if requested in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0]
        return {
            "protocolVersion": version,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "unreal-blueprint-agent", "version": VERSION},
            "instructions": (
                "Unreal Editor bridge. Start with search() or inspect_blueprint(detail='summary'); expand only what you need "
                "(inspect_graph with around/depth, inspect_widget_tree). Use short ids (A#, N#, WS#) returned by tools. "
                "Edits return diffs; compile_blueprint after editing. Never claim an edit succeeded without a tool result."
            ),
        }

    def call_tool(self, params: Dict[str, Any]) -> Dict[str, Any]:
        name = params.get("name", "")
        arguments = params.get("arguments") or {}
        tool = self.registry.get(name)
        if tool is None:
            return {"content": [{"type": "text", "text": f"Unknown tool: {name}"}], "isError": True}
        try:
            text = tool.run(self.context, arguments)
            is_error = False
        except ToolError as exc:
            text = f"FAILED\n{exc}"
            is_error = True
        except PluginError as exc:
            text = format_plugin_error(exc)
            is_error = True
        except PluginUnavailable as exc:
            text = format_unavailable(exc, self.context)
            is_error = True
        except (KeyError, ValueError) as exc:
            text = f"FAILED\n{exc}"
            is_error = True
        except Exception as exc:  # noqa: BLE001
            log.error("Tool %s crashed: %s\n%s", name, exc, traceback.format_exc())
            text = f"FAILED\nInternal error in {name}: {exc}"
            is_error = True
        if not isinstance(text, str):
            text = json.dumps(text, ensure_ascii=False)
        if self.context.config.response_mode == "debug":
            from .tokens import estimate_tokens
            text += f"\n[~{estimate_tokens(text)} tokens]"
        self.context.meter.record(name, text)
        result: Dict[str, Any] = {"content": [{"type": "text", "text": text}]}
        if is_error:
            result["isError"] = True
        return result
