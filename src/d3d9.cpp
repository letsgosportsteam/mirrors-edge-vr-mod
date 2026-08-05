// Mirror's Edge VR - rungs 0 and 1
//
// A d3d9.dll proxy that forwards every export to the real system library and, from rung 1,
// observes the device without altering it. It still renders nothing and changes nothing;
// the game must look and behave exactly as it does with this file absent.
//
// ---- rung 0: does the game actually call Direct3DCreate9? ----  ANSWERED: yes
//
// The executable delay-loads d3d10.dll and dxgi.dll, so it has a DX10 path, and an ini
// value is not evidence that a call happens. Measured 2026-08-04: Direct3DCreate9 called
// twice per run with SDKVersion=32, Direct3DCreate9Ex never.
//
// ---- rung 1: what are the real device parameters? ----
//
// Everything below is measurement, chosen because guessing any of it has a known cost:
//
//   * WHICH IDirect3D9 receives CreateDevice. Two are created per run and the reason is
//     unknown; the standing hypothesis is a throwaway for adapter enumeration. This
//     settles it rather than leaving a plausible story in the notes.
//   * The BACKBUFFER FORMAT, read from the surface descriptor and not from the present
//     parameters. In windowed mode BackBufferFormat is allowed to be D3DFMT_UNKNOWN, so
//     the request is not the truth. The Singularity project recorded X8R8G8B8 where the
//     answer was A8R8G8B8 and that one word later produced a D3DERR_INVALIDCALL, because
//     GetRenderTargetData requires identical formats on both surfaces.
//   * MSAA, which if present means a resolve step before anything can be shared.
//   * MANAGED vs DEFAULT pool counts, which decide how much work the texture wrapper is.
//
// ---- why vtable patching rather than COM wrapper objects ----
//
// Lifted as a decision from the Singularity mod, where it is load-bearing. A wrapper class
// per interface means unwrapping at every entry point that accepts one - SetTexture,
// SetStreamSource, SetIndices, UpdateTexture, StretchRect - and one missed site is a crash.
// Patching the vtable hands the game the REAL object, so every call that consumes it works
// untouched and only the intercepted slots differ.
//
// It also sidesteps a trap this project already measured: IDirect3D9 is created twice, and
// in one run BOTH CALLS RETURNED THE SAME ADDRESS - the first object had been released and
// the allocator reused the slot. Any scheme keyed on pointer value would have merged two
// distinct objects. Vtable patching needs no such map: D3D9 objects of a type share one
// vtable, so a single patch covers every instance, whatever address it lands on.
//
// ---- vtable slots, extracted from d3d9.h rather than remembered ----
//
// Taken by parsing STDMETHOD/STDMETHOD_ declarations in order out of the Windows SDK
// header. The same extraction reproduces every slot the Singularity mod's working hooks
// depend on - Present 17, SetRenderTarget 37, DrawPrimitive 81, SetVertexShaderConstantF
// 94, CreateQuery 118 - so the numbers here are vouched for by two independent sources.
// A first attempt at that extraction mishandled the STDMETHOD_(type, name) form and came
// out four slots low; it was caught precisely because it disagreed with the working values.

#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>

// ---------------------------------------------------------------- vtable slots

enum {
    D3D9_CreateDevice          = 16,   // IDirect3D9

    DEV_Reset                  = 16,   // IDirect3DDevice9
    DEV_Present                = 17,
    DEV_CreateTexture          = 23,
    DEV_CreateVolumeTexture    = 24,
    DEV_CreateCubeTexture      = 25,
    DEV_CreateVertexBuffer     = 26,
    DEV_CreateIndexBuffer      = 27,
};

// ---------------------------------------------------------------- state

static HMODULE          g_real       = nullptr;
static CRITICAL_SECTION g_lock;
static bool             g_lockReady  = false;
static wchar_t          g_logPath[MAX_PATH] = L"";
static long             g_createCalls = 0;

