# ZMD / Arknights Endfield RenderDoc 截取专门适配说明

本文只记录为了让 ZMD/RenderDoc 在 `D:\Hypergryph Launcher\games\Arknights Endfield` 上稳定部署、挂载 Vulkan/DXGI 捕获并完成回滚所做的**专门适配**。

本文不记录 RenderDoc 上游已有能力、仓库原有魔改能力，也不展开本次适配开始前已经存在的内容。

## 0. 文档范围与修改权威

本文档的权威来源是本分支中实际改动的代码和脚本；文档只解释这些代码修改痕迹。若文字说明与代码实际行为冲突，以代码为准。

本次 Endfield/ZMD 专门适配涉及的源码和脚本如下：

```text
version_proxy/version_proxy.cpp
renderdoc/driver/shaders/spirv/renderdoc_spirv.vcxproj
renderdoc/os/os_specific.h
renderdoc/os/posix/posix_process.cpp
renderdoc/os/win32/sys_win32_hooks.cpp
renderdoc/os/win32/win32_process.cpp
renderdoc/renderdoc.vcxproj
renderdoc/replay/entry_points.cpp
renderdoccmd/renderdoccmd.cpp
tools/Deploy-Zmd-Endfield.ps1
tools/Launch-Endfield-RenderTest.ps1
```

逐文件对应关系：

| 文件 | 为 Endfield/ZMD 做的专门适配 |
|---|---|
| `version_proxy/version_proxy.cpp` | 恢复 DXGI proxy 链路；运行时将本地 `dxgi.dll` 改名为 `dxgi.dll.tmp`；PEB 模块名伪装为 `winmm.dll`；加载 `rendertest.dll`；设置 RenderDoc capture options；写 `proxy.log`。 |
| `renderdoc/driver/shaders/spirv/renderdoc_spirv.vcxproj` | 增加 `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`，处理 MSVC 14.44 构建产物遇到游戏目录旧 `MSVCP140.dll` 14.29 时的 glslang mutex 初始化兼容问题。 |
| `renderdoc/os/os_specific.h` | 给 `Process::InjectIntoProcess()` 增加 `processIsSuspended` 参数。 |
| `renderdoc/os/posix/posix_process.cpp` | 同步 `InjectIntoProcess()` 函数签名，保持声明/定义一致。 |
| `renderdoc/os/win32/sys_win32_hooks.cpp` | `CreateProcess` hook 对强制 suspended 创建的子进程调用注入时传入 `processIsSuspended=true`。 |
| `renderdoc/os/win32/win32_process.cpp` | Windows 注入实现识别 suspended 目标；alternate-bitness 命令透传 `--process-suspended`；避免错误线程劫持、过早释放远程内存、重复 resume。 |
| `renderdoc/renderdoc.vcxproj` | 按平台拆分 MinHook HDE 源文件：Win32 编译 `hde32.c`，非 Win32 编译 `hde64.c`，保证 Win32/x64 构建都能配合注入链路。 |
| `renderdoc/replay/entry_points.cpp` | 增加内部注入入口 `RENDERDOC_InternalInjectIntoProcess()` 传递 suspended 状态，同时保留公开 `RENDERDOC_InjectIntoProcess()` ABI。 |
| `renderdoccmd/renderdoccmd.cpp` | `capaltbit` 增加 `--process-suspended` 参数并调用内部注入入口。 |
| `tools/Deploy-Zmd-Endfield.ps1` | 唯一源码级部署/回滚入口：复制产物、备份、HKLM Vulkan Layer 注册、清理 HKCU 重复项、生成游戏目录启动脚本、回滚。 |
| `tools/Launch-Endfield-RenderTest.ps1` | 部署到游戏目录的启动模板：恢复 `dxgi.dll.tmp -> dxgi.dll`、设置进程级 Vulkan 启用变量、启动 Hypergryph Launcher。 |

## 1. 针对该游戏做了哪些代码修改

### 1.1 DXGI proxy 恢复与固定

文件：`version_proxy/version_proxy.cpp`

游戏目录部署的 `dxgi.dll` 是我们的 proxy。游戏加载 `dxgi.dll` 时会先命中该 proxy，proxy 再转发到 System32 原版 DXGI。我们恢复并固定了以下行为：

