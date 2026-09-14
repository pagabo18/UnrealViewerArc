#!/usr/bin/env bash
# macOS/Linux installer: ./scripts/install.sh [--project <path.uproject>] [--engine] [--scope project|user] [--build] [--yes]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/_bootstrap.sh"
PY="${AGENT_PYTHON:-$(find_agent_python || true)}"
if [ -z "$PY" ]; then echo "Python 3.9+ not found. Install python3 and retry." >&2; exit 1; fi
exec "$PY" "$HERE/lib/installer.py" install --python "$PY" "$@"
