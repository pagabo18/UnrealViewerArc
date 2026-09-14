# Shared bootstrap for Windows wrappers: locate a Python 3 interpreter (system, py launcher, or Unreal's bundled one)
function Find-AgentPython {
    foreach ($name in @("python", "python3")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) {
            $ver = & $cmd.Source -c "import sys; print(sys.version_info >= (3, 9))" 2>$null
            if ($ver -eq "True") { return $cmd.Source }
        }
    }
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) {
        $path = & py -3 -c "import sys; print(sys.executable)" 2>$null
        if ($path) { return $path.Trim() }
    }
    # Unreal Engine bundled Python (works standalone)
    $roots = @("$env:ProgramFiles\Epic Games", "D:\Program Files\Epic Games", "C:\Epic Games", "D:\Epic Games")
    foreach ($root in $roots) {
        if (Test-Path $root) {
            Get-ChildItem -Path $root -Directory -Filter "UE_*" -ErrorAction SilentlyContinue | Sort-Object Name -Descending | ForEach-Object {
                $candidate = Join-Path $_.FullName "Engine\Binaries\ThirdParty\Python3\Win64\python.exe"
                if (Test-Path $candidate) { return $candidate }
            }
        }
    }
    # Registry (source builds / custom locations)
    try {
        Get-ChildItem "HKLM:\SOFTWARE\EpicGames\Unreal Engine" -ErrorAction Stop | ForEach-Object {
            $dir = (Get-ItemProperty $_.PSPath).InstalledDirectory
            $candidate = Join-Path $dir "Engine\Binaries\ThirdParty\Python3\Win64\python.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    } catch {}
    return $null
}
