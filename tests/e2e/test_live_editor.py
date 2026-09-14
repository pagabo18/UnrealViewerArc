"""End-to-end test against a RUNNING Unreal Editor with the plugin enabled.

Skipped unless CLAUDE_AGENT_PROJECT points at a .uproject whose editor is open
(Saved/ClaudeAgent/endpoint.json present and reachable). Creates and deletes
assets under /Game/__ClaudeAgentTests.
"""
import os

import pytest

from unreal_agent_mcp.bridge import PluginClient, PluginUnavailable
from unreal_agent_mcp.config import AgentConfig
from unreal_agent_mcp.context import AgentContext
from unreal_agent_mcp.protocol import StdioServer
from unreal_agent_mcp.tools import build_registry

PROJECT = os.environ.get("CLAUDE_AGENT_PROJECT")
TEST_ASSET = "/Game/__ClaudeAgentTests/BP_E2E"
TEST_WIDGET = "/Game/__ClaudeAgentTests/WBP_E2E"


def _context():
    config = AgentConfig.load(PROJECT)
    ctx = AgentContext(config)
    if not ctx.client.is_available():
        pytest.skip("Unreal Editor with ClaudeBlueprintAgent is not running")
    return ctx


pytestmark = pytest.mark.skipif(not PROJECT, reason="set CLAUDE_AGENT_PROJECT to run live editor tests")


class Runner:
    def __init__(self, ctx):
        self.server = StdioServer(build_registry(), ctx)
        self.ctx = ctx
        self.id = 0

    def call(self, tool, **args):
        self.id += 1
        result = self.server.handle({"jsonrpc": "2.0", "id": self.id, "method": "tools/call", "params": {"name": tool, "arguments": args}})["result"]
        text = result["content"][0]["text"]
        assert not result.get("isError"), f"{tool} failed:\n{text}"
        return text


def test_live_blueprint_roundtrip():
    ctx = _context()
    runner = Runner(ctx)
    ctx.client.call("plugin_raw", {}) if False else None
    try:
        runner.call("create_blueprint", path=TEST_ASSET, parent="Actor")
        summary = runner.call("inspect_blueprint", asset=TEST_ASSET)
        assert "parent Actor" in summary
        out = runner.call("blueprint_variable", asset=TEST_ASSET, op="add", name="Stamina", type="float", default="100", category="Stats")
        assert "+ Variable Stamina : float" in out and "Compile 1/1 OK" in out
        fn = runner.call("blueprint_function", asset=TEST_ASSET, op="create", name="RegenStamina", inputs=["DeltaTime:float"], compile=False)
        entry = next(tok for tok in fn.replace("\n", " ").split() if tok.startswith("N") and tok[1:].isdigit())
        node = runner.call("add_node", asset=TEST_ASSET, graph="RegenStamina", type="variable_set", variable="Stamina", after=entry, compile=False)
        assert f"{entry} -> N" in node
        compiled = runner.call("compile_blueprint", assets=[TEST_ASSET], save=True, validate=True)
        assert "Compile 1/1 OK" in compiled and "saved" in compiled
        graph = runner.call("inspect_graph", asset=TEST_ASSET, graph="RegenStamina")
        assert "Set Stamina" in graph
        runner.call("undo")
    finally:
        ctx.client.call("assets.info", {"asset": TEST_ASSET})
        from unreal_agent_mcp.bridge import PluginError
        try:
            ctx.client.call("blueprint.reload", {"asset": TEST_ASSET})
        except PluginError:
            pass


def test_live_widget_roundtrip():
    ctx = _context()
    runner = Runner(ctx)
    runner.call("create_blueprint", path=TEST_WIDGET, parent="UserWidget", kind="widget")
    runner.call("add_widget", asset=TEST_WIDGET, **{"class": "VerticalBox"}, name="VB_Menu", parent="CanvasPanel", compile=False)
    runner.call("add_widget", asset=TEST_WIDGET, **{"class": "Button"}, name="BTN_Play", parent="VB_Menu", compile=False)
    runner.call("add_widget", asset=TEST_WIDGET, **{"class": "TextBlock"}, name="TXT_Play", parent="BTN_Play", properties={"Text": "PLAY"}, compile=False)
    clone = runner.call("clone_widget", asset=TEST_WIDGET, source="BTN_Play", new_name="BTN_Credits", insert_after="BTN_Play", children={"TXT_Play": {"Text": "CREDITS"}}, compile=False)
    assert "BTN_Credits" in clone and "TXT_Credits" in clone
    tree = runner.call("inspect_widget_tree", asset=TEST_WIDGET)
    assert 'TXT_Credits TextBlock "CREDITS"' in tree
    bind = runner.call("bind_widget_event", asset=TEST_WIDGET, widget="BTN_Credits", event="OnClicked", function="OpenCredits", create_function=True)
    assert "OnClicked" in bind
    compiled = runner.call("compile_blueprint", assets=[TEST_WIDGET], save=True)
    assert "Compile 1/1 OK" in compiled
    preview = runner.call("preview_widget", asset=TEST_WIDGET, width=800, height=600, check=["BTN_Play", "BTN_Credits"])
    assert "BTN_Credits" in preview