1. 从 System32 加载真实 `dxgi.dll`；
2. 从 System32 预加载真实 `d3d12.dll`；
3. 缓存真实 DXGI 导出，例如：
   - `CreateDXGIFactory`
   - `CreateDXGIFactory1`
   - `CreateDXGIFactory2`
4. 将本地 proxy 文件从 `dxgi.dll` 重命名为 `dxgi.dll.tmp`；
5. 在 PEB loader list 中把自身模块名伪装成 `winmm.dll`；
6. 从同目录加载 `rendertest.dll`；
7. 通过 `RENDERDOC_GetAPI` 设置 RenderDoc capture options：
   - `eRENDERDOC_Option_AllowUnsupportedVendorExtensions = 0x10DE`
   - `eRENDERDOC_Option_HookIntoChildren = 1`
8. 写出 `proxy.log` 作为运行证据。

预期日志类似：

```text
[dxgi_proxy] Rename dxgi.dll -> dxgi.dll.tmp: OK
[dxgi_proxy] PEB masquerade: winmm.dll
[dxgi_proxy] LoadLibrary rendertest.dll: OK
[dxgi_proxy] Capture options: NVIDIA=OK, HookIntoChildren=1
```

说明：`dxgi.dll -> dxgi.dll.tmp` 是运行后的预期状态。下次启动前必须由专用启动脚本恢复，否则游戏目录里会缺少 `dxgi.dll`。

### 1.2 子进程捕获默认开启

文件：`version_proxy/version_proxy.cpp`

proxy 加载 `rendertest.dll` 后默认设置：

```text
eRENDERDOC_Option_HookIntoChildren = 1
```

目的：Endfield/启动器链路中可能存在多个子进程或渲染实例，不能只捕获最外层 Launcher 或最早加载 DXGI 的进程。

### 1.3 suspended 子进程注入修复

文件：

```text
renderdoc/os/os_specific.h
renderdoc/os/win32/sys_win32_hooks.cpp
renderdoc/os/win32/win32_process.cpp
renderdoc/os/posix/posix_process.cpp
renderdoc/replay/entry_points.cpp
renderdoccmd/renderdoccmd.cpp
```

修改点：

1. `InjectIntoProcess()` 增加 `processIsSuspended` 参数；
2. `CreateProcess` hook 强制子进程以 suspended 状态创建时，把该状态继续传给注入逻辑；
3. `LaunchAndInjectIntoProcess()` 对自己创建的 suspended 主线程也传递该状态；
4. alternate-bitness 注入命令增加：

```text
--process-suspended=%u
```

5. `renderdoccmd capaltbit` 接收并继续转发该状态；
6. Windows 注入逻辑在已知目标 suspended 时使用更稳妥的 remote-thread 路径，避免把目标主线程状态提前破坏。

目的：解决子进程注入时只捕获到少量数据、错误实例，或因 suspended 状态处理不当导致子进程启动/渲染异常的问题。

### 1.4 MSVCP140.dll / glslang 初始化兼容修复

文件：

```text
renderdoc/driver/shaders/spirv/renderdoc_spirv.vcxproj
```

Endfield 会优先加载游戏目录内较旧的 `MSVCP140.dll` 14.29，而我们的 ZMD/RenderDoc 构建环境使用 MSVC 14.44。此前 Vulkan 路径在 glslang 初始化时出现过 ABI 不兼容崩溃，典型栈为：

```text
MSVCP140.dll+0x13020
std::mutex::lock
glslang::InitializeProcess
rdcspv::Init
EGLHook::EGLHook
```

最小修复是在 SPIR-V/glslang 相关工程的 Release 预处理定义中加入：

```text
_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
```

这样避免 MSVC 14.44 STL 使用和旧版 `MSVCP140.dll` 不兼容的 constexpr mutex 初始化路径，不需要向游戏目录复制或替换 MSVC runtime DLL。

### 1.5 MinHook HDE 平台构建修正

文件：

```text
renderdoc/renderdoc.vcxproj
```

代码 diff 将 MinHook 的 HDE 源文件按平台拆开：

```text
Win32      -> hde32.c
非 Win32   -> hde64.c
```

