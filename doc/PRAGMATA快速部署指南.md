# PRAGMATA RenderDoc 快速部署指南

## 1. 目标与交付结果

本文用于从源码重新构建并部署 PRAGMATA 专用 RenderDoc。完成后应得到：

- 仓库构建输出中的 `dxgi.dll`：DXGI 代理。
- 仓库构建输出中的 `rendertest.dll`：RenderDoc 捕获核心。
- 仓库构建输出中的 `qrendertest.exe`：捕获和回放界面。
- 游戏目录中更新后的 `dxgi.dll` 与 `rendertest.dll`。
- 游戏目录下 `config.ini` 中经过验证的 `[Render]` 配置。

游戏目录只部署两个 DLL。不要把 `qrendertest.exe`、PDB、LIB、日志或整个 `x64\Release` 目录复制到游戏目录。

## 2. 路径和变量

本文不记录任何机器的真实目录。先在同一个 PowerShell 会话中定义以下变量，后续命令均复用它们：

~~~powershell
$repoRoot = '<REPO_ROOT>'
$gameRoot = '<GAME_ROOT>'
$backupBase = '<BACKUP_BASE>'

$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswherePath -PathType Leaf)) {
  throw "找不到 vswhere.exe：$vswherePath"
}

$msbuildPath = & $vswherePath -latest -products * -version '[17.0,18.0)' `
  -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuildPath -or -not (Test-Path -LiteralPath $msbuildPath -PathType Leaf)) {
  throw '找不到同时包含 MSBuild 和 x64 C++ 工具的 Visual Studio 2022'
}

foreach ($value in @($repoRoot, $gameRoot, $backupBase)) {
  if ([string]::IsNullOrWhiteSpace($value) -or $value -match '^<.+>$') {
    throw '必须先把 REPO_ROOT、GAME_ROOT 和 BACKUP_BASE 占位符替换为实际目录'
  }
}

function ConvertTo-AbsoluteDirectoryPath {
  param([Parameter(Mandatory)][string]$Path)

  $fullPath = [System.IO.Path]::GetFullPath($Path)
  $pathRoot = [System.IO.Path]::GetPathRoot($fullPath)
  if ($fullPath.Length -gt $pathRoot.Length) {
    return $fullPath.TrimEnd('\', '/')
  }
  return $fullPath
}

$repoRoot = ConvertTo-AbsoluteDirectoryPath $repoRoot
$gameRoot = ConvertTo-AbsoluteDirectoryPath $gameRoot
$backupBase = ConvertTo-AbsoluteDirectoryPath $backupBase
$buildRoot = Join-Path $repoRoot 'x64\Release'

if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'renderdoc.sln') -PathType Leaf)) {
  throw "仓库目录无效，缺少 renderdoc.sln：$repoRoot"
}
if (-not (Test-Path -LiteralPath (Join-Path $gameRoot 'PRAGMATA.exe') -PathType Leaf)) {
  throw "游戏目录无效，缺少 PRAGMATA.exe：$gameRoot"
}
if (-not (Test-Path -LiteralPath (Join-Path $gameRoot 'config.ini') -PathType Leaf)) {
  throw "游戏目录无效，缺少 config.ini：$gameRoot"
}

$pathComparison = [System.StringComparison]::OrdinalIgnoreCase
$gameRootPrefix = if ($gameRoot.EndsWith('\') -or $gameRoot.EndsWith('/')) {
  $gameRoot
} else {
  "$gameRoot\"
}
if ($backupBase.Equals($gameRoot, $pathComparison) -or
    $backupBase.StartsWith($gameRootPrefix, $pathComparison)) {
  throw 'BACKUP_BASE 必须位于游戏目录之外'
}
~~~

占位符含义：

| 占位符 | 含义 |
|---|---|
| `<REPO_ROOT>` | 本仓库根目录，即包含 `renderdoc.sln` 的目录 |
| `<GAME_ROOT>` | `PRAGMATA.exe` 与 `config.ini` 所在目录 |
| `<BACKUP_BASE>` | 游戏目录之外、用于保存部署备份的目录 |
| `<PREDEPLOY_BACKUP_ROOT>` | 某次完整部署备份的确切目录，即第 7 节备份命令输出的路径 |

执行前必须替换三个占位符。初始化脚本会立即把它们规范化为绝对路径，再派生 `$buildRoot`；后续切换当前目录不会改变路径含义。脚本还会验证仓库、游戏和备份目录，避免对错误目录执行构建或部署。代理 DLL 必须与实际启动的 `PRAGMATA.exe` 位于同一目录。MSBuild 由 Visual Studio Installer 自带的 `vswhere.exe` 自动查找，不依赖固定安装目录。

## 3. 前置条件

- Windows x64。
- Visual Studio 2022（任意版本类别），安装 MSBuild、“使用 C++ 的桌面开发”工作负载和 v143 x64 工具集。
- 仓库及其依赖完整可用。
- 使用 `pragmata` 分支进行 PRAGMATA 部署。
- 能正常读取和写入 `$gameRoot` 指向的游戏目录。
- 部署前由人手动退出 PRAGMATA、启动器中的游戏进程以及 `qrendertest.exe`。

不要用脚本强制结束游戏或 RenderDoc 工具。若文件仍被占用，停止部署并让使用者确认相关程序已经正常退出。

全新克隆的仓库还需要初始化当前唯一的子模块 `renderdoc/3rdparty/minhook`：

~~~powershell
git -C $repoRoot submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw "子模块初始化失败，退出码：$LASTEXITCODE" }
~~~

若需要补装编译组件或排查环境问题，参见 [RenderDoc 官方编译说明](../docs/CONTRIBUTING/Compiling.md)。

## 4. AI 执行约束

AI 或自动化工具执行本文时必须遵守：

1. 在现有仓库中操作，不创建临时 worktree。
2. 不运行 `git reset --hard`、`git clean` 或其他会丢失现有工作区改动的命令。
3. 发现非预期源码改动时停止并报告，不自动丢弃。
4. 只构建 Release x64。
5. 只部署 `dxgi.dll` 和 `rendertest.dll`。
6. 不强制结束进程。
7. 不生成或提交测试源码、二进制或部署副本。
8. 验证限定为构建退出码、必要文件、启动、捕获、打开和绘制完整性，不增加无关检查。
9. 修改 `config.ini` 前必须备份，并且只修改 `[Render]` 段。
10. 每一步失败后停止，不继续覆盖后续文件。

## 5. 检查源码状态

在 PowerShell 中执行：

~~~powershell
$currentBranch = (git -C $repoRoot branch --show-current).Trim()
if ($LASTEXITCODE -ne 0) { throw '无法读取当前分支' }
if ($currentBranch -ne 'pragmata') { throw "当前分支不是 pragmata：$currentBranch" }

$gitStatus = @(git -C $repoRoot status --porcelain)
if ($LASTEXITCODE -ne 0) { throw '无法读取工作区状态' }
if ($gitStatus.Count -ne 0) {
  $gitStatus | Write-Output
  throw '构建前工作区必须完全干净，包括子模块状态和未跟踪文件'
}
~~~

期望：

- 当前分支为 `pragmata`。
- 工作区完全干净；提交号只有在不混入未提交源码、子模块改动或未跟踪文件时，才能准确标识本次构建状态。

如果需要切换分支，先确认现有修改已经妥善保存，再执行：

~~~powershell
git -C $repoRoot switch pragmata
~~~

若要同步远端，使用快进更新：

~~~powershell
git -C $repoRoot pull --ff-only origin pragmata
~~~

拉取失败或提示分叉时停止，不要自动 rebase、强制推送或重置分支。

## 6. 全量编译 DLL 和 EXE

### 6.1 执行 Release x64 Rebuild

~~~powershell
$gitStatus = @(git -C $repoRoot status --porcelain)
if ($LASTEXITCODE -ne 0 -or $gitStatus.Count -ne 0) {
  throw 'Rebuild 前工作区不是干净状态'
}

Set-Location -LiteralPath $repoRoot
& $msbuildPath '.\renderdoc.sln' '/t:Rebuild' '/m' '/p:Configuration=Release' '/p:Platform=x64' '/v:minimal' '/nologo'
if ($LASTEXITCODE -ne 0) { throw "Release x64 Rebuild 失败，退出码：$LASTEXITCODE" }
~~~

必须使用 `Rebuild` 并等待退出码为 0。只看到部分 DLL 已生成不代表整套构建完成。

### 6.2 核对必要产物

~~~powershell
$requiredOutputs = @('dxgi.dll', 'rendertest.dll', 'qrendertest.exe')

foreach ($name in $requiredOutputs) {
  $path = Join-Path $buildRoot $name
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "缺少构建产物：$path"
  }
  Get-Item -LiteralPath $path | Select-Object Name, Length, LastWriteTime
}
~~~

三个文件的用途：

| 文件 | 使用位置 |
|---|---|
| `dxgi.dll` | 复制到 `$gameRoot` |
| `rendertest.dll` | 复制到 `$gameRoot` |
| `qrendertest.exe` | 直接从 `x64\Release` 运行，不复制到游戏目录 |

## 7. 备份现有部署和配置

先手动退出游戏和 `qrendertest.exe`。下面的脚本会完成四件事：

1. 记录源码 Git 提交号。
2. 记录 `config.ini`、`dxgi.dll`、`dxgi.dll.tmp` 和 `rendertest.dll` 在部署前是否存在。
3. 复制需要保留的现有文件。
4. 将遗留的 `dxgi.dll.tmp` 移出游戏目录，避免下次代理重命名冲突。

~~~powershell
$backupStamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$backupRoot = Join-Path $backupBase "predeploy-$backupStamp"
if (Test-Path -LiteralPath $backupBase -PathType Leaf) {
  throw "备份基准路径是文件而不是目录：$backupBase"
}
if (-not (Test-Path -LiteralPath $backupBase -PathType Container)) {
  New-Item -ItemType Directory -Path $backupBase -ErrorAction Stop | Out-Null
}
New-Item -ItemType Directory -Path $backupRoot -ErrorAction Stop | Out-Null

$sourceCommit = git -C $repoRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or -not $sourceCommit) {
  throw '无法读取源码 Git 提交号'
}
$sourceCommit = $sourceCommit.Trim()

$trackedNames = @('config.ini', 'dxgi.dll', 'dxgi.dll.tmp', 'rendertest.dll')
$fileStates = foreach ($name in $trackedNames) {
  $sourcePath = Join-Path $gameRoot $name
  [ordered]@{
    name = $name
    existed = [bool](Test-Path -LiteralPath $sourcePath -PathType Leaf)
  }
}

$deploymentState = [ordered]@{
  deploymentId = [guid]::NewGuid().ToString('D')
  createdAt = (Get-Date).ToString('o')
  sourceCommit = $sourceCommit
  gameRoot = $gameRoot
  backupRoot = $backupRoot
  files = @($fileStates)
}

foreach ($name in $trackedNames) {
  $sourcePath = Join-Path $gameRoot $name
  if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $backupRoot $name) -ErrorAction Stop
  }
}

Set-Content -LiteralPath (Join-Path $backupRoot 'source-commit.txt') -Value $sourceCommit -Encoding ascii -ErrorAction Stop
$stateTempPath = Join-Path $backupRoot 'deployment-state.json.tmp'
$statePath = Join-Path $backupRoot 'deployment-state.json'
$deploymentState | ConvertTo-Json -Depth 4 |
  Set-Content -LiteralPath $stateTempPath -Encoding utf8 -ErrorAction Stop
Move-Item -LiteralPath $stateTempPath -Destination $statePath -ErrorAction Stop

"源码提交：$sourceCommit"
"备份目录：$backupRoot"

$oldProxyTemp = Join-Path $gameRoot 'dxgi.dll.tmp'
if (Test-Path -LiteralPath $oldProxyTemp -PathType Leaf) {
  Move-Item -LiteralPath $oldProxyTemp `
    -Destination (Join-Path $backupRoot 'removed-dxgi.dll.tmp') -ErrorAction Stop
}
~~~

