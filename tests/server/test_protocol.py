import io
import json

from helpers import make_context
from unreal_agent_mcp.protocol import StdioServer
from unreal_agent_mcp.tools import build_registry


def test_initialize_and_list_tools():
    ctx, _ = make_context()
    server = StdioServer(build_registry(), ctx)
    response = server.handle({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2024-11-05", "capabilities": {}}})
    assert response["result"]["protocolVersion"] == "2024-11-05"
    assert response["result"]["serverInfo"]["name"] == "unreal-blueprint-agent"
    assert server.handle({"jsonrpc": "2.0", "method": "notifications/initialized"}) is None
    tools = server.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})["result"]["tools"]
    names = {tool["name"] for tool in tools}
    for required in ("search", "inspect_blueprint", "inspect_graph", "add_node", "connect", "clone_widget", "compile_blueprint", "batch", "get_capabilities"):
        assert required in names
    for tool in tools:
        assert tool["inputSchema"]["type"] == "object"
        assert "annotations" in tool


def test_unknown_method_and_tool():
    ctx, _ = make_context()
    server = StdioServer(build_registry(), ctx)
    error = server.handle({"jsonrpc": "2.0", "id": 5, "method": "nope"})
    assert error["error"]["code"] == -32601
    result = server.handle({"jsonrpc": "2.0", "id": 6, "method": "tools/call", "params": {"name": "nope", "arguments": {}}})["result"]
    assert result["isError"]


def test_stdio_roundtrip():
    ctx, _ = make_context()
    stdin = io.StringIO(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"}) + "\n" + json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "session_info", "arguments": {}}}) + "\n")
    stdout = io.StringIO()
    server = StdioServer(build_registry(), ctx, stdin=stdin, stdout=stdout)
    server.serve_forever()
    lines = [json.loads(line) for line in stdout.getvalue().strip().split("\n")]
    assert lines[0]["result"] == {}
    assert "Project:" in lines[1]["result"]["content"][0]["text"]


def test_missing_required_argument_is_reported_not_crashed():
    ctx, _ = make_context()
    server = StdioServer(build_registry(), ctx)
    result = server.handle({"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "inspect_blueprint", "arguments": {}}})["result"]
    assert result["isError"]
    assert "asset" in result["content"][0]["text"]
