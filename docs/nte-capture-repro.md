# 异环 / Neverness To Everness 截帧复现分支

本分支只记录 `E:\Neverness To Everness` 的专门适配。

## 代码定制
- `renderdoc/os/win32/sys_win32_hooks.cpp`：增加 `ShellExecuteExA/W` hook。NTE launcher 启动 `HTGame.exe` 的实证路径是 `ShellExecuteExW`，需要把返回的 `hProcess` 接入 `Process::InjectIntoProcess + AddChildProcess`。
- `renderdoc/os/win32/sys_win32_hooks.cpp`：过滤 `NTEBrowser.exe`、`NTEWebBooster.exe`、`CrashClientReporter.exe`，避免 CEF/报告进程占用 target-control 连接。
- `renderdoc/os/win32/win32_hook.cpp`：模块扫描时用 `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)` 固定当前枚举到的模块，避免快速 unload/reload 时按路径重新加载到另一份映射。

## 已知成功/限制
历史捕获证据存在 `NTEGame_2026.06.16_12.56.42_frame461.rdc`，日志含 `Captured GL frame` 与 `Got a new capture: 0 (frame 461)`。同轮验证过 `ShellExecuteExW` 可将 `HTGame.exe` 注入并握手。若验收严格限定 HTGame 主 3D 渲染帧，需要现场再次实测 overlay/API 和 `.rdc`。

## 使用
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "tools\Deploy-NTE-Capture.ps1" -BuildDir "D:\ZmdRenderdoc\renderdoc\x64\Development" -LauncherExe "E:\Neverness To Everness\NTELauncher\NTEGame.exe" -RunAsAdmin
```