备份目录放在游戏目录之外，避免备份 DLL 与游戏运行目录混在一起。脚本先复制四个受管文件并原子写入部署状态，再移动游戏目录中的遗留 `dxgi.dll.tmp`；因此元数据落盘失败时不会先改变游戏目录。保存输出的源码提交号和备份目录；第 13 节回滚会依赖 `deployment-state.json`。如果复制或移动失败，文件可能仍被占用，此时停止，不要强制结束进程或继续覆盖 DLL。若最后移动 `dxgi.dll.tmp` 失败，备份和状态文件仍然有效，但不得继续部署。

## 8. 定制 `config.ini`

打开：

~~~text
<GAME_ROOT>\config.ini
~~~

仅将 `[Render]` 段替换为：

~~~ini
[Render]
AllowMeshShader=Disable
Capability=DirectX12
ForceMeshShader=Disable
ParallelBuildCommandList=Disable
ParallelBuildProcessorCount=0
RenderWorkerThreadPriorityAboveNormal=Disable
TightFitShaderCache=Disable
UseComputeQueuePairing=Disable
UsingIndepentRenderWorker=Disable
~~~

注意：

- 不删除其他 INI 段。
- 不把 `UsingIndepentRenderWorker` 改成其他拼写。
- 不同时保留两个 `[Render]` 段。
- 修改后重新读取该段，逐行确认 9 个键和值。
- 配置变化只影响之后新启动的游戏和新生成的捕获，不会修复已有 `.rdc`。
- 截帧前建议使用游戏内帧率上限或显卡驱动设置，将帧率限制为稳定的 30 FPS。较低且稳定的帧率通常能减少捕获期间的命令量、资源变化和瞬时调度压力。该设置是稳定性建议，不是代理工作的硬性条件；修改帧率限制后，应重启游戏再生成新捕获。