static bool  g_d3d9Patched   = false;   // IDirect3D9 vtable patched (shared by all instances)
static bool  g_devicePatched = false;   // IDirect3DDevice9 vtable patched
static long  g_frames        = 0;
static bool  g_describedBackbuffer = false;

static LONG  g_poolDefault = 0, g_poolManaged = 0, g_poolSystemMem = 0, g_poolScratch = 0;

// ---------------------------------------------------------------- logging
//
// Every run is kept, archived under the time it STARTED. The Singularity project lost two
// of three tests in one session to a log that truncated on attach: each measurement is an
// A/B across launches, so destroying the previous run destroys what the current one is
// being compared against - silently, because the file still looks complete afterwards.

static void BuildLogPath()
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    wchar_t dir[MAX_PATH];
    _snwprintf_s(dir, _TRUNCATE, L"%s\\MirrorsEdgeVR", base);
    CreateDirectoryW(dir, nullptr);
    _snwprintf_s(g_logPath, _TRUNCATE, L"%s\\mevr.log", dir);
}

static void ArchivePreviousLog()
{
    if (!g_logPath[0]) return;

    HANDLE h = CreateFileW(g_logPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    FILETIME created{};
    SYSTEMTIME st{};
    if (GetFileTime(h, &created, nullptr, nullptr)) {
        FILETIME local{};
        FileTimeToLocalFileTime(&created, &local);
        FileTimeToSystemTime(&local, &st);
    }
    CloseHandle(h);

    wchar_t archived[MAX_PATH] = L"";
    wchar_t dir[MAX_PATH]      = L"";
    wcscpy_s(dir, g_logPath);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;

    _snwprintf_s(archived, _TRUNCATE, L"%s\\mevr_%04u-%02u-%02u_%02u-%02u-%02u.log",
                 dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    MoveFileW(g_logPath, archived);
}

// _Printf_format_string_ is not decoration: build.ps1 runs /analyze and fails the build on
// C6067 and its family, because a format string that disagrees with its arguments is the
// defect that has actually cost this codebase time - it presented as a startup hang.
static void Log(_Printf_format_string_ const char* fmt, ...)
{
    if (!g_logPath[0] || !g_lockReady) return;

    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(line, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(line);

    EnterCriticalSection(&g_lock);
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, line, (DWORD)n, &written, nullptr);
        WriteFile(h, "\r\n", 2, &written, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------- naming helpers

static const char* FormatName(D3DFORMAT f)
{
    switch (f) {
        case D3DFMT_UNKNOWN:        return "UNKNOWN";
        case D3DFMT_A8R8G8B8:       return "A8R8G8B8";
        case D3DFMT_X8R8G8B8:       return "X8R8G8B8";
        case D3DFMT_R5G6B5:         return "R5G6B5";
        case D3DFMT_A2R10G10B10:    return "A2R10G10B10";
        case D3DFMT_A16B16G16R16F:  return "A16B16G16R16F";
        case D3DFMT_A32B32G32R32F:  return "A32B32G32R32F";
        case D3DFMT_R32F:           return "R32F";
        case D3DFMT_G16R16:         return "G16R16";
        case D3DFMT_D24S8:          return "D24S8";
        case D3DFMT_D24X8:          return "D24X8";
        case D3DFMT_D16:            return "D16";
        case D3DFMT_DXT1:           return "DXT1";
        case D3DFMT_DXT3:           return "DXT3";
        case D3DFMT_DXT5:           return "DXT5";
        default:                    return "(see numeric)";
    }
}

static const char* PoolName(D3DPOOL p)
{
    switch (p) {
        case D3DPOOL_DEFAULT:   return "DEFAULT";
        case D3DPOOL_MANAGED:   return "MANAGED";
        case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
        case D3DPOOL_SCRATCH:   return "SCRATCH";
        default:                return "?";
    }
}

static void CountPool(D3DPOOL p)
{
    switch (p) {
        case D3DPOOL_DEFAULT:   InterlockedIncrement(&g_poolDefault);   break;
        case D3DPOOL_MANAGED:   InterlockedIncrement(&g_poolManaged);   break;
        case D3DPOOL_SYSTEMMEM: InterlockedIncrement(&g_poolSystemMem); break;
        default:                InterlockedIncrement(&g_poolScratch);   break;
    }
}

// ---------------------------------------------------------------- vtable patching

static void* PatchVTable(void* obj, int index, void* repl)
{
    if (!obj) return nullptr;
    void** vt = *(void***)obj;
    DWORD oldProt = 0;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &oldProt)) {
        Log("[patch] VirtualProtect FAILED on slot %d, GetLastError=%lu", index, GetLastError());
        return nullptr;
    }
    void* orig = vt[index];
    vt[index] = repl;
    VirtualProtect(&vt[index], sizeof(void*), oldProt, &oldProt);
    return orig;
}

// ---------------------------------------------------------------- device hooks

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*,
                                                 HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD,
                                                       D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVB)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL,
                                                  IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateIB)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                  IDirect3DIndexBuffer9**, HANDLE*);

