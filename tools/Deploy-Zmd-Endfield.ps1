<#
.SYNOPSIS
Deploys or rolls back the ZMD RenderDoc capture setup for Arknights Endfield.

.DESCRIPTION
This is the single source-controlled deployment entry point. It copies the DXGI proxy,
RenderDoc core and Vulkan layer manifest into the game directory, registers the 64-bit
HKLM Vulkan implicit layer, writes a rollback state, and installs the per-game scoped
launcher. The launcher template is tools\Launch-Endfield-RenderTest.ps1; the generated
Launch-Endfield-RenderTest.cmd exists only in the game directory as a deployment artifact.
#>
[CmdletBinding()]
param(
    [ValidateSet('Deploy', 'Rollback')]
    [string]$Mode = 'Deploy',

    [string]$GameDir = 'D:\Hypergryph Launcher\games\Arknights Endfield',
    [string]$BuildDir,
    [string]$StatePath,

    [Parameter(DontShow = $true)]
    [string]$RegistryRequestPath
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $candidateBuildDirs = @(
        (Join-Path $root 'x64\Release'),
        'D:\ZmdRenderdoc\1\renderdoc\x64\Release'
    )
    foreach ($candidateBuildDir in $candidateBuildDirs) {
        $hasAllArtifacts = $true
        foreach ($artifactName in @('dxgi.dll', 'rendertest.dll', 'rendertest.json')) {
            if (-not (Test-Path -LiteralPath (Join-Path $candidateBuildDir $artifactName) -PathType Leaf)) {
                $hasAllArtifacts = $false
                break
            }
        }
        if ($hasAllArtifacts) {
            $BuildDir = $candidateBuildDir
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($BuildDir)) {
        $BuildDir = Join-Path $root 'x64\Release'
    }
}

$registrySubKey = 'SOFTWARE\Khronos\Vulkan\ImplicitLayers'
$enableVariable = 'ENABLE_VULKAN_RENDERTEST_CAPTURE'
$managedFiles = @(
    'dxgi.dll',
    'dxgi.dll.tmp',
    'rendertest.dll',
    'rendertest.json',
    'Launch-Endfield-RenderTest.ps1',
    'Launch-Endfield-RenderTest.cmd',
    'proxy.log',
    'renderdoc.log'
)

function Get-CanonicalPath {
    [CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
}

function Assert-EndfieldStopped {
    $running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ProcessName -like 'Endfield*'
    })
    if ($running.Count -gt 0) {
        $summary = ($running | ForEach-Object { "$($_.ProcessName)[$($_.Id)]" }) -join ', '
        throw "Endfield must be closed before $Mode. Running: $summary"
    }
}

function Test-HypergryphLauncherRunning {
    $processes = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
    foreach ($process in $processes) {
        $path = [string]$process.ExecutablePath
        if ($path -like '*\Hypergryph Launcher\*') {
            return $true
        }
    }
    return $false
}

function Broadcast-EnvironmentChange {
    if (-not ('CodexEnvironmentBroadcast' -as [type])) {
        Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class CodexEnvironmentBroadcast
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SendMessageTimeout(
        IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
        uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);

    public static void Notify()
    {
        UIntPtr result;
        SendMessageTimeout(new IntPtr(0xffff), 0x001A, UIntPtr.Zero,
            "Environment", 0x0002, 5000, out result);
    }
}
'@
    }

    [CodexEnvironmentBroadcast]::Notify()
}

