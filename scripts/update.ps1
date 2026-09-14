# Windows updater: git pull + re-copy plugin/skill + refresh MCP config
param([switch]$NoPull, [switch]$Build, [string]$Python)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_bootstrap.ps1"
$py = if ($Python) { $Python } else { Find-AgentPython }
if (-not $py) { Write-Error "Python 3.9+ not found."; exit 1 }
$argsList = @("$PSScriptRoot\lib\installer.py", "update")
if ($NoPull) { $argsList += "--no-pull" }
if ($Build) { $argsList += "--build" }
& $py @argsList
exit $LASTEXITCODE
