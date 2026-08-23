[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$BuildDir = 'D:\ZmdRenderdoc\renderdoc\x64\Development',
    [string]$LauncherExe = 'E:\Neverness To Everness\NTELauncher\NTEGame.exe',
    [string]$LauncherArgs = '/launcher /directly',
    [switch]$RunAsAdmin
)
$ErrorActionPreference = 'Stop'
if(!(Test-Path -LiteralPath $BuildDir -PathType Container)) { throw "BuildDir not found: $BuildDir" }
if(!(Test-Path -LiteralPath $LauncherExe -PathType Leaf)) { throw "LauncherExe not found: $LauncherExe" }
$qrender = @('qrendertest.exe','qrenderahab.exe','qrenderdoc.exe') | ForEach-Object { Join-Path $BuildDir $_ } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if(!$qrender) { throw "Could not find qrender executable in $BuildDir" }
$profile = Join-Path $BuildDir 'zmd-nte-launcher-autostart.cap'
$loader = Join-Path $BuildDir 'zmd-nte-launcher-autostart.py'
$workingDir = Split-Path -Parent $LauncherExe
$profileJson = [ordered]@{ rdocCaptureSettings = 1; settings = [ordered]@{ autoStart = $true; executable = $LauncherExe; workingDir = $workingDir; commandLine = $LauncherArgs; inject = $false; queuedFrameCap = 0; numQueuedFrames = 0; environment = @(); options = [ordered]@{ hookIntoChildren = $true; allowFullscreen = $true; allowVSync = $true; apiValidation = $false; captureAllCmdLists = $false; captureCallstacks = $false; captureCallstacksOnlyDraws = $false; debugOutputMute = $true; delayForDebugger = 0; refAllResources = $true; softMemoryLimit = 0; verifyBufferAccess = $false; allowUnsupportedVendorExtensions = 0x10DE } } } | ConvertTo-Json -Depth 8
Set-Content -LiteralPath $profile -Value $profileJson -Encoding UTF8
Set-Content -LiteralPath $loader -Value "cap = pyrenderdoc.GetCaptureDialog()`ncap.LoadSettings(r'$profile')`n" -Encoding ASCII
Write-Host "Profile: $profile"
Write-Host "Loader:  $loader"
Write-Host "QRender: $qrender"
if($RunAsAdmin) { Start-Process -FilePath $qrender -ArgumentList @('--ui-python', $loader) -Verb RunAs -WorkingDirectory $BuildDir }
else { Start-Process -FilePath $qrender -ArgumentList @('--ui-python', $loader) -WorkingDirectory $BuildDir }
