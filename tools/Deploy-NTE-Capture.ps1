[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$BuildDir,
    [string]$GameDir = 'E:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64',
    [string]$LauncherExe = 'E:\Neverness To Everness\NTELauncher\NTEGame.exe',
    [string]$LauncherArgs = '/launcher /directly',
    [string]$QRenderExe,
    [switch]$ForceClose,
    [switch]$NoLaunchUI,
    [switch]$RunAsAdmin
)

$ErrorActionPreference = 'Stop'

function Resolve-ExistingDirectory([string]$Path, [string]$Label) {
    if(!(Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-ExistingFile([string]$Path, [string]$Label) {
    if(!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-RenderDocUi([string]$Dir, [string]$ExplicitPath) {
    if($ExplicitPath) {
        return Resolve-ExistingFile $ExplicitPath 'QRenderExe'
    }

    foreach($name in @('qrendertest.exe', 'qrenderdoc.exe', 'qrenderahab.exe')) {
        $candidate = Join-Path $Dir $name
        if(Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    return $null
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $repoRoot 'x64\Release'
}

$buildPath = Resolve-ExistingDirectory $BuildDir 'BuildDir'
$gamePath = Resolve-ExistingDirectory $GameDir 'GameDir'
$launcherPath = Resolve-ExistingFile $LauncherExe 'LauncherExe'

$dxgiSource = Resolve-ExistingFile (Join-Path $buildPath 'dxgi.dll') 'built dxgi.dll'
$coreSource = Resolve-ExistingFile (Join-Path $buildPath 'rendertest.dll') 'built rendertest.dll'

$processNames = @('NTEGame', 'HTGame', 'NTEBrowser', 'NTEWebBooster', 'CrashClientReporter')
$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $processNames -contains $_.ProcessName })
if($running.Count -gt 0) {
    if(!$ForceClose) {
        $running | Select-Object Id, ProcessName, Path | Format-Table -AutoSize
        throw 'NTE related processes are still running. Close them or pass -ForceClose.'
    }

    $running | Stop-Process -Force
    Start-Sleep -Seconds 2

    $left = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $processNames -contains $_.ProcessName })
    if($left.Count -gt 0) {
        $left | Select-Object Id, ProcessName, Path | Format-Table -AutoSize
        throw 'Some NTE related processes are still running after -ForceClose.'
    }
}

$backupDir = Join-Path $gamePath ('renderdoc-nte-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backupDir | Out-Null
foreach($name in @('dxgi.dll', 'dxgi.dll.tmp', 'rendertest.dll', 'proxy.log')) {
    $existing = Join-Path $gamePath $name
    if(Test-Path -LiteralPath $existing) {
        Move-Item -LiteralPath $existing -Destination (Join-Path $backupDir $name) -Force
    }
}

Copy-Item -LiteralPath $dxgiSource -Destination (Join-Path $gamePath 'dxgi.dll') -Force
Copy-Item -LiteralPath $coreSource -Destination (Join-Path $gamePath 'rendertest.dll') -Force

$deployed = foreach($name in @('dxgi.dll', 'rendertest.dll')) {
    $path = Join-Path $gamePath $name
    [PSCustomObject]@{
        Name = $name
        Size = (Get-Item -LiteralPath $path).Length
        SHA256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        Signature = (Get-AuthenticodeSignature -LiteralPath $path).Status
        Path = $path
    }
}

Write-Host "Backup: $backupDir"
$deployed | Format-Table -AutoSize

$qrender = Get-RenderDocUi -Dir $buildPath -ExplicitPath $QRenderExe
$profile = Join-Path $buildPath 'nte-launcher-autostart.cap'
$loader = Join-Path $buildPath 'nte-launcher-autostart.py'
$workingDir = Split-Path -Parent $launcherPath
$profileJson = [ordered]@{
    rdocCaptureSettings = 1
    settings = [ordered]@{
        autoStart = $true
        executable = $launcherPath
        workingDir = $workingDir
        commandLine = $LauncherArgs
        inject = $false
        queuedFrameCap = 0
        numQueuedFrames = 0
        environment = @()
        options = [ordered]@{
            hookIntoChildren = $true
            allowFullscreen = $true
            allowVSync = $true
            apiValidation = $false
            captureAllCmdLists = $false
            captureCallstacks = $false
            captureCallstacksOnlyDraws = $false
            debugOutputMute = $true
            delayForDebugger = 0
            refAllResources = $true
            softMemoryLimit = 0
            verifyBufferAccess = $false
            allowUnsupportedVendorExtensions = 0x10DE
        }
    }
} | ConvertTo-Json -Depth 8
Set-Content -LiteralPath $profile -Value $profileJson -Encoding UTF8
Set-Content -LiteralPath $loader -Value "cap = pyrenderdoc.GetCaptureDialog()`ncap.LoadSettings(r'$profile')`n" -Encoding ASCII

Write-Host "Profile: $profile"
Write-Host "Loader:  $loader"

if($NoLaunchUI) {
    Write-Host 'Deployment complete. UI launch skipped because -NoLaunchUI was supplied.'
    exit 0
}

if(!$qrender) {
    throw "Could not find qrendertest.exe/qrenderdoc.exe in BuildDir. Pass -QRenderExe or use -NoLaunchUI."
}

Write-Host "QRender: $qrender"
if($RunAsAdmin) {
    Start-Process -FilePath $qrender -ArgumentList @('--ui-python', $loader) -Verb RunAs -WorkingDirectory $buildPath
} else {
    Start-Process -FilePath $qrender -ArgumentList @('--ui-python', $loader) -WorkingDirectory $buildPath
}
