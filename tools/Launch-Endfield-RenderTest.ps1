$ErrorActionPreference = 'Stop'

$gameDir = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
$gamesDir = Split-Path -Parent $gameDir
$launcherRoot = [IO.Path]::GetFullPath(
    (Split-Path -Parent $gamesDir)).TrimEnd('\')
$launcherExe = Join-Path $launcherRoot 'Launcher.exe'

if (-not (Test-Path -LiteralPath $launcherExe -PathType Leaf)) {
    throw "Hypergryph launcher not found: $launcherExe"
}

$launcherName = Split-Path -Leaf $launcherExe
try {
    $processes = @(Get-CimInstance Win32_Process -ErrorAction Stop)
}
catch {
    throw "Unable to verify Hypergryph Launcher process state: $($_.Exception.Message)"
}

$unreadableLauncher = @($processes | Where-Object {
    $_.Name -ieq $launcherName -and
    [string]::IsNullOrWhiteSpace([string]$_.ExecutablePath)
})
if ($unreadableLauncher.Count -gt 0) {
    throw 'Unable to verify the path of an existing Launcher.exe process'
}

$running = @($processes |
    Where-Object {
        $path = [string]$_.ExecutablePath
        -not [string]::IsNullOrWhiteSpace($path) -and
        ($path.Equals($launcherExe, [StringComparison]::OrdinalIgnoreCase) -or
         $path.StartsWith(
             $launcherRoot + '\',
             [StringComparison]::OrdinalIgnoreCase))
    })
if ($running.Count -gt 0) {
    $summary = ($running | ForEach-Object {
        "$($_.Name)[$($_.ProcessId)]"
    }) -join ', '
    throw "Close all Hypergryph Launcher processes before scoped launch: $summary"
}

$dxgiPath = Join-Path $gameDir 'dxgi.dll'
$renamedDxgiPath = Join-Path $gameDir 'dxgi.dll.tmp'
$dxgiExists = Test-Path -LiteralPath $dxgiPath -PathType Leaf
$renamedDxgiExists = Test-Path -LiteralPath $renamedDxgiPath -PathType Leaf

if ($dxgiExists -eq $renamedDxgiExists) {
    if ($dxgiExists) {
        throw 'Ambiguous DXGI proxy state: both dxgi.dll and dxgi.dll.tmp exist'
    }
    throw 'DXGI proxy is missing: neither dxgi.dll nor dxgi.dll.tmp exists'
}

if ($renamedDxgiExists) {
    Move-Item -LiteralPath $renamedDxgiPath -Destination $dxgiPath
    if (-not (Test-Path -LiteralPath $dxgiPath -PathType Leaf) -or
        (Test-Path -LiteralPath $renamedDxgiPath -PathType Leaf)) {
        throw 'Failed to restore dxgi.dll from dxgi.dll.tmp'
    }
}

$env:ENABLE_VULKAN_RENDERTEST_CAPTURE = '1'
Start-Process -FilePath $launcherExe -WorkingDirectory $launcherRoot
