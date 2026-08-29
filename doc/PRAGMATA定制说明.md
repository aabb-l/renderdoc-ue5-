# PRAGMATA RenderDoc 定制说明

## 1. 文档目的

本文说明本仓库为 PRAGMATA 做的两类定制：

1. DXGI 代理不再依赖固定游戏进程名，并避免在 RenderDoc 自身工具进程中再次加载捕获组件。
2. PRAGMATA 的 `config.ini` 使用一组偏向稳定截帧与回放的渲染配置。

本文记录的是 2026-08-30 已完成 Release x64 全量重编译并实际用于 PRAGMATA 的基线。它用于后续升级、复现和排查，不代表这些游戏配置项的内部实现已经由游戏源码验证。

本文不记录任何机器的真实目录。示例中的占位符含义如下：

| 占位符 | 含义 |
|---|---|
| `<GAME_ROOT>` | `PRAGMATA.exe` 与 `config.ini` 所在目录 |
| `<BACKUP_BASE>` | 游戏目录之外、用于保存配置备份的目录 |
| `<CONFIG_BACKUP_ROOT>` | 某次配置备份的确切目录，即备份命令输出的路径 |

执行命令前必须先把占位符替换为当前机器上的实际目录。

## 2. 定制目标

原代理只在进程路径包含 `NRC-Win64-Shipping` 时加载 `rendertest.dll`。这会让代理绑定到某一个游戏名，PRAGMATA 的进程名不匹配时只会转发 DXGI，无法启用捕获。

取消固定名称限制后，代理又必须避免加载到 RenderDoc 自身的 UI 和命令行回放进程，否则工具进程可能预加载错误的 D3D12 运行时、再次安装图形 Hook，或形成自注入。

当前策略是：

- 默认：所有普通宿主进程启用完整代理注入。
- 例外：精确识别 RenderDoc 工具进程，只保留系统 DXGI 转发。
- 配置：让 PRAGMATA 使用 DirectX 12，同时减少并行命令构建、多队列和独立渲染工作线程带来的捕获复杂度。

## 3. 代码修改

### 3.1 `version_proxy/process_filter.h`

新增 `ShouldEnableProxyInjectionForProcessPath()`，负责进程过滤。

处理规则：

1. 接收完整进程路径。
2. 同时识别 `\\` 和 `/` 分隔符，只取最后的可执行文件名。
3. 使用 `_wcsicmp` 做忽略大小写的完整文件名比较。
4. 命中工具排除名单时返回 `false`，其他情况返回 `true`。

当前排除名单：

| 进程名 | 用途 | 代理行为 |
|---|---|---|
| `qrendertest.exe` | 当前定制版图形界面 | 执行公共代理初始化，不进行捕获注入 |
| `rendertestcmd.exe` | 当前定制版命令行工具 | 执行公共代理初始化，不进行捕获注入 |
| `qrenderdoc.exe` | 上游名称兼容 | 执行公共代理初始化，不进行捕获注入 |
| `renderdoccmd.exe` | 上游名称兼容 | 执行公共代理初始化，不进行捕获注入 |

这里使用精确文件名比较。例如 `my-qrendertest.exe` 不会被误判为工具进程。

### 3.2 `version_proxy/version_proxy.cpp`

删除旧的 `IsGameProcess()` 和 `NRC-Win64-Shipping` 字符串限制，在 `DllMain(DLL_PROCESS_ATTACH)` 开始时读取当前进程路径并调用新的过滤函数。

初始化流程现在分为公共路径和条件路径：

~~~text
所有进程
  ├─ 加载 System32\dxgi.dll
  └─ 缓存真实 DXGI 导出函数
       │
       ├─ 普通宿主进程
       │    ├─ 预加载 System32\d3d12.dll
       │    ├─ 执行代理文件重命名与 PEB 模块名伪装
       │    └─ 加载 rendertest.dll，安装捕获 Hook
       │
       └─ RenderDoc 工具进程
            └─ 跳过 D3D12 预加载、代理伪装和 rendertest.dll
~~~

公共路径始终先加载并缓存真实 DXGI 导出。工具进程仍执行公共代理初始化并转发系统 DXGI，但跳过所有捕获相关初始化。工具进程不预加载系统 D3D12，目的是让回放程序自行选择捕获文件需要的 D3D12Core 版本。

### 3.3 `version_proxy/version_proxy.vcxproj`

该项目原本已负责 `dxgi.dll` 输出名、模块定义文件以及 Release x64 等构建规则。本次提交只新增 `process_filter.h` 的 `ClInclude` 项目条目，保证解决方案视图、项目打包和后续维护都能看到该文件。

### 3.4 未包含的内容

- 不包含 `version_proxy/tests` 或其他测试源码。
- 不提交 `x64/Release` 下的 DLL、EXE、PDB、日志或部署副本。
- 构建产物由源码在目标机器上重新生成。

## 4. PRAGMATA `config.ini` 基线

文件位置：

~~~text
<GAME_ROOT>\config.ini
~~~

当前验证可用的 `[Render]` 段：

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

### 4.1 逐项说明

