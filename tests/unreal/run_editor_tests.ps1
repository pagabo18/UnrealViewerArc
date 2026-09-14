# Runs the plugin's editor automation tests headless.
# Usage: .\tests\unreal\run_editor_tests.ps1 -Project "C:\Path\MyGame.uproject" [-Engine "C:\Program Files\Epic Games\UE_5.5"]
param([Parameter(Mandatory=$true)][string]$Project, [string]$Engine, [string]$Filter = "ClaudeBlueprintAgent")
$ErrorActionPreference = "Stop"
if (-not $Engine) {
    $assoc = (Get-Content $Project | ConvertFrom-Json).EngineAssociation
    $Engine = "$env:ProgramFiles\Epic Games\UE_$assoc"
}
$editor = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $editor)) { Write-Error "UnrealEditor-Cmd.exe not found under $Engine"; exit 1 }
$report = Join-Path (Split-Path $Project) "Saved\ClaudeAgent\AutomationReport"
& $editor $Project -ExecCmds="Automation RunTests $Filter; Quit" -unattended -nopause -nosplash -NullRHI -log -ReportExportPath="$report" -testexit="Automation Test Queue Empty"
Write-Host "Exit code: $LASTEXITCODE. Report: $report"
exit $LASTEXITCODE