## 9. 部署 DLL

再次确认游戏和工具已退出，然后执行：

~~~powershell
try {
  Copy-Item -LiteralPath (Join-Path $buildRoot 'dxgi.dll') `
    -Destination (Join-Path $gameRoot 'dxgi.dll') -Force -ErrorAction Stop
  Copy-Item -LiteralPath (Join-Path $buildRoot 'rendertest.dll') `
    -Destination (Join-Path $gameRoot 'rendertest.dll') -Force -ErrorAction Stop
} catch {
  Write-Error 'DLL 部署未完整完成。不要启动游戏；请使用第 7 节生成的备份按第 13 节回滚。'
  throw
}
~~~

部署后只做源文件与目标文件的存在性、大小和时间检查：

~~~powershell
$deployNames = @('dxgi.dll', 'rendertest.dll')
foreach ($name in $deployNames) {
  $sourceItem = Get-Item -LiteralPath (Join-Path $buildRoot $name) -ErrorAction Stop
  $targetItem = Get-Item -LiteralPath (Join-Path $gameRoot $name) -ErrorAction Stop
  if ($sourceItem.Length -ne $targetItem.Length) {
    throw "部署文件大小与构建产物不一致：$name"
  }
  [pscustomobject]@{
    Name = $name
    SourceLength = $sourceItem.Length
    TargetLength = $targetItem.Length
    SourceTime = $sourceItem.LastWriteTime
    TargetTime = $targetItem.LastWriteTime
  }
}
~~~