function Find-DumpBin {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath | Select-Object -First 1
        if ($installPath) {
            $candidate = Get-ChildItem -LiteralPath (Join-Path $installPath 'VC\Tools\MSVC') `
                -Filter dumpbin.exe -File -Recurse -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName.EndsWith('\Hostx64\x64\dumpbin.exe', [System.StringComparison]::OrdinalIgnoreCase) } |
                Sort-Object FullName -Descending |
                Select-Object -First 1
            if ($candidate) {
                return $candidate.FullName
            }
        }
    }

    throw 'Unable to locate x64 dumpbin.exe'
}

function Assert-UnsignedArtifact {
    [CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing build artifact: $Path"
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'NotSigned') {
        throw "Artifact must be unsigned: $Path (actual: $($signature.Status))"
    }
}

function Assert-SourceArtifacts {
    foreach ($name in @('dxgi.dll', 'rendertest.dll', 'rendertest.json')) {
        $path = Join-Path $BuildDir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Missing build artifact: $path"
        }
    }

    Assert-UnsignedArtifact -Path (Join-Path $BuildDir 'dxgi.dll')
    Assert-UnsignedArtifact -Path (Join-Path $BuildDir 'rendertest.dll')

    $scopedLauncherSource = Join-Path $root 'tools\Launch-Endfield-RenderTest.ps1'
    if (-not (Test-Path -LiteralPath $scopedLauncherSource -PathType Leaf)) {
        throw "Missing scoped launcher source: $scopedLauncherSource"
    }

    $manifest = Get-Content -LiteralPath (Join-Path $BuildDir 'rendertest.json') -Raw |
        ConvertFrom-Json
    if ($manifest.layer.name -ne 'VK_LAYER_RENDERTEST_Capture') {
        throw "Unexpected Vulkan layer name: $($manifest.layer.name)"
    }
    if ($manifest.layer.library_path -ne '.\rendertest.dll') {
        throw "Unexpected Vulkan library_path: $($manifest.layer.library_path)"
    }
    $enableProperty = $manifest.layer.enable_environment.PSObject.Properties |
        Where-Object { $_.Name -eq $enableVariable } |
        Select-Object -First 1
    if (-not $enableProperty -or [string]$enableProperty.Value -ne '1') {
        throw "Manifest must require $enableVariable=1"
    }

    $dumpbin = Find-DumpBin
    $exports = (& $dumpbin /nologo /exports (Join-Path $BuildDir 'rendertest.dll') 2>&1 |
        Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw 'dumpbin /exports failed for rendertest.dll'
    }
    foreach ($export in @(
        'RENDERDOC_GetAPI',
        'VK_LAYER_RENDERDOC_CaptureGetInstanceProcAddr',
        'VK_LAYER_RENDERDOC_CaptureGetDeviceProcAddr',
        'VK_LAYER_RENDERDOC_CaptureNegotiateLoaderLayerInterfaceVersion'
    )) {
        if (-not $exports.Contains($export)) {
            throw "rendertest.dll is missing export: $export"
        }
    }
}

function Open-LayerRegistryBaseKey {
    [CmdletBinding()]
param([Parameter(Mandatory = $true)][Microsoft.Win32.RegistryHive]$Hive)

    return [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        $Hive,
        [Microsoft.Win32.RegistryView]::Registry64)
}

function Get-LayerRegistrySnapshot {
    [CmdletBinding()]
param(
        [Parameter(Mandatory = $true)][Microsoft.Win32.RegistryHive]$Hive,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    $baseKey = Open-LayerRegistryBaseKey -Hive $Hive
    try {
        $key = $baseKey.OpenSubKey($registrySubKey, $false)
        if (-not $key) {
            return [ordered]@{ Exists = $false; Name = $ManifestPath; Value = $null; Kind = $null }
        }
        try {
            $existingName = $key.GetValueNames() |
                Where-Object { $_.Equals($ManifestPath, [StringComparison]::OrdinalIgnoreCase) } |
                Select-Object -First 1
            if (-not $existingName) {
                return [ordered]@{ Exists = $false; Name = $ManifestPath; Value = $null; Kind = $null }
            }
            return [ordered]@{
                Exists = $true
                Name = $existingName
                Value = $key.GetValue($existingName, $null,
                    [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                Kind = [string]$key.GetValueKind($existingName)
            }
        }
        finally {
            $key.Dispose()
        }
    }
    finally {
        $baseKey.Dispose()
    }
}

function Set-LayerRegistryValue {
    [CmdletBinding()]
param(
        [Parameter(Mandatory = $true)][Microsoft.Win32.RegistryHive]$Hive,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    $baseKey = Open-LayerRegistryBaseKey -Hive $Hive
    try {
        $key = $baseKey.CreateSubKey($registrySubKey, $true)
        try {
            $key.SetValue($ManifestPath, 0, [Microsoft.Win32.RegistryValueKind]::DWord)
        }
        finally {
            $key.Dispose()
        }
    }
    finally {
        $baseKey.Dispose()
    }
}

function Remove-LayerRegistryValue {
    [CmdletBinding()]
param(
        [Parameter(Mandatory = $true)][Microsoft.Win32.RegistryHive]$Hive,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    $baseKey = Open-LayerRegistryBaseKey -Hive $Hive
    try {
        $key = $baseKey.OpenSubKey($registrySubKey, $true)
        if (-not $key) {
            return
        }
        try {
            foreach ($name in @($key.GetValueNames())) {
                if ($name.Equals($ManifestPath, [StringComparison]::OrdinalIgnoreCase)) {
                    $key.DeleteValue($name, $false)
                }
            }
        }
        finally {
            $key.Dispose()
        }
    }
    finally {
        $baseKey.Dispose()
    }
}

function Restore-LayerRegistryValue {
    [CmdletBinding()]
param(
        [Parameter(Mandatory = $true)][Microsoft.Win32.RegistryHive]$Hive,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][object]$Snapshot
    )

    $baseKey = Open-LayerRegistryBaseKey -Hive $Hive
    try {
        $key = $baseKey.CreateSubKey($registrySubKey, $true)
        try {
            foreach ($name in @($key.GetValueNames())) {
                if ($name.Equals($ManifestPath, [StringComparison]::OrdinalIgnoreCase)) {
                    $key.DeleteValue($name, $false)
                }
            }

            if ($Snapshot.Exists) {
                $kind = [Microsoft.Win32.RegistryValueKind]::$($Snapshot.Kind)
                $key.SetValue([string]$Snapshot.Name, $Snapshot.Value, $kind)
            }
        }
        finally {
            $key.Dispose()
        }
    }
    finally {
        $baseKey.Dispose()
    }
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-RegistryRequest {
    [CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Path)

    $resolvedRequestPath = (Resolve-Path -LiteralPath $Path).Path
    $resultPath = $resolvedRequestPath + '.result.json'
    $result = [ordered]@{
        Success = $false
        Error = $null
        Registry = 'HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers (64-bit)'
    }

    try {
        if (-not (Test-IsAdministrator)) {
            throw 'The HKLM registry helper is not running with administrator rights'
        }

        $request = Get-Content -LiteralPath $resolvedRequestPath -Raw | ConvertFrom-Json
        if ([int]$request.Version -ne 1) {
            throw "Unsupported registry request version: $($request.Version)"
        }

        $requestManifestPath = Get-CanonicalPath ([string]$request.ManifestPath)
        switch ([string]$request.Action) {
            'Set' {
                Set-LayerRegistryValue -Hive ([Microsoft.Win32.RegistryHive]::LocalMachine) `
                    -ManifestPath $requestManifestPath
            }
            'Restore' {
                if ($null -eq $request.Snapshot) {
                    throw 'Restore registry request is missing its snapshot'
                }
                Restore-LayerRegistryValue -Hive ([Microsoft.Win32.RegistryHive]::LocalMachine) `
                    -ManifestPath $requestManifestPath -Snapshot $request.Snapshot
            }
            default {
                throw "Unsupported registry request action: $($request.Action)"
            }
        }

        $result.Success = $true
    }
    catch {
        $result.Error = $_.Exception.Message
    }

    $result | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath $resultPath -Encoding UTF8
    if (-not $result.Success) {
        throw "Elevated registry request failed: $($result.Error)"
    }
}

function Invoke-ElevatedMachineRegistryAction {
    [CmdletBinding()]
param(
        [Parameter(Mandatory = $true)][ValidateSet('Set', 'Restore')][string]$Action,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$RequestDirectory,
        [AllowNull()][object]$Snapshot
    )

    $requestId = [Guid]::NewGuid().ToString('N')
    $requestPath = Join-Path $RequestDirectory "registry-$($Action.ToLowerInvariant())-$requestId.json"
    $resultPath = $requestPath + '.result.json'
    $request = [ordered]@{
        Version = 1
        Action = $Action
        ManifestPath = Get-CanonicalPath $ManifestPath
        Snapshot = $Snapshot
    }
    $request | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath $requestPath -Encoding UTF8

    if (Test-IsAdministrator) {
        Invoke-RegistryRequest -Path $requestPath
    }
    else {
        $hostPath = (Get-Process -Id $PID).Path
        if ([string]::IsNullOrWhiteSpace($hostPath)) {
            throw 'Unable to locate the current PowerShell executable for UAC elevation'
        }

        $escapedScriptPath = $PSCommandPath.Replace("'", "''")
        $escapedRequestPath = $requestPath.Replace("'", "''")
        $command = "& '$escapedScriptPath' -RegistryRequestPath '$escapedRequestPath'"
        $encodedCommand = [Convert]::ToBase64String(
            [Text.Encoding]::Unicode.GetBytes($command))

        try {
            $process = Start-Process -FilePath $hostPath -ArgumentList @(
                '-NoProfile',
                '-NonInteractive',
                '-ExecutionPolicy', 'Bypass',
                '-EncodedCommand', $encodedCommand
            ) -Verb RunAs -WindowStyle Hidden -Wait -PassThru
        }
        catch {
            throw "UAC elevation was cancelled or failed: $($_.Exception.Message)"
        }

        $process.Refresh()
        if ($process.ExitCode -ne 0) {
            throw "Elevated registry helper exited with code $($process.ExitCode)"
        }
    }

    if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        throw "Elevated registry helper did not create its result: $resultPath"
    }
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if (-not [bool]$result.Success) {
        throw "Elevated registry helper failed: $($result.Error)"
    }
}

function Get-UserEnvironmentSnapshot {
    [CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Name)

    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment')
    if (-not $key) {
        return [ordered]@{ Exists = $false; Name = $Name; Value = $null; Kind = $null }
    }
    try {
        $existingName = $key.GetValueNames() |
            Where-Object { $_.Equals($Name, [StringComparison]::OrdinalIgnoreCase) } |
            Select-Object -First 1
        if (-not $existingName) {
            return [ordered]@{ Exists = $false; Name = $Name; Value = $null; Kind = $null }
        }
        return [ordered]@{
            Exists = $true
            Name = $existingName
            Value = $key.GetValue($existingName, $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            Kind = [string]$key.GetValueKind($existingName)
        }
    }
    finally {
        $key.Dispose()
    }
}

function Remove-UserEnvironmentValue {
    [CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Name)

    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Environment', $true)
    try {
        foreach ($existingName in @($key.GetValueNames())) {
            if ($existingName.Equals($Name, [StringComparison]::OrdinalIgnoreCase)) {
                $key.DeleteValue($existingName, $false)
            }
        }
    }
    finally {
        $key.Dispose()
    }
}

function Restore-UserEnvironmentValue {
    [CmdletBinding()]
param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][object]$Snapshot
    )

    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Environment', $true)
    try {
        foreach ($existingName in @($key.GetValueNames())) {
            if ($existingName.Equals($Name, [StringComparison]::OrdinalIgnoreCase)) {
                $key.DeleteValue($existingName, $false)
            }
        }
        if ($Snapshot.Exists) {
            $kind = [Microsoft.Win32.RegistryValueKind]::$($Snapshot.Kind)
            $key.SetValue([string]$Snapshot.Name, $Snapshot.Value, $kind)
        }
    }
    finally {
        $key.Dispose()
    }
}

function Invoke-Deploy {
    Assert-EndfieldStopped

    if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) {
        throw "Game directory not found: $GameDir"
    }
    if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) {
        throw "Build directory not found: $BuildDir"
    }

    Assert-SourceArtifacts

    $gamePath = Get-CanonicalPath $GameDir
    $buildPath = Get-CanonicalPath $BuildDir
    $manifestPath = Join-Path $gamePath 'rendertest.json'
    $launcherWasRunning = Test-HypergryphLauncherRunning
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backupDir = Join-Path $gamePath "codex-dual-api-backup-$stamp"
    New-Item -ItemType Directory -Path $backupDir | Out-Null

    $backedUp = @()
    foreach ($name in $managedFiles) {
        $existing = Join-Path $gamePath $name
        if (Test-Path -LiteralPath $existing -PathType Leaf) {
            $backup = Join-Path $backupDir $name
            Copy-Item -LiteralPath $existing -Destination $backup -Force
            $sourceHash = (Get-FileHash -LiteralPath $existing -Algorithm SHA256).Hash
            $backupHash = (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash
            if ($sourceHash -ne $backupHash) {
                throw "Backup hash mismatch for $name"
            }
            $backedUp += $name
        }
    }

    $state = [ordered]@{
        Version = 3
        CreatedUtc = [DateTime]::UtcNow.ToString('o')
        GameDir = $gamePath
        BuildDir = $buildPath
        ManifestPath = $manifestPath
        BackupDir = $backupDir
        BackedUpFiles = $backedUp
        PreviousMachineRegistry = Get-LayerRegistrySnapshot `
            -Hive ([Microsoft.Win32.RegistryHive]::LocalMachine) `
            -ManifestPath $manifestPath
        PreviousUserRegistry = Get-LayerRegistrySnapshot `
            -Hive ([Microsoft.Win32.RegistryHive]::CurrentUser) `
            -ManifestPath $manifestPath
        PreviousUserEnable = Get-UserEnvironmentSnapshot -Name $enableVariable
        DeployedHashes = [ordered]@{}
    }
    $deploymentStatePath = Join-Path $backupDir 'deployment-state.json'
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $deploymentStatePath -Encoding UTF8

    foreach ($name in $managedFiles) {
        $existing = Join-Path $gamePath $name
        if (Test-Path -LiteralPath $existing -PathType Leaf) {
            Remove-Item -LiteralPath $existing -Force
        }
    }

    foreach ($name in @('dxgi.dll', 'rendertest.dll', 'rendertest.json')) {
        Copy-Item -LiteralPath (Join-Path $buildPath $name) `
            -Destination (Join-Path $gamePath $name) -Force
        $state.DeployedHashes[$name] =
            (Get-FileHash -LiteralPath (Join-Path $gamePath $name) -Algorithm SHA256).Hash
    }

    $scopedLauncherName = 'Launch-Endfield-RenderTest.ps1'
    $scopedLauncherPath = Join-Path $gamePath $scopedLauncherName
    Copy-Item -LiteralPath (Join-Path $root 'tools\Launch-Endfield-RenderTest.ps1') `
        -Destination $scopedLauncherPath -Force
    $state.DeployedHashes[$scopedLauncherName] =
        (Get-FileHash -LiteralPath $scopedLauncherPath -Algorithm SHA256).Hash

    $cmdLauncherName = 'Launch-Endfield-RenderTest.cmd'
    $cmdLauncherPath = Join-Path $gamePath $cmdLauncherName
    @'
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-Endfield-RenderTest.ps1"
if errorlevel 1 pause
'@ | Set-Content -LiteralPath $cmdLauncherPath -Encoding ASCII
    $state.DeployedHashes[$cmdLauncherName] =
        (Get-FileHash -LiteralPath $cmdLauncherPath -Algorithm SHA256).Hash

    Invoke-ElevatedMachineRegistryAction -Action Set -ManifestPath $manifestPath `
        -RequestDirectory $backupDir
    Remove-LayerRegistryValue -Hive ([Microsoft.Win32.RegistryHive]::CurrentUser) `
        -ManifestPath $manifestPath
    Remove-UserEnvironmentValue -Name $enableVariable
    Broadcast-EnvironmentChange

    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $deploymentStatePath -Encoding UTF8

    [pscustomobject]@{
        Mode = 'Deploy'
        GameDir = $gamePath
        BuildDir = $buildPath
        ManifestPath = $manifestPath
        CorePath = Join-Path $gamePath 'rendertest.dll'
        RegistryPath = 'HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers (64-bit)'
        RemovedUserRegistryDuplicate = $true
        EnableVariable = "$enableVariable=1 (Scoped process only)"
        ScopedLauncher = $cmdLauncherPath
        BackupDir = $backupDir
        StatePath = $deploymentStatePath
        RestartLauncherRequired = $launcherWasRunning
        DxgiSHA256 = $state.DeployedHashes['dxgi.dll']
        CoreSHA256 = $state.DeployedHashes['rendertest.dll']
    }
}

function Invoke-Rollback {
    if ([string]::IsNullOrWhiteSpace($StatePath)) {
        throw 'Rollback requires -StatePath <deployment-state.json>'
    }
    if (-not (Test-Path -LiteralPath $StatePath -PathType Leaf)) {
        throw "Rollback state not found: $StatePath"
    }

    $state = Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json
    $stateGameDir = Get-CanonicalPath ([string]$state.GameDir)
    $stateBackupDir = Get-CanonicalPath ([string]$state.BackupDir)
    $expectedBackupPrefix = $stateGameDir + '\codex-dual-api-backup-'
    if (-not $stateBackupDir.StartsWith($expectedBackupPrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Rollback backup is outside the expected game backup path: $stateBackupDir"
    }

    $script:GameDir = $stateGameDir
    Assert-EndfieldStopped

    $stateVersion = if ($state.PSObject.Properties.Name -contains 'Version') {
        [int]$state.Version
    }
    else {
        1
    }
    if ($stateVersion -ge 2) {
        if ($state.PSObject.Properties.Name -notcontains 'PreviousMachineRegistry' -or
            $null -eq $state.PreviousMachineRegistry) {
            throw 'Version 2-or-newer rollback state is missing PreviousMachineRegistry'
        }
        Invoke-ElevatedMachineRegistryAction -Action Restore `
            -ManifestPath ([string]$state.ManifestPath) `
            -RequestDirectory $stateBackupDir `
            -Snapshot $state.PreviousMachineRegistry
    }

    $displacedDir = Join-Path $stateBackupDir ('rollback-displaced-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Path $displacedDir | Out-Null
    $deployedFiles = @('dxgi.dll', 'rendertest.dll', 'rendertest.json')
    if ($stateVersion -ge 3) {
        $deployedFiles += @(
            'Launch-Endfield-RenderTest.ps1',
            'Launch-Endfield-RenderTest.cmd'
        )
    }
    foreach ($name in $deployedFiles) {
        $current = Join-Path $stateGameDir $name
        if (Test-Path -LiteralPath $current -PathType Leaf) {
            Move-Item -LiteralPath $current -Destination (Join-Path $displacedDir $name) -Force
        }
    }

    foreach ($name in @($state.BackedUpFiles)) {
        $backup = Join-Path $stateBackupDir ([string]$name)
        if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
            throw "Backup file is missing during rollback: $backup"
        }
        Copy-Item -LiteralPath $backup -Destination (Join-Path $stateGameDir ([string]$name)) -Force
    }

    if ($stateVersion -ge 2) {
        if ($state.PSObject.Properties.Name -notcontains 'PreviousUserRegistry' -or
            $null -eq $state.PreviousUserRegistry) {
            throw 'Version 2-or-newer rollback state is missing PreviousUserRegistry'
        }
        Restore-LayerRegistryValue -Hive ([Microsoft.Win32.RegistryHive]::CurrentUser) `
            -ManifestPath ([string]$state.ManifestPath) `
            -Snapshot $state.PreviousUserRegistry
    }
    else {
        Restore-LayerRegistryValue -Hive ([Microsoft.Win32.RegistryHive]::CurrentUser) `
            -ManifestPath ([string]$state.ManifestPath) `
            -Snapshot $state.PreviousRegistry
    }
    Restore-UserEnvironmentValue -Name $enableVariable -Snapshot $state.PreviousUserEnable
    Broadcast-EnvironmentChange

    [pscustomobject]@{
        Mode = 'Rollback'
        GameDir = $stateGameDir
        StatePath = (Resolve-Path -LiteralPath $StatePath).Path
        RestoredFiles = @($state.BackedUpFiles) -join ', '
        DisplacedCurrentFiles = $displacedDir
        MachineRegistryRestored = ($stateVersion -ge 2)
        UserRegistryRestored = $true
        EnvironmentRestored = $true
    }
}

if (-not [string]::IsNullOrWhiteSpace($RegistryRequestPath)) {
    Invoke-RegistryRequest -Path $RegistryRequestPath
}
elseif ($Mode -eq 'Deploy') {
    Invoke-Deploy
}
else {
    Invoke-Rollback
}
