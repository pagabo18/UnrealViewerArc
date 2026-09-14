import os
import tempfile

from unreal_agent_mcp.config import AgentConfig
from unreal_agent_mcp.context import AgentContext
from unreal_agent_mcp.protocol import StdioServer
from unreal_agent_mcp.tools import build_registry

from fake_plugin import FakePlugin, make_large_project, make_menu_project


def make_context(project=None, tmpdir=None, **overrides):
    tmpdir = tmpdir or tempfile.mkdtemp(prefix="unreal-agent-test-")
    os.makedirs(os.path.join(tmpdir, "Content"), exist_ok=True)
    with open(os.path.join(tmpdir, "TestProject.uproject"), "w", encoding="utf-8") as handle:
        handle.write('{"FileVersion": 3, "EngineAssociation": "5.5"}')
    config = AgentConfig.load(tmpdir)
    for key, value in overrides.items():
        setattr(config, key, value)
    plugin = FakePlugin(project or make_menu_project())
    ctx = AgentContext(config, client=plugin)
    return ctx, plugin


class ToolRunner:
    """Runs tools through the MCP server exactly like a client would."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.server = StdioServer(build_registry(), ctx)
        self.next_id = 1
        self.outputs = []
        self.server.handle({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}})
        self.server.handle({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def call(self, _tool, **args):
        response = self.server.handle({"jsonrpc": "2.0", "id": self.next_id, "method": "tools/call", "params": {"name": _tool, "arguments": args}})
        self.next_id += 1
        result = response["result"]
        text = result["content"][0]["text"]
        self.outputs.append((_tool, text, bool(result.get("isError"))))
        return text

    def ok(self, _tool, **args):
        text = self.call(_tool, **args)
        assert not self.outputs[-1][2], f"{_tool} failed:\n{text}"
        return text

    def fail(self, _tool, **args):
        text = self.call(_tool, **args)
        assert self.outputs[-1][2], f"{_tool} unexpectedly succeeded:\n{text}"
        return text

    def total_tokens(self):
        from unreal_agent_mcp.tokens import estimate_tokens
        return sum(estimate_tokens(text) for _, text, _ in self.outputs)
