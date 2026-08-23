// dxgi.dll proxy for Silver Palace app-local RenderDoc capture.
//
// Silver Palace uses the app directory DXGI search path successfully when the
// proxy behaves like a normal local DXGI shim: load System32 dxgi.dll/d3d12.dll,
// forward DXGI exports, and load sibling rendertest.dll for the target process.
// This variant intentionally avoids disk self-rename, PEB module-name mutation,
// and proxy.log side effects.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

static HMODULE g_hRealDxgi = NULL;

typedef HRESULT(WINAPI *PFN_CreateDXGIFactory)(REFIID riid, void **ppFactory);
typedef HRESULT(WINAPI *PFN_CreateDXGIFactory2)(UINT Flags, REFIID riid, void **ppFactory);
typedef HRESULT(WINAPI *PFN_DXGIGetDebugInterface1)(UINT Flags, REFIID riid, void **ppDebug);
typedef void *PFN_Generic;

static PFN_CreateDXGIFactory g_real_CreateDXGIFactory = NULL;
static PFN_CreateDXGIFactory g_real_CreateDXGIFactory1 = NULL;
static PFN_CreateDXGIFactory2 g_real_CreateDXGIFactory2 = NULL;
static PFN_DXGIGetDebugInterface1 g_real_DXGIGetDebugInterface1 = NULL;
static PFN_Generic g_real_DXGIDeclareAdapterRemovalSupport = NULL;
static PFN_Generic g_real_DXGIDisableVBlankVirtualization = NULL;
static PFN_Generic g_real_DXGIReportAdapterConfiguration = NULL;
static PFN_Generic g_real_ApplyCompatResolutionQuirking = NULL;
static PFN_Generic g_real_CompatString = NULL;
static PFN_Generic g_real_CompatValue = NULL;
static PFN_Generic g_real_DXGID3D10CreateDevice = NULL;
static PFN_Generic g_real_DXGID3D10CreateLayeredDevice = NULL;
static PFN_Generic g_real_DXGID3D10GetLayeredDeviceSize = NULL;
static PFN_Generic g_real_DXGID3D10RegisterLayers = NULL;
static PFN_Generic g_real_DXGIDumpJournal = NULL;
static PFN_Generic g_real_PIXBeginCapture = NULL;
static PFN_Generic g_real_PIXEndCapture = NULL;
static PFN_Generic g_real_PIXGetCaptureState = NULL;
static PFN_Generic g_real_SetAppCompatStringPointer = NULL;
static PFN_Generic g_real_UpdateHMDEmulationStatus = NULL;

static wchar_t ToLowerAscii(wchar_t ch)
{
    if(ch >= L'A' && ch <= L'Z')
        return ch + (L'a' - L'A');
    return ch;
}

static bool ContainsAsciiNoCase(const wchar_t *haystack, const wchar_t *needle)
{
    if(!haystack || !needle)
        return false;

    const size_t needleLen = wcslen(needle);
    if(needleLen == 0)
        return true;

    for(const wchar_t *scan = haystack; *scan; ++scan)
    {
        size_t i = 0;
        for(; i < needleLen && scan[i]; ++i)
        {
            if(ToLowerAscii(scan[i]) != ToLowerAscii(needle[i]))
                break;
        }

        if(i == needleLen)
            return true;
    }

    return false;
}

static bool BuildSystemDllPath(wchar_t *path, size_t pathCount, const wchar_t *dllName)
{
    if(!path || pathCount == 0 || !dllName)
        return false;

    DWORD len = GetSystemDirectoryW(path, (UINT)pathCount);
    if(len == 0 || len >= pathCount)
    {
        path[0] = L'\0';
        return false;
    }

    if(path[len - 1] != L'\\')
        wcscat_s(path, pathCount, L"\\");

    wcscat_s(path, pathCount, dllName);
    return true;
}

static bool BuildSiblingDllPath(wchar_t *path, size_t pathCount, HMODULE selfModule, const wchar_t *dllName)
{
    if(!path || pathCount == 0 || !dllName)
        return false;

    DWORD len = GetModuleFileNameW(selfModule, path, (DWORD)pathCount);
    if(len == 0 || len >= pathCount)
    {
        path[0] = L'\0';
        return false;
    }

    wchar_t *slash = wcsrchr(path, L'\\');
    if(slash)
        *(slash + 1) = L'\0';
    else
        path[0] = L'\0';

    wcscat_s(path, pathCount, dllName);
    return true;
}