该修改是构建层修复，确保 32 位配置不会错误编译 `hde64.c`，配合 alternate-bitness 注入路径保持 Win32/x64 构建都可用。

### 1.6 Vulkan Layer 启用方式调整

源码侧配合脚本使用：

- HKLM 只负责让 Vulkan loader 能发现 `rendertest.json`；
- 不设置全局 `ENABLE_VULKAN_RENDERTEST_CAPTURE=1`；
- 专用启动脚本只在启动 Hypergryph Launcher 的当前进程树中设置该变量。

这样可以让 Endfield 子进程继承 Vulkan layer 启用变量，同时避免影响其他 Vulkan 程序。

## 2. 分支包含哪些交付文件

源码修改：

```text
version_proxy/version_proxy.cpp
renderdoc/driver/shaders/spirv/renderdoc_spirv.vcxproj
renderdoc/os/os_specific.h
renderdoc/os/posix/posix_process.cpp
renderdoc/os/win32/sys_win32_hooks.cpp
renderdoc/os/win32/win32_process.cpp
renderdoc/renderdoc.vcxproj
renderdoc/replay/entry_points.cpp
renderdoccmd/renderdoccmd.cpp
```

部署脚本源码只保留 2 个：

```text
tools/Deploy-Zmd-Endfield.ps1            # 唯一部署/回滚入口，包含 HKLM 注册、备份、复制、回滚逻辑
tools/Launch-Endfield-RenderTest.ps1     # 部署到游戏目录后的启动模板
```

说明：`Launch-Endfield-RenderTest.cmd` 会由部署脚本在游戏目录中生成，属于部署产物，不上传到源码分支。

说明文档：

```text
docs/zmd-endfield-capture-repro.md
```

## 3. 编译要求

使用现有 Release x64 编译流程，最终 build 输出目录需要有：

```text
dxgi.dll
rendertest.dll
rendertest.json
```

常用输出目录：

```text
<repo>\x64\Release
```

如果使用现有 ZMD 输出，也可以用：

```text
D:\ZmdRenderdoc\1\renderdoc\x64\Release
```

部署时通过 `-BuildDir` 指定即可。

## 4. 最终部署流程

### 4.1 关闭目标程序

部署前关闭：

```text
Endfield.exe
Launcher.exe
Hypergryph Launcher 相关进程
```

部署脚本会检查 Endfield 是否还在运行；运行中会拒绝部署。

### 4.2 推荐方式：唯一部署入口

在仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Deploy-Zmd-Endfield.ps1
```

默认路径：

```text
GameDir : D:\Hypergryph Launcher\games\Arknights Endfield
BuildDir: <repo>\x64\Release
```

如果机器上存在：

```text
D:\ZmdRenderdoc\1\renderdoc\x64\Release
```

且 `<repo>\x64\Release` 不完整，部署脚本会优先使用可用的完整 build 输出。

### 4.3 指定 build 输出部署

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\Deploy-Zmd-Endfield.ps1 `
  -Mode Deploy `
  -GameDir 'D:\Hypergryph Launcher\games\Arknights Endfield' `
  -BuildDir 'D:\ZmdRenderdoc\1\renderdoc\x64\Release'
