param([string]$Python)
. "$PSScriptRoot\_bootstrap.ps1"
$py = if ($Python) { $Python } else { Find-AgentPython }
if (-not $py) { Write-Error "Python 3.9+ not found."; exit 1 }
& $py "$PSScriptRoot\lib\installer.py" doctor
exit $LASTEXITCODE
