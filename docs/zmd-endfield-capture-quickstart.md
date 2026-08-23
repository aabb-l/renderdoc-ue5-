# ZMD / Arknights Endfield 截帧快速使用说明

这份说明面向已经切到 `arknights-endfield` 分支的使用者；完整设计与代码适配记录见 `docs/zmd-endfield-capture-repro.md`。

下文占位符含义：

```text
<repo>                当前仓库根目录
<ENDFIELD_GAME_DIR>   本机 Arknights Endfield 游戏目录
```

不要直接把示例中的占位符原样执行；先替换成自己的路径。

## 1. 获取分支

新 clone：

```powershell
git clone --branch arknights-endfield --single-branch https://github.com/aabb-l/renderdoc-ue5-.git "<repo>"
cd "<repo>"
```

已有 clone：

```powershell
cd "<repo>"
git fetch origin arknights-endfield
git switch arknights-endfield
git pull --ff-only
```

## 2. 编译 Release x64

初始化必要子模块：

```powershell
git submodule update --init --recursive renderdoc/3rdparty/minhook
```

用 Visual Studio / MSBuild 构建 Release x64 的 `renderdoc` 和 `version_proxy`。命令行示例：

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"

& $msbuild ".\renderdoc\3rdparty\breakpad\client\windows\common.vcxproj" /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
& $msbuild ".\renderdoc\3rdparty\breakpad\client\windows\crash_generation\crash_generation_client.vcxproj" /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
& $msbuild ".\renderdoc\3rdparty\breakpad\client\windows\handler\exception_handler.vcxproj" /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"

& $msbuild ".\renderdoc\renderdoc.vcxproj" /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
& $msbuild ".\version_proxy\version_proxy.vcxproj" /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
```

预期产物：

```text
<repo>\x64\Release\dxgi.dll
<repo>\x64\Release\rendertest.dll
<repo>\x64\Release\rendertest.json
```

## 3. 部署

先关闭 Endfield 和 Hypergryph Launcher 相关进程，然后执行：

```powershell
cd "<repo>"

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File ".\tools\Deploy-Zmd-Endfield.ps1" `
  -Mode Deploy `
  -GameDir "<ENDFIELD_GAME_DIR>" `
  -BuildDir ".\x64\Release"
```

部署脚本会：

- 检查 `dxgi.dll`、`rendertest.dll`、`rendertest.json`；
- 备份目标目录中已有的相关文件；
- 复制新文件到 `<ENDFIELD_GAME_DIR>`；
- 注册 64 位 HKLM Vulkan implicit layer；
- 删除 HKCU 中同一路径的重复 Vulkan layer 项；
- 生成 `<ENDFIELD_GAME_DIR>\Launch-Endfield-RenderTest.ps1`；
- 生成 `<ENDFIELD_GAME_DIR>\Launch-Endfield-RenderTest.cmd`；
- 输出 `StatePath`，用于回滚。

修改 HKLM 时会触发 UAC，确认即可。

## 4. 启动游戏

部署后不要直接启动普通启动器，使用部署生成的脚本：

```powershell
& "<ENDFIELD_GAME_DIR>\Launch-Endfield-RenderTest.cmd"
```

这个脚本会：

1. 检查是否已有 Hypergryph Launcher / Endfield 进程；
2. 如果只存在 `dxgi.dll.tmp`，恢复为 `dxgi.dll`；
3. 设置当前进程树的 `ENABLE_VULKAN_RENDERTEST_CAPTURE=1`；
4. 启动 Hypergryph Launcher。

## 5. 验证部署状态

部署后、启动游戏前可以检查文件和 Vulkan 注册：

