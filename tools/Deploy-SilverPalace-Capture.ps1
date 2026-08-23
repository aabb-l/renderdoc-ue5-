[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory=$true)]
    [ValidateNotNullOrEmpty()]
    [string]$GameDir,
    [string]$BuildDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'x64\Release')
)

$ErrorActionPreference = 'Stop'
$Subject = 'CN=Codex DXGI Test Code Signing'

function Get-OrCreate-CodeSigningCertificate {
    $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(1) } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1

    if (!$cert) {
        $cert = New-SelfSignedCertificate `
            -Subject $Subject `
            -Type CodeSigningCert `
            -KeyAlgorithm RSA `
            -KeyLength 2048 `
            -HashAlgorithm SHA256 `
            -KeyUsage DigitalSignature `
            -CertStoreLocation Cert:\CurrentUser\My `
            -NotAfter (Get-Date).AddYears(3)
    }

    foreach ($storeName in @('Root', 'TrustedPublisher')) {
        $store = [System.Security.Cryptography.X509Certificates.X509Store]::new(
            $storeName,
            [System.Security.Cryptography.X509Certificates.StoreLocation]::CurrentUser)
        try {
            $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
            $exists = [bool]($store.Certificates | Where-Object Thumbprint -eq $cert.Thumbprint)
            if (!$exists) { $store.Add($cert) }
        } finally {
            $store.Close()
        }
    }

    return $cert
}

function Sign-FileForSilverPalace {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Certificate
    )

    $sig = Set-AuthenticodeSignature -LiteralPath $Path -Certificate $Certificate -HashAlgorithm SHA256 -Force
    if ($sig.Status -ne 'Valid') {
        throw "Signing failed for $Path : $($sig.Status) $($sig.StatusMessage)"
    }
}

$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
$Dxgi = Join-Path $BuildDir 'dxgi.dll'
$RenderTest = Join-Path $BuildDir 'rendertest.dll'

if (!(Test-Path -LiteralPath $GameDir -PathType Container)) { throw "GameDir not found: $GameDir" }
if (!(Test-Path -LiteralPath $Dxgi -PathType Leaf)) { throw "dxgi.dll not found: $Dxgi" }
if (!(Test-Path -LiteralPath $RenderTest -PathType Leaf)) { throw "rendertest.dll not found: $RenderTest" }

$running = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
    Where-Object {
        ($_.Name -match 'SilverPalace') -or
        ($_.ExecutablePath -and $_.ExecutablePath.StartsWith($GameDir, [System.StringComparison]::OrdinalIgnoreCase)) -or
        ($_.CommandLine -match 'SilverPalace|Silver Palace')
    }
if ($running) {
    throw "Silver Palace process is running. Close it before deployment. PID=$($running.ProcessId -join ',')"
}

$cert = Get-OrCreate-CodeSigningCertificate
Sign-FileForSilverPalace -Path $Dxgi -Certificate $cert
Sign-FileForSilverPalace -Path $RenderTest -Certificate $cert

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = Join-Path $GameDir ('codex-silver-palace-capture-backup-' + $stamp)
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
        Game = 'Silver Palace'
        Path = $p
        SHA256 = $hash.Hash
        Signature = $sig.Status
        Signer = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '' }
        BackupDir = $backupDir
        BackedUp = if ($backedUp.Count) { $backedUp -join ', ' } else { '(none)' }
    }
}

Write-Host 'Deployment finished with Silver Palace local code signing.'
