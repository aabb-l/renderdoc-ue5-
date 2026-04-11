修改RenderDoc的特征来绕过第一步检测
核心 DLL — Replay Marker 符号


文件路径	行号	修改类型	修改前	修改后
renderdoc/api/replay/renderdoc_replay.h	52	函数名修改	renderdoc__replay__marker()	rendertest__replay__marker()


说明：这是最核心的修改。REPLAY_PROGRAM_MARKER() 宏定义所有 RenderDoc 可执行文件导出的符号，DLL 通过动态查找该符号判断当前是 replay 环境还是目标游戏进程。若 DLL 期望 rendertest__replay__marker 但 EXE 导出的是 renderdoc__replay__marker，DLL 将误判为游戏进程并注入 GPU 钩子，导致 UI 崩溃。
进程创建与注入 — 可执行文件路径硬编码


文件路径	行号	修改类型	修改前	修改后
renderdoc/os/win32/win32_process.cpp	933	字符串替换	L"\\Win32\\Development\\renderdoccmd.exe"	L"\\Win32\\Development\\rendertestcmd.exe"
renderdoc/os/win32/win32_process.cpp	946	字符串替换	L"\\Win32\\Release\\renderdoccmd.exe"	L"\\Win32\\Release\\rendertestcmd.exe"
renderdoc/os/win32/win32_process.cpp	961	字符串替换	L"\\x86\\renderdoccmd.exe"	L"\\x86\\rendertestcmd.exe"
renderdoc/os/win32/win32_process.cpp	973	字符串替换	L"\\x64\\Development\\renderdoccmd.exe"	L"\\x64\\Development\\rendertestcmd.exe"
renderdoc/os/win32/win32_process.cpp	986	字符串替换	L"\\x64\\Release\\renderdoccmd.exe"	L"\\x64\\Release\\rendertestcmd.exe"
renderdoc/os/win32/win32_process.cpp	1006	字符串替换	L"\\renderdoccmd.exe"	L"\\rendertestcmd.exe"

## Shim DLL 路径硬编码

Shim DLL 路径硬编码


文件路径	行号	修改类型	修改前	修改后
renderdoc/os/win32/win32_process.cpp	1783	字符串拼接	"\\renderdocshim64.dll"	"\\rendertestshim64.dll"
renderdoc/os/win32/win32_process.cpp	1792	字符串拼接	"\\Win32\\Development\\renderdocshim32.dll"	"\\Win32\\Development\\rendertestshim32.dll"
renderdoc/os/win32/win32_process.cpp	1803	字符串拼接	"\\Win32\\Release\\renderdocshim32.dll"	"\\Win32\\Release\\rendertestshim32.dll"
renderdoc/os/win32/win32_process.cpp	1811	字符串拼接	"\\x86\\renderdocshim32.dll"	"\\x86\\rendertestshim32.dll"
renderdoc/os/win32/win32_process.cpp	1818	字符串拼接	"\\renderdocshim32.dll"


进程白名单 — 避免注入到 RenderTest 自身


文件路径	行号	修改类型	修改前	修改后
renderdoc/os/win32/sys_win32_hooks.cpp	342	字符串替换	"renderdoccmd.exe" || app.contains("qrenderdoc.exe")	"rendertestcmd.exe" || app.contains("qrendertest.exe")
renderdoc/os/win32/sys_win32_hooks.cpp	351	字符串替换	"renderdoccmd.exe" || cmd.contains("qrenderdoc.exe")	"rendertestcmd.exe" || cmd.contains("qrendertest.exe")


崩溃处理 — 命名内核对象


文件路径	行号	修改类型	修改前	修改后
renderdoccmd/renderdoccmd_win32.cpp	603	字符串替换	"RENDERDOC_CRASHHANDLE"	"RENDERTEST_CRASHHANDLE"
renderdoccmd/renderdoccmd_win32.cpp	818	字符串替换	GetModuleHandleA("renderdoc.dll")	GetModuleHandleA("rendertest.dll")
core/crash_handler.h	62	字符串拼接	"RenderDoc\\dumps\\a"	"RenderTest\\dumps\\a"
core/crash_handler.h	168	字符串拼接	"RenderDocBreakpadServer%llu"	"RenderTestBreakpadServer%llu"