不要从其他仓库或旧发布目录混用 DLL 与 EXE；`dxgi.dll`、`rendertest.dll` 和 `qrendertest.exe` 应来自同一次源码状态和同一次 Release x64 重编译。

### 9.1 后续启动前处理代理文件

游戏运行时，代理会尝试把自身从 `dxgi.dll` 重命名为 `dxgi.dll.tmp`。不要在游戏运行中恢复或覆盖代理。游戏正常退出后、下次启动前按文件状态处理：

- 只有 `dxgi.dll`：可以直接启动。
- 只有 `dxgi.dll.tmp`：把它恢复为 `dxgi.dll`，或者重新部署本次构建的 `dxgi.dll`。
- 两者同时存在：停止操作，将两者分别备份到游戏目录之外，再重新部署；不要直接覆盖其中任意一个。
- 两者都不存在：重新部署本次构建的 `dxgi.dll`。

只有 `dxgi.dll.tmp` 时可执行：

~~~powershell
$proxyPath = Join-Path $gameRoot 'dxgi.dll'
$proxyTempPath = Join-Path $gameRoot 'dxgi.dll.tmp'

if ((Test-Path -LiteralPath $proxyTempPath -PathType Leaf) -and
    -not (Test-Path -LiteralPath $proxyPath -PathType Leaf)) {
  Move-Item -LiteralPath $proxyTempPath -Destination $proxyPath -ErrorAction Stop
}
~~~