static bool IsSilverPalaceProcess()
{
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if(len == 0 || len >= MAX_PATH)
        return false;

    return ContainsAsciiNoCase(exePath, L"SilverPalace") ||
           ContainsAsciiNoCase(exePath, L"Silver Palace");
}

static void CacheAllRealProcs()
{
    g_real_CreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(g_hRealDxgi, "CreateDXGIFactory");
    g_real_CreateDXGIFactory1 = (PFN_CreateDXGIFactory)GetProcAddress(g_hRealDxgi, "CreateDXGIFactory1");
    g_real_CreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(g_hRealDxgi, "CreateDXGIFactory2");
    g_real_DXGIGetDebugInterface1 = (PFN_DXGIGetDebugInterface1)GetProcAddress(g_hRealDxgi, "DXGIGetDebugInterface1");
    g_real_DXGIDeclareAdapterRemovalSupport = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGIDeclareAdapterRemovalSupport");
    g_real_DXGIDisableVBlankVirtualization = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGIDisableVBlankVirtualization");
    g_real_DXGIReportAdapterConfiguration = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGIReportAdapterConfiguration");
    g_real_ApplyCompatResolutionQuirking = (PFN_Generic)GetProcAddress(g_hRealDxgi, "ApplyCompatResolutionQuirking");
    g_real_CompatString = (PFN_Generic)GetProcAddress(g_hRealDxgi, "CompatString");
    g_real_CompatValue = (PFN_Generic)GetProcAddress(g_hRealDxgi, "CompatValue");
    g_real_DXGID3D10CreateDevice = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGID3D10CreateDevice");
    g_real_DXGID3D10CreateLayeredDevice = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGID3D10CreateLayeredDevice");
    g_real_DXGID3D10GetLayeredDeviceSize = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGID3D10GetLayeredDeviceSize");
    g_real_DXGID3D10RegisterLayers = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGID3D10RegisterLayers");
    g_real_DXGIDumpJournal = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGIDumpJournal");
    g_real_PIXBeginCapture = (PFN_Generic)GetProcAddress(g_hRealDxgi, "PIXBeginCapture");
    g_real_PIXEndCapture = (PFN_Generic)GetProcAddress(g_hRealDxgi, "PIXEndCapture");
    g_real_PIXGetCaptureState = (PFN_Generic)GetProcAddress(g_hRealDxgi, "PIXGetCaptureState");
    g_real_SetAppCompatStringPointer = (PFN_Generic)GetProcAddress(g_hRealDxgi, "SetAppCompatStringPointer");
    g_real_UpdateHMDEmulationStatus = (PFN_Generic)GetProcAddress(g_hRealDxgi, "UpdateHMDEmulationStatus");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    (void)lpReserved;

    if(reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        wchar_t path[MAX_PATH] = {};
        if(BuildSystemDllPath(path, MAX_PATH, L"dxgi.dll"))
        {
            g_hRealDxgi = LoadLibraryW(path);
            if(g_hRealDxgi)
                CacheAllRealProcs();
        }

        if(BuildSystemDllPath(path, MAX_PATH, L"d3d12.dll"))
            LoadLibraryW(path);

        if(IsSilverPalaceProcess() && BuildSiblingDllPath(path, MAX_PATH, hModule, L"rendertest.dll"))
            LoadLibraryW(path);
    }

    return TRUE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory)
{
    if(!g_real_CreateDXGIFactory)
        return E_FAIL;
    return g_real_CreateDXGIFactory(riid, ppFactory);
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory)
{
    if(!g_real_CreateDXGIFactory1)
        return E_FAIL;
    return g_real_CreateDXGIFactory1(riid, ppFactory);
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory)
{
    if(!g_real_CreateDXGIFactory2)
        return E_FAIL;
    return g_real_CreateDXGIFactory2(Flags, riid, ppFactory);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **ppDebug)
{
    if(!g_real_DXGIGetDebugInterface1)
        return E_FAIL;
    return g_real_DXGIGetDebugInterface1(Flags, riid, ppDebug);
}

