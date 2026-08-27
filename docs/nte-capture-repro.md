# Neverness To Everness（异环）截帧复现说明

本分支记录异环的专用适配、部署脚本和验收流程。目标是：

- 让 app-local `dxgi.dll` 代理稳定挂载到游戏目录；
- 让 RenderDoc 核心 `rendertest.dll` 在异环启动链路里稳定进入目标进程；
- 让人类和 AI 都能按同一套步骤完成编译、部署、启动、截帧与回滚。

## 1. 这次实际改了什么

### 1.1 `version_proxy/version_proxy.cpp`

这是异环的 app-local `dxgi.dll` 代理主体。当前实现保留了以下行为：

- 先加载 System32 里的真实 `dxgi.dll` 和 `d3d12.dll`；
- 把代理文件从 `dxgi.dll` 重命名成 `dxgi.dll.tmp`；
- 把 PEB 里的模块名伪装成 `mfplat.dll`；
- 记录 `proxy.log`；
- **不再用进程名过滤 `rendertest.dll`**，而是始终加载同目录的 `rendertest.dll`。

这一步的意义是：

- 之前的 `IsGameProcess()` 只认老进程名，会把异环主进程误判成“非游戏进程”；
- 现在去掉过滤后，只要 `dxgi.dll` 真的被游戏目录加载，RenderDoc 核心就会跟着进来。

### 1.2 `renderdoc/os/win32/sys_win32_hooks.cpp`

这里补了 `ShellExecuteExA/W` hook。异环启动器实际拉起 `HTGame.exe` 的路径是 `ShellExecuteExW`，不是只靠 `CreateProcess`。

这次修改做了三件事：

- hook `shell32.dll!ShellExecuteExA/W`；
- 从 `ShellExecuteEx` 返回的 `hProcess` 取 PID；
- 调用 `Process::InjectIntoProcess(...)`，成功后再 `RenderDoc::Inst().AddChildProcess(...)`。

同时保留了对以下进程的跳过：

- `rendertestcmd.exe`
- `qrendertest.exe`
- `renderdoccmd.exe`
- `qrenderdoc.exe`
- `NTEBrowser.exe`
- `NTEWebBooster.exe`
- `CrashClientReporter.exe`

这能避免把启动器自己的辅助进程和报告进程误当成目标。

### 1.3 `renderdoc/os/win32/win32_hook.cpp`

这里把模块扫描时的“引用计数句柄”改成优先用 `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)` 固定当前枚举到的那一份映射。

原因是异环启动器/辅助进程会快速加载、卸载一些 delay-load 模块，按路径重新 `LoadLibraryW` 可能会拿到另一份映射，导致 hook 或断言不稳定。

## 2. 这套适配的边界

- 这是 **app-local** 方案，只作用于游戏目录；
- 不修改 `System32`；
- 不依赖全局 Vulkan 环境变量常驻；
- 只要启动链路和目标进程名/路径匹配，`rendertest.dll` 就会被带进去；
- 如果 `proxy.log` 里还出现 `Not game process`，说明你拿到的不是当前这版代理。

## 3. 编译产物在哪里

默认输出目录：

```text
<REPO_ROOT>\x64\Release
```

关键产物：

- `dxgi.dll`
- `rendertest.dll`
- `qrendertest.exe`
- `rendertestcmd.exe`

如果你在别的机器或别的工作区构建，把 `-BuildDir` 指到对应目录即可。

## 4. 推荐部署方式

脚本：

```text
tools\Deploy-NTE-Capture.ps1
```

它会做这些事：

1. 检查构建目录里是否存在 `dxgi.dll` 和 `rendertest.dll`；
2. 检查异环相关进程是否仍在运行；
3. 备份游戏目录里已有的：
   - `dxgi.dll`
   - `dxgi.dll.tmp`
   - `rendertest.dll`
   - `proxy.log`
4. 把新的 `dxgi.dll` 和 `rendertest.dll` 复制到游戏目录；
5. 生成 qrenderdoc 自动启动的 `.cap` 和 `.py`；
6. 需要的话直接拉起 RenderDoc UI。

### 一条命令部署并启动

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Deploy-NTE-Capture.ps1 -RunAsAdmin
```

如果你的构建目录不在默认位置，显式传入：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Deploy-NTE-Capture.ps1 `
  -BuildDir "<REPO_ROOT>\x64\Release" `
  -GameDir "E:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64" `
  -LauncherExe "E:\Neverness To Everness\NTELauncher\NTEGame.exe" `
  -RunAsAdmin
```

### 只部署，不启动 UI

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Deploy-NTE-Capture.ps1 -NoLaunchUI
```

## 5. 人类版 quickstart

1. 编译 `Release|x64`。
2. 跑部署脚本。
3. 等 RenderDoc UI 打开后，确认 profile 已自动加载。
4. 让脚本拉起异环启动器。
5. 进入游戏后按 **F12** 截帧。`F11` 只是 focus key，不是 capture key。
6. 如果键盘把 F12 解释成音量键，先让游戏窗口获得焦点，再用 RenderDoc UI 的 capture 按钮做一次验证。

## 6. AI 版 quickstart / 验收清单

AI 或自动化脚本在判断“是否成功挂载”时，优先看这几个证据：

- 游戏目录里是否出现 `dxgi.dll`，以及第一次加载后是否自改名为 `dxgi.dll.tmp`；
- `proxy.log` 是否包含：
  - `LoadLibrary rendertest.dll: OK`
  - **不再**包含 `Not game process, skipping rendertest.dll`；
- RenderDoc UI 是否能看到目标进程；
- 是否能看到对应 API 的 overlay / capture 状态；
- 是否实际生成 `.rdc` 文件。

如果上述任一项不成立，不要直接说“挂上了”，而要先区分：

- 代理没加载；
- 代理加载了但没进目标进程；
- 目标进程进来了但被错误过滤；
- 进程进来了但没进入正确 API / 正确实例。

## 7. 回滚

如果这次部署后要恢复，直接把备份目录里的文件移回去即可。

脚本已经把旧文件放在类似下面的目录里：

```text
E:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64\renderdoc-nte-backup-YYYYMMDD-HHMMSS
```

优先恢复：

- `dxgi.dll`
- `rendertest.dll`
- `proxy.log`（只用于排查，恢复时可选）

## 8. 备注

- 这份说明只记录异环专用适配，不展开其它游戏的通用方案；
- 以后如果又要收紧或放宽目标识别，优先看 `version_proxy/version_proxy.cpp` 和 `sys_win32_hooks.cpp`；
- 如果要定位“为什么这次没挂上”，先读 `proxy.log`，再看当前游戏目录里到底是 `dxgi.dll` 还是 `dxgi.dll.tmp`。
