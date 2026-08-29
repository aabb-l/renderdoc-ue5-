# PRAGMATA RenderDoc 快速部署指南

## 1. 目标与交付结果

本文用于从源码重新构建并部署 PRAGMATA 专用 RenderDoc。完成后应得到：

- 仓库构建输出中的 `dxgi.dll`：DXGI 代理。
- 仓库构建输出中的 `rendertest.dll`：RenderDoc 捕获核心。
- 仓库构建输出中的 `qrendertest.exe`：捕获和回放界面。
- 游戏目录中更新后的 `dxgi.dll` 与 `rendertest.dll`。
- `E:\PRAGMATA\config.ini` 中经过验证的 `[Render]` 配置。

游戏目录只部署两个 DLL。不要把 `qrendertest.exe`、PDB、LIB、日志或整个 `x64\Release` 目录复制到游戏目录。

## 2. 路径和变量

本文使用以下实际路径：

| 名称 | 当前值 | 含义 |
|---|---|---|
| `REPO_ROOT` | `E:\Project\RenderDoc\doc-nrc\renderdoc-ue5-github-validated` | 源码仓库 |
| `BUILD_ROOT` | `E:\Project\RenderDoc\doc-nrc\renderdoc-ue5-github-validated\x64\Release` | Release x64 构建输出 |
| `GAME_ROOT` | `E:\PRAGMATA` | `PRAGMATA.exe` 与 `config.ini` 所在目录 |
| `MSBUILD` | `C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe` | VS2022 MSBuild |

若机器或版本不同，先替换这些路径，再执行后续步骤。不要猜测游戏目录：代理 DLL 必须与实际启动的 `PRAGMATA.exe` 位于同一目录。

## 3. 前置条件

- Windows x64。
- Visual Studio 2022 Community，安装“使用 C++ 的桌面开发”工作负载和 v143 工具集。
- 仓库及其依赖完整可用。
- 使用 `pragmata` 分支进行 PRAGMATA 部署。
- 能正常读取和写入 `E:\PRAGMATA`。
- 部署前由人手动退出 PRAGMATA、启动器中的游戏进程以及 `qrendertest.exe`。

不要用脚本强制结束游戏或 RenderDoc 工具。若文件仍被占用，停止部署并让使用者确认相关程序已经正常退出。

全新克隆的仓库还需要初始化当前唯一的子模块 `renderdoc/3rdparty/minhook`：

~~~powershell
git submodule update --init --recursive
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
Set-Location -LiteralPath 'E:\Project\RenderDoc\doc-nrc\renderdoc-ue5-github-validated'
git branch --show-current
git status --short
~~~

期望：

- 当前分支为 `pragmata`。
- 工作区没有无法解释的源码改动。

如果需要切换分支，先确认现有修改已经妥善保存，再执行：

~~~powershell
git switch pragmata
~~~

若要同步远端，使用快进更新：

~~~powershell
git pull --ff-only origin pragmata
~~~

拉取失败或提示分叉时停止，不要自动 rebase、强制推送或重置分支。

## 6. 全量编译 DLL 和 EXE

### 6.1 执行 Release x64 Rebuild

~~~powershell
$repoRoot = 'E:\Project\RenderDoc\doc-nrc\renderdoc-ue5-github-validated'
$msbuildPath = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'

Set-Location -LiteralPath $repoRoot
& $msbuildPath '.\renderdoc.sln' '/t:Rebuild' '/m' '/p:Configuration=Release' '/p:Platform=x64' '/v:minimal' '/nologo'
if ($LASTEXITCODE -ne 0) { throw "Release x64 Rebuild 失败，退出码：$LASTEXITCODE" }
~~~

必须使用 `Rebuild` 并等待退出码为 0。只看到部分 DLL 已生成不代表整套构建完成。

### 6.2 核对必要产物

~~~powershell
$buildRoot = 'E:\Project\RenderDoc\doc-nrc\renderdoc-ue5-github-validated\x64\Release'
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
| `dxgi.dll` | 复制到 `E:\PRAGMATA` |
| `rendertest.dll` | 复制到 `E:\PRAGMATA` |
| `qrendertest.exe` | 直接从 `x64\Release` 运行，不复制到游戏目录 |

## 7. 备份现有部署和配置

先手动退出游戏和 `qrendertest.exe`。下面的脚本会完成四件事：

1. 记录源码 Git 提交号。
2. 记录 `config.ini`、`dxgi.dll`、`dxgi.dll.tmp` 和 `rendertest.dll` 在部署前是否存在。
3. 复制需要保留的现有文件。
4. 将遗留的 `dxgi.dll.tmp` 移出游戏目录，避免下次代理重命名冲突。

~~~powershell
$repoRoot = 'E:\Project\RenderDoc\doc-nrc\renderdoc-ue5-github-validated'
$gameRoot = 'E:\PRAGMATA'
$backupStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupBase = 'E:\PRAGMATA-backups'
$backupRoot = Join-Path $backupBase "predeploy-$backupStamp"
New-Item -ItemType Directory -Path $backupRoot -Force -ErrorAction Stop | Out-Null

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
  createdAt = (Get-Date).ToString('o')
  sourceCommit = $sourceCommit
  files = @($fileStates)
}

foreach ($name in @('config.ini', 'dxgi.dll', 'rendertest.dll')) {
  $sourcePath = Join-Path $gameRoot $name
  if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $backupRoot $name) -Force -ErrorAction Stop
  }
}

$oldProxyTemp = Join-Path $gameRoot 'dxgi.dll.tmp'
if (Test-Path -LiteralPath $oldProxyTemp -PathType Leaf) {
  Move-Item -LiteralPath $oldProxyTemp -Destination (Join-Path $backupRoot 'dxgi.dll.tmp') -Force -ErrorAction Stop
}

