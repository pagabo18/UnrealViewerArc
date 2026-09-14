#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/_bootstrap.sh"
PY="${AGENT_PYTHON:-$(find_agent_python || true)}"
exec "$PY" "$HERE/lib/installer.py" doctor
