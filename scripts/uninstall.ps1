# Windows uninstaller: removes plugin, MCP config and skill; keeps project assets
param([string]$Project, [switch]$Purge, [string]$Python)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_bootstrap.ps1"
$py = if ($Python) { $Python } else { Find-AgentPython }
if (-not $py) { Write-Error "Python 3.9+ not found."; exit 1 }
$argsList = @("$PSScriptRoot\lib\installer.py", "uninstall")
if ($Project) { $argsList += @("--project", $Project) }
if ($Purge) { $argsList += "--purge" }
& $py @argsList
exit $LASTEXITCODE