表格 6：文件路径与注册表


文件路径	行号	修改类型	修改前	修改后
renderdoc/os/win32/win32_stringio.cpp	289	字符串拼接	"/qrenderdoc.exe"	"/qrendertest.exe"
renderdoc/os/win32/win32_stringio.cpp	300	字符串拼接	"/../qrenderdoc.exe"	"/../qrendertest.exe"
renderdoc/os/win32/win32_stringio.cpp	316	注册表路径	L"RenderDoc.RDCCapture.1\\DefaultIcon"	L"RenderTest.RDCCapture.1\\DefaultIcon"
renderdoc/os/win32/win32_stringio.cpp	358	日志路径	L"RenderDoc\\%ls_..."	L"RenderTest\\%ls_..."
renderdoc/os/win32/win32_stringio.cpp	367	日志路径	L"RenderDoc\\%ls_..."	L"RenderTest\\%ls_..."


OpenGL 窗口类名


文件路径	行号	修改类型	修改前	修改后
driver/gl/wgl_platform.cpp	28	宏定义	L"renderdocGLclass"	L"rendertestGLclass"


全局 Hook 共享内存名称


文件路径	行号	修改类型	修改前	修改后
renderdocshim/renderdocshim.h	36	宏定义	"RenderDocGlobalHookData64"	"RenderTestGlobalHookData64"
renderdocshim/renderdocshim.h	39	宏定义	"RenderDocGlobalHookData32"	"RenderTestGlobalHookData32"


资源文件


文件路径	行号	修改类型	修改前	修改后
data/renderdoc.rc	87	资源字符串	"Core DLL for RenderDoc"	"Core DLL for RenderTest"
data/renderdoc.rc	92	资源字符串	"ProductName", "RenderDoc"	"ProductName", "RenderTest"


Qt UI 层


文件路径	行号	修改类型	修改前	修改后
qrenderdoc/renderdocui_stub.cpp	62	字符串拼接	L"qrenderdoc.exe"	L"qrendertest.exe"
qrenderdoc/Code/qrenderdoc.cpp	173	Qt 翻译上下文	"qrenderdoc"	"qrendertest"
qrenderdoc/Code/qrenderdoc.cpp	198	日志输出	"QRenderDoc initialising."	"QRenderTest initialising."
qrenderdoc/Code/qrenderdoc.cpp	267	会话名	"QRenderDoc"	"QRenderTest"
qrenderdoc/Code/qrenderdoc.cpp	330	描述文本	"Qt UI for RenderDoc"	"Qt UI for RenderTest"
qrenderdoc/Code/qrenderdoc.cpp	393	版本输出	"QRenderDoc v%s"	"QRenderTest v%s"
qrenderdoc/Windows/MainWindow.cpp	1217	窗口标题	"RenderDoc "	"RenderTest "


VS 项目文件（编译输出名 + UAC 提权）


文件路径	属性	修改类型	修改前	修改后
qrenderdoc/renderdocui_stub.vcxproj	RootNamespace	修改	renderdocui_stub	rendertestui_stub
qrenderdoc/renderdocui_stub.vcxproj	ProjectName	修改	renderdocui_stub	rendertestui_stub
qrenderdoc/renderdocui_stub.vcxproj	PrimaryOutput	新增	(无)	rendertestui
qrenderdoc/renderdocui_stub.vcxproj	TargetName	新增	(无)	rendertestui
qrenderdoc/renderdocui_stub.vcxproj	UACExecutionLevel	新增	(无)	RequireAdmini


CrashSight检测
这里可以在Hook的时候采用SetThreadContext注入

修改文件:renderdoc/os/win32/win32_process.cpp

      uintptr_t loc = FindRemoteDLL(pi.dwProcessId, STRINGIZE(RDOC_BASE_NAME) ".dll");
      CloseHandle(hProcess);
      hProcess = NULL;
      if(loc != 0)