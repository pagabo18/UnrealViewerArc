# Shared bootstrap for Windows wrappers: locate a real Python 3.9+ interpreter.
# Skips the Microsoft Store "python.exe" alias stub (WindowsApps) and falls back to
# the Python bundled with Unreal Engine, which works standalone.
function Test-AgentPython {
    param([string]$Exe)
    if (-not $Exe -or -not (Test-Path $Exe)) { return $false }
    if ($Exe -like "*\WindowsApps\*") { return $false }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        $out = & $Exe -c "import sys; print(sys.version_info >= (3, 9))" 2>$null
        return ($LASTEXITCODE -eq 0 -and "$out".Trim() -eq "True")
    } catch { return $false } finally { $ErrorActionPreference = $prev }
}

function Get-UnrealPythonCandidates {
    $candidates = @()
    $roots = @()
    $launcher = Join-Path $env:ProgramData "Epic\UnrealEngineLauncher\LauncherInstalled.dat"
    if (Test-Path $launcher) {
        try {
            $json = Get-Content $launcher -Raw | ConvertFrom-Json
            foreach ($item in $json.InstallationList) { if ($item.AppName -like "UE_*") { $roots += $item.InstallLocation } }
        } catch {}
    }
    try {
        Get-ChildItem "HKLM:\SOFTWARE\EpicGames\Unreal Engine" -ErrorAction Stop | ForEach-Object { $roots += (Get-ItemProperty $_.PSPath).InstalledDirectory }
    } catch {}
    try {
        $builds = Get-ItemProperty "HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds" -ErrorAction Stop
        $builds.PSObject.Properties | Where-Object { $_.Value -is [string] -and (Test-Path $_.Value) } | ForEach-Object { $roots += $_.Value }
    } catch {}
    foreach ($base in @("$env:ProgramFiles\Epic Games", "D:\Program Files\Epic Games", "C:\Epic Games", "D:\Epic Games", "E:\Epic Games")) {
        if (Test-Path $base) { Get-ChildItem -Path $base -Directory -Filter "UE_*" -ErrorAction SilentlyContinue | ForEach-Object { $roots += $_.FullName } }
    }
    foreach ($root in ($roots | Where-Object { $_ } | Sort-Object -Unique -Descending)) {
        $candidate = Join-Path $root "Engine\Binaries\ThirdParty\Python3\Win64\python.exe"
        if (Test-Path $candidate) { $candidates += $candidate }
    }
    return $candidates
}

function Find-AgentPython {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        # 1. explicit override
        if ($env:AGENT_PYTHON -and (Test-AgentPython $env:AGENT_PYTHON)) { return $env:AGENT_PYTHON }
        # 2. py launcher (most reliable on Windows)
        $py = Get-Command py -ErrorAction SilentlyContinue
        if ($py -and $py.Source -notlike "*\WindowsApps\*") {
            $path = & $py.Source -3 -c "import sys; print(sys.executable)" 2>$null
            if ($LASTEXITCODE -eq 0 -and $path -and (Test-AgentPython "$path".Trim())) { return "$path".Trim() }
        }
        # 3. python / python3 on PATH (all matches, skipping the Store stub)
        foreach ($name in @("python", "python3")) {
            foreach ($cmd in (Get-Command $name -All -ErrorAction SilentlyContinue)) {
                if (Test-AgentPython $cmd.Source) { return $cmd.Source }
            }
        }
        # 4. common install folders
        foreach ($pattern in @("$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe", "C:\Python3*\python.exe", "$env:ProgramFiles\Python3*\python.exe")) {
            foreach ($item in (Get-ChildItem $pattern -ErrorAction SilentlyContinue | Sort-Object FullName -Descending)) {
                if (Test-AgentPython $item.FullName) { return $item.FullName }
            }
        }
        # 5. Python bundled with Unreal Engine
        foreach ($candidate in (Get-UnrealPythonCandidates)) {
            if (Test-AgentPython $candidate) { return $candidate }
        }
        return $null
    } finally { $ErrorActionPreference = $prev }
}

function Show-PythonHelp {
    Write-Host ""
    Write-Host "Python 3.9+ was not found (the Microsoft Store 'python' alias does not count)." -ForegroundColor Yellow
    Write-Host "Options:"
    Write-Host "  a) Install Python:  winget install Python.Python.3.12   (then open a NEW PowerShell window)"
    Write-Host "  b) Or pass the Python that ships with Unreal, e.g.:"
    Write-Host '     .\scripts\install.ps1 -Python "C:\Program Files\Epic Games\UE_5.5\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" -Project "...uproject"'
    Write-Host ""
}
