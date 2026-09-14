#!/usr/bin/env bash
# Runs the plugin's editor automation tests headless (macOS/Linux).
# Usage: tests/unreal/run_editor_tests.sh /path/MyGame.uproject [/path/to/UE_5.5] [Filter]
set -euo pipefail
PROJECT="$1"; ENGINE="${2:-}"; FILTER="${3:-ClaudeBlueprintAgent}"
if [ -z "$ENGINE" ]; then
  ASSOC=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('EngineAssociation',''))" "$PROJECT")
  for c in "/Users/Shared/Epic Games/UE_$ASSOC" "$HOME/UnrealEngine"; do [ -d "$c" ] && ENGINE="$c" && break; done
fi
if [ "$(uname)" = "Darwin" ]; then EDITOR="$ENGINE/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"; else EDITOR="$ENGINE/Engine/Binaries/Linux/UnrealEditor"; fi
REPORT="$(dirname "$PROJECT")/Saved/ClaudeAgent/AutomationReport"
"$EDITOR" "$PROJECT" -ExecCmds="Automation RunTests $FILTER; Quit" -unattended -nopause -nosplash -NullRHI -log -ReportExportPath="$REPORT" -testexit="Automation Test Queue Empty"
echo "Report: $REPORT"
