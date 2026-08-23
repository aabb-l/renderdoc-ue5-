// dxgi_proxy.cpp — dxgi.dll proxy with RenderDoc payload loading and legacy masquerade
//
// Strategy:
// 1. Game calls LoadLibrary("dxgi.dll") → loads this proxy from game directory
// 2. DllMain only loads System32 dxgi.dll and caches real function pointers
// 3. DllMain preloads System32 d3d12.dll and loads rendertest.dll from this DLL's directory
// 4. Proxy exports forward to System32 dxgi.dll
// 5. The proxy renames its disk image, rewrites its PEB loader names, and records proxy.log

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdarg.h>

#include "../renderdoc/api/app/renderdoc_app.h"

// ---- logging ----
static FILE *g_log = NULL;

static void LogMsg(const char *fmt, ...)
{
    if(!g_log)
        return;

    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fflush(g_log);
}

// ---- PEB module name masquerade ----
// winternl.h only exposes part of these loader structures, so keep the
// minimal prefixes needed to traverse InLoadOrderModuleList.
typedef struct _MY_PEB_LDR_DATA
{
    ULONG Length;
    BOOLEAN Initialized;
    HANDLE SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} MY_PEB_LDR_DATA;

typedef struct _MY_LDR_DATA_TABLE_ENTRY
{
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} MY_LDR_DATA_TABLE_ENTRY;

static wchar_t g_masqueradeBaseName[64] = {};
static wchar_t g_masqueradeFullPath[MAX_PATH] = {};
static UNICODE_STRING g_originalBaseDllName = {};
static UNICODE_STRING g_originalFullDllName = {};
static BOOL g_hasOriginalModuleNames = FALSE;

static void AssignOwnedUnicodeString(UNICODE_STRING &target, wchar_t *storage,
                                     size_t storageCount, const wchar_t *value)
{
    wcscpy_s(storage, storageCount, value);
    target.Buffer = storage;
    target.Length = (USHORT)(wcslen(storage) * sizeof(wchar_t));
    target.MaximumLength = (USHORT)(storageCount * sizeof(wchar_t));
}