typedef HRESULT(WINAPI *PFN_Void_HRESULT)();
typedef HRESULT(WINAPI *PFN_1)(void *);
typedef HRESULT(WINAPI *PFN_3)(void *, void *, void *);
typedef HRESULT(WINAPI *PFN_5)(void *, void *, void *, void *, void *);
typedef void *(WINAPI *PFN_2_ptr)(void *, void *);
typedef void(WINAPI *PFN_2_void)(void *, void *);

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport()
{ return g_real_DXGIDeclareAdapterRemovalSupport ? ((PFN_Void_HRESULT)g_real_DXGIDeclareAdapterRemovalSupport)() : E_FAIL; }

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIDisableVBlankVirtualization()
{ return g_real_DXGIDisableVBlankVirtualization ? ((PFN_Void_HRESULT)g_real_DXGIDisableVBlankVirtualization)() : E_FAIL; }

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIReportAdapterConfiguration(void *a)
{ return g_real_DXGIReportAdapterConfiguration ? ((PFN_1)g_real_DXGIReportAdapterConfiguration)(a) : E_FAIL; }

extern "C" __declspec(dllexport) HRESULT WINAPI ApplyCompatResolutionQuirking(void *a)
{ return g_real_ApplyCompatResolutionQuirking ? ((PFN_1)g_real_ApplyCompatResolutionQuirking)(a) : E_FAIL; }

extern "C" __declspec(dllexport) HRESULT WINAPI CompatString(void *a, void *b, void *c)
{ return g_real_CompatString ? ((PFN_3)g_real_CompatString)(a, b, c) : E_FAIL; }

extern "C" __declspec(dllexport) HRESULT WINAPI CompatValue(void *a, void *b, void *c)
{ return g_real_CompatValue ? ((PFN_3)g_real_CompatValue)(a, b, c) : E_FAIL; }

extern "C" __declspec(dllexport) HRESULT WINAPI DXGID3D10CreateDevice(void *a, void *b, void *c, void *d, void *e)
{ return g_real_DXGID3D10CreateDevice ? ((PFN_5)g_real_DXGID3D10CreateDevice)(a, b, c, d, e) : E_FAIL; }

extern "C" __declspec(dllexport) HRESULT WINAPI DXGID3D10CreateLayeredDevice(void *a, void *b, void *c, void *d, void *e)
{ return g_real_DXGID3D10CreateLayeredDevice ? ((PFN_5)g_real_DXGID3D10CreateLayeredDevice)(a, b, c, d, e) : E_FAIL; }

extern "C" __declspec(dllexport) void *WINAPI DXGID3D10GetLayeredDeviceSize(void *a, void *b)
{ return g_real_DXGID3D10GetLayeredDeviceSize ? ((PFN_2_ptr)g_real_DXGID3D10GetLayeredDeviceSize)(a, b) : NULL; }

extern "C" __declspec(dllexport) HRESULT WINAPI DXGID3D10RegisterLayers(void *a, void *b, void *c, void *d, void *e)
{ return g_real_DXGID3D10RegisterLayers ? ((PFN_5)g_real_DXGID3D10RegisterLayers)(a, b, c, d, e) : E_FAIL; }

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIDumpJournal(void *a)
{ return g_real_DXGIDumpJournal ? ((PFN_1)g_real_DXGIDumpJournal)(a) : E_FAIL; }

extern "C" __declspec(dllexport) void *WINAPI PIXBeginCapture(void *a, void *b)
{ return g_real_PIXBeginCapture ? ((PFN_2_ptr)g_real_PIXBeginCapture)(a, b) : NULL; }

extern "C" __declspec(dllexport) void *WINAPI PIXEndCapture(void *a, void *b)
{ return g_real_PIXEndCapture ? ((PFN_2_ptr)g_real_PIXEndCapture)(a, b) : NULL; }

extern "C" __declspec(dllexport) void *WINAPI PIXGetCaptureState(void *a, void *b)
{ return g_real_PIXGetCaptureState ? ((PFN_2_ptr)g_real_PIXGetCaptureState)(a, b) : NULL; }

extern "C" __declspec(dllexport) void WINAPI SetAppCompatStringPointer(void *a, void *b)
{ if(g_real_SetAppCompatStringPointer) ((PFN_2_void)g_real_SetAppCompatStringPointer)(a, b); }

extern "C" __declspec(dllexport) void WINAPI UpdateHMDEmulationStatus(void *a, void *b)
{ if(g_real_UpdateHMDEmulationStatus) ((PFN_2_void)g_real_UpdateHMDEmulationStatus)(a, b); }