| 配置项 | 当前值 | 截帧侧含义与选择理由 |
|---|---|---|
| `AllowMeshShader` | `Disable` | 不允许 Mesh Shader 路径，减少特殊图形管线进入捕获的可能性；代价是仅依赖 Mesh Shader 的内容可能无法按原路径绘制。 |
| `Capability` | `DirectX12` | 明确使用 DirectX 12，与当前 RenderDoc 构建和 PRAGMATA 捕获路径一致。 |
| `ForceMeshShader` | `Disable` | 不强制 Mesh Shader；与 `AllowMeshShader=Disable` 保持一致，避免互相矛盾。 |
| `ParallelBuildCommandList` | `Disable` | 关闭并行构建命令列表，降低捕获时命令记录与提交顺序的复杂度。 |
| `ParallelBuildProcessorCount` | `0` | 与并行构建关闭配套，不指定固定处理器数量；`0` 的最终解释由游戏实现决定，在当前组合中不单独调整。 |
| `RenderWorkerThreadPriorityAboveNormal` | `Disable` | 关闭渲染工作线程的高优先级调度，减少捕获期间与 RenderDoc 工作线程争抢 CPU 调度资源的可能性。 |
| `TightFitShaderCache` | `Disable` | 不启用更激进的紧凑 Shader Cache 策略，优先保持捕获路径稳定。 |
| `UseComputeQueuePairing` | `Disable` | 关闭计算队列配对，减少跨队列同步路径。 |
| `UsingIndepentRenderWorker` | `Disable` | 关闭独立渲染工作线程，减少额外线程与命令提交路径。键名中的 `Indepent` 是游戏现有拼写，不要擅自改成 `Independent`。 |

这些说明以配置键名、捕获现象和当前实测基线为依据。游戏更新后若改变了配置语义，应以新版本实际行为为准。

### 4.2 为什么优先关闭并行和多队列特性

RenderDoc 需要记录图形 API 调用、资源状态、命令列表和队列同步。游戏侧同时启用并行命令构建、独立渲染工作线程或计算队列配对时，捕获中需要还原的时序和同步关系更多。

当前基线的取舍是：

- 优先保证“可以捕获、可以打开、绘制内容尽量完整”。
- 接受捕获期间 CPU 并行度或游戏性能可能降低。
- 不把每一个缺失绘制都归因于单个配置项；遇到问题时一次只改变一个设置并重新捕获。

## 5. 配置修改、备份与恢复

修改前先退出 PRAGMATA，再把配置备份到游戏目录之外的时间戳目录：

~~~powershell
$gameRoot = '<GAME_ROOT>'
$backupBase = '<BACKUP_BASE>'
$configPath = Join-Path $gameRoot 'config.ini'
$backupStamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$backupRoot = Join-Path $backupBase "config-$backupStamp"
if (-not (Test-Path -LiteralPath $backupBase -PathType Container)) {
  New-Item -ItemType Directory -Path $backupBase -ErrorAction Stop | Out-Null
}
New-Item -ItemType Directory -Path $backupRoot -ErrorAction Stop | Out-Null
Copy-Item -LiteralPath $configPath -Destination (Join-Path $backupRoot 'config.ini') -ErrorAction Stop
"配置备份目录：$backupRoot"
~~~

然后只替换 `[Render]` 段，不要删除或重排其他配置段。修改完成后确认：

- `[Render]` 只出现一次。
- 9 个键全部存在，拼写与大小写保持原样。
- `Capability=DirectX12`。
- `ParallelBuildCommandList=Disable` 且 `ParallelBuildProcessorCount=0`。

需要恢复时，先退出游戏，将下面的备份目录替换为上一步输出的实际目录，再执行：

~~~powershell
$gameRoot = '<GAME_ROOT>'
$backupRoot = '<CONFIG_BACKUP_ROOT>'
Copy-Item -LiteralPath (Join-Path $backupRoot 'config.ini') `
  -Destination (Join-Path $gameRoot 'config.ini') -Force -ErrorAction Stop
~~~

截帧前建议使用游戏内帧率上限或显卡驱动设置，将游戏限制在稳定的 30 FPS。较低且稳定的帧率通常能减少捕获期间的命令量、资源变化和瞬时调度压力。该设置是稳定性建议，不是代理工作的硬性条件；修改帧率限制后，应重启游戏再生成新捕获。

## 6. 已知边界

- 进程过滤采用“默认启用、工具排除”策略。若以后新增 RenderDoc 宿主 EXE，必须把其精确文件名加入 `renderDocTools`。
- 精确排除依赖 `GetModuleFileNameW` 成功读取当前进程路径。读取失败、路径为空或文件名无法识别时，当前策略会默认启用完整代理。
- 如果其他非游戏程序从部署目录加载该 `dxgi.dll`，它也会进入完整代理路径；因此只把代理部署到 PRAGMATA 实际可执行文件所在目录。
- 工具排除依赖可执行文件名。工具被再次重命名时需要同步更新名单。
- `config.ini` 是游戏侧设置，不是 RenderDoc 通用设置；它不应复制到其他游戏后直接视为最佳配置。
- 游戏、驱动、D3D12 Agility SDK 或 RenderDoc 升级后，应重新验证启动、捕获、打开捕获和绘制完整性。

## 7. 后续升级维护清单

升级 RenderDoc 或合并上游代码后，至少检查：

1. `version_proxy.cpp` 是否仍先缓存真实 DXGI 导出，再加载 `rendertest.dll`。
2. 工具进程是否仍跳过 D3D12 预加载、代理伪装和 `rendertest.dll`。
3. 四个工具进程名是否仍与实际构建产物一致。
4. `process_filter.h` 是否仍在 `version_proxy.vcxproj` 中。
5. Release x64 全量重编译是否成功生成 `dxgi.dll`、`rendertest.dll` 和 `qrendertest.exe`。
6. PRAGMATA 的 `[Render]` 段是否仍与本文件基线一致。
7. 新捕获是否可以由同一次构建生成的 `qrendertest.exe` 打开，并检查绘制是否完整。
