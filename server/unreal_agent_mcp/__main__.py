"""Command line entry: `python -m unreal_agent_mcp --project <path>`."""
import argparse
import logging
import os
import sys

from . import VERSION
from .config import AgentConfig
from .protocol import StdioServer
from .context import AgentContext
from .tools import build_registry


def parse_args(argv=None):
    parser = argparse.ArgumentParser(prog="unreal-agent-mcp", description="MCP server bridging Claude and the Unreal Editor plugin.")
    parser.add_argument("--project", help="Path to the .uproject (or its directory). Defaults to CLAUDE_AGENT_PROJECT or the current directory.")
    parser.add_argument("--port", type=int, help="Plugin HTTP port (overrides endpoint discovery).")
    parser.add_argument("--token", help="Plugin token (overrides endpoint discovery).")
    parser.add_argument("--log-level", default=os.environ.get("CLAUDE_AGENT_LOG_LEVEL", "INFO"))
    parser.add_argument("--version", action="version", version=VERSION)
    parser.add_argument("--selftest", action="store_true", help="Print tool list and exit (no editor needed).")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    logging.basicConfig(level=getattr(logging, args.log_level.upper(), logging.INFO), stream=sys.stderr,
                        format="[unreal-agent] %(levelname)s %(message)s")
    config = AgentConfig.load(args.project, port=args.port, token=args.token)
    context = AgentContext(config)
    registry = build_registry()
    if args.selftest:
        for tool in registry.tools():
            print(f"{tool.name}: {tool.description}")
        return 0
    server = StdioServer(registry, context)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
