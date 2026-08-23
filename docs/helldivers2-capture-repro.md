# Helldivers 2 截帧复现说明

快速部署/使用可先看：`docs/helldivers2-capture-quickstart.md`。

文档中的 `<HD2_BIN>` 表示使用者本机的 HD2 `bin` 目录；为避免泄露个人路径，这里不记录实际绝对路径。

本文档只记录本分支为了 `<HD2_BIN>\helldivers2.exe` 可复现截帧而保留的差异。代码修改痕迹是权威来源。

## 分支目标

- 目标程序：`<HD2_BIN>\helldivers2.exe`
- 输出产物：`x64\Release\dxgi.dll`、`x64\Release\rendertest.dll`
- 部署方式：把本地签名后的 `dxgi.dll` proxy 与 `rendertest.dll` 放到游戏 `bin` 目录
- 成功判据：最终 `helldivers2.exe --bundle-dir data --release` 进程中加载 `rendertest.dll`，并监听 RenderDoc target-control 端口。

## 保留的代码适配

### 1. 目标进程识别

文件：`version_proxy/version_proxy.cpp`

`version_proxy` 只在 `IsGameProcess()` 命中时加载同目录 `rendertest.dll`。本分支为 HD2 增加：

```cpp
wcsstr(exeName, L"helldivers2.exe") != NULL ||
wcsstr(exeName, L"Helldivers 2") != NULL
```

这是所有使用该 proxy 方案的目标都需要补齐的通用识别项；在本分支中按 HD2 路径/进程名补齐。

### 2. D3D11 delay-load fallback

文件：`renderdoc/driver/d3d11/d3d11_hooks.cpp`

HD2 首次进入 `D3D11CreateDeviceAndSwapChain_hook` 时，RenderDoc 的 onward pointer 可能还没有缓存。本分支在 `createFunc == NULL` 时先加载真实 `d3d11.dll`，再解析 `D3D11CreateDeviceAndSwapChain`：

```cpp
HMODULE d3d11Module = GetModuleHandleA("d3d11.dll");
if(d3d11Module == NULL)
  d3d11Module = LoadLibraryA("d3d11.dll");

if(d3d11Module != NULL)
  createFunc = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(
      d3d11Module, "D3D11CreateDeviceAndSwapChain");
```

### 3. HD2 重启链路保留 app-local `dxgi.dll`

文件：`version_proxy/version_proxy.cpp`

HD2 会从初始进程切换到最终进程：

```text
helldivers2.exe --bundle-dir data --release
```

基线 proxy 首次加载后会把游戏目录里的 `dxgi.dll` 改成 `dxgi.dll.tmp`。这会导致最终进程只能加载 `C:\Windows\System32\dxgi.dll`，从而错过 proxy。本分支只对 HD2 命中的进程跳过磁盘自改名：

```text
[dxgi_proxy] Rename dxgi.dll -> dxgi.dll.tmp: skipped for Helldivers 2 relaunch flow
```

PEB 伪装与 `proxy.log` 保持基线行为。

### 4. 部署时本地 Authenticode 代码签名

文件：`tools/Deploy-Helldivers2-Capture.ps1`

实测未签名产物会在 `rendertest.dll` 加载成功后退出；同一份源码加上本地 Authenticode 签名后可以进入最终 HD2/GameGuard 链路。因此部署脚本默认对以下两个文件签名后再复制到游戏目录：

```text
dxgi.dll
rendertest.dll
```

签名细节：

- 类型：Windows Authenticode 嵌入式代码签名。
- 哈希算法：SHA-256。
- 证书：当前用户证书存储里的自签名 Code Signing 证书。
- Subject：`CN=Codex DXGI Test Code Signing`。
- 私钥位置：`Cert:\CurrentUser\My`。
- 信任位置：脚本会把同一证书加入 `Cert:\CurrentUser\Root` 和 `Cert:\CurrentUser\TrustedPublisher`。
- 时间戳：不使用时间戳服务器。
- 作用范围：当前 Windows 用户；不是 Microsoft / WHQL / 商业 EV 证书，也不是机器级全局签名策略。

本分支不保留未签名部署开关，避免把已验证失败的路径写进复现流程。

## 不包含的内容

- 不修改 `version_proxy.vcxproj` 增加 PostBuild 签名。
- 不保留单独的签名脚本。
- 不保留临时验证脚本。
- 不新增其它游戏的适配项；基线已有内容不在本分支剔除范围。

## 编译流程

在本分支根目录执行；`<repo>` 表示该分支 worktree 根目录。

```powershell
cd "<repo>"
git submodule update --init --recursive renderdoc/3rdparty/minhook

$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild ".\renderdoc\renderdoc.vcxproj" /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
& $msbuild ".\version_proxy\version_proxy.vcxproj" /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
```

预期产物：

```text
x64\Release\rendertest.dll
x64\Release\dxgi.dll
```

如果首次构建缺少 Breakpad 静态库，先构建 `renderdoc/3rdparty/breakpad/client/windows` 下的 `common`、`crash_generation_client`、`exception_handler` 三个 Release x64 项目即可。

## 部署流程

先关闭 HD2 / GameGuard 相关进程，然后执行：

```powershell
cd "<repo>"

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File ".\tools\Deploy-Helldivers2-Capture.ps1" `
  -GameDir "<HD2_BIN>" `
  -BuildDir ".\x64\Release"
```

脚本行为：

1. 检查构建产物和游戏目录。
2. 拒绝在 HD2 / GameGuard 相关进程运行时替换文件。
3. 本地签名 `dxgi.dll` 与 `rendertest.dll`。
4. 在游戏目录创建 `codex-helldivers2-capture-backup-YYYYMMDD-HHMMSS`。
5. 备份旧 `dxgi.dll`、`dxgi.dll.tmp`、`rendertest.dll`、`proxy.log`。
6. 复制新 `dxgi.dll` 与 `rendertest.dll` 到游戏目录。
7. 输出 SHA-256 与签名状态。

部署输出中两个文件的 `Signature` 应为 `Valid`。

## 运行验证

启动游戏后检查：

```powershell
Get-Content "<HD2_BIN>\proxy.log" -Tail 80
Get-Process helldivers2 | ForEach-Object { $_.Modules | Where-Object ModuleName -match 'rendertest|dxgi|d3d11|d3d12' }
Get-NetTCPConnection | Where-Object { $_.LocalPort -ge 38920 -and $_.LocalPort -le 39000 }
```

期望看到：

```text
Rename dxgi.dll -> dxgi.dll.tmp: skipped for Helldivers 2 relaunch flow
LoadLibrary rendertest.dll: OK
rendertest.dll 位于游戏 bin 目录
0.0.0.0:38920 Listen，OwningProcess 为最终 helldivers2.exe
```

说明：模块列表里的 `dxgi.dll` 可能显示为 `C:\Windows\System32\dxgi.dll`，这是 proxy 预加载真实 DXGI 后的正常现象；判断是否挂载以 `rendertest.dll` 和 target-control 端口为准。
