# Silver Palace 截帧快速使用说明

这份说明面向已经切到 `codex/silver-palace-capture-repro` 分支的使用者；完整设计与代码适配记录见 `docs/silver-palace-capture-repro.md`。

下文的 `<SILVER_BIN>` 请替换为本机 Silver Palace 的 `Binaries\Win64` 目录；文档本身不记录具体个人路径。

## 1. 编译

在分支根目录执行或使用 Visual Studio 构建 Release x64：

```powershell
cd "<repo>"
git submodule update --init --recursive renderdoc/3rdparty/minhook

$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild ".\renderdoc\renderdoc.vcxproj" /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
& $msbuild ".\version_proxy\version_proxy.vcxproj" /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
```

预期产物在：

```text
<repo>\x64\Release\rendertest.dll
<repo>\x64\Release\dxgi.dll
```

## 2. 部署

先关闭 Silver Palace 相关进程，再执行：

```powershell
cd "<repo>"

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File ".\tools\Deploy-SilverPalace-Capture.ps1" `
  -GameDir "<SILVER_BIN>" `
  -BuildDir ".\x64\Release"
```

部署脚本会自动备份旧文件，并对 `dxgi.dll`、`rendertest.dll` 做当前 Windows 用户本地自签名 Authenticode 代码签名：

```text
Subject: CN=Codex DXGI Test Code Signing
Hash: SHA-256
Store: Cert:\CurrentUser\My
Trust: Cert:\CurrentUser\Root + Cert:\CurrentUser\TrustedPublisher
```

部署输出中两个文件的 `Signature` 应为 `Valid`。

## 3. 启动

```powershell
$GameDir = "<SILVER_BIN>"
$exe = Get-ChildItem -LiteralPath $GameDir -Filter "*.exe" |
  Where-Object { $_.Name -match 'SilverPalace|Silver' } |
  Select-Object -First 1
Start-Process -FilePath $exe.FullName -WorkingDirectory $GameDir
```

如果本机启动入口不在 `Binaries\Win64` 目录，用正常启动器启动也可以；关键是最终渲染进程需要从 `<SILVER_BIN>` 加载 app-local `dxgi.dll`。

## 4. 验证挂载

游戏启动后检查：

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

预期看到：

```text
rendertest.dll 位于 <SILVER_BIN>
0.0.0.0:38920 Listen
```

如果模块列表里 `dxgi.dll` 显示为 `C:\Windows\System32\dxgi.dll`，不代表失败；以 `rendertest.dll` 加载成功和 RenderDoc target-control 端口监听为准。

## 5. 常见注意点

- 不要在游戏运行时部署；脚本会拒绝替换文件。
- 目标目录里应保留 `dxgi.dll`；这个分支的 proxy 不做 `dxgi.dll.tmp` 自改名。
- 如果部署输出显示 `NotSigned` 或签名无效，先不要启动游戏，重新运行部署脚本确认签名成功。
- 这份分支只面向 Silver Palace 复现；其它游戏不要复用这里的目标识别假设。