## 10. 启动、捕获和打开

1. 正常启动 PRAGMATA。
2. 从当前构建输出运行：

   ~~~powershell
   & (Join-Path $buildRoot 'qrendertest.exe')
   ~~~

3. 在 Attach/进程列表中选择实际的 PRAGMATA 游戏进程。
4. 确认捕获 API 为 D3D12。
5. 建议先将游戏帧率限制为稳定的 30 FPS，重启游戏后进入可稳定复现的场景并触发捕获。
6. 在同一个 `qrendertest.exe` 中打开新生成的 `.rdc`。
7. 检查主要场景、角色、特效、阴影和后处理是否存在，切换多个事件确认绘制不是只在单帧视图中被隐藏。

新的代理逻辑不再要求进程名包含 `NRC-Win64-Shipping`。同时，`qrendertest.exe` 等工具进程会被精确排除，不会再次加载 `rendertest.dll`。

## 11. 验收清单

全部满足后才算部署完成：

- [ ] Release x64 全量 Rebuild 退出码为 0。
- [ ] `dxgi.dll`、`rendertest.dll`、`qrendertest.exe` 均来自本次构建。
- [ ] 部署前文件状态、源码 Git 提交号、旧 DLL、`dxgi.dll.tmp` 和 `config.ini` 已记录或备份。
- [ ] 只向游戏目录部署了 `dxgi.dll` 与 `rendertest.dll` 两个构建产物；`config.ini` 作为配置单独修改。
- [ ] `config.ini` 的 `[Render]` 段与本文完全一致。
- [ ] PRAGMATA 可以正常启动。
- [ ] 工具可以识别 D3D12 进程并触发捕获。
- [ ] 截帧场景已建议限制为稳定的 30 FPS。
- [ ] 新生成的捕获可以打开。
- [ ] 主要绘制、资源和后处理没有明显缺失。

## 12. 故障排查

### 12.1 部署时提示文件被占用

- 停止部署。
- 让使用者手动正常退出 PRAGMATA 和 `qrendertest.exe`。
- 不要强制结束进程。
- 文件释放后，从备份步骤重新开始。

### 12.2 游戏部署后无法启动

- 不继续反复覆盖。
- 按第 13 节恢复旧 DLL 和配置。
- 确认代理位于实际 `PRAGMATA.exe` 所在目录。
- 确认两个 DLL 与 `qrendertest.exe` 来自同一次构建。

### 12.3 可以捕获，但 `.rdc` 无法打开