static PFN_Present       g_origPresent = nullptr;
static PFN_Reset         g_origReset   = nullptr;
static PFN_CreateTexture g_origCreateTex = nullptr;
static PFN_CreateVB      g_origCreateVB  = nullptr;
static PFN_CreateIB      g_origCreateIB  = nullptr;

static void LogPresentParams(const char* what, const D3DPRESENT_PARAMETERS* pp)
{
    if (!pp) { Log("[%s] present parameters were NULL", what); return; }
    Log("[%s] %ux%u  BackBufferFormat=%s (%d)  count=%u",
        what, pp->BackBufferWidth, pp->BackBufferHeight,
        FormatName(pp->BackBufferFormat), (int)pp->BackBufferFormat, pp->BackBufferCount);
    Log("[%s] Windowed=%d  SwapEffect=%d  hDeviceWindow=%p  PresentationInterval=0x%08lX",
        what, (int)pp->Windowed, (int)pp->SwapEffect, (void*)pp->hDeviceWindow,
        (unsigned long)pp->PresentationInterval);
    Log("[%s] MultiSampleType=%d quality=%lu   <- non-zero means a RESOLVE is needed before sharing",
        what, (int)pp->MultiSampleType, (unsigned long)pp->MultiSampleQuality);
    Log("[%s] AutoDepthStencil=%d format=%s (%d)  Flags=0x%08lX  RefreshHz=%u",
        what, (int)pp->EnableAutoDepthStencil,
        FormatName(pp->AutoDepthStencilFormat), (int)pp->AutoDepthStencilFormat,
        (unsigned long)pp->Flags, pp->FullScreen_RefreshRateInHz);
}