```powershell
Get-ChildItem "<ENDFIELD_GAME_DIR>" -Filter "dxgi.dll*"
Get-ChildItem "<ENDFIELD_GAME_DIR>" -Filter "rendertest.*"

Get-ItemProperty "HKLM:\SOFTWARE\Khronos\Vulkan\ImplicitLayers" |
  Select-Object -Property *rendertest*

[pscustomobject]@{
  User    = [Environment]::GetEnvironmentVariable('ENABLE_VULKAN_RENDERTEST_CAPTURE','User')
  Machine = [Environment]::GetEnvironmentVariable('ENABLE_VULKAN_RENDERTEST_CAPTURE','Machine')
  Process = $env:ENABLE_VULKAN_RENDERTEST_CAPTURE
}
```

预期：

- `<ENDFIELD_GAME_DIR>\dxgi.dll` 存在；
- `<ENDFIELD_GAME_DIR>\rendertest.dll` 存在；
- `<ENDFIELD_GAME_DIR>\rendertest.json` 存在；
- HKLM 中存在 `<ENDFIELD_GAME_DIR>\rendertest.json = 0`；
- 用户级、机器级、当前 shell 的 `ENABLE_VULKAN_RENDERTEST_CAPTURE` 都为空。

游戏启动后可以检查：

```powershell
Get-CimInstance Win32_Process |
  Where-Object { $_.Name -like 'Endfield*' } |
  Select-Object ProcessId, Name, CommandLine

Get-NetTCPConnection |
  Where-Object { $_.LocalPort -ge 38920 -and $_.LocalPort -le 39000 }

Get-Content "<ENDFIELD_GAME_DIR>\proxy.log" -Tail 80
```

预期现象：

```text
左上角 overlay 显示 Capturing Vulkan
RenderDoc / qrenderdoc 可以看到目标
target-control 端口处于 Listen
proxy.log 中有 LoadLibrary rendertest.dll: OK
```

## 6. 截帧快捷键

默认提示里会显示类似：

```text
F11 to cycle. F12 to window captures saved.
```

注意：

- `F11` 是切换/聚焦捕获窗口；
- `F12` 通常是直接 capture；
- 如果物理 `F12` 变成音量键，使用 `Fn+F12`、切换 Fn Lock，或改用 `PrintScreen` / qrenderdoc 按钮捕获。

## 7. 重新部署

重新编译后需要重新部署：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File ".\tools\Deploy-Zmd-Endfield.ps1" `
  -Mode Deploy `
  -GameDir "<ENDFIELD_GAME_DIR>" `
  -BuildDir ".\x64\Release"
```

启动脚本只负责启动前恢复 `dxgi.dll.tmp -> dxgi.dll` 和设置进程级 Vulkan 环境变量；它不会自动同步新的 build 产物。

## 8. 回滚

部署输出会给出 `StatePath`，形如：

```text
<ENDFIELD_GAME_DIR>\codex-dual-api-backup-YYYYMMDD-HHMMSS\deployment-state.json
```

回滚命令：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File ".\tools\Deploy-Zmd-Endfield.ps1" `
  -Mode Rollback `
  -StatePath "<ENDFIELD_GAME_DIR>\codex-dual-api-backup-YYYYMMDD-HHMMSS\deployment-state.json"
```

回滚会恢复文件和 Vulkan layer 注册状态。

## 9. 常见问题

### 启动一次后 `dxgi.dll` 变成 `dxgi.dll.tmp`

这是该分支 proxy 的预期行为。下一次必须用：

```powershell
& "<ENDFIELD_GAME_DIR>\Launch-Endfield-RenderTest.cmd"
```

启动脚本会在启动前恢复 `dxgi.dll.tmp -> dxgi.dll`。

### 只有 Vulkan 注册但没有捕获

确认是通过 `Launch-Endfield-RenderTest.cmd` 启动。HKLM 只负责让 Vulkan loader 发现 layer，真正启用依赖启动脚本设置的进程级：

```text
ENABLE_VULKAN_RENDERTEST_CAPTURE=1
```

不要手动设置全局用户级或机器级启用变量。

### 部署脚本找不到 `dumpbin.exe`

`arknights-endfield` 分支已修复 x64 `dumpbin.exe` fallback 路径匹配。若仍失败，优先在 Developer PowerShell 中运行部署脚本，或确认已安装 VS C++ 工具链。