static void MasqueradeModuleName(HMODULE hModule, const wchar_t *newBaseName,
                                 const wchar_t *newFullPath)
{
#ifdef _WIN64
    PPEB peb = (PPEB)__readgsqword(0x60);
#else
    PPEB peb = (PPEB)__readfsdword(0x30);
#endif

    if(!peb || !peb->Ldr)
    {
        LogMsg("[dxgi_proxy] PEB masquerade: loader data unavailable\n");
        return;
    }

    MY_PEB_LDR_DATA *ldr = (MY_PEB_LDR_DATA *)peb->Ldr;
    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    LIST_ENTRY *curr = head->Flink;

    while(curr && curr != head)
    {
        MY_LDR_DATA_TABLE_ENTRY *entry =
            CONTAINING_RECORD(curr, MY_LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

        if(entry->DllBase == (PVOID)hModule)
        {
            if(!g_hasOriginalModuleNames)
            {
                g_originalBaseDllName = entry->BaseDllName;
                g_originalFullDllName = entry->FullDllName;
                g_hasOriginalModuleNames = TRUE;
            }

            AssignOwnedUnicodeString(entry->BaseDllName, g_masqueradeBaseName,
                                     sizeof(g_masqueradeBaseName) / sizeof(g_masqueradeBaseName[0]),
                                     newBaseName);
            AssignOwnedUnicodeString(entry->FullDllName, g_masqueradeFullPath,
                                     sizeof(g_masqueradeFullPath) / sizeof(g_masqueradeFullPath[0]),
                                     newFullPath);
            LogMsg("[dxgi_proxy] PEB masquerade: renamed to '%ls'\n", newBaseName);
            return;
        }

        curr = curr->Flink;
    }

    LogMsg("[dxgi_proxy] PEB masquerade: module %p not found in PEB!\n", (void *)hModule);
}

static void RestoreModuleName(HMODULE hModule)
{
    if(!g_hasOriginalModuleNames)
        return;

#ifdef _WIN64
    PPEB peb = (PPEB)__readgsqword(0x60);
#else
    PPEB peb = (PPEB)__readfsdword(0x30);
#endif

    if(!peb || !peb->Ldr)
        return;

    MY_PEB_LDR_DATA *ldr = (MY_PEB_LDR_DATA *)peb->Ldr;
    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    LIST_ENTRY *curr = head->Flink;

    while(curr && curr != head)
    {
        MY_LDR_DATA_TABLE_ENTRY *entry =
            CONTAINING_RECORD(curr, MY_LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

        if(entry->DllBase == (PVOID)hModule)
        {
            entry->BaseDllName = g_originalBaseDllName;
            entry->FullDllName = g_originalFullDllName;
            g_hasOriginalModuleNames = FALSE;
            LogMsg("[dxgi_proxy] PEB masquerade: original loader names restored\n");
            return;
        }

        curr = curr->Flink;
    }
}

static void BuildRealDxgiPath(wchar_t *path, size_t pathCount, const wchar_t *sysDir)
{
    wcscpy_s(path, pathCount, sysDir);
    wcscat_s(path, pathCount, L"\\");
    wcscat_s(path, pathCount, L"dxgi.dll");
}

static void BuildSystemD3D12Path(wchar_t *path, size_t pathCount, const wchar_t *sysDir)
{
    wcscpy_s(path, pathCount, sysDir);
    wcscat_s(path, pathCount, L"\\");
    wcscat_s(path, pathCount, L"d3d12.dll");
}

static void BuildSiblingRenderTestPath(wchar_t *path, size_t pathCount, HMODULE selfModule)
{
    GetModuleFileNameW(selfModule, path, (DWORD)pathCount);
    wchar_t *slash = wcsrchr(path, L'\\');
    if(slash)
        *(slash + 1) = L'\0';
    else if(pathCount > 0)
        path[0] = L'\0';
    wcscat_s(path, pathCount, L"rendertest.dll");
}

static void EnableRenderDocUnsupportedVendorExtensions(HMODULE renderTestModule)
{
    if(!renderTestModule)
        return;

    pRENDERDOC_GetAPI getAPI =
        (pRENDERDOC_GetAPI)GetProcAddress(renderTestModule, "RENDERDOC_GetAPI");
    if(!getAPI)
    {
        LogMsg("[dxgi_proxy] Capture options: RENDERDOC_GetAPI unavailable\n");
        return;
    }

    RENDERDOC_API_1_6_0 *api = NULL;
    if(!getAPI(eRENDERDOC_API_Version_1_6_0, (void **)&api) || !api ||
       !api->SetCaptureOptionU32)
    {
        LogMsg("[dxgi_proxy] Capture options: API 1.6.0 unavailable\n");
        return;
    }

    // This option expects an IHV vendor ID. Enable NVIDIA NvAPI passthrough.
    const int vendorResult = api->SetCaptureOptionU32(
        eRENDERDOC_Option_AllowUnsupportedVendorExtensions, 0x10DE);
    const int childResult =
        api->SetCaptureOptionU32(eRENDERDOC_Option_HookIntoChildren, 1);

    LogMsg("[dxgi_proxy] Capture options: NVIDIA=%s, HookIntoChildren=1 (%s)\n",
           vendorResult ? "OK" : "FAIL", childResult ? "OK" : "FAIL");
}

// ---- real dxgi.dll cached pointers ----
static HMODULE g_hRealDxgi = NULL;

typedef HRESULT(WINAPI *PFN_CreateDXGIFactory)(REFIID riid, void **ppFactory);
typedef HRESULT(WINAPI *PFN_CreateDXGIFactory2)(UINT Flags, REFIID riid, void **ppFactory);
typedef HRESULT(WINAPI *PFN_DXGIGetDebugInterface1)(UINT Flags, REFIID riid, void **ppDebug);

static PFN_CreateDXGIFactory  g_real_CreateDXGIFactory  = NULL;
static PFN_CreateDXGIFactory  g_real_CreateDXGIFactory1 = NULL;
static PFN_CreateDXGIFactory2 g_real_CreateDXGIFactory2 = NULL;
static PFN_DXGIGetDebugInterface1 g_real_DXGIGetDebugInterface1 = NULL;

typedef void *PFN_Generic;
static PFN_Generic g_real_DXGIDeclareAdapterRemovalSupport = NULL;
static PFN_Generic g_real_DXGIDisableVBlankVirtualization  = NULL;
static PFN_Generic g_real_DXGIReportAdapterConfiguration   = NULL;
static PFN_Generic g_real_ApplyCompatResolutionQuirking     = NULL;
static PFN_Generic g_real_CompatString                      = NULL;
static PFN_Generic g_real_CompatValue                       = NULL;
static PFN_Generic g_real_DXGID3D10CreateDevice             = NULL;
static PFN_Generic g_real_DXGID3D10CreateLayeredDevice      = NULL;
static PFN_Generic g_real_DXGID3D10GetLayeredDeviceSize     = NULL;
static PFN_Generic g_real_DXGID3D10RegisterLayers           = NULL;
static PFN_Generic g_real_DXGIDumpJournal                   = NULL;
static PFN_Generic g_real_PIXBeginCapture                   = NULL;
static PFN_Generic g_real_PIXEndCapture                     = NULL;
static PFN_Generic g_real_PIXGetCaptureState                = NULL;
static PFN_Generic g_real_SetAppCompatStringPointer         = NULL;
static PFN_Generic g_real_UpdateHMDEmulationStatus          = NULL;

static void CacheAllRealProcs()
{
    g_real_CreateDXGIFactory  = (PFN_CreateDXGIFactory)GetProcAddress(g_hRealDxgi, "CreateDXGIFactory");
    g_real_CreateDXGIFactory1 = (PFN_CreateDXGIFactory)GetProcAddress(g_hRealDxgi, "CreateDXGIFactory1");
    g_real_CreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(g_hRealDxgi, "CreateDXGIFactory2");
    g_real_DXGIGetDebugInterface1 = (PFN_DXGIGetDebugInterface1)GetProcAddress(g_hRealDxgi, "DXGIGetDebugInterface1");
    g_real_DXGIDeclareAdapterRemovalSupport = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGIDeclareAdapterRemovalSupport");
    g_real_DXGIDisableVBlankVirtualization  = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGIDisableVBlankVirtualization");
    g_real_DXGIReportAdapterConfiguration   = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGIReportAdapterConfiguration");
    g_real_ApplyCompatResolutionQuirking     = (PFN_Generic)GetProcAddress(g_hRealDxgi, "ApplyCompatResolutionQuirking");
    g_real_CompatString                      = (PFN_Generic)GetProcAddress(g_hRealDxgi, "CompatString");
    g_real_CompatValue                       = (PFN_Generic)GetProcAddress(g_hRealDxgi, "CompatValue");
    g_real_DXGID3D10CreateDevice             = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGID3D10CreateDevice");
    g_real_DXGID3D10CreateLayeredDevice      = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGID3D10CreateLayeredDevice");
    g_real_DXGID3D10GetLayeredDeviceSize     = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGID3D10GetLayeredDeviceSize");
    g_real_DXGID3D10RegisterLayers           = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGID3D10RegisterLayers");
    g_real_DXGIDumpJournal                   = (PFN_Generic)GetProcAddress(g_hRealDxgi, "DXGIDumpJournal");
    g_real_PIXBeginCapture                   = (PFN_Generic)GetProcAddress(g_hRealDxgi, "PIXBeginCapture");
    g_real_PIXEndCapture                     = (PFN_Generic)GetProcAddress(g_hRealDxgi, "PIXEndCapture");
    g_real_PIXGetCaptureState                = (PFN_Generic)GetProcAddress(g_hRealDxgi, "PIXGetCaptureState");
    g_real_SetAppCompatStringPointer         = (PFN_Generic)GetProcAddress(g_hRealDxgi, "SetAppCompatStringPointer");
    g_real_UpdateHMDEmulationStatus          = (PFN_Generic)GetProcAddress(g_hRealDxgi, "UpdateHMDEmulationStatus");

    LogMsg("[dxgi_proxy] Cached CreateDXGIFactory=%p, Factory1=%p, Factory2=%p\n",
           (void *)g_real_CreateDXGIFactory, (void *)g_real_CreateDXGIFactory1,
           (void *)g_real_CreateDXGIFactory2);
}

// ---- DllMain ----
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if(reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        wchar_t logPath[MAX_PATH];
        GetModuleFileNameW(NULL, logPath, MAX_PATH);
        wchar_t *logSlash = wcsrchr(logPath, L'\\');
        if(logSlash)
            *(logSlash + 1) = L'\0';
        else
            logPath[0] = L'\0';
        wcscat_s(logPath, L"proxy.log");
        _wfopen_s(&g_log, logPath, L"a");

        LogMsg("\n[dxgi_proxy] === DllMain ATTACH (module=%p) ===\n", (void *)hModule);

        // Capture paths before changing the loader entry. GetModuleFileNameW(hModule)
        // can observe the rewritten FullDllName after the PEB masquerade.
        wchar_t renderTestPath[MAX_PATH];
        BuildSiblingRenderTestPath(renderTestPath, MAX_PATH, hModule);

        wchar_t oldPath[MAX_PATH];
        GetModuleFileNameW(hModule, oldPath, MAX_PATH);

        // Step 1: Load the real System32 graphics DLLs.
        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW(sysDir, MAX_PATH);
        wchar_t path[MAX_PATH];

        BuildRealDxgiPath(path, MAX_PATH, sysDir);
        g_hRealDxgi = LoadLibraryW(path);
        LogMsg("[dxgi_proxy] LoadLibrary real dxgi: %s (%p)\n",
               g_hRealDxgi ? "OK" : "FAIL", (void *)g_hRealDxgi);

        BuildSystemD3D12Path(path, MAX_PATH, sysDir);
        HMODULE hD3D12 = LoadLibraryW(path);
        LogMsg("[dxgi_proxy] LoadLibrary real d3d12: %s (%p)\n",
               hD3D12 ? "OK" : "FAIL", (void *)hD3D12);

        // Step 2: Cache all real function pointers.
        if(g_hRealDxgi)
            CacheAllRealProcs();

        // Step 3: Rename the local proxy image on disk.
        wchar_t newPath[MAX_PATH];
        wcscpy_s(newPath, oldPath);
        wchar_t *renameSlash = wcsrchr(newPath, L'\\');
        if(renameSlash)
            *(renameSlash + 1) = L'\0';
        else
            newPath[0] = L'\0';
        wcscat_s(newPath, L"dxgi.dll.tmp");

        BOOL renamed = MoveFileW(oldPath, newPath);
        DWORD renameError = renamed ? ERROR_SUCCESS : GetLastError();
        LogMsg("[dxgi_proxy] Rename dxgi.dll -> dxgi.dll.tmp: %s (err=%lu)\n",
               renamed ? "OK" : "FAIL", renameError);

        // Step 4: Rewrite the proxy's loader names using process-lifetime buffers.
        wchar_t fakeFullPath[MAX_PATH];
        wcscpy_s(fakeFullPath, sysDir);
        wcscat_s(fakeFullPath, L"\\");
        wcscat_s(fakeFullPath, L"winmm.dll");
        MasqueradeModuleName(hModule, L"winmm.dll", fakeFullPath);

        // Step 5: Load RenderDoc from the path captured before PEB rewriting.
        HMODULE hRenderTest = LoadLibraryW(renderTestPath);
        LogMsg("[dxgi_proxy] LoadLibrary rendertest.dll: %s (%p)\n",
               hRenderTest ? "OK" : "FAIL", (void *)hRenderTest);
        EnableRenderDocUnsupportedVendorExtensions(hRenderTest);

        LogMsg("[dxgi_proxy] Init complete\n");
    }
    else if(reason == DLL_PROCESS_DETACH)
    {
        if(lpReserved == NULL)
            RestoreModuleName(hModule);

        if(g_log)
        {
            LogMsg("[dxgi_proxy] DllMain DETACH (lpReserved=%p)\n", lpReserved);
            fclose(g_log);
            g_log = NULL;
        }
    }
    return TRUE;
}

// ====================================================================
// Exported functions — use pre-cached pointers to System32 dxgi.dll
// ====================================================================

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory)
{
    if(!g_real_CreateDXGIFactory) return E_FAIL;
    return g_real_CreateDXGIFactory(riid, ppFactory);
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory)
{
    if(!g_real_CreateDXGIFactory1) return E_FAIL;
    return g_real_CreateDXGIFactory1(riid, ppFactory);
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory)
{
    if(!g_real_CreateDXGIFactory2) return E_FAIL;
    return g_real_CreateDXGIFactory2(Flags, riid, ppFactory);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **ppDebug)
{
    if(!g_real_DXGIGetDebugInterface1) return E_FAIL;
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