- 使用本次构建的 `x64\Release\qrendertest.exe`，不要混用旧 UI。
- 确认工具 EXE 保持 `qrendertest.exe` 名称；若自行重命名，必须同步更新源码中的工具排除名单并重编译。
- 重新启动游戏后生成一份新捕获；不要用配置变化判断旧捕获。

### 12.4 捕获可以打开，但缺少大量绘制

- 逐项比对本文的 `[Render]` 段。
- 特别确认 `ParallelBuildCommandList=Disable`、`ParallelBuildProcessorCount=0`、`UseComputeQueuePairing=Disable` 和 `UsingIndepentRenderWorker=Disable`。
- 完全退出游戏后重新启动并重新捕获。
- 一次只改变一个配置项；记录“配置值、场景、捕获能否打开、缺失内容”，避免同时改变多个变量后无法定位原因。

### 12.5 更新后代理没有进入目标进程

- 确认部署目录中实际启动的是 `PRAGMATA.exe`。
- 确认 `dxgi.dll` 与该 EXE 同目录。
- 确认源码中的过滤规则只排除四个 RenderDoc 工具名，没有把 PRAGMATA 加入排除名单。
- 对照构建时间，确认部署的不是旧 `dxgi.dll`。

## 13. 回滚

先手动退出游戏和工具。假设备份目录为：

~~~text
<BACKUP_BASE>\predeploy-YYYYMMDD-HHMMSS-fff
~~~

恢复步骤：

1. 找到本次部署前创建的确切备份目录。
2. 验证 `deployment-state.json` 存在并完整记录四个受管文件。
3. 把当前部署文件移到新的回滚暂存目录，不直接删除或覆盖。
4. 只恢复部署前原本存在的文件；原本不存在的文件保持移出状态。
5. 正常启动游戏，确认回到部署前状态。

示例脚本会保留备份目录，并把回滚前的当前文件放在其子目录中，因此仍可人工恢复：

~~~powershell
$gameRoot = '<GAME_ROOT>'
$backupRoot = '<PREDEPLOY_BACKUP_ROOT>'
$expectedNames = @('config.ini', 'dxgi.dll', 'dxgi.dll.tmp', 'rendertest.dll')

foreach ($value in @($gameRoot, $backupRoot)) {
  if ([string]::IsNullOrWhiteSpace($value) -or $value -match '^<.+>$') {
    throw '必须先替换 GAME_ROOT 和 PREDEPLOY_BACKUP_ROOT 占位符'
  }
}

function ConvertTo-AbsoluteDirectoryPath {
  param([Parameter(Mandatory)][string]$Path)

  $fullPath = [System.IO.Path]::GetFullPath($Path)
  $pathRoot = [System.IO.Path]::GetPathRoot($fullPath)
  if ($fullPath.Length -gt $pathRoot.Length) {
    return $fullPath.TrimEnd('\', '/')
  }
  return $fullPath
}

$gameRoot = ConvertTo-AbsoluteDirectoryPath $gameRoot
$backupRoot = ConvertTo-AbsoluteDirectoryPath $backupRoot
$statePath = Join-Path $backupRoot 'deployment-state.json'
$pathComparison = [System.StringComparison]::OrdinalIgnoreCase
$gameRootPrefix = if ($gameRoot.EndsWith('\') -or $gameRoot.EndsWith('/')) {
  $gameRoot
} else {
  "$gameRoot\"
}

if (-not (Test-Path -LiteralPath (Join-Path $gameRoot 'PRAGMATA.exe') -PathType Leaf)) {
  throw "游戏目录无效，缺少 PRAGMATA.exe：$gameRoot"
}
if ($backupRoot.Equals($gameRoot, $pathComparison) -or
    $backupRoot.StartsWith($gameRootPrefix, $pathComparison)) {
  throw 'PREDEPLOY_BACKUP_ROOT 必须位于游戏目录之外'
}
if (-not (Test-Path -LiteralPath $backupRoot -PathType Container)) {
  throw "备份目录不存在：$backupRoot"
}
if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
  throw "部署状态文件不存在：$statePath"
}

$deploymentState = Get-Content -LiteralPath $statePath -Raw -ErrorAction Stop | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace([string]$deploymentState.deploymentId)) {
  throw '部署状态缺少 deploymentId'
}
$parsedDeploymentId = [guid]::Empty
if (-not [guid]::TryParse([string]$deploymentState.deploymentId, [ref]$parsedDeploymentId)) {
  throw '部署状态中的 deploymentId 不是有效 GUID'
}
if ([string]::IsNullOrWhiteSpace([string]$deploymentState.gameRoot) -or
    -not $gameRoot.Equals([string]$deploymentState.gameRoot, $pathComparison)) {
  throw "部署状态中的 gameRoot 与当前游戏目录不一致：$($deploymentState.gameRoot)"
}
if ([string]::IsNullOrWhiteSpace([string]$deploymentState.backupRoot) -or
    -not $backupRoot.Equals([string]$deploymentState.backupRoot, $pathComparison)) {
  throw "部署状态中的 backupRoot 与当前备份目录不一致：$($deploymentState.backupRoot)"
}

