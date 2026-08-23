# Silver Palace 截帧复现说明

快速部署/使用可先看：`docs/silver-palace-capture-quickstart.md`。

文档中的 `<SILVER_BIN>` 表示使用者本机 Silver Palace 的 `Binaries\Win64` 目录；为避免泄露个人路径，这里不记录实际绝对路径。

本文档只记录本分支为了 `<SILVER_BIN>` 可复现截帧而保留的差异。代码修改痕迹是权威来源。

## 分支目标

- 目标目录：`<SILVER_BIN>`
- 输出产物：`x64\Release\dxgi.dll`、`x64\Release\rendertest.dll`
- 部署方式：把本地签名后的 app-local `dxgi.dll` proxy 与 `rendertest.dll` 放到目标目录。
- 成功判据：Silver Palace 进程加载目标目录下的 `rendertest.dll`，并监听 RenderDoc target-control 端口。

## 保留的代码适配

### 1. Silver Palace 目标识别

文件：`version_proxy/version_proxy.cpp`

基线 `IsGameProcess()` 只匹配既有目标名，Silver Palace 不会命中。本分支改为 Silver Palace 目标识别：

```cpp
return ContainsAsciiNoCase(exePath, L"SilverPalace") ||
       ContainsAsciiNoCase(exePath, L"Silver Palace");
```

只有目标进程命中时才加载同目录 `rendertest.dll`。

### 2. 诚实 app-local DXGI proxy

文件：`version_proxy/version_proxy.cpp`

本分支把 proxy 收敛成最小行为：

1. 从 System32 加载真实 `dxgi.dll`。
2. 缓存并转发 `version_proxy.def` 中声明的 DXGI 导出。
3. 从 System32 预加载 `d3d12.dll`。
4. 对 Silver Palace 目标进程加载同目录 `rendertest.dll`。

同时删除 Silver Palace 复现不需要、且会改变进程/磁盘状态的行为：

- 不再把 `dxgi.dll` 改名为 `dxgi.dll.tmp`。
- 不再修改 PEB module list 做模块名伪装。
- 不再创建或写入 `proxy.log`。

### 3. 部署时本地 Authenticode 代码签名

文件：`tools/Deploy-SilverPalace-Capture.ps1`

部署脚本默认在复制前对以下两个文件做签名：

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

先关闭 Silver Palace 相关进程，然后执行：

```powershell
cd "<repo>"

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File ".\tools\Deploy-SilverPalace-Capture.ps1" `
  -GameDir "<SILVER_BIN>" `
  -BuildDir ".\x64\Release"
```

脚本行为：

1. 检查构建产物和目标目录。
2. 拒绝在 Silver Palace 相关进程运行时替换文件。
3. 本地签名 `dxgi.dll` 与 `rendertest.dll`。
4. 在目标目录创建 `codex-silver-palace-capture-backup-YYYYMMDD-HHMMSS`。
5. 备份旧 `dxgi.dll`、`dxgi.dll.tmp`、`rendertest.dll`、`proxy.log`。
6. 复制新 `dxgi.dll` 与 `rendertest.dll` 到目标目录。
7. 输出 SHA-256 与签名状态。

部署输出中两个文件的 `Signature` 应为 `Valid`。

## 运行验证

启动游戏后检查目标进程、模块和 RenderDoc target-control 端口：

```powershell
$GameDir = "<SILVER_BIN>"
$targets = Get-CimInstance Win32_Process |
  Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($GameDir, [System.StringComparison]::OrdinalIgnoreCase) }
$targets | Select-Object ProcessId, Name, CommandLine

$targets | ForEach-Object {
  Get-Process -Id $_.ProcessId | ForEach-Object {
    $_.Modules | Where-Object ModuleName -match 'rendertest|dxgi|d3d12'
  }
}

Get-NetTCPConnection | Where-Object { $_.LocalPort -ge 38920 -and $_.LocalPort -le 39000 }
```

期望看到：

```text
rendertest.dll 位于 <SILVER_BIN>
0.0.0.0:38920 Listen，OwningProcess 为 Silver Palace 目标进程
```

说明：模块列表里的 `dxgi.dll` 可能显示为 `C:\Windows\System32\dxgi.dll`，这是 proxy 预加载真实 DXGI 后的正常现象；判断是否挂载以 `rendertest.dll` 和 target-control 端口为准。