// The backbuffer as it ACTUALLY exists, not as it was requested. In windowed mode
// BackBufferFormat may legitimately be D3DFMT_UNKNOWN, so the request cannot be trusted;
// and a format recorded from the request rather than the surface is exactly the mistake
// that cost the Singularity project a D3DERR_INVALIDCALL.
static void DescribeBackbuffer(IDirect3DDevice9* dev)
{
    if (!dev || g_describedBackbuffer) return;
    g_describedBackbuffer = true;

    IDirect3DSurface9* bb = nullptr;
    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        Log("[backbuffer] GetBackBuffer FAILED hr=0x%08lX", (unsigned long)hr);
        return;
    }
    D3DSURFACE_DESC d{};
    if (SUCCEEDED(bb->GetDesc(&d))) {
        Log("*** [backbuffer] MEASURED %ux%u  format=%s (%d)  MSAA=%d/%lu  usage=0x%08lX pool=%s",
            d.Width, d.Height, FormatName(d.Format), (int)d.Format,
            (int)d.MultiSampleType, (unsigned long)d.MultiSampleQuality,
            (unsigned long)d.Usage, PoolName(d.Pool));
    }
    bb->Release();

    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(dev->GetCreationParameters(&cp))) {
        Log("[device] BehaviorFlags=0x%08lX  adapter=%u  DeviceType=%d  focusWindow=%p",
            (unsigned long)cp.BehaviorFlags, cp.AdapterOrdinal, (int)cp.DeviceType,
            (void*)cp.hFocusWindow);
    }
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev, const RECT* src,
                                              const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    long f = InterlockedIncrement(&g_frames);
    if (f == 1) {
        Log("*** first Present - the game is rendering. This is the frame hook everything later hangs off.");
        DescribeBackbuffer(dev);
    } else if (f % 600 == 0) {
        Log("[present] frame %ld   pools so far: DEFAULT=%ld MANAGED=%ld SYSTEMMEM=%ld other=%ld",
            f, g_poolDefault, g_poolManaged, g_poolSystemMem, g_poolScratch);
    }
    return g_origPresent(dev, src, dst, wnd, dirty);
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    // A Reset means the device was recreated - resolution or windowed/fullscreen change,
    // or a lost device on alt-tab. Every DEFAULT-pool resource dies here, which is why the
    // eventual VR path has to care about it. For now it is only worth seeing.
    Log("--- Reset requested ---");
    LogPresentParams("reset", pp);
    HRESULT hr = g_origReset(dev, pp);
    Log("--- Reset returned hr=0x%08lX ---", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        g_describedBackbuffer = false;   // re-measure, the surface is new
        DescribeBackbuffer(dev);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels,
                                                    DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                    IDirect3DTexture9** out, HANDLE* shared)
{
    CountPool(pool);
    return g_origCreateTex(dev, w, h, levels, usage, fmt, pool, out, shared);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateVB(IDirect3DDevice9* dev, UINT len, DWORD usage, DWORD fvf,
                                               D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared)
{
    CountPool(pool);
    return g_origCreateVB(dev, len, usage, fvf, pool, out, shared);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateIB(IDirect3DDevice9* dev, UINT len, DWORD usage, D3DFORMAT fmt,
                                               D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared)
{
    CountPool(pool);
    return g_origCreateIB(dev, len, usage, fmt, pool, out, shared);
}

static void PatchDeviceOnce(IDirect3DDevice9* dev)
{
    if (g_devicePatched || !dev) return;
    g_devicePatched = true;

    g_origPresent   = (PFN_Present)      PatchVTable(dev, DEV_Present,           (void*)&Hook_Present);
    g_origReset     = (PFN_Reset)        PatchVTable(dev, DEV_Reset,             (void*)&Hook_Reset);
    g_origCreateTex = (PFN_CreateTexture)PatchVTable(dev, DEV_CreateTexture,     (void*)&Hook_CreateTexture);
    g_origCreateVB  = (PFN_CreateVB)     PatchVTable(dev, DEV_CreateVertexBuffer,(void*)&Hook_CreateVB);
    g_origCreateIB  = (PFN_CreateIB)     PatchVTable(dev, DEV_CreateIndexBuffer, (void*)&Hook_CreateIB);

    Log("[patch] device vtable patched: Present=%d Reset=%d CreateTexture=%d CreateVB=%d CreateIB=%d",
        DEV_Present, DEV_Reset, DEV_CreateTexture, DEV_CreateVertexBuffer, DEV_CreateIndexBuffer);

    // A null original is a patch that silently did nothing, and calling through it would
    // crash on the first frame. Say so loudly rather than discovering it in a minidump.
    if (!g_origPresent || !g_origReset || !g_origCreateTex || !g_origCreateVB || !g_origCreateIB)
        Log("[patch] *** ONE OR MORE ORIGINALS ARE NULL - a patch failed, expect a crash ***");
}

// ---------------------------------------------------------------- IDirect3D9::CreateDevice

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                      D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static PFN_CreateDevice g_origCreateDevice = nullptr;

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type,
                                                   HWND focus, DWORD behavior,
                                                   D3DPRESENT_PARAMETERS* pp,
                                                   IDirect3DDevice9** out)
{
    // `self` is the answer to "which of the two IDirect3D9 instances is the real one".
    Log("");
    Log("*** CreateDevice on IDirect3D9* %p   adapter=%u DeviceType=%d BehaviorFlags=0x%08lX focus=%p",
        (void*)self, adapter, (int)type, (unsigned long)behavior, (void*)focus);
    LogPresentParams("requested", pp);

    HRESULT hr = g_origCreateDevice(self, adapter, type, focus, behavior, pp, out);
    Log("*** CreateDevice returned hr=0x%08lX  device=%p", (unsigned long)hr, out ? (void*)*out : nullptr);

    // The engine may have had its request adjusted. Log what came back as well as what was asked.
    if (SUCCEEDED(hr)) {
        LogPresentParams("granted", pp);
        if (out && *out) PatchDeviceOnce(*out);
    }
    return hr;
}

// ---------------------------------------------------------------- the real library
//
// Loaded lazily on the first export call, NOT from DllMain: LoadLibrary under the loader
// lock is a documented deadlock, and nothing can call an export before attach completes.
//
// Resolved out of the system directory explicitly. A bare LoadLibraryW(L"d3d9.dll") would
// search the application directory first and find THIS file, loading us into ourselves.

static void EnsureReal()
{
    if (g_real) return;

    EnterCriticalSection(&g_lock);
    if (!g_real) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            wcscat_s(path, L"\\d3d9.dll");
            g_real = LoadLibraryW(path);
            if (g_real) {
                char narrow[MAX_PATH * 2];
                WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
                Log("[real] loaded %s at %p", narrow, (void*)g_real);
            } else {
                Log("[real] FAILED to load the system d3d9.dll, GetLastError=%lu", GetLastError());
            }
        }
    }
    LeaveCriticalSection(&g_lock);
}