$fileStates = @($deploymentState.files)
if ($fileStates.Count -ne $expectedNames.Count) {
  throw "部署状态文件记录数错误：应为 $($expectedNames.Count)，实际为 $($fileStates.Count)"
}

$recordedNames = @($fileStates | ForEach-Object { $_.name })
if (@($recordedNames | Select-Object -Unique).Count -ne $recordedNames.Count) {
  throw '部署状态文件包含重复的文件名'
}
foreach ($name in $expectedNames) {
  if ($recordedNames -notcontains $name) {
    throw "部署状态缺少文件记录：$name"
  }
}

foreach ($fileState in $fileStates) {
  if ($expectedNames -notcontains $fileState.name) {
    throw "部署状态包含非预期文件：$($fileState.name)"
  }
  if ($null -eq $fileState.existed -or $fileState.existed -isnot [bool]) {
    throw "部署状态的 existed 不是布尔值：$($fileState.name)"
  }
  if ($fileState.existed) {
    $backupPath = Join-Path $backupRoot $fileState.name
    if (-not (Test-Path -LiteralPath $backupPath -PathType Leaf)) {
      throw "原文件标记为存在，但备份缺失：$backupPath"
    }
  }
}

$rollbackStamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$rollbackRoot = Join-Path $backupRoot "rollback-current-$rollbackStamp"
New-Item -ItemType Directory -Path $rollbackRoot -ErrorAction Stop | Out-Null

foreach ($name in $expectedNames) {
  $currentPath = Join-Path $gameRoot $name
  if (Test-Path -LiteralPath $currentPath -PathType Leaf) {
    Move-Item -LiteralPath $currentPath -Destination (Join-Path $rollbackRoot $name) -ErrorAction Stop
  }
}

foreach ($fileState in $fileStates) {
  if ($fileState.existed) {
    Copy-Item -LiteralPath (Join-Path $backupRoot $fileState.name) `
      -Destination (Join-Path $gameRoot $fileState.name) -ErrorAction Stop
  }
}

"回滚前文件暂存目录：$rollbackRoot"
~~~

执行前必须替换 `<GAME_ROOT>` 和 `<PREDEPLOY_BACKUP_ROOT>`；后者应使用第 7 节输出的确切备份目录。部署前不存在的 DLL 不会被恢复；如果部署前存在 `dxgi.dll.tmp`，脚本会按状态记录把它恢复。回滚中的移动和复制不是跨文件事务；任一步失败时应立即停止，保留备份目录与脚本已经输出的回滚暂存目录，再人工核对四个受管文件，不要继续启动游戏或重复覆盖。

## 14. 分支和文档边界

- 代理生产代码应同时存在于 `pragmata` 与默认分支 `v1.x`。
- 本文和《PRAGMATA 定制说明》只属于 `pragmata`。
- `v1.x` 不包含 PRAGMATA 文档。
