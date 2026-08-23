# The Last of Us Part I 截帧复现说明

本文档记录 `D:\Games\The Last of Us - Part I\tlou-i.exe` 的截帧复现方式。内容以本分支的代码修改痕迹为准，只记录为了该游戏可复现截帧而保留的差异、构建方法、部署方法和运行验证方法。

## 目标

- 目标程序：`D:\Games\The Last of Us - Part I\tlou-i.exe`
- 图形 API：D3D12
- 部署方式：在游戏目录放置 `dxgi.dll` proxy 与 `rendertest.dll`
- 期望结果：游戏启动后 proxy 加载 `rendertest.dll`，RenderDoc overlay / UI 可对 TLOU D3D12 帧进行 capture。

## 代码修改权威记录

### 1. 通用 `version_proxy` 目标进程识别

文件：`version_proxy/version_proxy.cpp`

`version_proxy` 的基线逻辑只会在 `IsGameProcess()` 返回 true 时加载同目录 `rendertest.dll`。因此所有使用该 proxy 方案的游戏，都必须把当前游戏的 exe 或路径特征加入 `IsGameProcess()`；否则会出现 proxy 已加载，但日志显示跳过核心库的情况：

```text
[dxgi_proxy] Not game process, skipping rendertest.dll
```

TLOU 分支加入的目标识别为：

```cpp
wcsstr(exeName, L"tlou-i.exe") != NULL ||
wcsstr(exeName, L"The Last of Us - Part I") != NULL
```

这不是 TLOU 独有机制，而是所有 `version_proxy` 游戏都需要按目标补齐的通用适配项。

### 2. TLOU / NVIDIA vendor extension 适配

文件：`version_proxy/version_proxy.cpp`

本分支在 `rendertest.dll` 成功加载后，调用 `RENDERDOC_GetAPI` 并设置：

```cpp
api->SetCaptureOptionU32(eRENDERDOC_Option_AllowUnsupportedVendorExtensions, 0x10DE);
```

含义：`0x10DE` 是 NVIDIA vendor id。这里传 vendor id，而不是传 boolean，用于启用 NVIDIA / NvAPI passthrough。

### 3. TLOU D3D12 Shader Model 适配

文件：`renderdoc/driver/d3d12/d3d12_device_wrap.cpp`

将 Shader Model 查询 clamp 从 6.7 放到 6.9：

```cpp
D3D_SHADER_MODEL_6_7 -> D3D_SHADER_MODEL_6_9
```

日志文本同步改为：

```text
Clamping shader model from 0x%x to 6.9
```

## 明确没有做的事情

本分支故意不包含以下内容：

- 不把 proxy 重写成 honest proxy。
- 不移除基线 proxy 的 `dxgi.dll -> dxgi.dll.tmp` 自改名行为。
- 不移除基线 proxy 的 PEB 伪装行为。
- 不移除基线 proxy 的 `proxy.log` 行为。
- 不增加构建后自动签名。
- 部署脚本不执行签名。

因此运行后游戏目录出现 `dxgi.dll.tmp` 和 `proxy.log` 是预期行为。

## 构建流程

以下命令在本分支根目录执行。

```powershell
cd "<repo>"
```

其中 `<repo>` 表示当前分支根目录，例如 `tlou-capture-repro` worktree。

### 1. 初始化必需子模块

```powershell
git submodule update --init --recursive renderdoc/3rdparty/minhook
```

如果缺少 `minhook`，RenderDoc 核心库会编译失败。

### 2. 准备 MSBuild 路径

本机验证使用：

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
```

如果 VS 安装路径不同，可以用 `vswhere.exe` 查找 MSBuild。

### 3. 构建 Breakpad 依赖库

`rendertest.dll` 链接依赖以下库，先构建它们：

```powershell
& $msbuild ".\renderdoc\3rdparty\breakpad\client\windows\common.vcxproj" /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
& $msbuild ".\renderdoc\3rdparty\breakpad\client\windows\crash_generation\crash_generation_client.vcxproj" /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
& $msbuild ".\renderdoc\3rdparty\breakpad\client\windows\handler\exception_handler.vcxproj" /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
```

如果不先构建这些库，可能会在链接 `rendertest.dll` 时看到类似错误：

```text
LINK : fatal error LNK1181: cannot open input file "...\breakpad_common.lib"
```

### 4. 构建 RenderDoc 核心库

```powershell
& $msbuild ".\renderdoc\renderdoc.vcxproj" /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
```

期望输出：

```text
x64\Release\rendertest.dll
```

### 5. 构建 DXGI proxy

```powershell
& $msbuild ".\version_proxy\version_proxy.vcxproj" /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
```

期望输出：

```text
x64\Release\dxgi.dll
```

该分支没有 PostBuild 签名步骤。构建完成后可以确认：

```powershell
Get-AuthenticodeSignature ".\x64\Release\dxgi.dll"
Get-AuthenticodeSignature ".\x64\Release\rendertest.dll"
```

预期签名状态：`NotSigned`。

## 部署流程

部署前先关闭 TLOU。脚本会拒绝在 `tlou-i.exe` 正在运行时替换文件。

```powershell
cd "<repo>"

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File ".\tools\Deploy-TLOU-Capture.ps1" `
  -GameDir "D:\Games\The Last of Us - Part I" `
  -BuildDir ".\x64\Release"
