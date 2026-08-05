// Mirror's Edge VR - rung 0
//
// A d3d9.dll proxy that forwards every export to the real system library and changes
// nothing. It renders nothing, hooks nothing, and must be invisible to the game.
//
// It exists to settle exactly one question: DOES MIRROR'S EDGE ACTUALLY CALL
// Direct3DCreate9? The whole architecture inherited from the Singularity mod is built on
// a D3D9 device being there to wrap, and the game delay-loads d3d10.dll and dxgi.dll as
// well - it has a DX10 path. TdEngine.ini says AllowD3D10=False, but an ini value and a
// live call are different claims, and this project's ancestor lost repeated runs to
// instruments that agreed with a premise without testing it.
//
// ---- WHAT WOULD FOOL THIS TEST ----
//
// An EMPTY log directory is ambiguous and must not be read as "the game does not use
// D3D9". It has two very different causes:
//
//   1. the proxy never loaded  - wrong folder, wrong architecture, blocked by the loader
//   2. the proxy loaded, and the game genuinely never called Direct3DCreate9
//
// The `=== attached ===` header separates them: it is written from DllMain, before the
// game has had any chance to call anything. So:
//
//   no log file at all        -> case 1. The proxy is not being loaded. Nothing is proven
//                                about the renderer.
//   header but no CREATE line -> case 2. That IS the interesting negative result.
//   header and a CREATE line  -> D3D9 confirmed live, not merely configured.
//
// Rung 0 deliberately does NOT wrap IDirect3D9 to log CreateDevice parameters, tempting
// though that is - backbuffer format and size are wanted eventually, and a wrong format
// assumption cost the Singularity project a D3DERR_INVALIDCALL. But wrapping introduces a
// COM object that can crash the game, and a crash here would confound "does it call D3D9"
// with "is our wrapper correct". Those are separate rungs and get separate runs.

#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>

// ---------------------------------------------------------------- state

static HMODULE          g_real = nullptr;      // the genuine system d3d9.dll
static CRITICAL_SECTION g_lock;                // guards lazy load and log writes
static bool             g_lockReady = false;
static wchar_t          g_logPath[MAX_PATH]    = L"";
static long             g_createCalls          = 0;

// ---------------------------------------------------------------- logging
//
// Every run is kept. The Singularity project lost two of three tests in a single session to
// a log that truncated on attach: each measurement there is an A/B across launches, so
// destroying the previous run destroys the thing being compared against - silently, because
// the file still looks complete afterwards. Archiving costs nothing and removes the whole
// class of mistake.

static void BuildLogPath()
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    wchar_t dir[MAX_PATH];
    _snwprintf_s(dir, _TRUNCATE, L"%s\\MirrorsEdgeVR", base);
    CreateDirectoryW(dir, nullptr);
    _snwprintf_s(g_logPath, _TRUNCATE, L"%s\\rung0.log", dir);
}

// Rename an existing log out of the way, stamped with when that run STARTED. The stamp comes
// from the file's own creation time rather than from now, so the archived name describes the
// run it contains rather than the moment it was displaced.
static void ArchivePreviousLog()
{
    if (!g_logPath[0]) return;

    HANDLE h = CreateFileW(g_logPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;   // no previous run, nothing to preserve

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

    _snwprintf_s(archived, _TRUNCATE, L"%s\\rung0_%04u-%02u-%02u_%02u-%02u-%02u.log",
                 dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    MoveFileW(g_logPath, archived);
}

// _Printf_format_string_ is not decoration. build.ps1 runs /analyze and fails the build on
// C6067 and friends, because a format string that disagrees with its arguments is the defect
// that has actually cost this codebase time - it presented as an unexplained startup hang.
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

// ---------------------------------------------------------------- the real library
//
// Loaded lazily on the first export call, NOT from DllMain. Calling LoadLibrary under the
// loader lock is a documented way to deadlock, and there is no reason to risk it: nothing
// can call an export before the DLL has finished attaching.
//
// Resolved out of the system directory explicitly. A bare LoadLibraryW(L"d3d9.dll") would
// search the application directory first and find THIS file, which loads us into ourselves.

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
//
// Signatures follow d3d9.h. The D3DPERF_* family and DebugSetMute are instrumentation entry
// points; the game imports only Direct3DCreate9, but a proxy that omits the others would
// break any tool that expects a complete d3d9.dll.

typedef IDirect3D9*   (WINAPI *pfn_Create9)(UINT);
typedef HRESULT       (WINAPI *pfn_Create9Ex)(UINT, IDirect3D9Ex**);
typedef int           (WINAPI *pfn_BeginEvent)(D3DCOLOR, LPCWSTR);
typedef int           (WINAPI *pfn_EndEvent)(void);
typedef DWORD         (WINAPI *pfn_GetStatus)(void);
typedef BOOL          (WINAPI *pfn_QueryRepeatFrame)(void);
typedef void          (WINAPI *pfn_SetMarker)(D3DCOLOR, LPCWSTR);
typedef void          (WINAPI *pfn_SetOptions)(DWORD);
typedef void          (WINAPI *pfn_SetRegion)(D3DCOLOR, LPCWSTR);
typedef void          (WINAPI *pfn_DebugSetMute)(void);

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
    pfn_Create9 fn = (pfn_Create9)Real("Direct3DCreate9");
    IDirect3D9* d3d = fn ? fn(SDKVersion) : nullptr;

    long call = InterlockedIncrement(&g_createCalls);
    Log("*** Direct3DCreate9 CALLED (call #%ld) SDKVersion=%u -> IDirect3D9* %p",
        call, SDKVersion, (void*)d3d);
    if (call == 1) {
        Log("    D3D9 CONFIRMED LIVE - the renderer is Direct3D 9, not the delay-loaded");
        Log("    D3D10 path. The proxy architecture applies.");
    }
    return d3d;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
    pfn_Create9Ex fn = (pfn_Create9Ex)Real("Direct3DCreate9Ex");
    HRESULT hr = fn ? fn(SDKVersion, ppD3D) : E_NOTIMPL;
    // Worth knowing if it ever fires: the Singularity mod had to UPGRADE a plain device to
    // Ex for shared-surface interop. A game that asks for Ex itself skips that work.
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

    Log("=== Mirror's Edge VR rung 0 attached %04u-%02u-%02u %02u:%02u:%02u - run starts here ===",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Log("[host] %s", exe);
    Log("[host] pid %lu, this DLL is %u-bit",
        GetCurrentProcessId(), (unsigned)(sizeof(void*) * 8));
    Log("[note] This header proves the proxy LOADED. If no 'Direct3DCreate9 CALLED' line");
    Log("[note] follows, the game genuinely did not create a D3D9 device this run.");
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
        // The count at exit is the summary. Zero here with a header present is the
        // meaningful negative result described at the top of this file.
        Log("=== detached, Direct3DCreate9 was called %ld time(s) ===", g_createCalls);
    }
    return TRUE;
}
