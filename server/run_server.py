#!/usr/bin/env python3
"""Entry point used by the MCP configuration.

Runs with the Python interpreter the installer picked (system Python or the one
bundled with Unreal Engine); no third-party packages are required.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from unreal_agent_mcp.__main__ import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