Set-Content -LiteralPath (Join-Path $backupRoot 'source-commit.txt') -Value $sourceCommit -Encoding ascii -ErrorAction Stop
$deploymentState | ConvertTo-Json -Depth 4 |
  Set-Content -LiteralPath (Join-Path $backupRoot 'deployment-state.json') -Encoding utf8 -ErrorAction Stop

"源码提交：$sourceCommit"
"备份目录：$backupRoot"
~~~

备份目录放在游戏目录之外，避免备份 DLL 与游戏运行目录混在一起。保存输出的源码提交号和备份目录；第 13 节回滚会依赖 `deployment-state.json`。如果复制或移动失败，文件可能仍被占用，此时停止，不要强制结束进程或继续覆盖 DLL。

## 8. 定制 `config.ini`

打开：

~~~text
E:\PRAGMATA\config.ini
~~~

仅将 `[Render]` 段替换为：

~~~ini
[Render]
AllowMeshShader=Disable
Capability=DirectX12
ForceMeshShader=Disable
ParallelBuildCommandList=Disable
ParallelBuildProcessorCount=0
RenderWorkerThreadPriorityAboveNormal=Enable
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

## 9. 部署 DLL

再次确认游戏和工具已退出，然后执行：

~~~powershell
$buildRoot = 'E:\Project\RenderDoc\doc-nrc\renderdoc-ue5-github-validated\x64\Release'
$gameRoot = 'E:\PRAGMATA'

Copy-Item -LiteralPath (Join-Path $buildRoot 'dxgi.dll') -Destination (Join-Path $gameRoot 'dxgi.dll') -Force -ErrorAction Stop
Copy-Item -LiteralPath (Join-Path $buildRoot 'rendertest.dll') -Destination (Join-Path $gameRoot 'rendertest.dll') -Force -ErrorAction Stop
~~~

部署后只做文件存在性、大小和时间检查：

~~~powershell
Get-Item -LiteralPath 'E:\PRAGMATA\dxgi.dll', 'E:\PRAGMATA\rendertest.dll' |
  Select-Object FullName, Length, LastWriteTime
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
$gameRoot = 'E:\PRAGMATA'
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
   & 'E:\Project\RenderDoc\doc-nrc\renderdoc-ue5-github-validated\x64\Release\qrendertest.exe'
   ~~~

3. 在 Attach/进程列表中选择实际的 PRAGMATA 游戏进程。
4. 确认捕获 API 为 D3D12。
5. 进入可稳定复现的场景并触发捕获。
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
E:\PRAGMATA-backups\predeploy-YYYYMMDD-HHMMSS
~~~

恢复步骤：

1. 找到本次部署前创建的确切备份目录。
2. 验证 `deployment-state.json` 存在并完整记录四个受管文件。
3. 把当前部署文件移到新的回滚暂存目录，不直接删除或覆盖。
4. 只恢复部署前原本存在的文件；原本不存在的文件保持移出状态。
5. 正常启动游戏，确认回到部署前状态。

示例脚本会保留备份目录，并把回滚前的当前文件放在其子目录中，因此仍可人工恢复：

~~~powershell
$gameRoot = 'E:\PRAGMATA'
$backupRoot = 'E:\PRAGMATA-backups\predeploy-YYYYMMDD-HHMMSS'
$statePath = Join-Path $backupRoot 'deployment-state.json'
$expectedNames = @('config.ini', 'dxgi.dll', 'dxgi.dll.tmp', 'rendertest.dll')

if (-not (Test-Path -LiteralPath $backupRoot -PathType Container)) {
  throw "备份目录不存在：$backupRoot"
}
if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
  throw "部署状态文件不存在：$statePath"
}

$deploymentState = Get-Content -LiteralPath $statePath -Raw -ErrorAction Stop | ConvertFrom-Json
$recordedNames = @($deploymentState.files | ForEach-Object { $_.name })
foreach ($name in $expectedNames) {
  if ($recordedNames -notcontains $name) {
    throw "部署状态缺少文件记录：$name"
  }
}

foreach ($fileState in $deploymentState.files) {
  if ($fileState.existed) {
    $backupPath = Join-Path $backupRoot $fileState.name
    if (-not (Test-Path -LiteralPath $backupPath -PathType Leaf)) {
      throw "原文件标记为存在，但备份缺失：$backupPath"
    }
  }
}

$rollbackStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$rollbackRoot = Join-Path $backupRoot "rollback-current-$rollbackStamp"
New-Item -ItemType Directory -Path $rollbackRoot -Force -ErrorAction Stop | Out-Null

foreach ($name in $expectedNames) {
  $currentPath = Join-Path $gameRoot $name
  if (Test-Path -LiteralPath $currentPath -PathType Leaf) {
    Move-Item -LiteralPath $currentPath -Destination (Join-Path $rollbackRoot $name) -Force -ErrorAction Stop
  }
}

foreach ($fileState in $deploymentState.files) {
  if ($fileState.existed) {
    Copy-Item -LiteralPath (Join-Path $backupRoot $fileState.name) `
      -Destination (Join-Path $gameRoot $fileState.name) -Force -ErrorAction Stop
  }
}

"回滚前文件暂存目录：$rollbackRoot"
~~~

执行前必须把示例中的 `YYYYMMDD-HHMMSS` 替换为真实备份目录名。部署前不存在的 DLL 不会被恢复；如果部署前存在 `dxgi.dll.tmp`，脚本会按状态记录把它恢复。

## 14. 分支和文档边界

- 代理生产代码应同时存在于 `pragmata` 与默认分支 `v1.x`。
- 本文和《PRAGMATA 定制说明》只属于 `pragmata`。
- `v1.x` 不包含 PRAGMATA 文档。
