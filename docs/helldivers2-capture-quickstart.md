# Helldivers 2 截帧快速使用说明

这份说明面向已经切到 `codex/helldivers2-capture-repro` 分支的使用者；完整设计与代码适配记录见 `docs/helldivers2-capture-repro.md`。

下文的 `<HD2_BIN>` 请替换为本机 HD2 的 `bin` 目录；文档本身不记录具体个人路径。

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

先关闭 HD2 / GameGuard 相关进程，再执行：

```powershell
cd "<repo>"

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File ".\tools\Deploy-Helldivers2-Capture.ps1" `
  -GameDir "<HD2_BIN>" `
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
$GameDir = "<HD2_BIN>"
Start-Process `
  -FilePath (Join-Path $GameDir "helldivers2.exe") `
  -WorkingDirectory $GameDir
```

HD2 会切到最终进程：

```text
helldivers2.exe --bundle-dir data --release
```

## 4. 验证挂载

游戏启动后检查：

```powershell
Get-Content "<HD2_BIN>\proxy.log" -Tail 80
Get-NetTCPConnection | Where-Object { $_.LocalPort -ge 38920 -and $_.LocalPort -le 39000 }
```

预期看到：

```text
Rename dxgi.dll -> dxgi.dll.tmp: skipped for Helldivers 2 relaunch flow
LoadLibrary rendertest.dll: OK
0.0.0.0:38920 Listen
```

如果模块列表里 `dxgi.dll` 显示为 `C:\Windows\System32\dxgi.dll`，不代表失败；以 `rendertest.dll` 加载成功和 RenderDoc target-control 端口监听为准。

## 5. 常见注意点

- 不要在游戏运行时部署；脚本会拒绝替换文件。
- 游戏 `bin` 目录里应保留 `dxgi.dll`，不要只剩 `dxgi.dll.tmp`。
- 如果部署输出显示 `NotSigned` 或签名无效，先不要启动游戏，重新运行部署脚本确认签名成功。
- 这份分支只面向 HD2 复现；其它游戏不要复用这里的目标识别和重启链路假设。