```

部署脚本会做这些事：

1. 检查 `GameDir`、`dxgi.dll`、`rendertest.dll` 是否存在。
2. 检查 `tlou-i.exe` 是否正在运行；如果正在运行则停止部署。
3. 创建备份目录：`codex-tlou-capture-backup-YYYYMMDD-HHMMSS`。
4. 备份游戏目录中已有的：
   - `dxgi.dll`
   - `dxgi.dll.tmp`
   - `rendertest.dll`
   - `proxy.log`
5. 复制当前构建产物到游戏目录：
   - `x64\Release\dxgi.dll` -> `GameDir\dxgi.dll`
   - `x64\Release\rendertest.dll` -> `GameDir\rendertest.dll`
6. 输出部署后文件的 SHA-256 和签名状态。

脚本不会签名文件。输出中的 `Signature = NotSigned` 对本分支是预期结果。

## 使用与验证流程

### 1. 启动游戏

可以从 Steam 或直接启动：

```powershell
Start-Process -FilePath "D:\Games\The Last of Us - Part I\tlou-i.exe" -WorkingDirectory "D:\Games\The Last of Us - Part I"
```

### 2. 验证 proxy 加载

启动后检查游戏目录：

```powershell
Get-ChildItem "D:\Games\The Last of Us - Part I" -Filter "dxgi.dll*"
Get-Content "D:\Games\The Last of Us - Part I\proxy.log" -Tail 80
```

预期现象：

```text
Rename dxgi.dll -> dxgi.dll.tmp: OK
PEB masquerade: renamed to 'mfplat.dll'
LoadLibrary rendertest.dll: OK
```

说明：因为保留了基线自改名逻辑，首次加载后 `dxgi.dll` 会变成 `dxgi.dll.tmp`。下一次测试前需要重新运行部署脚本，让脚本重新放置 `dxgi.dll`。

如果看到：

```text
Not game process, skipping rendertest.dll
```

说明目标识别没有命中，应该优先检查 `IsGameProcess()` 中的 exe/path 匹配。

### 3. Capture

游戏出现 RenderDoc overlay 后，可以用以下任一方式 capture：

- RenderDoc UI 中的 Capture 按钮。
- 游戏内 RenderDoc 快捷键。

捕获文件通常输出到：

```text
%LOCALAPPDATA%\Temp\RenderTest
```

检查最近 capture：

```powershell
Get-ChildItem (Join-Path $env:TEMP "RenderTest") -Filter "*.rdc" |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 10 Name,Length,LastWriteTime
```

## 已知成功证据

历史成功捕获文件：

```text
$env:TEMP\RenderTest\tlou-i_2026.08.09_03.41_frame801.rdc
```

历史日志关键信息：

```text
Used API: D3D12
Got a new capture: 0 (frame 801)
```

当前未签名、非 honest-proxy 版本的启动验证中，`proxy.log` 已确认：

```text
LoadLibrary rendertest.dll: OK
```

## 快速排查表

| 现象 | 优先检查 |
|---|---|
| 游戏目录只有 `dxgi.dll.tmp`，没有 `dxgi.dll` | proxy 已自改名；下次启动前重新运行部署脚本 |
| `proxy.log` 显示 `Not game process` | `IsGameProcess()` 没匹配到目标 exe/path |
| 没有 RenderDoc overlay | 先确认 `proxy.log` 里是否有 `LoadLibrary rendertest.dll: OK` |
| 构建报缺少 `breakpad_common.lib` | 先构建 Breakpad 三个依赖项目 |
| 输出签名状态是 `NotSigned` | 本分支预期行为，不是错误 |
