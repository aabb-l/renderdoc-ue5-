// dxgi_proxy.cpp — dxgi.dll proxy with PEB module name masquerade
//
// Strategy:
// 1. Game calls LoadLibrary("dxgi.dll") → loads this proxy from game directory
// 2. DllMain always loads System32 dxgi.dll and caches its real function pointers
// 3. For non-RenderDoc tool processes, DllMain pre-loads System32 d3d12.dll,
//    loads rendertest.dll (RenderDoc hooks installed), and renames its own module
//    entry in the PEB from "dxgi.dll" to "mfplat.dll"
//    so ACE's module name scan does not find "dxgi.dll" in the loaded modules list
// 4. Proxy exports forward to System32 dxgi.dll (already inline-hooked by RenderDoc)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

#include "process_filter.h"

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
// winternl.h only exposes InMemoryOrderModuleList.
// We define our own full PEB_LDR_DATA and LDR_DATA_TABLE_ENTRY.
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

static void MasqueradeModuleName(HMODULE hModule, const wchar_t *newBaseName, const wchar_t *newFullPath)
{
#ifdef _WIN64
    PPEB peb = (PPEB)__readgsqword(0x60);
#else
    PPEB peb = (PPEB)__readfsdword(0x30);
#endif

    MY_PEB_LDR_DATA *ldr = (MY_PEB_LDR_DATA *)peb->Ldr;
    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    LIST_ENTRY *curr = head->Flink;

    while(curr != head)
    {
        MY_LDR_DATA_TABLE_ENTRY *entry = CONTAINING_RECORD(curr, MY_LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

        if(entry->DllBase == (PVOID)hModule)
        {
            // Overwrite BaseDllName
            size_t baseLen = wcslen(newBaseName) * sizeof(wchar_t);
            if(baseLen <= entry->BaseDllName.MaximumLength)
            {
                memset(entry->BaseDllName.Buffer, 0, entry->BaseDllName.MaximumLength);
                memcpy(entry->BaseDllName.Buffer, newBaseName, baseLen);
                entry->BaseDllName.Length = (USHORT)baseLen;
            }

            // Overwrite FullDllName
            size_t fullLen = wcslen(newFullPath) * sizeof(wchar_t);
            if(fullLen <= entry->FullDllName.MaximumLength)
            {
                memset(entry->FullDllName.Buffer, 0, entry->FullDllName.MaximumLength);
                memcpy(entry->FullDllName.Buffer, newFullPath, fullLen);
                entry->FullDllName.Length = (USHORT)fullLen;
            }

            LogMsg("[dxgi_proxy] PEB masquerade: renamed to '%ls'\n", newBaseName);
            return;
        }
        curr = curr->Flink;
    }
    LogMsg("[dxgi_proxy] PEB masquerade: module %p not found in PEB!\n", (void*)hModule);
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
           (void*)g_real_CreateDXGIFactory, (void*)g_real_CreateDXGIFactory1, (void*)g_real_CreateDXGIFactory2);
}

// ---- DllMain ----
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if(reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        wchar_t logPath[MAX_PATH];
        GetModuleFileNameW(NULL, logPath, MAX_PATH);
        wchar_t *s = wcsrchr(logPath, L'\\');
        if(s)
            *(s + 1) = L'\0';
        wcscat_s(logPath, L"proxy.log");
        g_log = _wfopen(logPath, L"a");

        LogMsg("\n[dxgi_proxy] === DllMain ATTACH (module=%p) ===\n", (void*)hModule);

        wchar_t processPath[MAX_PATH] = {};
        GetModuleFileNameW(NULL, processPath, MAX_PATH);
        const bool enableProxyInjection = ShouldEnableProxyInjectionForProcessPath(processPath);

        // Step 1: Always load real System32 DXGI so exports can be forwarded.
        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW(sysDir, MAX_PATH);
        wchar_t path[MAX_PATH];

        wsprintfW(path, L"%s\\dxgi.dll", sysDir);
        g_hRealDxgi = LoadLibraryW(path);
        LogMsg("[dxgi_proxy] LoadLibrary real dxgi: %s (%p)\n", g_hRealDxgi ? "OK" : "FAIL", (void*)g_hRealDxgi);

        // Step 2: Cache all real function pointers before any RenderDoc hooks are installed.
        if(g_hRealDxgi)
            CacheAllRealProcs();

        if(enableProxyInjection)
        {
            // Step 3: Preload D3D12 only for the target application. RenderDoc replay tools
            // must select the D3D12Core version embedded in the capture themselves.
            wsprintfW(path, L"%s\\d3d12.dll", sysDir);
            HMODULE hD3D12 = LoadLibraryW(path);
            LogMsg("[dxgi_proxy] LoadLibrary real d3d12: %s (%p)\n", hD3D12 ? "OK" : "FAIL", (void*)hD3D12);

            // Step 4: Masquerade — rename the dxgi.dll file on disk and in the PEB.
            // ACE may use NtQueryVirtualMemory(MemoryMappedFilenameInformation) which reads
            // the kernel file object name. Renaming the file updates that name.
            {
                // Rename file on disk: dxgi.dll -> dxgi.dll.tmp
                wchar_t dllDir[MAX_PATH];
                GetModuleFileNameW(NULL, dllDir, MAX_PATH);
                wchar_t *sl = wcsrchr(dllDir, L'\\');
                if(sl) *(sl + 1) = L'\0';

                wchar_t oldPath[MAX_PATH], newPath[MAX_PATH];
                wcscpy_s(oldPath, dllDir);
                wcscat_s(oldPath, L"dxgi.dll");
                wcscpy_s(newPath, dllDir);
                wcscat_s(newPath, L"dxgi.dll.tmp");

                BOOL renamed = MoveFileW(oldPath, newPath);
                LogMsg("[dxgi_proxy] Rename dxgi.dll -> dxgi.dll.tmp: %s (err=%lu)\n",
                       renamed ? "OK" : "FAIL", renamed ? 0 : GetLastError());

                // Also rename PEB entry
                wchar_t fakeFullPath[MAX_PATH];
                wsprintfW(fakeFullPath, L"%s\\mfplat.dll", sysDir);
                MasqueradeModuleName(hModule, L"mfplat.dll", fakeFullPath);
            }

            // Step 5: Load rendertest.dll for all non-RenderDoc host processes.
            wchar_t dllDir[MAX_PATH];
            GetModuleFileNameW(NULL, dllDir, MAX_PATH);
            wchar_t *slash = wcsrchr(dllDir, L'\\');
            if(slash)
                *(slash + 1) = L'\0';
            wchar_t rtPath[MAX_PATH];
            wcscpy_s(rtPath, dllDir);
            wcscat_s(rtPath, L"rendertest.dll");
            HMODULE hRT = LoadLibraryW(rtPath);
            LogMsg("[dxgi_proxy] LoadLibrary rendertest.dll: %s (%p)\n", hRT ? "OK" : "FAIL", (void*)hRT);
        }
        else
        {
            LogMsg("[dxgi_proxy] RenderDoc tool process, skipping d3d12 preload, masquerade, and rendertest.dll\n");
        }

        LogMsg("[dxgi_proxy] Init complete\n");
    }
    else if(reason == DLL_PROCESS_DETACH)
    {
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