static FARPROC Real(const char* name)
{
    EnsureReal();
    if (!g_real) return nullptr;
    FARPROC p = GetProcAddress(g_real, name);
    if (!p) Log("[real] MISSING export %s", name);
    return p;
}

// ---------------------------------------------------------------- forwarded exports

typedef IDirect3D9* (WINAPI *pfn_Create9)(UINT);
typedef HRESULT     (WINAPI *pfn_Create9Ex)(UINT, IDirect3D9Ex**);
typedef int         (WINAPI *pfn_BeginEvent)(D3DCOLOR, LPCWSTR);
typedef int         (WINAPI *pfn_EndEvent)(void);
typedef DWORD       (WINAPI *pfn_GetStatus)(void);
typedef BOOL        (WINAPI *pfn_QueryRepeatFrame)(void);
typedef void        (WINAPI *pfn_SetMarker)(D3DCOLOR, LPCWSTR);
typedef void        (WINAPI *pfn_SetOptions)(DWORD);
typedef void        (WINAPI *pfn_SetRegion)(D3DCOLOR, LPCWSTR);
typedef void        (WINAPI *pfn_DebugSetMute)(void);

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
    pfn_Create9 fn = (pfn_Create9)Real("Direct3DCreate9");
    IDirect3D9* d3d = fn ? fn(SDKVersion) : nullptr;

    long call = InterlockedIncrement(&g_createCalls);
    Log("*** Direct3DCreate9 CALLED (call #%ld) SDKVersion=%u -> IDirect3D9* %p",
        call, SDKVersion, (void*)d3d);

    // Patched once. All IDirect3D9 instances share a vtable, so this covers both the
    // throwaway and the real one - and the `self` argument in the hook reports which is
    // which, without any pointer-keyed bookkeeping that the address reuse would defeat.
    if (d3d && !g_d3d9Patched) {
        g_d3d9Patched = true;
        g_origCreateDevice = (PFN_CreateDevice)PatchVTable(d3d, D3D9_CreateDevice,
                                                           (void*)&Hook_CreateDevice);
        Log("[patch] IDirect3D9::CreateDevice (slot %d) patched, original=%p",
            D3D9_CreateDevice, (void*)g_origCreateDevice);
        if (!g_origCreateDevice)
            Log("[patch] *** ORIGINAL IS NULL - the patch failed, expect a crash ***");
    }
    return d3d;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
    pfn_Create9Ex fn = (pfn_Create9Ex)Real("Direct3DCreate9Ex");
    HRESULT hr = fn ? fn(SDKVersion, ppD3D) : E_NOTIMPL;
    // Worth knowing if it ever fires: the Singularity mod had to UPGRADE a plain device to
    // Ex for shared-surface interop. A game asking for Ex itself would skip that work.
    Log("*** Direct3DCreate9Ex called SDKVersion=%u -> hr=0x%08lX", SDKVersion, (unsigned long)hr);
    return hr;
}

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name)
{
    pfn_BeginEvent fn = (pfn_BeginEvent)Real("D3DPERF_BeginEvent");
    return fn ? fn(col, name) : 0;
}

