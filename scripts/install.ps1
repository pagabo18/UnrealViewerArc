# Windows installer: .\scripts\install.ps1 [-Project <path\to.uproject>] [-Engine] [-Scope project|user] [-Build] [-Yes]
param(
    [string]$Project,
    [switch]$Engine,
    [ValidateSet("project", "user")][string]$Scope = "project",
    [switch]$Build,
    [switch]$Yes,
    [string]$Python
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_bootstrap.ps1"
$py = if ($Python) { $Python } else { Find-AgentPython }
if (-not $py) { Show-PythonHelp; exit 1 }
$argsList = @("$PSScriptRoot\lib\installer.py", "install", "--python", $py)
if ($Project) { $argsList += @("--project", $Project) }
if ($Engine) { $argsList += "--engine" }
if ($Scope) { $argsList += @("--scope", $Scope) }
if ($Build) { $argsList += "--build" }
if ($Yes) { $argsList += "--yes" }
& $py @argsList
exit $LASTEXITCODE
