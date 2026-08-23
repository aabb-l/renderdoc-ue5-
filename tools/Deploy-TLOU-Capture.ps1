[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$GameDir = 'D:\Games\The Last of Us - Part I',
    [string]$BuildDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'x64\Release')
)

$ErrorActionPreference = 'Stop'

$Dxgi = Join-Path $BuildDir 'dxgi.dll'
$RenderTest = Join-Path $BuildDir 'rendertest.dll'

if (!(Test-Path -LiteralPath $GameDir -PathType Container)) { throw "GameDir not found: $GameDir" }
if (!(Test-Path -LiteralPath $Dxgi -PathType Leaf)) { throw "dxgi.dll not found: $Dxgi" }
if (!(Test-Path -LiteralPath $RenderTest -PathType Leaf)) { throw "rendertest.dll not found: $RenderTest" }

$running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -eq 'tlou-i' }
if ($running) { throw "tlou-i.exe is running. Close the game before deployment. PID=$($running.Id -join ',')" }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = Join-Path $GameDir ("codex-tlou-capture-backup-" + $stamp)
New-Item -ItemType Directory -Path $backupDir | Out-Null
$backedUp = @()

foreach ($name in @('dxgi.dll', 'dxgi.dll.tmp', 'rendertest.dll', 'proxy.log')) {
    $existing = Join-Path $GameDir $name
    if (Test-Path -LiteralPath $existing) {
        Move-Item -LiteralPath $existing -Destination (Join-Path $backupDir $name) -Force
        $backedUp += $name
    }
}

Copy-Item -LiteralPath $Dxgi -Destination (Join-Path $GameDir 'dxgi.dll') -Force
Copy-Item -LiteralPath $RenderTest -Destination (Join-Path $GameDir 'rendertest.dll') -Force

foreach ($name in @('dxgi.dll', 'rendertest.dll')) {
    $p = Join-Path $GameDir $name
    $hash = Get-FileHash -LiteralPath $p -Algorithm SHA256
    $sig = Get-AuthenticodeSignature -LiteralPath $p
    [pscustomobject]@{
        Game = 'The Last of Us Part I'
        Path = $p
        SHA256 = $hash.Hash
        Signature = $sig.Status
        Signer = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '' }
        BackupDir = $backupDir
        BackedUp = if ($backedUp.Count) { $backedUp -join ', ' } else { '(none)' }
    }
}

Write-Host 'Deployment finished. This script intentionally does not sign files.'