```


### 4.4 部署脚本会做什么

部署脚本会：

1. 检查游戏目录；
2. 检查 build 输出中的 `dxgi.dll`、`rendertest.dll`、`rendertest.json`；
3. 检查 `rendertest.json` 中的 Vulkan layer：

```text
VK_LAYER_RENDERTEST_Capture
.\rendertest.dll
```

4. 备份已有托管文件到：

```text
D:\Hypergryph Launcher\games\Arknights Endfield\codex-dual-api-backup-YYYYMMDD-HHMMSS
```

5. 复制到游戏目录：

```text
dxgi.dll
rendertest.dll
rendertest.json
Launch-Endfield-RenderTest.ps1
Launch-Endfield-RenderTest.cmd
```

6. 将 Vulkan implicit layer manifest 注册到 64 位 HKLM：

```text
HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers
```

7. 删除同 manifest 的 HKCU 重复注册；
8. 确保没有用户级或机器级全局启用变量：

```text
ENABLE_VULKAN_RENDERTEST_CAPTURE=1
```

9. 输出部署状态、SHA-256 和 rollback 用的：

```text
deployment-state.json
```

修改 HKLM 时会触发 UAC，确认即可。

## 5. 启动与截取流程

部署完成后，不要直接启动 Launcher。使用游戏目录中的专用启动脚本：

```powershell
& 'D:\Hypergryph Launcher\games\Arknights Endfield\Launch-Endfield-RenderTest.cmd'
```

该启动脚本会：

1. 检查是否已有 Hypergryph Launcher/Endfield 相关进程；
2. 如果只存在 `dxgi.dll.tmp`，恢复为 `dxgi.dll`；
3. 如果 `dxgi.dll` 与 `dxgi.dll.tmp` 同时存在或同时缺失，直接失败；
4. 设置当前启动进程树的环境变量：

```powershell
$env:ENABLE_VULKAN_RENDERTEST_CAPTURE = '1'
```

5. 启动 Hypergryph Launcher。

进入游戏后，左上角应能看到 RenderDoc overlay，例如：

```text
Capturing Vulkan. Window 1 active. Frame: ...
```

可以通过 qrenderdoc 连接目标，也可以使用快捷键 capture。

快捷键注意：RenderDoc 默认 `F11` 是切换/聚焦捕获窗口，直接 capture 通常是 `F12` 或 `PrintScreen`。如果按物理 `F12` 变成系统音量键，请使用 `Fn+F12`、切换 Fn Lock，或改用 `PrintScreen`。

## 6. 更新 build 后重新部署

重新编译 `dxgi.dll` 或 `rendertest.dll` 后，需要重新执行部署脚本，把新产物复制到游戏目录。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\Deploy-Zmd-Endfield.ps1 `
  -Mode Deploy `
  -BuildDir '<新的 x64\Release 路径>'
```

启动脚本只负责启动前恢复 `dxgi.dll.tmp -> dxgi.dll` 和设置进程级 Vulkan 环境变量；它不会每次启动自动同步 build 产物。

## 7. 回滚流程

部署输出会显示 `StatePath`，例如：

```text
D:\Hypergryph Launcher\games\Arknights Endfield\codex-dual-api-backup-YYYYMMDD-HHMMSS\deployment-state.json
```

用人类友好入口回滚：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\Deploy-Zmd-Endfield.ps1 `
  -Mode Rollback `
  -StatePath 'D:\Hypergryph Launcher\games\Arknights Endfield\codex-dual-api-backup-YYYYMMDD-HHMMSS\deployment-state.json'
```

底层回滚会恢复：

1. 原游戏目录文件；
2. 64 位 HKLM Vulkan layer 注册状态；
3. HKCU Vulkan layer 注册状态；
4. 原环境变量状态；
5. 当前被替换文件会被保存到 displaced 目录。

## 8. 常见问题

### 启动一次后 `dxgi.dll` 消失

这是预期行为。proxy 加载后会把自己重命名为：

```text
dxgi.dll.tmp
```

下次必须使用：

```text
Launch-Endfield-RenderTest.cmd
```

它会自动恢复。

### Vulkan 注册存在但没有激活

这是预期设计。HKLM 只负责发现 layer，真正启用依赖启动脚本设置的进程级：

```text
ENABLE_VULKAN_RENDERTEST_CAPTURE=1
```

不要手动设置全局用户级/机器级变量，否则可能影响其他 Vulkan 程序。

### qrenderdoc 可以 capture，但 F12 不生效

如果左上角 overlay 已显示，说明 hook 和目标控制正常。F12 不生效通常是键盘处于多媒体键模式，物理 F12 实际发送的是音量键。使用 `Fn+F12`、`PrintScreen` 或切换 Fn Lock。

### 只捕获到 Launcher 或少量数据

确认：

1. 是否用 `Launch-Endfield-RenderTest.cmd` 启动；
2. `proxy.log` 是否出现 `HookIntoChildren=1`；
3. qrenderdoc 是否连接的是真正的 Endfield 渲染实例；
4. 目标进程是否继承了 `ENABLE_VULKAN_RENDERTEST_CAPTURE=1`。