extern "C" int WINAPI D3DPERF_EndEvent(void)
{
    pfn_EndEvent fn = (pfn_EndEvent)Real("D3DPERF_EndEvent");
    return fn ? fn() : 0;
}

extern "C" DWORD WINAPI D3DPERF_GetStatus(void)
{
    pfn_GetStatus fn = (pfn_GetStatus)Real("D3DPERF_GetStatus");
    return fn ? fn() : 0;
}

extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
    pfn_QueryRepeatFrame fn = (pfn_QueryRepeatFrame)Real("D3DPERF_QueryRepeatFrame");
    return fn ? fn() : FALSE;
}

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name)
{
    pfn_SetMarker fn = (pfn_SetMarker)Real("D3DPERF_SetMarker");
    if (fn) fn(col, name);
}

extern "C" void WINAPI D3DPERF_SetOptions(DWORD opts)
{
    pfn_SetOptions fn = (pfn_SetOptions)Real("D3DPERF_SetOptions");
    if (fn) fn(opts);
}

extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name)
{
    pfn_SetRegion fn = (pfn_SetRegion)Real("D3DPERF_SetRegion");
    if (fn) fn(col, name);
}

extern "C" void WINAPI DebugSetMute(void)
{
    pfn_DebugSetMute fn = (pfn_DebugSetMute)Real("DebugSetMute");
    if (fn) fn();
}

// ---------------------------------------------------------------- attach / detach

static void LogHeader()
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t exeW[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exeW, MAX_PATH);
    char exe[MAX_PATH * 2] = "";
    WideCharToMultiByte(CP_UTF8, 0, exeW, -1, exe, sizeof(exe), nullptr, nullptr);

    Log("=== Mirror's Edge VR rung 1 attached %04u-%02u-%02u %02u:%02u:%02u - run starts here ===",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Log("[host] %s", exe);
    Log("[host] pid %lu, this DLL is %u-bit",
        GetCurrentProcessId(), (unsigned)(sizeof(void*) * 8));
    Log("[note] Observation only. Nothing is altered; the game must behave exactly as");
    Log("[note] it does without this file. Any visible difference is itself a finding.");
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
        BuildLogPath();
        ArchivePreviousLog();
        LogHeader();
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("");
        Log("=== SUMMARY ===");
        Log("Direct3DCreate9 calls : %ld", g_createCalls);
        Log("frames presented      : %ld", g_frames);
        Log("pool DEFAULT          : %ld   <- these die on a device Reset", g_poolDefault);
        Log("pool MANAGED          : %ld   <- the wrapper would have to translate these", g_poolManaged);
        Log("pool SYSTEMMEM        : %ld", g_poolSystemMem);
        Log("pool other            : %ld", g_poolScratch);
        Log("=== detached ===");
    }
    return TRUE;
}
