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

// ---- rung 2: does OpenXR work inside this 32-bit process? ----
//
// Creates a session, a swapchain, and submits a QUAD layer filled with a solid colour that
// CYCLES. Nothing of the game reaches the headset yet, and no engine behaviour is altered -
// the game is still fullscreen and untouched.
//
// The colour cycles on purpose. A static quad cannot distinguish "we are submitting frames
// continuously" from "we submitted one frame and the compositor is still showing it", and
// the difference is the entire point of the rung. A cycling colour cannot false-pass that
// way: if it is moving, the frame loop is live.
//
// Separated from putting the real frame on that quad (rung 3) because "OpenXR session works"
// and "our frame grab is correct" are unrelated failure modes. A black headset that could be
// either one is a wasted run.
//
// If no headset or runtime is present, initialisation fails once, says so, and the proxy
// keeps forwarding normally. The game must never fail to run because VR is unavailable.

#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <vector>
#include <psapi.h>
// Rung 10. The build script reads this include to decide whether to compile MinHook's sources,
// so it is the switch as well as the declaration - see the note in build.ps1.
#include "MinHook.h"

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
    DEV_SetRenderTarget        = 37,
    DEV_Clear                  = 43,
    DEV_CreateQuery            = 118,
    DEV_DrawPrimitive          = 81,
    DEV_DrawIndexedPrimitive  = 82,
    // The user-pointer draws. UE3's Canvas issues 2D geometry through these rather than through
    // a vertex buffer, so anything the HUD or a menu draws arrives here and NOWHERE ELSE.
    DEV_DrawPrimitiveUP        = 83,
    DEV_DrawIndexedPrimitiveUP = 84,
    DEV_SetVertexShaderConstantF = 94,
};

// ---------------------------------------------------------------- version
//
// The single source of truth for the build's identity. build.ps1 PARSES THIS LINE to name
// the release zip, rather than carrying its own copy that could disagree with the DLL's -
// the same reasoning that makes it read `#include <openxr...>` instead of a flag.
//
// It exists for one reason: mevr.log is the whole diagnostic channel, and a pasted log that
// cannot say which build produced it turns every bug report into a round trip. Logged in the
// header, above everything, so it survives truncation from either end.
#define MEVR_VERSION "0.1.0-alpha"

// ---------------------------------------------------------------- state

static HMODULE          g_real       = nullptr;
static HMODULE          g_selfModule = nullptr;   // this DLL, for finding mevr.ini beside it

// ---- development affordances, on by default, `Debug=off` in mevr.ini to remove them ----
//
// Off means: no on-screen overlay, and no hotkey does anything - EXCEPT the three that are not
// diagnostics at all and whose absence would leave a player stuck.
//
//   PAGE UP    recentre. A headset put on crooked needs this, and needs it MORE with debugging
//              off, because nothing else can fix the seating.
//   PAUSE hold close the game cleanly, so the engine writes its save.
//   F6         rescan for the view matrix. The startup sequence gives up loudly after ten
//              attempts and tells you to press F6; taking the key away would leave that message
//              pointing at nothing, and stereo unrecoverable for the run.
//
// ⚠️ The scan's own countdown and commit live inside the F6 handler and must keep running
// whatever this is set to - they are the mechanism, not the key.
static bool             g_debug      = true;
static CRITICAL_SECTION g_lock;
static bool             g_lockReady  = false;
static wchar_t          g_logPath[MAX_PATH] = L"";
static long             g_createCalls = 0;

static bool  g_d3d9Patched   = false;   // IDirect3D9 vtable patched (shared by all instances)
static bool  g_devicePatched = false;   // IDirect3DDevice9 vtable patched
// Declared up here rather than beside the rest of the rung 8a state, because the frame grab -
// which sits far above that code - has to know which kind of device it is grabbing from.
static bool  g_devIsEx       = false;   // the device really is D3D9Ex
// Rung 10, defined below the call sites in the XR setup and the frame loop.
static void  XrInitActions();
static bool  XrSyncInput();
static void  InstallXInputHook();
void         RecenterSixDof();
static long  g_frames        = 0;
static bool  g_describedBackbuffer = false;

static LONG  g_poolDefault = 0, g_poolManaged = 0, g_poolSystemMem = 0, g_poolScratch = 0;

// ---- OpenXR / D3D11 state ----
static ID3D11Device*        g_dev11    = nullptr;
static ID3D11DeviceContext* g_ctx11    = nullptr;
static XrInstance           g_xrInstance = XR_NULL_HANDLE;
static XrSystemId           g_xrSystem   = XR_NULL_SYSTEM_ID;
static XrSession            g_xrSession  = XR_NULL_HANDLE;
static XrSpace              g_xrSpace    = XR_NULL_HANDLE;
static XrSpace              g_viewSpace  = XR_NULL_HANDLE;   // head pose, located against LOCAL
static XrSwapchain          g_swapchain  = XR_NULL_HANDLE;
static ID3D11Texture2D**    g_scImages   = nullptr;
static uint32_t             g_scImageCount = 0;
static uint32_t             g_scW = 0, g_scH = 0;
static int64_t              g_scFormat = 0;      // the format we ASKED for; the texture may be typeless
static bool                 g_rtvFailLogged = false;
// Set whenever TestCooperativeLevel reports anything but D3D_OK, or a Reset fails. While it is
// true the mod issues NO D3D calls at all - see the guard at the top of Hook_Present.
static bool                 g_deviceLost = false;

// ---- rung 3: the frame grab (CPU path) ----
static IDirect3DSurface9*   g_sysSurf  = nullptr;   // SYSTEMMEM copy of the backbuffer
static ID3D11Texture2D*     g_upload   = nullptr;   // DYNAMIC D3D11 texture the frame lands in
static UINT                 g_capW = 0, g_capH = 0;
static D3DFORMAT            g_capFmt = D3DFMT_UNKNOWN;
static bool                 g_haveFrame = false;    // a real frame is sitting in g_upload
static bool                 g_captureFailLogged = false;
static double               g_capMsTotal = 0.0;
static long                 g_capSamples = 0;
static double               g_qpcFreq = 0.0;

static double NowMs()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / g_qpcFreq;
}
// Seconds since the first stamped line, for the t= field on [swan]/[move]/[mark] lines.
// Frame numbers alone cannot measure a stall - a frozen game still numbers its frames one
// per Present - so duration claims need wall clock, and one shared epoch keeps every stamp
// subtractable from every other.
static double g_logSecs0 = 0.0;
static double LogSecs()
{
    const double n = NowMs();
    if (g_logSecs0 == 0.0) g_logSecs0 = n;
    return (n - g_logSecs0) / 1000.0;
}
static uint32_t             g_recEyeW = 0, g_recEyeH = 0;
static bool                 g_xrReady   = false;   // session + space exist
static bool                 g_xrRunning = false;   // xrBeginSession succeeded
static bool                 g_xrTried   = false;   // init attempted; never retried
static XrSessionState       g_xrState   = XR_SESSION_STATE_UNKNOWN;
static long                 g_xrFrames  = 0;

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

// Rename an existing log out of the way, stamped with when that run STARTED.
//
// ⚠️ The stamp is parsed out of the file's OWN FIRST LINE, not taken from its creation time.
// This is not a stylistic choice and the obvious version is broken: NTFS **file system
// tunneling** reuses the creation timestamp when a file is recreated with the same name
// shortly after the previous one was moved away. So every archive got the SAME name, the
// second MoveFileW failed because the destination already existed, and runs silently
// concatenated into one file instead of rotating.
//
// That was measured here: mevr.log carried four run headers, and its creation time matched
// an archive already sitting beside it. A stale run was read as if it were the current one.
// The header line is written by us, once, and cannot be rewritten by the filesystem.
static void ArchivePreviousLog()
{
    if (!g_logPath[0]) return;

    HANDLE h = CreateFileW(g_logPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;   // no previous run, nothing to preserve

    char head[256] = {};
    DWORD got = 0;
    if (!ReadFile(h, head, sizeof(head) - 1, &got, nullptr)) got = 0;
    head[got] = 0;   // a failed read leaves an empty string, which falls through to the
                     // creation-time path below rather than parsing uninitialised bytes

    // Fall back to the file's creation time only if the header cannot be parsed - an
    // unreadable stamp should still produce a uniquely named archive rather than none.
    SYSTEMTIME st{};
    unsigned y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    const char* at = strstr(head, "attached ");
    if (at && sscanf_s(at + 9, "%u-%u-%u %u:%u:%u", &y, &mo, &d, &hh, &mm, &ss) == 6) {
        st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d;
        st.wHour = (WORD)hh; st.wMinute = (WORD)mm; st.wSecond = (WORD)ss;
    } else {
        FILETIME created{}, local{};
        if (GetFileTime(h, &created, nullptr, nullptr)) {
            FileTimeToLocalFileTime(&created, &local);
            FileTimeToSystemTime(&local, &st);
        }
    }
    CloseHandle(h);

    wchar_t dir[MAX_PATH] = L"";
    wcscpy_s(dir, g_logPath);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;

    // Even with a correct stamp, two runs can start inside the same second. A collision must
    // never silently fall through to "append to the live log" again.
    wchar_t archived[MAX_PATH] = L"";
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (attempt == 0)
            _snwprintf_s(archived, _TRUNCATE, L"%s\\mevr_%04u-%02u-%02u_%02u-%02u-%02u.log",
                         dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        else
            _snwprintf_s(archived, _TRUNCATE, L"%s\\mevr_%04u-%02u-%02u_%02u-%02u-%02u_%d.log",
                         dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         attempt + 1);
        if (GetFileAttributesW(archived) == INVALID_FILE_ATTRIBUTES) break;
    }
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

// ================================================================ OpenXR (rung 2)

static bool InitXR()
{
    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

    // VirtualDesktopXR is OpenXR 1.0 ONLY and rejects a 1.1 instance with -4. Inherited from
    // the Singularity project, where VDXR is also the only usable 32-bit runtime: Meta's own
    // crashes in xrCreateSession, and SteamVR ships no 32-bit runtime at all. Try 1.0 first
    // so the working case is not gated on the failing one.
    XrVersion versions[] = { XR_MAKE_VERSION(1, 0, 34), XR_CURRENT_API_VERSION };
    XrResult r = XR_ERROR_RUNTIME_FAILURE;
    for (XrVersion v : versions) {
        XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
        strcpy_s(ici.applicationInfo.applicationName, "MirrorsEdgeVR");
        ici.applicationInfo.apiVersion = v;
        ici.enabledExtensionCount = 1;
        ici.enabledExtensionNames  = exts;
        r = xrCreateInstance(&ici, &g_xrInstance);
        Log("[xr] xrCreateInstance apiVersion=%llu -> %d", (unsigned long long)v, (int)r);
        if (XR_SUCCEEDED(r)) break;
    }
    if (XR_FAILED(r)) {
        Log("[xr] no OpenXR instance. Is the headset connected and Virtual Desktop streaming?");
        return false;
    }

    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(g_xrInstance, &ip)))
        Log("[xr] runtime: %s", ip.runtimeName);

    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(g_xrInstance, &sgi, &g_xrSystem))) { Log("[xr] no HMD"); return false; }

    // What the runtime wants per eye. Not used to drive anything yet, but it is the number
    // the finished mod inherits its resolution from, and it already includes whatever
    // render-scale is set in the platform's own slider.
    {
        uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(g_xrInstance, g_xrSystem,
                                          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                          0, &viewCount, nullptr);
        if (viewCount >= 1 && viewCount <= 4) {
            XrViewConfigurationView vcv[4]{};
            for (uint32_t i = 0; i < viewCount; ++i) vcv[i] = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
            if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(
                    g_xrInstance, g_xrSystem, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                    viewCount, &viewCount, vcv))) {
                g_recEyeW = vcv[0].recommendedImageRectWidth;
                g_recEyeH = vcv[0].recommendedImageRectHeight;
                Log("[xr] headset wants %ux%u per eye (max %ux%u)", g_recEyeW, g_recEyeH,
                    vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight);
                Log("[xr] => side-by-side stereo would want -ResX=%u -ResY=%u",
                    g_recEyeW * 2, g_recEyeH);
            }
        }
    }

    // The D3D11 device MUST be on the adapter the runtime names, not simply the default one.
    PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;
    xrGetInstanceProcAddr(g_xrInstance, "xrGetD3D11GraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&pfn);
    if (!pfn) { Log("[xr] xrGetD3D11GraphicsRequirementsKHR unavailable"); return false; }
    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    if (XR_FAILED(pfn(g_xrInstance, g_xrSystem, &req))) { Log("[xr] graphics requirements failed"); return false; }

    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) { Log("[xr] no DXGI factory"); return false; }
    IDXGIAdapter1* ad = nullptr; IDXGIAdapter1* chosen = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; ad->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == req.adapterLuid.LowPart &&
            d.AdapterLuid.HighPart == req.adapterLuid.HighPart) { chosen = ad; break; }
        ad->Release();
    }
    fac->Release();
    if (!chosen) { Log("[xr] no adapter matched the runtime's LUID"); return false; }

    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0 }, got{};
    HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 1,
                                   D3D11_SDK_VERSION, &g_dev11, &got, &g_ctx11);
    chosen->Release();
    if (FAILED(hr)) { Log("[xr] D3D11CreateDevice failed hr=0x%08lX", (unsigned long)hr); return false; }

    XrGraphicsBindingD3D11KHR bind{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    bind.device = g_dev11;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &bind; sci.systemId = g_xrSystem;
    XrResult sr = xrCreateSession(g_xrInstance, &sci, &g_xrSession);
    if (XR_FAILED(sr)) { Log("[xr] xrCreateSession failed -> %d", (int)sr); return false; }

    XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateReferenceSpace(g_xrSession, &rs, &g_xrSpace))) {
        Log("[xr] xrCreateReferenceSpace failed"); return false;
    }

    // A VIEW space located against LOCAL gives the head pose directly. Failing here disables
    // head tracking and nothing else, so it is not treated as fatal.
    XrReferenceSpaceCreateInfo vs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    vs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vs.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateReferenceSpace(g_xrSession, &vs, &g_viewSpace))) {
        g_viewSpace = XR_NULL_HANDLE;
        Log("[xr] VIEW space failed - head tracking unavailable, rendering unaffected");
    }

    XrInitActions();

    Log("*** [xr] OpenXR session created. 32-bit OpenXR works in this process.");
    g_xrReady = true;
    return true;
}

// ================================================================ rung 10: motion controllers
//
// As a gamepad, deliberately. The controllers drive the game through the pad interface it
// already supports, rather than through anything that has to be discovered.
//
// ---- why a synthesised XInput pad and not written input properties ----
//
// The game's import table names DINPUT8.dll and XINPUT1_3.dll, so a 360 pad is a first-class
// input path in this engine already. Everything that path reaches - menus, jump, crouch,
// interact, the run button, movement, look - is bound and tested by the developers.
//
// The alternative was writing PlayerInput's axis properties directly, which this project has the
// object model to do. It would move the player and nothing else: the axes are readable but the
// BUTTONS are bound to exec functions, and reaching those means driving the script VM. Every
// button would have been a separate piece of reverse engineering, and menus would still need the
// keyboard. Synthesising the pad gets the entire surface for one hook.
//
// ---- what this deliberately is not ----
//
// No hands, no pointing, no positional use of the controllers at all. The sticks are sticks and
// the buttons are buttons. That keeps the whole rung inside a surface the game already
// understands, and leaves anything genuinely spatial for later, when it can be judged on its own.

// Declared locally rather than by including <Xinput.h>. The structs are stable across every
// XInput version and this way the build gains no header search path and no import - the DLL is
// reached through GetProcAddress, so nothing here makes us depend on it being present.
struct MEVR_XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct MEVR_XINPUT_STATE { DWORD dwPacketNumber; MEVR_XINPUT_GAMEPAD Gamepad; };
struct MEVR_XINPUT_VIBRATION { WORD wLeftMotorSpeed, wRightMotorSpeed; };
struct MEVR_XINPUT_CAPABILITIES {
    BYTE Type, SubType; WORD Flags;
    MEVR_XINPUT_GAMEPAD Gamepad; MEVR_XINPUT_VIBRATION Vibration;
};

enum : WORD {
    MEVR_PAD_DPAD_UP = 0x0001, MEVR_PAD_DPAD_DOWN = 0x0002,
    MEVR_PAD_DPAD_LEFT = 0x0004, MEVR_PAD_DPAD_RIGHT = 0x0008,
    MEVR_PAD_START = 0x0010, MEVR_PAD_BACK = 0x0020,
    MEVR_PAD_LTHUMB = 0x0040, MEVR_PAD_RTHUMB = 0x0080,
    MEVR_PAD_LSHOULDER = 0x0100, MEVR_PAD_RSHOULDER = 0x0200,
    MEVR_PAD_A = 0x1000, MEVR_PAD_B = 0x2000, MEVR_PAD_X = 0x4000, MEVR_PAD_Y = 0x8000
};

static XrActionSet g_actionSet = XR_NULL_HANDLE;
static XrAction    g_aMove = XR_NULL_HANDLE, g_aLook = XR_NULL_HANDLE;
static XrAction    g_aA = XR_NULL_HANDLE, g_aB = XR_NULL_HANDLE;
static XrAction    g_aX = XR_NULL_HANDLE, g_aY = XR_NULL_HANDLE;
static XrAction    g_aLTrig = XR_NULL_HANDLE, g_aRTrig = XR_NULL_HANDLE;
static XrAction    g_aLGrip = XR_NULL_HANDLE, g_aRGrip = XR_NULL_HANDLE;
static XrAction    g_aMenu = XR_NULL_HANDLE;
static XrAction    g_aLClick = XR_NULL_HANDLE, g_aRClick = XR_NULL_HANDLE;
// Phase 1 motion hands. These actions must exist before the action set is attached even while
// the feature is disabled: OpenXR does not permit extending an attached action set later.
static XrAction    g_aLGripPose = XR_NULL_HANDLE, g_aRGripPose = XR_NULL_HANDLE;
static XrAction    g_aLAimPose  = XR_NULL_HANDLE, g_aRAimPose  = XR_NULL_HANDLE;
static XrSpace     g_sLGripPose = XR_NULL_HANDLE, g_sRGripPose = XR_NULL_HANDLE;
static XrSpace     g_sLAimPose  = XR_NULL_HANDLE, g_sRAimPose  = XR_NULL_HANDLE;
static bool        g_actionsReady = false;
static bool        g_padEnabled = true;            // NUMPAD9 toggles
static bool        g_motionHands = false;           // incomplete Phase 1 path, opt-in only
static bool        g_motionHandsDebug = false;      // bounded pose-state and position reports
static MEVR_XINPUT_STATE g_pad = {};
static CRITICAL_SECTION  g_padLock;
static bool        g_padLockReady = false;
static long        g_padPolls = 0;

// ---- what the sticks actually carried, per report window ----
//
// Added after a report the log could not answer: jump, crouch, punch and turn all working,
// forward and back doing nothing. All of those are built in XrSyncInput from the same four
// fields on the same frame, so nothing that only records THAT the pad was synthesised can tell
// a dead left stick from a game that received it and refused to move.
static long  g_padSyncs = 0, g_padMoveLive = 0, g_padLookLive = 0;
static float g_padLo[4] = { 0, 0, 0, 0 };      // move x, move y, look x, look y
static float g_padHi[4] = { 0, 0, 0, 0 };
static WORD  g_padButtonsSeen = 0;
static BYTE  g_padTrigPeak[2] = { 0, 0 };
static long  g_padPollsSeen = 0;               // g_padPolls at the last report

struct MEVR_Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
struct RenderedHeadFrame {
    MEVR_Vec3 position;
    MEVR_Vec3 forward;
    MEVR_Vec3 right;
    MEVR_Vec3 up;
};
static bool GetRenderedHeadFrame(RenderedHeadFrame* out);
extern float g_worldScale;
static volatile LONG g_motionRecenterSerial = 0;

struct MotionPoseSample {
    bool active = false;
    XrSpaceLocationFlags flags = 0;
    XrPosef pose{};                             // relative to VIEW (the tracked head)
    MEVR_Vec3 cameraLocal;                      // UE3 F/R/U, in Unreal units
    MEVR_Vec3 worldPosition;                    // exact space expected by world-space limb IK
    XrQuaternionf worldOrientation{};           // UE3 X-forward, Y-right, Z-up basis
    float sourceQuatNorm = 0.0f;
    float worldQuatNorm = 0.0f;
    bool worldValid = false;
    long rejectedConversions = 0;
    int reportedStatus = -1;
};
static MotionPoseSample g_poseLGrip, g_poseRGrip, g_poseLAim, g_poseRAim;
// Located once by the existing stereo frame path before controller poses are sampled.
static XrView g_views[2]{};
static bool g_viewsValid = false;

// Present owns OpenXR and publishes one immutable position sample for the next game tick.
// Update1pArms can run on the game thread, so a sequence lock prevents it from observing a
// half-written 64-bit flags field or a target assembled from two different XR frames.
struct P13HandPoseSnapshot {
    bool active = false;
    XrSpaceLocationFlags flags = 0;
    MEVR_Vec3 cameraLocal;
    // The OpenXR grip orientation relative to VIEW. Keep this alongside cameraLocal so the
    // game-thread arm hook can compose both against the freshest game/world anchor instead of
    // inheriting the world transform captured at the end of the preceding rendered frame.
    XrQuaternionf viewOrientation{};
    MEVR_Vec3 worldPosition;
    XrQuaternionf worldOrientation{};
    bool worldValid = false;
};
struct P13PoseSnapshot {
    P13HandPoseSnapshot left;
    P13HandPoseSnapshot right;
    RenderedHeadFrame sampledHead{};
    MEVR_Vec3 sampledPawnLocation{};
    bool sampledHeadValid = false;
    bool sampledPawnValid = false;
    long presentFrame = 0;
};
static P13PoseSnapshot g_p13PublishedPose{};
static volatile LONG g_p13PoseSequence = 0;
static RenderedHeadFrame g_motionPoseHead{};
static bool g_motionPoseHeadValid = false;

// These are defined with the UE3 object-model code below. The early pose publisher only needs
// a guarded FVector read, so forward declarations keep the ownership boundary explicit.
extern uintptr_t g_playerPawn;
extern int g_offActorLocation;
static bool SafeRead(uintptr_t addr, void* out, size_t n);
static bool FiniteVec(const MEVR_Vec3& v);

// Live debug calibration in degrees, [left/right][pitch/yaw/roll]. Preserve the values measured
// in the headset for the wrists. Selection order is Right P/Y/R, then Left P/Y/R.
static int g_wristCalibrationDeg[2][3] = { { -30, 0, 0 }, { 150, 0, 0 } };
// Trim added to each ForeArmRoll helper's twist, in degrees about the forearm axis. P1.4d
// drives the helper as forearm-swing + measured hand twist + this trim, and run 7 measured the
// authored helper twist matching the hand's twist at neutral on both sides, so zero is the
// correct default and the overlay slots exist only for taste.
static int g_forearmRollCalibrationDeg[2] = { 0, 0 };
static int g_handTuneSelected = 0;

static void PublishP13PoseSnapshot()
{
    // Do the guarded process read before taking the sequence lock. The reader deliberately only
    // spins three times, so a comparatively slow ReadProcessMemory must never keep it odd.
    MEVR_Vec3 sampledPawn{};
    const bool sampledPawnValid =
        g_playerPawn && g_offActorLocation >= 0 &&
        SafeRead(g_playerPawn + g_offActorLocation, &sampledPawn, sizeof(sampledPawn)) &&
        FiniteVec(sampledPawn);
    InterlockedIncrement(&g_p13PoseSequence);      // odd: writer owns the snapshot
    MemoryBarrier();
    g_p13PublishedPose.left.active = g_poseLGrip.active;
    g_p13PublishedPose.left.flags = g_poseLGrip.flags;
    g_p13PublishedPose.left.cameraLocal = g_poseLGrip.cameraLocal;
    g_p13PublishedPose.left.viewOrientation = g_poseLGrip.pose.orientation;
    g_p13PublishedPose.left.worldPosition = g_poseLGrip.worldPosition;
    g_p13PublishedPose.left.worldOrientation = g_poseLGrip.worldOrientation;
    g_p13PublishedPose.left.worldValid = g_poseLGrip.worldValid;
    g_p13PublishedPose.right.active = g_poseRGrip.active;
    g_p13PublishedPose.right.flags = g_poseRGrip.flags;
    g_p13PublishedPose.right.cameraLocal = g_poseRGrip.cameraLocal;
    g_p13PublishedPose.right.viewOrientation = g_poseRGrip.pose.orientation;
    g_p13PublishedPose.right.worldPosition = g_poseRGrip.worldPosition;
    g_p13PublishedPose.right.worldOrientation = g_poseRGrip.worldOrientation;
    g_p13PublishedPose.right.worldValid = g_poseRGrip.worldValid;
    g_p13PublishedPose.sampledHead = g_motionPoseHead;
    g_p13PublishedPose.sampledHeadValid = g_motionPoseHeadValid;
    g_p13PublishedPose.sampledPawnLocation = sampledPawn;
    g_p13PublishedPose.sampledPawnValid = sampledPawnValid;
    g_p13PublishedPose.presentFrame = g_frames;
    MemoryBarrier();
    InterlockedIncrement(&g_p13PoseSequence);      // even: readers may consume it
}

static bool ReadP13PoseSnapshot(P13PoseSnapshot* out)
{
    if (!out) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG before = InterlockedCompareExchange(&g_p13PoseSequence, 0, 0);
        if (before == 0 || (before & 1)) continue;
        MemoryBarrier();
        const P13PoseSnapshot candidate = g_p13PublishedPose;
        MemoryBarrier();
        const LONG after = InterlockedCompareExchange(&g_p13PoseSequence, 0, 0);
        if (before == after && !(after & 1)) {
            *out = candidate;
            return true;
        }
    }
    return false;
}

static bool FiniteVec(const MEVR_Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static float VecLength(const MEVR_Vec3& v)
{
    return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
}

static MEVR_Vec3 RotateByQuaternion(const XrQuaternionf& q, const MEVR_Vec3& v)
{
    // q * v * conjugate(q), expanded. q is normalised by the caller.
    const MEVR_Vec3 t{ 2.0f * (q.y*v.z - q.z*v.y),
                       2.0f * (q.z*v.x - q.x*v.z),
                       2.0f * (q.x*v.y - q.y*v.x) };
    return { v.x + q.w*t.x + (q.y*t.z - q.z*t.y),
             v.y + q.w*t.y + (q.z*t.x - q.x*t.z),
             v.z + q.w*t.z + (q.x*t.y - q.y*t.x) };
}

static XrQuaternionf MultiplyQuaternion(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

// OpenXR VIEW: +X right, +Y up, -Z forward. UE3 camera-local: +X forward, +Y right, +Z up.
static MEVR_Vec3 XrViewToUECamera(const MEVR_Vec3& v)
{
    return { -v.z, v.x, v.y };
}

static MEVR_Vec3 CameraVectorToWorld(const RenderedHeadFrame& head, const MEVR_Vec3& v)
{
    return { head.forward.x*v.x + head.right.x*v.y + head.up.x*v.z,
             head.forward.y*v.x + head.right.y*v.y + head.up.y*v.z,
             head.forward.z*v.x + head.right.z*v.y + head.up.z*v.z };
}

static XrQuaternionf QuaternionFromUEBasis(const MEVR_Vec3& forward,
                                           const MEVR_Vec3& right,
                                           const MEVR_Vec3& up)
{
    // Rotation matrix columns are the UE3 hand's +X/+Y/+Z axes in world space.
    const float m00 = forward.x, m01 = right.x, m02 = up.x;
    const float m10 = forward.y, m11 = right.y, m12 = up.y;
    const float m20 = forward.z, m21 = right.z, m22 = up.z;
    XrQuaternionf q{};
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

static XrQuaternionf ViewGripOrientationToUEWorld(const XrQuaternionf& q,
                                                   const RenderedHeadFrame& head)
{
    const MEVR_Vec3 handForwardView = RotateByQuaternion(q, { 0.0f, 0.0f, -1.0f });
    const MEVR_Vec3 handRightView   = RotateByQuaternion(q, { 1.0f, 0.0f,  0.0f });
    const MEVR_Vec3 handUpView      = RotateByQuaternion(q, { 0.0f, 1.0f,  0.0f });
    const MEVR_Vec3 handForwardWorld =
        CameraVectorToWorld(head, XrViewToUECamera(handForwardView));
    const MEVR_Vec3 handRightWorld =
        CameraVectorToWorld(head, XrViewToUECamera(handRightView));
    const MEVR_Vec3 handUpWorld =
        CameraVectorToWorld(head, XrViewToUECamera(handUpView));
    return QuaternionFromUEBasis(handForwardWorld, handRightWorld, handUpWorld);
}

static bool NormalizedQuaternion(const XrQuaternionf& raw, XrQuaternionf* out)
{
    if (!out || !std::isfinite(raw.x) || !std::isfinite(raw.y) ||
        !std::isfinite(raw.z) || !std::isfinite(raw.w)) return false;
    const float norm2 = raw.x*raw.x + raw.y*raw.y + raw.z*raw.z + raw.w*raw.w;
    if (!std::isfinite(norm2) || norm2 < 0.81f || norm2 > 1.21f) return false;
    const float inv = 1.0f / sqrtf(norm2);
    *out = { raw.x*inv, raw.y*inv, raw.z*inv, raw.w*inv };
    return true;
}

static bool ConvertMotionPoseToUEWorld(const char* name, const RenderedHeadFrame& head,
                                       MotionPoseSample* sample)
{
    sample->worldValid = false;
    sample->cameraLocal = {};
    sample->worldPosition = {};
    sample->worldOrientation = {};
    sample->sourceQuatNorm = 0.0f;
    sample->worldQuatNorm = 0.0f;

    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                      XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (!sample->active || (sample->flags & need) != need) return false;

    const XrVector3f p = sample->pose.position;
    const XrQuaternionf raw = sample->pose.orientation;
    const float qn2 = raw.x*raw.x + raw.y*raw.y + raw.z*raw.z + raw.w*raw.w;
    const float metres = sqrtf(p.x*p.x + p.y*p.y + p.z*p.z);
    const bool honest = std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
                        std::isfinite(qn2) && qn2 > 0.81f && qn2 < 1.21f &&
                        metres >= 0.01f && metres <= 3.0f;
    if (!honest) {
        const long rejected = ++sample->rejectedConversions;
        if (g_motionHandsDebug && (rejected == 1 || rejected % 600 == 0))
            Log("[hands] rejected %s conversion #%ld: distance=%.3f m quaternion norm^2=%.4f",
                name, rejected, metres, qn2);
        return false;
    }

    sample->sourceQuatNorm = sqrtf(qn2);
    const float qi = 1.0f / sample->sourceQuatNorm;
    const XrQuaternionf q{ raw.x*qi, raw.y*qi, raw.z*qi, raw.w*qi };

    sample->cameraLocal = XrViewToUECamera({ p.x*g_worldScale,
                                             p.y*g_worldScale,
                                             p.z*g_worldScale });
    const MEVR_Vec3 worldOffset = CameraVectorToWorld(head, sample->cameraLocal);
    sample->worldPosition = { head.position.x + worldOffset.x,
                              head.position.y + worldOffset.y,
                              head.position.z + worldOffset.z };

    // Convert the controller's own OpenXR axes through the same two bases as position. This is
    // shared with the late game-thread re-anchor so the two paths cannot disagree about handedness.
    sample->worldOrientation = ViewGripOrientationToUEWorld(q, head);
    const XrQuaternionf& wq = sample->worldOrientation;
    sample->worldQuatNorm = sqrtf(wq.x*wq.x + wq.y*wq.y + wq.z*wq.z + wq.w*wq.w);
    sample->worldValid = FiniteVec(sample->cameraLocal) && FiniteVec(sample->worldPosition) &&
                         std::isfinite(sample->worldQuatNorm) &&
                         sample->worldQuatNorm > 0.99f && sample->worldQuatNorm < 1.01f;
    if (!sample->worldValid) {
        const long rejected = ++sample->rejectedConversions;
        if (g_motionHandsDebug && (rejected == 1 || rejected % 600 == 0))
            Log("[hands] rejected %s UE transform #%ld: output quaternion norm=%.4f",
                name, rejected, sample->worldQuatNorm);
    }
    return sample->worldValid;
}

static void XrDestroyMotionPoseSpaces()
{
    XrSpace* spaces[] = { &g_sLGripPose, &g_sRGripPose, &g_sLAimPose, &g_sRAimPose };
    for (XrSpace* space : spaces) {
        if (*space != XR_NULL_HANDLE) {
            xrDestroySpace(*space);
            *space = XR_NULL_HANDLE;
        }
    }
}

static void XrInitActions()
{
    if (g_xrInstance == XR_NULL_HANDLE || g_xrSession == XR_NULL_HANDLE) return;

    XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(asci.actionSetName, "gameplay");
    strcpy_s(asci.localizedActionSetName, "Gameplay");
    if (XR_FAILED(xrCreateActionSet(g_xrInstance, &asci, &g_actionSet))) {
        Log("[pad] xrCreateActionSet failed - controllers unavailable");
        return;
    }

    auto mk = [&](XrAction* out, const char* name, XrActionType type) {
        XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
        strcpy_s(aci.actionName, name);
        strcpy_s(aci.localizedActionName, name);
        aci.actionType = type;
        if (XR_FAILED(xrCreateAction(g_actionSet, &aci, out))) {
            Log("[pad] xrCreateAction(%s) failed", name);
            *out = XR_NULL_HANDLE;
        }
    };
    mk(&g_aMove,   "move",   XR_ACTION_TYPE_VECTOR2F_INPUT);
    mk(&g_aLook,   "look",   XR_ACTION_TYPE_VECTOR2F_INPUT);
    mk(&g_aA,      "abtn",   XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&g_aB,      "bbtn",   XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&g_aX,      "xbtn",   XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&g_aY,      "ybtn",   XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&g_aLTrig,  "ltrig",  XR_ACTION_TYPE_FLOAT_INPUT);
    mk(&g_aRTrig,  "rtrig",  XR_ACTION_TYPE_FLOAT_INPUT);
    mk(&g_aLGrip,  "lgrip",  XR_ACTION_TYPE_FLOAT_INPUT);
    mk(&g_aRGrip,  "rgrip",  XR_ACTION_TYPE_FLOAT_INPUT);
    mk(&g_aMenu,   "menu",   XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&g_aLClick, "lclick", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&g_aRClick, "rclick", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&g_aLGripPose, "left_grip_pose",  XR_ACTION_TYPE_POSE_INPUT);
    mk(&g_aRGripPose, "right_grip_pose", XR_ACTION_TYPE_POSE_INPUT);
    mk(&g_aLAimPose,  "left_aim_pose",   XR_ACTION_TYPE_POSE_INPUT);
    mk(&g_aRAimPose,  "right_aim_pose",  XR_ACTION_TYPE_POSE_INPUT);

    auto mkSpace = [&](XrAction action, XrSpace* out, const char* name) {
        if (action == XR_NULL_HANDLE) return;
        XrActionSpaceCreateInfo ci{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
        ci.action = action;
        ci.poseInActionSpace.orientation.w = 1.0f;
        const XrResult result = xrCreateActionSpace(g_xrSession, &ci, out);
        if (XR_FAILED(result)) {
            *out = XR_NULL_HANDLE;
            Log("[hands] xrCreateActionSpace(%s) failed -> %d", name, (int)result);
        }
    };
    mkSpace(g_aLGripPose, &g_sLGripPose, "left grip");
    mkSpace(g_aRGripPose, &g_sRGripPose, "right grip");
    mkSpace(g_aLAimPose,  &g_sLAimPose,  "left aim");
    mkSpace(g_aRAimPose,  &g_sRAimPose,  "right aim");

    auto path = [&](const char* s) {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(g_xrInstance, s, &p);
        return p;
    };

    // Touch, because that is what a Quest reports through Virtual Desktop. The simple controller
    // profile is suggested as well: it carries only a select and a menu button, which is not
    // playable, but it means an unrecognised controller still reaches the menus instead of
    // appearing completely dead.
    std::vector<XrActionSuggestedBinding> touch;
    auto bind = [&](std::vector<XrActionSuggestedBinding>& bindings,
                    XrAction action, const char* source) {
        if (action != XR_NULL_HANDLE) bindings.push_back({ action, path(source) });
    };
    bind(touch, g_aMove,      "/user/hand/left/input/thumbstick");
    bind(touch, g_aLook,      "/user/hand/right/input/thumbstick");
    bind(touch, g_aA,         "/user/hand/right/input/a/click");
    bind(touch, g_aB,         "/user/hand/right/input/b/click");
    bind(touch, g_aX,         "/user/hand/left/input/x/click");
    bind(touch, g_aY,         "/user/hand/left/input/y/click");
    bind(touch, g_aLTrig,     "/user/hand/left/input/trigger/value");
    bind(touch, g_aRTrig,     "/user/hand/right/input/trigger/value");
    bind(touch, g_aLGrip,     "/user/hand/left/input/squeeze/value");
    bind(touch, g_aRGrip,     "/user/hand/right/input/squeeze/value");
    bind(touch, g_aMenu,      "/user/hand/left/input/menu/click");
    bind(touch, g_aLClick,    "/user/hand/left/input/thumbstick/click");
    bind(touch, g_aRClick,    "/user/hand/right/input/thumbstick/click");
    bind(touch, g_aLGripPose, "/user/hand/left/input/grip/pose");
    bind(touch, g_aRGripPose, "/user/hand/right/input/grip/pose");
    bind(touch, g_aLAimPose,  "/user/hand/left/input/aim/pose");
    bind(touch, g_aRAimPose,  "/user/hand/right/input/aim/pose");
    XrInteractionProfileSuggestedBinding sib{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sib.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sib.suggestedBindings = touch.data();
    sib.countSuggestedBindings = (uint32_t)touch.size();
    XrResult sr = xrSuggestInteractionProfileBindings(g_xrInstance, &sib);
    if (XR_FAILED(sr)) Log("[pad] suggested bindings for oculus/touch rejected -> %d", (int)sr);

    std::vector<XrActionSuggestedBinding> simple;
    bind(simple, g_aA,         "/user/hand/right/input/select/click");
    bind(simple, g_aMenu,      "/user/hand/left/input/menu/click");
    bind(simple, g_aLGripPose, "/user/hand/left/input/grip/pose");
    bind(simple, g_aRGripPose, "/user/hand/right/input/grip/pose");
    bind(simple, g_aLAimPose,  "/user/hand/left/input/aim/pose");
    bind(simple, g_aRAimPose,  "/user/hand/right/input/aim/pose");
    XrInteractionProfileSuggestedBinding sib2{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sib2.interactionProfile = path("/interaction_profiles/khr/simple_controller");
    sib2.suggestedBindings = simple.data();
    sib2.countSuggestedBindings = (uint32_t)simple.size();
    const XrResult sr2 = xrSuggestInteractionProfileBindings(g_xrInstance, &sib2);
    if (XR_FAILED(sr2)) Log("[hands] suggested bindings for simple controller rejected -> %d", (int)sr2);

    // ⚠️ Permanent. Once action sets are attached to a session no more can be added, so this is
    // the last chance to have created every action - which is why they are all made above rather
    // than lazily when first needed.
    XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1;
    ai.actionSets = &g_actionSet;
    if (XR_FAILED(xrAttachSessionActionSets(g_xrSession, &ai))) {
        Log("[pad] xrAttachSessionActionSets failed - controllers unavailable");
        XrDestroyMotionPoseSpaces();
        return;
    }
    if (!g_padLockReady) { InitializeCriticalSection(&g_padLock); g_padLockReady = true; }
    g_actionsReady = true;
    Log("*** [pad] controller actions attached - sticks, triggers, grips and face buttons");
    Log("*** [hands] grip/aim pose actions attached; spaces Lg=%s Rg=%s La=%s Ra=%s",
        g_sLGripPose != XR_NULL_HANDLE ? "yes" : "NO",
        g_sRGripPose != XR_NULL_HANDLE ? "yes" : "NO",
        g_sLAimPose  != XR_NULL_HANDLE ? "yes" : "NO",
        g_sRAimPose  != XR_NULL_HANDLE ? "yes" : "NO");
}

// Read the controllers and build a 360 pad out of them. Called once per frame from the XR loop.
static bool XrSyncInput()
{
    if (!g_actionsReady) return false;

    XrActiveActionSet aas{ g_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    // Returns SESSION_NOT_FOCUSED whenever the headset menu is up. Not an error, and not worth
    // logging every frame - the actions simply report inactive and the pad reads as centred.
    if (XR_FAILED(xrSyncActions(g_xrSession, &si))) return false;
    // Pose actions share this action set. They remain live when NUMPAD9 disables pad synthesis.
    if (!g_padEnabled) return true;

    // Returns whether the action was ACTIVE, which the caller now records. isActive is the
    // runtime saying "this action is bound to a control on a controller that is present" - so
    // a stick that is never active is a stick that was never wired up, and a stick that is
    // always active but always centred is a player who did not push it. Those are different
    // bugs and the difference was previously discarded here.
    auto vec2 = [&](XrAction a, float* x, float* y) -> bool {
        *x = *y = 0.0f;
        if (a == XR_NULL_HANDLE) return false;
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = a;
        XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
        if (XR_SUCCEEDED(xrGetActionStateVector2f(g_xrSession, &gi, &st)) && st.isActive) {
            *x = st.currentState.x; *y = st.currentState.y;
            return true;
        }
        return false;
    };
    auto flt = [&](XrAction a) -> float {
        if (a == XR_NULL_HANDLE) return 0.0f;
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = a;
        XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT };
        if (XR_SUCCEEDED(xrGetActionStateFloat(g_xrSession, &gi, &st)) && st.isActive)
            return st.currentState;
        return 0.0f;
    };
    auto bl = [&](XrAction a) -> bool {
        if (a == XR_NULL_HANDLE) return false;
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = a;
        XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
        if (XR_SUCCEEDED(xrGetActionStateBoolean(g_xrSession, &gi, &st)) && st.isActive)
            return st.currentState == XR_TRUE;
        return false;
    };

    float mx, my, lx, ly;
    const bool moveLive = vec2(g_aMove, &mx, &my);
    const bool lookLive = vec2(g_aLook, &lx, &ly);

    MEVR_XINPUT_STATE s{};
    auto axis = [](float v) -> SHORT {
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        return (SHORT)(v * 32767.0f);
    };
    s.Gamepad.sThumbLX = axis(mx);
    s.Gamepad.sThumbLY = axis(my);
    s.Gamepad.sThumbRX = axis(lx);
    s.Gamepad.sThumbRY = axis(ly);
    s.Gamepad.bLeftTrigger  = (BYTE)(flt(g_aLTrig) * 255.0f);
    s.Gamepad.bRightTrigger = (BYTE)(flt(g_aRTrig) * 255.0f);

    WORD b = 0;
    if (bl(g_aA)) b |= MEVR_PAD_A;
    if (bl(g_aB)) b |= MEVR_PAD_B;
    if (bl(g_aX)) b |= MEVR_PAD_X;
    if (bl(g_aY)) b |= MEVR_PAD_Y;
    if (bl(g_aMenu))   b |= MEVR_PAD_START;
    // ---- left stick click is BACK, not LTHUMB ----
    //
    // Back is `GBA_InGameMenu` and was the one live binding no physical control could reach:
    // every button on a Touch pair is already spoken for, and the right controller's system
    // button belongs to the runtime. LTHUMB was the one press being spent on nothing.
    //
    // "Nothing" is the game's own decision, not an omission. DefaultInput.ini line 146 is
    // `.Bindings=(Name="XboxTypeS_LeftThumbstick",Command="")` - an explicit blanking, put there
    // to kill the engine default of ToggleDebugCamera in BaseInput.ini. It is dead in menus too;
    // every widget alias that names a stick click names the RIGHT one.
    //
    // So this costs nothing and buys the in-game menu. The pad is synthesised, so the remap
    // happens here - hand-editing the ini is not an option, the game hash-checks its config and
    // refuses to start, already measured at two runs.
    if (bl(g_aLClick)) b |= MEVR_PAD_BACK;
    if (bl(g_aRClick)) b |= MEVR_PAD_RTHUMB;
    // Grips are analogue on Touch and shoulder buttons on a pad, so they cross over at a
    // threshold rather than being dropped. Half pressed is deliberate: a grip is squeezed
    // decisively or not at all, unlike a trigger.
    if (flt(g_aLGrip) > 0.5f) b |= MEVR_PAD_LSHOULDER;
    if (flt(g_aRGrip) > 0.5f) b |= MEVR_PAD_RSHOULDER;
    s.Gamepad.wButtons = b;

    // ---- record the window, before the lock ----
    //
    // These are ours: written only here, read only by ReportPadState, and both run on the thread
    // that drives Present. Nothing shared with the game's poller, so no lock.
    //
    // EXTREMES rather than a sample, for the reason the animation probe keeps peaks: a value
    // read once every fifteen seconds catches the player mid-nothing and reports a centred stick
    // whatever is true.
    {
        const float v[4] = { mx, my, lx, ly };
        for (int i = 0; i < 4; ++i) {
            if (v[i] < g_padLo[i]) g_padLo[i] = v[i];
            if (v[i] > g_padHi[i]) g_padHi[i] = v[i];
        }
        g_padSyncs++;
        if (moveLive) g_padMoveLive++;
        if (lookLive) g_padLookLive++;
        g_padButtonsSeen |= b;
        if (s.Gamepad.bLeftTrigger  > g_padTrigPeak[0]) g_padTrigPeak[0] = s.Gamepad.bLeftTrigger;
        if (s.Gamepad.bRightTrigger > g_padTrigPeak[1]) g_padTrigPeak[1] = s.Gamepad.bRightTrigger;
    }

    EnterCriticalSection(&g_padLock);
    // The packet number must CHANGE when the state does, or a poller that compares packets will
    // decide nothing happened and skip the read entirely.
    const bool changed = memcmp(&s.Gamepad, &g_pad.Gamepad, sizeof(s.Gamepad)) != 0;
    const DWORD pn = g_pad.dwPacketNumber + (changed ? 1 : 0);
    g_pad = s;
    g_pad.dwPacketNumber = pn;
    LeaveCriticalSection(&g_padLock);
    return true;
}

static int MotionPoseStatus(const MotionPoseSample& sample)
{
    int status = sample.active ? 1 : 0;
    if (sample.flags & XR_SPACE_LOCATION_POSITION_VALID_BIT)       status |= 2;
    if (sample.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)    status |= 4;
    if (sample.flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)     status |= 8;
    if (sample.flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)  status |= 16;
    return status;
}

static void XrSampleMotionPose(const char* name, XrAction action, XrSpace space,
                               XrTime when, bool actionsSynced, MotionPoseSample* sample)
{
    sample->active = false;
    sample->flags = 0;
    sample->pose = {};
    sample->cameraLocal = {};
    sample->worldPosition = {};
    sample->worldOrientation = {};
    sample->sourceQuatNorm = 0.0f;
    sample->worldQuatNorm = 0.0f;
    sample->worldValid = false;

    if (actionsSynced && action != XR_NULL_HANDLE && space != XR_NULL_HANDLE &&
        g_viewSpace != XR_NULL_HANDLE) {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = action;
        XrActionStatePose state{ XR_TYPE_ACTION_STATE_POSE };
        if (XR_SUCCEEDED(xrGetActionStatePose(g_xrSession, &gi, &state)) && state.isActive) {
            sample->active = true;
            XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
            if (XR_SUCCEEDED(xrLocateSpace(space, g_viewSpace, when, &loc))) {
                sample->flags = loc.locationFlags;
                sample->pose = loc.pose;
            }
        }
    }

    const int status = MotionPoseStatus(*sample);
    if (g_motionHandsDebug && status != sample->reportedStatus) {
        // Do not emit four unhelpful "inactive" lines on the first frame. Once a pose has been
        // seen, every loss and recovery is logged exactly once at the transition.
        if (sample->reportedStatus >= 0 || status != 0) {
            Log("[hands] %s pose -> active=%d position=%s/%s orientation=%s/%s",
                name, sample->active ? 1 : 0,
                (sample->flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? "valid" : "invalid",
                (sample->flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) ? "tracked" : "untracked",
                (sample->flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ? "valid" : "invalid",
                (sample->flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) ? "tracked" : "untracked");
        }
        sample->reportedStatus = status;
    }
}

static void XrSampleMotionPoses(XrTime when, bool actionsSynced)
{
    if (!g_motionHands && !g_motionHandsDebug) return;

    XrSampleMotionPose("left grip",  g_aLGripPose, g_sLGripPose, when, actionsSynced, &g_poseLGrip);
    XrSampleMotionPose("right grip", g_aRGripPose, g_sRGripPose, when, actionsSynced, &g_poseRGrip);

    // Aim poses are created for Phase 2 but sampled only for diagnostics in Phase 1.
    if (g_motionHandsDebug) {
        XrSampleMotionPose("left aim",  g_aLAimPose, g_sLAimPose, when, actionsSynced, &g_poseLAim);
        XrSampleMotionPose("right aim", g_aRAimPose, g_sRAimPose, when, actionsSynced, &g_poseRAim);
    }

    RenderedHeadFrame head{};
    const bool haveHead = GetRenderedHeadFrame(&head);
    g_motionPoseHead = head;
    g_motionPoseHeadValid = haveHead;
    {
        static bool reported = false, previous = false;
        if (g_motionHandsDebug && (!reported || haveHead != previous)) {
            // The initial menu-side miss is expected, but naming it makes a run that never finds
            // the pawn distinguishable from a pose conversion that silently did nothing.
            Log("[hands] rendered UE3 head frame -> %s",
                haveHead ? "available (camera anchor + 6-DOF)" : "unavailable (no live player camera)");
            reported = true;
            previous = haveHead;
        }
    }

    if (haveHead) {
        ConvertMotionPoseToUEWorld("left grip",  head, &g_poseLGrip);
        ConvertMotionPoseToUEWorld("right grip", head, &g_poseRGrip);
        if (g_motionHandsDebug) {
            ConvertMotionPoseToUEWorld("left aim",  head, &g_poseLAim);
            ConvertMotionPoseToUEWorld("right aim", head, &g_poseRAim);
        }
    }

    // PAGE UP clears the camera's positional offset and the hand world anchor together. The
    // invariant is the controller relative to the rendered head, so measure that directly over
    // the recenter frame rather than expecting absolute world coordinates to stay fixed.
    {
        static LONG seenSerial = 0;
        static bool havePrevious = false;
        static MEVR_Vec3 previousLeft{}, previousRight{};
        const LONG serial = g_motionRecenterSerial;
        if (g_motionHandsDebug && serial != seenSerial) {
            if (havePrevious && g_poseLGrip.worldValid && g_poseRGrip.worldValid) {
                const MEVR_Vec3 dl{ g_poseLGrip.cameraLocal.x - previousLeft.x,
                                    g_poseLGrip.cameraLocal.y - previousLeft.y,
                                    g_poseLGrip.cameraLocal.z - previousLeft.z };
                const MEVR_Vec3 dr{ g_poseRGrip.cameraLocal.x - previousRight.x,
                                    g_poseRGrip.cameraLocal.y - previousRight.y,
                                    g_poseRGrip.cameraLocal.z - previousRight.z };
                Log("[hands] recenter #%ld: head-relative grip jump L %.2f UU R %.2f UU"
                    " (physical motion plus one frame; no recenter offset is applied to hands)",
                    serial, VecLength(dl), VecLength(dr));
            } else {
                Log("[hands] recenter #%ld: no pair of valid grip poses available for jump check", serial);
            }
            seenSerial = serial;
        }
        if (g_poseLGrip.worldValid && g_poseRGrip.worldValid) {
            previousLeft = g_poseLGrip.cameraLocal;
            previousRight = g_poseRGrip.cameraLocal;
            havePrevious = true;
        }
    }

    if (g_motionHandsDebug) {
        static double lastReportMs = 0.0;
        const double now = NowMs();
        if (now - lastReportMs >= 5000.0) {
            lastReportMs = now;
            Log("[hands] head-relative metres: grip L(%+.3f,%+.3f,%+.3f) R(%+.3f,%+.3f,%+.3f)"
                "  aim L(%+.3f,%+.3f,%+.3f) R(%+.3f,%+.3f,%+.3f)",
                g_poseLGrip.pose.position.x, g_poseLGrip.pose.position.y, g_poseLGrip.pose.position.z,
                g_poseRGrip.pose.position.x, g_poseRGrip.pose.position.y, g_poseRGrip.pose.position.z,
                g_poseLAim.pose.position.x,  g_poseLAim.pose.position.y,  g_poseLAim.pose.position.z,
                g_poseRAim.pose.position.x,  g_poseRAim.pose.position.y,  g_poseRAim.pose.position.z);
            if (g_poseLGrip.worldValid && g_poseRGrip.worldValid) {
                const MEVR_Vec3 separation{ g_poseRGrip.worldPosition.x - g_poseLGrip.worldPosition.x,
                                            g_poseRGrip.worldPosition.y - g_poseLGrip.worldPosition.y,
                                            g_poseRGrip.worldPosition.z - g_poseLGrip.worldPosition.z };
                Log("[hands] camera-local UU (F,R,U): grip L(%+.1f,%+.1f,%+.1f)"
                    " R(%+.1f,%+.1f,%+.1f)  distance L %.1f R %.1f",
                    g_poseLGrip.cameraLocal.x, g_poseLGrip.cameraLocal.y, g_poseLGrip.cameraLocal.z,
                    g_poseRGrip.cameraLocal.x, g_poseRGrip.cameraLocal.y, g_poseRGrip.cameraLocal.z,
                    VecLength(g_poseLGrip.cameraLocal), VecLength(g_poseRGrip.cameraLocal));
                Log("[hands] UE3 world UU: head(%+.1f,%+.1f,%+.1f) L(%+.1f,%+.1f,%+.1f)"
                    " R(%+.1f,%+.1f,%+.1f) separation %.1f | quat norms L %.4f->%.4f R %.4f->%.4f",
                    head.position.x, head.position.y, head.position.z,
                    g_poseLGrip.worldPosition.x, g_poseLGrip.worldPosition.y, g_poseLGrip.worldPosition.z,
                    g_poseRGrip.worldPosition.x, g_poseRGrip.worldPosition.y, g_poseRGrip.worldPosition.z,
                    VecLength(separation),
                    g_poseLGrip.sourceQuatNorm, g_poseLGrip.worldQuatNorm,
                    g_poseRGrip.sourceQuatNorm, g_poseRGrip.worldQuatNorm);
            } else {
                Log("[hands] UE3 conversion unavailable this report: head=%d left=%d right=%d",
                    haveHead ? 1 : 0, g_poseLGrip.worldValid ? 1 : 0, g_poseRGrip.worldValid ? 1 : 0);
            }
        }
    }
}

// The pad half of the movement diagnosis. Reported on the same window as the animation probe,
// so a "cannot move" report arrives with both ends of the chain on adjacent lines: what the
// runtime handed us, and what the game did with it.
static void ReportPadState()
{
    if (!g_actionsReady || g_padSyncs == 0) return;

    const long polls = g_padPolls;
    Log("[pad] over %ld syncs: LEFT  x %+.2f..%+.2f  y %+.2f..%+.2f   delivered by the runtime %ld/%ld",
        g_padSyncs, g_padLo[0], g_padHi[0], g_padLo[1], g_padHi[1], g_padMoveLive, g_padSyncs);
    Log("[pad]                 RIGHT x %+.2f..%+.2f  y %+.2f..%+.2f   delivered by the runtime %ld/%ld",
        g_padLo[2], g_padHi[2], g_padLo[3], g_padHi[3], g_padLookLive, g_padSyncs);

    // Named with what the game does with each, because the whole point of a 1:1 map is that the
    // pad name is not the thing the reporter pressed. LTHUMB is absent deliberately: the left
    // stick click is synthesised as BACK, so the bit is never set.
    static const struct { WORD bit; const char* name; } kPadBits[] = {
        { MEVR_PAD_A,         "A(use)"                 },
        { MEVR_PAD_B,         "B(lookat)"              },
        { MEVR_PAD_X,         "X(reaction)"            },
        { MEVR_PAD_Y,         "Y(weapon)"              },
        { MEVR_PAD_START,     "START(pause)"           },
        { MEVR_PAD_BACK,      "BACK(ingame-menu)"      },
        { MEVR_PAD_RTHUMB,    "RTHUMB(zoom)"           },
        { MEVR_PAD_LSHOULDER, "LSHOULDER(jump)"        },
        { MEVR_PAD_RSHOULDER, "RSHOULDER(lookbehind)"  },
    };
    char btns[256]; int n = 0;
    btns[0] = 0;
    for (int i = 0; i < (int)(sizeof(kPadBits) / sizeof(kPadBits[0])); ++i)
        if (g_padButtonsSeen & kPadBits[i].bit)
            n += _snprintf_s(btns + n, sizeof(btns) - n, _TRUNCATE, " %s", kPadBits[i].name);
    Log("[pad]   buttons this window:%s  |  triggers peak L %u R %u  |  the game read the pad %ld times",
        btns[0] ? btns : " none", (unsigned)g_padTrigPeak[0], (unsigned)g_padTrigPeak[1],
        polls - g_padPollsSeen);

    // The two conclusions worth stating outright, because the numbers above are only obvious
    // once you already know which fault you are looking at.
    if (g_padMoveLive == 0)
        Log("[pad] ⚠️ the LEFT stick was never DELIVERED this window - not bound, rather than not"
            " pushed. Movement cannot reach the game and the fault is on this side.");
    else if (g_padLo[0] == 0.0f && g_padHi[0] == 0.0f && g_padLo[1] == 0.0f && g_padHi[1] == 0.0f)
        Log("[pad]   the left stick was live all window and never left centre.");
    else if ((polls - g_padPollsSeen) == 0)
        Log("[pad] ⚠️ the left stick moved but the game did not READ the pad this window - the"
            " XInput hook is installed but nothing is polling it.");

    g_padSyncs = g_padMoveLive = g_padLookLive = 0;
    for (int i = 0; i < 4; ++i) g_padLo[i] = g_padHi[i] = 0.0f;
    g_padButtonsSeen = 0;
    g_padTrigPeak[0] = g_padTrigPeak[1] = 0;
    g_padPollsSeen = polls;
}

// ---- the hook ----
//
// XInputGetState is detoured rather than the import patched, so it is caught however the game
// reaches it. That matters here: the exe names XINPUT1_3.dll in its imports but no XInput
// function by name, which means the functions are imported BY ORDINAL and there is no import
// name to patch.

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, MEVR_XINPUT_STATE*);
typedef DWORD (WINAPI *PFN_XInputGetCaps)(DWORD, DWORD, MEVR_XINPUT_CAPABILITIES*);
static PFN_XInputGetState g_origXiGetState = nullptr;
static PFN_XInputGetCaps  g_origXiGetCaps  = nullptr;
static bool g_xiHooked = false;

static DWORD WINAPI Hook_XInputGetState(DWORD idx, MEVR_XINPUT_STATE* out)
{
    // Only pad 0. Reporting a controller on every index makes the game think four players are
    // present, and some engines then poll all of them every frame for nothing.
    if (g_padEnabled && g_actionsReady && idx == 0 && out) {
        EnterCriticalSection(&g_padLock);
        *out = g_pad;
        LeaveCriticalSection(&g_padLock);
        if (InterlockedIncrement(&g_padPolls) == 1)
            Log("*** [pad] the game is polling XInput - the synthesised pad is reaching it");
        return ERROR_SUCCESS;
    }
    if (g_origXiGetState) return g_origXiGetState(idx, out);
    return ERROR_DEVICE_NOT_CONNECTED;
}

static DWORD WINAPI Hook_XInputGetCaps(DWORD idx, DWORD flags, MEVR_XINPUT_CAPABILITIES* out)
{
    if (g_padEnabled && g_actionsReady && idx == 0 && out) {
        MEVR_XINPUT_CAPABILITIES c{};
        c.Type = 1;        // XINPUT_DEVTYPE_GAMEPAD
        c.SubType = 1;     // XINPUT_DEVSUBTYPE_GAMEPAD
        // Every field the pad can report, set to its maximum: this describes what the device is
        // CAPABLE of, not its current state, and a zeroed one reads as a pad with no sticks.
        c.Gamepad.wButtons = 0xF3FF;
        c.Gamepad.bLeftTrigger = 0xFF; c.Gamepad.bRightTrigger = 0xFF;
        c.Gamepad.sThumbLX = c.Gamepad.sThumbLY = (SHORT)0xFFC0;
        c.Gamepad.sThumbRX = c.Gamepad.sThumbRY = (SHORT)0xFFC0;
        *out = c;
        return ERROR_SUCCESS;
    }
    if (g_origXiGetCaps) return g_origXiGetCaps(idx, flags, out);
    return ERROR_DEVICE_NOT_CONNECTED;
}

// Retried from Present until it takes. The module is imported by the exe so it is loaded before
// we are, but ordering assumptions about another process's loader are exactly the kind of thing
// that holds until it does not.
static void InstallXInputHook()
{
    if (g_xiHooked) return;
    static long tries = 0;
    if (++tries > 600) return;                    // ten seconds at 60 fps, then stop asking

    HMODULE xi = GetModuleHandleA("xinput1_3.dll");
    if (!xi) xi = GetModuleHandleA("XINPUT1_3.dll");
    if (!xi) xi = GetModuleHandleA("xinput9_1_0.dll");
    if (!xi) xi = GetModuleHandleA("xinput1_4.dll");
    if (!xi) return;

    void* pState = (void*)GetProcAddress(xi, "XInputGetState");
    void* pCaps  = (void*)GetProcAddress(xi, "XInputGetCapabilities");
    if (!pState) { g_xiHooked = true; Log("[pad] XInputGetState not exported - cannot hook"); return; }

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        g_xiHooked = true;
        Log("[pad] MH_Initialize failed - controllers unavailable");
        return;
    }
    MH_STATUS a = MH_CreateHook(pState, (void*)&Hook_XInputGetState, (void**)&g_origXiGetState);
    if (a == MH_OK) MH_EnableHook(pState);
    if (pCaps) {
        MH_STATUS c = MH_CreateHook(pCaps, (void*)&Hook_XInputGetCaps, (void**)&g_origXiGetCaps);
        if (c == MH_OK) MH_EnableHook(pCaps);
    }
    g_xiHooked = true;
    Log("*** [pad] XInputGetState hooked in %s (status %d) - motion controllers act as a pad",
        xi == GetModuleHandleA("xinput1_3.dll") ? "xinput1_3.dll" : "an xinput module", (int)a);
}

// Actions only deliver data while FOCUSED, and frames may only be submitted while the session
// is running. Without this the difference between "not focused" and "not working" is invisible.
static void PumpXREvents()
{
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(g_xrInstance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            g_xrState = s->state;
            const char* nm = "?";
            switch (s->state) {
                case XR_SESSION_STATE_IDLE:         nm = "IDLE";         break;
                case XR_SESSION_STATE_READY:        nm = "READY";        break;
                case XR_SESSION_STATE_SYNCHRONIZED: nm = "SYNCHRONIZED"; break;
                case XR_SESSION_STATE_VISIBLE:      nm = "VISIBLE";      break;
                case XR_SESSION_STATE_FOCUSED:      nm = "FOCUSED";      break;
                case XR_SESSION_STATE_STOPPING:     nm = "STOPPING";     break;
                case XR_SESSION_STATE_LOSS_PENDING: nm = "LOSS_PENDING"; break;
                case XR_SESSION_STATE_EXITING:      nm = "EXITING";      break;
                default: break;
            }
            Log("[xr] session state -> %d %s", (int)s->state, nm);
            if (s->state == XR_SESSION_STATE_READY && !g_xrRunning) {
                XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_xrSession, &bi))) {
                    g_xrRunning = true;
                    Log("*** [xr] session RUNNING - frames can now be submitted");
                }
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_xrSession);
                g_xrRunning = false;
                Log("[xr] session stopped");
            } else if (s->state == XR_SESSION_STATE_LOSS_PENDING ||
                       s->state == XR_SESSION_STATE_EXITING) {
                XrDestroyMotionPoseSpaces();
                Log("[hands] pose spaces destroyed with the terminating XR session");
            }
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

static bool EnsureSwapchain(uint32_t w, uint32_t h)
{
    if (g_swapchain != XR_NULL_HANDLE && w == g_scW && h == g_scH) return true;
    if (g_swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_swapchain); g_swapchain = XR_NULL_HANDLE;
        delete[] g_scImages; g_scImages = nullptr;
    }

    // Two-call enumeration, with the SECOND count clamped to what was actually allocated.
    // The reference does not clamp, and /analyze is right that it need not be safe: nothing
    // stops a runtime reporting a larger count on the second call than it did on the first.
    uint32_t fmtCap = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(g_xrSession, 0, &fmtCap, nullptr)) || fmtCap == 0) {
        Log("[xr] no swapchain formats reported"); return false;
    }
    int64_t* fmts = new int64_t[fmtCap];
    uint32_t fmtCount = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(g_xrSession, fmtCap, &fmtCount, fmts)) || fmtCount == 0) {
        delete[] fmts; Log("[xr] swapchain format enumeration failed"); return false;
    }
    if (fmtCount > fmtCap) fmtCount = fmtCap;

    // Colour space matters and is not cosmetic. The game writes sRGB-ENCODED pixels. If the
    // swapchain is declared linear, the compositor assumes linear data and encodes a second
    // time, which reads as washed out and low contrast. B8G8R8A8_UNORM_SRGB is bit-identical
    // to the D3D9 A8R8G8B8 layout measured in rung 1, so the eventual copy stays legal.
    const int64_t prefer[] = { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                               DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                               DXGI_FORMAT_B8G8R8A8_UNORM };
    int64_t chosen = fmts[0];
    bool picked = false;
    for (int p = 0; p < 3 && !picked; ++p)
        for (uint32_t i = 0; i < fmtCount; ++i)
            if (fmts[i] == prefer[p]) { chosen = fmts[i]; picked = true; break; }
    Log("[xr] swapchain format %lld %s", (long long)chosen,
        (chosen == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || chosen == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            ? "(sRGB - correct for the game's output)" : "(LINEAR - expect washed out colours)");
    delete[] fmts;

    XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format      = chosen;
    sci.sampleCount = 1;
    sci.width = w; sci.height = h;
    sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g_xrSession, &sci, &g_swapchain))) {
        Log("[xr] xrCreateSwapchain FAILED"); return false;
    }

    xrEnumerateSwapchainImages(g_swapchain, 0, &g_scImageCount, nullptr);
    XrSwapchainImageD3D11KHR* imgs = new XrSwapchainImageD3D11KHR[g_scImageCount];
    for (uint32_t i = 0; i < g_scImageCount; ++i) imgs[i] = { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR };
    xrEnumerateSwapchainImages(g_swapchain, g_scImageCount, &g_scImageCount,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs));
    g_scImages = new ID3D11Texture2D*[g_scImageCount];
    for (uint32_t i = 0; i < g_scImageCount; ++i) g_scImages[i] = imgs[i].texture;
    delete[] imgs;

    g_scW = w; g_scH = h; g_scFormat = chosen;
    Log("[xr] swapchain %ux%u images=%u", w, h, g_scImageCount);

    // What the runtime ACTUALLY allocated, which is not necessarily what was asked for.
    // Recorded because the difference between the requested format and the resource format
    // is the whole reason the render-target view has to name its format explicitly.
    if (g_scImageCount > 0 && g_scImages[0]) {
        D3D11_TEXTURE2D_DESC td{};
        g_scImages[0]->GetDesc(&td);
        Log("[xr] swapchain texture: %ux%u DXGI format=%d bind=0x%08lX  (requested %lld)",
            td.Width, td.Height, (int)td.Format, (unsigned long)td.BindFlags, (long long)chosen);
        if ((int)td.Format != (int)chosen)
            Log("[xr] note: resource format differs from the requested one - typeless, as expected");
    }
    return true;
}

// ================================================================ rung 3: the frame grab
//
// ⚠️ NO LONGER THE DEFAULT. Superseded by the shared surface at rung 8b, which does the same job
// on the GPU in 0.4 ms against this path's 3.7-4.6. This is the fallback now: it runs when the
// device is not Ex, when NUMPAD8 asks for it, and for any single frame the fast path declines.
//
// Kept whole rather than deleted. It is the known-good reference the shared surface was measured
// against, and a fallback that has to be reconstructed to be used is not a fallback.
//
// The SLOW path on purpose: backbuffer -> GetRenderTargetData -> SYSTEMMEM -> lock -> D3D11
// dynamic texture -> CopyResource into the XR swapchain image.
//
// It cost the Singularity project roughly 9.8 ms of a 16 ms frame at 4K, and it was still the
// right thing to build first. The fast route needs a D3D9Ex device, and D3D9Ex does not
// support D3DPOOL_MANAGED at all - so it drags in translating every MANAGED allocation rung 1
// counted. That wrapper was a live source of bugs for a hundred runs in the reference. Proving
// the pipe end to end without it meant any problem that showed up later had one plausible cause
// instead of two - which is exactly how rung 8 was able to be judged when it arrived.
//
// Formats line up without conversion, which is why this is a copy and not a shader:
// D3D9 A8R8G8B8 is BGRA byte order, i.e. exactly DXGI_FORMAT_B8G8R8A8_UNORM.

static void ReleaseFrameCapture()
{
    if (g_sysSurf) { g_sysSurf->Release(); g_sysSurf = nullptr; }
    if (g_upload)  { g_upload->Release();  g_upload  = nullptr; }
    g_capW = g_capH = 0;
    g_capFmt = D3DFMT_UNKNOWN;
    g_haveFrame = false;
}

static bool EnsureCapture(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt)
{
    if (g_sysSurf && g_upload && w == g_capW && h == g_capH && fmt == g_capFmt) return true;
    ReleaseFrameCapture();

    // GetRenderTargetData requires the destination to be an offscreen plain surface in
    // SYSTEMMEM with IDENTICAL format and dimensions. A mismatched format is the documented
    // D3DERR_INVALIDCALL here, which is why rung 1 read the format off the surface rather
    // than trusting the present parameters.
    HRESULT hr = dev->CreateOffscreenPlainSurface(w, h, fmt, D3DPOOL_SYSTEMMEM, &g_sysSurf, nullptr);
    if (FAILED(hr)) {
        Log("[cap] CreateOffscreenPlainSurface %ux%u fmt=%s FAILED hr=0x%08lX",
            w, h, FormatName(fmt), (unsigned long)hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;   // required for DYNAMIC
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_dev11->CreateTexture2D(&td, nullptr, &g_upload);
    if (FAILED(hr)) {
        Log("[cap] CreateTexture2D (upload) FAILED hr=0x%08lX", (unsigned long)hr);
        ReleaseFrameCapture();
        return false;
    }

    g_capW = w; g_capH = h; g_capFmt = fmt;
    Log("[cap] capture chain ready: %ux%u %s -> B8G8R8A8_UNORM", w, h, FormatName(fmt));
    return true;
}

// Called from Present, BEFORE the real Present runs - the backbuffer holds the finished frame.
// ================================================================ rung 8b: the shared surface
//
// The whole point of the Ex device. The frame stays on the GPU: StretchRect from the backbuffer
// into a surface D3D11 already has open, and no byte crosses the bus.
//
// The slow path is a GPU->CPU->GPU round trip. GetRenderTargetData blocks until the GPU has
// finished, then 14.7 MB is copied by the CPU into a system-memory surface, then copied again
// into a D3D11 dynamic texture. Measured at 3.7-4.6 ms of every frame.
//
// ---- format, and why the shared surface is not simply "the same as the backbuffer" ----
//
// A shared D3D9 surface opens in D3D11 with the DXGI format its D3D9 format maps to. X8R8G8B8
// maps to B8G8R8X8_UNORM, and the XR swapchain images are in the B8G8R8A8 family -
// CopySubresourceRegion between the two families is not legal, and would fail every frame after
// succeeding at creation.
//
// So the shared surface is always created A8R8G8B8, whatever the backbuffer is, and StretchRect
// converts on the way in. That is a GPU blit it is allowed to do, and it puts the result in the
// family the swapchain wants.
//
// ---- synchronisation ----
//
// Two devices touching one surface with no keyed mutex. The D3D9 blit is queued, not finished,
// when StretchRect returns, so D3D11 can read it mid-write and show a torn frame. An event query
// flushed to completion is the documented way to order this, and it is a GPU-side wait for a
// blit rather than a full readback - the thing being waited for is thousands of times cheaper
// than the thing the slow path waited for.

static IDirect3DSurface9* g_sharedRT   = nullptr;   // D3D9 side, shared
static HANDLE             g_sharedH    = nullptr;
static ID3D11Texture2D*   g_sharedTex  = nullptr;   // the same memory, D3D11 side
static IDirect3DQuery9*   g_sharedSync = nullptr;
static bool  g_fastCapture     = true;      // NUMPAD8 toggles
static bool  g_fastCaptureOK   = false;     // the shared pair exists and works
static bool  g_fastFailLogged  = false;

// ⚠️ ONE function decides where this frame's picture is, and every consumer asks it.
//
// The first version of rung 8b changed the copy in FillEye to read the shared texture and left
// the guard three lines above it testing g_upload - which the fast path never allocates. So
// FillEye returned false before reaching the copy, neither eye was ever filled, no projection
// layer was submitted, and the headset fell back to the rung 2 test quad. Everything upstream
// was working and reported so: the shared surface was live, stereo was armed, both swapchains
// existed.
//
// Two places deciding the same thing, one of them by proxy, is how that happens. The quad path
// had the same proxy guard.
static ID3D11Resource* FrameSource()
{
    if (g_fastCapture && g_devIsEx && g_fastCaptureOK) return (ID3D11Resource*)g_sharedTex;
    return (ID3D11Resource*)g_upload;
}

static void ReleaseSharedCapture()
{
    if (g_sharedSync) { g_sharedSync->Release(); g_sharedSync = nullptr; }
    if (g_sharedTex)  { g_sharedTex->Release();  g_sharedTex  = nullptr; }
    if (g_sharedRT)   { g_sharedRT->Release();   g_sharedRT   = nullptr; }
    g_sharedH = nullptr;
    g_fastCaptureOK = false;
}

static bool EnsureSharedCapture(IDirect3DDevice9* dev, UINT w, UINT h)
{
    if (g_fastCaptureOK && w == g_capW && h == g_capH) return true;
    ReleaseSharedCapture();
    if (!g_devIsEx || !g_dev11) return false;

    // pSharedHandle must point at a NULL handle going in; the runtime fills it. Lockable FALSE:
    // nothing ever reads this from the CPU, which is the entire point.
    g_sharedH = nullptr;
    HRESULT hr = dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                         FALSE, &g_sharedRT, &g_sharedH);
    if (FAILED(hr) || !g_sharedRT || !g_sharedH) {
        Log("[fast] CreateRenderTarget(shared) FAILED hr=0x%08lX handle=%p - staying on the slow path",
            (unsigned long)hr, (void*)g_sharedH);
        ReleaseSharedCapture();
        return false;
    }

    hr = g_dev11->OpenSharedResource(g_sharedH, __uuidof(ID3D11Texture2D), (void**)&g_sharedTex);
    if (FAILED(hr) || !g_sharedTex) {
        // ⚠️ The most likely cause is the two devices being on different adapters. The D3D11
        // device is created on the adapter the OpenXR runtime names; the D3D9 device is on
        // whichever adapter the game asked for. On a single-GPU machine they agree.
        Log("[fast] OpenSharedResource FAILED hr=0x%08lX - the D3D9 and D3D11 devices are"
            " probably on different adapters; staying on the slow path", (unsigned long)hr);
        ReleaseSharedCapture();
        return false;
    }

    // EVENT, not OCCLUSION. This one is ours and is never seen by the game's own query hook,
    // which filters on type for exactly this reason.
    if (FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_sharedSync))) g_sharedSync = nullptr;

    D3D11_TEXTURE2D_DESC td{};
    g_sharedTex->GetDesc(&td);
    Log("*** [fast] shared surface live: %ux%u, D3D11 sees DXGI format %d, sync query %s",
        td.Width, td.Height, (int)td.Format, g_sharedSync ? "yes" : "NO (expect tearing)");
    g_fastCaptureOK = true;
    return true;
}

// The fast path. Returns false to mean "fall back this frame", never to mean "give up".
static bool CaptureFrameShared(IDirect3DDevice9* dev, IDirect3DSurface9* bb, UINT w, UINT h)
{
    if (!EnsureSharedCapture(dev, w, h)) return false;

    HRESULT hr = dev->StretchRect(bb, nullptr, g_sharedRT, nullptr, D3DTEXF_NONE);
    if (FAILED(hr)) {
        if (!g_fastFailLogged) {
            g_fastFailLogged = true;
            Log("[fast] StretchRect FAILED hr=0x%08lX - falling back to the slow path",
                (unsigned long)hr);
        }
        return false;
    }

    // Wait for the blit, not for the frame. GetData spins until the queued work up to this point
    // has retired; without it D3D11 can sample a surface D3D9 is still writing.
    if (g_sharedSync) {
        g_sharedSync->Issue(D3DISSUE_END);
        while (g_sharedSync->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) { /* spin */ }
    }
    return true;
}

static bool CaptureFrame(IDirect3DDevice9* dev)
{
    if (!g_dev11) return false;
    const double t0 = NowMs();

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;

    D3DSURFACE_DESC d{};
    if (FAILED(bb->GetDesc(&d))) { bb->Release(); return false; }

    // The fast path first. It needs the backbuffer and nothing else, so the slow path's system
    // memory surface is not even allocated while it is working.
    if (g_fastCapture && g_devIsEx) {
        g_capW = d.Width; g_capH = d.Height;
        const bool ok = CaptureFrameShared(dev, bb, d.Width, d.Height);
        bb->Release();
        if (ok) {
            g_haveFrame = true;
            g_capMsTotal += NowMs() - t0;
            g_capSamples++;
            return true;
        }
        // Fell back. Re-take the backbuffer and continue into the slow path below rather than
        // dropping the frame - a fallback that skips frames is a worse failure than a slow one.
        bb = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    }

    if (!EnsureCapture(dev, d.Width, d.Height, d.Format)) { bb->Release(); return false; }

    HRESULT hr = dev->GetRenderTargetData(bb, g_sysSurf);
    bb->Release();
    if (FAILED(hr)) {
        if (!g_captureFailLogged) {
            g_captureFailLogged = true;
            Log("[cap] GetRenderTargetData FAILED hr=0x%08lX - quad falls back to the test colour",
                (unsigned long)hr);
        }
        return false;
    }

    D3DLOCKED_RECT lr{};
    if (FAILED(g_sysSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;

    D3D11_MAPPED_SUBRESOURCE ms{};
    hr = g_ctx11->Map(g_upload, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    if (FAILED(hr)) { g_sysSurf->UnlockRect(); return false; }

    // Row by row: the two pitches are independent and are usually NOT equal.
    const BYTE* src = (const BYTE*)lr.pBits;
    BYTE*       dst = (BYTE*)ms.pData;
    if (!src || !dst) {
        // Both calls reported success, so this is not expected - but memcpy from a null
        // source is an access violation inside the game's render thread, and a guard is
        // cheaper than the minidump.
        g_ctx11->Unmap(g_upload, 0);
        g_sysSurf->UnlockRect();
        if (!g_captureFailLogged) {
            g_captureFailLogged = true;
            Log("[cap] lock succeeded but a pointer was NULL (src=%p dst=%p) - skipping frame",
                (void*)src, (void*)dst);
        }
        return false;
    }
    const size_t rowBytes = (size_t)g_capW * 4;
    for (UINT y = 0; y < g_capH; ++y)
        memcpy(dst + (size_t)y * ms.RowPitch, src + (size_t)y * lr.Pitch, rowBytes);

    g_ctx11->Unmap(g_upload, 0);
    g_sysSurf->UnlockRect();

    g_capMsTotal += NowMs() - t0;
    g_capSamples++;
    return true;
}

static void ApplyHeadTracking(XrTime when);   // defined with the rung 5b code
static void ApplyMotionHandPosition(uintptr_t pawn, const P13PoseSnapshot& pose);
static void UpdateMotionRigDiscovery();       // P1.2 cache, refreshed outside the game hook

// ---- two measurements aimed at what is left of the judder ----
//
// The mean frame rate cannot distinguish the two remaining explanations, and they need opposite
// fixes, so each gets a number of its own.
//
// PACING: how many display periods each delivered frame actually spanned. One bucket means an
// even cadence. Several means a ragged one, which is what snapping looks like regardless of what
// the mean says - and the mean says the same thing either way.
//
// POSE HONESTY: the projection layer tells the compositor which pose the image was rendered
// from, and it is told the CURRENT head pose. But the image was rendered by the game, from the
// game's camera, which lags behind the head. So the claim is slightly false, by however much the
// engine has not caught up - and the compositor reprojects on the claim. Every display frame it
// synthesises is corrected toward a pose the image does not have. That produces motion the image
// never contained, worst while turning and absent while still, and no frame rate fixes it.
//
// Measured as a difference of CHANGES rather than of absolutes, because the head pose is in room
// space and the matrix is in world space and they have no common origin - but per frame, how far
// the head turned and how far the rendered image turned are directly comparable, and the gap
// between them is the size of the lie.
static void ReportPacing();
static void TickPacing();

// ---- do the two eyes agree, and does the roll get applied twice? ----
//
// Both eyes are rendered from ONE matrix with a lateral offset, so they carry identical
// orientation by construction. The compositor is told something different for each: it is handed
// g_views[0] and g_views[1] as submitted, orientation included. Anywhere those two disagree, the
// image says one thing and the layer says another, per eye - and disagreement between the eyes
// is not something the brain smooths over the way it does a lag. It fights it.
//
// Two ways that can happen, neither previously checked:
//
//   CANT. Some headsets angle their panels, and the runtime then reports two eye orientations
//   that genuinely differ. Rendering both from one matrix ignores that. The error is fixed in
//   size but it is a per-eye error, so it reads as the image refusing to fuse rather than as
//   something being in the wrong place.
//
//   DOUBLE ROLL. The engine zeroes ViewRotation.Roll, so the game's image has no roll, and
//   ApplyRoll puts the head's roll into the matrix. But the submitted pose ALSO carries the
//   head's roll, and the compositor orients the layer by it. If both are live the roll is
//   applied twice. That would grow with head tilt rather than head speed, so it has been
//   invisible in every test that swept the view and left the head level.
static void ReportStereoGeometry();

// ================================================================ rung 6c: per-eye stereo
//
// ALTERNATE-EYE, deliberately, as the reference project's ladder does it. One eye is rendered
// per frame and each eye's image is therefore one frame stale.
//
// Why that rather than true simultaneous stereo: the engine renders the scene once per frame,
// so two genuinely different images need either draw-call duplication or engine re-entry -
// both large, and both able to fail in ways that look like a geometry bug. Alternating proves
// the per-eye offset, the two swapchains, the projection layer and the FOV maths first, and
// costs no extra frame grab: still one backbuffer per frame.
//
// ---- the offset comes from the matrix itself ----
//
// Per-eye parallax is the rung 6b injection at +/- half IPD along the camera's RIGHT axis. That
// axis is read from the incoming matrix in the same call that modifies it - for a row-vector
// world->clip matrix, column 0 is the direction mapping to clip.x, so normalising it gives
// world-space right. Recomputing it from the pawn's rotation would introduce a second source
// that can disagree with the thing being modified; this one cannot.
//
// ---- world scale is a guess and is meant to be tuned ----
//
// OpenXR reports IPD in metres. UE3 units per metre is game-specific: the Singularity project
// measured 3.32 UU for 6.3 cm, about 1.9 cm per unit. Mirror's Edge is unmeasured, so the
// scale starts at 50 UU/m and F11 adjusts it. Too small reads as a flat cardboard cut-out;
// too large as a miniature world.

static XrSwapchain       g_eyeSwap[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
static ID3D11Texture2D** g_eyeImages[2] = { nullptr, nullptr };
static uint32_t          g_eyeImageCount[2] = { 0, 0 };
static uint32_t          g_eyeW = 0, g_eyeH = 0;
static bool              g_eyeFilled[2] = { false, false };
static int               g_nextEye = 0;          // which eye the NEXT frame will render
static int               g_renderedEye = -1;     // which eye the frame just finished IS
int                      g_stereoMode = 0;       // 0 = mono quad, 1 = alternate-eye
// 100 UU/m, judged in the headset rather than derived. F11 swept 25/35/50/70/100/140 and 100
// was the one that read as life-sized. At a 6.3 cm IPD that is a half-offset of ~3.15 UU, close
// to the 3.32 UU the Singularity project measured for the same separation - two different games
// arriving at a similar world scale, which is reassuring but was not the reason for choosing it.
float                    g_worldScale = 100.0f;  // UE3 units per metre - MEASURED, not tuned
// Comfort preference, separate from world scale. Now defaults to 100% - the true 6.3 cm IPD at
// the measured 100 UU/m - because simultaneous stereo removed the doubling that made 50%
// preferable under alternate-eye. Reported after that change: no doubling at ANY setting, which
// is the confirmation that the doubling was temporal disparity rather than separation.
float                    g_stereoStrength = 1.00f;
// Defined with the injection code further down; declared here because the eye is chosen at the
// end of each frame, which happens above it.
extern float             g_eyeInject;
// Rung 7 (draw duplication) likewise lives below but is consulted by the frame path here.
extern bool              g_simulStereo;
extern bool              g_sceneMatValid;
extern bool              g_c0IsScene;
extern bool              g_rtIsScene;
extern int               g_dupOnlyTarget;
extern bool              g_forceVisible;
extern int               g_occlusionMode;
struct RtSeen { IDirect3DSurface9* surf; UINT w, h; D3DFORMAT fmt; long draws; long sceneDraws; };
extern RtSeen            g_rtSeen[16];
extern int               g_rtSeenCount;

// ---- where the engine actually renders the scene, which is not always the backbuffer ----
//
// 0 means "not learned, use the backbuffer", and that is the state every working run has been
// in: at 2560x1440 the scene target and the backbuffer are the same size, so the distinction
// never arose. It arises at 1600x1200 - a 4:3 mode - where the engine renders the scene 16:9
// into a 1600x900 target and letterboxes it. The backbuffer-sized surface then takes ZERO
// draws, g_rtIsScene is false for the target that matters, and duplication never runs.
//
// The name for this is already in the history: "the backbuffer is not what the engine renders
// at - rung 9's premise was wrong". Same premise, a different consequence.
UINT                     g_sceneW = 0, g_sceneH = 0;
// The game has moved the world into a scene buffer whose size cannot carry side-by-side
// stereo (run 14: 1280x720 against a 2560x1440 backbuffer). Duplicating the backbuffer's
// leftover draws in that mode smears the right eye, and adopting the mono buffer fights the
// game's own full-width draws - the only coherent presentation is mono until the mode ends.
bool                     g_sceneSplitMono = false;
// Per-frame scene-draw tallies for the split-flip detector, reset every Present.
long                     g_frameSceneOnBackbuffer = 0;
long                     g_frameSceneOffscreen = 0;
// Set by the HOME-key user marker; while positive, the arm-continuity watchdog logs every
// update unconditionally and decrements it. Render thread sets, game thread consumes.
volatile LONG            g_markerBurst = 0;

// The scene's rectangle within the captured backbuffer. Every one of these collapses to the
// whole backbuffer while g_sceneW/H are unset or equal to it, so every use below is an identity
// transform in the configurations that work today - which is the property that makes this safe
// to thread through the eye path at all.
//
// ⚠️ The offset ASSUMES the letterbox is centred. That is what UE3 does and what a 1600x900
// scene in a 1600x1200 frame looks like, but it is an assumption, not a measurement. If a mode
// turns up that pillarboxes to one side, this is the line that will be wrong.
static inline UINT SceneW() { return g_sceneW ? g_sceneW : g_capW; }
static inline UINT SceneH() { return g_sceneH ? g_sceneH : g_capH; }
static inline UINT SceneX() { return (g_capW > SceneW()) ? (g_capW - SceneW()) / 2 : 0; }
static inline UINT SceneY() { return (g_capH > SceneH()) ? (g_capH - SceneH()) / 2 : 0; }
extern float             g_sceneMat[16];
extern volatile LONG     g_dupDraws;
// Duplicated draws in THIS frame, as opposed to g_dupDraws which is a running total for the
// occlusion override. Reset every frame at the submit, so it answers one question: does the
// frame about to be presented actually contain two eyes side by side?
extern volatile LONG     g_dupFrameDraws;
float                    g_halfIpdUU = 0.0f;     // filled from the located views
float                    g_gameHalfFovX = 0.0f;  // read out of the view matrix, radians
float                    g_gameHalfFovY = 0.0f;
bool                     g_gameFovValid = false;
float                    g_headRoll = 0.0f;      // radians, sampled once per frame
// The pitch the view SHOULD have, in radians, UE3 sense (positive is up). Set once per frame
// beside the head sample; the render thread corrects the matrix against it. See ApplyPitchFix.
float                    g_pitchTarget = 0.0f;
bool                     g_pitchTargetValid = false;
bool                     g_pitchFix = true;      // NUMPAD3 toggles, for the A/B
// The controller's own pitch, sampled INSIDE the frame rather than in Present. See the read site.
float                    g_liveCtlPitch = 0.0f;
float                    g_liveCtlYaw   = 0.0f;
bool                     g_liveCtlValid = false;
long                     g_liveCtlFrame = -1;
// The camera position used as the pivot for every matrix rotation, read inside the frame. See
// the read site for why the Present-sampled one was not good enough.
float                    g_livePivot[3] = { 0, 0, 0 };
bool                     g_livePivotValid = false;
// Published by the XR frame loop for the render thread to ask the runtime about.
XrTime                   g_predTime   = 0;
XrDuration               g_predPeriod = 0;
float                    g_yawLagRad  = 0.0f;    // how far the view is behind the head
bool                     g_yawLagFix  = true;    // NUMPAD6
// Yaw we wrote ourselves since the render thread last looked. Anything the controller's yaw
// moved BEYOND this came from somewhere else - the mouse, the stick, a scripted turn - and is
// not lag. See the correction for why telling them apart matters.
float                    g_writtenYawAccum = 0.0f;
bool                     g_rollEnabled = true;   // HOME toggles
int                      g_rollSign = 1;         // END flips
static float             g_lastLoggedFovX = -1.0f;
float                    g_targetHalfFovX = 0.0f;   // forced frustum, radians
float                    g_targetHalfFovY = 0.0f;
bool                     g_fovForce = true;         // F8 toggles
static bool              g_fovLogged = false;
static long              g_fovWrites = 0;
float                    g_camCache[3] = { 0, 0, 0 };
float                    g_camFwd[3]   = { 1, 0, 0 };   // where that camera is LOOKING
bool                     g_camCacheValid = false;
volatile LONG            g_vmAccepted = 0;
volatile LONG            g_vmRejected = 0;
bool                     g_vmValidate = true;   // F12 turns the per-upload filter off
static bool EnsureEyeSwapchains(uint32_t w, uint32_t h)
{
    if (g_eyeSwap[0] != XR_NULL_HANDLE && w == g_eyeW && h == g_eyeH) return true;
    for (int e = 0; e < 2; ++e) {
        if (g_eyeSwap[e] != XR_NULL_HANDLE) { xrDestroySwapchain(g_eyeSwap[e]); g_eyeSwap[e] = XR_NULL_HANDLE; }
        delete[] g_eyeImages[e]; g_eyeImages[e] = nullptr;
        g_eyeFilled[e] = false;
    }
    if (!g_scFormat) return false;               // format chosen by the mono path already

    for (int e = 0; e < 2; ++e) {
        XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format      = g_scFormat;
        sci.sampleCount = 1;
        sci.width = w; sci.height = h;
        sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(g_xrSession, &sci, &g_eyeSwap[e]))) {
            Log("[eye] xrCreateSwapchain failed for eye %d", e); return false;
        }
        xrEnumerateSwapchainImages(g_eyeSwap[e], 0, &g_eyeImageCount[e], nullptr);
        XrSwapchainImageD3D11KHR* imgs = new XrSwapchainImageD3D11KHR[g_eyeImageCount[e]];
        for (uint32_t i = 0; i < g_eyeImageCount[e]; ++i) imgs[i] = { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR };
        xrEnumerateSwapchainImages(g_eyeSwap[e], g_eyeImageCount[e], &g_eyeImageCount[e],
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs));
        g_eyeImages[e] = new ID3D11Texture2D*[g_eyeImageCount[e]];
        for (uint32_t i = 0; i < g_eyeImageCount[e]; ++i) g_eyeImages[e][i] = imgs[i].texture;
        delete[] imgs;
    }
    g_eyeW = w; g_eyeH = h;
    Log("[eye] two swapchains %ux%u, %u images each", w, h, g_eyeImageCount[0]);
    return true;
}

// Copy the captured frame into one eye's swapchain.
static bool FillEye(int eye)
{
    // Asked once, up front, and the same answer is used by the guard and by the copy. Guarding
    // on one texture and copying from another is what put the test quad in the headset.
    ID3D11Resource* src = FrameSource();
    if (eye < 0 || eye > 1 || g_eyeSwap[eye] == XR_NULL_HANDLE || !src) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage(g_eyeSwap[eye], &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    bool ok = false;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(g_eyeSwap[eye], &wi))) {
        if (g_simulStereo) {
            // Both eyes live in one frame side by side, so each swapchain takes its own half -
            // of the SCENE rectangle, which is the whole frame unless the engine letterboxed it.
            // Cutting the full backbuffer instead would hand the compositor the black bars.
            D3D11_BOX box{};
            box.left  = SceneX() + (UINT)(eye == 0 ? 0 : SceneW() / 2);
            box.right = box.left + SceneW() / 2;
            box.top = SceneY(); box.bottom = SceneY() + SceneH();
            box.front = 0; box.back = 1;
            g_ctx11->CopySubresourceRegion(g_eyeImages[eye][idx], 0, 0, 0, 0, src, 0, &box);
        } else {
            g_ctx11->CopyResource(g_eyeImages[eye][idx], src);
        }
        ok = true;
    }
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(g_eyeSwap[eye], &ri);
    if (ok) g_eyeFilled[eye] = true;
    return ok;
}

// One XR frame: wait, begin, put the captured frame (or the test colour) on the quad, submit.
static void SubmitTestQuad()
{
    if (!g_xrRunning) return;

    XrFrameState fs{ XR_TYPE_FRAME_STATE };
    if (XR_FAILED(xrWaitFrame(g_xrSession, nullptr, &fs))) return;
    xrBeginFrame(g_xrSession, nullptr);

    long n = InterlockedIncrement(&g_xrFrames);
    bool submitted = false;
    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };

    // Sized to the captured frame once there is one, so the game's pixels map 1:1 into the
    // swapchain and no scaling happens on this path. Falls back to a square while there is
    // nothing to show.
    const uint32_t wantW = g_haveFrame ? g_capW : 1024;
    const uint32_t wantH = g_haveFrame ? g_capH : 1024;

    if (fs.shouldRender && EnsureSwapchain(wantW, wantH)) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_swapchain, &ai, &idx))) {
            XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrWaitSwapchainImage(g_swapchain, &wi))) {
              ID3D11Resource* qsrc = FrameSource();
              if (g_haveFrame && qsrc) {
                // Legal despite the swapchain texture being TYPELESS: B8G8R8A8_UNORM and
                // B8G8R8A8_TYPELESS share a type group, so CopyResource is a raw bit copy.
                g_ctx11->CopyResource(g_scImages[idx], qsrc);
                submitted = true;
              } else {
                // The colour CYCLES so a live loop cannot be mistaken for one frozen frame -
                // see the note at the top of this file. Roughly a three-second period.
                float t = (float)(n % 180) / 180.0f * 6.2831853f;
                FLOAT rgba[4] = { 0.5f + 0.5f * sinf(t),
                                  0.5f + 0.5f * sinf(t + 2.0944f),
                                  0.5f + 0.5f * sinf(t + 4.1888f),
                                  1.0f };
                // The view format MUST be named explicitly. Runtimes commonly allocate the
                // swapchain texture as *_TYPELESS so it can be viewed as either sRGB or
                // UNORM, and a null description means "use the resource's own format" -
                // which is invalid for a typeless resource, and is why this failed on VDXR
                // with the quad submitting successfully every frame while staying empty.
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format        = (DXGI_FORMAT)g_scFormat;
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

                ID3D11RenderTargetView* rtv = nullptr;
                HRESULT rvhr = g_dev11->CreateRenderTargetView(g_scImages[idx], &rd, &rtv);
                if (SUCCEEDED(rvhr)) {
                    g_ctx11->ClearRenderTargetView(rtv, rgba);
                    rtv->Release();
                    submitted = true;
                } else if (!g_rtvFailLogged) {
                    // Reported on FIRST OCCURRENCE, not on frame 1. The swapchain is only
                    // created once shouldRender is true, which may be several frames in - so
                    // a frame-1 guard can miss the failure entirely and report nothing at
                    // all. That is exactly what happened in the 22:27 run.
                    g_rtvFailLogged = true;
                    Log("[xr] CreateRenderTargetView FAILED hr=0x%08lX (view format %lld) - quad blank",
                        (unsigned long)rvhr, (long long)g_scFormat);
                }
              }
            }
            XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(g_swapchain, &ri);
        }
    }

    // ---- ⚠️ A MONO FRAME MUST NOT BE SUBMITTED AS STEREO ----
    //
    // Stereo mode being ON says what we INTEND. It does not say the frame in hand actually has
    // two eyes in it. Duplication is gated per draw - on the scene matrix being live, on the
    // render target, on the pawn's pose being readable - and every one of those legitimately
    // goes false: a menu, a cutscene camera, the seconds after a death.
    //
    // When it does, the engine renders one ordinary picture and this code cut it down the middle
    // and gave half to each eye. That is the "it looks wrong once I go back to the menu": not a
    // stereo image at all, just the left half of the menu in the left eye and the right half in
    // the right. Before any of this arms, the same menu goes out through the mono quad and looks
    // like a screen floating in front of you - which is correct, and is what should happen
    // whenever the frame is mono, not only before the first arm.
    //
    // So the frame is asked, not the mode. The layer choice below already falls back to the quad;
    // it simply never got the chance.
    //
    // Held for a few frames rather than switched instantly: duplication can miss a frame at a
    // transition, and alternating layer types frame to frame would be far worse than either. Going
    // back to stereo is immediate, because the first duplicated frame is unambiguous.
    const LONG dupThisFrame = InterlockedExchange(&g_dupFrameDraws, 0);
    static int monoRun = 0;
    if (dupThisFrame >= 4) monoRun = 0; else if (monoRun < 1000) ++monoRun;
    const bool frameIsStereo = (monoRun < 10);
    {
        static bool wasStereo = false;
        if (frameIsStereo != wasStereo) {
            wasStereo = frameIsStereo;
            Log("[eye] frame is %s - presenting the %s",
                frameIsStereo ? "side-by-side" : "MONO (nothing duplicated)",
                frameIsStereo ? "stereo projection" : "head-locked quad");
        }
    }

    // ---- stereo: locate the eyes, fill the one this frame rendered, submit a projection ----
    XrCompositionLayerProjection      proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView  projViews[2]{};
    bool stereoSubmitted = false;
    // Validity is per frame, not a lifetime latch: a frame that skips stereo view location must
    // not leave last frame's eye poses looking current to later consumers.
    g_viewsValid = false;

    if (g_stereoMode == 1 && g_haveFrame) {
        uint32_t viewCount = 0;
        XrViewState vs{ XR_TYPE_VIEW_STATE };
        XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_xrSpace;
        for (int e = 0; e < 2; ++e) g_views[e] = { XR_TYPE_VIEW };
        if (XR_SUCCEEDED(xrLocateViews(g_xrSession, &vli, &vs, 2, &viewCount, g_views)) &&
            viewCount == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            g_viewsValid = true;
            // Half the real interpupillary distance, in metres, straight from the runtime -
            // then into UE3 units by the tunable world scale.
            const float dx = g_views[1].pose.position.x - g_views[0].pose.position.x;
            const float dy = g_views[1].pose.position.y - g_views[0].pose.position.y;
            const float dz = g_views[1].pose.position.z - g_views[0].pose.position.z;
            g_halfIpdUU = 0.5f * sqrtf(dx*dx + dy*dy + dz*dz) * g_worldScale;

            // ---- the target frustum: match the headset's VERTICAL, derive the horizontal ----
            //
            // Vertical, not horizontal, and the reference is explicit about why: the game
            // renders 16:9 while a per-eye view is nearly square, so equal VERTICAL leaves the
            // horizontal comfortably wider than needed - both axes covered, surplus falling
            // outside the eye. Matching horizontally instead leaves top and bottom short, which
            // is the visible half of the aspect mismatch and exactly the letterboxing seen now.
            float up = 0.0f, dn = 0.0f;
            for (int e = 0; e < 2; ++e) {
                if (g_views[e].fov.angleUp   >  up) up = g_views[e].fov.angleUp;
                if (-g_views[e].fov.angleDown > dn) dn = -g_views[e].fov.angleDown;
            }
            const float halfY = (up > dn) ? up : dn;
            if (halfY > 0.1f && SceneH() > 0) {
                g_targetHalfFovY = halfY;
                // Square pixels: the horizontal follows from the aspect of what ONE EYE
                // actually renders into. Under draw duplication that is half the SCENE, so the
                // aspect halves - and this single value is used both to force the matrix and to
                // tell the compositor, which is the only way they can agree. Taking it from the
                // backbuffer instead would describe the letterbox rather than the picture.
                const float aspect = g_simulStereo ? ((float)SceneW() * 0.5f / (float)SceneH())
                                                   : ((float)SceneW() / (float)SceneH());
                g_targetHalfFovX = atanf(tanf(halfY) * aspect);
                if (!g_fovLogged) {
                    g_fovLogged = true;
                    Log("[fov] headset wants %.1f deg vertical; targeting %.1f x %.1f at %.2f aspect",
                        halfY * 114.5916f, g_targetHalfFovX * 114.5916f,
                        g_targetHalfFovY * 114.5916f, aspect);
                }
            }
        }

        // Sized to the region FillEye actually copies, or the copy and the swapchain disagree.
        const uint32_t eyeW = g_simulStereo ? (SceneW() / 2) : SceneW();
        if (g_viewsValid && EnsureEyeSwapchains(eyeW, SceneH())) {
            // Simultaneous: both halves are from THIS frame, so both eyes fill every frame.
            if (g_simulStereo) { FillEye(0); FillEye(1); }
            else if (g_renderedEye >= 0) FillEye(g_renderedEye);
            if (g_eyeFilled[0] && g_eyeFilled[1]) {
                for (int e = 0; e < 2; ++e) {
                    projViews[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                    projViews[e].pose = g_views[e].pose;
                    // Submit the frustum the game ACTUALLY rendered, not the headset's.
                    // Symmetric, because that is what the engine produces. The image will not
                    // fill the eye - the game renders 16:9 and an eye is nearly square - and a
                    // correct rectangle inside black is the honest result. Widening the game's
                    // own FOV to cover the eye is the next step, and FOVAngle is already
                    // located at TdPlayerController +0x030C for it.
                    // ⚠️ When forcing, submit the FORCED constant - never the observed value.
                    //
                    // The reference project followed the observation and that WAS the flicker:
                    // a submitted frustum that tracks the engine inherits every wobble the
                    // engine's own interpolation produces. Forced and submitted from one
                    // number, the two agree by construction and there is nothing left to drift.
                    // ⚠️ g_gameFovValid is in this condition because the RENDER side cannot force
                    // without it - it needs the game's own tangents to compute the scale. Only
                    // the submit side tested for it, so when the flag was false the layer
                    // declared the forced frustum for an image rendered at the engine's. The two
                    // conditions must be the same condition; that they were merely similar is
                    // what let them come apart.
                    if (g_fovForce && g_gameFovValid && g_targetHalfFovX > 0.0f) {
                        projViews[e].fov.angleLeft  = -g_targetHalfFovX;
                        projViews[e].fov.angleRight =  g_targetHalfFovX;
                        projViews[e].fov.angleUp    =  g_targetHalfFovY;
                        projViews[e].fov.angleDown  = -g_targetHalfFovY;
                    } else if (g_gameFovValid) {
                        projViews[e].fov.angleLeft  = -g_gameHalfFovX;
                        projViews[e].fov.angleRight =  g_gameHalfFovX;
                        projViews[e].fov.angleUp    =  g_gameHalfFovY;
                        projViews[e].fov.angleDown  = -g_gameHalfFovY;
                    } else {
                        projViews[e].fov = g_views[e].fov;
                    }
                    projViews[e].subImage.swapchain = g_eyeSwap[e];
                    projViews[e].subImage.imageRect = { {0,0}, {(int32_t)g_eyeW, (int32_t)g_eyeH} };
                }
                proj.space     = g_xrSpace;
                proj.viewCount = 2;
                proj.views     = projViews;
                // ⚠️ THE ONLY THING THE MONO TEST MAY GATE IS THE LAYER CHOICE.
                //
                // It gated the whole block above for one build, and that DEADLOCKED: g_halfIpdUU
                // is computed in here from the located views, ShouldDuplicate requires it to be
                // non-zero, so no duplication meant no IPD meant duplication could never start.
                // Ten frames after launch it latched and the run was mono for good, while every
                // other signal - the scan, the register, 100% acceptance - looked perfect.
                //
                // Locating the views, deriving the IPD and filling the eyes are what ARM stereo.
                // Whether this frame's picture happens to be side by side decides only which
                // layer is handed to the compositor, and it must be asked here, at the end.
                stereoSubmitted = (frameIsStereo || !g_simulStereo);
            }
        }
    }

    const XrCompositionLayerBaseHeader* layers[1];
    uint32_t layerCount = 0;
    if (stereoSubmitted) {
        layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj);
        layerCount = 1;
    } else if (submitted) {
        // ---- head-locked, not world-locked ----
        //
        // The quad used to sit in LOCAL space, so it stayed put while the head turned - and
        // once head tracking landed that became actively wrong: the game camera turned with
        // the head while the screen slid out of view. In VIEW space the quad rides the head,
        // so the image the game just rendered from the head's orientation is always in front
        // of it. That is the reference project's "MonoTracked" rung.
        quad.space      = (g_viewSpace != XR_NULL_HANDLE) ? g_viewSpace : g_xrSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_swapchain;
        quad.subImage.imageRect = { {0, 0}, {(int32_t)g_scW, (int32_t)g_scH} };
        quad.pose.orientation.w = 1.0f;
        quad.pose.position      = { 0.0f, 0.0f, -2.0f };   // 2 m in front, LOCAL space
        // Height follows the frame's aspect, so a 16:9 image is not stretched into a square.
        const float aspect = (g_scH > 0) ? ((float)g_scW / (float)g_scH) : 1.0f;
        quad.size               = { 2.0f, 2.0f / aspect };
        layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
        layerCount = 1;
    }

    // Head tracking uses the SAME predicted display time the frame is submitted for, so the
    // pose the game renders from and the pose the compositor expects agree.
    ApplyHeadTracking(fs.predictedDisplayTime);
    // Once a frame, beside the head sample. Both read the same runtime and both describe the same
    // instant, so keeping them together is what stops the sticks from lagging the view.
    const bool actionsSynced = XrSyncInput();
    XrSampleMotionPoses(fs.predictedDisplayTime, actionsSynced);
    // The first position test proved that a write made here is reset by Update1pArms before the
    // skeleton evaluates. Publish instead; the native arm hook consumes this exact sample after
    // the game's own arm update on the next tick.
    PublishP13PoseSnapshot();
    UpdateMotionRigDiscovery();
    TickPacing();
    ReportStereoGeometry();
    // ReportPoseHonesty is gone. It compared the CHANGE in head yaw against the change in the
    // image's yaw, which a constant lag leaves identical - so it read zero through the entire
    // period when the view was sitting five degrees behind, and that zero was taken as an
    // all-clear. The invariant measurement in the constant hook answers the same question
    // without the blind spot, and reports the offset itself rather than its derivative.

    // Published for the render thread, which needs a time to ask the runtime about and has no
    // frame state of its own. The period comes with it because the correction is expressed as
    // "how much further will the head have turned one frame from now".
    g_predTime   = fs.predictedDisplayTime;
    g_predPeriod = fs.predictedDisplayPeriod;

    XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
    fei.displayTime          = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount           = layerCount;
    fei.layers               = layerCount ? layers : nullptr;
    XrResult er = xrEndFrame(g_xrSession, &fei);

    // ---- choose the eye the NEXT frame will render ----
    //
    // The offset has to be in place before the engine draws, and the only place we run before
    // that is here, at the end of the previous frame. So this Present decides what the next
    // frame is, and the Present after it captures the result and assigns it to that eye.
    // g_renderedEye is what the frame just captured actually was.
    if (g_stereoMode == 1 && !g_simulStereo) {
        g_renderedEye = g_nextEye;
        g_nextEye = 1 - g_nextEye;
        g_eyeInject = (g_nextEye == 0) ? -1.0f : +1.0f;   // eye 0 = left
    } else {
        g_renderedEye = -1;
        g_eyeInject = 0.0f;
    }

    if (n == 1)        Log("*** [xr] first XR frame submitted, xrEndFrame -> %d", (int)er);
    else if (n % 600 == 0) {
        // The capture cost. It was the number that decided whether the D3D9Ex work was worth
        // taking on - it said yes, and rung 8 took it from 3.7-4.6 ms to 0.4. Still reported,
        // because it is now the fastest way to tell which grab path a run is actually using: a
        // number near 4 means the fast path declined and the fallback is carrying the frame.
        //
        // A mean over the window, and reset, so it tracks the current scene rather than being
        // flattened by the menu at startup.
        const double mean = g_capSamples ? (g_capMsTotal / g_capSamples) : 0.0;
        // ---- the delivered rate, and what the headset wanted ----
        //
        // Asked directly, because it decides whether any of the remaining judder is ours to fix.
        // A game delivering fewer frames than the display asks for judders no matter how
        // perfectly the poses are computed: the compositor has to show something on the frames
        // that did not arrive, and an uneven ratio - 62 into 90 - never lands the same way twice.
        // That is a different problem from a late pose, and no amount of matrix correction
        // touches it.
        //
        // predictedDisplayPeriod is the runtime's own answer for the second number, rather than
        // an assumption about the headset.
        static double lastMs = 0.0;
        const double nowMs = NowMs();
        if (lastMs > 0.0) {
            const double fps = 600000.0 / (nowMs - lastMs);
            const double hz  = g_predPeriod ? (1.0e9 / (double)g_predPeriod) : 0.0;
            Log("[xr] frame %ld  state=%d shouldRender=%d endFrame=%d  |  frame grab %.2f ms mean"
                " over %ld  |  %.1f fps delivered, headset wants %.1f Hz",
                n, (int)g_xrState, (int)fs.shouldRender, (int)er, mean, g_capSamples, fps, hz);
            // ---- the CADENCE, which the mean hides completely ----
            //
            // A mean of 62 is the same number whether every frame took 16.1 ms or half took 8
            // and half took 24. The eye cannot tell the difference between 60 and 62 fps; it can
            // absolutely tell the difference between an even cadence and a ragged one, and a
            // ragged one is what snapping looks like.
            //
            // So this counts how many DISPLAY periods each delivered frame ended up spanning.
            // All in one bucket means the cadence is even and the remaining judder is not
            // delivery. Spread across buckets means it is, whatever the mean says.
            ReportPacing();
        } else {
            Log("[xr] frame %ld  state=%d shouldRender=%d endFrame=%d  |  frame grab %.2f ms mean over %ld",
                n, (int)g_xrState, (int)fs.shouldRender, (int)er, mean, g_capSamples);
        }
        lastMs = nowMs;
        g_capMsTotal = 0.0; g_capSamples = 0;
    }
}

// ================================================================ rung 4a: the object model
//
// Locates GNames and GObjects and derives the UObject layout for UE3 536, at runtime, by
// scanning and validating rather than by static analysis.
//
// ---- why scan instead of using Ghidra ----
//
// The Singularity project found these by decompiling FName::FName, following it to the
// interning function, and picking out the writable globals it touched - then had to confirm
// the guess against a live process anyway. The scan skips the first half and keeps the second,
// and it produces an answer that is self-validating rather than inferred.
//
// It is only viable because there is a test that cannot pass by accident: the RUNTIME GNames
// array always begins None, ByteProperty, IntProperty, because the engine interns its own type
// names before anything else. Three exact strings in order, at an address arrived at by
// arithmetic, is not something random data does.
//
// ⚠️ Note that this is the RUNTIME array. A cooked package's name table is a different thing
// in a different order - assuming otherwise already produced one false failure when verifying
// the decompressor, and the same mistake here would be much harder to spot.
//
// ---- everything is read through ReadProcessMemory ----
//
// Scanning walks addresses that are not guaranteed mapped. ReadProcessMemory returns false on
// an unmapped page instead of raising, so a wrong guess costs a failed read rather than
// killing the game's render thread.

static uintptr_t g_imgBase = 0, g_textLo = 0, g_textHi = 0, g_dataLo = 0, g_dataHi = 0;
static uintptr_t g_imgLo = 0, g_imgHi = 0;

// A UObject's vtable points into the GAME MODULE, not specifically into .text.
//
// The first GObjects scan tested `.text` only, and MSVC emits vtables into .rdata - which in
// this image sits ABOVE .text, so every real object was rejected. It reported 20/64 on a
// wxWidgets RTTI table, which is noise that happened to hold a few code-shaped pointers.
//
// Testing the whole image is also the more honest question: "does this dword point at
// something in the executable" is what actually distinguishes an object array.
static bool InModule(uint32_t a) { return a >= g_imgLo && a < g_imgHi; }
static uintptr_t g_gnamesAddr = 0;
static uint32_t  g_nameTextOff = 0;
static bool      g_nameWide = false;      // offset of the char data inside FNameEntry
static uintptr_t g_gobjAddr = 0;
static int       g_offName = -1;
static bool      g_objModelDone = false;
static volatile LONG g_objModelThreadFinished = 0;

static bool SafeRead(uintptr_t addr, void* out, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
}

static bool SafeU32(uintptr_t addr, uint32_t* out) { return SafeRead(addr, out, 4); }

// Reads an ANSI string, refusing anything unprintable. The refusal is the point: it is what
// stops a random dword that happens to look like a pointer from scoring as a name.
static bool SafeAnsi(uintptr_t addr, char* out, size_t cap)
{
    size_t i = 0;
    for (; i + 1 < cap; ++i) {
        char c;
        if (!SafeRead(addr + i, &c, 1)) return false;
        if (c == 0) break;
        if ((unsigned char)c < 32 || (unsigned char)c > 126) return false;
        out[i] = c;
    }
    out[i] = 0;
    return i > 0;
}

static void FindSections()
{
    g_imgBase = (uintptr_t)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)g_imgBase;
    auto* nt  = (IMAGE_NT_HEADERS*)(g_imgBase + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = {};
        memcpy(name, sec[i].Name, 8);
        uintptr_t lo = g_imgBase + sec[i].VirtualAddress;
        uintptr_t hi = lo + sec[i].Misc.VirtualSize;
        if (!strcmp(name, ".text")) { g_textLo = lo; g_textHi = hi; }
        if (!strcmp(name, ".data")) { g_dataLo = lo; g_dataHi = hi; }
    }
    g_imgLo = g_imgBase;
    g_imgHi = g_imgBase + nt->OptionalHeader.SizeOfImage;
    Log("[obj] image %p-%p  .text %p-%p  .data %p-%p", (void*)g_imgLo, (void*)g_imgHi, (void*)g_textLo, (void*)g_textHi, (void*)g_dataLo, (void*)g_dataHi);
}

// Does the FNameEntry at `entry` hold `expect` when its text starts at `textOff`?
static bool NameEntryIs(uintptr_t entry, uint32_t textOff, const char* expect)
{
    char buf[128];
    if (!SafeAnsi(entry + textOff, buf, sizeof(buf))) return false;
    return strcmp(buf, expect) == 0;
}

// The UTF-16 form. Mirror's Edge stores name text WIDE, which is what defeated the first two
// scans - one assumed ANSI outright, the other only tried wide if the ANSI search found
// nothing at all, and it always found something.
static bool NameEntryIsW(uintptr_t entry, uint32_t textOff, const char* expect)
{
    for (size_t i = 0; ; ++i) {
        uint16_t ch;
        if (!SafeRead(entry + textOff + i * 2, &ch, 2)) return false;
        const char e = expect[i];
        if (e == 0) return ch == 0;
        if (ch != (uint16_t)(unsigned char)e) return false;
        if (i > 64) return false;
    }
}

// ---- find a literal string anywhere in committed, writable memory ----
//
// The first scan assumed the FNameEntry text offset was one of five values and that the text
// was ANSI. It found nothing, which says one of those assumptions is wrong but not which.
// Anchoring on the STRING instead assumes neither: wherever "ByteProperty" physically is, the
// entry containing it is a fixed distance below, and something points at that entry.
//
// ⚠️ ONE pass for ALL patterns. The previous version took a single pattern and was called four
// times, walking 1.1 GB each time - 22.8 seconds in total, which the player felt as a stalled
// level load. Memory bandwidth is the cost here, not comparisons, so the number of passes is
// the only thing that matters.
//
// ⚠️ Both encodings are ALWAYS searched. The previous version tried UTF-16 only when the ANSI
// search returned zero hits, which sounded reasonable and was wrong: three ANSI hits turned up
// from package name tables and from this DLL's own string literals, so the wide search - the
// one that might have found the real entries - never ran.
struct ScanPat { const void* data; size_t len; const char* label; };

static const uintptr_t kMaxHitsPerPat = 24;

static void ScanMemoryForPatterns(const ScanPat* pats, int npats,
                                  std::vector<uintptr_t>* out, size_t* scannedOut)
{
    size_t scanned = 0;
    std::vector<uint8_t> buf;

    // Our own module is excluded. Every literal searched for is also compiled into this DLL,
    // so including it guarantees a self-hit that looks like a real find - one already showed up
    // surrounded by our own printf format strings.
    MODULEINFO mi{};
    uintptr_t selfLo = 0, selfHi = 0;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ScanMemoryForPatterns, &self);
    if (self && GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi))) {
        selfLo = (uintptr_t)mi.lpBaseOfDll;
        selfHi = selfLo + mi.SizeOfImage;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t addr = 0x10000; addr < 0x7FFF0000; ) {
        if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) break;
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        uintptr_t next = base + mbi.RegionSize;
        if (next <= addr) break;

        const bool readable = (mbi.State == MEM_COMMIT) &&
                              !(mbi.Protect & PAGE_GUARD) &&
                              (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY |
                                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
        const bool isSelf = (selfHi > selfLo) && (base < selfHi) && (next > selfLo);

        if (readable && !isSelf && mbi.RegionSize <= (256u << 20)) {
            buf.resize(mbi.RegionSize);
            SIZE_T got = 0;
            if (ReadProcessMemory(GetCurrentProcess(), mbi.BaseAddress, buf.data(),
                                  mbi.RegionSize, &got) && got > 0) {
                scanned += got;
                for (int p = 0; p < npats; ++p) {
                    if (out[p].size() >= kMaxHitsPerPat) continue;
                    const size_t L = pats[p].len;
                    if (got < L) continue;
                    const uint8_t first = *(const uint8_t*)pats[p].data;
                    for (size_t i = 0; i + L <= got; ++i) {
                        if (buf[i] != first) continue;                  // cheap reject
                        if (memcmp(buf.data() + i, pats[p].data, L) != 0) continue;
                        out[p].push_back(base + i);
                        if (out[p].size() >= kMaxHitsPerPat) break;
                    }
                }
            }
        }
        addr = next;
    }
    if (scannedOut) *scannedOut = scanned;
}

static void HexDump(uintptr_t addr, int bytes, const char* label)
{
    char line[256];
    for (int row = 0; row < bytes; row += 16) {
        int n = 0;
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "[obj]   %s %p:", label,
                         (void*)(addr + row));
        for (int i = 0; i < 16 && row + i < bytes; ++i) {
            uint8_t b;
            if (!SafeRead(addr + row + i, &b, 1)) { n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " ??"); continue; }
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %02X", b);
        }
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "  ");
        for (int i = 0; i < 16 && row + i < bytes; ++i) {
            uint8_t b;
            if (!SafeRead(addr + row + i, &b, 1)) b = '?';
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "%c",
                             (b >= 32 && b <= 126) ? (char)b : '.');
        }
        Log("%s", line);
    }
}

// ---- the fast path, which is what should have worked from the start ----
//
// Walk .data for the TArray shape and validate each candidate by reading its first three
// entries. No memory-wide search: the array points at the entries, so there is nothing to
// hunt for.
//
// The first version of this was right in structure and wrong in one detail - it tested ANSI
// only. Mirror's Edge stores name text as UTF-16. Both encodings are tried here, over a wider
// offset range, and the winning combination is reported rather than assumed.
//
// The Count/Max bounds are deliberately loose. The earlier version required
// Max <= Count*4+1024, which is an invented constraint - a TArray's Max is whatever the last
// growth left it at, and rejecting a real candidate for failing a made-up rule is exactly the
// kind of self-inflicted null result this rung has already produced twice.
static bool FindGNamesFast()
{
    for (uintptr_t a = g_dataLo; a + 12 < g_dataHi; a += 4) {
        uint32_t data, count, maxn;
        if (!SafeU32(a, &data) || !SafeU32(a + 4, &count) || !SafeU32(a + 8, &maxn)) continue;
        if (data < 0x10000) continue;
        if (count < 1000 || count > 2000000) continue;
        if (maxn < count) continue;

        uint32_t e0, e1, e2;
        if (!SafeU32(data,     &e0) || e0 < 0x10000) continue;
        if (!SafeU32(data + 4, &e1) || e1 < 0x10000) continue;
        if (!SafeU32(data + 8, &e2) || e2 < 0x10000) continue;

        for (uint32_t off = 0; off <= 0x40; off += 2) {
            const bool wide = NameEntryIsW(e0, off, "None") &&
                              NameEntryIsW(e1, off, "ByteProperty") &&
                              NameEntryIsW(e2, off, "IntProperty");
            const bool ansi = !wide &&
                              NameEntryIs(e0, off, "None") &&
                              NameEntryIs(e1, off, "ByteProperty") &&
                              NameEntryIs(e2, off, "IntProperty");
            if (!wide && !ansi) continue;

            g_gnamesAddr  = a;
            g_nameTextOff = off;
            g_nameWide    = wide;
            Log("*** [obj] GNames at %p  Data=%p Count=%u Max=%u", (void*)a, (void*)data, count, maxn);
            Log("*** [obj] FNameEntry text at +0x%02X, encoding %s", off, wide ? "UTF-16" : "ANSI");
            Log("[obj]     validated: entries 0/1/2 read None / ByteProperty / IntProperty");
            HexDump(e1 - 0x10, 0x40, "entry1");
            return true;
        }
    }
    return false;
}

static bool FindGNames()
{
    if (FindGNamesFast()) return true;
    Log("[obj] fast .data scan found nothing - falling back to the memory-wide search");

    static const wchar_t wNone[] = L"None";
    static const wchar_t wByte[] = L"ByteProperty";
    static const wchar_t wInt[]  = L"IntProperty";

    const ScanPat pats[] = {
        { "None\0",         5,             "ansi None" },
        { "ByteProperty\0", 13,            "ansi ByteProperty" },
        { "IntProperty\0",  12,            "ansi IntProperty" },
        { wNone,            sizeof(wNone), "wide None" },
        { wByte,            sizeof(wByte), "wide ByteProperty" },
        { wInt,             sizeof(wInt),  "wide IntProperty" },
    };
    const int NP = (int)(sizeof(pats) / sizeof(pats[0]));

    std::vector<uintptr_t> hits[NP];
    size_t scanned = 0;
    const double t0 = NowMs();
    ScanMemoryForPatterns(pats, NP, hits, &scanned);
    Log("[obj] scanned %.1f MB in %.0f ms (one pass, self excluded)",
        scanned / 1048576.0, NowMs() - t0);
    for (int p = 0; p < NP; ++p)
        Log("[obj]     %-20s x%zu%s", pats[p].label, hits[p].size(),
            hits[p].size() >= kMaxHitsPerPat ? " (capped)" : "");

    // Try both encodings. Which one the entries use is exactly what is unknown.
    for (int enc = 0; enc < 2; ++enc) {
        const int iNone = enc * 3 + 0, iByte = enc * 3 + 1, iInt = enc * 3 + 2;
        if (hits[iByte].empty()) continue;

        if (enc == 0) {
            for (size_t i = 0; i < hits[iByte].size() && i < 3; ++i) {
                Log("[obj] --- bytes around ANSI hit %zu (%p) ---", i, (void*)hits[iByte][i]);
                HexDump(hits[iByte][i] - 0x18, 0x38, "a");
            }
        } else {
            for (size_t i = 0; i < hits[iByte].size() && i < 3; ++i) {
                Log("[obj] --- bytes around WIDE hit %zu (%p) ---", i, (void*)hits[iByte][i]);
                HexDump(hits[iByte][i] - 0x18, 0x38, "w");
            }
        }

        // GNames.Data is an array of FNameEntry*, so three consecutive pointers land a fixed
        // distance before None / ByteProperty / IntProperty. That gap IS the text offset.
        for (uintptr_t a = g_dataLo; a + 12 < g_dataHi; a += 4) {
            uint32_t data;
            if (!SafeU32(a, &data) || data < 0x10000) continue;
            uint32_t p0, p1, p2;
            if (!SafeU32(data, &p0) || !SafeU32(data + 4, &p1) || !SafeU32(data + 8, &p2)) continue;

            for (uintptr_t bh : hits[iByte]) {
                if (p1 > bh || bh - p1 > 0x40) continue;
                const uint32_t off = (uint32_t)(bh - p1);
                bool okNone = false, okInt = false;
                for (uintptr_t h : hits[iNone]) if (h == p0 + off) { okNone = true; break; }
                for (uintptr_t h : hits[iInt])  if (h == p2 + off) { okInt  = true; break; }
                if (!okNone || !okInt) continue;

                uint32_t count = 0, maxn = 0;
                SafeU32(a + 4, &count); SafeU32(a + 8, &maxn);
                g_gnamesAddr  = a;
                g_nameTextOff = off;
                Log("*** [obj] GNames at %p  Data=%p Count=%u Max=%u  text at +0x%02X (%s)",
                    (void*)a, (void*)data, count, maxn, off, enc ? "UTF-16" : "ANSI");
                return true;
            }
        }
        Log("[obj] %s: strings present, but no .data TArray points at three of them in order",
            enc ? "UTF-16" : "ANSI");
    }

    Log("[obj] GNames NOT FOUND. Either it lives outside .data, or the array does not hold");
    Log("[obj] FNameEntry* directly. The hex dumps above are the evidence for the next step.");
    return false;
}

static bool NameOf(uint32_t index, char* out, size_t cap)
{
    if (!g_gnamesAddr) return false;
    uint32_t data, count;
    if (!SafeU32(g_gnamesAddr, &data) || !SafeU32(g_gnamesAddr + 4, &count)) return false;
    if (index >= count) return false;
    uint32_t entry;
    if (!SafeU32(data + index * 4, &entry) || entry < 0x10000) return false;
    if (!g_nameWide) return SafeAnsi(entry + g_nameTextOff, out, cap);
    // UTF-16 in memory, narrowed for logging. Anything outside printable ASCII rejects the
    // whole name - that rejection is what keeps a stray dword from scoring as a valid index.
    size_t i = 0;
    for (; i + 1 < cap; ++i) {
        uint16_t ch;
        if (!SafeRead(entry + g_nameTextOff + i * 2, &ch, 2)) return false;
        if (ch == 0) break;
        if (ch < 32 || ch > 126) return false;
        out[i] = (char)ch;
    }
    out[i] = 0;
    return i > 0;
}

// Does a candidate array behave like UObjects? Returns the Name offset, or -1.
//
// This is the validation that makes GObjects selection self-checking rather than
// threshold-based. A name offset must decode for nearly every object AND vary across them -
// distinctness is the part that matters, because null fields decode as index 0 ("None") and so
// score a perfect hit rate while being constant.
static int FindNameOffsetFor(uint32_t data, uint32_t count)
{
    const int kMaxOff = 0x60, kSlots = kMaxOff / 4;
    int hits[kSlots] = {};
    uint32_t seen[kSlots][64] = {};
    int nseen[kSlots] = {};
    int sampled = 0;

    for (uint32_t i = 0; i < count && sampled < 400; ++i) {
        uint32_t obj, vt;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        if (!SafeU32(obj, &vt) || !InModule(vt)) continue;
        sampled++;
        for (int off = 4; off < kMaxOff; off += 4) {
            const int s = off / 4;
            uint32_t v; char nm[64];
            if (!SafeU32(obj + off, &v) || !NameOf(v, nm, sizeof(nm))) continue;
            hits[s]++;
            bool known = false;
            for (int k = 0; k < nseen[s]; ++k) if (seen[s][k] == v) { known = true; break; }
            if (!known && nseen[s] < 64) seen[s][nseen[s]++] = v;
        }
    }
    if (sampled < 64) return -1;

    int best = -1, bestOff = -1;
    for (int off = 4; off < kMaxOff; off += 4) {
        const int s = off / 4;
        if (nseen[s] < 32) continue;                      // must genuinely vary
        if (hits[s] < sampled * 95 / 100) continue;        // and decode nearly always
        if (hits[s] > best) { best = hits[s]; bestOff = off; }
    }
    return bestOff;
}

static bool FindGObjects()
{
    // Same TArray shape, but the elements are UObject* whose first dword is a vtable pointing
    // into .text. Requiring that of a sample is what separates it from any other pointer array.
    //
    // ⚠️ No Max upper bound. The version that ran first carried `maxn > count*4+1024` here -
    // the same invented rule that was already removed from the GNames scan and forgotten
    // about in this one. GNames itself reports Count=38211 Max=49641, so the rule happens not
    // to bite there; there is no reason to assume GObjects grew as politely.
    //
    // The threshold is 75%, not 90%. Slots hold objects mid-construction and freshly freed
    // pointers, and demanding near-perfection of a live heap is another way to reject a
    // correct answer.
    // ⚠️ Collect ALL candidates and choose; do not take the first that passes.
    //
    // First-match is order-dependent and .data contents vary between runs. One run picked
    // 0x0204A344 (Count=113310, 64/64 vtables) and the next picked 0x01FFA644 (Count=57020,
    // 58/64) purely because the latter sits at a lower address and happened to scrape past the
    // threshold that day. Everything downstream then decoded garbage.
    //
    // The selection below is self-validating: the winner is the candidate for which a NAME
    // OFFSET actually exists - one that decodes for nearly every object and varies across
    // them. GObjects and the layout confirm each other, so neither is chosen on a threshold
    // alone.
    struct Cand { uintptr_t addr; uint32_t data, count, maxn; int ok, checked; };
    Cand cands[32]; int ncand = 0;
    int candidates = 0;

    for (uintptr_t a = g_dataLo; a + 12 < g_dataHi; a += 4) {
        if (a == g_gnamesAddr) continue;
        uint32_t data, count, maxn;
        if (!SafeU32(a, &data) || !SafeU32(a + 4, &count) || !SafeU32(a + 8, &maxn)) continue;
        if (data < 0x10000) continue;
        if (count < 5000 || count > 4000000) continue;
        if (maxn < count) continue;
        // ⚠️ Max must be a PLAUSIBLE allocation count, not merely >= Count.
        //
        // The previous version removed the invented `Max <= Count*4+1024` rule and put nothing
        // in its place, which is an over-correction: it then accepted Max = 0xFFFFFFFF and
        // reported a static .data structure as GObjects. Rejecting a made-up rule is not a
        // reason to stop sanity-checking the field.
        if (maxn > 50000000) continue;
        // GObjects.Data is a HEAP allocation. A "Data" pointer landing inside the module image
        // - as the false positive's did, 332 bytes from its own header - is a static structure
        // being read as an array.
        if (InModule(data)) continue;
        candidates++;

        int checked = 0, vtblOk = 0;
        for (uint32_t i = 0; i < count && checked < 64; ++i) {
            uint32_t obj;
            if (!SafeU32(data + i * 4, &obj)) break;
            if (obj == 0) continue;             // UE3 leaves holes where objects were destroyed
            checked++;
            uint32_t vtbl;
            if (SafeU32(obj, &vtbl) && InModule(vtbl)) vtblOk++;
        }
        if (checked >= 32 && vtblOk >= checked * 9 / 10 && ncand < 32)
            cands[ncand++] = { a, data, count, maxn, vtblOk, checked };
    }

    if (!ncand) {
        Log("[obj] GObjects NOT FOUND after %d TArray-shaped candidates", candidates);
        return false;
    }

    // Best vtable ratio first, then largest Count - the real object array is the big one.
    for (int i = 0; i < ncand; ++i)
        for (int j = i + 1; j < ncand; ++j) {
            const int ri = cands[i].ok * 1000 / cands[i].checked;
            const int rj = cands[j].ok * 1000 / cands[j].checked;
            if (rj > ri || (rj == ri && cands[j].count > cands[i].count)) {
                Cand t = cands[i]; cands[i] = cands[j]; cands[j] = t;
            }
        }

    Log("[obj] %d GObjects candidate(s) passed the shape and vtable tests:", ncand);
    for (int i = 0; i < ncand; ++i)
        Log("[obj]     %p Data=%p Count=%-7u Max=%-10u %d/%d vtables", (void*)cands[i].addr,
            (void*)cands[i].data, cands[i].count, cands[i].maxn, cands[i].ok, cands[i].checked);

    for (int i = 0; i < ncand; ++i) {
        const int off = FindNameOffsetFor(cands[i].data, cands[i].count);
        if (off < 0) {
            Log("[obj]     %p rejected: no offset decodes as a varying name",
                (void*)cands[i].addr);
            continue;
        }
        g_gobjAddr = cands[i].addr;
        g_offName  = off;
        Log("*** [obj] GObjects at %p  Data=%p Count=%u Max=%u  (%d/%d vtables in module)",
            (void*)cands[i].addr, (void*)cands[i].data, cands[i].count, cands[i].maxn,
            cands[i].ok, cands[i].checked);
        Log("*** [obj] confirmed by UObject::Name resolving at +0x%02X", off);
        if (g_gnamesAddr)
            Log("[obj]     GObjects - GNames = 0x%X   (adjacency does NOT transfer from"
                " Singularity, which had 0x30)", (unsigned)(cands[i].addr - g_gnamesAddr));
        return true;
    }

    Log("[obj] GObjects NOT FOUND - %d candidates passed the shape test, none produced a"
        " usable Name offset", ncand);
    return false;
}

// Scores every dword offset in the object header by how often it decodes as a valid GNames
// index whose name is printable. The right offset wins by a wide margin; this is the
// reference's method, and the reason it is a score rather than a guess is that UE3 484 and 536
// do not have to agree on the layout.
static void DetectUObjectLayout()
{
    if (!g_gobjAddr || !g_gnamesAddr) return;

    uint32_t data, count;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) return;

    const int kMaxOff = 0x60;
    const int kSlots  = kMaxOff / 4;
    int hits[kSlots] = {};
    // ⚠️ DISTINCTNESS is the discriminator, not the hit rate.
    //
    // The first scoring run had +0x18, +0x1C, +0x2C, +0x30, +0x40, +0x44 and +0x48 all at
    // 400/400 and refused to choose. Those offsets hold ZERO for every object - null pointers
    // and unset FNames - and index 0 decodes as "None", so a constant field scored a perfect
    // hit rate. The Name field is different in kind: it VARIES across objects.
    uint32_t seen[kSlots][64] = {};
    int      nseen[kSlots] = {};
    int sampled = 0;

    for (uint32_t i = 0; i < count && sampled < 400; ++i) {
        uint32_t obj;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        uint32_t vtbl;
        if (!SafeU32(obj, &vtbl) || !InModule(vtbl)) continue;
        sampled++;
        for (int off = 4; off < kMaxOff; off += 4) {
            const int s = off / 4;
            uint32_t v;
            if (!SafeU32(obj + off, &v)) continue;
            char nm[128];
            if (!NameOf(v, nm, sizeof(nm))) continue;
            hits[s]++;
            bool known = false;
            for (int k = 0; k < nseen[s]; ++k) if (seen[s][k] == v) { known = true; break; }
            if (!known && nseen[s] < 64) seen[s][nseen[s]++] = v;
        }
    }

    // Only offsets that both decode reliably AND vary are candidates.
    int best = -1, bestOff = -1, second = -1;
    for (int off = 4; off < kMaxOff; off += 4) {
        const int s = off / 4;
        if (nseen[s] < 8) continue;                  // constant or near-constant: not a name
        if (hits[s] < sampled * 95 / 100) continue;
        if (hits[s] > best) { second = best; best = hits[s]; bestOff = off; }
        else if (hits[s] > second) second = hits[s];
    }
    Log("[obj] UObject layout scored over %d objects (hit rate / distinct values):", sampled);
    for (int off = 4; off < kMaxOff; off += 4)
        if (hits[off / 4] > 0)
            Log("[obj]     +0x%02X  %3d/%d  %2d distinct%s", off, hits[off / 4], sampled,
                nseen[off / 4], nseen[off / 4] < 8 ? "   <- constant, rejected" : "");

    // ⚠️ "decodes as a name" is a WEAK test on its own, and the first scoring run showed why:
    // twenty offsets scored 40-70% and nothing was decisive. Any small integer below the name
    // count indexes SOME valid entry, and real structures are full of small integers - counts,
    // flags, indices, near-zero floats. The test does not distinguish a name from a number.
    //
    // So dump the actual names the leading offsets produce. A human can tell "Class",
    // "Function", "Package", "Default__..." from a list of unrelated words instantly, and no
    // statistic here does that as reliably.
    int ranked[3] = { -1, -1, -1 };
    for (int r = 0; r < 3; ++r) {
        int bh = -1;
        for (int off = 4; off < kMaxOff; off += 4) {
            bool already = false;
            for (int k = 0; k < r; ++k) if (ranked[k] == off) already = true;
            if (already) continue;
            if (hits[off / 4] > bh) { bh = hits[off / 4]; ranked[r] = off; }
        }
    }
    for (int r = 0; r < 3 && ranked[r] > 0; ++r) {
        const int off = ranked[r];
        char line[512]; int n = 0;
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                         "[obj]   +0x%02X sample names:", off);
        int shown = 0;
        for (uint32_t i = 0; i < count && shown < 10; ++i) {
            uint32_t obj;
            if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
            uint32_t vtbl;
            if (!SafeU32(obj, &vtbl) || !InModule(vtbl)) continue;
            uint32_t v; char nm[64];
            if (!SafeU32(obj + off, &v) || !NameOf(v, nm, sizeof(nm))) continue;
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %s", nm);
            shown++;
        }
        Log("%s", line);
    }

    if (bestOff > 0) {
        g_offName = bestOff;
        Log("*** [obj] UObject::Name at +0x%02X (%d/%d, %d distinct values)",
            bestOff, best, sampled, nseen[bestOff / 4]);
    } else {
        Log("[obj] Name offset NOT decisive (best +0x%02X %d/%d, second %d) - do not trust it.",
            bestOff, best, sampled, second);
        Log("[obj] Read the sample names above: the right offset produces class and package");
        Log("[obj] names, not an assortment of unrelated words.");
    }
}

// Reports whether the classes we actually care about are present and findable by name.
// A layout that scores well but cannot find TdPlayerPawn has not proven anything useful.
// ---- the frame rate cap ----
//
// 62.0 fps, sixteen samples, not a digit of variation, against a headset asking for 120. That is
// a CAP and not a performance limit - a game short of headroom varies, and this does not. 62 is
// also UE3's own default for MaxSmoothedFrameRate, which names the mechanism outright.
//
// ⚠️ 62 into 120 is worse than 60 into 120, and that is the non-obvious part. 120/60 is exactly
// 2, so every game frame is shown for exactly two display frames and the cadence never changes.
// 120/62 is 1.935, so most frames are shown twice and roughly every fifteenth is shown once -
// a beat of about two per second. A regular cadence reads as motion; an irregular one reads as
// snapping, which is the report, and is why moving very slowly hides it: the beat is still
// there, it just has too little image movement to carry it.
//
// So the cap is worth changing even if the game cannot go faster. Both directions are reachable
// from one float.
uintptr_t g_engineObj       = 0;
int       g_offMaxSmoothFps = -1;
// ⚠️ 60, not 120, and the measurement chose it rather than the arithmetic.
//
// The cap write works - raising it from 62 took the delivered rate to 75-80, so the game was
// never short of headroom, it was held. But 75-80 against a 120 Hz display is the worst place to
// be: each frame spans sometimes one display period and sometimes two, measured as a near even
// split between the two buckets. The image holds for double the time on every miss, which is
// visibly worse than holding for double the time EVERY frame.
//
// At 60 the cadence measured 600 frames in a single bucket, twice, exactly. It is fewer unique
// images than the game can produce and it is the only rate it can hold that divides 120.
//
// This is a floor, not a destination. The game reaches 75-80 while spending 4-5 ms of every
// frame in our own frame grab; at 80 fps that is 12.5 ms of which the grab is a third. Take the
// grab off the critical path and 8.3 ms - a true 120, one image per display period - is within
// reach of what the engine is already doing.
// Back to 60, the Ex bring-up having been judged: the device swap and the shared surface both
// hold up, and rung 8 took the grab from 4 ms to 0.4 and the delivered rate from 75-80 to
// 95-119.
//
// 60 stays the default anyway, and the reason is not speed. Measured across a cap sweep:
//
//   cap 60   ->  16.67 ms mean, the cap holding exactly
//   cap 120  ->  13.12 ms mean, wandering between 7.5 and 18.8
//
// The second is faster and looks worse, which was confirmed by eye. 120 into a rate the game
// cannot hold means four frames shown once and every fifth shown twice - a 20 Hz beat - while 60
// divides 120 exactly and every frame is shown twice, forever. Evenness is what the eye reacts
// to; speed only helps once it is even.
//
// Not a permanent verdict. The game reaches 95-119 in lighter scenes, so a 90 Hz headset mode
// would be 1:1 almost everywhere and better than either. NUMPAD7 cycles for exactly that kind of
// test.
float     g_fpsCap          = 60.0f;    // NUMPAD7 cycles
static int LookupProp(const char* className, const char* propName, bool verbose);

static void FindEngineObject()
{
    if (g_offName < 0 || !g_gobjAddr) return;
    uint32_t data, count;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) return;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t obj;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        uint32_t vtbl;
        if (!SafeU32(obj, &vtbl) || !InModule(vtbl)) continue;

        // Matched on the CLASS name, not the object's. The engine's own name varies by build and
        // the class does not, and UObject::Class was already measured at +0x34.
        uint32_t clsPtr, cn;
        char cls[128];
        if (!SafeU32(obj + 0x34, &clsPtr) || clsPtr < 0x10000) continue;
        if (!SafeU32(clsPtr + g_offName, &cn) || !NameOf(cn, cls, sizeof(cls))) continue;
        if (!strstr(cls, "GameEngine")) continue;

        // The class default object carries every property at the right offsets and is not the
        // live engine; writing to it changes nothing anybody reads.
        uint32_t on; char onm[128];
        if (SafeU32(obj + g_offName, &on) && NameOf(on, onm, sizeof(onm)) &&
            !strncmp(onm, "Default__", 9)) continue;

        g_engineObj       = obj;
        g_offMaxSmoothFps = LookupProp(cls, "MaxSmoothedFrameRate", true);
        Log("*** [fps] engine object %p, class %s, MaxSmoothedFrameRate at +0x%X",
            (void*)obj, cls, g_offMaxSmoothFps);
        return;
    }
    Log("[fps] no GameEngine object found - the frame cap cannot be moved from here");
}

static void ProbeKnownObjects()
{
    if (g_offName < 0) return;
    uint32_t data, count;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) return;

    const char* wanted[] = { "TdPlayerPawn", "TdPlayerController", "TdPlayerCamera",
                             "TdSwanNeck", "TdGameInfo" };
    int found[5] = {};
    int live = 0;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t obj;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        uint32_t vtbl;
        if (!SafeU32(obj, &vtbl) || !InModule(vtbl)) continue;
        live++;
        uint32_t nameIdx;
        if (!SafeU32(obj + g_offName, &nameIdx)) continue;
        char nm[128];
        if (!NameOf(nameIdx, nm, sizeof(nm))) continue;
        for (int w = 0; w < 5; ++w) if (!strcmp(nm, wanted[w])) found[w]++;
    }
    Log("[obj] walked %d live objects of %u slots", live, count);
    for (int w = 0; w < 5; ++w)
        Log("[obj]     %-20s %s (%d)", wanted[w], found[w] ? "FOUND" : "not found", found[w]);
}

// ⚠️ Runs on its OWN THREAD, never on the render thread.
//
// The first version ran synchronously inside Present and took 22.8 seconds, which the player
// experienced as the level refusing to load. A diagnostic that changes the thing it is
// measuring is worse than no diagnostic - and this one is read-only, so there is no reason for
// the game to wait on it.
//
// Safe to run concurrently: it only reads through ReadProcessMemory and writes to the log,
// which is already guarded. It touches no D3D or OpenXR state. The object graph may shift
// underneath it, which costs a missed candidate at worst, never a wrong one - every match is
// validated against three exact strings before it is believed.
// Defined below. The scan body reads better before them, so they are declared here rather
// than reordered.
static void DetectPointerFields();
static void ProbeSwanNeck();
static bool DerivePropertyOffsets();
static void DumpClassProperties(const char* className, int maxLines);
static int  LookupProp(const char* className, const char* propName, bool verbose);
static void ResolveMotionRigOffsets();
static void ResolveInputGates();
static void InstallUpdate1pArmsHook();
extern int g_offActorRotation;
extern int g_offActorLocation;
extern int g_offFOVAngle;
extern int g_offDesiredFOV;
extern int g_offDefaultFOV;
extern int g_offCamLoc;
extern int g_offCamRot;
extern int g_offMoveState;
extern int g_offWeapon;
extern int g_offWeaponAnimState;
extern int g_offCtlPawn;        // PlayerController::Pawn - the pawn without a 115k-object walk
extern int g_offCtlCamera;      // PlayerController::PlayerCamera - how ViewTarget is reached
extern int g_offCamViewTarget;  // Camera::ViewTarget - whose view is actually being rendered

static DWORD WINAPI ObjectModelThread(LPVOID)
{
    // ⚠️ The scan RETRIES, and this is a correctness fix rather than caution.
    //
    // It ran exactly once, early, against a heap the game was still filling - and it is a
    // sampling test on live data, so it can fail on a run where nothing is actually wrong. One
    // run rejected the correct candidate (0x0204A344, 64/64 vtables, the same address every run
    // has found) with "no offset decodes as a varying name", while an earlier run with the
    // IDENTICAL Count=88227 Max=90128 accepted it. GNames was still growing at the time -
    // Count=38211 against the ~50000 it settles at - so name indices beyond that count did not
    // decode yet, and the 95% hit-rate threshold missed.
    //
    // The consequence was total: g_offName stayed -1, which gates the whole per-frame hotkey and
    // tracking block, so there was no player controller, no view-matrix scan, no stereo. An
    // intermittent startup heuristic was able to disable every rung above it with one bad roll.
    //
    // Only the LOCATION phase repeats. Everything after it derives from addresses that are
    // already confirmed, and re-running it would just repeat its logging.
    bool located = false;
    for (int attempt = 1; attempt <= 20 && !located; ++attempt) {
        Log("");
        Log("======== object model scan (rung 4a, background thread) - attempt %d ========",
            attempt);
        const double t0 = NowMs();
        FindSections();
        located = FindGNames() && FindGObjects();
        Log("======== attempt %d took %.1f ms (off the render thread) ========",
            attempt, NowMs() - t0);
        if (!located) {
            // Longer waits after the first few: by then the game is at a menu rather than
            // mid-load, and a 2 second scan every 5 seconds forever is not free.
            const DWORD wait = (attempt < 5) ? 5000 : 15000;
            Log("[obj] not located yet - retrying in %lu s (the heap is still settling)",
                wait / 1000);
            Sleep(wait);
        }
    }

    if (!located) {
        Log("[obj] GIVING UP after 20 attempts - no rung above the object model can run");
        InterlockedExchange(&g_objModelThreadFinished, 1);
        return 0;
    }

    const double t0 = NowMs();
    {
        char nm[128];
        for (uint32_t i = 0; i < 5; ++i)
            if (NameOf(i, nm, sizeof(nm))) Log("[obj]     GNames[%u] = \"%s\"", i, nm);
        {
            DetectUObjectLayout();
            ProbeKnownObjects();
            DetectPointerFields();
            ProbeSwanNeck();
            // Validated against TdSwanNeck's already-measured layout before anything reads
            // from it, so a wrong derivation is caught here rather than surfacing later as a
            // bad offset in the head-tracking path.
            if (DerivePropertyOffsets()) {
                // Exactly what rung 5b needs, asked for by name. Rotation is the head-tracking
                // write target; FOVAngle is the single FOV lever CalcCamera reads;
                // PlayerCameraRotation is the value the pawn caches after composing the view,
                // which is the read-back for checking what the engine actually used.
                g_offActorRotation = LookupProp("TdPlayerController", "Rotation", true);
                g_offActorLocation = LookupProp("TdPlayerController", "Location", true);
                g_offFOVAngle      = LookupProp("TdPlayerController", "FOVAngle", true);
                // ⚠️ FOVAngle alone is written straight back over every tick.
                //
                // Decompiled TdPlayerController.AdjustFOV ends with `FOVAngle = DesiredFOV` on
                // the path taken whenever FOVZoomRate is 0, which is the default set at line
                // 1381 of the same class. So FOVAngle is an OUTPUT, recomputed each frame, and
                // writing it achieved nothing: the measurement said 100% of scene matrices still
                // carried the engine's own 58.7 degree vertical.
                //
                // DesiredFOV is the input. DefaultFOV is taken as well because the class's own
                // initialisation does `DesiredFOV = DefaultFOV`, so anything that re-runs that -
                // a respawn, a checkpoint reload - would otherwise undo it.
                g_offDesiredFOV    = LookupProp("TdPlayerController", "DesiredFOV", true);
                g_offDefaultFOV    = LookupProp("TdPlayerController", "DefaultFOV", true);
                // ⚠️ These two RETURN VALUES ARE USED. The first version called LookupProp
                // and discarded them, so g_offCamLoc/g_offCamRot stayed -1, GetCameraPose
                // refused, and the view-matrix scan tested zero windows while reporting only
                // "no candidate matched".
                g_offCamRot = LookupProp("TdPlayerPawn", "PlayerCameraRotation", true);
                g_offCamLoc = LookupProp("TdPlayerPawn", "PlayerCameraLocation", true);
                LookupProp("TdPlayerPawn", "SwanNeck1p", true);
                g_offMoveState = LookupProp("TdPlayerPawn", "MovementState", true);
                // A position proof must still fail closed when the player is armed. Both are
                // ordinary byte/pointer fields; P1.3 only admits Weapon=None and Unarmed(0).
                g_offWeapon = LookupProp("TdPawn", "Weapon", true);
                g_offWeaponAnimState = LookupProp("TdPawn", "WeaponAnimState", true);

                // ---- the lookup that replaces a search with a read ----
                //
                // Controller::Pawn is the pawn, directly. FindPlayerPawn answers the same question
                // by walking every object in the game - measured at 1.2-1.6 s over ~115000 slots,
                // on the render thread - and then not repeating for 600 frames, which is the
                // delay after every death.
                g_offCtlPawn = LookupProp("TdPlayerController", "Pawn", true);

                // Camera::ViewTarget is whose view is being RENDERED, which is a different
                // question from where the pawn is and the one the scene test keeps needing.
                // TdPlayerCamera.UpdateViewTarget guards its whole style switch on
                // Pawn(OutVT.Target).CalcCamera(), so Target is the actor the frame belongs to:
                // when it is not the pawn, the matrices on their way to the GPU are not the
                // player's view and no amount of rejecting them is a fault.
                g_offCtlCamera     = LookupProp("TdPlayerController", "PlayerCamera", true);
                g_offCamViewTarget = LookupProp("TdPlayerCamera", "ViewTarget", true);

                // Phase 1.2 is read-only: resolve the whole first-person arm rig now, on this
                // background thread, so runtime discovery is only a handful of validated pointer
                // reads. No GObjects census belongs in a per-frame hand path.
                ResolveMotionRigOffsets();

                // P1.3 must run after the native has finished configuring the first-person arm
                // controllers. Its UFunction stores the native address, derived below from the
                // live object model rather than hard-coded for one executable layout.
                InstallUpdate1pArmsHook();

                // Nothing above this line can tell "the stick never arrived" from "the game
                // ignored it", and the first public build produced a report that needed exactly
                // that distinction. See the block above ResolveInputGates.
                ResolveInputGates();
                DumpClassProperties("TdPlayerController", 60);
                FindEngineObject();
            }
        }
    }
    Log("======== layout derivation took %.1f ms (off the render thread) ========",
        NowMs() - t0);
    Log("");
    InterlockedExchange(&g_objModelThreadFinished, 1);
    return 0;
}

// ---- which header dwords are pointers to other UObjects? ----
//
// Name was found by decoding it. Outer and Class cannot be: they hold POINTERS, not name
// indices, so the name-scoring pass is blind to them. The test here is "does this dword point
// at something that is itself a UObject" - a vtable inside the module, and a decodable name at
// the offset we already trust.
//
// Sample pointee names are logged rather than a verdict being computed. Singularity's notes
// record picking +0x28 as Class because 783 objects pointed at the right UClasses - and being
// wrong, because those were the classes' own members, whose OUTER is the class. Pointing at a
// UClass is not sufficient evidence of being Class, so the names go in the log for judgement
// rather than into an automatic answer that has already misfired once elsewhere.
static void DetectPointerFields()
{
    if (!g_gobjAddr || g_offName < 0) return;
    uint32_t data, count;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) return;

    const int kMaxOff = 0x60, kSlots = kMaxOff / 4;
    int pts[kSlots] = {}, sampled = 0;

    for (uint32_t i = 0; i < count && sampled < 400; ++i) {
        uint32_t obj;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        uint32_t vt;
        if (!SafeU32(obj, &vt) || !InModule(vt)) continue;
        sampled++;
        for (int off = 4; off < kMaxOff; off += 4) {
            uint32_t p;
            if (!SafeU32(obj + off, &p) || p < 0x10000) continue;
            uint32_t pvt, pn; char nm[64];
            if (!SafeU32(p, &pvt) || !InModule(pvt)) continue;
            if (!SafeU32(p + g_offName, &pn) || !NameOf(pn, nm, sizeof(nm))) continue;
            pts[off / 4]++;
        }
    }

    Log("[obj] pointer-to-UObject scoring over %d objects:", sampled);
    for (int off = 4; off < kMaxOff; off += 4) {
        if (pts[off / 4] < sampled / 4) continue;
        char line[512]; int n = 0;
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                         "[obj]   +0x%02X %3d/%d ->", off, pts[off / 4], sampled);
        int shown = 0;
        for (uint32_t i = 0; i < count && shown < 8; ++i) {
            uint32_t obj, p, pvt, pn; char nm[64];
            if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
            if (!SafeU32(obj, &pvt) || !InModule(pvt)) continue;
            if (!SafeU32(obj + off, &p) || p < 0x10000) continue;
            if (!SafeU32(p, &pvt) || !InModule(pvt)) continue;
            if (!SafeU32(p + g_offName, &pn) || !NameOf(pn, nm, sizeof(nm))) continue;
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %s", nm);
            shown++;
        }
        Log("%s", line);
    }
}

// ---- find TdSwanNeck's property offsets by their DEFAULT VALUES ----
//
// Walking UProperty chains would work and needs three more offsets derived first. This does
// not need any of them, because the defaults in DefaultGame.ini are distinctive enough to
// identify by content:
//
//   DegToUnDeg        = 182.0440063   a float nothing else here will hold
//   DownwardPitchWorld= 48151         likewise as an int
//   ForwardPitchWorld = 65536
//   Quadratic Fwd/Down= 35.0 / 30.0
//   StartTranslateAt  = 15.0
//
// Finding several of those at fixed offsets inside an object named TdSwanNeck is both the
// property map AND the proof that this object is the live instance rather than its UClass -
// a UClass does not carry instance values.
static void ProbeSwanNeck()
{
    if (!g_gobjAddr || g_offName < 0) return;
    uint32_t data, count;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) return;

    struct Sig { const char* name; bool isFloat; float f; uint32_t u; };
    const Sig sigs[] = {
        { "DegToUnDeg",                   true,  182.0440063f, 0 },
        { "DownwardPitchWorld",           false, 0.0f,         48151 },
        { "ForwardPitchWorld",            false, 0.0f,         65536 },
        { "QuadraticForwardTranslation",  true,  35.0f,        0 },
        { "QuadraticDownwardTranslation", true,  30.0f,        0 },
        { "LinearForwardTranslation",     true,  25.0f,        0 },
        { "StartTranslateAtDegree",       true,  15.0f,        0 },
    };
    const int NS = (int)(sizeof(sigs) / sizeof(sigs[0]));
    const uint32_t kScanBytes = 0x400;

    int instance = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t obj, vt, ni; char nm[64];
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        if (!SafeU32(obj, &vt) || !InModule(vt)) continue;
        if (!SafeU32(obj + g_offName, &ni) || !NameOf(ni, nm, sizeof(nm))) continue;
        if (strcmp(nm, "TdSwanNeck") != 0) continue;

        Log("[swan] object #%u at %p named TdSwanNeck", i, (void*)obj);
        int matched = 0;
        for (int s = 0; s < NS; ++s) {
            for (uint32_t off = 0; off < kScanBytes; off += 4) {
                uint32_t raw;
                if (!SafeU32(obj + off, &raw)) continue;
                bool hit;
                if (sigs[s].isFloat) {
                    float f; memcpy(&f, &raw, 4);
                    hit = (f > sigs[s].f - 0.01f) && (f < sigs[s].f + 0.01f);
                } else {
                    hit = (raw == sigs[s].u);
                }
                if (hit) {
                    Log("[swan]     +0x%03X = %-28s (%s)", off, sigs[s].name,
                        sigs[s].isFloat ? "float" : "int");
                    matched++;
                    break;
                }
            }
        }
        if (matched >= 4) {
            instance++;
            Log("[swan]   ^ %d/%d defaults present - this is a LIVE INSTANCE", matched, NS);
        } else {
            Log("[swan]   ^ only %d/%d defaults present - probably the UClass, not an instance",
                matched, NS);
        }
    }
    if (!instance) Log("[swan] no TdSwanNeck instance carrying the expected defaults was found");
}

static void RunObjectModelScan()
{
    if (g_objModelDone) return;
    g_objModelDone = true;
    HANDLE h = CreateThread(nullptr, 0, ObjectModelThread, nullptr, 0, nullptr);
    if (h) { CloseHandle(h); Log("[obj] scan started on a background thread"); }
    else   { Log("[obj] CreateThread failed (%lu) - scan skipped", GetLastError()); }
}

// ================================================================ rung 5a: the property walker
//
// Finding offsets by their default values worked for TdSwanNeck and does not generalise: it
// needs distinctive constants, and it stops working the moment anything writes to them. The
// general mechanism is UE3's own reflection - every UClass carries a linked list of UProperty
// objects, each holding its name and its byte offset within an instance.
//
// Three offsets are needed and all three are DERIVED, because UE3 536 has already disagreed
// with the Singularity build on FNameEntry while agreeing on the UObject header, and there is
// no way to tell which case applies without checking:
//
//   UStruct::Children   pointer to the first UField
//   UField::Next        pointer to the next
//   UProperty::Offset   byte offset within the instance
//
// ---- the derivation validates itself against a known answer ----
//
// TdSwanNeck's layout is already MEASURED by value search: LinearForwardTranslation is at
// +0x60, QuadraticForwardTranslation at +0x68, DegToUnDeg at +0x7C. So the walk is run against
// TdSwanNeck first, and the candidate for UProperty::Offset is the one that reports those
// same numbers. Two independent methods have to agree before anything else uses the result.

// Non-static deliberately: these are forward-declared `extern` further up, where the scan
// body uses them. Declaring a name extern and then defining it static is ill-formed even
// though MSVC accepted it here, and a latent linkage inconsistency in a file this size is not
// worth leaving for someone to trip over later.
int g_offActorRotation = -1;
int g_offActorLocation = -1;
int g_offFOVAngle      = -1;
int g_offDesiredFOV    = -1;   // the INPUT; FOVAngle is recomputed from it every tick
int g_offDefaultFOV    = -1;
static int g_offChildren = -1;
static int g_offNext     = -1;
static int g_offPropOff  = -1;
static int g_offOuter    = -1;   // UObject::Outer, derived from a known property -> owner link

// Phase 1.2 reflected rig layout. Pawn fields point at live objects; controller fields are the
// data P1.3/P1.4 will eventually write after this rung proves the object lifecycle is safe.
static int g_offMesh1p = -1;
static int g_offLeftHandWorldIK = -1, g_offRightHandWorldIK = -1;
static int g_offLeftHandRotation = -1, g_offRightHandRotation = -1;
static int g_offLeftForeArmRoll = -1, g_offRightForeArmRoll = -1;
static int g_offLimbEffector = -1, g_offLimbEffectorSpace = -1;
static int g_offLimbJointTarget = -1, g_offLimbJointTargetSpace = -1;
static int g_offSkelControlStrength = -1;
static int g_offSkelStrengthTarget = -1, g_offSkelBlendTimeToGo = -1;
static int g_offSingleBoneRotation = -1, g_offSingleBoneRotationSpace = -1;
// P1.3 detached-arm experiment. These stay optional so failure to expose the deeper skeletal
// layout cannot take the already-proven wrist-position rung down with it.
static int g_offMeshAnimations = -1, g_offMeshSkelControlIndex = -1;
static int g_offMeshSpaceBases = -1, g_offPrimitiveLocalToWorld = -1;
static int g_offAnimTreeSkelControlLists = -1, g_offSkelNextControl = -1;
static int g_offSingleBoneTranslation = -1, g_offSingleBoneTranslationSpace = -1;
static volatile LONG g_detachedArmOffsetsReady = 0;
static volatile LONG g_motionRigOffsetsReady = 0;

static bool ObjNameIs(uintptr_t obj, const char* want);   // defined with the swan-neck code

// Find a UClass by name: an object whose own Class points at the object named "Class".
static uintptr_t FindClassByName(const char* want)
{
    if (!g_gobjAddr || g_offName < 0) return 0;
    uint32_t data, count;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t obj, vt, cls;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        if (!SafeU32(obj, &vt) || !InModule(vt)) continue;
        if (!ObjNameIs(obj, want)) continue;
        if (!SafeU32(obj + 0x34, &cls) || cls < 0x10000) continue;
        if (ObjNameIs(cls, "Class")) return obj;     // its Class is "Class" => it IS a class
    }
    return 0;
}

static bool ReadObjName(uintptr_t obj, char* out, size_t cap)
{
    uint32_t ni;
    if (!SafeU32(obj + g_offName, &ni)) return false;
    return NameOf(ni, out, cap);
}

// Derive Children / Next / Offset against TdSwanNeck, whose real layout is already known.
static bool DerivePropertyOffsets()
{
    const uintptr_t cls = FindClassByName("TdSwanNeck");
    if (!cls) { Log("[prop] TdSwanNeck UClass not found"); return false; }
    Log("[prop] TdSwanNeck UClass at %p", (void*)cls);

    // Known from the value search. If the walker cannot reproduce these it is wrong.
    struct Known { const char* name; uint32_t off; };
    // LinearDownwardTranslation and the two pitch consts are included here even though the
    // value search could not isolate them - their offsets follow from the declaration order in
    // the decompiled class, and requiring the walker to agree with ALL of them is a stronger
    // test than requiring it to agree with the five that were individually measured.
    const Known known[] = {
        { "LinearForwardTranslation",     0x60 },
        { "LinearDownwardTranslation",    0x64 },
        { "QuadraticForwardTranslation",  0x68 },
        { "QuadraticDownwardTranslation", 0x6C },
        { "StartTranslateAtDegree",       0x70 },
        { "ForwardPitchWorld",            0x74 },
        { "DownwardPitchWorld",           0x78 },
        { "DegToUnDeg",                   0x7C },
    };
    const int NK = (int)(sizeof(known) / sizeof(known[0]));

    // ---- Children: a pointer in the UClass leading to an object named like a property ----
    for (int co = 4; co <= 0xB0 && g_offChildren < 0; co += 4) {
        uint32_t first;
        if (!SafeU32(cls + co, &first) || first < 0x10000) continue;
        uint32_t vt;
        if (!SafeU32(first, &vt) || !InModule(vt)) continue;
        char nm[96];
        if (!ReadObjName(first, nm, sizeof(nm))) continue;
        for (int k = 0; k < NK; ++k) {
            if (strcmp(nm, known[k].name) != 0) continue;
            g_offChildren = co;
            Log("[prop] UStruct::Children at +0x%02X -> \"%s\"", co, nm);
            break;
        }
    }
    if (g_offChildren < 0) {
        Log("[prop] Children NOT FOUND - no pointer in the UClass leads to a known property");
        return false;
    }

    uint32_t head = 0;
    SafeU32(cls + g_offChildren, &head);

    // ---- Outer: a known UProperty must point back to its declaring UClass ----
    //
    // Retail cooking can remove a property from UStruct::Children without removing the
    // UProperty object that script bytecode references. Recovering those detached properties
    // requires identifying their owner, not trusting a globally duplicated field name. The
    // first TdSwanNeck field and its already-proven class give an exact pointer equality test.
    int outerMatches = 0;
    for (int off = 4; off <= 0x60; off += 4) {
        uint32_t value = 0;
        if (SafeU32(head + off, &value) && value == cls) {
            g_offOuter = off;
            outerMatches++;
        }
    }
    if (outerMatches == 1) {
        Log("*** [prop] UObject::Outer at +0x%02X (known TdSwanNeck property -> owner UClass)",
            g_offOuter);
    } else {
        Log("[prop] UObject::Outer NOT DERIVED - known property had %d pointers to its owner",
            outerMatches);
        g_offOuter = -1;
    }

    // ---- Next: found STRUCTURALLY, by which offset yields a long chain ----
    //
    // The first version required the next link to be a name from the known list, and failed
    // immediately: Children points at LinearForwardTranslation, whose Next is
    // LinearDownwardTranslation - deliberately left out of that list because it shares the
    // value 25.0 with LinearForward and so could not be told apart by value search.
    //
    // Matching names was the wrong test regardless. A linked list is identified by BEING a
    // list: the right offset walks many distinct, well-named objects without cycling, and
    // every wrong offset terminates within a step or two.
    int bestLen = 0;
    for (int no = 4; no <= 0xB0; no += 4) {
        uintptr_t seen[96]; int nseen = 0; int len = 0;
        uintptr_t p = head;
        while (len < 96) {
            uint32_t nxt;
            if (!SafeU32(p + no, &nxt) || nxt < 0x10000) break;
            uint32_t vt;
            if (!SafeU32(nxt, &vt) || !InModule(vt)) break;
            char nm[96];
            if (!ReadObjName(nxt, nm, sizeof(nm))) break;
            bool cycle = false;
            for (int k = 0; k < nseen; ++k) if (seen[k] == nxt) { cycle = true; break; }
            if (cycle) break;
            if (nseen < 96) seen[nseen++] = nxt;
            p = nxt; len++;
        }
        if (len > bestLen) { bestLen = len; g_offNext = no; }
    }
    if (bestLen < 3) {
        Log("[prop] Next NOT FOUND - longest chain from any offset was %d", bestLen);
        g_offNext = -1;
        return false;
    }
    Log("[prop] UField::Next at +0x%02X (chain of %d from the first field)", g_offNext, bestLen);
    {
        char line[512]; int n = 0;
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "[prop]   chain:");
        uintptr_t p = head; int shown = 0;
        while (p && shown < 10) {
            char nm[96];
            if (!ReadObjName(p, nm, sizeof(nm))) break;
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %s", nm);
            shown++;
            uint32_t nxt;
            if (!SafeU32(p + g_offNext, &nxt) || nxt < 0x10000) break;
            p = nxt;
        }
        Log("%s", line);
    }

    // ---- Offset: the dword in each UProperty that equals its MEASURED offset ----
    //
    // Every candidate must agree for EVERY known property, not just one. A single match is
    // easy to hit by accident; five in a row at the same dword is not.
    for (int oo = 4; oo <= 0xB0 && g_offPropOff < 0; oo += 4) {
        int agree = 0, seen = 0;
        for (uintptr_t p = head; p; ) {
            char nm[96];
            if (!ReadObjName(p, nm, sizeof(nm))) break;
            for (int k = 0; k < NK; ++k) {
                if (strcmp(nm, known[k].name) != 0) continue;
                seen++;
                uint32_t v;
                if (SafeU32(p + oo, &v) && v == known[k].off) agree++;
            }
            uint32_t nxt;
            if (!SafeU32(p + g_offNext, &nxt) || nxt < 0x10000) break;
            p = nxt;
        }
        if (seen >= 3 && agree == seen) {
            g_offPropOff = oo;
            Log("*** [prop] UProperty::Offset at +0x%02X (agreed with %d/%d measured offsets)",
                oo, agree, seen);
        }
    }
    if (g_offPropOff < 0) {
        Log("[prop] Offset NOT FOUND - no dword reproduced the measured TdSwanNeck offsets");
        return false;
    }
    return true;
}

// Look up ONE property by name, walking the class and its superclasses. Returns -1 if absent.
//
// This replaces reading a bulk dump. The dump capped at 40 lines never reached Actor's
// Rotation - it exhausted its budget inside TdPlayerController's own properties - and the
// Actor dump came back empty, which a targeted query diagnoses instead of hiding: it reports
// how many fields it walked, so "not found" and "the chain broke" stop looking alike.
static int LookupProp(const char* className, const char* propName, bool verbose)
{
    if (g_offChildren < 0 || g_offNext < 0 || g_offPropOff < 0) return -1;
    uintptr_t cls = FindClassByName(className);
    if (!cls) { if (verbose) Log("[prop] class %s NOT FOUND", className); return -1; }

    int walked = 0;
    for (int depth = 0; cls && depth < 16; ++depth) {
        char cn[96] = "?";
        ReadObjName(cls, cn, sizeof(cn));
        uint32_t head = 0;
        if (SafeU32(cls + g_offChildren, &head) && head >= 0x10000) {
            uintptr_t seen[512]; int nseen = 0;
            for (uintptr_t p = head; p && nseen < 512; ) {
                char nm[96];
                if (!ReadObjName(p, nm, sizeof(nm))) break;
                walked++;
                if (strcmp(nm, propName) == 0) {
                    uint32_t off;
                    if (SafeU32(p + g_offPropOff, &off) && off < 0x8000) {
                        Log("*** [prop] %s::%s at +0x%04X   (declared on %s, %d fields walked)",
                            className, propName, off, cn, walked);
                        return (int)off;
                    }
                }
                bool cycle = false;
                for (int k = 0; k < nseen; ++k) if (seen[k] == p) { cycle = true; break; }
                if (cycle) break;
                seen[nseen++] = p;
                uint32_t nxt;
                if (!SafeU32(p + g_offNext, &nxt) || nxt < 0x10000) break;
                p = nxt;
            }
        } else if (verbose && depth == 0) {
            Log("[prop] %s has no readable Children pointer at +0x%02X", cn, g_offChildren);
        }
        uint32_t super;
        if (!SafeU32(cls + 0x3C, &super) || super < 0x10000) break;   // SuperField
        cls = super;
    }
    if (verbose)
        Log("[prop] %s::%s NOT FOUND after walking %d fields up the class chain",
            className, propName, walked);
    return -1;
}

struct DetachedPropRequest {
    const char* ownerClass;
    const char* propertyName;
    const char* propertyClass;
    int* destination;
    int foundOffset;
    int matches;
    bool disagreement;
};

// Resolve every missing cooked property in ONE GObjects pass. The former call site invoked
// LookupDetachedProp separately for 23 fields. Each invocation walked roughly 115,000 objects,
// turning identical validation work into a measured 44-second hand-activation delay.
static void LookupDetachedPropsBatch(DetachedPropRequest* requests, int requestCount)
{
    if (!requests || requestCount <= 0 || !g_gobjAddr || g_offName < 0 ||
        g_offOuter < 0 || g_offPropOff < 0) return;
    uint32_t data = 0, count = 0;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) return;

    const double started = NowMs();
    for (int r = 0; r < requestCount; ++r) {
        requests[r].foundOffset = -1;
        requests[r].matches = 0;
        requests[r].disagreement = false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t obj = 0, vt = 0;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000 ||
            !SafeU32(obj, &vt) || !InModule(vt)) continue;

        char objectName[96] = "?";
        if (!ReadObjName(obj, objectName, sizeof(objectName))) continue;
        bool nameWanted = false;
        for (int r = 0; r < requestCount; ++r) {
            if (*requests[r].destination < 0 &&
                strcmp(objectName, requests[r].propertyName) == 0) {
                nameWanted = true; break;
            }
        }
        if (!nameWanted) continue;

        uint32_t propCls = 0, outer = 0, ownerClass = 0, off = 0;
        if (!SafeU32(obj + 0x34, &propCls) || propCls < 0x10000 ||
            !SafeU32(obj + g_offOuter, &outer) || outer < 0x10000 ||
            !SafeU32(outer + 0x34, &ownerClass) || ownerClass < 0x10000 ||
            !ObjNameIs(ownerClass, "Class") ||
            !SafeU32(obj + g_offPropOff, &off) || off >= 0x8000) continue;

        char propertyClassName[96] = "?", ownerName[96] = "?";
        if (!ReadObjName(propCls, propertyClassName, sizeof(propertyClassName)) ||
            !ReadObjName(outer, ownerName, sizeof(ownerName))) continue;

        for (int r = 0; r < requestCount; ++r) {
            DetachedPropRequest& request = requests[r];
            if (*request.destination >= 0 ||
                strcmp(objectName, request.propertyName) != 0 ||
                strcmp(propertyClassName, request.propertyClass) != 0 ||
                strcmp(ownerName, request.ownerClass) != 0) continue;
            if (request.foundOffset < 0) request.foundOffset = (int)off;
            else if (request.foundOffset != (int)off) request.disagreement = true;
            request.matches++;
        }
    }

    int resolved = 0;
    for (int r = 0; r < requestCount; ++r) {
        DetachedPropRequest& request = requests[r];
        if (*request.destination >= 0) continue;
        if (request.matches > 0 && !request.disagreement) {
            *request.destination = request.foundOffset;
            ++resolved;
            Log("*** [prop] %s::%s at +0x%04X from %d detached %s object%s"
                " (batched, Outer validated)", request.ownerClass, request.propertyName,
                request.foundOffset, request.matches, request.propertyClass,
                request.matches == 1 ? "" : "s");
        } else {
            Log("[prop] detached %s::%s %s (%d owner-validated %s object%s)",
                request.ownerClass, request.propertyName,
                request.disagreement ? "DISAGREED ON OFFSET" : "NOT FOUND",
                request.matches, request.propertyClass,
                request.matches == 1 ? "" : "s");
        }
    }
    Log("[prop] batched detached-property pass resolved %d fields over %u objects in %.1f ms",
        resolved, count, NowMs() - started);
}

// ================================================================ P1.3 update timing hook
//
// Update1pArms is declared `native final` by the shipped TdPlayerPawn package. UE3 stores the
// executable for every UFunction in UFunction::Func, but that member's offset is build-specific.
// Derive it by the strong mode formed by thousands of non-native UFunctions: all of those point
// at the same ProcessInternal implementation, while native functions point at their own execs.
// Only executable-section pointers participate, which excludes the shared UFunction vtable that
// otherwise wins this census from .rdata.
typedef void (__fastcall *PFN_Update1pArms)(void* self, void* edx, void* stack, void* result);
static PFN_Update1pArms g_origUpdate1pArms = nullptr;
static uintptr_t g_update1pArmsTarget = 0;
static void RestoreDedicatedHandForearmOverridesBeforeGame(uintptr_t pawn);
static void RestoreMotionHandPositionOverridesBeforeGame(uintptr_t pawn);
static void RestoreWristRotationOverridesBeforeGame(uintptr_t pawn);
static void RestoreDetachedArmOverridesBeforeGame(uintptr_t pawn);
static void ApplyDetachedShoulders(uintptr_t pawn, const P13PoseSnapshot& pose);
static void ApplyWristRotations(uintptr_t pawn, const P13PoseSnapshot& pose);
static void MonitorArmContinuity(uintptr_t pawn, const P13PoseSnapshot& pose);

static bool LateReanchorOneHand(const RenderedHeadFrame& head, bool leftHand,
                                P13HandPoseSnapshot* pose)
{
    if (!pose || !pose->worldValid || !FiniteVec(pose->cameraLocal)) return false;
    XrQuaternionf view{};
    if (!NormalizedQuaternion(pose->viewOrientation, &view)) return false;

    // Close to the headset the optical and inertial trackers disagree: run 19 measured the RAW
    // view-space grip orientation stepping 15-21 degrees per update with the controller held
    // still near the face, identical to the written hand's steps - the judder arrives from the
    // runtime. Reject one isolated large step and hold the last accepted orientation; a step
    // that persists is real motion and is accepted one update late (>20 deg per update is
    // ~1400 deg/s, faster than any deliberate wrist flick).
    auto stepDeg = [](const XrQuaternionf& a, const XrQuaternionf& b) {
        float dot = fabsf(a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w);
        if (dot > 1.0f) dot = 1.0f;
        return 2.0f * acosf(dot) * 57.2957795f;
    };
    const int hand = leftHand ? 0 : 1;
    static bool haveAccepted[2] = { false, false };
    static bool haveRejected[2] = { false, false };
    static XrQuaternionf accepted[2]{};
    static XrQuaternionf rejected[2]{};
    if (haveAccepted[hand] && stepDeg(view, accepted[hand]) > 20.0f) {
        const bool persisted = haveRejected[hand] &&
                               stepDeg(view, rejected[hand]) < 15.0f;
        if (!persisted) {
            rejected[hand] = view;
            haveRejected[hand] = true;
            view = accepted[hand];
        } else {
            haveRejected[hand] = false;
        }
    } else {
        haveRejected[hand] = false;
    }
    accepted[hand] = view;
    haveAccepted[hand] = true;
    const MEVR_Vec3 worldOffset = CameraVectorToWorld(head, pose->cameraLocal);
    const MEVR_Vec3 worldPosition{ head.position.x + worldOffset.x,
                                   head.position.y + worldOffset.y,
                                   head.position.z + worldOffset.z };
    const XrQuaternionf worldOrientation = ViewGripOrientationToUEWorld(view, head);
    const float norm2 = worldOrientation.x*worldOrientation.x +
                        worldOrientation.y*worldOrientation.y +
                        worldOrientation.z*worldOrientation.z +
                        worldOrientation.w*worldOrientation.w;
    if (!FiniteVec(worldPosition) || !std::isfinite(norm2) ||
        norm2 < 0.98f || norm2 > 1.02f) return false;
    pose->worldPosition = worldPosition;
    pose->worldOrientation = worldOrientation;
    return true;
}

// Present sampled a controller relative to the head, but the player can advance several Unreal
// units before Update1pArms consumes it. Move the sampled head anchor by the pawn's actual game-
// thread displacement, then compose the head-relative controller pose here. This removes the
// locomotion-only one-frame world-space error without predicting velocity or smoothing input.
static void LateReanchorP13Pose(uintptr_t pawn, P13PoseSnapshot* pose)
{
    if (!pose) return;

    MEVR_Vec3 pawnLocation{};
    const bool havePawn = g_offActorLocation >= 0 &&
        SafeRead(pawn + g_offActorLocation, &pawnLocation, sizeof(pawnLocation)) &&
        FiniteVec(pawnLocation);

    // ONE coherent camera read per arm update, position and axes together, taken on the game
    // thread after the game's own update. The previous design anchored position to the
    // Present-time snapshot and bridged locomotion with a pawn-delta correction - and the
    // marked run-22 captures measured the result: consecutive arm updates saw the anchor
    // alternate between two temporal states, so ALL camera translation reached the hands as
    // half-rate double-lumps (0 then 2x per update: +/-1-3 UU trembling at rest, 15-20 UU
    // lumps while walking) while the rendered world moved smoothly. Reading the anchor at
    // consume time makes target latency CONSTANT, which is what smooth means; the correction
    // and its crouch-jerk gate protected a mechanism that no longer exists. The Present
    // snapshot remains the fallback for frames where the live camera is unreadable.
    RenderedHeadFrame lateHead{};
    bool haveLateHead = GetRenderedHeadFrame(&lateHead);
    if (!haveLateHead && pose->sampledHeadValid) {
        lateHead = pose->sampledHead;
        haveLateHead = true;
    }
    if (!haveLateHead) return;

    const bool left = LateReanchorOneHand(lateHead, true, &pose->left);
    const bool right = LateReanchorOneHand(lateHead, false, &pose->right);

    // A stale camera anchor during menu/level transitions can put the world target tens of
    // thousands of UU from the pawn for a few frames: the first eligible frame of run 5
    // measured 21,328 UU and slammed both detached shoulders to the reach cap. A tracked hand
    // is never far from the pawn that owns the mesh, so treat an implausible target exactly
    // like lost tracking until the anchor is coherent again.
    if (havePawn) {
        P13HandPoseSnapshot* hands[2] = { &pose->left, &pose->right };
        for (int i = 0; i < 2; ++i) {
            if (!hands[i]->worldValid) continue;
            const float distance = VecLength({
                hands[i]->worldPosition.x - pawnLocation.x,
                hands[i]->worldPosition.y - pawnLocation.y,
                hands[i]->worldPosition.z - pawnLocation.z });
            if (std::isfinite(distance) && distance <= 250.0f) continue;
            hands[i]->worldValid = false;
            static long lastReport = -1000;
            if (g_frames - lastReport >= 90) {
                lastReport = g_frames;
                Log("*** [hands-late] %s world target %.0f UU from pawn rejected:"
                    " camera anchor is not coherent with the pawn yet",
                    i == 0 ? "LEFT" : "RIGHT", distance);
            }
        }
    }

    if (g_motionHandsDebug && (left || right)) {
        static long samples = 0;
        static long oldestPose = 0;
        const long age = g_frames - pose->presentFrame;
        if (age > oldestPose) oldestPose = age;
        if (++samples >= 600) {
            Log("[hands-late] re-anchored 600 arm updates from the live camera;"
                " controller pose age %ld frames worst", oldestPose);
            samples = 0;
            oldestPose = 0;
        }
    }
}

static void __fastcall Hook_Update1pArms(void* self, void* edx, void* stack, void* result)
{
    if (!g_origUpdate1pArms) return;
    // The preceding frame may have borrowed a dormant controller for the left shoulder. Always
    // put the authored tree and controller bytes back before Mirror's Edge updates parkour,
    // weapon, or root-offset ownership for this tick.
    // Dedicated hand rotation borrows SwingControl after shoulder detachment has established its
    // frame topology. Unwind in exact reverse order so the game always receives the authored tree.
    RestoreDedicatedHandForearmOverridesBeforeGame(reinterpret_cast<uintptr_t>(self));
    RestoreWristRotationOverridesBeforeGame(reinterpret_cast<uintptr_t>(self));
    RestoreDetachedArmOverridesBeforeGame(reinterpret_cast<uintptr_t>(self));
    RestoreMotionHandPositionOverridesBeforeGame(reinterpret_cast<uintptr_t>(self));
    g_origUpdate1pArms(self, edx, stack, result);

    static volatile LONG calls = 0;
    const LONG call = InterlockedIncrement(&calls);
    if (call == 1)
        Log("*** [hands-p1.3] Update1pArms hook executed; VR IK now runs after the game arm update");

    if (!g_motionHands) return;
    P13PoseSnapshot pose{};
    ReadP13PoseSnapshot(&pose);   // an unreadable snapshot remains invalid and releases ownership
    LateReanchorP13Pose(reinterpret_cast<uintptr_t>(self), &pose);
    ApplyMotionHandPosition(reinterpret_cast<uintptr_t>(self), pose);
    ApplyDetachedShoulders(reinterpret_cast<uintptr_t>(self), pose);
    ApplyWristRotations(reinterpret_cast<uintptr_t>(self), pose);
    MonitorArmContinuity(reinterpret_cast<uintptr_t>(self), pose);
}

static int DeriveUFunctionFuncOffset(uintptr_t* sharedScriptTarget)
{
    if (sharedScriptTarget) *sharedScriptTarget = 0;
    if (!g_gobjAddr || g_offName < 0 || !g_textLo || g_textHi <= g_textLo) return -1;

    uint32_t data = 0, count = 0;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) return -1;

    const int kFirst = 0x40, kLast = 0x100, kStep = 4;
    const int kOffsets = (kLast - kFirst) / kStep;
    const int kBuckets = 8;
    struct Tally { uintptr_t value[kBuckets]; int hits[kBuckets]; };
    Tally tally[kOffsets]{};
    uint32_t functionClass = 0;
    int sampled = 0;

    for (uint32_t i = 0; i < count && sampled < 3000; ++i) {
        uint32_t obj = 0, cls = 0;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000 ||
            !SafeU32(obj + 0x34, &cls) || cls < 0x10000) continue;
        if (!functionClass) {
            if (!ObjNameIs(cls, "Function")) continue;
            functionClass = cls;
        } else if (cls != functionClass) continue;

        ++sampled;
        for (int k = 0; k < kOffsets; ++k) {
            uint32_t value = 0;
            if (!SafeU32(obj + kFirst + k * kStep, &value) ||
                value < g_textLo || value >= g_textHi) continue;
            Tally& t = tally[k];
            int freeSlot = -1, slot = -1;
            for (int b = 0; b < kBuckets; ++b) {
                if (t.hits[b] && t.value[b] == value) { slot = b; break; }
                if (!t.hits[b] && freeSlot < 0) freeSlot = b;
            }
            if (slot < 0) slot = freeSlot;
            if (slot >= 0) { t.value[slot] = value; ++t.hits[slot]; }
        }
    }

    int bestOffset = -1, bestHits = 0;
    uintptr_t bestTarget = 0;
    for (int k = 0; k < kOffsets; ++k) {
        for (int b = 0; b < kBuckets; ++b) {
            if (tally[k].hits[b] > bestHits) {
                bestHits = tally[k].hits[b];
                bestTarget = tally[k].value[b];
                bestOffset = kFirst + k * kStep;
            }
        }
    }

    Log("[hands-p1.3] UFunction::Func probe sampled %d function objects; best +0x%02X"
        " -> %p shared by %d (%.1f%%)", sampled, bestOffset, (void*)bestTarget, bestHits,
        sampled ? 100.0 * bestHits / sampled : 0.0);
    if (sampled < 50 || bestOffset < 0 || bestHits * 2 < sampled) {
        Log("[hands-p1.3] Update1pArms hook refused: no majority executable Func field");
        return -1;
    }
    if (sharedScriptTarget) *sharedScriptTarget = bestTarget;
    return bestOffset;
}

static void InstallUpdate1pArmsHook()
{
    if (!g_motionHands || g_origUpdate1pArms || g_update1pArmsTarget) return;

    uintptr_t scriptTarget = 0;
    const int funcOffset = DeriveUFunctionFuncOffset(&scriptTarget);
    if (funcOffset < 0) return;

    uint32_t data = 0, count = 0;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) return;

    uintptr_t target = 0;
    int matches = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t obj = 0, cls = 0, outer = 0, fn = 0, ownerClass = 0;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000 ||
            !ObjNameIs(obj, "Update1pArms") ||
            !SafeU32(obj + 0x34, &cls) || !ObjNameIs(cls, "Function") ||
            !SafeU32(obj + g_offOuter, &outer) || outer < 0x10000 ||
            !ObjNameIs(outer, "TdPlayerPawn") ||
            !SafeU32(outer + 0x34, &ownerClass) || !ObjNameIs(ownerClass, "Class") ||
            !SafeU32(obj + funcOffset, &fn) || fn < g_textLo || fn >= g_textHi) continue;
        if (!target) target = fn;
        else if (target != fn) {
            Log("[hands-p1.3] Update1pArms hook refused: owner-validated functions disagree"
                " (%p vs %p)", (void*)target, (void*)fn);
            return;
        }
        ++matches;
    }

    if (!target || matches == 0 || target == scriptTarget) {
        Log("[hands-p1.3] Update1pArms hook refused: native target was %p, matches=%d,"
            " shared script target=%p", (void*)target, matches, (void*)scriptTarget);
        return;
    }

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        Log("[hands-p1.3] Update1pArms hook refused: MH_Initialize status %d", (int)init);
        return;
    }
    const MH_STATUS create = MH_CreateHook(reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(&Hook_Update1pArms),
        reinterpret_cast<void**>(&g_origUpdate1pArms));
    if (create != MH_OK) {
        g_origUpdate1pArms = nullptr;
        Log("[hands-p1.3] Update1pArms MH_CreateHook failed, status %d", (int)create);
        return;
    }
    const MH_STATUS enable = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enable != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        g_origUpdate1pArms = nullptr;
        Log("[hands-p1.3] Update1pArms MH_EnableHook failed, status %d", (int)enable);
        return;
    }

    g_update1pArmsTarget = target;
    Log("*** [hands-p1.3] hooked native TdPlayerPawn.Update1pArms at %p"
        " (UFunction::Func +0x%02X, %d owner-validated object%s)",
        (void*)target, funcOffset, matches, matches == 1 ? "" : "s");
}

// Resolve every field needed to identify the first-person arm rig, plus the controller data
// later phases will manipulate.  P1.2 only reads these fields; resolving the future write
// targets now proves they exist on this exact game build before any motion override is added.
static void ResolveMotionRigOffsets()
{
    g_offMesh1p              = LookupProp("TdPawn", "Mesh1p", true);
    g_offLeftHandWorldIK     = LookupProp("TdPawn", "LeftHandWorldIKController", true);
    g_offRightHandWorldIK    = LookupProp("TdPawn", "RightHandWorldIKController", true);
    g_offLeftHandRotation    = LookupProp("TdPawn", "LeftHandRotationController", true);
    g_offRightHandRotation   = LookupProp("TdPawn", "RightHandRotationController", true);
    g_offLeftForeArmRoll     = LookupProp("TdPawn", "LeftForeArmRollRotationController", true);
    g_offRightForeArmRoll    = LookupProp("TdPawn", "RightForeArmRollRotationController", true);

    g_offLimbEffector        = LookupProp("SkelControlLimb", "EffectorLocation", true);
    g_offLimbEffectorSpace   = LookupProp("SkelControlLimb", "EffectorLocationSpace", true);
    // Joint targeting is useful for a later elbow-pole refinement, but is not required for the
    // first controller-driven wrist target. Record it without making this rung depend on it.
    g_offLimbJointTarget      = LookupProp("SkelControlLimb", "JointTargetLocation", true);
    g_offLimbJointTargetSpace = LookupProp("SkelControlLimb", "JointTargetLocationSpace", true);
    g_offSkelControlStrength  = LookupProp("SkelControlBase", "ControlStrength", true);
    g_offSkelStrengthTarget   = LookupProp("SkelControlBase", "StrengthTarget", true);
    g_offSkelBlendTimeToGo    = LookupProp("SkelControlBase", "BlendTimeToGo", true);
    g_offSingleBoneRotation   = LookupProp("SkelControlSingleBone", "BoneRotation", true);
    g_offSingleBoneRotationSpace =
        LookupProp("SkelControlSingleBone", "BoneRotationSpace", true);

    g_offMeshAnimations = LookupProp("SkeletalMeshComponent", "Animations", true);
    g_offMeshSkelControlIndex =
        LookupProp("SkeletalMeshComponent", "SkelControlIndex", true);
    g_offMeshSpaceBases = LookupProp("SkeletalMeshComponent", "SpaceBases", true);
    g_offPrimitiveLocalToWorld = LookupProp("PrimitiveComponent", "LocalToWorld", true);
    g_offAnimTreeSkelControlLists = LookupProp("AnimTree", "SkelControlLists", true);
    g_offSkelNextControl = LookupProp("SkelControlBase", "NextControl", true);
    g_offSingleBoneTranslation =
        LookupProp("SkelControlSingleBone", "BoneTranslation", true);
    g_offSingleBoneTranslationSpace =
        LookupProp("SkelControlSingleBone", "BoneTranslationSpace", true);

    // Mirror's Edge's retail cook detaches editinline and native controller properties from
    // Children. They remain real UProperty objects because the shipped bytecode uses them.
    // Resolve every missing field in one census; each candidate still has to match its exact
    // name, UProperty subclass, Outer UClass, and offset agreement.
    DetachedPropRequest detached[] = {
        { "TdPawn", "LeftHandWorldIKController", "ObjectProperty",
          &g_offLeftHandWorldIK },
        { "TdPawn", "RightHandWorldIKController", "ObjectProperty",
          &g_offRightHandWorldIK },
        { "TdPawn", "LeftHandRotationController", "ObjectProperty",
          &g_offLeftHandRotation },
        { "TdPawn", "RightHandRotationController", "ObjectProperty",
          &g_offRightHandRotation },
        { "TdPawn", "LeftForeArmRollRotationController", "ObjectProperty",
          &g_offLeftForeArmRoll },
        { "TdPawn", "RightForeArmRollRotationController", "ObjectProperty",
          &g_offRightForeArmRoll },
        { "SkelControlLimb", "EffectorLocation", "StructProperty",
          &g_offLimbEffector },
        { "SkelControlLimb", "EffectorLocationSpace", "ByteProperty",
          &g_offLimbEffectorSpace },
        { "SkelControlLimb", "JointTargetLocation", "StructProperty",
          &g_offLimbJointTarget },
        { "SkelControlLimb", "JointTargetLocationSpace", "ByteProperty",
          &g_offLimbJointTargetSpace },
        { "SkelControlBase", "ControlStrength", "FloatProperty",
          &g_offSkelControlStrength },
        { "SkelControlBase", "StrengthTarget", "FloatProperty",
          &g_offSkelStrengthTarget },
        { "SkelControlBase", "BlendTimeToGo", "FloatProperty",
          &g_offSkelBlendTimeToGo },
        { "SkelControlSingleBone", "BoneRotation", "StructProperty",
          &g_offSingleBoneRotation },
        { "SkelControlSingleBone", "BoneRotationSpace", "ByteProperty",
          &g_offSingleBoneRotationSpace },
        { "SkeletalMeshComponent", "Animations", "ObjectProperty",
          &g_offMeshAnimations },
        { "SkeletalMeshComponent", "SkelControlIndex", "ArrayProperty",
          &g_offMeshSkelControlIndex },
        { "SkeletalMeshComponent", "SpaceBases", "ArrayProperty",
          &g_offMeshSpaceBases },
        { "PrimitiveComponent", "LocalToWorld", "StructProperty",
          &g_offPrimitiveLocalToWorld },
        { "AnimTree", "SkelControlLists", "ArrayProperty",
          &g_offAnimTreeSkelControlLists },
        { "SkelControlBase", "NextControl", "ObjectProperty",
          &g_offSkelNextControl },
        { "SkelControlSingleBone", "BoneTranslation", "StructProperty",
          &g_offSingleBoneTranslation },
        { "SkelControlSingleBone", "BoneTranslationSpace", "ByteProperty",
          &g_offSingleBoneTranslationSpace },
    };
    LookupDetachedPropsBatch(detached, (int)(sizeof(detached) / sizeof(detached[0])));

    // This one property object is absent from the retail live object table, while its right
    // neighbor survives at +0x0414. The cooked TdPawn declaration places the two object pointers
    // consecutively, Right then Left. Derive only that adjacent slot; UpdateMotionRigDiscovery
    // still reads it and requires a live SkelControlSingleBone before the rig can be accepted.
    if (g_offLeftForeArmRoll < 0 && g_offRightForeArmRoll >= 0) {
        g_offLeftForeArmRoll = g_offRightForeArmRoll + 4;
        Log("*** [prop] TdPawn::LeftForeArmRollRotationController provisionally at +0x%04X"
            " (adjacent after owner-validated Right; live class validation required)",
            g_offLeftForeArmRoll);
    }

    // A cooked import can disappear from the live object table even while adjacent native fields
    // remain. EffectorLocationSpace did so in the first post-Update1pArms test, after resolving
    // independently at +0xA0 in each of the three preceding launches. Derive that one missing byte
    // only when the two surviving, owner-validated fields prove the authored native layout:
    //
    //   EffectorLocation Vector (+12 bytes), EffectorLocationSpace byte,
    //   JointTargetLocationSpace byte
    //
    // P1.3 additionally reads this candidate from both live limb controllers and requires a valid
    // EBoneControlSpace value before writing, so this relationship alone never grants write access.
    if (g_offLimbEffectorSpace < 0 && g_offLimbEffector >= 0 &&
        g_offLimbJointTargetSpace == g_offLimbEffector + (int)sizeof(MEVR_Vec3) + 1) {
        g_offLimbEffectorSpace = g_offLimbEffector + (int)sizeof(MEVR_Vec3);
        Log("*** [prop] SkelControlLimb::EffectorLocationSpace provisionally at +0x%04X"
            " (between owner-validated EffectorLocation and JointTargetLocationSpace;"
            " both live limbs must validate)", g_offLimbEffectorSpace);
    }

    // Engine.u declares these consecutively as ControlStrength, BlendInTime, BlendOutTime,
    // StrengthTarget, BlendTimeToGo. SetSkelControlStrength owns the first, fourth, and fifth.
    // Retail cooking can omit any individual metadata object, so recover a missing tail only from
    // the independently owner-validated ControlStrength offset. P1.3 validates all three floats on
    // both live controllers before this structural fallback can grant write access.
    if (g_offSkelControlStrength >= 0) {
        if (g_offSkelStrengthTarget < 0) {
            g_offSkelStrengthTarget = g_offSkelControlStrength + 3 * (int)sizeof(float);
            Log("*** [prop] SkelControlBase::StrengthTarget provisionally at +0x%04X"
                " (shipped Engine.u field sequence; both live limbs must validate)",
                g_offSkelStrengthTarget);
        }
        if (g_offSkelBlendTimeToGo < 0) {
            g_offSkelBlendTimeToGo = g_offSkelControlStrength + 4 * (int)sizeof(float);
            Log("*** [prop] SkelControlBase::BlendTimeToGo provisionally at +0x%04X"
                " (shipped Engine.u field sequence; both live limbs must validate)",
                g_offSkelBlendTimeToGo);
        }
    }

    const bool resolvedPointers =
        g_offLeftHandWorldIK >= 0 && g_offRightHandWorldIK >= 0 &&
        g_offLeftHandRotation >= 0 && g_offRightHandRotation >= 0 &&
        g_offLeftForeArmRoll >= 0 && g_offRightForeArmRoll >= 0;
    const bool reflectedControllerFields =
        g_offLimbEffector >= 0 && g_offLimbEffectorSpace >= 0 &&
        g_offSkelControlStrength >= 0 && g_offSkelStrengthTarget >= 0 &&
        g_offSkelBlendTimeToGo >= 0 &&
        g_offSingleBoneRotation >= 0 && g_offSingleBoneRotationSpace >= 0;

    // The retail cook keeps Mesh1p reflected but strips the editinline IK pointer properties
    // and the native Engine controller fields from Children. That is not permission to guess
    // offsets. Runtime discovery can still proceed from Mesh1p's proven boundary and derive the
    // pawn pointers from the complete class pattern; later writes remain disabled until their
    // own layout is independently derived.
    InterlockedExchange(&g_motionRigOffsetsReady, g_offMesh1p >= 0 ? 1 : -1);
    const bool detachedArmLayout =
        g_offMeshAnimations >= 0 && g_offMeshSkelControlIndex >= 0 &&
        g_offMeshSpaceBases >= 0 && g_offPrimitiveLocalToWorld >= 0 &&
        g_offAnimTreeSkelControlLists >= 0 && g_offSkelNextControl >= 0 &&
        g_offSingleBoneTranslation >= 0 && g_offSingleBoneTranslationSpace >= 0 &&
        g_offSingleBoneRotation >= 0 && g_offSkelControlStrength >= 0 &&
        g_offSkelStrengthTarget >= 0 && g_offSkelBlendTimeToGo >= 0;
    InterlockedExchange(&g_detachedArmOffsetsReady, detachedArmLayout ? 1 : -1);
    Log("[hands-rig] reflected discovery base %s; pawn pointers %s; controller fields %s;"
        " optional joint target %s",
        g_offMesh1p >= 0 ? "READY" : "INCOMPLETE - discovery disabled",
        resolvedPointers ? "available" : "stripped - require structural derivation",
        reflectedControllerFields ? "available" : "stripped - writes remain disabled",
        (g_offLimbJointTarget >= 0 && g_offLimbJointTargetSpace >= 0)
            ? "available" : "unavailable");
    Log("[hands-detach] bilateral shoulder layout %s (Animations, control lists/index,"
        " SpaceBases, LocalToWorld, single-bone translation)",
        detachedArmLayout ? "READY" : "INCOMPLETE - wrist IK remains available");
}

// ================================================================ the input gates
//
// ---- why this exists ----
//
// The first public build produced a report the log could not answer: jump, crouch, punch and
// smooth turn all working, forward and back doing nothing. Every one of those controls arrives
// through the same synthesised pad, in the same four fields, on the same frame - so "the left
// stick never reached the game" and "the game received it and declined to move" produce BYTE
// IDENTICAL logs. Two entirely different faults, one indistinguishable symptom, and a round
// trip to the reporter to learn which.
//
// The pad side is answered beside XrSyncInput, where the sticks are built. This is the other
// half: what the GAME thinks it was given, and whether it was allowed to act on it.
//
//   TdPlayerController::InputSize   the analogue magnitude the game computed for itself. Zero
//                                   while the player pushes forward means the pad never landed,
//                                   and no amount of looking at our own end would have shown it.
//   bLeftThumbStickPassedDeadZone   the game's own verdict on that magnitude.
//   IgnoreMoveInput / bCinematicMode
//                                   UE3 zeroes aForward and aStrafe when move input is ignored
//                                   and touches NOTHING ELSE - look, jump, crouch and fire all
//                                   keep working. That is the reported symptom exactly, and a
//                                   scripted sequence that never hands control back leaves it
//                                   set. Worth knowing before anything is built to fix it.

// ---- UBoolProperty::BitMask, derived the way everything else here is derived ----
//
// A UE3 bool is one BIT. UProperty::Offset gives only the dword it lives in - which is why the
// TdPlayerController dump shows twenty different bools all reporting +0x052C - so the offset
// alone cannot read one. The mask lives in UBoolProperty, past the end of UProperty, and the
// end of UProperty is precisely the kind of build-specific number this file refuses to assume.
//
// It falls out of a constraint no wrong dword satisfies: within one group of bools sharing an
// Offset, the masks are DISTINCT POWERS OF TWO. ArrayDim is 1 for every property - a power of
// two, but not distinct, so a group of two kills it. PropertyFlags collide and are rarely a
// single bit. A pointer is neither. The twenty-strong group on TdPlayerController is what makes
// this decisive rather than suggestive; a lone bool would pass every candidate and prove
// nothing, which is why a minimum group size is required as well as a minimum count.
static int g_offBoolMask = -1;

static bool PropIsBool(uintptr_t prop)
{
    uint32_t cls;
    if (!SafeU32(prop + 0x34, &cls) || cls < 0x10000) return false;   // UObject::Class, +0x34
    return ObjNameIs(cls, "BoolProperty");
}

static void DeriveBoolMaskOffset(const char* className)
{
    if (g_offChildren < 0 || g_offNext < 0 || g_offPropOff < 0) return;
    const uintptr_t cls0 = FindClassByName(className);
    if (!cls0) { Log("[prop] %s not found - bool flags cannot be read", className); return; }

    int passed[8] = {}, npassed = 0, bestBools = 0, bestGroup = 0;
    for (int bo = 4; bo <= 0xC0; bo += 4) {
        if (bo == g_offPropOff) continue;

        uint32_t goff[96] = {}, gbits[96] = {};    // an offset, and the bits already claimed there
        int gsize[96] = {};
        int groups = 0, bools = 0, biggest = 0;
        bool ok = true;

        for (uintptr_t cls = cls0; cls && ok; ) {
            uint32_t head = 0;
            if (SafeU32(cls + g_offChildren, &head) && head >= 0x10000) {
                uintptr_t seen[512]; int nseen = 0;
                for (uintptr_t p = head; p && nseen < 512 && ok; ) {
                    uint32_t off = 0, m = 0;
                    if (PropIsBool(p) && SafeU32(p + g_offPropOff, &off) && SafeU32(p + bo, &m)) {
                        if (m == 0 || (m & (m - 1)) != 0) {
                            ok = false;                      // not a single bit - wrong dword
                        } else {
                            int g = -1;
                            for (int k = 0; k < groups; ++k) if (goff[k] == off) { g = k; break; }
                            if (g < 0 && groups < 96) { g = groups++; goff[g] = off; }
                            if (g >= 0) {
                                if (gbits[g] & m) ok = false;   // two bools sharing one bit
                                else {
                                    gbits[g] |= m;
                                    if (++gsize[g] > biggest) biggest = gsize[g];
                                }
                            }
                            bools++;
                        }
                    }
                    bool cycle = false;
                    for (int k = 0; k < nseen; ++k) if (seen[k] == p) { cycle = true; break; }
                    if (cycle) break;
                    seen[nseen++] = p;
                    uint32_t nxt;
                    if (!SafeU32(p + g_offNext, &nxt) || nxt < 0x10000) break;
                    p = nxt;
                }
            }
            uint32_t super;
            if (!SafeU32(cls + 0x3C, &super) || super < 0x10000) break;   // SuperField
            cls = super;
        }

        if (ok && bools >= 8 && biggest >= 4) {
            if (npassed < 8) passed[npassed++] = bo;
            if (bools > bestBools) { bestBools = bools; bestGroup = biggest; }
        }
    }

    if (npassed == 0) {
        Log("[prop] UBoolProperty::BitMask NOT FOUND - bool flags will be skipped");
        return;
    }
    // BitMask is the first member PAST UProperty, so among the survivors the lowest offset above
    // Offset is the one. More than one survivor is reported rather than hidden: it means the
    // constraint was not as tight as it looks on this build, and a wrong pick would otherwise
    // show up much later as a flag that is always zero.
    g_offBoolMask = -1;
    for (int i = 0; i < npassed; ++i)
        if (passed[i] > g_offPropOff && (g_offBoolMask < 0 || passed[i] < g_offBoolMask))
            g_offBoolMask = passed[i];
    if (g_offBoolMask < 0) g_offBoolMask = passed[0];

    Log("*** [prop] UBoolProperty::BitMask at +0x%02X   (%d bools, largest group %d, every mask a"
        " distinct single bit)", g_offBoolMask, bestBools, bestGroup);
    if (npassed > 1)
        Log("[prop]   ⚠️ %d dwords satisfied the test; took the lowest above Offset. A flag that"
            " reads as always-zero means this picked wrong.", npassed);
}

// ---- one property, and HOW to read it ----
//
// Same walk as LookupProp, but it also answers what kind of field arrived. That matters here
// and nowhere else so far: UE3 changed the ignore-input flags from bools into byte COUNTERS
// partway through its life, and Mirror's Edge sits close enough to the changeover that which
// one this build carries is not knowable from the name. A mask of zero means "read the byte".
struct FlagRef { int off; uint32_t mask; };

static bool LookupFlag(const char* className, const char* propName, FlagRef* out)
{
    out->off = -1; out->mask = 0;
    if (g_offChildren < 0 || g_offNext < 0 || g_offPropOff < 0) return false;
    uintptr_t cls = FindClassByName(className);
    if (!cls) return false;

    for (int depth = 0; cls && depth < 16; ++depth) {
        char cn[96] = "?";
        ReadObjName(cls, cn, sizeof(cn));
        uint32_t head = 0;
        if (SafeU32(cls + g_offChildren, &head) && head >= 0x10000) {
            uintptr_t seen[512]; int nseen = 0;
            for (uintptr_t p = head; p && nseen < 512; ) {
                char nm[96];
                if (!ReadObjName(p, nm, sizeof(nm))) break;
                if (strcmp(nm, propName) == 0) {
                    uint32_t off;
                    if (SafeU32(p + g_offPropOff, &off) && off < 0x8000) {
                        const bool isBool = PropIsBool(p);
                        uint32_t m = 0;
                        if (isBool) {
                            // A bool we cannot mask is worse than no bool at all - the whole
                            // dword reads as "set" whenever ANY of its twenty flags is.
                            if (g_offBoolMask < 0 || !SafeU32(p + g_offBoolMask, &m) || m == 0) {
                                Log("[input] %s::%s is a bool but its mask is unknown - skipped",
                                    className, propName);
                                return false;
                            }
                        }
                        out->off = (int)off; out->mask = m;
                        Log("*** [input] %s::%s at +0x%04X  (%s, declared on %s)",
                            className, propName, off,
                            isBool ? "bit" : "byte", cn);
                        return true;
                    }
                }
                bool cycle = false;
                for (int k = 0; k < nseen; ++k) if (seen[k] == p) { cycle = true; break; }
                if (cycle) break;
                seen[nseen++] = p;
                uint32_t nxt;
                if (!SafeU32(p + g_offNext, &nxt) || nxt < 0x10000) break;
                p = nxt;
            }
        }
        uint32_t super;
        if (!SafeU32(cls + 0x3C, &super) || super < 0x10000) break;
        cls = super;
    }
    return false;
}

// ---- the names are CANDIDATES, not a schema ----
//
// Every one of these is a guess at what this revision of UE3 calls the thing, and half of them
// are expected to be absent. Absence is reported once, in one line, because "this build has no
// bCinematicMode" is itself an answer worth having in a bug report - and because a silent miss
// here would look exactly like a flag that is always zero.
static const char* const kGateNames[] = {
    "IgnoreMoveInput", "bIgnoreMoveInput",
    "IgnoreLookInput", "bIgnoreLookInput",
    "bCinematicMode", "bCinemaDisableInputMove", "bCinemaDisableInputLook",
    "bIgnoreButtonInput",
    "bLeftThumbStickPassedDeadZone", "bRightThumbStickPassedDeadZone",
    "bIsWalking", "bIsStopping",
};
static const int kGateMax = (int)(sizeof(kGateNames) / sizeof(kGateNames[0]));

struct Gate { const char* name; FlagRef ref; };
static Gate g_gates[kGateMax];
static int  g_gateCount = 0;
static int  g_offInputSize = -1;

static void ResolveInputGates()
{
    DeriveBoolMaskOffset("TdPlayerController");

    char missing[512]; int mn = 0; int nmissing = 0;
    missing[0] = 0;
    for (int i = 0; i < kGateMax; ++i) {
        FlagRef r;
        if (LookupFlag("TdPlayerController", kGateNames[i], &r)) {
            g_gates[g_gateCount].name = kGateNames[i];
            g_gates[g_gateCount].ref  = r;
            g_gateCount++;
        } else {
            nmissing++;
            mn += _snprintf_s(missing + mn, sizeof(missing) - mn, _TRUNCATE, " %s", kGateNames[i]);
        }
    }

    // The stick magnitude the game computed for itself. The single most useful number here:
    // it sits on the game's side of the pad and answers the "did it arrive" half outright.
    g_offInputSize = LookupProp("TdPlayerController", "InputSize", true);

    Log("[input] %d of %d gates resolved; InputSize %s", g_gateCount, kGateMax,
        g_offInputSize >= 0 ? "found" : "NOT FOUND");
    if (nmissing)
        Log("[input]   absent on this build (expected - the names differ by UE3 revision):%s",
            missing);
}

// Walk a class and its superclasses, logging every property with its offset.
static void DumpClassProperties(const char* className, int maxLines)
{
    if (g_offChildren < 0 || g_offNext < 0 || g_offPropOff < 0) return;
    uintptr_t cls = FindClassByName(className);
    if (!cls) { Log("[prop] class %s not found", className); return; }

    Log("[prop] ---- %s ----", className);
    int lines = 0;
    for (int depth = 0; cls && depth < 12; ++depth) {
        char cn[96] = "?";
        ReadObjName(cls, cn, sizeof(cn));
        uint32_t head = 0;
        if (!SafeU32(cls + g_offChildren, &head)) break;
        for (uintptr_t p = head; p && lines < maxLines; ) {
            char nm[96];
            uint32_t off;
            if (!ReadObjName(p, nm, sizeof(nm))) break;
            if (SafeU32(p + g_offPropOff, &off) && off < 0x4000) {
                Log("[prop]   +0x%04X  %-40s (%s)", off, nm, cn);
                lines++;
            }
            uint32_t nxt;
            if (!SafeU32(p + g_offNext, &nxt) || nxt < 0x10000) break;
            p = nxt;
        }
        uint32_t super;
        if (!SafeU32(cls + 0x3C, &super) || super < 0x10000) break;   // SuperField
        cls = super;
    }
}

// ================================================================ rung 4b: write a property
//
// The swan neck is a pitch-driven lean: look down past StartTranslateAtDegree and the camera
// translates forward and down, modelling craning your neck to see your feet. In a headset the
// player's real neck already does that, so the engine doing it too is a comfort candidate.
//
// ---- why this is a memory write and not an ini edit ----
//
// Mirror's Edge hash-checks its config files and refuses to start when they are modified -
// measured, and it cost two runs. But the hash is over the FILE. It says nothing about the
// values once they are loaded, so writing the properties directly sidesteps the check entirely
// and needs no external patcher.
//
// ---- identifying the live instance ----
//
// NOT by its default values. That worked to FIND the offsets, and stops working the moment we
// write to them - the object would stop matching its own signature. Instead: an instance's
// Class (+0x34) points at its UClass, while the UClass's Class points at the object named
// "Class". That test survives our own writes.
//
// Instances are recreated on level load, so the pointer is re-resolved rather than cached
// forever, and re-resolution is cheap because it is only done when the cached one stops
// looking like a TdSwanNeck.

static const uint32_t SWAN_LinearFwd   = 0x60;
static const uint32_t SWAN_LinearDown  = 0x64;
static const uint32_t SWAN_QuadFwd     = 0x68;
static const uint32_t SWAN_QuadDown    = 0x6C;

static uintptr_t g_swanNeck = 0;
// 0 = original, 1 = zeroed, 2 = exaggerated 8x. ZEROED is the VR default as of run 24: the
// player's F7 dose-response test convicted the swan lag spring as the standing-judder
// oscillator (zeroed stops it, original judders at ~1 UU/frame on 585/600 frames, 8x is much
// worse), and a lag spring under the camera is a flatscreen comfort feature that a headset
// actively does not want. F7 still cycles for A/B.
static int  g_swanMode = 1;
static float g_swanSaved[4] = { 0, 0, 0, 0 };
static bool  g_swanSavedOk = false;
// Occupancy state. g_swanStandDownFrame: frame the current game-owned span began (the calm-
// exit edge), -1 when the mod occupies the spring; the assert that retakes it stamps the
// span's length. g_swanGameMoveLogged: the once-per-span telemetry latch for the game moving
// the spring off the originals the exit edge left standing.
static long  g_swanStandDownFrame = -1;
static bool  g_swanGameMoveLogged = false;
// ~0.6 s at 72 Hz. The marked hard land stayed locked for seconds past Landing->Walking, so
// the recovery's camera tail outlives the state flip; the dwell keeps the re-zero out of it.
static const long kSwanCalmDwell = 45;
static const char* MoveName(int st);    // kMoveNames lookup, defined beside the table

static bool ObjNameIs(uintptr_t obj, const char* want)
{
    uint32_t ni; char nm[64];
    if (!SafeU32(obj + g_offName, &ni) || !NameOf(ni, nm, sizeof(nm))) return false;
    return strcmp(nm, want) == 0;
}

static bool LooksLikeSwanInstance(uintptr_t obj)
{
    if (!obj || g_offName < 0) return false;
    uint32_t vt;
    if (!SafeU32(obj, &vt) || !InModule(vt)) return false;
    if (!ObjNameIs(obj, "TdSwanNeck")) return false;
    uint32_t cls;
    if (!SafeU32(obj + 0x34, &cls) || cls < 0x10000) return false;
    // The UClass itself has Class -> "Class"; an instance has Class -> "TdSwanNeck".
    return ObjNameIs(cls, "TdSwanNeck");
}

// ⚠️ A FAILED search must not repeat every frame.
//
// This walks ~88,000 objects with several ReadProcessMemory calls each. Cached, that costs
// three reads a frame and is free. UNCACHED it is a few hundred thousand syscalls per frame,
// and it runs on the render thread.
//
// That is exactly what happened: with no TdSwanNeck instance present - at a menu, during a
// load, before one is constructed - the search failed, so it ran in full on every frame. The
// framerate collapsed AND F7 appeared dead, because "no instance" means no write. One cause,
// two symptoms that look unrelated.
//
// The Singularity project measured the identical failure in its run 35: "a failed scan is
// enormously expensive, and it repeats every frame... 69-82 ms a frame against a normal 6. So
// a failed scan arms a backoff." That note was read while porting and not applied here.
static long g_swanNextTry = 0;          // frame number before which we do not search again
static const long kSwanBackoffFrames = 600;
static bool g_swanMissLogged = false;
static uint32_t g_swanCursor = 0;       // resume slot of the amortized walk, 0 = fresh pass
static double g_swanWalkMs = 0.0;       // accumulated cost of the pass in progress

// The one-shot walk was measured at 1084 ms over 88678 slots ON THE RENDER THREAD - a visible
// one-second world freeze on every level transition, exactly the size of the worst single-frame
// stalls in the run-12 pacing histograms. This caller runs every frame, so walk a small
// time-budgeted slice per call and resume at the cursor: the instance is found within a few
// seconds of wall time while no individual frame pays more than the budget.
static uintptr_t FindSwanNeck()
{
    if (LooksLikeSwanInstance(g_swanNeck)) return g_swanNeck;

    if (g_swanNeck) {          // had one, lost it - a level transition, so try again promptly
        Log("[swan] cached instance %p is no longer valid at frame %ld t=%.2fs, re-resolving",
            (void*)g_swanNeck, g_frames, LogSecs());
        g_swanNeck = 0;
        g_swanSavedOk = false;
        g_swanStandDownFrame = -1;      // the span's bookkeeping died with its object
        g_swanGameMoveLogged = false;
        g_swanNextTry = 0;
        g_swanCursor = 0;
        g_swanWalkMs = 0.0;
    }
    if (g_frames < g_swanNextTry) return 0;

    if (!g_gobjAddr || g_offName < 0) { g_swanNextTry = g_frames + kSwanBackoffFrames; return 0; }
    uint32_t data, count;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) {
        g_swanNextTry = g_frames + kSwanBackoffFrames; return 0;
    }

    const double t0 = NowMs();
    // While a non-original spring mode is waiting to be re-applied to a replacement instance
    // (level streaming churns it), every frame of walk time is a frame of the lag-spring judder
    // being back - run 24 measured it returning mid-zeroed for exactly the resolve gap. Spend
    // more per frame then; 6 ms still fits the 13.9 ms frame with the game's own work.
    const double kBudgetMs = (g_swanMode != 0) ? 6.0 : 3.0;
    if (g_swanCursor >= count) g_swanCursor = 0;
    uint32_t i = g_swanCursor;
    for (; i < count; ++i) {
        if ((i & 0x3F) == 0 && NowMs() - t0 > kBudgetMs) break;
        uint32_t obj;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        if (!LooksLikeSwanInstance(obj)) continue;
        g_swanNeck = obj;
        g_swanMissLogged = false;
        Log("[swan] live instance at %p at frame %ld t=%.2fs (amortized walk: %.1f ms total,"
            " found at slot %u of %u)", (void*)obj, g_frames, LogSecs(),
            g_swanWalkMs + NowMs() - t0, i, count);
        g_swanCursor = 0;
        g_swanWalkMs = 0.0;
        return obj;
    }
    g_swanWalkMs += NowMs() - t0;
    g_swanCursor = i;
    if (i < count) return 0;               // pass continues next frame at the cursor

    // One COMPLETE pass found nothing. Cost is reported the first time, because a pass that
    // finds nothing still walks every slot and that number is the reason the backoff exists.
    g_swanCursor = 0;
    g_swanNextTry = g_frames + kSwanBackoffFrames;
    if (!g_swanMissLogged) {
        g_swanMissLogged = true;
        Log("[swan] no live TdSwanNeck yet - %.1f ms spread over a %u-slot pass;"
            " backing off %ld frames", g_swanWalkMs, count, kSwanBackoffFrames);
    }
    g_swanWalkMs = 0.0;
    return 0;
}

static bool WriteF32(uintptr_t addr, float v)
{
    SIZE_T wrote = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr, &v, 4, &wrote) && wrote == 4;
}

static void ApplySwanNeck()
{
    if (g_offName < 0) return;
    const uintptr_t obj = FindSwanNeck();
    if (!obj) return;

    // One movement-state read shared by every gate and log line below. The byte changes on
    // the game thread mid-frame; two reads disagreeing inside one call would make the log lie
    // about what the gate saw. 0xFF = unreadable, and unreadable is never calm.
    uint8_t move = 0xFF;
    if (!(g_playerPawn && g_offMoveState >= 0 &&
          SafeRead(g_playerPawn + g_offMoveState, &move, 1)))
        move = 0xFF;
    const bool calm = (move == 1 || move == 15 || move == 29 || move == 30);

    // Saved on first sight so the original values can be put back, rather than assuming the
    // shipped defaults - the player may have a config that differs from DefaultGame.ini.
    if (!g_swanSavedOk) {
        const uint32_t offs[4] = { SWAN_LinearFwd, SWAN_LinearDown, SWAN_QuadFwd, SWAN_QuadDown };
        bool all = true;
        float tmp[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 4; ++i)
            if (!SafeRead(obj + offs[i], &tmp[i], sizeof(float))) { all = false; break; }
        if (all) {
            for (int i = 0; i < 4; ++i) g_swanSaved[i] = tmp[i];
            g_swanSavedOk = true;
            // Move-stamped because this capture is UNGATED: on an instance the game recreated
            // mid-transition these "originals" may be a transition profile's values, and a
            // later restore would then restore the wrong spring. 25/25/35/30 every time so
            // far - the stamp is here to catch the day that stops being true.
            Log("[swan] original values at frame %ld t=%.2fs (move %s(%d)): linear %.1f/%.1f"
                "  quadratic %.1f/%.1f", g_frames, LogSecs(), MoveName(move), (int)move,
                tmp[0], tmp[1], tmp[2], tmp[3]);
        }
    }

    if (!g_swanSavedOk) return;

    // ---- occupancy, not write timing: zeros stand ONLY in sustained calm ----
    //
    // The phase-1 marked run overturned the write-fight theory of the transition freezes.
    // All three - a death fall stuck 4.3 s in out-of-enum state 72 until the pawn was
    // destroyed, a hard-land recovery locked for seconds past Landing->Walking, a pipe-grab
    // Climb mount stuck 10.7 s against full stick input (InputSize 1.0, ignore flags clear) -
    // ran with ZERO swan writes inside the stall window, while 30+ ordinary jumps and small
    // falls sailed through the identical stand-down machinery. What the frozen states shared
    // was the spring's CONTENTS: linear still held the mod's zeros. The span telemetry shows
    // the game rewriting the quadratic pair (35/30) at every airborne entry and never once
    // touching linear - linear is read-only config to the game - and the states that froze
    // are exactly the scripted recoveries long known to READ the spring to execute (run 26).
    // Standing zeros starve those reads. So the rule is occupancy: zeros may occupy the
    // spring only while the pawn is provably calm, and every calm exit puts the originals
    // back before a sequence can read starvation values.
    //
    // Run 25 still applies inside calm (the game swaps camera profiles on its own schedule
    // and rewrites the spring on the same instance), so calm occupancy is verify-and-assert
    // every frame, not write-once. Run 27's polarity also still applies: calm is a WHITELIST
    // (Walking/Crouch/Balance/LedgeWalk with a readable state); everything else - state 72
    // and unreadable bytes included - belongs to the game.
    static long calmStreak = 0;
    static bool wasCalm = false;
    static int  prevMode = 1;           // matches g_swanMode's initial value
    const bool modeChanged = (g_swanMode != prevMode);
    prevMode = g_swanMode;
    calmStreak = calm ? calmStreak + 1 : 0;

    if (wasCalm && !calm && g_swanMode != 0) {
        // Calm-exit edge: one restore, field-by-field, skipping fields already at their
        // original value - so the quadratic pair the game routinely stages on this very
        // frame is never re-written and a value race cannot occur even in principle. In
        // practice this writes the two linear fields (zeros out, originals in), which the
        // game has never been observed to write; there is nothing to fight.
        const uint32_t offs[4] = { SWAN_LinearFwd, SWAN_LinearDown, SWAN_QuadFwd, SWAN_QuadDown };
        int wrote = 0;
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            float now = 0.0f;
            if (!SafeRead(obj + offs[i], &now, sizeof(float))) { ok = false; break; }
            if (fabsf(now - g_swanSaved[i]) <= 0.01f) continue;
            if (WriteF32(obj + offs[i], g_swanSaved[i])) ++wrote; else ok = false;
        }
        Log("*** [swan] EXIT-RESTORE at frame %ld t=%.2fs (move %s(%d)): originals back in"
            " %d field%s%s", g_frames, LogSecs(), MoveName(move), (int)move,
            wrote, wrote == 1 ? "" : "s", ok ? "" : "  <- A READ/WRITE FAILED");
        g_swanStandDownFrame = g_frames;
        g_swanGameMoveLogged = false;
    }
    wasCalm = calm;

    if (!calm) {
        // Game-owned span: no writes. Telemetry only - one line the first time the game
        // moves the spring away from the originals the exit edge left standing, so any
        // transition profile that differs from the shipped values finally becomes visible.
        if (g_swanStandDownFrame >= 0 && !g_swanGameMoveLogged) {
            const uint32_t offs[4] = { SWAN_LinearFwd, SWAN_LinearDown, SWAN_QuadFwd, SWAN_QuadDown };
            float now[4] = { 0, 0, 0, 0 };
            bool moved = false, readable = true;
            for (int i = 0; i < 4; ++i) {
                if (!SafeRead(obj + offs[i], &now[i], sizeof(float))) { readable = false; break; }
                if (fabsf(now[i] - g_swanSaved[i]) > 0.01f) moved = true;
            }
            if (readable && moved) {
                g_swanGameMoveLogged = true;
                Log("*** [swan] game moved the spring at frame %ld t=%.2fs (move %s(%d)):"
                    " linear %.1f/%.1f quadratic %.1f/%.1f", g_frames, LogSecs(),
                    MoveName(move), (int)move, now[0], now[1], now[2], now[3]);
            }
        }
        return;
    }

    if (g_swanMode == 0) {
        // Vanilla mode. Its only write ever is putting originals back if an F7 change to
        // mode 0 caught zeros still standing in calm; a non-calm change needs nothing (the
        // exit edge already restored), and after the catch-up the game is never touched -
        // even its own calm-state profile swaps (run 25) are left alone.
        if (modeChanged) {
            const uint32_t offs[4] = { SWAN_LinearFwd, SWAN_LinearDown, SWAN_QuadFwd, SWAN_QuadDown };
            int wrote = 0;
            for (int i = 0; i < 4; ++i) {
                float now = 0.0f;
                if (!SafeRead(obj + offs[i], &now, sizeof(float))) break;
                if (fabsf(now - g_swanSaved[i]) <= 0.01f) continue;
                if (WriteF32(obj + offs[i], g_swanSaved[i])) ++wrote;
            }
            if (wrote > 0)
                Log("*** [swan] mode 0: originals restored in %d field%s at frame %ld t=%.2fs",
                    wrote, wrote == 1 ? "" : "s", g_frames, LogSecs());
        }
        g_swanStandDownFrame = -1;
        return;
    }

    // Calm, non-vanilla mode. Sit out the dwell before re-zeroing: the assert used to fire
    // on the exact Landing->Walking / Climb->Walking frame - by construction, the first calm
    // frame - and the marked hard land stayed locked for seconds PAST that write, because
    // the recovery's camera tail runs on into Walking. The dwell keeps the write out of the
    // tail; against a judder that needs sustained standing to build, ~0.6 s is invisible.
    if (calmStreak < kSwanCalmDwell) return;

    const float mul = (g_swanMode == 1) ? 0.0f : 8.0f;
    const uint32_t offs[4] = { SWAN_LinearFwd, SWAN_LinearDown, SWAN_QuadFwd, SWAN_QuadDown };
    bool drifted = false;
    float now[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; ++i) {
        if (!SafeRead(obj + offs[i], &now[i], sizeof(float))) return;  // instance validator's job
        if (fabsf(now[i] - g_swanSaved[i] * mul) > 0.01f) drifted = true;
    }
    if (!drifted) { g_swanStandDownFrame = -1; return; }
    const bool ok = WriteF32(obj + SWAN_LinearFwd,  g_swanSaved[0] * mul) &&
                    WriteF32(obj + SWAN_LinearDown, g_swanSaved[1] * mul) &&
                    WriteF32(obj + SWAN_QuadFwd,    g_swanSaved[2] * mul) &&
                    WriteF32(obj + SWAN_QuadDown,   g_swanSaved[3] * mul);
    static long asserted = 0;
    ++asserted;
    char ends[48] = "";
    if (g_swanStandDownFrame >= 0) {
        sprintf_s(ends, " - ends %ld-frame game-owned span", g_frames - g_swanStandDownFrame);
        g_swanStandDownFrame = -1;
    }
    Log("*** [swan] ASSERT #%ld mode %d at frame %ld t=%.2fs (move %s(%d), calm %ld frames):"
        " overwrote linear %.1f/%.1f quadratic %.1f/%.1f%s%s", asserted, g_swanMode,
        g_frames, LogSecs(), MoveName(move), (int)move, calmStreak,
        now[0], now[1], now[2], now[3], ends, ok ? "" : "  <- A WRITE FAILED");
}

// F7 toggles it. A raw key read, like PAUSE - this is a test lever, and an A/B the player can
// perform without relaunching is worth far more than one that needs two runs to compare.
static void CheckSwanHotkey()
{
    if (!g_debug) return;
    static bool wasDown = false;
    const bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (down && !wasDown) {
        g_swanMode = (g_swanMode + 1) % 3;
        static const char* kNames[3] = { "original", "zeroed", "exaggerated 8x" };
        Log("[swan] F7 -> mode %d (%s)", g_swanMode, kNames[g_swanMode]);
        // A keypress is an explicit request, so it clears the backoff and retries at once.
        // Otherwise F7 can look dead for up to 600 frames while the backoff is armed - and
        // "F7 did nothing" is precisely the report that has to be unambiguous.
        g_swanNextTry = 0;
        g_swanMissLogged = false;
        if (!g_swanNeck)
            Log("[swan] (no instance resolved yet - retrying now, the write follows if found)");
    }
    wasDown = down;
}

// Rung 6a lives below but is driven from here, so its handful of shared symbols are declared
// up front rather than reordering two large blocks.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetVSConstF)(IDirect3DDevice9*, UINT, const float*, UINT);
extern PFN_SetVSConstF g_origSetVSConstF;
extern float g_eyeInject;
extern uintptr_t g_playerPawn;
extern int  g_vmCandidates;
extern int  g_vmBestReg;
extern bool g_vmBestRow;
extern bool  g_vmBestWorld;
extern float g_vmBestScore;
extern volatile LONG g_vmScanArmed;
extern volatile LONG g_vmWindowsTested;
extern volatile LONG g_vmPoseFailures;
// The watchdog's signal, raised by the render thread's constant hook and consumed in Present.
extern volatile LONG g_vmRescanRequest;
// Whether the committed register has ever been seen carrying the view, and how many substantial
// windows it has failed while it has not. Both are cleared by whoever commits a register.
extern bool          g_vmProven;
extern int           g_vmStrikes;
extern bool  g_vmValidate;
extern int   g_vmReg;
extern bool  g_vmRow;
extern int   g_vmMode;
extern float g_vmOffset[3];
extern float g_eyeInject;
extern int   g_stereoMode;
extern float g_worldScale;
extern float g_stereoStrength;
bool      LooksLikePlayerPawn(uintptr_t obj);
uintptr_t FindPlayerPawn();
bool      GetCameraPose(float* loc, float* fwd);

// ================================================================ rung 5b: head tracking
//
// Writes the headset's orientation into TdPlayerController::Rotation (+0x00F4), which the
// decompiled UpdateRotation uses as the base for every frame:
//
//     ViewRotation = Rotation;  ... input delta ...  SetRotation(ViewRotation);
//
// So a write there is read back rather than discarded. That is the OPPOSITE of what the
// Singularity project found, where writes to the controller's rotation were overwritten each
// frame by a native rotation source and a detour was needed. Worth stating plainly because it
// is an inherited premise that does NOT transfer, and this rung is the test of it.
//
// ---- a DELTA is folded in, not an absolute pose ----
//
// Writing the head pose absolutely would fight mouse and stick input: the engine adds its own
// delta to the same field, so an absolute write throws that away every frame. Adding only the
// CHANGE in head orientation since the previous frame lets both compose - the reference
// project reached the same design and describes it as "mouse and head tracking now add rather
// than fight".
//
// ---- ⚠️ UE3 yaw does not wrap on int32 ----
//
// 65536 units is a full turn, so two headings 0.3 degrees apart differ by 65590 and a raw
// subtraction reads as 360.3 degrees. The Singularity project lost months to exactly this,
// with two sites carrying comments claiming the wrap was correct. Casting the difference to
// int16 gives the shortest signed path for free, because 65536 IS 2^16.

// g_viewSpace is declared up with the other XR handles, since InitXR creates it.
static uintptr_t g_playerCtl = 0;
static long      g_ctlNextTry = 0;
static bool      g_ctlMissLogged = false;
static bool      g_headTracking = true;
// Both negative, MEASURED rather than reasoned. OpenXR is right-handed with Y up and -Z
// forward; UE3 is left-handed with Z up. Rather than argue the conversion out on paper, the
// first run shipped +1/+1 with F5 and F4 to flip them, and both came back inverted.
// ⚠️ pitch is +1 now, and that is a consequence of the HeadBasis fix rather than a taste change.
//
// It was -1 because the measured pitch came out inverted, and F4 was flipped to cancel it. The
// inversion was the sign error on the forward vector's y component; with that corrected the
// measurement is the right way up and a second negation would put it back. Yaw is untouched -
// it reads only x and z, which were always correct, so its -1 is the genuine handedness between
// OpenXR's anticlockwise yaw and UE3's clockwise one.
static int       g_yawSign = -1, g_pitchSign = +1;

// Pitch anchored to the head rather than accumulated. NUMPAD1 toggles, NUMPAD2 cycles how much
// of the camera animation is kept. See the write site for why yaw is not treated the same way.
bool             g_pitchAbsolute = true;
// One switch per axis, independently settable, so any combination is reachable. They are three
// different mechanisms underneath - pitch and yaw travel through Controller.Rotation and have to
// be separated from the head's own contribution; roll never touches that field at all and is
// read straight out of the matrix - but that is an implementation detail and not a reason to
// give the player a coupled control.
//
// All default to following, so the game behaves as it shipped until asked otherwise. Yaw is the
// one to leave alone in most cases: an animation that turns the player is carrying them
// somewhere, and cancelling it leaves the body facing one way and the view another.
bool             g_animFollow     = true;   // PITCH; NUMPAD2
bool             g_animRollFollow = true;   // ROLL;  NUMPAD4
bool             g_animYawFollow  = true;   // YAW;   NUMPAD5
extern float     g_animNow[3];              // camera animation contribution, degrees, P/Y/R
static bool      g_headPrimed = false;
static int32_t   g_lastHeadYaw = 0, g_lastHeadPitch = 0;
static long      g_headWrites = 0;
static long      g_headJumpsRejected = 0;

// The shortest signed path between two UE3 angles. Never subtract them raw.
static inline int32_t YawDelta(int32_t a, int32_t b) { return (int16_t)((int32_t)(a - b)); }

// ---- does the player own the camera right now? ----
//
// The transition-freeze investigation's endpoint (runs 19:20/20:14 plus the player's report:
// the fall's audio completes while the view hangs mid-air; head-look dead on the pipe). The
// game runs its scripted-camera sequences - the slow-motion death fall (out-of-enum state
// 72), the hard-land recovery (Landing), the pipe-catch dangle (IntoClimb/Climb) - through
// the same controller/camera fields the steering below writes every frame, and the [vm]
// windows prove the game kept rendering from those fields through every freeze (0.0-0.5%
// rejection, tolerance auto-widening as designed). Re-asserting the headset pose into the
// controller there is the swan lesson (run 26) replayed on the camera path: an every-frame
// fight with a game-side value inside states whose logic round-trips through it. The result
// was not judder this time but a frozen view over a running game.
//
// The steering therefore stands down outside states where the player demonstrably owns the
// camera. The whitelist is the hands-eligibility family INCLUDING the airborne states -
// head-look during ordinary jumps and falls is core VR function, and those states measured
// clean across 30+ transitions in the marked runs. Unreadable and unknown states (72
// included) fail CLOSED, the polarity run 27 proved on the swan. Re-entry waits out a dwell
// so the resume cannot land inside a sequence's tail - the same reasoning as kSwanCalmDwell,
// and cheap here because pitch stays display-corrected in the matrix regardless; only the
// yaw steering pauses perceptibly, for under a second.
//
// This deliberately narrows the older rule above the absolute-pitch write ("the frames where
// something else moved the camera need it most"): that reasoning holds for calm-state camera
// profile swaps, and standing down here does not touch it - the absolute anchor still runs
// every owned frame. It was wrong only where the "something else" is a sequence the game is
// executing through these fields.
static const long kHeadOwnDwell = 45;
static bool HeadSteeringOwned()
{
    static long ownedStreak = 0;
    static long standDownFrame = -1;    // frame the current game-owned window began, -1 = none
    uint8_t move = 0xFF;
    if (!(g_playerPawn && g_offMoveState >= 0 &&
          SafeRead(g_playerPawn + g_offMoveState, &move, 1)))
        move = 0xFF;
    const bool owned = (move == 1 || move == 2 || move == 11 || move == 15 ||
                        move == 24 || move == 29 || move == 30);
    if (!owned) {
        if (standDownFrame < 0) {
            standDownFrame = g_frames;
            Log("*** [head] camera is game-owned (move %s(%d)) at frame %ld t=%.2fs -"
                " steering stands down", MoveName((int)move), (int)move, g_frames, LogSecs());
        }
        ownedStreak = 0;
        return false;
    }
    ++ownedStreak;
    if (ownedStreak < kHeadOwnDwell) return false;
    if (standDownFrame >= 0) {
        Log("[head] steering resumes at frame %ld t=%.2fs (move %s(%d)) after %ld-frame"
            " game-owned window", g_frames, LogSecs(), MoveName((int)move), (int)move,
            g_frames - standDownFrame);
        standDownFrame = -1;
    }
    return true;
}

static bool LooksLikePlayerController(uintptr_t obj)
{
    if (!obj || g_offName < 0) return false;
    uint32_t vt;
    if (!SafeU32(obj, &vt) || !InModule(vt)) return false;
    if (!ObjNameIs(obj, "TdPlayerController")) return false;
    uint32_t cls;
    if (!SafeU32(obj + 0x34, &cls) || cls < 0x10000) return false;
    return ObjNameIs(cls, "TdPlayerController");   // the UClass's own Class is "Class"
}

static uintptr_t FindPlayerController()
{
    if (LooksLikePlayerController(g_playerCtl)) return g_playerCtl;
    if (g_playerCtl) {
        Log("[head] controller %p no longer valid, re-resolving", (void*)g_playerCtl);
        g_playerCtl = 0;
        g_headPrimed = false;
        g_ctlNextTry = 0;
    }
    if (g_frames < g_ctlNextTry) return 0;
    if (!g_gobjAddr || g_offName < 0) { g_ctlNextTry = g_frames + 600; return 0; }

    uint32_t data, count;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) {
        g_ctlNextTry = g_frames + 600; return 0;
    }
    // Time-sliced like the swan and pawn walks - this was the last full walk left on the
    // render thread, and it fires on exactly the churny moments (transitions, possession
    // changes) where a multi-second freeze hurts most.
    static uint32_t walkCursor = 0;
    static double walkMs = 0.0;
    const double t0 = NowMs();
    const double kBudgetMs = 3.0;
    if (walkCursor >= count) walkCursor = 0;
    uint32_t i = walkCursor;
    for (; i < count; ++i) {
        if ((i & 0x3F) == 0 && NowMs() - t0 > kBudgetMs) break;
        uint32_t obj;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        if (!LooksLikePlayerController(obj)) continue;
        g_playerCtl = obj;
        g_ctlMissLogged = false;
        Log("[head] TdPlayerController instance at %p (amortized walk, %.1f ms total)",
            (void*)obj, walkMs + NowMs() - t0);
        walkCursor = 0; walkMs = 0.0;
        return obj;
    }
    walkMs += NowMs() - t0;
    walkCursor = i;
    if (i < count) return 0;               // pass continues next call at the cursor

    walkCursor = 0;
    // Backoff, for the reason recorded at FindSwanNeck: an uncached full walk on the render
    // thread costs hundreds of thousands of syscalls a frame.
    g_ctlNextTry = g_frames + 600;
    if (!g_ctlMissLogged) { g_ctlMissLogged = true; Log("[head] no live TdPlayerController yet"); }
    walkMs = 0.0;
    return 0;
}

// The head's forward and up axes in room space, from the pose quaternion.
//
// ⚠️ ONE copy, and that is the point. These lines were pasted into three places and one copy
// carried a sign error on fwd[1] - `2*(yz - wx)` where the negated third column of the rotation
// matrix is `-2*(yz - wx)`. The forward vector was reflected through the horizontal plane.
//
// It hid for four rounds because of where it does and does not matter:
//
//   * pitch came out negated, which the F4 sign toggle had already been flipped to absorb. A
//     real defect cancelled by a setting chosen to make the symptom go away.
//   * yaw uses only x and z, so it was never affected.
//   * roll was fine at eye level and catastrophic when pitched. The level-up reference is
//     worldUp minus its forward component, so at level pitch it is (0,1,0) either way, but as
//     the head pitches up the reference lies down towards horizontal and the sign error flips
//     its horizontal part - which by 68 degrees is nearly the whole vector. The measured roll
//     read 180 degrees and the image turned upside down.
//
// Which is why this is now a function. Three copies of a formula is three chances to get it
// wrong and one place where being wrong is obvious.
static void HeadBasis(const XrQuaternionf& q, float fwd[3], float up[3])
{
    // Columns of the quaternion's rotation matrix. OpenXR is right-handed with Y up and -Z
    // forward, so forward is the NEGATED third column and up is the second, unchanged.
    fwd[0] = -2.0f * (q.x * q.z + q.w * q.y);
    fwd[1] = -2.0f * (q.y * q.z - q.w * q.x);
    fwd[2] = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    up[0]  =  2.0f * (q.x * q.y - q.w * q.z);
    up[1]  =  1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    up[2]  =  2.0f * (q.y * q.z + q.w * q.x);
}

// The head's levelled yaw in radians, room space, with no state of its own.
//
// ⚠️ Deliberately NOT GetHeadYawPitch. That function holds its last good yaw in a static across
// calls, to ride out the singularity when looking straight up - correct for the one caller that
// drives the game's rotation, and wrong to share. The lag correction calls this twice per frame
// at two different times, from a different thread; routing that through the same static would
// have each caller's hold state overwriting the other's, and near vertical they would disagree
// about what "last good" meant. Two callers, one static, is a bug waiting for a pose.
static bool GetHeadYawRaw(XrTime when, float* outYaw)
{
    if (g_viewSpace == XR_NULL_HANDLE || g_xrSpace == XR_NULL_HANDLE) return false;
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(g_viewSpace, g_xrSpace, when, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;

    float f[3], u[3];
    HeadBasis(loc.pose.orientation, f, u);
    if (sqrtf(f[0]*f[0] + f[2]*f[2]) < 0.05f) return false;   // near vertical: no yaw to read
    *outYaw = atan2f(f[0], f[2]);
    return true;
}

// The head's pitch in radians, with no state of its own - the companion to GetHeadYawRaw, and
// separate from GetHeadYawPitch for the same reason: that one holds a static across calls and
// must not be shared with a caller on another thread asking about another time.
static bool GetHeadPitchRaw(XrTime when, float* outPitch)
{
    if (g_viewSpace == XR_NULL_HANDLE || g_xrSpace == XR_NULL_HANDLE) return false;
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(g_viewSpace, g_xrSpace, when, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;

    float f[3], u[3];
    HeadBasis(loc.pose.orientation, f, u);
    *outPitch = atan2f(f[1], sqrtf(f[0]*f[0] + f[2]*f[2]));
    return true;
}

// Head orientation as UE3 rotator units. Yaw and pitch are taken from the forward vector
// rather than an Euler decomposition, which avoids the ambiguity near vertical.
static bool GetHeadYawPitch(XrTime when, int32_t* outYaw, int32_t* outPitch)
{
    if (g_viewSpace == XR_NULL_HANDLE || g_xrSpace == XR_NULL_HANDLE) return false;
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(g_viewSpace, g_xrSpace, when, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;

    float f[3], u[3];
    HeadBasis(loc.pose.orientation, f, u);
    const float fx = f[0], fy = f[1], fz = f[2];

    const float len      = sqrtf(fx * fx + fz * fz);
    const float pitchRad = atan2f(fy, len);

    // ⚠️ Near vertical the forward vector's horizontal part collapses and atan2 on it returns
    // whatever the noise says - it can swing the full 360 between one frame and the next. Head
    // tracking writes DELTAS, so a yaw that jumps 180 degrees is written as a 180 degree turn:
    // the reported "look up or down far enough and the screen flips".
    //
    // Pitch is unaffected - it comes from fy, which is largest exactly where this fails - so the
    // last good yaw is held while the view keeps pitching. Nothing is lost: at 87 degrees of
    // pitch there is no meaningful facing to track anyway, and the held value is correct again
    // the moment the head comes back down.
    static float lastYaw = 0.0f;
    if (len > 0.05f) lastYaw = atan2f(-fx, -fz);
    const float yawRad = lastYaw;

    const float kToUE = 32768.0f / 3.14159265f;    // pi radians == 32768 UE3 units
    *outYaw   = (int32_t)(yawRad   * kToUE);
    *outPitch = (int32_t)(pitchRad * kToUE);
    return true;
}

// Head roll in radians: the signed angle, about the view's forward axis, between the headset's
// own up vector and a level reference.
//
// Taken from vectors rather than an Euler decomposition, because an Euler order has to be
// assumed and the wrong one silently mixes roll with yaw near vertical. This assumes nothing:
// project world-up perpendicular to where the head is looking, and measure the angle to the
// head's actual up.
//
// Degenerate when looking straight up or down, where "level" has no meaning - detected by the
// projection collapsing, and the previous value is held rather than snapping to zero.
static bool GetHeadRoll(XrTime when, float* outRoll, float* outWeight)
{
    if (g_viewSpace == XR_NULL_HANDLE || g_xrSpace == XR_NULL_HANDLE) return false;
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(g_viewSpace, g_xrSpace, when, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;

    float f[3], u[3];
    HeadBasis(loc.pose.orientation, f, u);
    const float fx = f[0], fy = f[1], fz = f[2];
    const float ux = u[0], uy = u[1], uz = u[2];

    // World up with the forward component removed: the "level up" for this facing.
    const float d = fy;                       // dot(worldUp, forward), worldUp = (0,1,0)
    float lx = -d * fx, ly = 1.0f - d * fy, lz = -d * fz;
    const float ll = sqrtf(lx*lx + ly*ly + lz*lz);

    // ⚠️ ll is cos(pitch), and the cutoff has to TAPER rather than switch.
    //
    // A hard `ll < 0.05` cutoff only held the roll within 3 degrees of vertical, and that is far
    // too late. The roll angle is already violently sensitive well before the reference actually
    // collapses: near vertical a small turn of the head sweeps the level-up reference right
    // around the forward axis, so the computed roll can swing most of a turn between frames. It
    // presented as the image rotating when looking all the way up or down - the reported "flip",
    // which is a roll and not a yaw.
    //
    // So the weight fades to zero over a band instead: full tracking below about 72 degrees of
    // pitch, fading out to held by about 84. The cost is that roll is not tracked while staring
    // at the ceiling or the floor, where "level" is barely meaningful anyway and there is no
    // stable answer to track.
    if (ll < 0.10f) return false;             // near-vertical: level is undefined, hold
    float w = (ll - 0.10f) / 0.20f;
    if (w > 1.0f) w = 1.0f;
    *outWeight = w;

    lx /= ll; ly /= ll; lz /= ll;

    // Signed angle from level-up to head-up, measured about forward.
    const float cosA = lx*ux + ly*uy + lz*uz;
    const float sinA = (ly*uz - lz*uy) * fx + (lz*ux - lx*uz) * fy + (lx*uy - ly*ux) * fz;
    *outRoll = atan2f(sinA, cosA);
    return true;
}

// Defined with the stereo code below; driven from here because the head pose is sampled once
// per frame in this function and both roll and 6-DOF read it.
void UpdateSixDof(XrTime when);
extern bool  g_sixDof;
extern bool  g_haveCentre;
extern float g_dofOffset[3];
extern float g_sceneMat[16];      // read by the steep-pitch trace below
extern bool  g_sceneMatValid;

static void ApplyHeadTracking(XrTime when)
{
    // Sampled here rather than in the D3D hook: the hook runs thousands of times a frame and
    // an xrLocateSpace per call would be absurd. One sample per frame, used by every upload.
    {
        // ⚠️ Roll is NOT sampled here any more. It moved to the render thread, inside the frame.
        //
        // Sampled in Present it described the PREVIOUS frame, and the image was drawn with that
        // value while the pose submitted alongside carried the CURRENT roll. A projection layer
        // reprojects by the difference between the pose it is handed and the pose at display, so
        // a stale roll in the image gets corrected against as though it were current - the
        // compositor faithfully removing a discrepancy that exists only because the two halves
        // were sampled a frame apart.
        //
        // Same class as the yaw lag and the same fix: sample it where the frame is actually
        // being drawn, for the time it will actually be seen.
        //
        // The steep-pitch trace that used to sit here is gone with it. It was written to find the
        // rotation when looking straight up, it found it - the sign error in HeadBasis - and a
        // diagnostic kept past its answer is just a slower build and a longer log.
        //
        // ⚠️ 6-DOF moved out too, and it was the LAST thing still sampled here. Its offset is a
        // TRANSLATION, so a stale one slides the world rather than turning it - and translation
        // error reads as the picture swimming, which is what judder looks like from inside.
        //
        // Pitching the head moves the eyes further than yawing it does: they sit forward of the
        // neck, so a nod swings them through a vertical arc while a turn swings them through a
        // horizontal one about a nearer axis. A frame-old offset is therefore worst in pitch,
        // which is where the judder is, and it is the only remaining value with that property
        // now that the rotation path measures at half a degree on every axis.
        (void)when;
    }

    if (!g_headTracking || g_offActorRotation < 0) return;
    const uintptr_t ctl = FindPlayerController();
    if (!ctl) return;

    int32_t hy, hp;
    // Keeps the pawn pointer warm for the view-matrix scan, which needs the camera pose and
    // runs on the render thread inside a hot D3D hook where a full object walk is impossible.
    FindPlayerPawn();

    // Cache the camera position once per frame. The injection hook re-validates every upload
    // against it, and cannot afford a ReadProcessMemory per call - c0 is written thousands of
    // times a frame.
    {
        float loc[3], fwd[3];
        if (GetCameraPose(loc, fwd)) {
            g_camCache[0] = loc[0]; g_camCache[1] = loc[1]; g_camCache[2] = loc[2];
            // The DIRECTION is cached too now. Position alone cannot tell the scene view from
            // anything else rendered from the same point - see the acceptance test.
            g_camFwd[0] = fwd[0]; g_camFwd[1] = fwd[1]; g_camFwd[2] = fwd[2];
            g_camCacheValid = true;
        } else {
            g_camCacheValid = false;     // no pose means no way to tell the matrices apart
        }
    }

    if (!GetHeadYawPitch(when, &hy, &hp)) return;

    if (!g_headPrimed) {           // first sample defines the reference, no jump on connect
        g_lastHeadYaw = hy; g_lastHeadPitch = hp; g_headPrimed = true;
        Log("[head] primed at yaw %d pitch %d", hy, hp);
        return;
    }

    int32_t dYaw   = YawDelta(hy, g_lastHeadYaw)   * g_yawSign;
    int32_t dPitch = YawDelta(hp, g_lastHeadPitch) * g_pitchSign;
    g_lastHeadYaw = hy; g_lastHeadPitch = hp;
    // Stand down while the game owns its camera (death, hard-land recovery, scripted catch).
    // The reference above keeps tracking the head through the window, so the resume starts
    // from the current pose with nothing banked - the same no-jump property as priming.
    if (!HeadSteeringOwned()) return;
    // An absolute pitch has to be re-asserted every frame even when the head has not moved -
    // that IS the correction, and it is the frames where something else moved the camera that
    // need it most. Only the relative path can skip a still head.
    if (dYaw == 0 && dPitch == 0 && !g_pitchAbsolute) return;

    // ---- reject an implausibly large single-frame delta ----
    //
    // Measured: "primed at pitch 2103" then "write #1 dPitch +4246" - about 23 degrees applied
    // in one frame, which pointed the camera at the floor. The head had moved between priming
    // and the first write, because the player was putting the headset on, and the whole
    // accumulated movement arrived as a single step.
    //
    // No real head turns 23 degrees in one frame at 90 Hz. A delta that large means time
    // passed, not that the head moved that fast, and applying it is always wrong. Dropped and
    // counted rather than clamped: clamping would still inject a large bogus turn, just more
    // slowly.
    // dPitch only counts against this on the relative path. An absolute pitch cannot inject a
    // bogus turn no matter how large the step is - it names a destination, not a movement - and
    // letting it veto the write would block YAW as well, for a step that was harmless.
    const int32_t kMaxStep = 2000;                  // ~11 degrees, far above any real frame
    const bool bigPitch = !g_pitchAbsolute && (dPitch > kMaxStep || dPitch < -kMaxStep);
    if (dYaw > kMaxStep || dYaw < -kMaxStep || bigPitch) {
        if (++g_headJumpsRejected <= 5 || (g_headJumpsRejected % 100) == 0)
            Log("[head] rejected an implausible step (dYaw %+d dPitch %+d) - re-syncing, %ld so far",
                dYaw, dPitch, g_headJumpsRejected);
        return;                                     // lastHead is already updated, so we re-sync
    }

    // FRotator is {Pitch, Yaw, Roll} as int32. Roll is deliberately untouched: the decompiled
    // UpdateRotation sets ViewRotation.Roll = 0 before writing back, so head roll cannot
    // travel this path at all and needs the view matrix instead.
    // Every yaw delta we write is banked here for the lag correction to subtract. Accumulated
    // rather than latched, so it cannot be lost to the render thread reading between two writes.
    g_writtenYawAccum += (float)dYaw * (6.28318531f / 65536.0f);

    int32_t rot[3];
    if (!SafeRead(ctl + g_offActorRotation, rot, sizeof(rot))) return;

    // ---- pitch is ABSOLUTE, yaw stays relative, and the asymmetry is deliberate ----
    //
    // A delta scheme has no memory of where the head actually points, so every disturbance it
    // does not originate is permanent. A camera animation tilts the view, the player's neck does
    // not follow it exactly, the animation ends - and the offset between the physical head and
    // the rendered pitch stays for the rest of the session. That is the reported "annoying if
    // the animation ends with me looking at a different up/down rotation".
    //
    // Writing pitch absolutely removes the whole class: the rendered pitch is a function of
    // where the head is pointing now, so any disturbance corrects itself on the next frame,
    // whatever caused it - animation, the clamp, a dropped frame.
    //
    // Yaw cannot have the same treatment and it is not an oversight. Yaw has to accumulate
    // beyond the head's physical range or the player could never turn around, so mouse and stick
    // input must survive, which means the field has to be free to hold whatever the engine and
    // the player put there. Pitch has no such need - looking up is done by looking up.
    if (g_pitchAbsolute) {
        // The engine composes the camera as Controller.Rotation plus the animation contribution,
        // so writing the head pitch here renders as head + animation: the animation still
        // carries the view, and it unwinds by itself because the anchor is absolute.
        //
        // g_animFollow decides whether that is kept. ON is today's behaviour and the default -
        // a vault or a roll that moves the camera is a large part of how this game reads. OFF
        // takes the measured contribution back out, so the view holds still through the
        // animation while the body does whatever it likes.
        int32_t want = hp * g_pitchSign;
        if (!g_animFollow) {
            // ⚠️ RELAXED, not subtracted outright. Subtracting the measured contribution in one
            // step made the camera shake violently, and the loop is easy to see once written
            // down.
            //
            // The contribution is PlayerCameraRotation - Controller.Rotation, and part of it -
            // the swan neck - is a FUNCTION OF the controller's own pitch: TdSwanNeck pitches
            // the camera by an amount driven by where the view is already pointing. So writing
            // pitch minus the contribution changes the contribution, which changes what is
            // written next frame. Closed loop, unity gain, one frame of delay. That oscillates,
            // and it oscillates hard.
            //
            // A first-order approach breaks it: move a fraction of the way each frame, so the
            // loop gain is scaled by that fraction and settles instead of ringing. It converges
            // in about a fifth of a second, which is faster than any animation it has to cancel.
            //
            // Clamped as well, because a loop that can still misbehave under some pose should
            // not be able to point the view at the sky - 25 degrees is far more than any
            // contribution measured here.
            static float corr = 0.0f;                      // UE3 units currently subtracted
            const float kUnitsPerDeg = 65536.0f / 360.0f;
            const float target = g_animNow[0] * kUnitsPerDeg;
            corr += 0.15f * (target - corr);
            const float kMax = 25.0f * kUnitsPerDeg;
            if (corr >  kMax) corr =  kMax;
            if (corr < -kMax) corr = -kMax;
            want -= (int32_t)corr;
        }
        rot[0] = want;

        // ⚠️ The pitch TARGET is no longer set here. It is sampled on the render thread, inside
        // the frame, for the time the frame will be seen - the same move roll made, for the same
        // reason. Set here it described the previous frame, and ApplyPitchFix spent every frame
        // correcting the view towards where the head used to be.
        //
        // The write above still uses this frame's `hp`, and should: Controller.Rotation is what
        // the GAME reads for aiming and movement, and a frame of latency there is invisible.
        // What is SEEN is corrected in the matrix, where it can be done for the right instant.
    } else {
        rot[0] += dPitch;
    }
    rot[1] += dYaw;

    // ⚠️ CLAMP THE PITCH. This is the "look all the way up or down and the image rotates".
    //
    // Measured: a write landed on pitch 16446, which is 90.3 degrees. Past 90 the camera has
    // gone OVER THE TOP - it is looking backwards and upside down - and the picture inverts.
    // That is the flip, and it is neither the roll nor the yaw singularity the previous two
    // commits went after.
    //
    // The engine never gets a chance to stop it. UE3's PlayerController clamps pitch against
    // ViewPitchMin/Max inside UpdateRotation; these writes go straight into the Rotation field
    // and never pass through it. The head's own pitch cannot exceed 90 - it comes from
    // atan2(fy, horizontal) - but this accumulates DELTAS on top of whatever else moves the
    // camera, so the total drifts past what any single sample could reach.
    //
    // 16000 is 87.9 degrees: close enough to straight up that nothing is lost, far enough from
    // the singularity that neither the roll reference nor the yaw can collapse there either.
    // Read back as a signed 16-bit rotator first, because the field holds 58697 for -6839.
    {
        const int32_t kPitchMax = 16000;
        int32_t p = rot[0] & 0xFFFF;
        if (p > 32767) p -= 65536;
        if (p >  kPitchMax) p =  kPitchMax;
        if (p < -kPitchMax) p = -kPitchMax;
        rot[0] = p;
    }
    SIZE_T wrote = 0;
    WriteProcessMemory(GetCurrentProcess(), (LPVOID)(ctl + g_offActorRotation), rot,
                       sizeof(int32_t) * 2, &wrote);

    if (++g_headWrites == 1 || (g_headWrites % 600) == 0)
        Log("[head] write #%ld  dYaw %+d dPitch %+d -> pitch %d yaw %d",
            g_headWrites, dYaw, dPitch, rot[0], rot[1]);
}

// The view-matrix scan runs for exactly one frame of draw calls.
//
// One frame, not continuous: the hook sits on a call made thousands of times per frame, and
// leaving the test live would be a permanent tax on the render thread for a question that
// only needs answering once. It also keeps the log readable.
//
// Factored out of the F6 handler so the startup sequence can arm it too. Both callers need
// the same prerequisites and the same countdown, and a second copy would be a second place
// for them to drift.
static int g_vmArmCountdown = 0;

// ---- how much evidence a scan must see before its answer is allowed to stand ----
//
// Separators between two MEASURED runs, not guessed thresholds. Two launches four minutes
// apart, same build, same level:
//
//     20:31:02   windows tested 5551, candidates 417  -> c0 ROW WORLD, 0.0-0.9% rejected after
//     20:29:10   windows tested  304, candidates   6  -> c6 COL,       99.9-100% rejected after
//
// The second armed at the MAIN MENU, which uploads almost nothing. It committed a register
// that was never the view, set g_autoDone, and never looked again - so that whole session ran
// with no stereo and with eye offsets going into a foreign matrix. Every number is an order of
// magnitude apart, so anything between them separates the two; these sit in the middle.
static const LONG  kVMMinWindows    = 1000;
static const int   kVMMinCandidates = 50;

// ---- and why standing on the world origin disqualifies a scan outright ----
//
// TestWindow separates the real matrix from the noise by probing it TWICE: once with the camera
// position, once with the world origin. A matrix that maps the CAMERA to clip.w ~ 0 is the view;
// one that maps the ORIGIN there is a translated-world transform. The tolerances differ - 25.0
// against 1.0 - precisely because the two probes are asking different questions.
//
// Put the camera at the origin and they stop being two questions. The menu run logged it:
//
//     [vm] c5   COL  w(cam)=    0.010  w(origin)=    0.010  dotFwd=+1.0000
//     [vm] c6   COL  w(cam)=    1.000  w(origin)=    1.000  dotFwd=+1.0000
//
// Identical on every candidate. The test collapses into "does this matrix map the origin to
// zero", which every UI and 2D transform in the frame passes, and the scan picks one of them.
//
// 1000 UU is 10 m at the measured 100 UU/m. Refusing costs nothing: the startup sequence waits
// and tries again, and a real gameplay camera is thousands of UU out - the good run above read
// w(origin)=19044 - within seconds of the level starting.
static const float kVMMinCamDist = 1000.0f;

// Set when a scan ran but its answer was thrown away for lack of evidence. The startup sequence
// reads it to tell "I looked and the world was not there yet" from "I looked at the world and
// found nothing" - only the second is a real failure, and only it should burn a retry.
static bool g_vmScanRefused = false;

static bool ArmVMScan(bool quiet)
{
    if (!g_origSetVSConstF) {
        if (!quiet) Log("[vm] SetVertexShaderConstantF is not hooked - cannot scan");
        return false;
    }
    if (!LooksLikePlayerPawn(g_playerPawn)) {
        if (!quiet) Log("[vm] no TdPlayerPawn resolved yet - the scan needs the camera pose");
        return false;
    }

    // A pawn EXISTING was the only prerequisite before, and it is not enough - one exists at the
    // menu too. What the scan actually needs is a camera somewhere in the world.
    {
        float loc[3], fwd[3];
        if (!GetCameraPose(loc, fwd)) {
            if (!quiet) Log("[vm] the pawn has no readable camera pose yet - not scanning"
                            " (camLoc offset %d, camRot offset %d)", g_offCamLoc, g_offCamRot);
            return false;
        }
        const float dist = sqrtf(loc[0] * loc[0] + loc[1] * loc[1] + loc[2] * loc[2]);
        if (dist < kVMMinCamDist) {
            if (!quiet)
                Log("[vm] camera is %.0f UU from the world origin, under the %.0f needed to tell"
                    " the two probes apart - not scanning. This is the menu, or a level that has"
                    " not placed the player yet; either way it clears as soon as they move.",
                    dist, kVMMinCamDist);
            return false;
        }
    }

    g_vmCandidates = 0; g_vmBestReg = -1; g_vmBestScore = -1e9f;
    InterlockedExchange(&g_vmWindowsTested, 0);
    InterlockedExchange(&g_vmPoseFailures, 0);
    Log("");
    Log("[vm] ---- scanning one frame of vertex shader constants ----");
    InterlockedExchange(&g_vmScanArmed, 1);
    g_vmArmCountdown = 2;                 // this Present, then the next frame's draws
    return true;
}

// Set by any manual press of the keys the startup sequence drives. Once the user has taken
// the wheel the sequence stops, so it can never undo a deliberate toggle.
static bool g_autoDone = false;

static void CheckVMHotkey()
{
    static bool p6 = false;
    const bool d6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;

    // F6 survives Debug=off deliberately - see g_debug. It is the only way back if the startup
    // sequence gives up, and it is what that message tells the player to press.
    if (d6 && !p6) { g_autoDone = true; ArmVMScan(false); }
    p6 = d6;

    // ⚠️ Everything from here to the countdown is diagnostic and gated; the countdown is NOT.
    // It is the scan's commit step, and returning early past it would arm a scan that never
    // lands - stereo would simply never come on with debugging off.
    if (g_debug) {

    // F2 cycles the injection. A 300 UU offset is far larger than any per-eye separation will
    // ever be, on purpose: the point of this rung is an unmistakable yes or no, and a subtle
    // change would repeat the mistake the swan-neck test made twice.
    static bool p2 = false;
    const bool d2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (d2 && !p2) {
        if (g_vmReg < 0) {
            Log("[vm] F2 ignored - no matrix committed yet, press F6 to scan first");
        } else {
            // UE3 world axes: X forward, Y right, Z up. Kept local rather than shared with the
            // injection block, which sits further down the file - one small table duplicated
            // beats another pair of extern declarations threaded through it.
            static const float kOffsets[4][3] = {
                { 0, 0, 0 }, { 300, 0, 0 }, { 0, 300, 0 }, { 0, 0, 300 }
            };
            g_vmMode = (g_vmMode + 1) % 4;
            g_vmOffset[0] = kOffsets[g_vmMode][0];
            g_vmOffset[1] = kOffsets[g_vmMode][1];
            g_vmOffset[2] = kOffsets[g_vmMode][2];
            static const char* kNames[4] = { "OFF", "forward 300", "right 300", "up 300" };
            Log("*** [vm] F2 -> injection %s", kNames[g_vmMode]);
        }
    }
    p2 = d2;
    }   // end if (g_debug)

    if (g_vmArmCountdown > 0 && --g_vmArmCountdown == 0) {
        InterlockedExchange(&g_vmScanArmed, 0);
        // These three numbers separate failures that previously all read the same. Zero
        // windows tested means the scan never ran at all, which is a completely different
        // problem from windows tested and none matching - and the first version reported both
        // as "no candidate matched", which cost a run.
        // Snapshotted through the interlocked path: they are written from the render thread's
        // hook, and mixing a plain read with interlocked writes is what C28112 objects to.
        const LONG tested = InterlockedCompareExchange(&g_vmWindowsTested, 0, 0);
        const LONG poseFail = InterlockedCompareExchange(&g_vmPoseFailures, 0, 0);
        Log("[vm] windows tested %ld, camera-pose failures %ld, candidates %d",
            tested, poseFail, g_vmCandidates);
        if (tested == 0)
            Log("[vm] NOTHING WAS TESTED - the camera pose was unavailable (camLoc offset %d,"
                " camRot offset %d, pawn %p)", g_offCamLoc, g_offCamRot, (void*)g_playerPawn);
        else if (g_vmCandidates == 0)
            Log("[vm] windows were tested but none matched - the matrix may not be uploaded as"
                " vertex shader constants, or the probes/tolerances are wrong");
        // ⚠️ A BEST is not an ANSWER. Ranking picks the least-bad of whatever it was shown, and
        // shown six matrices from a menu frame it returns one of the six with score 1.0000 -
        // the score says "this beat its rivals", never "there were rivals worth beating".
        //
        // So the count is the evidence and the score is not. Refusing here leaves g_vmReg at -1,
        // which is the safe state: no register means nothing is injected, where a WRONG register
        // means the eye offsets land in some other pass and the frame comes apart.
        else if (tested < kVMMinWindows || g_vmCandidates < kVMMinCandidates) {
            g_vmScanRefused = true;
            Log("*** [vm] REFUSING TO COMMIT - %ld windows and %d candidates is not a frame that"
                " was rendering the world (need %ld and %d)",
                tested, g_vmCandidates, kVMMinWindows, kVMMinCandidates);
            Log("[vm] best on offer was c%d %s, score %.4f - discarded. Will rescan.",
                g_vmBestReg, g_vmBestRow ? "ROW" : "COL", g_vmBestScore);
        }
        else {
            Log("*** [vm] BEST: c%d %s in %s space  (score %.4f, %d candidates seen)",
                g_vmBestReg, g_vmBestRow ? "ROW" : "COL",
                g_vmBestWorld ? "WORLD" : "TRANSLATED-WORLD", g_vmBestScore, g_vmCandidates);
            // Committed only on a successful scan, so injection can never run against a
            // register nothing validated.
            g_vmReg = g_vmBestReg;
            g_vmRow = g_vmBestRow;
            // A new register has proved nothing yet, whatever the last one managed.
            g_vmProven = false;
            g_vmStrikes = 0;
            Log("[vm] committed.%s", g_debug
                ? " F2 cycles the injection: off / forward / right / up (300 UU)" : "");
        }
        Log("");
    }
}

// ---- startup sequence: what F6 -> F1 -> F10 used to do by hand ----
//
// Those three keys were a bring-up scaffold. Each rung needed its predecessor confirmed before
// the next was worth switching on, so each got a key. All three are settled now, and typing
// them into a game you cannot see - because the headset is already on by then - is not a test,
// it is a chore.
//
// This is a sequence rather than a set of defaults because the steps have real prerequisites.
// The scan needs a hooked device AND a resolved pawn AND a valid camera pose, none of which
// exist at DLL load or at the main menu; stereo needs the register the scan commits. Flipping
// the flags on at startup would just arrange for all three to fail in order.
//
// It retries rather than firing once: the pawn appears at level load, and the camera pose can
// still be junk for a moment after that. Retrying costs a pointer read per attempt.
//
// The state lives outside the function so the watchdog can put the sequence back to the start.
// It was function-local while a run only ever armed once; a rescan is a second arming, and
// statics that cannot be reset would have made the watchdog fire into a sequence that had
// already decided it was finished.
static int  g_autoStage    = 0;     // 0 = waiting for the world, 1 = scan armed
static int  g_autoWait     = 0;
static int  g_autoRetries  = 0;
static int  g_autoAttempts = 0;     // every 20th stage-0 poll reports its reason

static void ResetAutoArm()
{
    g_autoStage = 0; g_autoWait = 0; g_autoRetries = 0; g_autoAttempts = 0;
    g_vmScanRefused = false;
    g_autoDone = false;
}

static void AutoArm()
{
    if (g_autoDone) return;

    if (!g_xrReady) return;       // no session, nothing to be stereo on
    if (g_autoWait > 0) { --g_autoWait; return; }

    if (g_autoStage == 0) {
        g_vmScanRefused = false;
        // ⚠️ Every twentieth attempt is LOUD, and the one-shot announcement it replaces cost a
        // run. The sequence polls every 30 frames and cannot report each one, but a single line
        // at the top of the log left "never armed at all" indistinguishable from "blocked on the
        // pawn" and from "blocked at the origin" - three different faults, one silence.
        const bool loud = (g_autoAttempts++ % 20) == 0;
        if (!ArmVMScan(!loud)) {
            g_autoWait = 30;
            return;
        }
        g_autoStage = 1;
        g_autoWait = 10;          // the scan itself needs 2 frames; this is slack, not a poll
        return;
    }

    if (g_vmReg >= 0) {
        g_stereoMode = 1;
        g_simulStereo = true;
        g_eyeFilled[0] = g_eyeFilled[1] = false;
        InterlockedExchange(&g_dupDraws, 0);
        g_autoDone = true;
        Log("*** [auto] ready - stereo ON, simultaneous. No keypresses needed.");
        Log(g_debug ? "[auto] F1 turns stereo off, F10 falls back to alternate-eye, F6 rescans."
                    : "[auto] Debug is off: F6 still rescans, PAGE UP recentres, hold PAUSE exits.");
        return;
    }

    // A scan that was thrown away for lack of evidence is not a failure to find the matrix, it
    // is a frame that was not drawing the world - a loading screen, a fade, a cutscene hand-off.
    // Waiting is the correct response and it must not consume the retry budget, or a slow level
    // load would exhaust ten attempts before the world appeared and then give up on it.
    if (g_vmScanRefused) {
        g_vmScanRefused = false;
        g_autoStage = 0;
        g_autoWait = 120;
        return;
    }

    // The scan ran and committed nothing. Almost always a camera pose that was not usable yet,
    // which fixes itself a second later - so retry, and give up loudly rather than silently.
    if (++g_autoRetries >= 10) {
        g_autoDone = true;
        Log("[auto] the scan committed no register after %d attempts - press F6 by hand", g_autoRetries);
        return;
    }
    g_autoStage = 0;
    g_autoWait = 120;
}

// ---- the watchdog: a committed register that has never carried the view ----
//
// The acceptance counter has printed the rate every 600 frames for a long time, and on the run
// that went wrong it printed 99.9% rejected eleven times in a row while nothing acted on it.
// Everything needed to notice was already being measured; noticing was the missing part.
//
// ⚠️ THE FIRST VERSION OF THIS FIRED ON A CORRECT REGISTER AND COST A SESSION.
//
// It fired whenever a 600-frame window rejected most of its uploads while the camera pose was
// valid, on the reasoning that a valid pose means we are looking at the player's view. That
// reasoning is wrong. A valid pose means the PAWN EXISTS - nothing more. The engine spends long
// stretches with a live pawn while rendering something that is not its camera, and the measured
// run walks through three of them on one committed-correct register:
//
//     33962 accepted,    65 rejected ( 0.2%)   gameplay
//      3451 accepted, 18308 rejected (84.1%)   dying - the view is not the new pawn's yet
//     71789 accepted,    67 rejected ( 0.1%)   gameplay again
//       119 accepted, 19076 rejected (99.4%)   the pause menu  <- fired here, dropped c0
//
// A wrong register is indistinguishable from a pause menu BY RATE, so rate cannot be the test
// and no threshold rescues it. What separates them is history: a correct register works within
// seconds of gameplay and keeps working, a wrong one has never worked once. The menu commit ran
// 99.9-100% rejected for ELEVEN consecutive windows and never had a good one.
//
// So the register is asked to prove itself, once. After that the watchdog is done with it for
// good, and menus, deaths and cutscenes cannot touch it however long they last. Before that it
// gets five substantial windows to manage a single one - which the correct register above did
// on its very first, and the wrong one never would.
static const long kVMJudgeSamples = 5000;   // a window smaller than this judges nothing
static const int  kVMMaxStrikes   = 5;      // ~50 s of never once working
static const int  kVMMaxRescans   = 5;
static int        g_vmRescans     = 0;

static void CheckVMWatchdog()
{
    // Nothing committed means nothing to be wrong about, and a sequence still working means it
    // owns the outcome - the watchdog judges commits that have been declared finished.
    if (g_vmReg < 0 || !g_autoDone) return;

    if (!InterlockedCompareExchange(&g_vmRescanRequest, 0, 0)) return;
    InterlockedExchange(&g_vmRescanRequest, 0);

    Log("");
    Log("*** [vm] WATCHDOG FIRED - the committed register has never carried the view");

    // Drop the register FIRST and unconditionally, including on the give-up path. A wrong
    // register is worse than none: none means the frame is merely flat, wrong means the eye
    // offsets are rewriting a pass that has nothing to do with the view.
    Log("[vm] dropping c%d. Nothing is injected until a scan commits again.", g_vmReg);
    g_vmReg = -1;
    g_vmProven = false;
    g_vmStrikes = 0;
    g_sceneMatValid = false;
    g_c0IsScene = false;

    if (++g_vmRescans > kVMMaxRescans) {
        Log("[vm] %d rescans have not held - not trying again. Press F6 by hand.", kVMMaxRescans);
        return;
    }
    Log("[vm] rescan %d of %d - restarting the startup sequence", g_vmRescans, kVMMaxRescans);
    ResetAutoArm();
}

extern bool g_overlay;
extern bool g_dupUI;   // NUMPAD0; defined with the user-pointer draw hooks
extern int   g_renderedEye;
extern float g_halfIpdUU;

static void CheckHeadHotkeys()
{
    // ---- always available, debugging or not ----
    //
    // Recentring is not a diagnostic. A headset put on at an angle needs it, and a player with
    // debugging off has nothing else that can fix the seating.
    {
        static bool pPgUpAlways = false;
        const bool dPgUpAlways = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
        if (dPgUpAlways && !pPgUpAlways) {
            RecenterSixDof();
            Log("*** [6dof] PAGE UP -> recentring head and motion hands together");
        }
        pPgUpAlways = dPgUpAlways;
    }
    if (!g_debug) return;

    static bool p9 = false, p5 = false, p4 = false;
    const bool d9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool d5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    const bool d4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    if (d9 && !p9) {
        g_headTracking = !g_headTracking;
        g_headPrimed = false;      // re-prime so re-enabling does not apply a stale delta
        Log("[head] F9 -> head tracking %s", g_headTracking ? "ON" : "OFF");
    }
    // NUMPAD, not another F-key: the F row and the navigation cluster are full, and the numpad
    // is one of the few blocks this game does not bind.
    static bool pN1 = false, pN2 = false;
    const bool dN1 = (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) != 0;
    const bool dN2 = (GetAsyncKeyState(VK_NUMPAD2) & 0x8000) != 0;
    if (dN1 && !pN1) {
        g_pitchAbsolute = !g_pitchAbsolute;
        Log("*** [head] NUMPAD1 -> pitch %s", g_pitchAbsolute
            ? "ABSOLUTE (anchored to the head; disturbances self-correct)"
            : "RELATIVE (accumulated deltas; the old behaviour, drift included)");
    }
    if (dN2 && !pN2) {
        g_animFollow = !g_animFollow;
        Log("*** [head] NUMPAD2 -> camera animations %s the view in pitch%s",
            g_animFollow ? "CARRY" : "do NOT move",
            g_pitchAbsolute ? "" : "  (no effect while pitch is RELATIVE)");
    }
    static bool pN3 = false, pN4 = false;
    const bool dN3 = (GetAsyncKeyState(VK_NUMPAD3) & 0x8000) != 0;
    const bool dN4 = (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) != 0;
    if (dN3 && !pN3) {
        g_pitchFix = !g_pitchFix;
        Log("*** [head] NUMPAD3 -> matrix pitch correction %s", g_pitchFix
            ? "ON (the view's pitch is fixed in the matrix, no frame of lag)"
            : "OFF (pitch comes from the engine's Rotation alone, one frame behind)");
    }
    if (dN4 && !pN4) {
        g_animRollFollow = !g_animRollFollow;
        Log("*** [head] NUMPAD4 -> camera animations %s the view",
            g_animRollFollow ? "ROLL" : "do NOT roll");
    }
    static bool pN5 = false;
    const bool dN5 = (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) != 0;
    if (dN5 && !pN5) {
        g_animYawFollow = !g_animYawFollow;
        Log("*** [head] NUMPAD5 -> camera animations %s the view",
            g_animYawFollow ? "TURN" : "do NOT turn");
    }
    // NUMPAD7 cycles the frame cap. 60 is in the list ahead of the higher values on purpose: it
    // is the only one that divides 120 exactly, so it is the smoothest CADENCE even though it is
    // fewer frames than the 62 the game ships with. Whether smooth-and-fewer beats
    // more-but-uneven is a question about eyes, not arithmetic, so it is a keypress.
    static bool pN7 = false;
    const bool dN7 = (GetAsyncKeyState(VK_NUMPAD7) & 0x8000) != 0;
    if (dN7 && !pN7) {
        // kCaps[0] IS the initial value at the top of the file, so the two cannot disagree and
        // the first press always moves somewhere. 72 and 144 are here because they are headset
        // rates in their own right: at 144 Hz it is 72 that divides evenly and 60 that does not,
        // which is the whole point of the annotation below.
        static const float kCaps[] = { 60.0f, 72.0f, 90.0f, 120.0f, 144.0f, 250.0f, 62.0f };
        static int ci = 0;
        ci = (ci + 1) % (int)(sizeof(kCaps) / sizeof(kCaps[0]));
        g_fpsCap = kCaps[ci];

        // ---- whether it divides evenly is a fact about THIS headset, not a constant ----
        //
        // "60 divides 120" was written into the log as literal text, which is true only while the
        // runtime is at 120 Hz. Ask the runtime instead: g_predPeriod is the display period it
        // reported for the last frame, so the ratio is the real one.
        const double hz = g_predPeriod ? (1.0e9 / (double)g_predPeriod) : 0.0;
        const double ratio = (hz > 1.0 && g_fpsCap > 0.0f) ? (hz / (double)g_fpsCap) : 0.0;
        const bool even = (ratio >= 0.99) &&
                          (fabs(ratio - floor(ratio + 0.5)) < 0.02);
        // Three outcomes, not two. A cap ABOVE the headset's rate is not an uneven cadence -
        // the compositor simply cannot show every frame, so work is thrown away. Calling that
        // "uneven" would point at the wrong problem, which is the one thing this line exists
        // to avoid.
        const char* verdict =
            (ratio < 0.995) ? "  <- ABOVE the headset's rate; frames are rendered and discarded"
          : even            ? "  <- EVEN CADENCE"
                            : "  <- uneven, frames will be held for differing times";
        if (hz > 1.0)
            Log("*** [fps] NUMPAD7 -> cap %.0f   headset is %.0f Hz, %.2f display periods per"
                " frame%s%s", g_fpsCap, hz, ratio, verdict,
                (g_fpsCap == 62.0f) ? "  (the game's own default, for comparison)" : "");
        else
            Log("*** [fps] NUMPAD7 -> cap %.0f  (no display period yet - headset rate unknown)",
                g_fpsCap);
    }
    pN7 = dN7;

    // NUMPAD8 switches the frame grab between the shared surface and the old CPU round trip.
    // THIS one can be a runtime toggle where the device type could not: the grab is chosen per
    // frame and owns nothing the game can see.
    // NUMPAD9 drops the synthesised pad. Worth having as a keypress rather than a rebuild: if the
    // game decides a controller is present it switches its prompts and can stop listening to the
    // keyboard, and that needs to be reversible from inside the headset.
    static bool pN9 = false;
    const bool dN9 = (GetAsyncKeyState(VK_NUMPAD9) & 0x8000) != 0;
    if (dN9 && !pN9) {
        g_padEnabled = !g_padEnabled;
        Log("*** [pad] NUMPAD9 -> motion controllers %s",
            g_padEnabled ? "ON (acting as a gamepad)" : "OFF (keyboard and mouse only)");
    }
    pN9 = dN9;

    static bool pN8 = false;
    const bool dN8 = (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) != 0;
    if (dN8 && !pN8) {
        g_fastCapture = !g_fastCapture;
        Log("*** [fast] NUMPAD8 -> frame grab %s", g_fastCapture
            ? "SHARED SURFACE (stays on the GPU)"
            : "CPU round trip (the known-good path)");
    }
    pN8 = dN8;

    static bool pN6 = false;
    const bool dN6 = (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0;
    if (dN6 && !pN6) {
        g_yawLagFix = !g_yawLagFix;
        Log("*** [head] NUMPAD6 -> turn-lag compensation %s (last measured %.2f deg)",
            g_yawLagFix ? "ON" : "OFF", g_yawLagRad * 57.29578f);
    }
    pN6 = dN6;

    // A render-path change that touches the HUD wants an A/B from inside the headset, not a
    // relaunch: with it off the HUD goes back to one full-width draw, half per eye.
    static bool pN0 = false;
    const bool dN0 = (GetAsyncKeyState(VK_NUMPAD0) & 0x8000) != 0;
    if (dN0 && !pN0) {
        g_dupUI = !g_dupUI;
        Log("*** [up] NUMPAD0 -> HUD duplication %s", g_dupUI ? "ON (both eyes)"
                                                              : "OFF (one full-width draw)");
    }
    pN0 = dN0;

    // All three at once, because the useful question is which COMBINATION is set and reading it
    // off three separate lines scattered through the log is how a test gets mis-attributed.
    if ((dN2 && !pN2) || (dN4 && !pN4) || (dN5 && !pN5))
        Log("[head] animation lock now: pitch %s  roll %s  yaw %s",
            g_animFollow ? "follow" : "LOCKED", g_animRollFollow ? "follow" : "LOCKED",
            g_animYawFollow ? "follow" : "LOCKED");
    pN1 = dN1; pN2 = dN2; pN3 = dN3; pN4 = dN4; pN5 = dN5;

    if (d5 && !p5) { g_yawSign   = -g_yawSign;   Log("[head] F5 -> yaw sign %+d", g_yawSign); }
    if (d4 && !p4) { g_pitchSign = -g_pitchSign; Log("[head] F4 -> pitch sign %+d", g_pitchSign); }
    static bool p3 = false;
    const bool d3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    if (d3 && !p3) { g_overlay = !g_overlay; Log("[hud] F3 -> overlay %s", g_overlay ? "ON" : "OFF"); }

    // BACKSPACE drops a numbered marker into the log at the player's say-so - "the judder is
    // happening NOW" - and opens a dense window: the arm-continuity watchdog logs every update
    // unconditionally for the next ~40, so the marked moment carries per-update data instead
    // of rate-limited samples. Ends the ambiguity of matching a report to a window after the
    // fact.
    static bool pMark = false;
    const bool dMark = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
    if (dMark && !pMark) {
        static long markerCount = 0;
        ++markerCount;
        Log("*** [mark] ======== USER MARKER #%ld at frame %ld t=%.2fs ========"
            " (next ~40 arm updates logged densely)", markerCount, g_frames, LogSecs());
        InterlockedExchange(&g_markerBurst, 40);
    }
    pMark = dMark;

    // Live calibration. Left/Right walks wrist R/L P/Y/R, then forearm R/L roll offset.
    static bool pLeft = false, pRight = false, pUp = false, pDown = false;
    const bool dLeft  = (GetAsyncKeyState(VK_LEFT)  & 0x8000) != 0;
    const bool dRight = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
    const bool dUp    = (GetAsyncKeyState(VK_UP)    & 0x8000) != 0;
    const bool dDown  = (GetAsyncKeyState(VK_DOWN)  & 0x8000) != 0;
    if (g_overlay && g_motionHands) {
        if (dLeft && !pLeft) {
            g_handTuneSelected = (g_handTuneSelected + 7) % 8;
        }
        if (dRight && !pRight) {
            g_handTuneSelected = (g_handTuneSelected + 1) % 8;
        }
        if ((dLeft && !pLeft) || (dRight && !pRight)) {
            if (g_handTuneSelected < 6) {
                const int axis = g_handTuneSelected % 3;
                Log("[hands-tune] selected WRIST %s %s",
                    g_handTuneSelected < 3 ? "RIGHT" : "LEFT",
                    axis == 0 ? "PITCH" : (axis == 1 ? "YAW" : "ROLL"));
            } else {
                Log("[hands-tune] selected FOREARM %s ROLL",
                    g_handTuneSelected == 6 ? "RIGHT" : "LEFT");
            }
        }
        int delta = 0;
        if (dUp && !pUp) delta += 5;
        if (dDown && !pDown) delta -= 5;
        if (delta) {
            const bool forearm = g_handTuneSelected >= 6;
            const int hand = forearm ? (g_handTuneSelected == 6 ? 1 : 0)
                                      : (g_handTuneSelected < 3 ? 1 : 0);
            const int axis = forearm ? 2 : (g_handTuneSelected % 3);
            int& value = forearm ? g_forearmRollCalibrationDeg[hand]
                                  : g_wristCalibrationDeg[hand][axis];
            value += delta;
            if (value > 180) value -= 360;
            if (value < -180) value += 360;
            Log("*** [hands-tune] WRIST R %+d %+d %+d L %+d %+d %+d |"
                " FOREARM ROLL R %+d L %+d"
                " (selected %s %s %s, %+d deg)",
                g_wristCalibrationDeg[1][0], g_wristCalibrationDeg[1][1],
                g_wristCalibrationDeg[1][2], g_wristCalibrationDeg[0][0],
                g_wristCalibrationDeg[0][1], g_wristCalibrationDeg[0][2],
                g_forearmRollCalibrationDeg[1], g_forearmRollCalibrationDeg[0],
                forearm ? "FOREARM" : "WRIST",
                hand ? "RIGHT" : "LEFT",
                axis == 0 ? "PITCH" : (axis == 1 ? "YAW" : "ROLL"),
                delta);
        }
    }
    pLeft = dLeft; pRight = dRight; pUp = dUp; pDown = dDown;
    static bool p8 = false;
    const bool d8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (d8 && !p8) {
        g_fovForce = !g_fovForce;
        Log("*** [fov] F8 -> forcing %s (%.1f x %.1f target)", g_fovForce ? "ON" : "OFF",
            g_targetHalfFovX * 114.5916f, g_targetHalfFovY * 114.5916f);
    }
    p8 = d8;

    // F12 restores the pre-fix injection: no per-upload validation, so every matrix arriving at
    // the register is offset, foreign ones included. Provided so the two behaviours can be
    // compared in one session rather than across two builds.
    static bool p12 = false;
    const bool d12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (d12 && !p12) {
        g_vmValidate = !g_vmValidate;
        Log("*** [vm] F12 -> per-upload validation %s%s", g_vmValidate ? "ON" : "OFF",
            g_vmValidate ? "" : "  (pre-fix behaviour: foreign matrices are offset too)");
    }
    p12 = d12;
    p3 = d3;

    // F1 toggles stereo. The mono quad path stays available on purpose: it is the known-good
    // fallback, and this session has repeatedly needed one.
    static bool p1 = false;
    const bool d1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (d1 && !p1) {
        g_autoDone = true;
        if (g_vmReg < 0) {
            Log("[eye] F1 ignored - no view matrix committed, press F6 first");
        } else {
            g_stereoMode = g_stereoMode ? 0 : 1;
            g_eyeFilled[0] = g_eyeFilled[1] = false;
            Log("*** [eye] F1 -> stereo %s", g_stereoMode ? "ON (alternate-eye)" : "OFF (mono quad)");
        }
    }
    p1 = d1;

    // F10 switches between alternate-eye and simultaneous. Alternate-eye is kept as the
    // fallback: it is known to work, and draw duplication touches far more of the render path.
    static bool p10 = false;
    const bool d10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (d10 && !p10) {
        g_autoDone = true;
        if (g_stereoMode != 1) {
            Log("[eye] F10 ignored - turn stereo on with F1 first");
        } else {
            g_simulStereo = !g_simulStereo;
            g_eyeFilled[0] = g_eyeFilled[1] = false;
            InterlockedExchange(&g_dupDraws, 0);
            Log("*** [eye] F10 -> %s stereo",
                g_simulStereo ? "SIMULTANEOUS (draw duplication)" : "ALTERNATE-EYE");
        }
    }
    p10 = d10;

    // INSERT bisects which scene-sized target is duplicated; DELETE overrides occlusion
    // queries. Two independent switches so one run separates the two hypotheses instead of
    // confounding them - which is what "test rather than guess" actually requires.
    static bool pIns = false, pDel = false;
    const bool dIns = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    const bool dDel = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
    if (dIns && !pIns) {
        g_dupOnlyTarget++;
        if (g_dupOnlyTarget >= g_rtSeenCount) g_dupOnlyTarget = -1;
        if (g_dupOnlyTarget < 0) Log("*** [dup] INSERT -> duplicating ALL scene-sized targets");
        else Log("*** [dup] INSERT -> duplicating ONLY target %d (%p %ux%u fmt %d)",
                 g_dupOnlyTarget, (void*)g_rtSeen[g_dupOnlyTarget].surf,
                 g_rtSeen[g_dupOnlyTarget].w, g_rtSeen[g_dupOnlyTarget].h,
                 (int)g_rtSeen[g_dupOnlyTarget].fmt);
    }
    if (dDel && !pDel) {
        // Cycles all three modes, because a toggle that can only force the override ON cannot
        // demonstrate anything: AUTO already overrides while duplication runs, so the previous
        // version's "on" was indistinguishable from its "off". Mode 2 turns culling back on and
        // is the only setting that can bring the flickering BACK - which is what makes this an
        // A/B rather than an assertion.
        g_occlusionMode = (g_occlusionMode + 1) % 3;
        g_forceVisible = false;                     // superseded by the mode
        static const char* kOcc[3] = { "AUTO (override while duplicating)",
                                       "ALWAYS override",
                                       "NEVER override - engine culling live" };
        Log("*** [occ] DELETE -> mode %d: %s", g_occlusionMode, kOcc[g_occlusionMode]);
    }
    pIns = dIns; pDel = dDel;

    // HOME toggles head roll, END flips its sign. A sign toggle because the OpenXR-to-UE3
    // handedness has already come out inverted for both yaw and pitch, and guessing a third
    // time is worse than one keypress.
    static bool pHome = false, pEnd = false;
    const bool dHome = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    const bool dEnd  = (GetAsyncKeyState(VK_END)  & 0x8000) != 0;
    if (dHome && !pHome) {
        g_rollEnabled = !g_rollEnabled;
        Log("*** [roll] HOME -> head roll %s", g_rollEnabled ? "ON" : "OFF");
    }
    if (dEnd && !pEnd) {
        g_rollSign = -g_rollSign;
        Log("*** [roll] END -> sign %+d", g_rollSign);
    }
    pHome = dHome; pEnd = dEnd;

    // PAGE UP recentres 6-DOF, PAGE DOWN toggles it. Recentre matters because the origin is
    // captured wherever the head happened to be at the first valid pose - typically mid-air
    // while the headset is being put on.
    // PAGE UP is handled at the top of this function, outside the debug gate.
    static bool pPgDn = false;
    const bool dPgDn = (GetAsyncKeyState(VK_NEXT)  & 0x8000) != 0;
    if (dPgDn && !pPgDn) {
        g_sixDof = !g_sixDof;
        Log("*** [6dof] PAGE DOWN -> %s", g_sixDof ? "ON" : "OFF (decaying to neutral)");
    }
    pPgDn = dPgDn;

    // F11 adjusts world scale. UE3 units per metre is game-specific and unmeasured here, and
    // it is the one number that decides whether the world feels life-sized.
    static bool p11 = false;
    const bool d11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (d11 && !p11) {
        // Stereo STRENGTH, not world scale. 0 is mono, 1.0 is the true 6.3 cm separation at
        // the measured 100 UU/m, and 1.5 is included so the hyperstereo failure is reachable -
        // being able to see what wrong looks like is what makes "is this right?" answerable.
        static const float kStrength[] = { 0.0f, 0.25f, 0.50f, 0.75f, 1.0f, 1.5f };
        static int si = 4;   // 1.00, the default
        si = (si + 1) % (int)(sizeof(kStrength) / sizeof(kStrength[0]));
        g_stereoStrength = kStrength[si];
        Log("*** [eye] F11 -> stereo strength %.0f%% (separation %.2f of %.2f UU, world scale fixed at %.0f)",
            g_stereoStrength * 100.0f, g_halfIpdUU * g_stereoStrength, g_halfIpdUU, g_worldScale);
    }
    p11 = d11;
    p9 = d9; p5 = d5; p4 = d4;
}

// ================================================================ rung 6a: find the view matrix
//
// Stereo, head roll and 6-DOF all need the world->clip matrix on its way to the GPU. UE3
// uploads it through SetVertexShaderConstantF; the Singularity project found it at register
// c0, stored as ROWS, in TRANSLATED-WORLD space.
//
// None of that is assumed here. Every part is re-derived, because this project has already
// been bitten by inherited facts twice - FNameEntry differed, and the controller-rotation
// premise inverted outright.
//
// ---- the tests, and why two are needed ----
//
// 1. A world->clip matrix maps the CAMERA POSITION to clip.w ~ 0: the eye is at the near
//    plane's origin. If UE3 renders in translated-world space the CPU has already subtracted
//    the view origin, so the point that passes is (0,0,0) rather than the camera's world
//    position. Testing BOTH says which space is in use.
//
// 2. ⚠️ At the origin the w test DEGENERATES. clip.w for p = (0,0,0) collapses to r[3].w under
//    BOTH storage conventions, so it cannot tell ROW from COL and it admits any matrix whose w
//    constant is near zero - which is most of them. The Singularity notes record 18 candidates
//    from this test alone.
//
//    The discriminator is direction: for a genuine world->clip matrix the xyz of the w term IS
//    the camera's forward axis, and ROW and COL read that from different components. We know
//    the real forward vector independently, from TdPlayerPawn::PlayerCameraRotation.
//
// Nothing is injected yet. This run reports what is there.

int g_offCamLoc = -1;
int g_offCamRot = -1;
int g_offCtlPawn = -1;
int g_offCtlCamera = -1;
int g_offCamViewTarget = -1;

uintptr_t g_playerPawn = 0;
static long      g_pawnNextTry = 0;
static bool      g_pawnMissLogged = false;
// Last pawn accepted and last pawn refused via the controller, so each logs a CHANGE rather
// than once per frame.
static uintptr_t g_pawnFromCtl = 0;
static uintptr_t g_pawnRejectedFromCtl = 0;

// Non-static: declared above, where the rung 5b code drives the pawn search.
// Is obj an instance of `wantClass` OR of anything derived from it?
//
// UE3 chains classes through SuperField at +0x3C - the same field LookupProp walks to find an
// inherited property, and for the same reason: what you are looking for is usually declared
// further up than the object's own class.
static bool IsAOfClass(uintptr_t obj, const char* wantClass)
{
    uint32_t cls;
    if (!SafeU32(obj + 0x34, &cls) || cls < 0x10000) return false;
    for (int depth = 0; cls && depth < 16; ++depth) {
        if (ObjNameIs(cls, wantClass)) return true;
        uint32_t super;
        if (!SafeU32(cls + 0x3C, &super) || super < 0x10000) break;
        cls = super;
    }
    return false;
}

// ⚠️ WHAT THE PAWN IS CALLED IS NOT THE TEST. What it IS, is.
//
// This asked two questions about names and both were wrong, in the same way, one level apart.
//
// It required the object's OWN NAME to be "TdPlayerPawn". UE3 derives a spawned actor's name
// from its class, so that holds for anything the engine spawns and need not hold for an actor a
// designer placed in a level.
//
// It then required the EXACT class. Measured, and it is why a new game never worked while
// loading a save always did: the tutorial level's player pawn is a **TdTutorialPawn**, and there
// is no TdPlayerPawn instance in that level at all. The full walk found one object of the class
// and it was the class default object:
//
//     [vm] no live TdPlayerPawn - 1395.1 ms over 104834 slots, 1 objects of that class
//                                 named: Default__TdPlayerPawn
//     [view] ViewTarget -> 4785C800 class "TdTutorialPawn"  |  pawn is 00000000
//
// So the chain is walked and the question asked once. The offsets stay valid either way:
// PlayerCameraLocation and PlayerCameraRotation are declared on TdPlayerPawn, and a subclass
// inherits them at the same offsets.
//
// The one thing that answers yes and should not is the class default object - a structurally
// perfect instance - which is what the own-name check was really buying, and all it was buying.
bool LooksLikePlayerPawn(uintptr_t obj)
{
    if (!obj || g_offName < 0) return false;
    uint32_t vt;
    if (!SafeU32(obj, &vt) || !InModule(vt)) return false;
    if (!IsAOfClass(obj, "TdPlayerPawn")) return false;
    char nm[64];
    if (!ReadObjName(obj, nm, sizeof(nm))) return false;
    return strncmp(nm, "Default__", 9) != 0;
}

uintptr_t FindPlayerPawn()
{
    if (LooksLikePlayerPawn(g_playerPawn)) return g_playerPawn;
    if (g_playerPawn) { g_playerPawn = 0; g_pawnNextTry = 0; }

    // ---- ⚠️ ASK THE CONTROLLER. The walk is the fallback, not the method. ----
    //
    // Controller::Pawn is the answer as one pointer read. The walk below answers the same
    // question in 1.2-1.6 seconds across ~115000 slots, on the render thread, and then refuses
    // to repeat for 600 frames - which is not a cache, it is the delay after every death,
    // measured at 10-14 s of mono while the new pawn sits there unfound.
    //
    // The controller SURVIVES the death that destroys the pawn, so this path is live at exactly
    // the moment the walk is not. Validated through LooksLikePlayerPawn rather than trusted: a
    // stale or mid-respawn Pawn pointer must fail the same test as anything else.
    if (g_offCtlPawn >= 0 && g_playerCtl) {
        uint32_t p = 0;
        if (SafeU32(g_playerCtl + g_offCtlPawn, &p) && p >= 0x10000) {
            char cn[64] = "?";
            uint32_t clsObj = 0;
            if (SafeU32(p + 0x34, &clsObj) && clsObj >= 0x10000)
                ReadObjName(clsObj, cn, sizeof(cn));

            if (LooksLikePlayerPawn(p)) {
                g_playerPawn = p;
                g_pawnMissLogged = false;
                if (g_pawnFromCtl != p) {
                    g_pawnFromCtl = p;
                    Log("[vm] pawn %p class \"%s\" - from Controller::Pawn, no walk",
                        (void*)p, cn);
                }
                return p;
            }

            // ⚠️ The controller HAS a pawn and we are refusing it. That is the shape of both
            // naming failures above - a live pawn on the other side of an assumption - and both
            // times the log said only "not found". It says what it turned down now.
            if (g_pawnRejectedFromCtl != p) {
                g_pawnRejectedFromCtl = p;
                Log("[vm] ⚠️ Controller::Pawn is %p class \"%s\" and it is being REJECTED -"
                    " not a TdPlayerPawn by any ancestor. The camera offsets do not apply to it.",
                    (void*)p, cn);
            }
        }
    }

    if (g_frames < g_pawnNextTry) return 0;
    if (!g_gobjAddr || g_offName < 0) { g_pawnNextTry = g_frames + 600; return 0; }

    uint32_t data, count;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) {
        g_pawnNextTry = g_frames + 600; return 0;
    }
    // ---- diag: a miss must say what it SAW, not only that it saw nothing ----
    //
    // The old miss line was three words and it cost two runs. "no live TdPlayerPawn yet" cannot
    // distinguish an unreadable object table from a table with no such class in it from a table
    // full of them being turned away by the predicate. Naming what it turned down is what
    // produced "1 objects of that class named: Default__TdPlayerPawn" - which said, in one line,
    // that the level had no such pawn and the search was looking for the wrong thing entirely.
    //
    // Time-sliced like the swan walk, and for a measured reason: run 28's death froze the game
    // for 4294 ms while this walk scanned 116k slots on the render thread hunting a pawn that
    // was legitimately destroyed. The pass now resumes at a cursor under a per-call budget;
    // the controller path above still answers instantly the moment the respawned pawn exists.
    static uint32_t walkCursor = 0;
    static double walkMs = 0.0;
    static int  walkKinds = 0;
    static char walkSeen[3][64] = {};
    const double t0 = NowMs();
    const double kBudgetMs = 3.0;
    if (walkCursor >= count) walkCursor = 0;
    uint32_t i = walkCursor;

    for (; i < count; ++i) {
        if ((i & 0x3F) == 0 && NowMs() - t0 > kBudgetMs) break;
        uint32_t obj, vt;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        if (!SafeU32(obj, &vt) || !InModule(vt)) continue;
        if (!IsAOfClass(obj, "TdPlayerPawn")) continue;

        char nm[64] = "";
        if (!ReadObjName(obj, nm, sizeof(nm))) continue;
        // Report the CLASS, not the object name: the class is what varies between levels and it
        // is the thing that was wrong. TdTutorialPawn is invisible in a list of object names.
        char cn[64] = "?";
        uint32_t clsObj = 0;
        if (SafeU32(obj + 0x34, &clsObj) && clsObj >= 0x10000) ReadObjName(clsObj, cn, sizeof(cn));
        if (walkKinds < 3) strcpy_s(walkSeen[walkKinds], cn);
        walkKinds++;
        if (strncmp(nm, "Default__", 9) == 0) continue;   // the CDO is not a live pawn

        g_playerPawn = obj;
        g_pawnMissLogged = false;
        Log("[vm] pawn %p class \"%s\" - found by amortized WALK, %.1f ms total at slot %u"
            " of %u (Controller::Pawn should have got here first)",
            (void*)obj, cn, walkMs + NowMs() - t0, i, count);
        walkCursor = 0; walkMs = 0.0; walkKinds = 0;
        return obj;
    }
    walkMs += NowMs() - t0;
    walkCursor = i;
    if (i < count) return 0;               // pass continues next call at the cursor

    walkCursor = 0;
    g_pawnNextTry = g_frames + 600;   // same backoff discipline as the other searches
    if (!g_pawnMissLogged) {
        g_pawnMissLogged = true;
        Log("[vm] no live pawn of any TdPlayerPawn kind - %.1f ms spread over a %u-slot pass,"
            " %d seen%s%s%s%s",
            walkMs, count, walkKinds,
            walkKinds > 0 ? ", classes: " : "",
            walkKinds > 0 ? walkSeen[0] : "",
            walkKinds > 1 ? ", " : "", walkKinds > 1 ? walkSeen[1] : "");
    }
    walkMs = 0.0; walkKinds = 0;
    return 0;
}

// ================================================================ Phase 1.2: live arm-rig discovery
//
// This cache is deliberately rebuilt from the pawn's reflected editinline properties rather
// than found through GObjects. A death or level load can destroy the pawn and every controller
// beneath it; the surviving PlayerController gives FindPlayerPawn the replacement in one read.
// Every sampled pass rereads and revalidates the nested pointers, so the cache never grants a
// future write path permission to use stale controller objects.
struct MotionRigCache {
    uintptr_t pawn;
    uintptr_t mesh;
    uintptr_t leftWorld;
    uintptr_t rightWorld;
    uintptr_t leftRotation;
    uintptr_t rightRotation;
    uintptr_t leftForearm;
    uintptr_t rightForearm;
    bool valid;
};

static MotionRigCache g_motionRig = {};
static unsigned long g_motionRigGeneration = 0;
static uintptr_t g_lastRigRejectedPawn = 0;
static uintptr_t g_lastRigRejectedObject = 0;
static int g_lastRigRejectedField = -2;

static bool ReadClassName(uintptr_t obj, char* out, size_t cap)
{
    if (!out || !cap) return false;
    strcpy_s(out, cap, "?");
    uint32_t cls = 0;
    return obj >= 0x10000 && SafeU32(obj + 0x34, &cls) && cls >= 0x10000 &&
           ReadObjName(cls, out, cap);
}

static bool LooksLikeRigObject(uintptr_t obj, const char* expectedClass)
{
    if (obj < 0x10000) return false;
    uint32_t vt = 0;
    if (!SafeU32(obj, &vt) || !InModule(vt) || !IsAOfClass(obj, expectedClass)) return false;
    char name[64];
    if (!ReadObjName(obj, name, sizeof(name))) return false;
    return strncmp(name, "Default__", 9) != 0;
}

static bool MotionRigPointerOffsetsKnown()
{
    return g_offLeftHandWorldIK >= 0 && g_offRightHandWorldIK >= 0 &&
           g_offLeftHandRotation >= 0 && g_offRightHandRotation >= 0 &&
           g_offLeftForeArmRoll >= 0 && g_offRightForeArmRoll >= 0;
}

// The retail cook removes six editinline properties from UClass::Children even though their
// storage and gameplay use remain. Derive that storage from the authored declaration itself:
//
//   world limb L/R, local limb L/R, nullable wrist rotation R/L,
//   heavy-weapon spring, forearm rotation R/L
//
// Seven required live, class-validated UObjects plus the two measured nullable slots are a
// fingerprint rather than a guessed address. AT_C1P legitimately leaves both hand-rotation
// pointers null, so requiring all nine objects made the stripped-property fallback impossible on
// exactly the first-person rig it exists to recover. The scan remains bounded to the pawn's own
// fields before Mesh1p and requires distinct bilateral instances.
static bool DeriveMotionRigPointerOffsets(uintptr_t pawn)
{
    if (MotionRigPointerOffsetsKnown()) return true;
    if (!pawn || g_offMesh1p <= 0x40) return false;

    const char* expected[] = {
        "TdSkelControlLimb", "TdSkelControlLimb",
        "SkelControlLimb", "SkelControlLimb",
        "SkelControlSingleBone", "SkelControlSingleBone",
        "TdSkelControlSpring",
        "SkelControlSingleBone", "SkelControlSingleBone",
    };
    const int count = (int)(sizeof(expected) / sizeof(expected[0]));

    for (int off = 0x40; off + count * 4 <= g_offMesh1p; off += 4) {
        uintptr_t objects[count] = {};
        bool match = true;
        for (int i = 0; i < count; ++i) {
            uint32_t value = 0;
            if (!SafeU32(pawn + off + i * 4, &value)) {
                match = false;
                break;
            }
            const bool nullableHandRotation = i == 4 || i == 5;
            if ((!value && !nullableHandRotation) ||
                (value && !LooksLikeRigObject(value, expected[i]))) {
                match = false;
                break;
            }
            objects[i] = value;
        }
        if (!match) continue;
        if (objects[0] == objects[1] || objects[2] == objects[3] ||
            (objects[4] && objects[5] && objects[4] == objects[5]) ||
            objects[7] == objects[8]) continue;

        g_offLeftHandWorldIK  = off;
        g_offRightHandWorldIK = off + 4;
        g_offRightHandRotation = off + 16;
        g_offLeftHandRotation  = off + 20;
        g_offRightForeArmRoll = off + 28;
        g_offLeftForeArmRoll  = off + 32;

        Log("*** [hands-rig] derived stripped TdPawn controller block at +0x%04X"
            " from 7 required objects + 2 nullable hand-rotation slots", off);
        for (int i = 0; i < count; ++i) {
            char objectName[64] = "?", className[64] = "?";
            ReadObjName(objects[i], objectName, sizeof(objectName));
            ReadClassName(objects[i], className, sizeof(className));
            Log("[hands-rig]   +0x%04X -> %p name \"%s\" class \"%s\"",
                off + i * 4, (void*)objects[i], objectName, className);
        }
        return true;
    }

    // Some first-person construction frames leave one of the five nonessential middle objects
    // null, so the nine-slot fingerprint above can miss even though the two world limbs are
    // already live. The cook still exposes LeftForeArmRoll's exact storage offset. Its authored
    // declaration is 32 bytes after LeftHandWorldIK; derive only that measured adjacency, then
    // require two distinct live TdSkelControlLimb objects before accepting it. This is a retryable
    // fallback, not a blind baked offset.
    if (g_offLeftForeArmRoll >= 0x60) {
        const int off = g_offLeftForeArmRoll - 32;
        const bool rightRollAgrees = g_offRightForeArmRoll < 0 ||
                                     g_offRightForeArmRoll == off + 28;
        uint32_t leftWorld = 0, rightWorld = 0;
        if (rightRollAgrees && off >= 0x40 && off + 36 <= g_offMesh1p &&
            SafeU32(pawn + off, &leftWorld) &&
            SafeU32(pawn + off + 4, &rightWorld) &&
            leftWorld != rightWorld &&
            LooksLikeRigObject(leftWorld, "TdSkelControlLimb") &&
            LooksLikeRigObject(rightWorld, "TdSkelControlLimb")) {
            g_offLeftHandWorldIK = off;
            g_offRightHandWorldIK = off + 4;
            g_offRightHandRotation = off + 16;
            g_offLeftHandRotation = off + 20;
            g_offRightForeArmRoll = off + 28;
            g_offLeftForeArmRoll = off + 32;
            Log("*** [hands-rig] recovered controller block at +0x%04X from reflected"
                " LeftForeArmRoll + bilateral live world-IK validation", off);
            return true;
        }
    }

    return false;
}

static void ClearMotionRig(const char* reason)
{
    if (g_motionRig.valid) {
        Log("[hands-rig] invalidated generation %lu: %s (pawn %p, mesh %p)",
            g_motionRigGeneration, reason, (void*)g_motionRig.pawn, (void*)g_motionRig.mesh);
    }
    ZeroMemory(&g_motionRig, sizeof(g_motionRig));
}

static void ReportRigRejection(uintptr_t pawn, int field, const char* fieldName,
                               uintptr_t object, const char* expectedClass,
                               const char* reason)
{
    // An absent pawn or one bad editinline pointer can persist for hundreds of frames while UE3
    // respawns. Log each distinct rejection, not every ten-frame sample of the same state.
    if (g_lastRigRejectedPawn == pawn && g_lastRigRejectedField == field &&
        g_lastRigRejectedObject == object) return;
    g_lastRigRejectedPawn = pawn;
    g_lastRigRejectedField = field;
    g_lastRigRejectedObject = object;

    char actual[64] = "?";
    if (object) ReadClassName(object, actual, sizeof(actual));
    Log("[hands-rig] waiting: pawn %p %s %s at %p (class \"%s\", expected %s)",
        (void*)pawn, fieldName, reason, (void*)object, actual, expectedClass);
}

static bool SameMotionRig(const MotionRigCache& a, const MotionRigCache& b)
{
    return a.pawn == b.pawn && a.mesh == b.mesh &&
           a.leftWorld == b.leftWorld && a.rightWorld == b.rightWorld &&
           a.leftRotation == b.leftRotation && a.rightRotation == b.rightRotation &&
           a.leftForearm == b.leftForearm && a.rightForearm == b.rightForearm;
}

static void UpdateMotionRigDiscovery()
{
    if ((!g_motionHands && !g_motionHandsDebug) ||
        InterlockedCompareExchange(&g_motionRigOffsetsReady, 0, 0) != 1) return;

    const uintptr_t pawn = FindPlayerPawn();
    if (!pawn) {
        ClearMotionRig("no live player pawn");
        ReportRigRejection(0, -1, "pawn", 0, "TdPlayerPawn", "is unavailable");
        return;
    }

    if (!DeriveMotionRigPointerOffsets(pawn)) {
        ClearMotionRig("stripped controller block has not been derived");
        ReportRigRejection(pawn, -2, "controller block", 0,
                           "7 required objects + 2 nullable hand rotations", "was not found");
        return;
    }

    struct RigField {
        const char* name;
        const char* expectedClass;
        int offset;
        uintptr_t* destination;
        bool required;
    };

    MotionRigCache candidate = {};
    candidate.pawn = pawn;
    RigField fields[] = {
        { "Mesh1p", "TdSkeletalMeshComponent", g_offMesh1p, &candidate.mesh, true },
        { "LeftHandWorldIKController", "TdSkelControlLimb", g_offLeftHandWorldIK,
          &candidate.leftWorld, true },
        { "RightHandWorldIKController", "TdSkelControlLimb", g_offRightHandWorldIK,
          &candidate.rightWorld, true },
        // These helpers are declared on TdPawn but are legitimately null in the shipped
        // first-person setup we measured. Position-only P1.3 needs the world limbs, not these.
        // A non-null helper is still required to validate as the exact controller family.
        { "LeftHandRotationController", "SkelControlSingleBone", g_offLeftHandRotation,
          &candidate.leftRotation, false },
        { "RightHandRotationController", "SkelControlSingleBone", g_offRightHandRotation,
          &candidate.rightRotation, false },
        { "LeftForeArmRollRotationController", "SkelControlSingleBone", g_offLeftForeArmRoll,
          &candidate.leftForearm, false },
        { "RightForeArmRollRotationController", "SkelControlSingleBone", g_offRightForeArmRoll,
          &candidate.rightForearm, false },
    };

    for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); ++i) {
        uint32_t value = 0;
        if (!SafeU32(pawn + fields[i].offset, &value)) {
            ClearMotionRig("a reflected rig pointer became unreadable");
            ReportRigRejection(pawn, i, fields[i].name, 0, fields[i].expectedClass,
                               "is unreadable");
            return;
        }
        *fields[i].destination = value;
        if (!value && !fields[i].required) continue;
        if (!LooksLikeRigObject(value, fields[i].expectedClass)) {
            ClearMotionRig("a reflected rig pointer failed validation");
            ReportRigRejection(pawn, i, fields[i].name, value, fields[i].expectedClass,
                               "failed validation");
            return;
        }
    }

    // Left and right resolving to the same controller is a readable, correctly typed result but
    // still cannot be a safe two-hand rig. Treat it as an incomplete construction state.
    if (candidate.leftWorld == candidate.rightWorld ||
        (candidate.leftRotation && candidate.rightRotation &&
         candidate.leftRotation == candidate.rightRotation) ||
        (candidate.leftForearm && candidate.rightForearm &&
         candidate.leftForearm == candidate.rightForearm)) {
        ClearMotionRig("left/right rig controllers alias each other");
        ReportRigRejection(pawn, 7, "left/right controllers", candidate.leftWorld,
                           "distinct objects", "are aliased");
        return;
    }

    candidate.valid = true;
    if (g_motionRig.valid && SameMotionRig(g_motionRig, candidate)) return;
    if (g_motionRig.valid) ClearMotionRig("pawn or nested rig object was replaced");

    g_motionRig = candidate;
    ++g_motionRigGeneration;
    g_lastRigRejectedPawn = 0;
    g_lastRigRejectedObject = 0;
    g_lastRigRejectedField = -2;

    char pawnClass[64], meshClass[64], leftWorldClass[64], rightWorldClass[64];
    char leftRotationClass[64] = "(none)", rightRotationClass[64] = "(none)";
    char leftForearmClass[64] = "(none)", rightForearmClass[64] = "(none)";
    ReadClassName(candidate.pawn, pawnClass, sizeof(pawnClass));
    ReadClassName(candidate.mesh, meshClass, sizeof(meshClass));
    ReadClassName(candidate.leftWorld, leftWorldClass, sizeof(leftWorldClass));
    ReadClassName(candidate.rightWorld, rightWorldClass, sizeof(rightWorldClass));
    if (candidate.leftRotation)
        ReadClassName(candidate.leftRotation, leftRotationClass, sizeof(leftRotationClass));
    if (candidate.rightRotation)
        ReadClassName(candidate.rightRotation, rightRotationClass, sizeof(rightRotationClass));
    if (candidate.leftForearm)
        ReadClassName(candidate.leftForearm, leftForearmClass, sizeof(leftForearmClass));
    if (candidate.rightForearm)
        ReadClassName(candidate.rightForearm, rightForearmClass, sizeof(rightForearmClass));
    Log("*** [hands-rig] acquired generation %lu: pawn %p \"%s\", Mesh1p %p \"%s\"",
        g_motionRigGeneration, (void*)candidate.pawn, pawnClass,
        (void*)candidate.mesh, meshClass);
    Log("[hands-rig] world IK: L %p \"%s\", R %p \"%s\"",
        (void*)candidate.leftWorld, leftWorldClass,
        (void*)candidate.rightWorld, rightWorldClass);
    Log("[hands-rig] optional wrist rotation: L %p \"%s\", R %p \"%s\"",
        (void*)candidate.leftRotation, leftRotationClass,
        (void*)candidate.rightRotation, rightRotationClass);
    Log("[hands-rig] optional forearm roll: L %p \"%s\", R %p \"%s\" (read-only)",
        (void*)candidate.leftForearm, leftForearmClass,
        (void*)candidate.rightForearm, rightForearmClass);
}

// ================================================================ Phase 1.3: two-hand position proof
//
// One writable vector and one strength state per hand. This deliberately does not rotate either
// wrist or pose fingers. Every write revalidates the pawn, mesh, and both limb controllers after
// the game's own Update1pArms has returned.
enum P13BlockReason {
    P13_READY = 0,
    P13_LAYOUT,
    P13_RIG,
    P13_VIEW,
    P13_WEAPON_LAYOUT,
    P13_ARMED,
    P13_MOVEMENT,
    P13_TRACKING,
    P13_REACH,
    P13_WRITE_FAILED,
};

static const char* P13ReasonName(P13BlockReason reason)
{
    switch (reason) {
        case P13_READY:         return "eligible: unarmed Walking/Jump/Falling/Crouch/180Turn/Balance/LedgeWalk + tracked grip";
        case P13_LAYOUT:        return "rig/controller layout is not ready";
        case P13_RIG:           return "no validated live position rig";
        case P13_VIEW:          return "ViewTarget is not the player pawn";
        case P13_WEAPON_LAYOUT: return "weapon ownership fields are unavailable";
        case P13_ARMED:         return "weapon equipped or weapon animation is not Unarmed";
        case P13_MOVEMENT:      return "movement is game-owned";
        case P13_TRACKING:      return "grip position is not active, valid, and tracked";
        case P13_REACH:         return "grip target is outside the 10..150 UU proof volume";
        case P13_WRITE_FAILED:  return "a guarded IK write or read-back failed";
        default:                return "unknown";
    }
}

static bool WriteRigBytes(uintptr_t address, const void* data, size_t size)
{
    if (!address || !data || !size) return false;
    SIZE_T wrote = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)address, data, size, &wrote) &&
           wrote == size;
}

static bool CurrentViewTargetIsPawn(uintptr_t pawn)
{
    if (!pawn || !g_playerCtl || g_offCtlCamera < 0 || g_offCamViewTarget < 0) return false;
    uint32_t camera = 0, target = 0;
    return SafeU32(g_playerCtl + g_offCtlCamera, &camera) && camera >= 0x10000 &&
           SafeU32(camera + g_offCamViewTarget, &target) && target == (uint32_t)pawn;
}

static volatile LONG g_viewTargetRetryRunning = 0;
static volatile LONG g_motionInitRetryRunning = 0;

static DWORD WINAPI ViewTargetRetryThread(LPVOID)
{
    Log("[view] TdPlayerCamera::ViewTarget was unavailable during startup; retrying in background");
    const int recovered = LookupProp("TdPlayerCamera", "ViewTarget", true);
    if (recovered >= 0) {
        g_offCamViewTarget = recovered;
        Log("*** [view] recovered ViewTarget offset +0x%04X; hand eligibility unblocked",
            recovered);
    }
    InterlockedExchange(&g_viewTargetRetryRunning, 0);
    return 0;
}

static void RetryViewTargetOffset()
{
    if (g_offCamViewTarget >= 0 || g_offCtlCamera < 0 ||
        InterlockedCompareExchange(&g_objModelThreadFinished, 0, 0) != 1) return;
    static long nextTry = 0;
    if (g_frames < nextTry) return;
    nextTry = g_frames + 600;
    if (InterlockedCompareExchange(&g_viewTargetRetryRunning, 1, 0) != 0) return;
    HANDLE thread = CreateThread(nullptr, 0, ViewTargetRetryThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&g_viewTargetRetryRunning, 0);
        Log("[view] could not start ViewTarget retry thread (error %lu)", GetLastError());
    }
}

static DWORD WINAPI MotionInitRetryThread(LPVOID)
{
    Log("[hands-rig] startup metadata was incomplete; retrying arm layout in background");
    ResolveMotionRigOffsets();
    InstallUpdate1pArmsHook();
    InterlockedExchange(&g_motionInitRetryRunning, 0);
    return 0;
}

static void RetryMotionArmInitialization()
{
    if (g_update1pArmsTarget ||
        InterlockedCompareExchange(&g_objModelThreadFinished, 0, 0) != 1) return;
    static long nextTry = 0;
    if (g_frames < nextTry) return;
    nextTry = g_frames + 600;
    if (InterlockedCompareExchange(&g_motionInitRetryRunning, 1, 0) != 0) return;
    HANDLE thread = CreateThread(nullptr, 0, MotionInitRetryThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&g_motionInitRetryRunning, 0);
        Log("[hands-rig] could not start arm-layout retry thread (error %lu)", GetLastError());
    }
}

static bool SetP13Strength(uintptr_t controller, float strength)
{
    if (!LooksLikeRigObject(controller, "TdSkelControlLimb") ||
        g_offSkelControlStrength < 0 || g_offSkelStrengthTarget < 0 ||
        g_offSkelBlendTimeToGo < 0) return false;

    // Equivalent to SetSkelControlStrength(strength, 0): without StrengthTarget and
    // BlendTimeToGo, the controller update restores the old target before skeletal evaluation.
    const float noBlend = 0.0f;
    return WriteRigBytes(controller + g_offSkelStrengthTarget, &strength, sizeof(strength)) &&
           WriteRigBytes(controller + g_offSkelBlendTimeToGo, &noBlend, sizeof(noBlend)) &&
           WriteRigBytes(controller + g_offSkelControlStrength, &strength, sizeof(strength));
}

static P13BlockReason P13Eligibility(uintptr_t pawn, const P13HandPoseSnapshot& pose,
                                     long presentFrame, bool leftHand,
                                     uintptr_t* outController,
                                     uint8_t* outMovement, float* outReach)
{
    if (outController) *outController = 0;
    if (InterlockedCompareExchange(&g_motionRigOffsetsReady, 0, 0) != 1 ||
        g_offMesh1p < 0 || g_offLeftHandWorldIK < 0 ||
        g_offLimbEffector < 0 || g_offLimbEffectorSpace < 0 ||
        g_offSkelControlStrength < 0 || g_offSkelStrengthTarget < 0 ||
        g_offSkelBlendTimeToGo < 0) return P13_LAYOUT;
    if (!LooksLikePlayerPawn(pawn)) return P13_RIG;

    uint32_t mesh = 0, leftController = 0, rightController = 0;
    if (!SafeU32(pawn + g_offMesh1p, &mesh) ||
        !LooksLikeRigObject(mesh, "TdSkeletalMeshComponent") ||
        !SafeU32(pawn + g_offLeftHandWorldIK, &leftController) ||
        !LooksLikeRigObject(leftController, "TdSkelControlLimb") ||
        g_offRightHandWorldIK < 0 ||
        !SafeU32(pawn + g_offRightHandWorldIK, &rightController) ||
        !LooksLikeRigObject(rightController, "TdSkelControlLimb") ||
        rightController == leftController) return P13_RIG;

    // EBoneControlSpace has six usable values (World through OtherBone). Check both independently
    // validated limb instances before trusting a structurally recovered byte.
    uint8_t leftSpace = 0xFF, rightSpace = 0xFF;
    if (!SafeRead(leftController + g_offLimbEffectorSpace, &leftSpace, 1) || leftSpace > 5 ||
        !SafeRead(rightController + g_offLimbEffectorSpace, &rightSpace, 1) || rightSpace > 5)
        return P13_LAYOUT;

    float leftStrength = 0.0f, leftTarget = 0.0f, leftBlend = 0.0f;
    float rightStrength = 0.0f, rightTarget = 0.0f, rightBlend = 0.0f;
    const bool strengthLayoutHonest =
        SafeRead(leftController + g_offSkelControlStrength, &leftStrength, sizeof(float)) &&
        SafeRead(leftController + g_offSkelStrengthTarget, &leftTarget, sizeof(float)) &&
        SafeRead(leftController + g_offSkelBlendTimeToGo, &leftBlend, sizeof(float)) &&
        SafeRead(rightController + g_offSkelControlStrength, &rightStrength, sizeof(float)) &&
        SafeRead(rightController + g_offSkelStrengthTarget, &rightTarget, sizeof(float)) &&
        SafeRead(rightController + g_offSkelBlendTimeToGo, &rightBlend, sizeof(float)) &&
        std::isfinite(leftStrength) && leftStrength >= 0.0f && leftStrength <= 1.0f &&
        std::isfinite(leftTarget) && leftTarget >= 0.0f && leftTarget <= 1.0f &&
        std::isfinite(leftBlend) && leftBlend >= 0.0f && leftBlend <= 60.0f &&
        std::isfinite(rightStrength) && rightStrength >= 0.0f && rightStrength <= 1.0f &&
        std::isfinite(rightTarget) && rightTarget >= 0.0f && rightTarget <= 1.0f &&
        std::isfinite(rightBlend) && rightBlend >= 0.0f && rightBlend <= 60.0f;
    if (!strengthLayoutHonest) return P13_LAYOUT;
    if (outController) *outController = leftHand ? leftController : rightController;

    if (!g_playerCtl || g_offCtlCamera < 0 || g_offCamViewTarget < 0)
        return P13_LAYOUT;
    if (!CurrentViewTargetIsPawn(pawn)) return P13_VIEW;
    if (g_offWeapon < 0 || g_offWeaponAnimState < 0) return P13_WEAPON_LAYOUT;

    uint32_t weapon = 0;
    uint8_t weaponAnim = 0;
    if (!SafeU32(pawn + g_offWeapon, &weapon) ||
        !SafeRead(pawn + g_offWeaponAnimState, &weaponAnim, 1))
        return P13_WEAPON_LAYOUT;
    if (weapon != 0 || weaponAnim != 0) return P13_ARMED;

    uint8_t movement = 0xFF;
    // State 24 is EMovement's 180Turn (the quick-turn move players make when stopping a run).
    // Run 8 measured it holding about a second and forcing a release+reacquire double snap of
    // both arms; the camera sweep carries the hand targets through the turn naturally, so VR
    // keeps the arms rather than snapping to animation and back. 29 Balance (narrow beams) and
    // 30 LedgeWalk are slow deliberate locomotion where the player asked to keep the hands -
    // both confirmed as the blocking states in run 18.
    if (g_offMoveState < 0 || !SafeRead(pawn + g_offMoveState, &movement, 1) ||
        !(movement == 1 || movement == 2 || movement == 11 || movement == 15 ||
          movement == 24 || movement == 29 || movement == 30)) {
        if (outMovement) *outMovement = movement;
        return P13_MOVEMENT;
    }
    if (outMovement) *outMovement = movement;

    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                      XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    const long poseAge = g_frames - presentFrame;
    // If XR submission stops while the game continues ticking, the last valid controller pose
    // must not pin the arm in space indefinitely. One frame is normal for this producer/consumer
    // ordering; three allows brief render/game-thread skew without accepting a stale hand.
    if (presentFrame <= 0 || poseAge < 0 || poseAge > 3) return P13_TRACKING;
    if (!pose.active || !pose.worldValid || (pose.flags & need) != need)
        return P13_TRACKING;

    const float reach = VecLength(pose.cameraLocal);
    if (outReach) *outReach = reach;
    if (!FiniteVec(pose.worldPosition) || !std::isfinite(reach) ||
        reach < 10.0f || reach > 150.0f) return P13_REACH;
    return P13_READY;
}

// The arm chain's effective root this tick: authored shoulder socket plus the detachment offset
// being applied. Published by ApplyDetachedShoulders for the effector singularity clamp, which
// runs one tick later (socket sway per tick is ~1-2 UU, well inside the clamp's margin).
static bool g_armChainRootValid[2] = { false, false };
static MEVR_Vec3 g_armChainRoot[2]{};

struct P13LimbControlSave {
    bool active = false;
    uintptr_t pawn = 0, controller = 0;
    MEVR_Vec3 effector{};
    uint8_t effectorSpace = 0;
    bool jointValid = false;
    MEVR_Vec3 jointTarget{};
    uint8_t jointTargetSpace = 0;
    float strength = 0.0f, strengthTarget = 0.0f, blendTimeToGo = 0.0f;
};

struct P13HandState {
    bool owned = false;
    uintptr_t ownedPawn = 0;
    uintptr_t ownedController = 0;
    P13BlockReason reportedReason = (P13BlockReason)-1;
    long writes = 0;
    bool writeFaultLatched = false;
    P13LimbControlSave override{};
};

static P13HandState g_p13LeftState{}, g_p13RightState{};

static bool CaptureP13LimbControl(uintptr_t pawn, uintptr_t controller,
                                  P13LimbControlSave* out)
{
    if (!out || !LooksLikePlayerPawn(pawn) ||
        !LooksLikeRigObject(controller, "TdSkelControlLimb")) return false;
    P13LimbControlSave save{};
    save.active = true;
    save.pawn = pawn;
    save.controller = controller;
    if (!SafeRead(controller + g_offLimbEffector, &save.effector, sizeof(save.effector)) ||
        !SafeRead(controller + g_offLimbEffectorSpace, &save.effectorSpace, 1) ||
        !SafeRead(controller + g_offSkelControlStrength,
                  &save.strength, sizeof(save.strength)) ||
        !SafeRead(controller + g_offSkelStrengthTarget,
                  &save.strengthTarget, sizeof(save.strengthTarget)) ||
        !SafeRead(controller + g_offSkelBlendTimeToGo,
                  &save.blendTimeToGo, sizeof(save.blendTimeToGo)) ||
        !FiniteVec(save.effector) || save.effectorSpace > 5 ||
        !std::isfinite(save.strength) || save.strength < 0.0f || save.strength > 1.0f ||
        !std::isfinite(save.strengthTarget) || save.strengthTarget < 0.0f ||
        save.strengthTarget > 1.0f || !std::isfinite(save.blendTimeToGo) ||
        save.blendTimeToGo < 0.0f || save.blendTimeToGo > 60.0f) return false;
    // The joint target is optional: its authored value is a constant (0,0,0) space 3, but it
    // must round-trip exactly like every other field the arm-update overwrites.
    if (g_offLimbJointTarget >= 0 && g_offLimbJointTargetSpace >= 0 &&
        SafeRead(controller + g_offLimbJointTarget,
                 &save.jointTarget, sizeof(save.jointTarget)) &&
        SafeRead(controller + g_offLimbJointTargetSpace, &save.jointTargetSpace, 1) &&
        FiniteVec(save.jointTarget) && save.jointTargetSpace <= 5)
        save.jointValid = true;
    *out = save;
    return true;
}

static bool RestoreOneP13PositionOverride(uintptr_t pawn, P13HandState& state,
                                          const char* hand)
{
    if (!state.override.active) return true;
    const P13LimbControlSave save = state.override;
    state.override = {};
    if (save.pawn != pawn || !LooksLikePlayerPawn(pawn) ||
        !LooksLikeRigObject(save.controller, "TdSkelControlLimb")) {
        Log("[hands-p1.3] %s prior position override discarded without writes:"
            " pawn/controller replaced", hand);
        state.owned = false;
        state.ownedPawn = 0;
        state.ownedController = 0;
        return true;
    }
    const bool restored =
        WriteRigBytes(save.controller + g_offLimbEffector,
                      &save.effector, sizeof(save.effector)) &&
        WriteRigBytes(save.controller + g_offLimbEffectorSpace,
                      &save.effectorSpace, 1) &&
        (!save.jointValid ||
         (WriteRigBytes(save.controller + g_offLimbJointTarget,
                        &save.jointTarget, sizeof(save.jointTarget)) &&
          WriteRigBytes(save.controller + g_offLimbJointTargetSpace,
                        &save.jointTargetSpace, 1))) &&
        WriteRigBytes(save.controller + g_offSkelControlStrength,
                      &save.strength, sizeof(save.strength)) &&
        WriteRigBytes(save.controller + g_offSkelStrengthTarget,
                      &save.strengthTarget, sizeof(save.strengthTarget)) &&
        WriteRigBytes(save.controller + g_offSkelBlendTimeToGo,
                      &save.blendTimeToGo, sizeof(save.blendTimeToGo));
    if (!restored) {
        state.writeFaultLatched = true;
        state.owned = false;
        state.ownedPawn = 0;
        state.ownedController = 0;
        Log("[hands-p1.3] %s RESTORE FAILED - position ownership disabled for this process",
            hand);
    }
    return restored;
}

static void RestoreMotionHandPositionOverridesBeforeGame(uintptr_t pawn)
{
    RestoreOneP13PositionOverride(pawn, g_p13LeftState, "LEFT");
    RestoreOneP13PositionOverride(pawn, g_p13RightState, "RIGHT");
}

static void ApplyOneMotionHandPosition(uintptr_t pawn, const P13HandPoseSnapshot& pose,
                                       long presentFrame, bool leftHand, P13HandState& state)
{
    const char* hand = leftHand ? "LEFT" : "RIGHT";
    uintptr_t controller = 0;
    uint8_t movement = 0xFF;
    float reach = 0.0f;
    P13BlockReason reason = state.writeFaultLatched ? P13_WRITE_FAILED :
        P13Eligibility(pawn, pose, presentFrame, leftHand,
                       &controller, &movement, &reach);

    // A replacement pawn/controller cannot be released through its predecessor: the old pointer
    // is precisely the object that may already have been destroyed. Drop bookkeeping first.
    if (state.owned && (state.ownedPawn != pawn ||
                        (controller && state.ownedController != controller))) {
        Log("[hands-p1.3] %s ownership discarded without a write: pawn/controller replaced"
            " (%p/%p -> %p/%p)", hand, (void*)state.ownedPawn,
            (void*)state.ownedController, (void*)pawn, (void*)controller);
        state.owned = false;
        state.ownedPawn = 0;
        state.ownedController = 0;
    }

    if (reason != state.reportedReason) {
        if (reason == P13_MOVEMENT)
            Log("[hands-p1.3] %s state -> BLOCKED: %s (state %u)",
                hand, P13ReasonName(reason), movement);
        else
            Log("[hands-p1.3] %s state -> %s: %s", hand,
                reason == P13_READY ? "READY" : "BLOCKED", P13ReasonName(reason));
        state.reportedReason = reason;
    }

    if (reason != P13_READY) {
        if (state.owned) {
            // The prior frame's complete controller state was restored before Update1pArms.
            // The game has already configured this frame, so release means no post-game write.
            Log("[hands-p1.3] %s release: %s; authored controller left untouched",
                hand, P13ReasonName(reason));
            state.owned = false;
            state.ownedPawn = 0;
            state.ownedController = 0;
        }
        return;
    }

    if (!state.owned) {
        MEVR_Vec3 beforeTarget{};
        float beforeStrength = 0.0f, beforeStrengthTarget = 0.0f, beforeBlend = 0.0f;
        uint8_t beforeSpace = 0xFF;
        SafeRead(controller + g_offLimbEffector, &beforeTarget, sizeof(beforeTarget));
        SafeRead(controller + g_offLimbEffectorSpace, &beforeSpace, 1);
        SafeRead(controller + g_offSkelControlStrength, &beforeStrength, sizeof(beforeStrength));
        SafeRead(controller + g_offSkelStrengthTarget,
                 &beforeStrengthTarget, sizeof(beforeStrengthTarget));
        SafeRead(controller + g_offSkelBlendTimeToGo, &beforeBlend, sizeof(beforeBlend));
        Log("*** [hands-p1.3] %s acquire pawn %p controller %p:"
            " prior strength %.3f -> %.3f blend %.3f space %u"
            " target(%+.1f,%+.1f,%+.1f), reach %.1f UU",
            hand, (void*)pawn, (void*)controller, beforeStrength, beforeStrengthTarget,
            beforeBlend, beforeSpace, beforeTarget.x, beforeTarget.y, beforeTarget.z, reach);
        state.owned = true;
        state.ownedPawn = pawn;
        state.ownedController = controller;
    }

    P13LimbControlSave before{};
    if (!CaptureP13LimbControl(pawn, controller, &before)) {
        Log("[hands-p1.3] %s controller capture failed - disabling position ownership", hand);
        state.owned = false;
        state.ownedPawn = 0;
        state.ownedController = 0;
        state.writeFaultLatched = true;
        state.reportedReason = P13_WRITE_FAILED;
        return;
    }
    state.override = before;

    // Beyond the chain's 50.38 UU the two-bone solve sits at its straight-arm singularity: the
    // bend plane is undefined and FK dither flips it per update. Run 17 measured exactly that -
    // 16-17 degree forearm steps with hand, target, and head all still while socket-to-target
    // read 53-57 UU (and every jump-window flip ever logged sat in the same 44-57 band).
    // TdSkelControlLimb also provably ignores JointTargetLocation (a written world pole changed
    // nothing), so the one lever is the effector itself: keep it strictly inside the reachable
    // sphere and let the detached shoulder cover the remainder - the job it exists for.
    MEVR_Vec3 effector = pose.worldPosition;
    const int rootIndex = leftHand ? 0 : 1;
    if (g_armChainRootValid[rootIndex]) {
        const MEVR_Vec3& root = g_armChainRoot[rootIndex];
        const MEVR_Vec3 delta{ effector.x - root.x, effector.y - root.y, effector.z - root.z };
        const float distance = VecLength(delta);
        const float maxSolvable = 47.0f;
        if (std::isfinite(distance) && distance > maxSolvable) {
            const float scale = maxSolvable / distance;
            effector = { root.x + delta.x * scale, root.y + delta.y * scale,
                         root.z + delta.z * scale };
            static long clampReported = -1000;
            if (g_motionHandsDebug && distance > 52.0f && g_frames - clampReported >= 900) {
                clampReported = g_frames;
                Log("[hands-p1.3] %s effector clamped %.1f -> %.1f UU from the chain root:"
                    " straight-arm singularity guard", hand, distance, maxSolvable);
            }
        }
    }

    const uint8_t worldSpace = 0;
    const float strength = 1.0f;
    const bool wrote =
        WriteRigBytes(controller + g_offLimbEffector,
                      &effector, sizeof(effector)) &&
        WriteRigBytes(controller + g_offLimbEffectorSpace, &worldSpace, sizeof(worldSpace)) &&
        SetP13Strength(controller, strength);

    MEVR_Vec3 readTarget{};
    float readStrength = 0.0f, readStrengthTarget = 0.0f, readBlend = -1.0f;
    uint8_t readSpace = 0xFF;
    const bool readBack = wrote &&
        SafeRead(controller + g_offLimbEffector, &readTarget, sizeof(readTarget)) &&
        SafeRead(controller + g_offLimbEffectorSpace, &readSpace, 1) &&
        SafeRead(controller + g_offSkelControlStrength, &readStrength, sizeof(readStrength)) &&
        SafeRead(controller + g_offSkelStrengthTarget,
                 &readStrengthTarget, sizeof(readStrengthTarget)) &&
        SafeRead(controller + g_offSkelBlendTimeToGo, &readBlend, sizeof(readBlend));
    const MEVR_Vec3 error{ readTarget.x - effector.x,
                           readTarget.y - effector.y,
                           readTarget.z - effector.z };
    const bool honest = readBack && readSpace == 0 &&
                        fabsf(readStrength - 1.0f) < 0.001f &&
                        fabsf(readStrengthTarget - 1.0f) < 0.001f &&
                        fabsf(readBlend) < 0.001f && VecLength(error) < 0.01f;
    if (!honest) {
        Log("[hands-p1.3] %s WRITE/READ-BACK FAILED: wrote=%d read=%d space=%u"
            " strength=%.3f target=%.3f blend=%.3f error=%.3f UU - disabling ownership",
            hand, wrote ? 1 : 0, readBack ? 1 : 0, readSpace, readStrength,
            readStrengthTarget, readBlend, VecLength(error));
        RestoreOneP13PositionOverride(pawn, state, hand);
        state.owned = false;
        state.ownedPawn = 0;
        state.ownedController = 0;
        state.writeFaultLatched = true;  // fail closed for this hand for the rest of the process
        state.reportedReason = P13_WRITE_FAILED;
        return;
    }

    ++state.writes;
    if (state.writes == 1 || (g_motionHandsDebug && state.writes % 600 == 0)) {
        Log("[hands-p1.3] %s write #%ld target(%+.1f,%+.1f,%+.1f)"
            " reach %.1f UU strength %.1f -> %.1f blend %.1f space %u read-back exact",
            hand, state.writes, readTarget.x, readTarget.y, readTarget.z,
            reach, readStrength, readStrengthTarget, readBlend, readSpace);
    }
}

static void ApplyMotionHandPosition(uintptr_t pawn, const P13PoseSnapshot& pose)
{
    if (!g_motionHands) return;
    ApplyOneMotionHandPosition(pawn, pose.left, pose.presentFrame, true, g_p13LeftState);
    ApplyOneMotionHandPosition(pawn, pose.right, pose.presentFrame, false, g_p13RightState);
}

// ================================================================ P1.3 detached-shoulder proof
//
// The authored AT_C1P tree has a real RightShoulder translation control but no left-hand twin.
// It does, however, have one empty CameraJoint control-list slot, while the root list contains a
// dormant additive single-bone RootControl behind SwingControl. During an eligible ordinary
// walking frame we borrow that dormant controller and map the empty slot to LeftShoulder. At the
// start of the next Update1pArms call every byte and link is restored BEFORE the game runs. The
// game therefore always sees its authored tree when it takes ownership for a move or weapon.
//
// SK_UpperBody's shipped reference skeleton was measured rather than guessed. The translation
// controls live on LeftShoulder 16 / RightShoulder 45, but SkelControlLimb's two-bone solve is
// rooted one child lower at LeftArm 17 / RightArm 46. ForeArm-to-Hand reach is 50.38 UU; the
// 13.43-UU Shoulder-to-Arm offset is not part of the limb solver and must not delay detachment.
// Runtime SkelControlIndex entries for root, both hands and RightShoulder must all corroborate
// those indices before the left entry is touched.
struct UE3Array32 {
    uint32_t data;
    int32_t count;
    int32_t capacity;
};
struct UE3ControlListHead {
    uint32_t boneName;
    uint32_t boneNameNumber;
    uint32_t controlHead;
    int32_t drawY;
};
struct UE3Matrix44 { float m[4][4]; };

struct DetachedControlSave {
    MEVR_Vec3 translation{};
    uint8_t translationSpace = 0;
    int32_t rotation[3]{};
    uint8_t rotationSpace = 0;
    float strength = 0.0f;
    float strengthTarget = 0.0f;
    float blendTimeToGo = 0.0f;
};

struct DetachedRigFrame {
    uintptr_t pawn = 0, mesh = 0, tree = 0;
    uintptr_t swingControl = 0, leftControl = 0, rightControl = 0;
    uint32_t listData = 0, indexData = 0, spaceBaseData = 0;
    int listCount = 0, indexCount = 0, spaceBaseCount = 0;
    int rootSlot = -1, leftHandSlot = -1, rightHandSlot = -1;
    int rightShoulderSlot = -1, spareSlot = -1, mapBias = 0;
    uint8_t originalLeftMap = 0xFF;
    uint32_t originalSwingNext = 0, originalSpareHead = 0;
    UE3Matrix44 localToWorld{};
};

struct DetachedOverrideState {
    bool active = false;
    bool leftTopology = false;
    bool leftControl = false;
    bool rightControl = false;
    uintptr_t pawn = 0, swing = 0, left = 0, right = 0;
    uint32_t spareHeadAddress = 0, leftMapAddress = 0;
    uint32_t originalSwingNext = 0, originalSpareHead = 0;
    uint8_t originalLeftMap = 0xFF;
    DetachedControlSave leftSaved{}, rightSaved{};
};

struct DetachedHandSolver {
    MEVR_Vec3 filtered{};
    MEVR_Vec3 lastApplied{};
    double lastMs = 0.0;
    bool reachEngaged = false;
    bool reportedDetached = false;
    long writes = 0;
    float diagnosticDistance = 0.0f;
    float diagnosticExcess = 0.0f;
};

struct ArmGeometryDiagnostic {
    bool valid = false;
    bool forearmAxisValid = false;
    bool bendNormalValid = false;
    float shoulderToTarget = 0.0f;
    float meshHandToTarget = 0.0f;
    float upperLength = 0.0f;
    float lowerLength = 0.0f;
    MEVR_Vec3 forearmAxis{};
    MEVR_Vec3 bendNormal{};
};

static DetachedOverrideState g_detachedOverride{};
static DetachedHandSolver g_leftDetach{}, g_rightDetach{};
static ArmGeometryDiagnostic g_armGeometryDiag[2]{};
static bool g_detachedWriteFault = false;
static const char* g_detachedDiscoveryFailure = nullptr;
static uintptr_t g_detachedReportedMesh = 0;

static void ReportDetachedDiscoveryFailure(const char* reason)
{
    if (reason == g_detachedDiscoveryFailure) return;
    g_detachedDiscoveryFailure = reason;
    Log("[hands-detach] waiting: %s", reason);
}

static bool ReadUE3Array(uintptr_t object, int offset, int maxCount, UE3Array32* out)
{
    UE3Array32 value{};
    if (!out || offset < 0 || !SafeRead(object + offset, &value, sizeof(value)) ||
        value.data < 0x10000 || value.count <= 0 || value.count > maxCount ||
        value.capacity < value.count || value.capacity > maxCount * 4) return false;
    *out = value;
    return true;
}

static int FindControlListSlot(const UE3Array32& lists, const char* bone,
                               UE3ControlListHead* found)
{
    for (int i = 0; i < lists.count; ++i) {
        UE3ControlListHead entry{};
        if (!SafeRead(lists.data + i * sizeof(entry), &entry, sizeof(entry))) return -1;
        char name[64] = "?";
        if (!NameOf(entry.boneName, name, sizeof(name))) return -1;
        if (!strcmp(name, bone)) {
            if (found) *found = entry;
            return i;
        }
    }
    return -1;
}

static bool ReadMappedControl(const UE3Array32& indices, int boneIndex, uint8_t* value)
{
    return value && boneIndex >= 0 && boneIndex < indices.count &&
           SafeRead(indices.data + boneIndex, value, 1);
}

static bool CaptureDetachedControl(uintptr_t control, DetachedControlSave* save)
{
    if (!save || !LooksLikeRigObject(control, "SkelControlSingleBone")) return false;
    DetachedControlSave s{};
    if (!SafeRead(control + g_offSingleBoneTranslation, &s.translation,
                  sizeof(s.translation)) ||
        !SafeRead(control + g_offSingleBoneTranslationSpace, &s.translationSpace, 1) ||
        !SafeRead(control + g_offSingleBoneRotation, s.rotation, sizeof(s.rotation)) ||
        !SafeRead(control + g_offSingleBoneRotationSpace, &s.rotationSpace, 1) ||
        !SafeRead(control + g_offSkelControlStrength, &s.strength, sizeof(float)) ||
        !SafeRead(control + g_offSkelStrengthTarget, &s.strengthTarget, sizeof(float)) ||
        !SafeRead(control + g_offSkelBlendTimeToGo, &s.blendTimeToGo, sizeof(float)) ||
        !FiniteVec(s.translation) || s.translationSpace > 5 || s.rotationSpace > 5 ||
        !std::isfinite(s.strength) || s.strength < 0.0f || s.strength > 1.0f ||
        !std::isfinite(s.strengthTarget) || s.strengthTarget < 0.0f ||
        s.strengthTarget > 1.0f || !std::isfinite(s.blendTimeToGo) ||
        s.blendTimeToGo < 0.0f || s.blendTimeToGo > 60.0f) return false;
    *save = s;
    return true;
}

static bool RestoreDetachedControl(uintptr_t control, const DetachedControlSave& save)
{
    return LooksLikeRigObject(control, "SkelControlSingleBone") &&
        WriteRigBytes(control + g_offSingleBoneTranslation,
                      &save.translation, sizeof(save.translation)) &&
        WriteRigBytes(control + g_offSingleBoneTranslationSpace,
                      &save.translationSpace, 1) &&
        WriteRigBytes(control + g_offSingleBoneRotation, save.rotation,
                      sizeof(save.rotation)) &&
        WriteRigBytes(control + g_offSingleBoneRotationSpace,
                      &save.rotationSpace, 1) &&
        WriteRigBytes(control + g_offSkelControlStrength,
                      &save.strength, sizeof(float)) &&
        WriteRigBytes(control + g_offSkelStrengthTarget,
                      &save.strengthTarget, sizeof(float)) &&
        WriteRigBytes(control + g_offSkelBlendTimeToGo,
                      &save.blendTimeToGo, sizeof(float));
}

static void RestoreDetachedArmOverridesBeforeGame(uintptr_t pawn)
{
    if (!g_detachedOverride.active) return;
    DetachedOverrideState saved = g_detachedOverride;
    g_detachedOverride = {};

    // A destroyed/replaced pawn makes every nested pointer suspect. Discard bookkeeping without
    // touching it; a live matching pawn must pass the exact controller-class checks below.
    if (saved.pawn != pawn || !LooksLikePlayerPawn(pawn)) {
        Log("[hands-detach] prior override discarded without writes: pawn replaced");
        g_leftDetach = {};
        g_rightDetach = {};
        return;
    }

    bool restored = true;
    if (saved.leftControl)
        restored = RestoreDetachedControl(saved.left, saved.leftSaved) && restored;
    if (saved.rightControl)
        restored = RestoreDetachedControl(saved.right, saved.rightSaved) && restored;
    if (saved.leftTopology) {
        restored = WriteRigBytes(saved.leftMapAddress, &saved.originalLeftMap, 1) && restored;
        restored = WriteRigBytes(saved.spareHeadAddress, &saved.originalSpareHead,
                                 sizeof(uint32_t)) && restored;
        restored = WriteRigBytes(saved.swing + g_offSkelNextControl,
                                 &saved.originalSwingNext, sizeof(uint32_t)) && restored;
    }
    if (!restored) {
        g_detachedWriteFault = true;
        Log("[hands-detach] RESTORE FAILED - shoulder detachment disabled for this process");
    }
}

static bool DiscoverDetachedRig(uintptr_t pawn, DetachedRigFrame* out)
{
    if (!out || g_detachedWriteFault ||
        InterlockedCompareExchange(&g_detachedArmOffsetsReady, 0, 0) != 1 ||
        !LooksLikePlayerPawn(pawn)) {
        ReportDetachedDiscoveryFailure("deep reflected layout or live pawn is unavailable");
        return false;
    }

    uint32_t mesh = 0, tree = 0;
    if (!SafeU32(pawn + g_offMesh1p, &mesh) ||
        !LooksLikeRigObject(mesh, "TdSkeletalMeshComponent") ||
        !SafeU32(mesh + g_offMeshAnimations, &tree) ||
        !LooksLikeRigObject(tree, "AnimTree")) {
        ReportDetachedDiscoveryFailure("Mesh1p does not expose a validated live AnimTree");
        return false;
    }

    UE3Array32 lists{}, indices{}, bases{};
    if (!ReadUE3Array(tree, g_offAnimTreeSkelControlLists, 128, &lists) ||
        !ReadUE3Array(mesh, g_offMeshSkelControlIndex, 512, &indices) ||
        !ReadUE3Array(mesh, g_offMeshSpaceBases, 512, &bases) ||
        indices.count <= 48 || bases.count <= 48) {
        ReportDetachedDiscoveryFailure("control lists/index or SpaceBases array is invalid");
        return false;
    }

    UE3ControlListHead root{}, leftHand{}, rightHand{}, rightShoulder{}, spare{};
    const int rootSlot = FindControlListSlot(lists, "root", &root);
    const int leftHandSlot = FindControlListSlot(lists, "LeftHand", &leftHand);
    const int rightHandSlot = FindControlListSlot(lists, "RightHand", &rightHand);
    const int rightShoulderSlot =
        FindControlListSlot(lists, "RightShoulder", &rightShoulder);
    const int spareSlot = FindControlListSlot(lists, "CameraJoint", &spare);
    const bool leftAlreadyDetached = g_detachedOverride.active &&
        g_detachedOverride.pawn == pawn && g_detachedOverride.leftTopology;
    if (rootSlot < 0 || leftHandSlot < 0 || rightHandSlot < 0 ||
        rightShoulderSlot < 0 || spareSlot < 0 ||
        (!leftAlreadyDetached && spare.controlHead != 0) ||
        !LooksLikeRigObject(root.controlHead, "SkelControlSingleBone") ||
        !LooksLikeRigObject(rightShoulder.controlHead, "SkelControlSingleBone")) {
        ReportDetachedDiscoveryFailure("authored root/hand/shoulder/CameraJoint lists disagree");
        return false;
    }

    uint32_t rootNext = 0, leftControl = 0, leftTail = 0;
    if (!SafeU32(root.controlHead + g_offSkelNextControl, &rootNext)) {
        ReportDetachedDiscoveryFailure("root control link is unreadable");
        return false;
    }
    leftControl = leftAlreadyDetached ? (uint32_t)g_detachedOverride.left : rootNext;
    if ((leftAlreadyDetached &&
         (rootNext != 0 || spare.controlHead != leftControl ||
          g_detachedOverride.swing != root.controlHead)) ||
        !LooksLikeRigObject(leftControl, "SkelControlSingleBone") ||
        !SafeU32(leftControl + g_offSkelNextControl, &leftTail) || leftTail != 0 ||
        leftControl == rightShoulder.controlHead) {
        ReportDetachedDiscoveryFailure("dormant RootControl chain is not the measured two-node chain");
        return false;
    }

    uint8_t rootMap = 0, leftHandMap = 0, rightHandMap = 0, rightShoulderMap = 0;
    uint8_t currentLeftMap = 0;
    if (!ReadMappedControl(indices, 0, &rootMap) ||
        !ReadMappedControl(indices, 16, &currentLeftMap) ||
        !ReadMappedControl(indices, 19, &leftHandMap) ||
        !ReadMappedControl(indices, 45, &rightShoulderMap) ||
        !ReadMappedControl(indices, 48, &rightHandMap)) {
        ReportDetachedDiscoveryFailure("measured root/shoulder/hand bone indices are unreadable");
        return false;
    }

    int bias = -1;
    for (int candidate = 0; candidate <= 1; ++candidate) {
        if (rootMap == rootSlot + candidate &&
            leftHandMap == leftHandSlot + candidate &&
            rightHandMap == rightHandSlot + candidate &&
            rightShoulderMap == rightShoulderSlot + candidate) {
            bias = candidate;
            break;
        }
    }
    const uint8_t originalLeftMap = leftAlreadyDetached
        ? g_detachedOverride.originalLeftMap : currentLeftMap;
    const bool leftUnmapped = originalLeftMap == 0xFF || (bias == 1 && originalLeftMap == 0);
    const bool currentLeftExpected = leftAlreadyDetached
        ? currentLeftMap == (uint8_t)(spareSlot + bias)
        : currentLeftMap == originalLeftMap;
    if (bias < 0 || !leftUnmapped || !currentLeftExpected) {
        ReportDetachedDiscoveryFailure("runtime bone-to-control map does not corroborate the asset");
        return false;
    }

    UE3Matrix44 localToWorld{};
    if (!SafeRead(mesh + g_offPrimitiveLocalToWorld, &localToWorld,
                  sizeof(localToWorld))) {
        ReportDetachedDiscoveryFailure("Mesh1p LocalToWorld is unreadable");
        return false;
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(localToWorld.m[r][c])) {
                ReportDetachedDiscoveryFailure("Mesh1p LocalToWorld contains non-finite data");
                return false;
            }

    DetachedRigFrame rig{};
    rig.pawn = pawn; rig.mesh = mesh; rig.tree = tree;
    rig.swingControl = root.controlHead;
    rig.leftControl = leftControl;
    rig.rightControl = rightShoulder.controlHead;
    rig.listData = lists.data; rig.indexData = indices.data; rig.spaceBaseData = bases.data;
    rig.listCount = lists.count; rig.indexCount = indices.count; rig.spaceBaseCount = bases.count;
    rig.rootSlot = rootSlot; rig.leftHandSlot = leftHandSlot;
    rig.rightHandSlot = rightHandSlot; rig.rightShoulderSlot = rightShoulderSlot;
    rig.spareSlot = spareSlot; rig.mapBias = bias;
    rig.originalLeftMap = originalLeftMap;
    rig.originalSwingNext = leftControl;
    rig.originalSpareHead = leftAlreadyDetached
        ? g_detachedOverride.originalSpareHead : spare.controlHead;
    rig.localToWorld = localToWorld;
    *out = rig;
    if (g_detachedDiscoveryFailure || g_detachedReportedMesh != mesh) {
        Log("*** [hands-detach] bilateral rig validated: %d bones, %d control lists;"
            " L shoulder 16 -> spare slot %d, R shoulder 45 -> slot %d (map bias %d)",
            indices.count, lists.count, spareSlot, rightShoulderSlot, bias);
        g_detachedDiscoveryFailure = nullptr;
        g_detachedReportedMesh = mesh;
    }
    return true;
}

static bool BoneWorldPosition(const DetachedRigFrame& rig, int boneIndex, MEVR_Vec3* out)
{
    if (!out || boneIndex < 0 || boneIndex >= rig.spaceBaseCount) return false;
    UE3Matrix44 bone{};
    if (!SafeRead(rig.spaceBaseData + boneIndex * sizeof(bone), &bone, sizeof(bone)))
        return false;
    const float x = bone.m[3][0], y = bone.m[3][1], z = bone.m[3][2];
    MEVR_Vec3 world{
        x*rig.localToWorld.m[0][0] + y*rig.localToWorld.m[1][0] +
            z*rig.localToWorld.m[2][0] + rig.localToWorld.m[3][0],
        x*rig.localToWorld.m[0][1] + y*rig.localToWorld.m[1][1] +
            z*rig.localToWorld.m[2][1] + rig.localToWorld.m[3][1],
        x*rig.localToWorld.m[0][2] + y*rig.localToWorld.m[1][2] +
            z*rig.localToWorld.m[2][2] + rig.localToWorld.m[3][2]
    };
    if (!FiniteVec(world)) return false;
    *out = world;
    return true;
}

static bool ShoulderWorldPosition(const DetachedRigFrame& rig, int boneIndex,
                                  const MEVR_Vec3& previousOffset, MEVR_Vec3* out)
{
    if (!BoneWorldPosition(rig, boneIndex, out)) return false;
    // SpaceBases still describes the preceding evaluated frame. Remove the translation we
    // supplied for that frame to recover the authored shoulder socket before solving again.
    out->x -= previousOffset.x;
    out->y -= previousOffset.y;
    out->z -= previousOffset.z;
    return FiniteVec(*out);
}

static bool MotionTurnInputActive();

static MEVR_Vec3 SolveDetachedOffset(DetachedHandSolver& solver,
                                    const MEVR_Vec3& socket,
                                    const MEVR_Vec3& target, bool eligible,
                                    bool holdEngagement)
{
    if (!eligible) {
        solver.filtered = {};
        solver.lastApplied = {};
        solver.lastMs = 0.0;
        solver.reachEngaged = false;
        solver.diagnosticDistance = 0.0f;
        solver.diagnosticExcess = 0.0f;
        return {};
    }

    const MEVR_Vec3 delta{ target.x - socket.x, target.y - socket.y, target.z - socket.z };
    const float distance = VecLength(delta);
    solver.diagnosticDistance = distance;
    // SkelControlLimb rotates Hand + ForeArm around Arm: 25.81 + 24.57 = 50.38 UU. The old
    // shoulder-to-hand value incorrectly included the 13.43-UU Shoulder->Arm link, producing the
    // exact dead zone reported in headset testing. Keep 1.58 UU in reserve so shoulder motion
    // begins just before the two-bone solver locks perfectly straight.
    const float usableReach = 48.8f;
    // Legitimate distances top out near usableReach plus the 60-UU excess cap. Far beyond that
    // is not a reachable pose but a reference glitch; never engage or integrate toward one.
    const bool plausible = std::isfinite(distance) && distance < 150.0f;
    if (!holdEngagement && !solver.reachEngaged && plausible && distance > usableReach)
        solver.reachEngaged = true;
    float excess = solver.reachEngaged && plausible ? distance - usableReach : 0.0f;
    if (excess < 0.0f) excess = 0.0f;
    if (excess > 60.0f) excess = 60.0f;
    solver.diagnosticExcess = excess;
    MEVR_Vec3 desired{};
    if (distance > 0.01f && excess > 0.0f) {
        const float scale = excess / distance;
        desired = { delta.x*scale, delta.y*scale, delta.z*scale };
    }

    const double now = NowMs();
    float dt = solver.lastMs > 0.0 ? (float)((now - solver.lastMs) * 0.001) : (1.0f/60.0f);
    solver.lastMs = now;
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;
    const float alpha = 1.0f - expf(-14.0f * dt);
    solver.filtered.x += (desired.x - solver.filtered.x) * alpha;
    solver.filtered.y += (desired.y - solver.filtered.y) * alpha;
    solver.filtered.z += (desired.z - solver.filtered.z) * alpha;
    if (VecLength(solver.filtered) < 0.05f) solver.filtered = {};
    // Do not chatter between two skeletal topologies at full extension. Once engaged, require the
    // controller to come a real 2 UU back inside the boundary before arming a future detach.
    if (!holdEngagement && solver.reachEngaged && distance < usableReach - 2.0f &&
        VecLength(solver.filtered) < 0.05f)
        solver.reachEngaged = false;
    return solver.filtered;
}

static bool WriteDetachedControl(uintptr_t control, const MEVR_Vec3& worldOffset)
{
    const uint8_t worldSpace = 0;
    const int32_t zeroRotation[3] = { 0, 0, 0 };
    const float one = 1.0f, zero = 0.0f;
    return WriteRigBytes(control + g_offSingleBoneTranslation,
                         &worldOffset, sizeof(worldOffset)) &&
           WriteRigBytes(control + g_offSingleBoneTranslationSpace, &worldSpace, 1) &&
           WriteRigBytes(control + g_offSingleBoneRotation,
                         zeroRotation, sizeof(zeroRotation)) &&
           WriteRigBytes(control + g_offSkelStrengthTarget, &one, sizeof(one)) &&
           WriteRigBytes(control + g_offSkelBlendTimeToGo, &zero, sizeof(zero)) &&
           WriteRigBytes(control + g_offSkelControlStrength, &one, sizeof(one));
}

static bool DetachedControlReadbackIsExact(uintptr_t control,
                                           const MEVR_Vec3& expectedOffset)
{
    MEVR_Vec3 translation{};
    uint8_t space = 0xFF;
    int32_t rotation[3] = { 1, 1, 1 };
    float strength = -1.0f, target = -1.0f, blend = -1.0f;
    if (!SafeRead(control + g_offSingleBoneTranslation,
                  &translation, sizeof(translation)) ||
        !SafeRead(control + g_offSingleBoneTranslationSpace, &space, 1) ||
        !SafeRead(control + g_offSingleBoneRotation, rotation, sizeof(rotation)) ||
        !SafeRead(control + g_offSkelControlStrength, &strength, sizeof(strength)) ||
        !SafeRead(control + g_offSkelStrengthTarget, &target, sizeof(target)) ||
        !SafeRead(control + g_offSkelBlendTimeToGo, &blend, sizeof(blend))) return false;
    const MEVR_Vec3 error{ translation.x - expectedOffset.x,
                           translation.y - expectedOffset.y,
                           translation.z - expectedOffset.z };
    return space == 0 && rotation[0] == 0 && rotation[1] == 0 && rotation[2] == 0 &&
           fabsf(strength - 1.0f) < 0.001f && fabsf(target - 1.0f) < 0.001f &&
           fabsf(blend) < 0.001f && VecLength(error) < 0.01f;
}

static void ApplyDetachedShoulders(uintptr_t pawn, const P13PoseSnapshot& pose)
{
    if (!g_motionHands || g_detachedWriteFault) return;
    DetachedRigFrame rig{};
    if (!DiscoverDetachedRig(pawn, &rig)) {
        g_leftDetach.filtered = {}; g_leftDetach.lastApplied = {};
        g_rightDetach.filtered = {}; g_rightDetach.lastApplied = {};
        return;
    }

    uintptr_t ignored = 0;
    const bool leftEligible =
        P13Eligibility(pawn, pose.left, pose.presentFrame, true,
                       &ignored, nullptr, nullptr) == P13_READY;
    const bool rightEligible =
        P13Eligibility(pawn, pose.right, pose.presentFrame, false,
                       &ignored, nullptr, nullptr) == P13_READY;

    MEVR_Vec3 leftSocket{}, rightSocket{};
    const bool haveLeftSocket = ShoulderWorldPosition(
        rig, 17, g_leftDetach.lastApplied, &leftSocket);
    const bool haveRightSocket = ShoulderWorldPosition(
        rig, 46, g_rightDetach.lastApplied, &rightSocket);
    auto updateGeometry = [&](int hand, int armBone, const MEVR_Vec3& socket,
                              const MEVR_Vec3& target, bool haveSocket) {
        ArmGeometryDiagnostic diag{};
        MEVR_Vec3 arm{}, elbow{}, meshHand{};
        if (haveSocket && BoneWorldPosition(rig, armBone, &arm) &&
            BoneWorldPosition(rig, armBone + 1, &elbow) &&
            BoneWorldPosition(rig, armBone + 2, &meshHand)) {
            diag.valid = true;
            diag.shoulderToTarget = VecLength({ target.x - socket.x,
                                                target.y - socket.y,
                                                target.z - socket.z });
            diag.meshHandToTarget = VecLength({ target.x - meshHand.x,
                                                target.y - meshHand.y,
                                                target.z - meshHand.z });
            diag.upperLength = VecLength({ elbow.x - arm.x, elbow.y - arm.y,
                                           elbow.z - arm.z });
            diag.lowerLength = VecLength({ meshHand.x - elbow.x,
                                           meshHand.y - elbow.y,
                                           meshHand.z - elbow.z });
            if (diag.lowerLength > 1.0f && std::isfinite(diag.lowerLength)) {
                const float inverseLength = 1.0f / diag.lowerLength;
                diag.forearmAxis = { (meshHand.x - elbow.x) * inverseLength,
                                     (meshHand.y - elbow.y) * inverseLength,
                                     (meshHand.z - elbow.z) * inverseLength };
                diag.forearmAxisValid = FiniteVec(diag.forearmAxis);
                if (diag.upperLength > 1.0f && std::isfinite(diag.upperLength)) {
                    const MEVR_Vec3 upper{
                        (elbow.x - arm.x) / diag.upperLength,
                        (elbow.y - arm.y) / diag.upperLength,
                        (elbow.z - arm.z) / diag.upperLength
                    };
                    MEVR_Vec3 normal{
                        upper.y*diag.forearmAxis.z - upper.z*diag.forearmAxis.y,
                        upper.z*diag.forearmAxis.x - upper.x*diag.forearmAxis.z,
                        upper.x*diag.forearmAxis.y - upper.y*diag.forearmAxis.x
                    };
                    const float normalLength = VecLength(normal);
                    // At a straight elbow the bend plane is undefined. Hold the preceding
                    // anatomical twist rather than letting numerical noise choose a plane.
                    if (normalLength > 0.10f && std::isfinite(normalLength)) {
                        normal.x /= normalLength; normal.y /= normalLength;
                        normal.z /= normalLength;
                        diag.bendNormal = normal;
                        diag.bendNormalValid = FiniteVec(normal);
                    }
                }
            }
        }
        g_armGeometryDiag[hand] = diag;
    };
    updateGeometry(0, 17, leftSocket, pose.left.worldPosition, haveLeftSocket);
    updateGeometry(1, 46, rightSocket, pose.right.worldPosition, haveRightSocket);
    const bool holdShoulderEngagement = MotionTurnInputActive();
    const MEVR_Vec3 leftOffset = SolveDetachedOffset(
        g_leftDetach, leftSocket, pose.left.worldPosition,
        leftEligible && haveLeftSocket, holdShoulderEngagement);
    const MEVR_Vec3 rightOffset = SolveDetachedOffset(
        g_rightDetach, rightSocket, pose.right.worldPosition,
        rightEligible && haveRightSocket, holdShoulderEngagement);
    g_armChainRootValid[0] = leftEligible && haveLeftSocket;
    g_armChainRootValid[1] = rightEligible && haveRightSocket;
    g_armChainRoot[0] = { leftSocket.x + leftOffset.x, leftSocket.y + leftOffset.y,
                          leftSocket.z + leftOffset.z };
    g_armChainRoot[1] = { rightSocket.x + rightOffset.x, rightSocket.y + rightOffset.y,
                          rightSocket.z + rightOffset.z };
    // Keep one render topology for the entire interval in which VR owns each arm. The reach
    // solver now changes only shoulder translation. Switching between authored and detached
    // routing at the extension threshold produced the visible spasms even when the new offset
    // was effectively zero; eligibility/animation handoff is the only legitimate topology edge.
    const bool useLeft = leftEligible;
    const bool useRight = rightEligible;

    if (!useLeft && g_leftDetach.reportedDetached) {
        Log("*** [hands-detach] LEFT reattached at authored shoulder socket");
        g_leftDetach.reportedDetached = false;
    }
    if (!useRight && g_rightDetach.reportedDetached) {
        Log("*** [hands-detach] RIGHT reattached at authored shoulder socket");
        g_rightDetach.reportedDetached = false;
    }
    if (!useLeft && !useRight) return;

    DetachedOverrideState frame{};
    frame.active = true;
    frame.pawn = pawn;
    frame.swing = rig.swingControl;
    frame.left = rig.leftControl;
    frame.right = rig.rightControl;
    frame.originalSwingNext = rig.originalSwingNext;
    frame.originalSpareHead = rig.originalSpareHead;
    frame.originalLeftMap = rig.originalLeftMap;
    frame.spareHeadAddress = rig.listData + rig.spareSlot * sizeof(UE3ControlListHead) + 8;
    frame.leftMapAddress = rig.indexData + 16;

    bool wrote = true;
    if (useLeft) {
        frame.leftControl = CaptureDetachedControl(rig.leftControl, &frame.leftSaved);
        if (!frame.leftControl) wrote = false;
        const uint32_t noNext = 0;
        const uint32_t leftHead = (uint32_t)rig.leftControl;
        const uint8_t leftMap = (uint8_t)(rig.spareSlot + rig.mapBias);
        if (wrote) {
            frame.leftTopology = true;
            wrote = WriteRigBytes(rig.swingControl + g_offSkelNextControl,
                                  &noNext, sizeof(noNext)) &&
                    WriteRigBytes(frame.spareHeadAddress,
                                  &leftHead, sizeof(leftHead)) &&
                    WriteRigBytes(frame.leftMapAddress, &leftMap, 1) &&
                    WriteDetachedControl(rig.leftControl, leftOffset);
            uint32_t readNext = ~0u, readHead = 0;
            uint8_t readMap = 0xFF;
            wrote = wrote &&
                    SafeU32(rig.swingControl + g_offSkelNextControl, &readNext) &&
                    SafeU32(frame.spareHeadAddress, &readHead) &&
                    SafeRead(frame.leftMapAddress, &readMap, 1) &&
                    readNext == 0 && readHead == leftHead && readMap == leftMap &&
                    DetachedControlReadbackIsExact(rig.leftControl, leftOffset);
        }
    }
    if (useRight && wrote) {
        frame.rightControl = CaptureDetachedControl(rig.rightControl, &frame.rightSaved);
        wrote = frame.rightControl && WriteDetachedControl(rig.rightControl, rightOffset) &&
                DetachedControlReadbackIsExact(rig.rightControl, rightOffset);
    }

    g_detachedOverride = frame;
    if (!wrote) {
        RestoreDetachedArmOverridesBeforeGame(pawn);
        g_detachedWriteFault = true;
        g_leftDetach.lastApplied = {};
        g_rightDetach.lastApplied = {};
        Log("[hands-detach] WRITE/READ LAYOUT FAILED - shoulder detachment disabled;");
        Log("[hands-detach] wrist position IK remains active and unchanged");
        return;
    }

    if (useLeft) {
        g_leftDetach.lastApplied = leftOffset;
        ++g_leftDetach.writes;
        if (!g_leftDetach.reportedDetached) {
            Log("*** [hands-detach] LEFT detached via borrowed RootControl;"
                " offset %.1f UU toward controller", VecLength(leftOffset));
            g_leftDetach.reportedDetached = true;
        }
    } else {
        g_leftDetach.lastApplied = {};
    }
    if (useRight) {
        g_rightDetach.lastApplied = rightOffset;
        ++g_rightDetach.writes;
        if (!g_rightDetach.reportedDetached) {
            Log("*** [hands-detach] RIGHT detached via authored shoulder control;"
                " offset %.1f UU toward controller", VecLength(rightOffset));
            g_rightDetach.reportedDetached = true;
        }
    } else {
        g_rightDetach.lastApplied = {};
    }
    if (g_motionHandsDebug && ((useLeft && g_leftDetach.writes % 600 == 0) ||
                               (useRight && g_rightDetach.writes % 600 == 0))) {
        Log("[hands-detach] offsets L %.1f R %.1f UU; shoulder maps L %d R %d;"
            " list slots spare %d/right %d",
            VecLength(leftOffset), VecLength(rightOffset),
            rig.spareSlot + rig.mapBias, rig.rightShoulderSlot + rig.mapBias,
            rig.spareSlot, rig.rightShoulderSlot);
    }
}

// ================================================================ P1.4 bilateral wrist rotation
//
// AT_C1P does not instantiate the TdPawn Left/RightHandRotationController properties; the live
// pointers are correctly null. It does have one SkelControlSingleBone on each ForeArmRoll list.
// While ordinary unarmed walking owns a tracked grip pose, temporarily remove those two controls
// from their roll-bone map and append them to the END of the corresponding Hand control chain.
// Running after world-position limb IK makes the absolute world rotation the final hand result.
// As with shoulder detachment, the complete authored topology and controller bytes are restored
// before the next Update1pArms call, so parkour and weapons never inherit the borrowed controls.
struct WristSideFrame {
    bool active = false;
    uintptr_t control = 0, handTail = 0;
    uint32_t rollMapAddress = 0;
    uint8_t originalRollMap = 0xFF;
    DetachedControlSave saved{};
};
struct WristOverrideState {
    bool active = false;
    uintptr_t pawn = 0;
    WristSideFrame left{}, right{};
};
struct WristRuntimeSide {
    uintptr_t control = 0, handTail = 0;
    uint32_t rollMapAddress = 0;
    uint8_t originalRollMap = 0xFF, unmappedValue = 0xFF;
};

static WristOverrideState g_wristOverride{};
static bool g_wristWriteFault = false;
static uintptr_t g_wristReportedMesh = 0;
static long g_wristLeftWrites = 0, g_wristRightWrites = 0;

static bool LooksLikeLiveUObject(uintptr_t object)
{
    uint32_t vt = 0, cls = 0;
    return object >= 0x10000 && SafeU32(object, &vt) && InModule(vt) &&
           SafeU32(object + 0x34, &cls) && cls >= 0x10000;
}

static bool FindHandControlTail(uintptr_t head, uintptr_t borrowed, uintptr_t* outTail)
{
    if (!outTail || !LooksLikeLiveUObject(head)) return false;
    uintptr_t seen[32]{};
    int count = 0;
    uintptr_t current = head;
    while (count < 32) {
        if (!LooksLikeLiveUObject(current) || current == borrowed) return false;
        for (int i = 0; i < count; ++i) if (seen[i] == current) return false;
        seen[count++] = current;
        uint32_t next = 0;
        if (!SafeU32(current + g_offSkelNextControl, &next)) return false;
        if (!next) {
            *outTail = current;
            return true;
        }
        current = next;
    }
    return false;
}

static bool DiscoverWristSide(const DetachedRigFrame& rig, bool left,
                              WristRuntimeSide* out)
{
    if (!out) return false;
    const UE3Array32 lists{ rig.listData, rig.listCount, rig.listCount };
    const char* rollBone = left ? "LeftForeArmRoll" : "RightForeArmRoll";
    const char* handBone = left ? "LeftHand" : "RightHand";
    const int rollBoneIndex = left ? 41 : 71;
    UE3ControlListHead roll{}, hand{};
    const int rollSlot = FindControlListSlot(lists, rollBone, &roll);
    const int handSlot = FindControlListSlot(lists, handBone, &hand);
    if (rollSlot < 0 || handSlot < 0 ||
        !LooksLikeRigObject(roll.controlHead, "SkelControlSingleBone") ||
        !LooksLikeLiveUObject(hand.controlHead) || roll.controlHead == hand.controlHead)
        return false;

    uint32_t rollNext = ~0u;
    uint8_t rollMap = 0xFF;
    uintptr_t tail = 0;
    if (!SafeU32(roll.controlHead + g_offSkelNextControl, &rollNext) || rollNext != 0 ||
        !SafeRead(rig.indexData + rollBoneIndex, &rollMap, 1) ||
        rollMap != (uint8_t)(rollSlot + rig.mapBias) ||
        !FindHandControlTail(hand.controlHead, roll.controlHead, &tail)) return false;

    WristRuntimeSide side{};
    side.control = roll.controlHead;
    side.handTail = tail;
    side.rollMapAddress = rig.indexData + rollBoneIndex;
    side.originalRollMap = rollMap;
    side.unmappedValue = rig.mapBias == 1 ? 0 : 0xFF;
    *out = side;
    return true;
}

static bool GripQuaternionToUERotator(const XrQuaternionf& raw, bool leftHand,
                                     const int calibrationDeg[2][3], int32_t out[3],
                                     XrQuaternionf* outOrientation = nullptr)
{
    if (!out || !std::isfinite(raw.x) || !std::isfinite(raw.y) ||
        !std::isfinite(raw.z) || !std::isfinite(raw.w)) return false;
    const float rawNorm2 = raw.x*raw.x + raw.y*raw.y + raw.z*raw.z + raw.w*raw.w;
    if (!std::isfinite(rawNorm2) || rawNorm2 < 0.98f || rawNorm2 > 1.02f) return false;
    const float rawInv = 1.0f / sqrtf(rawNorm2);
    const XrQuaternionf tracked{
        raw.x*rawInv, raw.y*rawInv, raw.z*rawInv, raw.w*rawInv
    };

    // The controller's grip frame tracks correctly; only the grip-to-mirrored-hand rest frame
    // needs calibration. These six values are changed live by the overlay. Build a LOCAL UE-like
    // yaw/pitch/roll offset, then post-multiply it so every subsequent tracked motion remains
    // intact. UE positive pitch is the negative conventional +Y quaternion rotation.
    const int hand = leftHand ? 0 : 1;
    const float halfToRad = 3.14159265358979323846f / 360.0f;
    const float hp = calibrationDeg[hand][0] * halfToRad;
    const float hy = calibrationDeg[hand][1] * halfToRad;
    const float hr = calibrationDeg[hand][2] * halfToRad;
    const XrQuaternionf pitchOffset{ 0.0f, -sinf(hp), 0.0f, cosf(hp) };
    const XrQuaternionf yawOffset{ 0.0f, 0.0f, sinf(hy), cosf(hy) };
    const XrQuaternionf rollOffset{ sinf(hr), 0.0f, 0.0f, cosf(hr) };
    const XrQuaternionf gripToHand =
        MultiplyQuaternion(MultiplyQuaternion(yawOffset, pitchOffset), rollOffset);
    const XrQuaternionf q = MultiplyQuaternion(tracked, gripToHand);
    const float norm2 = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    if (!std::isfinite(norm2) || norm2 < 0.98f || norm2 > 1.02f) return false;
    const float inv = 1.0f / sqrtf(norm2);
    const XrQuaternionf n{ q.x*inv, q.y*inv, q.z*inv, q.w*inv };
    if (outOrientation) *outOrientation = n;
    const MEVR_Vec3 forward = RotateByQuaternion(n, { 1.0f, 0.0f, 0.0f });
    const MEVR_Vec3 right   = RotateByQuaternion(n, { 0.0f, 1.0f, 0.0f });
    const MEVR_Vec3 up      = RotateByQuaternion(n, { 0.0f, 0.0f, 1.0f });
    const float horizontal = sqrtf(forward.x*forward.x + forward.y*forward.y);
    const float pitch = atan2f(forward.z, horizontal);
    const float yaw = atan2f(forward.y, forward.x);
    // UE3's positive roll rotates its +Y/right axis toward -Z, the opposite sign of the
    // conventional right-handed +X quaternion rotation used to construct the basis.
    const float roll = atan2f(-right.z, up.z);
    const float unrealUnits = 32768.0f / 3.14159265358979323846f;
    out[0] = (int32_t)lroundf(pitch * unrealUnits);
    out[1] = (int32_t)lroundf(yaw * unrealUnits);
    out[2] = (int32_t)lroundf(roll * unrealUnits);
    return true;
}

static bool WriteWristControl(uintptr_t control, const int32_t rotation[3])
{
    const uint8_t worldSpace = 0;
    const float one = 1.0f, zero = 0.0f;
    if (!WriteRigBytes(control + g_offSingleBoneRotation,
                       rotation, sizeof(int32_t) * 3) ||
        !WriteRigBytes(control + g_offSingleBoneRotationSpace, &worldSpace, 1) ||
        !WriteRigBytes(control + g_offSkelStrengthTarget, &one, sizeof(one)) ||
        !WriteRigBytes(control + g_offSkelBlendTimeToGo, &zero, sizeof(zero)) ||
        !WriteRigBytes(control + g_offSkelControlStrength, &one, sizeof(one))) return false;
    int32_t readRotation[3]{};
    uint8_t readSpace = 0xFF;
    float strength = -1.0f, target = -1.0f, blend = -1.0f;
    return SafeRead(control + g_offSingleBoneRotation,
                    readRotation, sizeof(readRotation)) &&
           SafeRead(control + g_offSingleBoneRotationSpace, &readSpace, 1) &&
           SafeRead(control + g_offSkelControlStrength, &strength, sizeof(strength)) &&
           SafeRead(control + g_offSkelStrengthTarget, &target, sizeof(target)) &&
           SafeRead(control + g_offSkelBlendTimeToGo, &blend, sizeof(blend)) &&
           readRotation[0] == rotation[0] && readRotation[1] == rotation[1] &&
           readRotation[2] == rotation[2] && readSpace == 0 &&
           fabsf(strength - 1.0f) < 0.001f && fabsf(target - 1.0f) < 0.001f &&
           fabsf(blend) < 0.001f;
}

static bool ApplyOneWristSide(const WristRuntimeSide& runtime,
                              const P13HandPoseSnapshot& pose,
                              bool leftHand,
                              WristSideFrame* frame)
{
    if (!frame) return false;
    int32_t rotation[3]{};
    if (!GripQuaternionToUERotator(pose.worldOrientation, leftHand,
                                   g_wristCalibrationDeg, rotation)) return false;
    WristSideFrame saved{};
    saved.active = CaptureDetachedControl(runtime.control, &saved.saved);
    saved.control = runtime.control;
    saved.handTail = runtime.handTail;
    saved.rollMapAddress = runtime.rollMapAddress;
    saved.originalRollMap = runtime.originalRollMap;
    if (!saved.active) return false;

    const uint32_t appended = (uint32_t)runtime.control;
    const bool wrote =
        WriteRigBytes(runtime.rollMapAddress, &runtime.unmappedValue, 1) &&
        WriteRigBytes(runtime.handTail + g_offSkelNextControl,
                      &appended, sizeof(appended)) &&
        WriteWristControl(runtime.control, rotation);
    uint8_t readMap = 0xFF;
    uint32_t readNext = 0;
    if (!wrote || !SafeRead(runtime.rollMapAddress, &readMap, 1) ||
        !SafeU32(runtime.handTail + g_offSkelNextControl, &readNext) ||
        readMap != runtime.unmappedValue || readNext != appended) {
        *frame = saved;
        return false;
    }
    *frame = saved;
    return true;
}

static void RestoreOneWristSide(const WristSideFrame& side, bool* restored)
{
    if (!side.active || !restored) return;
    const uint32_t noNext = 0;
    *restored = RestoreDetachedControl(side.control, side.saved) && *restored;
    *restored = WriteRigBytes(side.handTail + g_offSkelNextControl,
                              &noNext, sizeof(noNext)) && *restored;
    *restored = WriteRigBytes(side.rollMapAddress,
                              &side.originalRollMap, 1) && *restored;
}

static void RestoreWristRotationOverridesBeforeGame(uintptr_t pawn)
{
    if (!g_wristOverride.active) return;
    const WristOverrideState saved = g_wristOverride;
    g_wristOverride = {};
    if (saved.pawn != pawn || !LooksLikePlayerPawn(pawn)) {
        Log("[hands-wrist] prior override discarded without writes: pawn replaced");
        return;
    }
    bool restored = true;
    RestoreOneWristSide(saved.left, &restored);
    RestoreOneWristSide(saved.right, &restored);
    if (!restored) {
        g_wristWriteFault = true;
        Log("[hands-wrist] RESTORE FAILED - wrist rotation disabled for this process");
    }
}

static void ApplyBorrowedWristRotations(uintptr_t pawn, const P13PoseSnapshot& pose)
{
    if (!g_motionHands || g_wristWriteFault) return;
    DetachedRigFrame rig{};
    WristRuntimeSide leftRuntime{}, rightRuntime{};
    if (!DiscoverDetachedRig(pawn, &rig) ||
        !DiscoverWristSide(rig, true, &leftRuntime) ||
        !DiscoverWristSide(rig, false, &rightRuntime)) {
        static bool reported = false;
        if (!reported) {
            Log("[hands-wrist] waiting: bilateral ForeArmRoll controls or Hand chain tails"
                " did not validate");
            reported = true;
        }
        return;
    }
    if (g_wristReportedMesh != rig.mesh) {
        Log("*** [hands-wrist] bilateral rotation rig validated: borrowed roll controls"
            " append after both authored Hand chains; roll bones 41/71 disabled only while owned");
        g_wristReportedMesh = rig.mesh;
    }

    const XrSpaceLocationFlags orientationNeed =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    uintptr_t ignored = 0;
    const bool leftEligible =
        P13Eligibility(pawn, pose.left, pose.presentFrame, true,
                       &ignored, nullptr, nullptr) == P13_READY &&
        (pose.left.flags & orientationNeed) == orientationNeed;
    const bool rightEligible =
        P13Eligibility(pawn, pose.right, pose.presentFrame, false,
                       &ignored, nullptr, nullptr) == P13_READY &&
        (pose.right.flags & orientationNeed) == orientationNeed;
    if (!leftEligible && !rightEligible) return;

    WristOverrideState frame{};
    frame.active = true;
    frame.pawn = pawn;
    bool wrote = true;
    if (leftEligible)
        wrote = ApplyOneWristSide(leftRuntime, pose.left, true, &frame.left);
    if (rightEligible && wrote)
        wrote = ApplyOneWristSide(rightRuntime, pose.right, false, &frame.right);
    g_wristOverride = frame;
    if (!wrote) {
        RestoreWristRotationOverridesBeforeGame(pawn);
        g_wristWriteFault = true;
        Log("[hands-wrist] WRITE/READ-BACK FAILED - wrist rotation disabled;");
        Log("[hands-wrist] position IK and shoulder detachment remain active");
        return;
    }

    if (leftEligible) ++g_wristLeftWrites;
    if (rightEligible) ++g_wristRightWrites;
    if ((leftEligible && g_wristLeftWrites == 1) ||
        (rightEligible && g_wristRightWrites == 1)) {
        Log("*** [hands-wrist] controller-driven world rotation acquired: L=%d R=%d;"
            " exact read-back passed", leftEligible ? 1 : 0, rightEligible ? 1 : 0);
    }
    if (g_motionHandsDebug &&
        ((leftEligible && g_wristLeftWrites % 600 == 0) ||
         (rightEligible && g_wristRightWrites % 600 == 0))) {
        int32_t l[3]{}, r[3]{};
        GripQuaternionToUERotator(pose.left.worldOrientation, true,
                                  g_wristCalibrationDeg, l);
        GripQuaternionToUERotator(pose.right.worldOrientation, false,
                                  g_wristCalibrationDeg, r);
        Log("[hands-wrist] UE rotator P/Y/R: L(%d,%d,%d) R(%d,%d,%d);"
            " writes L %ld R %ld", l[0], l[1], l[2], r[0], r[1], r[2],
            g_wristLeftWrites, g_wristRightWrites);
    }
}

// ================================================= P1.4d dedicated hand + forearm rotation
//
// The shipped tree contains two dormant generic single-bone controls on root and Hips. During a
// VR-owned frame, append HipsControl after the left Hand chain and SwingControl after the right
// Hand chain. This frees the authored ForeArmRoll controls to remain on their intended helper
// bones. Every link, map byte, flag word and control value is restored before the game updates.
// If any measured invariant fails, the proven borrowed-ForeArmRoll wrist path above remains the
// automatic fallback.
struct DedicatedRotationSideFrame {
    bool active = false;
    uintptr_t donor = 0, forearm = 0, handTail = 0;
    uint32_t sourceMapAddress = 0;
    uint8_t originalSourceMap = 0xFF;
    uint32_t originalDonorNext = 0;
    uint32_t originalDonorFlags = 0, originalForearmFlags = 0;
    DetachedControlSave donorSaved{}, forearmSaved{};
};

struct DedicatedRotationOverrideState {
    bool active = false;
    uintptr_t pawn = 0;
    int flagsOffset = -1;
    DedicatedRotationSideFrame left{}, right{};
};

struct DedicatedRotationRuntimeSide {
    uintptr_t donor = 0, forearm = 0, handTail = 0;
    uint32_t sourceMapAddress = 0;
    uint8_t originalSourceMap = 0xFF, unmappedValue = 0xFF;
    uint32_t donorNext = 0, donorFlags = 0, forearmFlags = 0;
};

struct DedicatedRotationRig {
    uintptr_t pawn = 0, mesh = 0;
    int flagsOffset = -1;
    uint32_t rotationOnlyFlags = 0;
    // Carried from the validated base rig so diagnostics can read composed bone frames.
    uint32_t spaceBaseData = 0;
    int spaceBaseCount = 0;
    UE3Matrix44 localToWorld{};
    DedicatedRotationRuntimeSide left{}, right{};
};

// Why the most recent DiscoverDedicatedRotationRig returned false. A transient failure swaps
// the whole rotation topology to the borrowed-wrist fallback for that frame, which is a visible
// arm snap - the transition log must therefore say exactly which invariant broke.
static const char* g_dedicatedRotationFailure = "never attempted";

// Rows 0..2 of a SpaceBases matrix are the bone's local axes in component space - the same
// row-vector convention BoneWorldPosition uses for its translation row. Compose with
// LocalToWorld and normalize away component scale to recover the bone's world basis.
static bool BoneWorldBasis(uint32_t spaceBaseData, int spaceBaseCount,
                           const UE3Matrix44& localToWorld, int boneIndex,
                           MEVR_Vec3 axes[3])
{
    if (!axes || boneIndex < 0 || boneIndex >= spaceBaseCount || !spaceBaseData) return false;
    UE3Matrix44 bone{};
    if (!SafeRead(spaceBaseData + (uint32_t)boneIndex * sizeof(UE3Matrix44),
                  &bone, sizeof(bone))) return false;
    for (int r = 0; r < 3; ++r) {
        const float x = bone.m[r][0], y = bone.m[r][1], z = bone.m[r][2];
        const MEVR_Vec3 world{
            x*localToWorld.m[0][0] + y*localToWorld.m[1][0] + z*localToWorld.m[2][0],
            x*localToWorld.m[0][1] + y*localToWorld.m[1][1] + z*localToWorld.m[2][1],
            x*localToWorld.m[0][2] + y*localToWorld.m[1][2] + z*localToWorld.m[2][2]
        };
        const float length = VecLength(world);
        if (!std::isfinite(length) || length < 1.0e-6f) return false;
        axes[r] = { world.x / length, world.y / length, world.z / length };
    }
    return FiniteVec(axes[0]) && FiniteVec(axes[1]) && FiniteVec(axes[2]);
}

// Same axis conventions as GripQuaternionToUERotator: X forward, Y right, Z up.
static void UERotatorDegFromBasis(const MEVR_Vec3 axes[3], float out[3])
{
    const float horizontal = sqrtf(axes[0].x*axes[0].x + axes[0].y*axes[0].y);
    out[0] = atan2f(axes[0].z, horizontal) * 57.2957795f;
    out[1] = atan2f(axes[0].y, axes[0].x) * 57.2957795f;
    out[2] = atan2f(-axes[1].z, axes[2].z) * 57.2957795f;
}

// The exact rotator extraction GripQuaternionToUERotator performs, reusable for any world
// orientation that must be delivered through a SkelControlSingleBone rotator.
static void UERotatorFromQuaternion(const XrQuaternionf& q, int32_t out[3])
{
    const MEVR_Vec3 forward = RotateByQuaternion(q, { 1.0f, 0.0f, 0.0f });
    const MEVR_Vec3 right   = RotateByQuaternion(q, { 0.0f, 1.0f, 0.0f });
    const MEVR_Vec3 up      = RotateByQuaternion(q, { 0.0f, 0.0f, 1.0f });
    const float horizontal = sqrtf(forward.x*forward.x + forward.y*forward.y);
    const float unrealUnits = 32768.0f / 3.14159265358979323846f;
    out[0] = (int32_t)lroundf(atan2f(forward.z, horizontal) * unrealUnits);
    out[1] = (int32_t)lroundf(atan2f(forward.y, forward.x) * unrealUnits);
    out[2] = (int32_t)lroundf(atan2f(-right.z, up.z) * unrealUnits);
}

static float QuatAngleDeg(const XrQuaternionf& a, const XrQuaternionf& b)
{
    float dot = fabsf(a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w);
    if (dot > 1.0f) dot = 1.0f;
    return 2.0f * acosf(dot) * 57.2957795f;
}

// Twist of target relative to reference about the reference's local +X, in radians.
static float RelativeTwistAboutXRad(const XrQuaternionf& reference,
                                    const XrQuaternionf& target)
{
    const XrQuaternionf inverse{ -reference.x, -reference.y, -reference.z, reference.w };
    XrQuaternionf rel = MultiplyQuaternion(inverse, target);
    if (rel.w < 0.0f) { rel.x = -rel.x; rel.y = -rel.y; rel.z = -rel.z; rel.w = -rel.w; }
    return 2.0f * atan2f(rel.x, rel.w);
}

// Decompose target-relative-to-reference into the twist about the reference's local X and the
// total separation angle. The twist is the exact number an ideal roll helper would carry.
static void RelativeTwistDeg(const XrQuaternionf& reference, const XrQuaternionf& target,
                             float* twistDeg, float* totalDeg)
{
    if (twistDeg) *twistDeg = RelativeTwistAboutXRad(reference, target) * 57.2957795f;
    if (totalDeg) *totalDeg = QuatAngleDeg(reference, target);
}

static DedicatedRotationOverrideState g_dedicatedRotationOverride{};
static bool g_dedicatedRotationWriteFault = false;
static uintptr_t g_dedicatedRotationReportedMesh = 0;
static long g_dedicatedLeftWrites = 0, g_dedicatedRightWrites = 0;
struct ForearmDiagnostic {
    bool valid = false;
    bool haveOrientation = false;
    bool outputJump = false;
    int32_t wrist[3]{};
    int32_t forearmRotator[3]{};
    // The game's own freshly computed ForeArmRoll control value, captured after its update and
    // before our overwrite. Its space and values document the helper's authored convention.
    bool gameForearmValid = false;
    uint8_t gameForearmSpace = 0xFF;
    int32_t gameForearm[3]{};
    XrQuaternionf handOrientation{};
    bool haveForearmBone = false;
    XrQuaternionf forearmBoneOrientation{};
    XrQuaternionf lastOrientation{};
    // What was written on the PRECEDING update, for comparison against SpaceBases: the composed
    // skeleton available during this update is that older frame's result.
    bool havePreviousOrientation = false;
    XrQuaternionf previousOrientation{};
};
static ForearmDiagnostic g_forearmDiagnostic[2]{};
// Continuity reference for the forearm twist. The measured twist is circular, and run 19
// caught the written roll target flipping 170 degrees while the hand moved 2: the shortest
// representation changes sign at the +/-180 antipode, which the soft cap then amplifies into a
// +95 -> -95 flip. Unwrapping against the previous update keeps it continuous; anatomy keeps
// it bounded; eligibility gaps reset it alongside the jump diagnostic.
static bool g_forearmTwistUnwrapValid[2] = { false, false };
static float g_forearmTwistUnwrap[2] = { 0.0f, 0.0f };

static bool MotionTurnInputActive()
{
    if (!g_padEnabled || !g_padLockReady) return false;
    EnterCriticalSection(&g_padLock);
    const SHORT lookX = g_pad.Gamepad.sThumbRX;
    LeaveCriticalSection(&g_padLock);
    const double now = NowMs();
    static double settleUntilMs = 0.0;
    if (abs((int)lookX) > 4096) { // outside the game's normal stick dead zone
        // SpaceBases describes the preceding game evaluation while the controller target is the
        // newest published render pose. Let those reference spaces settle after artificial yaw
        // before shoulder reach engagement resumes.
        settleUntilMs = now + 150.0;
        return true;
    }
    return now < settleUntilMs;
}

static bool DiscoverDedicatedRotationSide(const DetachedRigFrame& rig,
                                          const UE3Array32& lists,
                                          const UE3Array32& indices,
                                          bool leftHand, int flagsOffset,
                                          DedicatedRotationRuntimeSide* out)
{
    if (!out) return false;
    const char* sourceBone = leftHand ? "Hips" : "root";
    const char* rollBone = leftHand ? "LeftForeArmRoll" : "RightForeArmRoll";
    const char* handBone = leftHand ? "LeftHand" : "RightHand";
    const int sourceBoneIndex = leftHand ? 1 : 0;
    const int rollBoneIndex = leftHand ? 41 : 71;
    UE3ControlListHead source{}, roll{}, hand{};
    const int sourceSlot = FindControlListSlot(lists, sourceBone, &source);
    const int rollSlot = FindControlListSlot(lists, rollBone, &roll);
    const int handSlot = FindControlListSlot(lists, handBone, &hand);
    if (sourceSlot < 0 || rollSlot < 0 || handSlot < 0 ||
        !LooksLikeRigObject(source.controlHead, "SkelControlSingleBone") ||
        !LooksLikeRigObject(roll.controlHead, "SkelControlSingleBone") ||
        !LooksLikeLiveUObject(hand.controlHead) ||
        source.controlHead == roll.controlHead || source.controlHead == hand.controlHead) {
        g_dedicatedRotationFailure = leftHand
            ? "left source/roll/hand control-list slots or classes"
            : "right source/roll/hand control-list slots or classes";
        return false;
    }

    uint8_t sourceMap = 0xFF, rollMap = 0xFF;
    uint32_t donorNext = ~0u, forearmNext = ~0u;
    uint32_t donorFlags = 0, forearmFlags = 0;
    uintptr_t handTail = 0;
    if (!ReadMappedControl(indices, sourceBoneIndex, &sourceMap) ||
        !ReadMappedControl(indices, rollBoneIndex, &rollMap) ||
        sourceMap != (uint8_t)(sourceSlot + rig.mapBias) ||
        rollMap != (uint8_t)(rollSlot + rig.mapBias)) {
        g_dedicatedRotationFailure = leftHand
            ? "left source/roll SkelControlIndex map bytes"
            : "right source/roll SkelControlIndex map bytes";
        return false;
    }
    if (!SafeU32(source.controlHead + g_offSkelNextControl, &donorNext) ||
        !SafeU32(roll.controlHead + g_offSkelNextControl, &forearmNext) ||
        forearmNext != 0 ||
        !SafeU32(source.controlHead + flagsOffset, &donorFlags) ||
        !SafeU32(roll.controlHead + flagsOffset, &forearmFlags) ||
        !FindHandControlTail(hand.controlHead, source.controlHead, &handTail)) {
        g_dedicatedRotationFailure = leftHand
            ? "left donor/forearm links, flags, or hand chain tail"
            : "right donor/forearm links, flags, or hand chain tail";
        return false;
    }

    // HipsControl is a one-node list. SwingControl can still point at RootControl when the left
    // shoulder is attached, or be cut to zero by this frame's validated detachment override.
    if ((leftHand && donorNext != 0) ||
        (!leftHand && donorNext != 0 && donorNext != rig.leftControl)) {
        g_dedicatedRotationFailure = leftHand
            ? "left donor NextControl topology"
            : "right donor NextControl topology";
        return false;
    }

    DedicatedRotationRuntimeSide side{};
    side.donor = source.controlHead;
    side.forearm = roll.controlHead;
    side.handTail = handTail;
    side.sourceMapAddress = indices.data + sourceBoneIndex;
    side.originalSourceMap = sourceMap;
    side.unmappedValue = rig.mapBias == 1 ? 0 : 0xFF;
    side.donorNext = donorNext;
    side.donorFlags = donorFlags;
    side.forearmFlags = forearmFlags;
    *out = side;
    return true;
}

static bool DiscoverDedicatedRotationRig(uintptr_t pawn, DedicatedRotationRig* out)
{
    if (!out || g_dedicatedRotationWriteFault ||
        g_offSingleBoneTranslation < 4 || !LooksLikePlayerPawn(pawn)) {
        g_dedicatedRotationFailure = "write fault latched, layout missing, or pawn invalid";
        return false;
    }
    DetachedRigFrame base{};
    if (!DiscoverDetachedRig(pawn, &base)) {
        g_dedicatedRotationFailure = "base detached rig (mesh/tree/lists/SpaceBases)";
        return false;
    }
    const UE3Array32 lists{ base.listData, base.listCount, base.listCount };
    const UE3Array32 indices{ base.indexData, base.indexCount, base.indexCount };
    const int flagsOffset = g_offSingleBoneTranslation - 4;
    DedicatedRotationRuntimeSide left{}, right{};
    if (!DiscoverDedicatedRotationSide(base, lists, indices, true, flagsOffset, &left) ||
        !DiscoverDedicatedRotationSide(base, lists, indices, false, flagsOffset, &right))
        return false;   // the side discovery already recorded the exact failed invariant
    if (left.forearmFlags == 0 || left.forearmFlags != right.forearmFlags ||
        left.donor == right.donor || left.forearm == right.forearm) {
        g_dedicatedRotationFailure = "bilateral forearm flag agreement or object identity";
        return false;
    }

    // Both authored ForeArmRoll controls independently agreeing on this word is the runtime
    // validation for the otherwise stripped packed-bool metadata. Copy the complete known-good
    // rotation-only word; no individual bit mask is guessed.
    DedicatedRotationRig rig{};
    rig.pawn = pawn; rig.mesh = base.mesh;
    rig.flagsOffset = flagsOffset;
    rig.rotationOnlyFlags = left.forearmFlags;
    rig.spaceBaseData = base.spaceBaseData;
    rig.spaceBaseCount = base.spaceBaseCount;
    rig.localToWorld = base.localToWorld;
    rig.left = left; rig.right = right;
    *out = rig;
    return true;
}

static bool WriteDedicatedHandControl(uintptr_t control, int flagsOffset,
                                      uint32_t rotationOnlyFlags,
                                      const int32_t rotation[3])
{
    const MEVR_Vec3 zeroTranslation{};
    const uint8_t worldSpace = 0;
    const float one = 1.0f, zero = 0.0f;
    if (!WriteRigBytes(control + flagsOffset, &rotationOnlyFlags, sizeof(rotationOnlyFlags)) ||
        !WriteRigBytes(control + g_offSingleBoneTranslation,
                       &zeroTranslation, sizeof(zeroTranslation)) ||
        !WriteRigBytes(control + g_offSingleBoneTranslationSpace, &worldSpace, 1) ||
        !WriteRigBytes(control + g_offSingleBoneRotation, rotation, sizeof(int32_t) * 3) ||
        !WriteRigBytes(control + g_offSingleBoneRotationSpace, &worldSpace, 1) ||
        !WriteRigBytes(control + g_offSkelStrengthTarget, &one, sizeof(one)) ||
        !WriteRigBytes(control + g_offSkelBlendTimeToGo, &zero, sizeof(zero)) ||
        !WriteRigBytes(control + g_offSkelControlStrength, &one, sizeof(one))) return false;
    MEVR_Vec3 readTranslation{};
    int32_t readRotation[3]{};
    uint8_t readRotationSpace = 0xFF;
    uint32_t readFlags = 0;
    return SafeRead(control + g_offSingleBoneTranslation,
                    &readTranslation, sizeof(readTranslation)) &&
           SafeRead(control + g_offSingleBoneRotation, readRotation, sizeof(readRotation)) &&
           SafeRead(control + g_offSingleBoneRotationSpace, &readRotationSpace, 1) &&
           SafeU32(control + flagsOffset, &readFlags) &&
           VecLength(readTranslation) < 0.001f &&
           readRotation[0] == rotation[0] && readRotation[1] == rotation[1] &&
           readRotation[2] == rotation[2] && readRotationSpace == worldSpace &&
           readFlags == rotationOnlyFlags;
}

static bool ApplyOneDedicatedRotationSide(const DedicatedRotationRig& rig,
                                          const DedicatedRotationRuntimeSide& runtime,
                                          const P13HandPoseSnapshot& pose, bool leftHand,
                                          DedicatedRotationSideFrame* out)
{
    if (!out) return false;
    int32_t rotation[3]{};
    XrQuaternionf handOrientation{};
    if (!GripQuaternionToUERotator(pose.worldOrientation, leftHand,
                                   g_wristCalibrationDeg, rotation,
                                   &handOrientation)) return false;

    // MEASURED rig convention (run 7's [hands-bones]): the composed ForeArmRoll helper always
    // carries the FOREARM's swing, the game's own control contributes a pure roll (space-4
    // {0,0,+25} left / {0,-4,+95} right), and at the authored neutral pose the helper's twist
    // essentially equals the hand's twist relative to the forearm (-26 vs -26 L, -19 vs -14 R).
    // Writing the hand's full world orientation here instead put the helper up to 168 degrees
    // from its parent and corkscrewed the visible forearm away from the hand. Build the target
    // the way the rig expects: the forearm bone's swing composed with the hand's twist about
    // the forearm axis, plus the tunable rest trim. SpaceBases is one update stale, which lags
    // only the swing component by one update; the twist follows the live controller. The
    // world-space write path is kept because run 7 proved it lands exactly as commanded.
    const int hand = leftHand ? 0 : 1;
    const int armBone = leftHand ? 17 : 46;
    bool haveForearmTarget = false;
    bool haveForearmBone = false;
    XrQuaternionf forearmBoneQ{};
    int32_t forearmRotation[3]{};
    XrQuaternionf forearmOrientation{};
    MEVR_Vec3 forearmAxes[3];
    if (BoneWorldBasis(rig.spaceBaseData, rig.spaceBaseCount, rig.localToWorld,
                       armBone + 1, forearmAxes)) {
        const XrQuaternionf forearmQ =
            QuaternionFromUEBasis(forearmAxes[0], forearmAxes[1], forearmAxes[2]);
        forearmBoneQ = forearmQ;
        haveForearmBone = true;
        float twist = RelativeTwistAboutXRad(forearmQ, handOrientation) +
                      g_forearmRollCalibrationDeg[hand] *
                          (3.14159265358979323846f / 180.0f);
        // Take the branch nearest the previous update's twist, slew-limited and bounded.
        // Run 23 measured the naked unwrap drifting a full turn: singular-zone forearm-bone
        // flips feed +/-170-degree APPARENT twist steps, and nearest-branch continuity happily
        // accumulated them until the right helper sat pinned at the +95 cap while the wrist
        // read -50. The slew cap (25 deg/update is triple any deliberate wring) turns those
        // spikes into negligible nudges that cancel, and the fold keeps the reference inside
        // +/-180 so drift is structurally impossible rather than merely unlikely.
        if (g_forearmTwistUnwrapValid[hand]) {
            float unwrapDelta = twist - g_forearmTwistUnwrap[hand];
            while (unwrapDelta > 3.14159265358979323846f)
                unwrapDelta -= 6.28318530717958647692f;
            while (unwrapDelta < -3.14159265358979323846f)
                unwrapDelta += 6.28318530717958647692f;
            const float maxTwistStep = 25.0f * (3.14159265358979323846f / 180.0f);
            if (unwrapDelta > maxTwistStep) unwrapDelta = maxTwistStep;
            else if (unwrapDelta < -maxTwistStep) unwrapDelta = -maxTwistStep;
            twist = g_forearmTwistUnwrap[hand] + unwrapDelta;
            if (twist > 3.14159265358979323846f) twist -= 6.28318530717958647692f;
            else if (twist < -3.14159265358979323846f) twist += 6.28318530717958647692f;
        }
        g_forearmTwistUnwrap[hand] = twist;
        g_forearmTwistUnwrapValid[hand] = true;
        // Candy-wrapper guard, sized from measurement: run 8 reached -139 degrees of
        // hand-in-forearm twist and the skin spanning helper-to-elbow visibly collapsed
        // ("towel getting wrung"). Carry twist 1:1 below the knee so everyday poses stay
        // exact, then compress smoothly toward an asymptote the skinning tolerates; the short
        // wrist band absorbs the remainder. Unlike the retired hard +/-55 limiter, there is
        // no stop and no reference state: motion never freezes, it only loses gain.
        const float knee = 55.0f * (3.14159265358979323846f / 180.0f);
        const float ceiling = 95.0f * (3.14159265358979323846f / 180.0f);
        const float magnitude = fabsf(twist);
        if (magnitude > knee) {
            const float span = ceiling - knee;
            const float compressed = knee + span * tanhf((magnitude - knee) / span);
            twist = twist < 0.0f ? -compressed : compressed;
        }
        const XrQuaternionf twistQ{ sinf(twist * 0.5f), 0.0f, 0.0f, cosf(twist * 0.5f) };
        XrQuaternionf target = MultiplyQuaternion(forearmQ, twistQ);
        if (NormalizedQuaternion(target, &target)) {
            forearmOrientation = target;
            UERotatorFromQuaternion(target, forearmRotation);
            haveForearmTarget = true;
        }
    }

    ForearmDiagnostic& diag = g_forearmDiagnostic[hand];
    diag.valid = true;
    diag.haveForearmBone = haveForearmBone;
    diag.forearmBoneOrientation = forearmBoneQ;
    diag.wrist[0] = rotation[0];
    diag.wrist[1] = rotation[1];
    diag.wrist[2] = rotation[2];
    diag.handOrientation = handOrientation;
    if (haveForearmTarget) {
        diag.forearmRotator[0] = forearmRotation[0];
        diag.forearmRotator[1] = forearmRotation[1];
        diag.forearmRotator[2] = forearmRotation[2];
        if (diag.haveOrientation) {
            // Measured between quaternions, not rotator components: the full-rotator write is
            // immune to Euler branch flips, so only a real orientation step may count as a jump.
            float dot = fabsf(forearmOrientation.x*diag.lastOrientation.x +
                              forearmOrientation.y*diag.lastOrientation.y +
                              forearmOrientation.z*diag.lastOrientation.z +
                              forearmOrientation.w*diag.lastOrientation.w);
            if (dot > 1.0f) dot = 1.0f;
            diag.outputJump = diag.outputJump ||
                2.0f * acosf(dot) > 15.0f * (3.14159265358979323846f / 180.0f);
        }
        diag.previousOrientation = diag.lastOrientation;
        diag.havePreviousOrientation = diag.haveOrientation;
        diag.lastOrientation = forearmOrientation;
        diag.haveOrientation = true;
    }

    DedicatedRotationSideFrame save{};
    save.active = CaptureDetachedControl(runtime.donor, &save.donorSaved) &&
                  CaptureDetachedControl(runtime.forearm, &save.forearmSaved);
    diag.gameForearmValid = save.active;
    if (save.active) {
        diag.gameForearm[0] = save.forearmSaved.rotation[0];
        diag.gameForearm[1] = save.forearmSaved.rotation[1];
        diag.gameForearm[2] = save.forearmSaved.rotation[2];
        diag.gameForearmSpace = save.forearmSaved.rotationSpace;
    }
    save.donor = runtime.donor; save.forearm = runtime.forearm;
    save.handTail = runtime.handTail;
    save.sourceMapAddress = runtime.sourceMapAddress;
    save.originalSourceMap = runtime.originalSourceMap;
    save.originalDonorNext = runtime.donorNext;
    save.originalDonorFlags = runtime.donorFlags;
    save.originalForearmFlags = runtime.forearmFlags;
    if (!save.active) return false;

    const uint32_t noNext = 0;
    const uint32_t appended = (uint32_t)runtime.donor;
    const bool wrote =
        WriteRigBytes(runtime.sourceMapAddress, &runtime.unmappedValue, 1) &&
        WriteRigBytes(runtime.donor + g_offSkelNextControl, &noNext, sizeof(noNext)) &&
        WriteRigBytes(runtime.handTail + g_offSkelNextControl, &appended, sizeof(appended)) &&
        WriteDedicatedHandControl(runtime.donor, rig.flagsOffset,
                                  rig.rotationOnlyFlags, rotation) &&
        // WriteWristControl is byte-for-byte the P1.4c write that already drove these exact
        // ForeArmRoll SkelControlSingleBone objects in world space with their authored flags,
        // so the helper's authored flags and translation stay put. On a tick where the forearm
        // basis is unreadable the game's own captured value simply remains in effect.
        (!haveForearmTarget || WriteWristControl(runtime.forearm, forearmRotation));
    uint8_t readMap = 0xFF;
    uint32_t readDonorNext = ~0u, readTailNext = 0;
    if (!wrote || !SafeRead(runtime.sourceMapAddress, &readMap, 1) ||
        !SafeU32(runtime.donor + g_offSkelNextControl, &readDonorNext) ||
        !SafeU32(runtime.handTail + g_offSkelNextControl, &readTailNext) ||
        readMap != runtime.unmappedValue || readDonorNext != 0 || readTailNext != appended) {
        *out = save;
        return false;
    }
    *out = save;
    return true;
}

static void RestoreOneDedicatedRotationSide(const DedicatedRotationSideFrame& side,
                                            int flagsOffset, bool* restored)
{
    if (!side.active || !restored) return;
    const uint32_t noNext = 0;
    *restored = WriteRigBytes(side.handTail + g_offSkelNextControl,
                              &noNext, sizeof(noNext)) && *restored;
    *restored = RestoreDetachedControl(side.forearm, side.forearmSaved) && *restored;
    *restored = WriteRigBytes(side.forearm + flagsOffset,
                              &side.originalForearmFlags,
                              sizeof(side.originalForearmFlags)) && *restored;
    *restored = RestoreDetachedControl(side.donor, side.donorSaved) && *restored;
    *restored = WriteRigBytes(side.donor + flagsOffset, &side.originalDonorFlags,
                              sizeof(side.originalDonorFlags)) && *restored;
    *restored = WriteRigBytes(side.donor + g_offSkelNextControl,
                              &side.originalDonorNext,
                              sizeof(side.originalDonorNext)) && *restored;
    *restored = WriteRigBytes(side.sourceMapAddress, &side.originalSourceMap, 1) && *restored;
}

static void RestoreDedicatedHandForearmOverridesBeforeGame(uintptr_t pawn)
{
    if (!g_dedicatedRotationOverride.active) return;
    const DedicatedRotationOverrideState saved = g_dedicatedRotationOverride;
    g_dedicatedRotationOverride = {};
    if (saved.pawn != pawn || !LooksLikePlayerPawn(pawn)) {
        Log("[hands-forearm] prior dedicated override discarded: pawn replaced");
        return;
    }
    bool restored = true;
    RestoreOneDedicatedRotationSide(saved.left, saved.flagsOffset, &restored);
    RestoreOneDedicatedRotationSide(saved.right, saved.flagsOffset, &restored);
    if (!restored) {
        g_dedicatedRotationWriteFault = true;
        Log("[hands-forearm] RESTORE FAILED - dedicated rotation path disabled");
    }
}

// One measured snapshot of what the skeleton actually composed, against what was commanded.
// SpaceBases is the PRECEDING update's composition, so the roll bone is compared with
// previousOrientation - the value written on that update - making "roll composed vs prior
// write" a direct test that world-space replacement semantics hold on this control. The
// hand/roll twists relative to the forearm are the numbers that decide the correct roll-helper
// construction; the game ctrl value shows the authored convention for the same tick.
static void ReportForearmBoneFrames(const DedicatedRotationRig& rig)
{
    const float toDeg = 180.0f / 32768.0f;
    for (int hand = 0; hand < 2; ++hand) {
        const ForearmDiagnostic& diag = g_forearmDiagnostic[hand];
        if (!diag.valid) continue;
        const int armBone = hand == 0 ? 17 : 46;
        const int rollBone = hand == 0 ? 41 : 71;
        MEVR_Vec3 forearmAxes[3], handAxes[3], rollAxes[3];
        if (!BoneWorldBasis(rig.spaceBaseData, rig.spaceBaseCount, rig.localToWorld,
                            armBone + 1, forearmAxes) ||
            !BoneWorldBasis(rig.spaceBaseData, rig.spaceBaseCount, rig.localToWorld,
                            armBone + 2, handAxes) ||
            !BoneWorldBasis(rig.spaceBaseData, rig.spaceBaseCount, rig.localToWorld,
                            rollBone, rollAxes)) {
            Log("[hands-bones] %s bone bases unreadable", hand == 0 ? "LEFT" : "RIGHT");
            continue;
        }
        float forearmRot[3], handRot[3], rollRot[3];
        UERotatorDegFromBasis(forearmAxes, forearmRot);
        UERotatorDegFromBasis(handAxes, handRot);
        UERotatorDegFromBasis(rollAxes, rollRot);
        const XrQuaternionf forearmQ =
            QuaternionFromUEBasis(forearmAxes[0], forearmAxes[1], forearmAxes[2]);
        const XrQuaternionf handQ =
            QuaternionFromUEBasis(handAxes[0], handAxes[1], handAxes[2]);
        const XrQuaternionf rollQ =
            QuaternionFromUEBasis(rollAxes[0], rollAxes[1], rollAxes[2]);
        float handTwist = 0.0f, handTotal = 0.0f, rollTwist = 0.0f, rollTotal = 0.0f;
        RelativeTwistDeg(forearmQ, handQ, &handTwist, &handTotal);
        RelativeTwistDeg(forearmQ, rollQ, &rollTwist, &rollTotal);
        Log("[hands-bones] %s composed P/Y/R deg: forearm(%+.1f,%+.1f,%+.1f)"
            " hand(%+.1f,%+.1f,%+.1f) roll(%+.1f,%+.1f,%+.1f) |"
            " written hand(%+.1f,%+.1f,%+.1f) roll(%+.1f,%+.1f,%+.1f)",
            hand == 0 ? "LEFT" : "RIGHT",
            forearmRot[0], forearmRot[1], forearmRot[2],
            handRot[0], handRot[1], handRot[2],
            rollRot[0], rollRot[1], rollRot[2],
            diag.wrist[0] * toDeg, diag.wrist[1] * toDeg, diag.wrist[2] * toDeg,
            diag.forearmRotator[0] * toDeg, diag.forearmRotator[1] * toDeg,
            diag.forearmRotator[2] * toDeg);
        Log("[hands-bones] %s relative deg: hand-in-forearm twist %+.1f of %.1f,"
            " roll-in-forearm twist %+.1f of %.1f |"
            " roll composed vs prior write %.1f | game ctrl (%+.1f,%+.1f,%+.1f) space %u",
            hand == 0 ? "LEFT" : "RIGHT",
            handTwist, handTotal, rollTwist, rollTotal,
            diag.havePreviousOrientation
                ? QuatAngleDeg(rollQ, diag.previousOrientation) : -1.0f,
            diag.gameForearmValid ? diag.gameForearm[0] * toDeg : -999.0f,
            diag.gameForearmValid ? diag.gameForearm[1] * toDeg : -999.0f,
            diag.gameForearmValid ? diag.gameForearm[2] * toDeg : -999.0f,
            diag.gameForearmValid ? (unsigned)diag.gameForearmSpace : 255u);
        uint32_t limbController = 0;
        const int limbOffset = hand == 0 ? g_offLeftHandWorldIK : g_offRightHandWorldIK;
        MEVR_Vec3 jointTarget{};
        uint8_t jointSpace = 0xFF;
        if (limbOffset >= 0 && g_offLimbJointTarget >= 0 && g_offLimbJointTargetSpace >= 0 &&
            SafeU32(rig.pawn + limbOffset, &limbController) && limbController >= 0x10000 &&
            SafeRead(limbController + g_offLimbJointTarget, &jointTarget,
                     sizeof(jointTarget)) &&
            SafeRead(limbController + g_offLimbJointTargetSpace, &jointSpace, 1)) {
            Log("[hands-bones] %s joint target (%+.1f,%+.1f,%+.1f) space %u",
                hand == 0 ? "LEFT" : "RIGHT",
                jointTarget.x, jointTarget.y, jointTarget.z, (unsigned)jointSpace);
        }
    }
}

static void ReportForearmDiagnostics(const DedicatedRotationRig& rig,
                                     const uint8_t movement[2], const float reach[2],
                                     const bool eligible[2])
{
    if (!g_motionHandsDebug) return;
    static long samples = 0, lastClose = -1000, lastJump = -1000;
    ++samples;
    const bool close = (eligible[0] && reach[0] > 0.0f && reach[0] < 25.0f) ||
                       (eligible[1] && reach[1] > 0.0f && reach[1] < 25.0f) ||
                       (g_armGeometryDiag[0].valid &&
                        g_armGeometryDiag[0].meshHandToTarget > 12.0f) ||
                       (g_armGeometryDiag[1].valid &&
                        g_armGeometryDiag[1].meshHandToTarget > 12.0f);
    const bool jumped = g_forearmDiagnostic[0].outputJump ||
                        g_forearmDiagnostic[1].outputJump;
    const bool report = (samples % 120) == 0 ||
                        (close && samples - lastClose >= 15) ||
                        (jumped && samples - lastJump >= 10);
    if (close && report) lastClose = samples;
    if (jumped && report) lastJump = samples;
    if (report) {
        const float toDeg = 180.0f / 32768.0f;
        for (int hand = 0; hand < 2; ++hand) {
            const ForearmDiagnostic& diag = g_forearmDiagnostic[hand];
            const ArmGeometryDiagnostic& geometry = g_armGeometryDiag[hand];
            const DetachedHandSolver& detach = hand == 0 ? g_leftDetach : g_rightDetach;
            Log("[hands-diag] %s mv=%u elig=%d reach=%.1f shoulder=%.1f meshErr=%.1f"
                " upper/lower=%.1f/%.1f det=%d excess=%.1f |"
                " wrist P%+.1f Y%+.1f R%+.1f forearm R%+.1f geomAxis=%d jump=%d",
                hand == 0 ? "LEFT" : "RIGHT", (unsigned)movement[hand],
                eligible[hand] ? 1 : 0, reach[hand],
                geometry.valid ? geometry.shoulderToTarget : -1.0f,
                geometry.valid ? geometry.meshHandToTarget : -1.0f,
                geometry.valid ? geometry.upperLength : -1.0f,
                geometry.valid ? geometry.lowerLength : -1.0f,
                detach.reachEngaged ? 1 : 0, detach.diagnosticExcess,
                diag.wrist[0] * toDeg, diag.wrist[1] * toDeg,
                diag.wrist[2] * toDeg, diag.forearmRotator[2] * toDeg,
                geometry.forearmAxisValid ? 1 : 0,
                diag.outputJump ? 1 : 0);
        }
        ReportForearmBoneFrames(rig);
    }
    g_forearmDiagnostic[0].outputJump = false;
    g_forearmDiagnostic[1].outputJump = false;
}

static bool ApplyDedicatedHandForearmRotations(uintptr_t pawn, const P13PoseSnapshot& pose)
{
    if (!g_motionHands || g_dedicatedRotationWriteFault) return false;
    DedicatedRotationRig rig{};
    if (!DiscoverDedicatedRotationRig(pawn, &rig)) {
        static bool reported = false;
        if (!reported) {
            Log("[hands-forearm] dedicated Hips/Swing controls did not validate;"
                " using stable borrowed-wrist fallback");
            reported = true;
        }
        return false;
    }
    if (g_dedicatedRotationReportedMesh != rig.mesh) {
        Log("*** [hands-forearm] dedicated rig validated: HipsControl -> LeftHand,"
            " SwingControl -> RightHand; original ForeArmRoll controls remain mapped;"
            " packed flags +0x%02X = 0x%08X", rig.flagsOffset,
            rig.rotationOnlyFlags);
        g_dedicatedRotationReportedMesh = rig.mesh;
    }

    const XrSpaceLocationFlags orientationNeed =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    uintptr_t ignored = 0;
    uint8_t leftMovement = 0xFF, rightMovement = 0xFF;
    float leftReach = 0.0f, rightReach = 0.0f;
    const P13BlockReason leftReason =
        P13Eligibility(pawn, pose.left, pose.presentFrame, true,
                       &ignored, &leftMovement, &leftReach);
    const P13BlockReason rightReason =
        P13Eligibility(pawn, pose.right, pose.presentFrame, false,
                       &ignored, &rightMovement, &rightReach);
    const bool leftOrientationTracked =
        (pose.left.flags & orientationNeed) == orientationNeed;
    const bool rightOrientationTracked =
        (pose.right.flags & orientationNeed) == orientationNeed;
    const bool leftEligible = leftReason == P13_READY && leftOrientationTracked;
    const bool rightEligible = rightReason == P13_READY && rightOrientationTracked;
    // The world-space helper write carries almost no per-frame state; only the jump
    // diagnostic's continuity sample and the twist unwrap reference must forget poses from
    // before an ownership gap.
    if (!leftEligible) {
        g_forearmDiagnostic[0].haveOrientation = false;
        g_forearmTwistUnwrapValid[0] = false;
    }
    if (!rightEligible) {
        g_forearmDiagnostic[1].haveOrientation = false;
        g_forearmTwistUnwrapValid[1] = false;
    }
    if (!leftEligible && !rightEligible) return true;

    DedicatedRotationOverrideState frame{};
    frame.active = true; frame.pawn = pawn; frame.flagsOffset = rig.flagsOffset;
    bool wrote = true;
    if (leftEligible)
        wrote = ApplyOneDedicatedRotationSide(rig, rig.left, pose.left, true,
                                              &frame.left);
    if (rightEligible && wrote)
        wrote = ApplyOneDedicatedRotationSide(rig, rig.right, pose.right, false,
                                              &frame.right);
    const uint8_t movement[2] = { leftMovement, rightMovement };
    const float reach[2] = { leftReach, rightReach };
    const bool eligible[2] = { leftEligible, rightEligible };
    ReportForearmDiagnostics(rig, movement, reach, eligible);
    g_dedicatedRotationOverride = frame;
    if (!wrote) {
        RestoreDedicatedHandForearmOverridesBeforeGame(pawn);
        g_dedicatedRotationWriteFault = true;
        Log("[hands-forearm] dedicated WRITE/READ-BACK FAILED; using borrowed-wrist fallback");
        return false;
    }
    if (leftEligible) ++g_dedicatedLeftWrites;
    if (rightEligible) ++g_dedicatedRightWrites;
    if ((leftEligible && g_dedicatedLeftWrites == 1) ||
        (rightEligible && g_dedicatedRightWrites == 1)) {
        Log("*** [hands-forearm] dedicated hand + visible forearm rotation acquired:"
            " L=%d R=%d; exact topology/value read-back passed",
            leftEligible ? 1 : 0, rightEligible ? 1 : 0);
    }
    return true;
}

static void ApplyWristRotations(uintptr_t pawn, const P13PoseSnapshot& pose)
{
    // Which rotation topology ran this arm update. Before this log existed, a transient
    // discovery failure silently swapped dedicated -> borrowed for exactly one frame; that
    // swap re-routes the Hand chains and unmaps the roll bones, so even one frame of it is a
    // visible arm snap that no other line records.
    static int reportedPath = -1;
    const int path = ApplyDedicatedHandForearmRotations(pawn, pose) ? 1 : 0;
    if (path == 0) ApplyBorrowedWristRotations(pawn, pose);
    if (path != reportedPath) {
        if (path == 1)
            Log("*** [hands-path] rotation topology -> dedicated Hips/Swing donors"
                " + world-space ForeArmRoll");
        else
            Log("*** [hands-path] rotation topology -> borrowed-wrist fallback"
                " (roll bones unmapped); dedicated failed at: %s",
                g_dedicatedRotationFailure);
        reportedPath = path;
    }
}

// Per-update discontinuity watchdog for the "arm spasms while the controller is tracked"
// report. The 2-second [hands-diag] cadence cannot catch a one-update flap of ownership,
// rotation writes, target, written hand orientation, shoulder offset, or the camera anchor.
// Log the exact update on which any of them steps, with every candidate's value, so the
// guilty quantity is identified by measurement instead of hypothesis.
static void MonitorArmContinuity(uintptr_t pawn, const P13PoseSnapshot& pose)
{
    if (!g_motionHandsDebug) return;
    struct HandContinuity {
        bool haveTarget = false;
        MEVR_Vec3 targetFromPawn{};
        bool haveWorldTarget = false;
        MEVR_Vec3 worldTarget{};
        bool owned = false;
        bool rotationActive = false;
        bool haveHand = false;
        XrQuaternionf hand{};
        bool haveRaw = false;
        XrQuaternionf raw{};
        bool haveForearm = false;
        XrQuaternionf forearm{};
        bool haveRoll = false;
        XrQuaternionf roll{};
        bool haveJoint = false;
        MEVR_Vec3 joint{};
        MEVR_Vec3 shoulderOffset{};
    };
    static HandContinuity last[2]{};
    static bool haveHeadBasis = false;
    static MEVR_Vec3 lastHeadForward{}, lastHeadRight{};
    static long updates = 0, lastLogged = -1000;
    ++updates;

    // Track forward AND right axes: run 7 proved a forward-only check is blind to a pure
    // camera roll step, which rotates every hand orientation while the targets barely move.
    float headStepDeg = -1.0f, headRollStepDeg = -1.0f;
    RenderedHeadFrame head{};
    if (GetRenderedHeadFrame(&head)) {
        if (haveHeadBasis) {
            auto axisStep = [](const MEVR_Vec3& a, const MEVR_Vec3& b) {
                float dot = a.x*b.x + a.y*b.y + a.z*b.z;
                if (dot > 1.0f) dot = 1.0f;
                if (dot < -1.0f) dot = -1.0f;
                return acosf(dot) * 57.2957795f;
            };
            headStepDeg = axisStep(head.forward, lastHeadForward);
            headRollStepDeg = axisStep(head.right, lastHeadRight);
        }
        lastHeadForward = head.forward;
        lastHeadRight = head.right;
        haveHeadBasis = true;
    }
    // The PHYSICAL head's per-update step, for the rotation-ghosting report: with stick look
    // the game camera and the camera-anchored hands rotate together, but with physical rotation
    // the world follows the HMD smoothly while the anchor advances by injected yaw writes. If
    // the anchor step above is lumpy while this HMD step is smooth, the hands snap against a
    // smooth world - which is exactly what run 18 captured once (anchor 14.4 deg in one update).
    static bool haveHmd = false;
    static XrQuaternionf lastHmd{};
    float hmdStepDeg = -1.0f;
    if (g_viewsValid) {
        const XrQuaternionf hmd = g_views[0].pose.orientation;
        if (haveHmd) hmdStepDeg = QuatAngleDeg(hmd, lastHmd);
        lastHmd = hmd;
        haveHmd = true;
    } else {
        haveHmd = false;
    }
    const bool stickLook = MotionTurnInputActive();

    MEVR_Vec3 pawnLocation{};
    const bool havePawn = g_offActorLocation >= 0 &&
        SafeRead(pawn + g_offActorLocation, &pawnLocation, sizeof(pawnLocation)) &&
        FiniteVec(pawnLocation);

    // The tgt column is pawn-relative and therefore ambiguous: it trips when the pawn moves
    // (ordinary locomotion) exactly as it does when the world target jumps. These two columns
    // split it - the pawn's own step and each hand's WORLD-target step - so a judder window
    // names who moved without interpretation.
    static bool havePreviousPawnStep = false;
    static MEVR_Vec3 previousPawnLocation{};
    float pawnStep = -1.0f;
    if (havePawn) {
        if (havePreviousPawnStep)
            pawnStep = VecLength({ pawnLocation.x - previousPawnLocation.x,
                                   pawnLocation.y - previousPawnLocation.y,
                                   pawnLocation.z - previousPawnLocation.z });
        previousPawnLocation = pawnLocation;
        havePreviousPawnStep = true;
    } else {
        havePreviousPawnStep = false;
    }

    char trips[128] = "";
    auto trip = [&](const char* tag) {
        strcat_s(trips, sizeof(trips), " ");
        strcat_s(trips, sizeof(trips), tag);
    };
    HandContinuity now[2]{};
    bool ownedWas[2]{}, rotationWas[2]{};
    float targetStep[2] = { -1.0f, -1.0f };
    float worldTargetStep[2] = { -1.0f, -1.0f };
    float chainDistance[2] = { -1.0f, -1.0f };
    float handStep[2] = { -1.0f, -1.0f };
    float rawStep[2] = { -1.0f, -1.0f };
    float forearmStep[2] = { -1.0f, -1.0f };
    float rollStep[2] = { -1.0f, -1.0f };
    float jointStep[2] = { -1.0f, -1.0f };
    float shoulderStep[2] = { -1.0f, -1.0f };
    for (int hand = 0; hand < 2; ++hand) {
        const P13HandPoseSnapshot& handPose = hand == 0 ? pose.left : pose.right;
        const P13HandState& state = hand == 0 ? g_p13LeftState : g_p13RightState;
        const DetachedHandSolver& detach = hand == 0 ? g_leftDetach : g_rightDetach;
        const ForearmDiagnostic& diag = g_forearmDiagnostic[hand];
        HandContinuity& cur = now[hand];
        cur.owned = state.owned;
        cur.rotationActive = hand == 0 ? g_dedicatedRotationOverride.left.active
                                       : g_dedicatedRotationOverride.right.active;
        cur.shoulderOffset = detach.filtered;
        if (havePawn && handPose.worldValid) {
            cur.haveTarget = true;
            cur.targetFromPawn = { handPose.worldPosition.x - pawnLocation.x,
                                   handPose.worldPosition.y - pawnLocation.y,
                                   handPose.worldPosition.z - pawnLocation.z };
        }
        if (handPose.worldValid) {
            cur.haveWorldTarget = true;
            cur.worldTarget = handPose.worldPosition;
        }
        // The raw view-space grip orientation, straight from the runtime. When the written hand
        // steps but this does too, the step arrived in tracking; when this is quiet and the
        // written hand steps, our conversion made it. Run 18's close-to-face snapping needs
        // exactly that discrimination.
        if (handPose.active) {
            XrQuaternionf raw{};
            if (NormalizedQuaternion(handPose.viewOrientation, &raw)) {
                cur.haveRaw = true;
                cur.raw = raw;
            }
        }
        if (cur.rotationActive) {
            cur.haveHand = true;
            cur.hand = diag.handOrientation;
            // The IK-driven forearm bone and the written roll target: run 8's right-arm spasm
            // showed nothing in target/hand/shoulder/head, so these are the remaining movers.
            if (diag.haveForearmBone) {
                cur.haveForearm = true;
                cur.forearm = diag.forearmBoneOrientation;
            }
            if (diag.haveOrientation) {
                cur.haveRoll = true;
                cur.roll = diag.lastOrientation;
            }
        }
        // The joint target steers SkelControlLimb's bend plane; it is the one solve input this
        // mod never writes. Run 10 showed the forearm bone flip-flopping 12-22 deg per update
        // against a still hand while jumping - if the game re-aims this vector per tick from
        // its own jump animation, the step will show here in lockstep with the farm step.
        uint32_t limbController = 0;
        const int limbOffset = hand == 0 ? g_offLeftHandWorldIK : g_offRightHandWorldIK;
        if (limbOffset >= 0 && g_offLimbJointTarget >= 0 &&
            SafeU32(pawn + limbOffset, &limbController) && limbController >= 0x10000 &&
            SafeRead(limbController + g_offLimbJointTarget, &cur.joint, sizeof(cur.joint)) &&
            FiniteVec(cur.joint))
            cur.haveJoint = true;

        const HandContinuity& prev = last[hand];
        ownedWas[hand] = prev.owned;
        rotationWas[hand] = prev.rotationActive;
        const bool left = hand == 0;
        if (cur.owned != prev.owned) trip(left ? "L-own" : "R-own");
        // Ownership held but the rotation writes skipped or resumed: exactly the silent flap.
        if (cur.owned == prev.owned && cur.rotationActive != prev.rotationActive)
            trip(left ? "L-rot" : "R-rot");
        if (prev.haveTarget && cur.haveTarget) {
            targetStep[hand] = VecLength({
                cur.targetFromPawn.x - prev.targetFromPawn.x,
                cur.targetFromPawn.y - prev.targetFromPawn.y,
                cur.targetFromPawn.z - prev.targetFromPawn.z });
            if (targetStep[hand] > 10.0f) trip(left ? "L-tgt" : "R-tgt");
        }
        if (prev.haveWorldTarget && cur.haveWorldTarget) {
            worldTargetStep[hand] = VecLength({
                cur.worldTarget.x - prev.worldTarget.x,
                cur.worldTarget.y - prev.worldTarget.y,
                cur.worldTarget.z - prev.worldTarget.z });
            if (worldTargetStep[hand] > 10.0f) trip(left ? "L-wtgt" : "R-wtgt");
        }
        // Distance from the (one-tick-stale) chain root to the raw world target: the clamp
        // holds the solver at 47 UU from THIS estimate, so values at ~50+ here mean the true
        // distance can still cross the 50.38 singular zone - the suspected residual of run 21's
        // at-rest forearm flip.
        if (cur.haveWorldTarget && g_armChainRootValid[hand])
            chainDistance[hand] = VecLength({
                cur.worldTarget.x - g_armChainRoot[hand].x,
                cur.worldTarget.y - g_armChainRoot[hand].y,
                cur.worldTarget.z - g_armChainRoot[hand].z });
        if (prev.haveHand && cur.haveHand) {
            handStep[hand] = QuatAngleDeg(prev.hand, cur.hand);
            if (handStep[hand] > 15.0f) trip(left ? "L-hand" : "R-hand");
        }
        if (prev.haveRaw && cur.haveRaw) {
            rawStep[hand] = QuatAngleDeg(prev.raw, cur.raw);
            if (rawStep[hand] > 15.0f) trip(left ? "L-raw" : "R-raw");
        }
        if (prev.haveForearm && cur.haveForearm) {
            forearmStep[hand] = QuatAngleDeg(prev.forearm, cur.forearm);
            if (forearmStep[hand] > 15.0f) trip(left ? "L-farm" : "R-farm");
        }
        if (prev.haveRoll && cur.haveRoll) {
            rollStep[hand] = QuatAngleDeg(prev.roll, cur.roll);
            if (rollStep[hand] > 15.0f) trip(left ? "L-roll" : "R-roll");
        }
        if (prev.haveJoint && cur.haveJoint) {
            jointStep[hand] = VecLength({ cur.joint.x - prev.joint.x,
                                          cur.joint.y - prev.joint.y,
                                          cur.joint.z - prev.joint.z });
            if (jointStep[hand] > 6.0f) trip(left ? "L-joint" : "R-joint");
        }
        shoulderStep[hand] = VecLength({
            cur.shoulderOffset.x - prev.shoulderOffset.x,
            cur.shoulderOffset.y - prev.shoulderOffset.y,
            cur.shoulderOffset.z - prev.shoulderOffset.z });
        if (shoulderStep[hand] > 3.0f) trip(left ? "L-sh" : "R-sh");
    }
    if (headStepDeg > 8.0f) trip("head");
    if (headRollStepDeg > 8.0f) trip("head-roll");

    last[0] = now[0];
    last[1] = now[1];
    // A user marker overrides both the trip requirement and the rate limit: the player said
    // "now", so every update in the window is worth a line.
    const bool marked = InterlockedCompareExchange(&g_markerBurst, 0, 0) > 0;
    if (marked) {
        InterlockedDecrement(&g_markerBurst);
        trip("MARK");
    }
    if (!trips[0] || (!marked && updates - lastLogged < 15)) return;
    lastLogged = updates;
    Log("*** [hands-spazz] upd %ld head f/r %.1f/%.1f hmd %.1f deg age %ld stick %d"
        " pawn %.1f |"
        " L own %d>%d rot %d>%d tgt %.1f wtgt %.1f cd %.1f raw %.1f hand %.1f farm %.1f"
        " roll %.1f jt %.1f sh %.2f |"
        " R own %d>%d rot %d>%d tgt %.1f wtgt %.1f cd %.1f raw %.1f hand %.1f farm %.1f"
        " roll %.1f jt %.1f sh %.2f | trip:%s",
        updates, headStepDeg, headRollStepDeg, hmdStepDeg, g_frames - pose.presentFrame,
        stickLook ? 1 : 0, pawnStep,
        ownedWas[0] ? 1 : 0, now[0].owned ? 1 : 0,
        rotationWas[0] ? 1 : 0, now[0].rotationActive ? 1 : 0,
        targetStep[0], worldTargetStep[0], chainDistance[0], rawStep[0], handStep[0],
        forearmStep[0], rollStep[0], jointStep[0], shoulderStep[0],
        ownedWas[1] ? 1 : 0, now[1].owned ? 1 : 0,
        rotationWas[1] ? 1 : 0, now[1].rotationActive ? 1 : 0,
        targetStep[1], worldTargetStep[1], chainDistance[1], rawStep[1], handStep[1],
        forearmStep[1], rollStep[1], jointStep[1], shoulderStep[1],
        trips);
}

#if 0
// Retired experiment: borrowing the live Aim1p chains for forearm twist changed their topology
// underneath the game and preceded two crashes. Keep the implementation out of the binary while
// its replacement is designed around a controller the game does not mutate.
// ========================================================== P1.4b visible forearm rotation
//
// The controls borrowed above are the authored ForeArmRoll controls, which leaves the model's
// dedicated forearm-twist helper bones idle while the wrists are tracked. Reuse the bilateral
// unarmed Aim1p single-bone controls on those helper bones. Unlike LeftForeArm/RightForeArm,
// these helpers do not parent the hands, so their rotation changes only the skinned forearm and
// cannot move the IK end effector away from the controller. The Aim1p controls and both maps are
// restored before the game runs, preserving the same parkour/weapon handoff as the wrist layer.
struct ForearmSideFrame {
    bool active = false;
    uintptr_t control = 0;
    uint32_t aimMapAddress = 0, rollMapAddress = 0;
    uint8_t originalAimMap = 0xFF, originalRollMap = 0xFF;
    uint32_t originalNext = 0;
    DetachedControlSave saved{};
};
struct ForearmOverrideState {
    bool active = false;
    uintptr_t pawn = 0;
    ForearmSideFrame left{}, right{};
};
struct ForearmRuntimeSide {
    uintptr_t control = 0;
    uint32_t aimMapAddress = 0, rollMapAddress = 0;
    uint8_t originalAimMap = 0xFF, originalRollMap = 0xFF, unmappedValue = 0xFF;
    uint32_t originalNext = 0;
};

static ForearmOverrideState g_forearmOverride{};
static bool g_forearmWriteFault = false;
static uintptr_t g_forearmReportedMesh = 0;
static long g_forearmLeftWrites = 0, g_forearmRightWrites = 0;

static bool DiscoverForearmSide(const DetachedRigFrame& rig, bool left,
                                ForearmRuntimeSide* out)
{
    if (!out) return false;
    const UE3Array32 lists{ rig.listData, rig.listCount, rig.listCount };
    const char* aimBone = left ? "SpineXLeft" : "SpineXRight";
    const int aimBoneIndex = left ? 15 : 44;
    const int rollBoneIndex = left ? 41 : 71;
    UE3ControlListHead aim{};
    const int aimSlot = FindControlListSlot(lists, aimBone, &aim);
    if (aimSlot < 0 || !LooksLikeRigObject(aim.controlHead, "TdSkelControlAim1p"))
        return false;

    uint8_t aimMap = 0xFF, rollMap = 0xFF;
    uint32_t next = 0;
    const uint8_t unmapped = rig.mapBias == 1 ? 0 : 0xFF;
    if (!ReadMappedControl({ rig.indexData, rig.indexCount, rig.indexCount },
                           aimBoneIndex, &aimMap) ||
        !ReadMappedControl({ rig.indexData, rig.indexCount, rig.indexCount },
                           rollBoneIndex, &rollMap) ||
        aimMap != (uint8_t)(aimSlot + rig.mapBias) || rollMap != unmapped ||
        !SafeU32(aim.controlHead + g_offSkelNextControl, &next) ||
        !LooksLikeLiveUObject(next)) return false;

    ForearmRuntimeSide side{};
    side.control = aim.controlHead;
    side.aimMapAddress = rig.indexData + aimBoneIndex;
    side.rollMapAddress = rig.indexData + rollBoneIndex;
    side.originalAimMap = aimMap;
    side.originalRollMap = rollMap;
    side.unmappedValue = unmapped;
    side.originalNext = next;
    *out = side;
    return true;
}

static bool ApplyOneForearmSide(const ForearmRuntimeSide& runtime,
                                const P13HandPoseSnapshot& pose,
                                bool leftHand, ForearmSideFrame* frame)
{
    if (!frame) return false;
    ForearmSideFrame saved{};
    saved.active = CaptureDetachedControl(runtime.control, &saved.saved);
    saved.control = runtime.control;
    saved.aimMapAddress = runtime.aimMapAddress;
    saved.rollMapAddress = runtime.rollMapAddress;
    saved.originalAimMap = runtime.originalAimMap;
    saved.originalRollMap = runtime.originalRollMap;
    saved.originalNext = runtime.originalNext;
    if (!saved.active) return false;

    // Aim1p is authored as an additive single-bone rotation control. Feed it only UE roll in
    // bone space: pitch/yaw still come from the limb IK, while turning a controller as if holding
    // a hammer pronates/supinates the visible forearm around its own +X axis.
    int32_t handRotation[3]{};
    if (!GripQuaternionToUERotator(pose.worldOrientation, leftHand,
                                   g_wristCalibrationDeg, handRotation)) return false;
    const int hand = leftHand ? 0 : 1;
    int32_t forearmRotation[3] = { 0, 0, handRotation[2] };
    forearmRotation[2] += (int32_t)lroundf(
        g_forearmRollCalibrationDeg[hand] * (32768.0f / 180.0f));
    const uint8_t boneSpace = 4;
    const float one = 1.0f, zero = 0.0f;
    const uint32_t noNext = 0;
    const uint8_t forearmMap = (uint8_t)(runtime.originalAimMap);
    const bool wrote =
        WriteRigBytes(runtime.aimMapAddress, &runtime.unmappedValue, 1) &&
        WriteRigBytes(runtime.rollMapAddress, &forearmMap, 1) &&
        WriteRigBytes(runtime.control + g_offSkelNextControl, &noNext, sizeof(noNext)) &&
        WriteRigBytes(runtime.control + g_offSingleBoneRotation,
                      forearmRotation, sizeof(forearmRotation)) &&
        WriteRigBytes(runtime.control + g_offSingleBoneRotationSpace, &boneSpace, 1) &&
        WriteRigBytes(runtime.control + g_offSkelStrengthTarget, &one, sizeof(one)) &&
        WriteRigBytes(runtime.control + g_offSkelBlendTimeToGo, &zero, sizeof(zero)) &&
        WriteRigBytes(runtime.control + g_offSkelControlStrength, &one, sizeof(one));

    uint8_t readAim = 0xFF, readRoll = 0xFF;
    uint32_t readNext = ~0u;
    int32_t readRotation[3]{};
    uint8_t readSpace = 0xFF;
    if (!wrote || !SafeRead(runtime.aimMapAddress, &readAim, 1) ||
        !SafeRead(runtime.rollMapAddress, &readRoll, 1) ||
        !SafeU32(runtime.control + g_offSkelNextControl, &readNext) ||
        !SafeRead(runtime.control + g_offSingleBoneRotation,
                  readRotation, sizeof(readRotation)) ||
        !SafeRead(runtime.control + g_offSingleBoneRotationSpace, &readSpace, 1) ||
        readAim != runtime.unmappedValue || readRoll != forearmMap || readNext != 0 ||
        readRotation[0] != 0 || readRotation[1] != 0 ||
        readRotation[2] != forearmRotation[2] || readSpace != boneSpace) {
        *frame = saved;
        return false;
    }
    *frame = saved;
    return true;
}

static void RestoreOneForearmSide(const ForearmSideFrame& side, bool* restored)
{
    if (!side.active || !restored) return;
    *restored = RestoreDetachedControl(side.control, side.saved) && *restored;
    *restored = WriteRigBytes(side.control + g_offSkelNextControl,
                              &side.originalNext, sizeof(side.originalNext)) && *restored;
    *restored = WriteRigBytes(side.rollMapAddress,
                              &side.originalRollMap, 1) && *restored;
    *restored = WriteRigBytes(side.aimMapAddress,
                              &side.originalAimMap, 1) && *restored;
}

static void RestoreForearmRotationOverridesBeforeGame(uintptr_t pawn)
{
    if (!g_forearmOverride.active) return;
    const ForearmOverrideState saved = g_forearmOverride;
    g_forearmOverride = {};
    if (saved.pawn != pawn || !LooksLikePlayerPawn(pawn)) {
        Log("[hands-forearm] prior override discarded without writes: pawn replaced");
        return;
    }
    bool restored = true;
    RestoreOneForearmSide(saved.left, &restored);
    RestoreOneForearmSide(saved.right, &restored);
    if (!restored) {
        g_forearmWriteFault = true;
        Log("[hands-forearm] RESTORE FAILED - forearm rotation disabled for this process");
    }
}

static void ApplyForearmRotations(uintptr_t pawn, const P13PoseSnapshot& pose)
{
    if (!g_motionHands || g_forearmWriteFault || !g_wristOverride.active) return;
    const bool leftEligible = g_wristOverride.left.active;
    const bool rightEligible = g_wristOverride.right.active;
    if (!leftEligible && !rightEligible) return;

    DetachedRigFrame rig{};
    ForearmRuntimeSide leftRuntime{}, rightRuntime{};
    if (!DiscoverDetachedRig(pawn, &rig) ||
        (leftEligible && !DiscoverForearmSide(rig, true, &leftRuntime)) ||
        (rightEligible && !DiscoverForearmSide(rig, false, &rightRuntime))) {
        static bool reported = false;
        if (!reported) {
            Log("[hands-forearm] waiting: bilateral Aim1p controls or roll-bone maps"
                " did not validate");
            reported = true;
        }
        return;
    }
    if (g_forearmReportedMesh != rig.mesh) {
        Log("*** [hands-forearm] visible roll-bone rig validated: Aim1p controls"
            " temporarily mapped to LeftForeArmRoll 41 / RightForeArmRoll 71");
        g_forearmReportedMesh = rig.mesh;
    }

    ForearmOverrideState frame{};
    frame.active = true;
    frame.pawn = pawn;
    bool wrote = true;
    if (leftEligible)
        wrote = ApplyOneForearmSide(leftRuntime, pose.left, true, &frame.left);
    if (rightEligible && wrote)
        wrote = ApplyOneForearmSide(rightRuntime, pose.right, false, &frame.right);
    g_forearmOverride = frame;
    if (!wrote) {
        RestoreForearmRotationOverridesBeforeGame(pawn);
        g_forearmWriteFault = true;
        Log("[hands-forearm] WRITE/READ-BACK FAILED - forearm layer disabled;");
        Log("[hands-forearm] wrist rotation, position IK, and detachment remain active");
        return;
    }

    if (leftEligible) ++g_forearmLeftWrites;
    if (rightEligible) ++g_forearmRightWrites;
    if ((leftEligible && g_forearmLeftWrites == 1) ||
        (rightEligible && g_forearmRightWrites == 1)) {
        Log("*** [hands-forearm] controller-driven visible forearm rotation acquired:"
            " L=%d R=%d; exact topology/flag read-back passed",
            leftEligible ? 1 : 0, rightEligible ? 1 : 0);
    }
}
#endif

#if 0
// ===================================================== P1.4c rejected lower-arm experiment
//
// Aim1p was the wrong donor: the game mutates that native chain during play. This version uses
// only the two plain, fixed-topology SkelControlSingleBone objects authored on root and Hips.
// While VR owns a hand, HipsControl is mapped to LeftForeArmRoll and SwingControl to
// RightForeArmRoll. Their original maps, links, flags, and controller bytes are restored before
// Update1pArms. The real ForeArmRoll controls remain borrowed by the Hand chains for the already
// validated wrist solution.
struct ForearmTwistSideSave {
    bool active = false;
    bool restoreNext = false;
    uintptr_t control = 0;
    uint32_t sourceMapAddress = 0, rollMapAddress = 0;
    uint8_t originalSourceMap = 0xFF, originalRollMap = 0xFF;
    uint32_t originalNext = 0, originalFlags = 0;
    DetachedControlSave controlSaved{};
};

struct ForearmTwistOverrideState {
    bool active = false;
    uintptr_t pawn = 0;
    ForearmTwistSideSave left{}, right{};
};

struct ForearmTwistRig {
    uintptr_t pawn = 0, mesh = 0;
    uintptr_t hipsControl = 0, swingControl = 0;
    uint32_t leftSourceMapAddress = 0, rightSourceMapAddress = 0;
    uint32_t leftRollMapAddress = 0, rightRollMapAddress = 0;
    uint8_t leftSourceMap = 0xFF, rightSourceMap = 0xFF;
    uint8_t leftRollMap = 0xFF, rightRollMap = 0xFF, unmappedValue = 0xFF;
    uint32_t swingNext = 0;
};

static ForearmTwistOverrideState g_forearmTwistOverride{};
static bool g_forearmTwistWriteFault = false;
static uintptr_t g_forearmTwistReportedMesh = 0;
static long g_forearmTwistLeftWrites = 0, g_forearmTwistRightWrites = 0;

static bool IsExactRigClass(uintptr_t object, const char* className)
{
    char actual[96] = "?";
    return LooksLikeLiveUObject(object) && ReadClassName(object, actual, sizeof(actual)) &&
           strcmp(actual, className) == 0;
}

static bool DiscoverForearmTwistRig(uintptr_t pawn, bool needLeft, bool needRight,
                                    ForearmTwistRig* out)
{
    if (!out || g_forearmTwistWriteFault ||
        g_offSingleBoneRotationFlags < 0 || !g_maskSingleBoneApplyRotation ||
        !g_maskSingleBoneAddRotation || !LooksLikePlayerPawn(pawn)) return false;

    uint32_t mesh = 0, tree = 0;
    if (!SafeU32(pawn + g_offMesh1p, &mesh) ||
        !LooksLikeRigObject(mesh, "TdSkeletalMeshComponent") ||
        !SafeU32(mesh + g_offMeshAnimations, &tree) ||
        !LooksLikeRigObject(tree, "AnimTree")) return false;
    UE3Array32 lists{}, indices{};
    if (!ReadUE3Array(tree, g_offAnimTreeSkelControlLists, 128, &lists) ||
        !ReadUE3Array(mesh, g_offMeshSkelControlIndex, 512, &indices) ||
        indices.count <= 71) return false;

    UE3ControlListHead root{}, hips{};
    const int rootSlot = FindControlListSlot(lists, "root", &root);
    const int hipsSlot = FindControlListSlot(lists, "Hips", &hips);
    if (rootSlot < 0 || hipsSlot < 0 || rootSlot == hipsSlot ||
        !IsExactRigClass(root.controlHead, "SkelControlSingleBone") ||
        !IsExactRigClass(hips.controlHead, "SkelControlSingleBone") ||
        root.controlHead == hips.controlHead) return false;

    uint32_t hipsNext = ~0u, swingNext = 0;
    uint8_t rootMap = 0xFF, hipsMap = 0xFF, leftRollMap = 0xFF, rightRollMap = 0xFF;
    if (!SafeU32(hips.controlHead + g_offSkelNextControl, &hipsNext) || hipsNext != 0 ||
        !SafeU32(root.controlHead + g_offSkelNextControl, &swingNext) ||
        !ReadMappedControl(indices, 0, &rootMap) ||
        !ReadMappedControl(indices, 1, &hipsMap) ||
        !ReadMappedControl(indices, 41, &leftRollMap) ||
        !ReadMappedControl(indices, 71, &rightRollMap)) return false;

    int bias = -1;
    for (int candidate = 0; candidate <= 1; ++candidate) {
        if (rootMap == rootSlot + candidate && hipsMap == hipsSlot + candidate) {
            bias = candidate;
            break;
        }
    }
    if (bias < 0) return false;
    const uint8_t unmapped = bias == 1 ? 0 : 0xFF;
    if ((needLeft && leftRollMap != unmapped) ||
        (needRight && rightRollMap != unmapped)) return false;

    // Left shoulder detachment deliberately cuts SwingControl -> RootControl for this frame.
    // Otherwise the authored second node must still be the exact dormant generic control.
    if (g_detachedOverride.leftTopology) {
        if (g_detachedOverride.pawn != pawn || g_detachedOverride.swing != root.controlHead ||
            swingNext != 0) return false;
    } else {
        uint32_t rootTail = ~0u;
        if (!IsExactRigClass(swingNext, "SkelControlSingleBone") ||
            !SafeU32(swingNext + g_offSkelNextControl, &rootTail) || rootTail != 0)
            return false;
    }

    ForearmTwistRig rig{};
    rig.pawn = pawn; rig.mesh = mesh;
    rig.hipsControl = hips.controlHead; rig.swingControl = root.controlHead;
    rig.leftSourceMapAddress = indices.data + 1;
    rig.rightSourceMapAddress = indices.data;
    rig.leftRollMapAddress = indices.data + 41;
    rig.rightRollMapAddress = indices.data + 71;
    rig.leftSourceMap = hipsMap; rig.rightSourceMap = rootMap;
    rig.leftRollMap = leftRollMap; rig.rightRollMap = rightRollMap;
    rig.unmappedValue = unmapped; rig.swingNext = swingNext;
    *out = rig;
    return true;
}

static bool WriteForearmTwistControl(uintptr_t control,
                                     const P13HandPoseSnapshot& pose,
                                     bool leftHand, uint32_t originalFlags)
{
    int32_t handRotation[3]{};
    if (!GripQuaternionToUERotator(pose.worldOrientation, leftHand,
                                   g_wristCalibrationDeg, handRotation)) return false;
    const int hand = leftHand ? 0 : 1;
    const int32_t offset = (int32_t)lroundf(
        g_forearmRollCalibrationDeg[hand] * (32768.0f / 180.0f));
    const int32_t rotation[3] = { 0, 0, handRotation[2] + offset };
    const MEVR_Vec3 noTranslation{};
    const uint8_t boneSpace = 4;
    const float one = 1.0f, zero = 0.0f;
    uint32_t flags = originalFlags | g_maskSingleBoneApplyRotation;
    flags &= ~g_maskSingleBoneAddRotation;
    if (!WriteRigBytes(control + g_offSingleBoneTranslation,
                       &noTranslation, sizeof(noTranslation)) ||
        !WriteRigBytes(control + g_offSingleBoneRotation, rotation, sizeof(rotation)) ||
        !WriteRigBytes(control + g_offSingleBoneRotationSpace, &boneSpace, 1) ||
        !WriteRigBytes(control + g_offSingleBoneRotationFlags, &flags, sizeof(flags)) ||
        !WriteRigBytes(control + g_offSkelStrengthTarget, &one, sizeof(one)) ||
        !WriteRigBytes(control + g_offSkelBlendTimeToGo, &zero, sizeof(zero)) ||
        !WriteRigBytes(control + g_offSkelControlStrength, &one, sizeof(one))) return false;

    int32_t readRotation[3]{};
    MEVR_Vec3 readTranslation{};
    uint8_t readSpace = 0xFF;
    uint32_t readFlags = 0;
    return SafeRead(control + g_offSingleBoneTranslation,
                    &readTranslation, sizeof(readTranslation)) &&
           SafeRead(control + g_offSingleBoneRotation,
                    readRotation, sizeof(readRotation)) &&
           SafeRead(control + g_offSingleBoneRotationSpace, &readSpace, 1) &&
           SafeU32(control + g_offSingleBoneRotationFlags, &readFlags) &&
           VecLength(readTranslation) < 0.001f && readRotation[0] == 0 &&
           readRotation[1] == 0 && readRotation[2] == rotation[2] && readSpace == boneSpace &&
           (readFlags & g_maskSingleBoneApplyRotation) != 0 &&
           (readFlags & g_maskSingleBoneAddRotation) == 0;
}

static bool ApplyOneForearmTwist(const ForearmTwistRig& rig,
                                 const P13HandPoseSnapshot& pose,
                                 bool leftHand, ForearmTwistSideSave* out)
{
    if (!out) return false;
    const uintptr_t control = leftHand ? rig.hipsControl : rig.swingControl;
    ForearmTwistSideSave save{};
    save.active = CaptureDetachedControl(control, &save.controlSaved);
    save.restoreNext = !leftHand;
    save.control = control;
    save.sourceMapAddress = leftHand ? rig.leftSourceMapAddress : rig.rightSourceMapAddress;
    save.rollMapAddress = leftHand ? rig.leftRollMapAddress : rig.rightRollMapAddress;
    save.originalSourceMap = leftHand ? rig.leftSourceMap : rig.rightSourceMap;
    save.originalRollMap = leftHand ? rig.leftRollMap : rig.rightRollMap;
    save.originalNext = leftHand ? 0 : rig.swingNext;
    if (!save.active || !SafeU32(control + g_offSingleBoneRotationFlags,
                                  &save.originalFlags)) return false;

    const uint32_t noNext = 0;
    const bool wrote =
        WriteRigBytes(save.sourceMapAddress, &rig.unmappedValue, 1) &&
        WriteRigBytes(save.rollMapAddress, &save.originalSourceMap, 1) &&
        (!save.restoreNext || WriteRigBytes(control + g_offSkelNextControl,
                                             &noNext, sizeof(noNext))) &&
        WriteForearmTwistControl(control, pose, leftHand, save.originalFlags);
    uint8_t readSource = 0xFF, readRoll = 0xFF;
    uint32_t readNext = ~0u;
    if (!wrote || !SafeRead(save.sourceMapAddress, &readSource, 1) ||
        !SafeRead(save.rollMapAddress, &readRoll, 1) ||
        (save.restoreNext && !SafeU32(control + g_offSkelNextControl, &readNext)) ||
        readSource != rig.unmappedValue || readRoll != save.originalSourceMap ||
        (save.restoreNext && readNext != 0)) {
        *out = save;
        return false;
    }
    *out = save;
    return true;
}

static void RestoreOneForearmTwist(const ForearmTwistSideSave& side, bool* restored)
{
    if (!side.active || !restored) return;
    *restored = RestoreDetachedControl(side.control, side.controlSaved) && *restored;
    *restored = WriteRigBytes(side.control + g_offSingleBoneRotationFlags,
                              &side.originalFlags, sizeof(side.originalFlags)) && *restored;
    if (side.restoreNext)
        *restored = WriteRigBytes(side.control + g_offSkelNextControl,
                                  &side.originalNext, sizeof(side.originalNext)) && *restored;
    *restored = WriteRigBytes(side.rollMapAddress,
                              &side.originalRollMap, 1) && *restored;
    *restored = WriteRigBytes(side.sourceMapAddress,
                              &side.originalSourceMap, 1) && *restored;
}

static void RestoreForearmTwistOverridesBeforeGame(uintptr_t pawn)
{
    if (!g_forearmTwistOverride.active) return;
    const ForearmTwistOverrideState saved = g_forearmTwistOverride;
    g_forearmTwistOverride = {};
    if (saved.pawn != pawn || !LooksLikePlayerPawn(pawn)) {
        Log("[hands-forearm] prior plain-control override discarded: pawn replaced");
        return;
    }
    bool restored = true;
    RestoreOneForearmTwist(saved.left, &restored);
    RestoreOneForearmTwist(saved.right, &restored);
    if (!restored) {
        g_forearmTwistWriteFault = true;
        Log("[hands-forearm] RESTORE FAILED - plain-control forearm twist disabled");
    }
}

static void ApplyForearmTwists(uintptr_t pawn, const P13PoseSnapshot& pose)
{
    if (!g_motionHands || g_forearmTwistWriteFault || !g_wristOverride.active) return;
    const bool useLeft = g_wristOverride.left.active;
    const bool useRight = g_wristOverride.right.active;
    if (!useLeft && !useRight) return;

    ForearmTwistRig rig{};
    if (!DiscoverForearmTwistRig(pawn, useLeft, useRight, &rig)) {
        static bool reported = false;
        if (!reported) {
            Log("[hands-forearm] waiting: fixed root/Hips controls, flags, or roll maps"
                " did not validate");
            reported = true;
        }
        return;
    }
    if (g_forearmTwistReportedMesh != rig.mesh) {
        Log("*** [hands-forearm] fixed plain-control rig validated: HipsControl ->"
            " LeftForeArmRoll 41, SwingControl -> RightForeArmRoll 71");
        g_forearmTwistReportedMesh = rig.mesh;
    }

    ForearmTwistOverrideState frame{};
    frame.active = true;
    frame.pawn = pawn;
    bool wrote = true;
    if (useLeft) wrote = ApplyOneForearmTwist(rig, pose.left, true, &frame.left);
    if (useRight && wrote) wrote = ApplyOneForearmTwist(rig, pose.right, false, &frame.right);
    g_forearmTwistOverride = frame;
    if (!wrote) {
        RestoreForearmTwistOverridesBeforeGame(pawn);
        g_forearmTwistWriteFault = true;
        Log("[hands-forearm] WRITE/READ-BACK FAILED - twist disabled; stable hand controls remain");
        return;
    }
    if (useLeft) ++g_forearmTwistLeftWrites;
    if (useRight) ++g_forearmTwistRightWrites;
    if ((useLeft && g_forearmTwistLeftWrites == 1) ||
        (useRight && g_forearmTwistRightWrites == 1)) {
        Log("*** [hands-forearm] controller roll acquired through fixed plain controls:"
            " L=%d R=%d", useLeft ? 1 : 0, useRight ? 1 : 0);
    }
}
#endif

// ---- diag: whose view is the engine actually rendering? ----
//
// The question behind three separate faults. The scene test rejects matrices during menus, death
// cameras and cutscenes, and the log could only ever say that rejection went up, never why. If
// Target is the pawn the frame is the player's view and rejection is a real fault; if it is
// anything else, rejection is the correct answer to a frame that was never ours.
//
// ✅ The struct layout was a guess when this was written - LookupProp gives the offset of the
// ViewTarget STRUCT, and that its first member is the Target pointer was UE3 knowledge rather
// than a measurement here. The first run settled it, reading back "CameraActor" through an intro
// cutscene and "TdTutorialPawn" in gameplay. Both are real classes in the right places, which a
// wrong offset does not produce.
//
// It earned its keep immediately: TdTutorialPawn is how the new-game failure was found, and no
// amount of staring at the pawn search would have shown it. Nothing is GATED on this yet - the
// scene test and the watchdog should use it, and that is a later rung, not this one.
static void ReportViewTarget()
{
    if (g_offCtlCamera < 0 || g_offCamViewTarget < 0 || !g_playerCtl) return;

    uint32_t cam = 0;
    if (!SafeU32(g_playerCtl + g_offCtlCamera, &cam) || cam < 0x10000) {
        Log("[view] PlayerCamera unreadable at +0x%04X", g_offCtlCamera);
        return;
    }
    uint32_t target = 0;
    if (!SafeU32(cam + g_offCamViewTarget, &target) || target < 0x10000) {
        Log("[view] ViewTarget.Target unreadable at camera %p +0x%04X"
            " - the struct's first member is not a pointer, so the layout guess is wrong",
            (void*)cam, g_offCamViewTarget);
        return;
    }

    char cls[64] = "?";
    uint32_t clsObj = 0;
    if (SafeU32(target + 0x34, &clsObj) && clsObj >= 0x10000) ReadObjName(clsObj, cls, sizeof(cls));

    // Only on a CHANGE. Steady state is one line per transition, which is exactly the signal -
    // gameplay to cutscene and back - rather than a periodic reminder of what has not changed.
    static uint32_t lastTarget = 0;
    if (target == lastTarget) return;
    lastTarget = target;
    Log("*** [view] ViewTarget -> %p class \"%s\"  |  pawn is %p  =>  %s",
        (void*)target, cls, (void*)g_playerPawn,
        (target == (uint32_t)g_playerPawn) ? "THE PLAYER'S VIEW"
                                           : "NOT the pawn - cutscene, death cam or scripted camera");
}

PFN_SetVSConstF g_origSetVSConstF = nullptr;

static volatile LONG g_vmScanArmed = 0;
volatile LONG g_vmWindowsTested = 0;
volatile LONG g_vmPoseFailures  = 0;
volatile LONG g_vmRescanRequest = 0;
bool          g_vmProven  = false;
int           g_vmStrikes = 0;
static int  g_vmBestReg = -1;
static bool g_vmBestRow = false;
bool  g_vmBestWorld = false;
float g_vmBestScore = -1e9f;
static int  g_vmCandidates = 0;

static bool ReadCameraAnchor(float* loc, int32_t* rot)
{
    if (g_offCamLoc < 0 || g_offCamRot < 0) return false;
    if (!LooksLikePlayerPawn(g_playerPawn)) return false;
    return SafeRead(g_playerPawn + g_offCamLoc, loc, 12) &&
           SafeRead(g_playerPawn + g_offCamRot, rot, 12);
}

// Camera pose straight from the pawn's cached values - the same numbers CalcCamera produced.
static bool GetCameraPose(float* loc, float* fwd)
{
    int32_t rot[3];
    if (!ReadCameraAnchor(loc, rot)) return false;

    const float kToRad = 3.14159265f / 32768.0f;
    const float pitch = rot[0] * kToRad, yaw = rot[1] * kToRad;
    fwd[0] = cosf(pitch) * cosf(yaw);
    fwd[1] = cosf(pitch) * sinf(yaw);
    fwd[2] = sinf(pitch);
    return true;
}

// The world transform of the rendered head anchor. Position starts at the exact camera location
// CalcCamera cached on the pawn, then receives the same current 6-DOF offset as the view matrix.
// Orientation uses the cached camera animation/rotation, which is the frame Mesh1p must inhabit.
static bool GetRenderedHeadFrame(RenderedHeadFrame* out)
{
    if (!out) return false;
    float loc[3];
    int32_t rot[3];
    if (!ReadCameraAnchor(loc, rot)) return false;

    const float kToRad = 3.14159265f / 32768.0f;
    const float pitch = (float)rot[0] * kToRad;
    const float yaw   = (float)rot[1] * kToRad;
    const float roll  = (float)rot[2] * kToRad;
    const float sp = sinf(pitch), cp = cosf(pitch);
    const float sy = sinf(yaw),   cy = cosf(yaw);
    const float sr = sinf(roll),  cr = cosf(roll);

    // UE3 FRotationMatrix axes: X forward, Y right, Z up.
    out->forward = { cp*cy, cp*sy, sp };
    out->right = { sr*sp*cy - cr*sy,
                   sr*sp*sy + cr*cy,
                  -sr*cp };
    out->up = { -(cr*sp*cy + sr*sy),
                  cy*sr - cr*sp*sy,
                  cr*cp };
    const MEVR_Vec3 sixDofRight = out->right;  // ApplySixDof runs before the head-roll injection

    // Head roll never reaches PlayerCameraRotation: UE3 zeros controller roll and the mod adds
    // it in the projection path. Put the same roll into the logical head frame used by hands,
    // otherwise a stationary controller would orbit when the player tilted their head.
    if (g_rollEnabled && fabsf(g_headRoll) > 1e-5f) {
        const float th = g_headRoll * (float)g_rollSign;
        const float ch = cosf(th), sh = sinf(th);
        const MEVR_Vec3 oldRight = out->right;
        const MEVR_Vec3 oldUp = out->up;
        out->right = { oldRight.x*ch + oldUp.x*sh,
                       oldRight.y*ch + oldUp.y*sh,
                       oldRight.z*ch + oldUp.z*sh };
        out->up = { oldUp.x*ch - oldRight.x*sh,
                    oldUp.y*ch - oldRight.y*sh,
                    oldUp.z*ch - oldRight.z*sh };
    }

    // ApplySixDof uses the levelled camera-right axis, world up, and their derived forward.
    // Repeat that composition here so the rendered head and hand targets share one anchor.
    float rx = sixDofRight.x, ry = sixDofRight.y;
    const float rl = sqrtf(rx*rx + ry*ry);
    if (rl < 1e-6f) return false;
    rx /= rl; ry /= rl;
    const float ox = rx*g_dofOffset[0] + ry*g_dofOffset[2];
    const float oy = ry*g_dofOffset[0] - rx*g_dofOffset[2];
    const float oz =                          g_dofOffset[1];
    out->position = { loc[0] + ox, loc[1] + oy, loc[2] + oz };

    return FiniteVec(out->position) && FiniteVec(out->forward) &&
           FiniteVec(out->right) && FiniteVec(out->up);
}

// Evaluate one 4-register window under one storage convention.
static void TestWindow(int reg, const float* m, bool asRow,
                       const float* camLoc, const float* fwd)
{
    // Rows of a row-vector matrix, or columns of a column-vector one.
    auto at = [&](int r, int c) { return asRow ? m[r * 4 + c] : m[c * 4 + r]; };

    auto wOf = [&](float x, float y, float z) {
        return x * at(0, 3) + y * at(1, 3) + z * at(2, 3) + at(3, 3);
    };
    const float wCam = wOf(camLoc[0], camLoc[1], camLoc[2]);
    const float wOrg = wOf(0.0f, 0.0f, 0.0f);

    // The xyz of the w term should be the camera forward axis.
    float ax = at(0, 3), ay = at(1, 3), az = at(2, 3);
    const float len = sqrtf(ax * ax + ay * ay + az * az);
    if (len < 1e-6f) return;
    ax /= len; ay /= len; az /= len;
    const float dotFwd = ax * fwd[0] + ay * fwd[1] + az * fwd[2];

    // The origin probe is pure matrix content - no camera reading to go stale - so it gets a
    // tight tolerance. The world probe absorbs a frame of camera latency and gets a loose one.
    const bool originHit = fabsf(wOrg) < 1.0f;
    const bool worldHit  = fabsf(wCam) < 25.0f;
    if (!originHit && !worldHit) return;
    if (fabsf(dotFwd) < 0.9f) return;          // direction must agree, or it is not the view

    g_vmCandidates++;
    Log("[vm] c%-3d %s  w(cam)=%9.3f  w(origin)=%9.3f  dotFwd=%+.4f  %s",
        reg, asRow ? "ROW" : "COL", wCam, wOrg, dotFwd,
        originHit ? "<- TRANSLATED-WORLD" : "<- world space");

    // ---- rank, do not take the first ----
    //
    // The first scan produced ~20 candidates a frame and the real one was not first. They are
    // the degeneracy the reference project warned about, arriving from the opposite direction:
    // there the ORIGIN probe was the noise-free one, because that engine renders
    // translated-world. Mirror's Edge uploads WORLD-space matrices, so here it is the origin
    // probe that admits a block of unrelated float3x4 transforms the sliding window straddles.
    //
    // The real matrix is not marginally better, it is exact: w on its own probe is 0.000 and
    // dotFwd is 1.0000, against 0.90-0.98 and |w| in the hundreds of thousands for the noise.
    // Scoring on both separates them by a wide margin rather than a threshold.
    const float probeW = worldHit ? fabsf(wCam) : fabsf(wOrg);
    const float score  = fabsf(dotFwd) - probeW * 0.01f;
    if (score > g_vmBestScore) {
        g_vmBestScore = score;
        g_vmBestReg   = reg;
        g_vmBestRow   = asRow;
        g_vmBestWorld = worldHit;
    }
}

// ---------------------------------------------------------------- rung 6b: injection
//
// Once the matrix is located, moving the camera is a pre-multiplied translation applied on the
// way to the GPU:
//
//     M' = T(-o) . M      row-vector form, so   row3 -= o.x*row0 + o.y*row1 + o.z*row2
//
// Exact for any offset including forward, and it needs no knowledge of the projection. It also
// holds in either space - world or translated-world - because a translation moves the origin,
// not the axes. The space only ever mattered for finding the matrix.
//
// The game's buffer is const and belongs to the engine, so the covering call is copied,
// modified and forwarded. Only calls that actually cover the matrix pay that cost.

static void ApplyRoll(float* m, bool rowStorage);      // defined with the stereo code below
static void ApplyPitchFix(float* m, bool rowStorage);  // likewise
static void ApplyRollFix(float* m, bool rowStorage);   // likewise
static void ApplyYawFix(float* m, bool rowStorage);    // likewise
static void ApplyYawLag(float* m, bool rowStorage);    // likewise
static void ApplySixDof(float* m, bool rowStorage);

int   g_vmReg = -1;              // committed after a successful scan
bool  g_vmRow = true;
int   g_vmMode = 0;              // 0 off, 1..3 fixed test offsets
float g_eyeInject = 0.0f;        // -1 left eye, +1 right eye, 0 none - set per frame
float g_vmOffset[3] = { 0, 0, 0 };
static long g_vmInjections = 0;


// ---- the camera position, read inside the frame, BEFORE anything is judged against it ----
//
// ⚠️ This has to run ahead of the acceptance test, not after it, and the ordering was the whole
// zip-line fault.
//
// The test asks whether a matrix renders from where the camera is: it maps the camera position
// through the matrix and requires the result to be near zero. The position it used came from
// Present, so it described where the camera was a frame ago, and the tolerance was a flat 25
// units. Standing still that is generous. On a zip line the camera covers far more than 25 units
// in a frame, so the REAL scene matrix failed its own test and was rejected - and a rejected
// matrix leaves g_sceneMat holding the previous frame's, which every duplicated draw then
// renders from. The whole view is built from a stale camera, which is what "misaligned" is.
//
// It is also why widening the tolerance alone would be the wrong fix: the error is not that 25
// is too small, it is that the reference was stale, and any tolerance loose enough to cover a
// zip line would be loose enough to admit anything at walking pace.
//
// So the position is read here, once per frame, before the first matrix of the frame is judged.
// The distance it moved since the last frame is kept as well, because that is exactly the
// uncertainty between the game thread updating this and the render thread reading it - a bound
// that comes from the measurement rather than from a number chosen in advance.
static float g_pivotStep = 0.0f;      // how far the camera moved since the last frame

static void SampleLivePivot()
{
    static long lastFrame = -1;
    if (lastFrame == g_frames) return;
    lastFrame = g_frames;

    if (g_offCamLoc < 0 || !g_playerPawn) { g_livePivotValid = false; return; }
    float cl[3];
    if (!SafeRead(g_playerPawn + g_offCamLoc, cl, sizeof(cl))) { g_livePivotValid = false; return; }

    if (g_livePivotValid) {
        const float dx = cl[0] - g_livePivot[0];
        const float dy = cl[1] - g_livePivot[1];
        const float dz = cl[2] - g_livePivot[2];
        g_pivotStep = sqrtf(dx*dx + dy*dy + dz*dz);

        // Fine-grained camera-motion census for the run-23 world tremble: the user marked the
        // floor itself juddering, which points at THIS value dithering at a scale the existing
        // whole-UU "camera step" line rounds away. Mean, max, and the count of frames in the
        // dither band say whether the game camera is the source, with 0.01 UU resolution.
        static float stepMax = 0.0f, stepSum = 0.0f;
        static long stepCount = 0, ditherFrames = 0;
        if (std::isfinite(g_pivotStep)) {
            if (g_pivotStep > stepMax) stepMax = g_pivotStep;
            stepSum += g_pivotStep;
            if (g_pivotStep > 0.2f && g_pivotStep < 5.0f) ++ditherFrames;
            if (++stepCount >= 600) {
                Log("[vm] camera position steps over 600 frames: mean %.2f max %.2f UU,"
                    " %ld frames in the 0.2-5.0 dither band",
                    stepSum / (float)stepCount, stepMax, ditherFrames);
                stepMax = 0.0f; stepSum = 0.0f; stepCount = 0; ditherFrames = 0;
            }
        }
    }
    g_livePivot[0] = cl[0]; g_livePivot[1] = cl[1]; g_livePivot[2] = cl[2];
    g_livePivotValid = true;
}

static HRESULT STDMETHODCALLTYPE Hook_SetVSConstF(IDirect3DDevice9* dev, UINT startReg,
                                                  const float* data, UINT count)
{
    SampleLivePivot();

    // ---- injection, before the scan block so a scan does not see our own modification ----
    if ((g_vmMode != 0 || g_stereoMode == 1) && g_vmReg >= 0 && data && count >= 4 &&
        startReg <= (UINT)g_vmReg && startReg + count >= (UINT)g_vmReg + 4 &&
        count <= 256) {
        // ---- ⚠️ re-validate EVERY call. c0 carries more than one matrix ----
        //
        // Measured: the derived FOV alternates between 90.0 x 58.7 and 160.0 x 160.0 on
        // consecutive uploads to the same register. Something other than the scene
        // view-projection - a shadow or light transform - lands at c0 too, and injecting into
        // it corrupts that pass while looking like a rendering bug somewhere else entirely.
        //
        // So the register is where to LOOK, never permission to modify. The same test that
        // found the matrix decides each call: a world->clip matrix maps the camera position to
        // clip.w ~ 0, and nothing else at this register does.
        //
        // Cheap enough for a hot path - four multiply-adds against a pose cached once per
        // frame, no memory reads.
        {
            const float* q = data + (size_t)((UINT)g_vmReg - startReg) * 4;
            const float w = g_camCache[0] * (g_vmRow ? q[3]  : q[12])
                          + g_camCache[1] * (g_vmRow ? q[7]  : q[13])
                          + g_camCache[2] * (g_vmRow ? q[11] : q[14])
                          + (g_vmRow ? q[15] : q[15]);
            // g_vmValidate off reproduces the pre-fix behaviour exactly: inject into EVERY
            // upload at this register, foreign matrices included. Kept as a toggle rather than
            // reverted, so the two states can be compared in one run instead of across two
            // builds - and so the fix is not lost to answer a question about it.
            // ⚠️ POSITION IS NOT ENOUGH, and the log named the gap outright.
            //
            // The w test only asks "is this matrix rendered from where the camera is". Anything
            // else drawn from the same point passes it, whatever direction it faces - and things
            // are. Measured this run: matrices arriving with a pitch of -90.0 and of -45, while
            // the controller driving the real view sat at 1.2 degrees. A camera looking straight
            // down is not the player's view by any reading.
            //
            // Those were being accepted, cached as g_sceneMat, and then handed to the eye
            // offsets, the pitch and roll corrections and the draw duplication - all of which
            // then operated on a matrix belonging to something else. The corrections would have
            // rotated it by tens of degrees, which is the shape of a view that comes apart in
            // one specific situation and is fine everywhere else.
            //
            // So the direction has to agree too. PlayerCameraRotation is the right reference
            // because it is the COMPOSED camera rotation - animation included - so a legitimate
            // 40 degree landing dip moves both it and the matrix together and still passes.
            //
            // 0.90 is about 25 degrees of slack: loose enough for the frame of lag on the cached
            // pose during a fast turn, tight enough that the 45 and 90 degree impostors cannot
            // get through.
            float dirOk = 1.0f;
            {
                const float mx = g_vmRow ? q[3]  : q[12];
                const float my = g_vmRow ? q[7]  : q[13];
                const float mz = g_vmRow ? q[11] : q[14];
                const float ml = sqrtf(mx*mx + my*my + mz*mz);
                if (ml > 1e-6f)
                    dirOk = (mx*g_camFwd[0] + my*g_camFwd[1] + mz*g_camFwd[2]) / ml;
            }
            // Tolerance from the measurement rather than a fixed number: whatever distance the
            // camera covered last frame bounds how far this frame's true position can be from
            // the one just read. Standing still it collapses to the original 25 and rejects
            // exactly what it always did; on a zip line it opens up by precisely the amount the
            // camera is actually moving, and by nothing more.
            const float tol = 25.0f + g_pivotStep * 1.5f;
            const bool isScene = (g_camCacheValid && fabsf(w) <= tol && dirOk >= 0.90f);
            g_c0IsScene = isScene;   // tracked even when validation is off, for ShouldDuplicate

            // ---- acceptance, reported where duplication can see it ----
            //
            // These counters existed and were only ever printed on the alternate-eye path, which
            // simultaneous stereo returns before reaching. So under the mode this has been
            // running in for the whole session, the single most important number - is the scene
            // matrix being accepted at all - has been invisible. A rejected matrix is not a
            // dropped frame, it silently leaves the previous one in place, and nothing said so.
            {
                static long lastFrame = -1;
                static long acc = 0, rej = 0;
                // The watchdog's own pair. Deliberately NOT the same counters: acc/rej answer
                // "what is happening", including the near-total rejection of a respawn window,
                // and that reading is worth keeping intact. These answer the narrower question
                // the watchdog acts on - with a camera pose we trust, is this register the view.
                static long wacc = 0, wrej = 0;
                if (isScene) acc++; else rej++;
                if (g_camCacheValid) { if (isScene) wacc++; else wrej++; }
                if (lastFrame != g_frames) {
                    lastFrame = g_frames;
                    static long frames = 0;
                    if (++frames % 600 == 0) {
                        Log("[vm] over 600 frames: %ld accepted, %ld rejected (%.1f%%)  |"
                            " camera step %.0f UU/frame, tolerance %.0f",
                            acc, rej, 100.0f * (float)rej / (float)((acc + rej) ? (acc + rej) : 1),
                            g_pivotStep, tol);

                        // ---- has this register EVER been the view? ----
                        //
                        // Half is not a judgement about one window, it is the line between "this
                        // register carries the view and the engine sometimes draws other things"
                        // and "this register has never carried the view at all". Real ones sit
                        // at 0.1-24% while playing; the wrong one sat at 99.9% forever.
                        //
                        // One window under the line is proof, and proof is permanent. Everything
                        // that made the first version fire on a working register - the menu, the
                        // death camera, a cutscene - arrives after that proof and is ignored.
                        const long tot = wacc + wrej;
                        if (tot >= kVMJudgeSamples) {
                            if (wrej * 2 < tot) {
                                if (!g_vmProven) {
                                    g_vmProven = true;
                                    Log("[vm] c%d proved itself - %ld of %ld accepted. The"
                                        " watchdog is finished with this register.",
                                        g_vmReg, wacc, tot);
                                }
                            } else if (!g_vmProven && ++g_vmStrikes >= kVMMaxStrikes) {
                                Log("[vm] c%d has failed %d substantial windows without ever"
                                    " once being the view (last: %ld of %ld rejected)",
                                    g_vmReg, g_vmStrikes, wrej, tot);
                                InterlockedExchange(&g_vmRescanRequest, 1);
                            }
                        }
                        acc = rej = 0;
                        wacc = wrej = 0;
                    }
                }
            }

            if (g_vmValidate && !isScene) {
                InterlockedIncrement(&g_vmRejected);
                return g_origSetVSConstF(dev, startReg, data, count);
            }
            InterlockedIncrement(&g_vmAccepted);

            // ANSWERED, and removed: do all the accepted matrices in a frame agree? They do.
            // 127896 accepted uploads across 600 frames, none disagreeing, worst 0.03 degrees.
            // One view per frame, and the motion-blur matrix it was written to catch is either
            // not uploaded to this register or does not pass the position test.
            //
            // Removed rather than left in because it ran per accepted upload - a normalise and
            // an acos roughly two hundred times a frame, in the hottest function in the program,
            // to keep re-answering a settled question. With the frame budget now the thing
            // standing between 60 fps and 120, a diagnostic past its answer is not free.

            // Cache the UNMODIFIED scene matrix for the draw-duplication path, which builds
            // both eyes from it. Taken here because this is the only point at which the
            // engine's own matrix is known to be both current and untouched.
            memcpy(g_sceneMat, q, sizeof(float) * 16);
            g_sceneMatValid = true;

            // ---- the controller's pitch, read HERE and not in Present ----
            //
            // Present runs after the frame is rendered, so anything sampled there is a frame old
            // by the time the next frame's draws use it. For a value that only drifts that is
            // fine; for one that jumps with the mouse or with an animation it is exactly the
            // error being corrected.
            //
            // This is the one point that is both on the render thread and inside the frame, so
            // the controller's pitch read here belongs to the same frame as the matrix beside it.
            //
            // ⚠️ VALIDATED, not merely non-null. The first version reasoned that
            // ApplyHeadTracking revalidates the pointer every frame so this could skip it - but
            // that check runs in PRESENT, after the frame is drawn. Between the controller being
            // destroyed and Present noticing, this reads a freed object, and the log shows the
            // pointer going stale twice in a single run.
            //
            // The value read would still look like a plausible angle - any dword masked to 16
            // bits does - so nothing downstream could tell, and it feeds a correction that
            // ROTATES THE VIEW. That is the shape of an intermittent visual break during a jump
            // that does not reproduce on reload.
            //
            // Once per frame is cheap enough to validate properly rather than argue about.
            if (g_liveCtlFrame != g_frames && g_offActorRotation >= 0) {
                g_liveCtlFrame = g_frames;
                g_liveCtlValid = false;
                if (LooksLikePlayerController(g_playerCtl)) {
                    int32_t cr[2] = { 0, 0 };            // FRotator {Pitch, Yaw}
                    if (SafeRead(g_playerCtl + g_offActorRotation, cr, sizeof(cr))) {
                        auto signed16 = [](int32_t v) {
                            int32_t s = v & 0xFFFF;
                            return (s > 32767) ? (s - 65536) : s;
                        };
                        const float kRad = 6.28318531f / 65536.0f;
                        g_liveCtlPitch = (float)signed16(cr[0]) * kRad;
                        g_liveCtlYaw   = (float)signed16(cr[1]) * kRad;
                        g_liveCtlValid = true;
                    }
                }

                // ---- how far the view is actually BEHIND the head, measured not predicted ----
                //
                // ⚠️ The previous version predicted one display period ahead and corrected that.
                // It measured 0.7 degrees when the real lag is several times larger, and the
                // reason is that it was answering the wrong question: it asked how far the head
                // will move in one period, when what matters is how far the view has ALREADY
                // fallen behind - which is the whole render pipeline, not one period of it.
                //
                // ⚠️ And the pose-honesty check could not see this either. It compares the CHANGE
                // in head yaw against the CHANGE in the image's yaw, and a constant lag produces
                // identical changes. During a steady sweep it reports zero while the view sits
                // degrees behind. It rules out the compositor being lied to about acceleration,
                // which it did, and says nothing about a standing offset. That is a real limit
                // of that measurement and it was read as an all-clear.
                //
                // This measures the offset itself. (matrix yaw + head yaw) is invariant - the two
                // conventions run in opposite senses, established when the 6-DOF frame was
                // checked - so it holds still except when the player turns by other means. Its
                // deviation from its own slow mean is precisely how far the engine has not caught
                // up, whatever the cause: the frame of write latency, the render pipeline, any
                // smoothing the engine applies to its own rotation.
                //
                // Pitch never had this problem because ApplyPitchFix anchors it to an absolute
                // head pitch every frame. Yaw has no absolute reference - it must be free to
                // accumulate past the head's range - so nothing has ever corrected it. That
                // asymmetry predicts the symptom exactly: a bounce while sweeping LEFT AND
                // RIGHT, and none while looking up and down.
                // ---- head roll, sampled for the moment this frame will be SEEN ----
                //
                // Moved off Present for the reason recorded there: the image and the pose
                // submitted with it were a frame apart, and the compositor reprojects the
                // difference. Asked for one period ahead, which is where this frame lands, so the
                // roll drawn into the image is the roll the pose will claim.
                if (g_predTime) {
                    float rollNow = 0.0f, rollWeight = 0.0f;
                    if (GetHeadRoll(g_predTime + g_predPeriod, &rollNow, &rollWeight)) {
                        float d = rollNow - g_headRoll;
                        while (d >  3.14159265f) d -= 6.28318531f;
                        while (d < -3.14159265f) d += 6.28318531f;
                        // ORIENTATION_VALID also survived the same tracking discontinuity that
                        // produced the 1.31 m position jump. It reported about 171 degrees of
                        // roll for one frame, rotating both logical hand anchors around the head.
                        // Reject one isolated >45-degree step. If a new value persists for two
                        // samples it is a real reference change and is accepted on the second.
                        static bool haveRejectedRoll = false;
                        static float rejectedRoll = 0.0f;
                        const float maxPhysicalStep = 45.0f *
                            (3.14159265358979323846f / 180.0f);
                        bool acceptRoll = fabsf(d) <= maxPhysicalStep;
                        if (!acceptRoll) {
                            float fromRejected = rollNow - rejectedRoll;
                            while (fromRejected >  3.14159265f) fromRejected -= 6.28318531f;
                            while (fromRejected < -3.14159265f) fromRejected += 6.28318531f;
                            if (haveRejectedRoll && fabsf(fromRejected) <
                                15.0f * (3.14159265358979323846f / 180.0f)) {
                                acceptRoll = true;
                                Log("*** [head] large roll change persisted for two samples;"
                                    " accepting new tracking reference");
                            } else {
                                rejectedRoll = rollNow;
                                haveRejectedRoll = true;
                                Log("*** [head] rejected isolated %+.1f deg roll discontinuity",
                                    d * 57.2957795f);
                            }
                        }
                        if (acceptRoll) {
                            g_headRoll += rollWeight * d;
                            haveRejectedRoll = false;
                        }
                    }

                    // ---- and the pitch target, for the same reason ----
                    //
                    // The last axis still being sampled in Present. Yaw is measured against the
                    // matrix on this thread, roll moved here a commit ago, and pitch was left
                    // behind - so ApplyPitchFix has been correcting the view towards where the
                    // head was one frame ago and calling it done.
                    //
                    // It hid because it was small next to everything else. The frame grab used
                    // to take 4 ms of a 13 ms frame and the yaw lag was five degrees; a frame of
                    // pitch error was in the noise. At 0.4 ms and 119 fps, with yaw and roll both
                    // measured in fractions of a degree, it is what is left - which is exactly
                    // when "judder looking up and down, but not left and right" becomes possible
                    // to notice.
                    float pitchNow = 0.0f;
                    if (GetHeadPitchRaw(g_predTime + g_predPeriod, &pitchNow)) {
                        g_pitchTarget = pitchNow * (float)g_pitchSign;
                        g_pitchTargetValid = true;
                    }

                    // 6-DOF, for the same instant as everything else. Its offset is a
                    // TRANSLATION - a stale one slides the world instead of turning it, and a
                    // sliding world is what judder looks like from inside a headset.
                    UpdateSixDof(g_predTime + g_predPeriod);

                    // How much does that offset move between frames? A translation the size of
                    // the offset itself is not the issue; a translation that JUMPS is. Reported
                    // so a stale-sample artifact can be told from a correct one that simply
                    // feels unfamiliar.
                    {
                        static float prev[3] = { 0, 0, 0 };
                        static float worst = 0.0f;
                        static long  n = 0;
                        const float dx = g_dofOffset[0] - prev[0];
                        const float dy = g_dofOffset[1] - prev[1];
                        const float dz = g_dofOffset[2] - prev[2];
                        const float step = sqrtf(dx*dx + dy*dy + dz*dz);
                        prev[0] = g_dofOffset[0]; prev[1] = g_dofOffset[1]; prev[2] = g_dofOffset[2];
                        if (step > worst) worst = step;
                        if (++n >= 600) {
                            Log("[6dof] offset moves at most %.1f UU per frame  (%.1f cm - at 120"
                                " fps a real head cannot do much more than 2)", worst, worst);
                            n = 0; worst = 0.0f;
                        }
                    }
                }

                g_yawLagRad = 0.0f;
                if (g_predTime && g_sceneMatValid) {
                    float hy = 0.0f;
                    const float mfx = g_vmRow ? g_sceneMat[3] : g_sceneMat[12];
                    const float mfy = g_vmRow ? g_sceneMat[7] : g_sceneMat[13];
                    if (GetHeadYawRaw(g_predTime, &hy) &&
                        (fabsf(mfx) + fabsf(mfy) > 1e-6f)) {
                        auto wrapf = [](float a) {
                            while (a >  3.14159265f) a -= 6.28318531f;
                            while (a < -3.14159265f) a += 6.28318531f;
                            return a;
                        };
                        const float psi = wrapf(atan2f(mfy, mfx) + hy);
                        static bool  have = false;
                        static float mean = 0.0f;
                        if (!have) { mean = psi; have = true; }

                        // ---- a mouse turn is not lag, and must not be corrected as one ----
                        //
                        // ⚠️ This is the drift after the mouse stops. The invariant moves for two
                        // completely different reasons: the engine falling behind the head, which
                        // is the error, and the player deliberately turning by other means, which
                        // is not. The first version could not tell them apart and leaned on a
                        // slow mean to absorb the second - so a mouse turn was treated as several
                        // degrees of lag, fought while the mouse moved, and then released as the
                        // mean caught up. Released over about a second, which is the camera
                        // creeping left or right a beat after the mouse stops.
                        //
                        // They can be told apart exactly, because we know what WE wrote. Whatever
                        // the controller's yaw moved beyond our own writes came from somewhere
                        // else, and belongs in the reference immediately rather than being
                        // mistaken for error and slowly forgiven.
                        float external = 0.0f;
                        {
                            static bool  havePrev = false;
                            static float prevCtl = 0.0f;
                            if (g_liveCtlValid) {
                                if (havePrev) {
                                    const float dCtl = wrapf(g_liveCtlYaw - prevCtl);
                                    external = wrapf(dCtl - g_writtenYawAccum);
                                }
                                prevCtl = g_liveCtlYaw;
                                havePrev = true;
                            }
                            g_writtenYawAccum = 0.0f;
                        }
                        mean = wrapf(mean + external);

                        float d = wrapf(psi - mean);
                        // Only a slow leak now, for accumulated numerical drift. The deliberate
                        // turns it used to have to absorb are handled above, exactly, so this no
                        // longer has to be a compromise between two jobs.
                        mean = wrapf(mean + 0.01f * d);

                        const float kMax = 20.0f * (3.14159265f / 180.0f);
                        if (d >  kMax) d =  kMax;
                        if (d < -kMax) d = -kMax;
                        if (!g_yawLagFix) d = 0.0f;

                        // Reported whether or not it is being applied, so NUMPAD6 off still
                        // answers "how big is the lag" rather than only "does correcting help".
                        static float worstVal = 0.0f, sumVal = 0.0f;
                        static long  n = 0;
                        const float raw = fabsf(wrapf(psi - mean));
                        if (raw > worstVal) worstVal = raw;
                        sumVal += raw;
                        if (++n >= 600) {
                            Log("[head] view is behind the head by %.2f deg mean, %.2f deg worst,"
                                " over %ld frames  (correction %s)",
                                (sumVal / (float)n) * 57.29578f, worstVal * 57.29578f, n,
                                g_yawLagFix ? "ON" : "OFF");
                            n = 0; worstVal = 0.0f; sumVal = 0.0f;
                        }
                        g_yawLagRad = d;
                    }
                }
            }

            // ---- read the FOV the game is ACTUALLY rendering with ----
            //
            // For a row-vector world->clip matrix, column 0 is the right axis scaled by
            // 1/tan(fovX/2) and column 3 is the unit forward axis. So tan(halfFov) =
            // |col3|/|col0|, and the same for y with column 1. Nothing has to be assumed about
            // UE3's FOVAngle convention or how it folds in aspect - the answer is read out of
            // what came through.
            //
            // ⚠️ ABOVE the simultaneous-stereo early return, and that placement is the whole
            // point. It used to sit below, so it only ran on the alternate-eye path - and the
            // duplication path, which needs this number to force its eye matrices, could only
            // get it if alternate-eye had happened to run first in the same session. The manual
            // key order F1 then F10 always did, so it always worked; arming stereo straight into
            // duplication never ran it at all, the force silently did nothing, and the layer
            // went on submitting the forced frustum for an unforced render.
            const float c0x = g_vmRow ? q[0] : q[0],  c0y = g_vmRow ? q[4] : q[1],  c0z = g_vmRow ? q[8]  : q[2];
            const float c1x = g_vmRow ? q[1] : q[4],  c1y = g_vmRow ? q[5] : q[5],  c1z = g_vmRow ? q[9]  : q[6];
            const float c3x = g_vmRow ? q[3] : q[12], c3y = g_vmRow ? q[7] : q[13], c3z = g_vmRow ? q[11] : q[14];
            const float l0 = sqrtf(c0x*c0x + c0y*c0y + c0z*c0z);
            const float l1 = sqrtf(c1x*c1x + c1y*c1y + c1z*c1z);
            const float l3 = sqrtf(c3x*c3x + c3y*c3y + c3z*c3z);
            if (l0 > 1e-6f && l1 > 1e-6f && l3 > 1e-6f) {
                g_gameHalfFovX = atanf(l3 / l0);
                g_gameHalfFovY = atanf(l3 / l1);
                // Logged as well as shown. The overlay carried this number and the log did not,
                // so it could only be reported from memory after the headset came off - which is
                // precisely the gap the overlay was added to close, reintroduced one value at a
                // time. Anything worth putting on screen is worth a log line.
                const float degX = g_gameHalfFovX * 114.5916f;
                const float degY = g_gameHalfFovY * 114.5916f;
                if (!g_gameFovValid || fabsf(degX - g_lastLoggedFovX) > 1.0f) {
                    g_lastLoggedFovX = degX;
                    Log("[fov] game renders %.1f x %.1f degrees (read from the matrix)", degX, degY);
                }
                g_gameFovValid = true;

                // ---- how often does the engine cull NARROWER than we draw? ----
                //
                // The log above only prints on a CHANGE, so its line counts are transitions and
                // not frames - and read as frames they say nothing. What they did show is that
                // the scene matrix alternates between the forced 135.4 and the engine's own
                // 90.0, and a 90.0 matrix culls 58.7 degrees vertically against the 100 we
                // render. Geometry outside that is dropped on those uploads and present on the
                // others, which is what "disappearing" looks like from inside the headset.
                //
                // Counting it settles whether that is an occasional blip or half the frames,
                // and those need different fixes: an occasional one is a write that loses a
                // race, a persistent one means FOVAngle is not the field the view is built from.
                if (g_targetHalfFovY > 0.0f) {
                    static long narrow = 0, total = 0;
                    if (g_gameHalfFovY < g_targetHalfFovY * 0.98f) narrow++;
                    if (++total >= 2000) {
                        Log("[fov] scene matrices culling NARROWER than we render: %ld of %ld"
                            " (%.0f%%) - last narrow one was %.1f vertical against our %.1f",
                            narrow, total, 100.0f * (float)narrow / (float)total,
                            degY, g_targetHalfFovY * 114.5916f);
                        narrow = 0; total = 0;
                    }
                }
            }
        }

        // Under simultaneous stereo the per-eye offset is applied per DRAW, not per upload, so
        // the upload passes through unchanged. Injecting here as well would offset both eyes
        // by a shared amount on top of their own.
        if (g_simulStereo) return g_origSetVSConstF(dev, startReg, data, count);

        float buf[256 * 4];
        memcpy(buf, data, sizeof(float) * 4 * count);
        float* m = buf + (size_t)((UINT)g_vmReg - startReg) * 4;

        float ox = g_vmOffset[0], oy = g_vmOffset[1], oz = g_vmOffset[2];
        // Kept for the FOV forcing below, which needs the ORIGINAL scales to compute its
        // factors and must not read them back after the translation has been applied.
        const float oldTanX = (g_gameFovValid && g_gameHalfFovX > 0.0f) ? tanf(g_gameHalfFovX) : 0.0f;
        const float oldTanY = (g_gameFovValid && g_gameHalfFovY > 0.0f) ? tanf(g_gameHalfFovY) : 0.0f;

        // ---- per-eye parallax, along the matrix's OWN right axis ----
        //
        // Column 0 of a row-vector world->clip matrix is the direction mapping to clip.x, so
        // normalising it gives world-space right. Read from the UNMODIFIED incoming data in
        // this same call, so it cannot be stale and cannot disagree with the matrix it is
        // about to modify - which recomputing it from the pawn's rotation could.
        // g_stereoStrength scales the separation only. World scale stays at the MEASURED
        // 100 UU/m, which is geometry and not a matter of taste - see ENGINE_NOTES for the
        // derivation from the game's own movement speeds. Conflating the two is what made this
        // confusing: one is a fact about the game, the other is a preference about comfort.
        if (g_stereoMode == 1 && g_eyeInject != 0 && g_halfIpdUU > 0.0f && g_stereoStrength > 0.0f) {
            const float rx = g_vmRow ? m[0] : m[0];
            const float ry = g_vmRow ? m[4] : m[1];
            const float rz = g_vmRow ? m[8] : m[2];
            const float rl = sqrtf(rx*rx + ry*ry + rz*rz);
            if (rl > 1e-6f) {
                const float s = g_eyeInject * g_halfIpdUU * g_stereoStrength / rl;
                ox += rx * s; oy += ry * s; oz += rz * s;
            }
        }
        if (g_vmRow) {
            // Rows are m[0..3], m[4..7], m[8..11], m[12..15].
            for (int c = 0; c < 4; ++c)
                m[12 + c] -= ox * m[0 + c] + oy * m[4 + c] + oz * m[8 + c];
        } else {
            // Columns: element (r,c) is m[c*4 + r]; the translation lives in column 3.
            for (int r = 0; r < 4; ++r)
                m[3 * 4 + r] -= ox * m[0 * 4 + r] + oy * m[1 * 4 + r] + oz * m[2 * 4 + r];
        }
        // ---- force the projection IN THE MATRIX ----
        //
        // Rescaling the x and y columns is exactly a clip-space x/y scale, which is exactly an
        // FOV change, and it composes correctly AFTER the positional offset - which is why it
        // runs here rather than before.
        //
        // ⚠️ Why not simply ask the engine. The reference project measured that UE3 ACCEPTS a
        // wide FOV and will not HOLD it: the camera update interpolates back toward its default
        // every tick, producing a 128 -> 80 degree swing that reads as a zoom on the monitor and
        // flickering black bars in the headset. The engine gets the last word before rendering,
        // so asking politely cannot win. Setting it here, on every upload, is the one place
        // nothing can argue.
        //
        // tan(halfFov) = |col3| / |col_n|, so scaling column n by tan(old)/tan(new) sets the new
        // field exactly. All FOUR elements of the column scale, including the translation term
        // the offset just wrote - the scale applies to the whole clip-x mapping, not part of it.
        if (g_fovForce && oldTanX > 1e-6f && oldTanY > 1e-6f &&
            g_targetHalfFovX > 0.0f && g_targetHalfFovY > 0.0f) {
            const float sx = oldTanX / tanf(g_targetHalfFovX);
            const float sy = oldTanY / tanf(g_targetHalfFovY);
            if (g_vmRow) {
                for (int r = 0; r < 4; ++r) { m[r * 4 + 0] *= sx; m[r * 4 + 1] *= sy; }
            } else {
                for (int i = 0; i < 4; ++i) { m[0 * 4 + i] *= sx; m[1 * 4 + i] *= sy; }
            }
        }
        // Same three, in the same order, as the duplication path's BuildEyeMatrix. Kept in step
        // deliberately: the two paths drifting apart is exactly how the FOV read ended up living
        // on only one of them, and that cost several runs to find.
        ApplyRollFix(m, g_vmRow);
        ApplyPitchFix(m, g_vmRow);
        ApplyYawFix(m, g_vmRow);
        ApplyYawLag(m, g_vmRow);
        ApplySixDof(m, g_vmRow);
        ApplyRoll(m, g_vmRow);

        if (++g_vmInjections == 1 || (g_vmInjections % 20000) == 0)
            Log("[vm] injection #%ld  offset (%.1f, %.1f, %.1f) eye %+.0f  accepted %ld rejected %ld",
                g_vmInjections, ox, oy, oz, g_eyeInject,
                InterlockedCompareExchange(&g_vmAccepted, 0, 0),
                InterlockedCompareExchange(&g_vmRejected, 0, 0));
        return g_origSetVSConstF(dev, startReg, buf, count);
    }

    // Hot path: thousands of calls a frame. Do nothing at all unless a scan is armed.
    if (InterlockedCompareExchange(&g_vmScanArmed, 0, 0) && count >= 4 && data) {
        float camLoc[3], fwd[3];
        if (GetCameraPose(camLoc, fwd)) {
            const UINT windows = count - 3;
            for (UINT i = 0; i < windows && i < 64; ++i) {
                InterlockedIncrement(&g_vmWindowsTested);
                TestWindow((int)(startReg + i), data + i * 4, true,  camLoc, fwd);
                TestWindow((int)(startReg + i), data + i * 4, false, camLoc, fwd);
            }
        } else {
            InterlockedIncrement(&g_vmPoseFailures);
        }
    }
    return g_origSetVSConstF(dev, startReg, data, count);
}

// ================================================================ rung 7: simultaneous stereo
//
// Draw-call duplication. Every scene draw is issued TWICE - once into the left half of the
// backbuffer with the left-eye matrix, once into the right half with the right-eye matrix -
// so both eyes come from the same frame instead of alternating.
//
// That removes the temporal disparity that made 100% separation fail to fuse: under
// alternate-eye each eye's image was one frame stale, and near objects sweep the view fastest,
// so the inter-eye difference carried a TIME offset on top of the spatial one.
//
// ---- side-by-side inside the existing backbuffer, not a wider one ----
//
// Each eye gets half the width: 1280x1440. That cost horizontal resolution and kept the frame
// grab at 3.7 MP, which was the reason the D3D9Ex work stayed deferred until rung 8.
//
// ⚠️ That reason is gone and the resolution is still halved. Rung 8 took the grab to 0.4 ms and
// it no longer scales with anything the CPU touches, so the constraint this sizing served has
// been removed - but rung 9 tried to widen the buffer and MEASURED why that does not work: the
// engine sizes its scene targets from its OWN configured resolution and upscales to whatever
// backbuffer it is handed, so a wider backbuffer enlarges an upscale and nothing else.
//
// Worse, it breaks the split. ShouldDuplicate identifies the scene by matching the BACKBUFFER's
// dimensions, which was a coincidence that held for nine rungs rather than a property. Any
// future attempt at per-eye resolution has to fix that identity test FIRST, and separately.
//
// It also improves the aspect: 1280x1440 is 0.89 against the headset's 0.93, where the full
// 16:9 frame was 1.78 and needed most of its horizontal field thrown away.
//
// ---- re-entrancy ----
//
// The draw hook calls the original draw twice and uploads constants between them. Those
// uploads must NOT re-enter the injection path, or each would be offset again. One flag,
// checked first in both hooks.

bool         g_simulStereo = false;          // F10 toggles; alternate-eye remains the fallback
static bool  g_inDupDraw = false;            // re-entrancy guard
float        g_sceneMat[16] = { 0 };         // last scene matrix seen at c0, UNMODIFIED
bool         g_sceneMatValid = false;
// True while c0 currently holds the scene view matrix rather than a foreign one.
bool         g_c0IsScene = false;
volatile LONG g_dupDraws = 0;
volatile LONG g_dupFrameDraws = 0;

// ---- only duplicate draws aimed at a SCENE-SIZED render target ----
//
// From the reference's run 9, which diagnosed exactly this symptom:
//
//   "Splitting EVERY draw also split draws aimed at render targets that have nothing to do
//    with the eyes: shadow maps above all. Rendering the shadow scene twice into two halves of
//    a shadow map leaves it holding two squashed copies, each covering half its width. Objects
//    then sample garbage shadow data, which reads exactly as distant surfaces going dark and
//    recovering when the view changes and the cascade is rebuilt."
//
// The c0 gate added last round is necessary and not sufficient: a shadow pass can legitimately
// use the scene matrix while rendering somewhere that is not the scene. The viewport we set is
// also computed from the BACKBUFFER width, so aiming it at a differently-sized target is wrong
// twice over.
//
// ⚠️ The reference's run 27 records that size equality alone is NOT enough - UE3 allocates
// whole-scene dominant shadow maps at scene resolution, so a full-res offscreen target passes a
// size test while being nothing of the kind. Keying on the surface pointer is the fix there.
// This starts with size because it is cheap and catches the smaller maps; the census below
// exists to show whether anything full-resolution is still slipping through.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
static PFN_SetRenderTarget g_origSetRenderTarget = nullptr;
bool  g_rtIsScene = true;

RtSeen        g_rtSeen[16]{};
int           g_rtSeenCount = 0;
static RtSeen* g_rtCurrent = nullptr;
int            g_dupOnlyTarget = -1;   // -1 = every scene-sized target; else an index

static HRESULT STDMETHODCALLTYPE Hook_SetRenderTarget(IDirect3DDevice9* dev, DWORD idx,
                                                      IDirect3DSurface9* surf)
{
    if (idx == 0) {
        g_rtIsScene = true;          // a null target restores the backbuffer
        g_rtCurrent = nullptr;
        if (surf) {
            D3DSURFACE_DESC d{};
            if (SUCCEEDED(surf->GetDesc(&d))) {
                // Against the SCENE size, which is the backbuffer until something proves it is
                // not - see AdoptSceneTarget. Identical to the old test in every configuration
                // where the two agree, which is every one that has ever worked.
                const UINT sw = g_sceneW ? g_sceneW : g_capW;
                const UINT sh = g_sceneH ? g_sceneH : g_capH;
                g_rtIsScene = (d.Width == sw && d.Height == sh);
                // Census keyed on the SURFACE, not on its descriptor: several distinct targets
                // can share a size and format, and merging them is what hid the problem in the
                // reference for eighteen runs.
                for (int i = 0; i < g_rtSeenCount; ++i)
                    if (g_rtSeen[i].surf == surf) { g_rtCurrent = &g_rtSeen[i]; break; }
                if (!g_rtCurrent && g_rtSeenCount < 16) {
                    g_rtSeen[g_rtSeenCount] = { surf, d.Width, d.Height, d.Format, 0 };
                    g_rtCurrent = &g_rtSeen[g_rtSeenCount++];
                }
            }
        }
    }
    return g_origSetRenderTarget(dev, idx, surf);
}

// ---- ⚠️ A FALLBACK, NOT A REPLACEMENT ----
//
// This may only change anything in a configuration that is ALREADY broken, and the rule enforces
// it: if any backbuffer-sized target is taking scene draws, the existing behaviour is working and
// nothing here fires. Only when no such target is being drawn to at all - the letterboxed case,
// where the backbuffer-sized surface sits at zero - does it look for where the scene really went.
//
// Written this way round on purpose. "Adopt the target with the most scene draws" is a
// better-sounding rule that could adopt a full-resolution shadow map and break a configuration
// that works today - the reference's run 27 records exactly that hazard, UE3 allocating dominant
// shadow maps at scene resolution.
//
// ⚠️ Judged over THIS window only, and the counters are cleared on the way out. Lifetime totals
// looked simpler and are wrong twice: a backbuffer-sized target that took scene draws once at a
// menu would veto adoption for the rest of the run, and an adoption made under one resolution
// could never be undone after a Reset changed the backbuffer under it. Both are the same mistake
// - deciding a live question from a record that only accumulates - so the decision is made afresh
// and can go back as easily as forward.
static void AdoptSceneTarget()
{
    if (!g_capW || !g_capH) { return; }

    int  best = -1;
    long bestScene = 0;
    long bestBackbufferScene = 0;
    for (int i = 0; i < g_rtSeenCount; ++i) {
        const long sd = g_rtSeen[i].sceneDraws;
        if (g_rtSeen[i].w == g_capW && g_rtSeen[i].h == g_capH) {
            if (sd > bestBackbufferScene) bestBackbufferScene = sd;
        }
        else if (sd > bestScene) { bestScene = sd; best = i; }
    }
    for (int i = 0; i < g_rtSeenCount; ++i) g_rtSeen[i].sceneDraws = 0;

    // A handful of matrices on a full-size shadow/composite target is not evidence that the
    // scene moved back. Run 2 had 10 such draws against 177,092 on the real 1280x720 scene and
    // the old any-nonzero rule oscillated the target until stereo disengaged. The test must
    // also be competitive in BOTH directions: run 13 measured the SPLIT mode, where the world
    // renders into a 1280x720 scene buffer (151,275 scene draws per window) while the backbuffer
    // still legitimately carries the first-person arms and post passes (61,099). The old
    // backbuffer-times-4 test kept stereo on the backbuffer there, so only its minority draws
    // were duplicated and the right eye accumulated them over never-repainted pixels - the
    // run-13 smear trails. Backbuffer ownership now requires PARITY with the busiest offscreen
    // candidate; adoption below requires a decisive 2x majority; between the two bands the
    // current choice holds, which is the hysteresis that prevents oscillation.
    const long kMinimumSceneDraws = 200;
    const bool backbufferIsLive = bestBackbufferScene >= kMinimumSceneDraws &&
        (bestScene < kMinimumSceneDraws || bestBackbufferScene >= bestScene);

    // The original premise holds: the scene is going where it always did. Revert if we had
    // previously followed it elsewhere, so a resolution change cannot strand us on a stale size.
    if (backbufferIsLive) {
        if (g_sceneW || g_sceneH) {
            Log("*** [rt] the scene is back in the backbuffer (%ux%u) - dropping the override",
                g_capW, g_capH);
            g_sceneW = g_sceneH = 0;
        }
        if (g_sceneSplitMono) {
            g_sceneSplitMono = false;
            Log("*** [rt] the world renders into the backbuffer again - stereo duplication"
                " resumed");
        }
        return;
    }

    // Enough draws to be a scene pass rather than a stray probe or a two-triangle blit.
    if (best < 0 || bestScene < kMinimumSceneDraws) return;
    // Adoption needs a decisive majority over the backbuffer, not a merely busier offscreen
    // pass. Both measured split windows clear this comfortably (2.47x and 2.57x).
    if (bestScene < bestBackbufferScene * 2) return;

    const UINT w = g_rtSeen[best].w, h = g_rtSeen[best].h;
    // Side-by-side duplication needs the scene surface to share the backbuffer's double-wide
    // geometry. Run 14 adopted a genuinely MONO half-size scene buffer and the per-eye
    // viewports fought the game's own full-width draws: lower resolution AND the smear, then
    // mono, then a crash, across three checkpoint loads. A size-matched candidate is followed
    // as designed; a mismatched one means the game is in a reduced-resolution scene mode where
    // the only coherent presentation is mono, with backbuffer duplication suppressed so the
    // right eye cannot accumulate the leftovers.
    if (w != g_capW || h != g_capH) {
        if (!g_sceneSplitMono) {
            g_sceneSplitMono = true;
            Log("");
            Log("*** [rt] the world renders into a %ux%u buffer while the backbuffer is %ux%u."
                " Side-by-side stereo cannot follow a half-size mono scene; presenting MONO"
                " with duplication suppressed so the eyes stay coherent.",
                w, h, g_capW, g_capH);
            // The trigger hunt: run 20's onset had both arms hanging 0.84 m below the head
            // with the shoulders dragged ~24 UU - a maximally stretched 1p mesh. Snapshot the
            // hand state at every onset so the posture correlation proves or disproves itself
            // across natural occurrences without a dedicated test session.
            Log("[rt] onset hand state: L det=%d excess=%.1f dist=%.1f |"
                " R det=%d excess=%.1f dist=%.1f  (UU; dist is socket-to-target)",
                g_leftDetach.reachEngaged ? 1 : 0, g_leftDetach.diagnosticExcess,
                g_leftDetach.diagnosticDistance,
                g_rightDetach.reachEngaged ? 1 : 0, g_rightDetach.diagnosticExcess,
                g_rightDetach.diagnosticDistance);
        }
        return;
    }

    if (w == g_sceneW && h == g_sceneH) return;
    g_sceneW = w; g_sceneH = h;
    Log("");
    Log("*** [rt] the scene is not being rendered into the backbuffer. Backbuffer is %ux%u,"
        " the scene goes to %ux%u (%ld draws with the scene matrix this window).",
        g_capW, g_capH, w, h, bestScene);
    Log("[rt] following it there, so stereo works instead of silently falling back to mono.");
}

static void ReportRenderTargets()
{
    const UINT sw = g_sceneW ? g_sceneW : g_capW;
    const UINT sh = g_sceneH ? g_sceneH : g_capH;
    Log("[rt] distinct render targets seen this window:");
    for (int i = 0; i < g_rtSeenCount; ++i) {
        Log("[rt]   %p  %4ux%-4u fmt %-3d  %8ld draws (%ld scene)  %s",
            (void*)g_rtSeen[i].surf, g_rtSeen[i].w, g_rtSeen[i].h, (int)g_rtSeen[i].fmt,
            g_rtSeen[i].draws, g_rtSeen[i].sceneDraws,   // sceneDraws is a live window, not a total
            (g_rtSeen[i].w == sw && g_rtSeen[i].h == sh) ? "<- scene-sized, DUPLICATED" : "");
        g_rtSeen[i].draws = 0;
    }
}

// ---- Clear census, for the run-16 trails ----
//
// The trails look like stale colour or depth surviving between frames: ghost copies of exactly
// the nearest, highest-contrast content (arms, billboards) stacked along their motion history.
// Whether the game's clears still cover the main target - and with which flags - while the
// half-res effect pass is active is a measurement the mod never made. Bucketed by the target
// bound at the call and the clear flags; a partial (rect-limited) clear is marked in the key,
// because a clear that no longer covers the full surface is precisely the suspected defect.
typedef HRESULT (STDMETHODCALLTYPE *PFN_Clear)(IDirect3DDevice9*, DWORD, const D3DRECT*,
                                               DWORD, D3DCOLOR, float, DWORD);
static PFN_Clear g_origClear = nullptr;
struct ClearBucket { UINT w, h; DWORD key; long count; };
static ClearBucket g_clearBuckets[12];
static int g_clearBucketCount = 0;

static HRESULT STDMETHODCALLTYPE Hook_Clear(IDirect3DDevice9* dev, DWORD rectCount,
                                            const D3DRECT* rects, DWORD flags,
                                            D3DCOLOR color, float z, DWORD stencil)
{
    const UINT w = g_rtCurrent ? g_rtCurrent->w : 0;
    const UINT h = g_rtCurrent ? g_rtCurrent->h : 0;
    const DWORD key = flags | (rectCount ? 0x80000000ul : 0ul);
    int slot = -1;
    for (int i = 0; i < g_clearBucketCount; ++i)
        if (g_clearBuckets[i].w == w && g_clearBuckets[i].h == h &&
            g_clearBuckets[i].key == key) { slot = i; break; }
    if (slot < 0 && g_clearBucketCount < 12) {
        slot = g_clearBucketCount++;
        g_clearBuckets[slot] = { w, h, key, 0 };
    }
    if (slot >= 0) ++g_clearBuckets[slot].count;
    return g_origClear(dev, rectCount, rects, flags, color, z, stencil);
}

static void ReportClears()
{
    Log("[clear] Clear calls this window by bound target"
        " (flags: 1=target 2=zbuffer 4=stencil, R=rect-limited):");
    for (int i = 0; i < g_clearBucketCount; ++i) {
        Log("[clear]   %4ux%-4u flags=0x%lX%s  x%ld",
            g_clearBuckets[i].w, g_clearBuckets[i].h,
            (unsigned long)(g_clearBuckets[i].key & 0x7FFFFFFFul),
            (g_clearBuckets[i].key & 0x80000000ul) ? " R" : "",
            g_clearBuckets[i].count);
        g_clearBuckets[i].count = 0;
    }
}

// ---- hypothesis 2: OCCLUSION QUERIES, and why it is at least as likely as shadows ----
//
// UE3 wraps draws in occlusion queries and culls objects whose visible-pixel count comes back
// at or near zero. Draw duplication changes what those queries measure: each object is now
// rendered into two HALF-WIDTH viewports, so the count the engine reads is not the one it
// would have got, and objects get culled that should not be. That reads as things flickering
// in and out - which is the reported symptom, and it fits at least as well as the shadow-map
// theory does.
//
// The reference hit this at run 30 and recorded the right way to test it:
//
//   "Override the answer, not the availability. Refusing to create the occlusion query crashed
//    the game; patching GetData to lie ran the identical experiment with the engine's control
//    flow untouched. Prefer the intervention that leaves the program on its normal path."
//
// So DELETE makes every occlusion query report a large visible count. Nothing is culled, the
// engine's control flow is untouched, and if the flickering stops the cause is settled.
//
// All queries created by one device share a vtable, so a single patch covers every query the
// game will ever make.
typedef HRESULT (STDMETHODCALLTYPE *PFN_QueryGetData)(IDirect3DQuery9*, void*, DWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateQuery)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
static PFN_QueryGetData g_origQueryGetData = nullptr;
static PFN_CreateQuery  g_origCreateQuery = nullptr;
static bool  g_queryPatched = false;
static long  g_queriesFaked = 0;

// ---- the mode the reference settled on, adopted rather than rediscovered ----
//
//   0 = AUTO   override while draw duplication is running, normal otherwise   <- default
//   1 = always report visible
//   2 = never override; the engine's culling is always live
//
// AUTO because duplication is the configuration whose split frame invalidates the query, and
// mono has no reason to pay for disabled culling. It also means the safe fallback keeps the
// engine's own culling and its speed.
//
// The reference also records a mode 3 - refuse to create the queries at all - which CRASHED
// that build. Not implemented here, deliberately, so it cannot be retried by accident.
//
// ⚠️ And it records that even with the override its real mechanism was never identified: the
// leading hypothesis there is that occlusion boxes arrive via DrawPrimitiveUP, which is
// unhooked, so they draw at FULL-FRAME coordinates against a depth buffer holding half-remapped
// geometry. If forcing visible does not stop the flickering here, that is the next thing to
// test - DrawPrimitiveUP is slot 83 and DrawIndexedPrimitiveUP is 84.
int          g_occlusionMode = 0;
bool         g_forceVisible = false;      // DELETE forces mode 1 for an A/B

static bool OverrideOcclusion()
{
    if (g_forceVisible || g_occlusionMode == 1) return true;
    if (g_occlusionMode == 2) return false;
    return InterlockedCompareExchange(&g_dupDraws, 0, 0) != 0;   // AUTO
}

static HRESULT STDMETHODCALLTYPE Hook_QueryGetData(IDirect3DQuery9* q, void* pData,
                                                   DWORD size, DWORD flags)
{
    const HRESULT hr = g_origQueryGetData(q, pData, size, flags);
    // Only rewrite an answer the runtime actually produced: S_FALSE means "not ready" and
    // carries no data. And ONLY for occlusion queries - all of a device's queries share one
    // vtable, so EVENT queries (fences) come through here too, and the first version of this
    // would have overwritten their results with a pixel count. The reference checks the type
    // per call for exactly that reason.
    if (OverrideOcclusion() && hr == S_OK && pData && size >= sizeof(DWORD) &&
        q->GetType() == D3DQUERYTYPE_OCCLUSION) {
        *(DWORD*)pData = 0x00100000;      // "plenty of pixels visible"
        // Logged on the first override and then periodically, so the log can say whether it was
        // active during any given stretch of a run. A once-only message cannot answer that,
        // which is why the previous run could not establish what changed or when.
        // 20000 was too fine: at ~1800 queries a second it filled the log with 180 identical
        // lines in one run and buried the [fov] counts that mattered. The question it answers -
        // is the override live, and for how much of the run - is answered just as well at
        // 500000, and the log is a diagnostic tool only for as long as it can be read.
        if (++g_queriesFaked == 1 || (g_queriesFaked % 500000) == 0)
            Log("[occ] overriding to VISIBLE (mode %d, %ld queries faked)",
                g_occlusionMode, g_queriesFaked);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateQuery(IDirect3DDevice9* dev, D3DQUERYTYPE type,
                                                  IDirect3DQuery9** out)
{
    const HRESULT hr = g_origCreateQuery(dev, type, out);
    if (SUCCEEDED(hr) && out && *out && type == D3DQUERYTYPE_OCCLUSION && !g_queryPatched) {
        g_queryPatched = true;
        g_origQueryGetData = (PFN_QueryGetData)PatchVTable(*out, 7, (void*)&Hook_QueryGetData);
        Log("[occ] occlusion query vtable patched (GetData slot 7), original=%p",
            (void*)g_origQueryGetData);
    }
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrim)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexed)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT,
                                                     UINT, UINT, UINT, UINT);
static PFN_DrawPrim    g_origDrawPrim = nullptr;
static PFN_DrawIndexed g_origDrawIndexed = nullptr;

// ---- head ROLL, applied in clip space ----
//
// Roll is the one axis of the head pose that never reached the game: the controller write owns
// pitch and yaw, and UpdateRotation zeroes ViewRotation.Roll before writing it back. Tilting
// your head sideways left the horizon glued to the headset instead of staying level.
//
// ⚠️ AND IT CANNOT BE LEFT TO THE COMPOSITOR, which is the trap. The projection layer already
// carries the head's full orientation, roll included, so it looks like the runtime should
// rotate the image for us. It does not: a projection layer is reprojected by the DELTA between
// the pose we claim and the pose the display is at. Claim a rolled pose for an unrolled render
// while the head is at that same roll, the delta is zero, and the image presents straight.
// Baking roll into the render is what makes the claimed pose true, and the two then agree.
//
// ⚠️ The frustum is NOT square - tanX and tanY differ, and under duplication tanX is the
// half-width per-eye value. Rotating the raw clip columns would SHEAR. So convert to the
// symmetric view-space direction, rotate there, and convert back:
//
//     x_v = clip.x * tanX,  y_v = clip.y * tanY      (aspect removed)
//     rotate by theta
//     clip.x' = x_v' / tanX,  clip.y' = y_v' / tanY
//
// which collapses to a mix of the two columns with the aspect ratio as the cross term.
//
// Applied AFTER the forced projection, so the tangents read here are the frustum the eye
// actually sees rather than whatever the engine happened to send. It composes with the
// positional offset in either order - an offset is a pre-multiplication in world space, a roll
// is a post-multiplication in clip space, so they commute.
// ---- 6-DOF: the headset's physical translation, applied to the view matrix ----
//
// Same injection as the per-eye offset, from a different source. The subtlety is which frame
// the translation is expressed in.
//
// OpenXR reports the head position in LOCAL space, which is fixed to the room and does not
// rotate with the head. Applying that directly along the game's world axes would be wrong the
// moment the player turns: leaning left would move the camera along a fixed world direction
// rather than along the camera's own left.
//
// So the offset is converted into HEAD-LOCAL coordinates first - rotate the room-space delta by
// the conjugate of the head orientation - and then applied along the camera's own right, up and
// forward axes, read out of the matrix exactly as the per-eye offset reads its right axis. The
// game camera's orientation tracks the head's, so head-local and camera-local coincide.
//
// Metres to UE3 units uses the measured 100 UU/m, so a 30 cm lean is 30 units. That figure is
// derived from the game's own movement speeds and is not a taste setting.
//
// ⚠️ Failure must DECAY, not freeze. The reference records losing a run to this: positional
// tracking dropped while orientation kept working - the IMU carries rotation, position needs
// the cameras - the update was skipped, and the last offset persisted forever, leaving the view
// about 0.9 m from where it belonged with no way back. Skipping looked like the safe choice and
// was the worst one. Here the offset decays toward neutral whenever the position bit is clear.
bool         g_sixDof = true;
bool         g_haveCentre = false;
static float g_centre[3] = { 0, 0, 0 };
float        g_dofOffset[3] = { 0, 0, 0 };   // head-local, UE3 units
static bool  g_haveLastSixDofPosition = false;
static XrVector3f g_lastSixDofPosition{};

void RecenterSixDof()
{
    g_haveCentre = false;
    g_haveLastSixDofPosition = false;
    // The old offset must not survive the frame in which PAGE UP is pressed. The next valid head
    // pose becomes the new centre; until then both the camera and hands use the neutral anchor.
    g_dofOffset[0] = g_dofOffset[1] = g_dofOffset[2] = 0.0f;
    InterlockedIncrement(&g_motionRecenterSerial);
}

void UpdateSixDof(XrTime when)
{
    if (g_viewSpace == XR_NULL_HANDLE || g_xrSpace == XR_NULL_HANDLE) return;
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(g_viewSpace, g_xrSpace, when, &loc))) return;

    const bool posOk = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                       (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    if (!posOk || !g_sixDof) {
        // Decay home over roughly a second rather than holding a stale offset.
        for (int i = 0; i < 3; ++i) g_dofOffset[i] *= 0.97f;
        g_haveLastSixDofPosition = false;
        return;
    }

    const XrVector3f p = loc.pose.position;
    if (!g_haveCentre) {
        g_centre[0] = p.x; g_centre[1] = p.y; g_centre[2] = p.z;
        g_lastSixDofPosition = p;
        g_haveLastSixDofPosition = true;
        g_haveCentre = true;
        Log("[6dof] centre set at (%.3f, %.3f, %.3f) m", p.x, p.y, p.z);
        return;
    }

    // POSITION_VALID is not a continuity guarantee. On the reported spazz frame OpenXR moved
    // the head 1.31 metres between adjacent 120 Hz samples while leaving the valid bit set, and
    // both controller targets inherited that exact jump. A human head cannot translate 25 cm
    // in one frame. Treat a larger step as the tracking origin being reacquired: move the centre
    // by the same amount so the rendered offset is continuous in the new coordinate frame.
    if (g_haveLastSixDofPosition) {
        const float sx = p.x - g_lastSixDofPosition.x;
        const float sy = p.y - g_lastSixDofPosition.y;
        const float sz = p.z - g_lastSixDofPosition.z;
        const float step = sqrtf(sx*sx + sy*sy + sz*sz);
        if (step > 0.25f) {
            g_centre[0] += sx; g_centre[1] += sy; g_centre[2] += sz;
            Log("*** [6dof] rejected %.2f m one-frame head-position discontinuity;"
                " tracking centre rebased without moving camera/hands", step);
        }
    }
    g_lastSixDofPosition = p;
    g_haveLastSixDofPosition = true;

    const float dx = p.x - g_centre[0], dy = p.y - g_centre[1], dz = p.z - g_centre[2];

    // ---- decompose in a LEVEL, YAW-ONLY frame ----
    //
    // ⚠️ The first version rotated the delta by the conjugate of the full head orientation and
    // applied the result on the camera's own right/up/forward. That is wrong twice, and both
    // errors were visible:
    //
    //   ROLL. The head-local frame rolls with the head; the game camera does not - roll is an
    //   image rotation applied later, so the matrix's axes stay level. Decomposing in a rolled
    //   frame and recomposing on a level one rotates the whole offset by the roll angle, which
    //   turns the small sideways motion of a head tilt into a vertical one. That is the reported
    //   "roll right and the height goes up".
    //
    //   PITCH. Applying the vertical component along the CAMERA's up means crouching while
    //   looking down moves the view down AND backwards. Standing up is vertical in the world no
    //   matter where you are looking.
    //
    // So: strip head YAW only, keeping the horizontal plane horizontal, and let the apply site
    // put the vertical component on the world's up axis. Yaw is the only part that has to be
    // removed, because yaw is the only part the room and the game world disagree about.
    static float hfx = 0.0f, hfz = -1.0f;      // head forward, levelled, room space
    {
        float f[3], u[3];
        HeadBasis(loc.pose.orientation, f, u);
        const float fx = f[0], fz = f[2];
        const float len = sqrtf(fx * fx + fz * fz);
        // Looking straight up or down: the levelled forward collapses and its direction is
        // meaningless. Hold the last good one rather than let it spin.
        if (len > 0.05f) { hfx = fx / len; hfz = fz / len; }
    }
    // Right = forward x up, with OpenXR's up = (0,1,0). Levelled, so it carries no roll.
    const float hrx = -hfz, hrz = hfx;

    g_dofOffset[0] = (dx * hrx + dz * hrz) * g_worldScale;   // right
    g_dofOffset[1] =  dy                   * g_worldScale;   // up, true vertical
    g_dofOffset[2] = (dx * hfx + dz * hfz) * g_worldScale;   // forward

    // ---- diagnostic: how much does the room-to-world yaw offset wobble? ----
    //
    // ⚠️ The first version of this measured the wrong thing, and its answer read as alarming
    // when it was not. It tracked both (camYaw - headYaw) and (camYaw + headYaw) intending the
    // stable one to reveal whether the two frames ran in opposite directions. The SUM came back
    // stable - but that is the EXPECTED result for a correct frame, because the two angles were
    // defined with opposite senses and the test could not have said otherwise.
    //
    // UE3 lays out X forward and Y right, so seen from above atan2(fwd.y, fwd.x) grows CLOCKWISE.
    // OpenXR has X right and -Z forward, so its yaw grows ANTICLOCKWISE. A single physical turn
    // to the right raises one angle and lowers the other. Two mirror-image conventions cannot
    // discriminate handedness; they only ever agree on the sum.
    //
    // The offsets themselves never go through angles - they are decomposed and recomposed on
    // right/forward vectors, which carry their own sense - and that path checks out by hand:
    // standing 20 cm right of centre while facing north puts the offset 20 cm along the camera's
    // right. So the frame is correct, and what is left to measure is the WOBBLE.
    //
    // Which is what this now reports: the invariant against its own running mean, so a wrap
    // through 180 degrees cannot masquerade as a 350 degree swing the way it did before.
    if (g_sceneMatValid) {
        const float crx = g_vmRow ? g_sceneMat[0] : g_sceneMat[0];
        const float cry = g_vmRow ? g_sceneMat[4] : g_sceneMat[1];
        if (fabsf(crx) + fabsf(cry) > 1e-6f) {
            auto wrap = [](float a) {
                while (a >  3.14159265f) a -= 6.28318531f;
                while (a < -3.14159265f) a += 6.28318531f;
                return a;
            };
            // Sum, not difference: the invariant for these two conventions, established above.
            const float phi = wrap(atan2f(-crx, cry) + atan2f(hfx, hfz));
            static bool  have = false;
            static float mean = 0.0f, worst = 0.0f;
            static int   n = 0;
            if (!have) { mean = phi; have = true; }
            const float err = wrap(phi - mean);
            mean = wrap(mean + 0.02f * err);          // slow: the true value only moves on a turn
            if (fabsf(err) > worst) worst = fabsf(err);
            if (++n >= 600) {
                Log("[6dof] room-to-world yaw wobble: worst %.1f deg over %d frames"
                    "  (offset R%+.0f U%+.0f F%+.0f UU - the sway it causes is |offset| x sin(wobble))",
                    worst * 57.29578f, n,
                    g_dofOffset[0], g_dofOffset[1], g_dofOffset[2]);
                n = 0; worst = 0.0f;
            }
        }
    }
}

// Offsets the matrix by the current 6-DOF translation, along the camera's OWN axes taken from
// the matrix - the same source the per-eye offset uses, so the two cannot disagree.
static void ApplySixDof(float* m, bool rowStorage)
{
    if (!g_sixDof) return;
    const float ax = fabsf(g_dofOffset[0]) + fabsf(g_dofOffset[1]) + fabsf(g_dofOffset[2]);
    if (ax < 0.01f) return;

    auto col = [&](int c, int i) { return rowStorage ? m[i * 4 + c] : m[c * 4 + i]; };

    // Only the camera's RIGHT axis is read, and it is levelled before use. The camera's up and
    // forward are deliberately not used: the up component belongs on the WORLD's up axis (UE3
    // is Z-up, verified by the F2 injection test), and a levelled forward follows from right
    // and up by a cross product. Reading fewer axes means fewer that can be pitched or rolled
    // when they should not be.
    float rx = col(0,0), ry = col(0,1);          // camera right, world space; z dropped = levelled
    const float rl = sqrtf(rx*rx + ry*ry);
    if (rl < 1e-6f) return;                      // looking straight along the world up axis
    rx /= rl; ry /= rl;

    // forward = right x worldUp = (ry, -rx, 0)
    const float ox = rx*g_dofOffset[0] + ry*g_dofOffset[2];
    const float oy = ry*g_dofOffset[0] - rx*g_dofOffset[2];
    const float oz =                          g_dofOffset[1];

    if (rowStorage) {
        for (int c = 0; c < 4; ++c)
            m[12 + c] -= ox * m[0 + c] + oy * m[4 + c] + oz * m[8 + c];
    } else {
        for (int r = 0; r < 4; ++r)
            m[3 * 4 + r] -= ox * m[0 * 4 + r] + oy * m[1 * 4 + r] + oz * m[2 * 4 + r];
    }
}

// ---- rotate the camera about one of its own axes, in the matrix ----
//
// A world-space pre-multiplication, exactly like the 6-DOF translation, and pivoted on the
// camera position for the same reason: rotating about the world origin would swing the camera
// through an arc instead of turning it on the spot.
//
// Not the clip-space shear ApplyRoll uses. Roll leaves clip.z alone so mixing two columns is
// exact there; a pitch changes the view-space depth of everything, so the near/far mapping has
// to come along or the depth buffer stops agreeing with the picture. Rotating the WORLD before
// the projection gets that for free - the engine's own projection then does the depth mapping,
// whatever convention it uses, and nothing here has to know what that convention is.
//
//   clip = v M, and replacing rows 0..2 with R*rows gives clip = (v R) M: the world turns, so
//   the camera appears to turn the other way. Hence the transpose, which for a rotation is the
//   inverse. Row 3 absorbs the pivot.
static void ApplyCameraRotation(float* m, bool rowStorage, const float axis[3], float ang,
                                const float pivot[3])
{
    if (fabsf(ang) < 1e-4f) return;
    float ax = axis[0], ay = axis[1], az = axis[2];
    const float al = sqrtf(ax*ax + ay*ay + az*az);
    if (al < 1e-6f) return;
    ax /= al; ay /= al; az /= al;

    // Rodrigues at -ang, which is the transpose of the rotation by +ang.
    const float c = cosf(-ang), s = sinf(-ang), t = 1.0f - c;
    const float R[3][3] = {
        { t*ax*ax + c,     t*ax*ay - s*az,  t*ax*az + s*ay },
        { t*ax*ay + s*az,  t*ay*ay + c,     t*ay*az - s*ax },
        { t*ax*az - s*ay,  t*ay*az + s*ax,  t*az*az + c    }
    };

    auto at = [&](int row, int k) -> float& {
        return rowStorage ? m[row * 4 + k] : m[k * 4 + row];
    };

    float nr[3][4];
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 4; ++k)
            nr[i][k] = R[i][0]*at(0,k) + R[i][1]*at(1,k) + R[i][2]*at(2,k);

    // Pivot: v' = (v - p)R + p, so row 3 takes p*M - p*(RM).
    for (int k = 0; k < 4; ++k) {
        float d = 0.0f;
        for (int j = 0; j < 3; ++j) d += pivot[j] * (at(j,k) - nr[j][k]);
        at(3,k) += d;
    }
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 4; ++k) at(i,k) = nr[i][k];
}

static void ReportStereoGeometry()
{
    if (!g_viewsValid) return;

    float f0[3], u0[3], f1[3], u1[3];
    HeadBasis(g_views[0].pose.orientation, f0, u0);
    HeadBasis(g_views[1].pose.orientation, f1, u1);

    auto ang = [](const float a[3], const float b[3]) {
        float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
        if (d >  1.0f) d =  1.0f;
        if (d < -1.0f) d = -1.0f;
        return acosf(d);
    };
    const float cantFwd = ang(f0, f1);      // the eyes looking in different directions
    const float cantUp  = ang(u0, u1);      // and rolled relative to each other

    // The roll already present in what is SUBMITTED, measured the same way GetHeadRoll measures
    // the head's: world up with the forward component removed, then the signed angle to the
    // eye's own up, about forward.
    float submittedRoll = 0.0f;
    {
        const float d = f0[1];
        float lx = -d*f0[0], ly = 1.0f - d*f0[1], lz = -d*f0[2];
        const float ll = sqrtf(lx*lx + ly*ly + lz*lz);
        if (ll > 0.05f) {
            lx /= ll; ly /= ll; lz /= ll;
            const float cosA = lx*u0[0] + ly*u0[1] + lz*u0[2];
            const float sinA = (ly*u0[2] - lz*u0[1]) * f0[0]
                             + (lz*u0[0] - lx*u0[2]) * f0[1]
                             + (lx*u0[1] - ly*u0[0]) * f0[2];
            submittedRoll = atan2f(sinA, cosA);
        }
    }

    static float worstCantF = 0.0f, worstCantU = 0.0f, worstRollGap = 0.0f;
    static long  n = 0;
    if (cantFwd > worstCantF) worstCantF = cantFwd;
    if (cantUp  > worstCantU) worstCantU = cantUp;
    // The image carries g_headRoll and the layer carries submittedRoll. If both are live and
    // equal, the roll is being applied twice and this reads as the head roll itself.
    const float bothRoll = fabsf(submittedRoll) + fabsf(g_headRoll);
    if (bothRoll > worstRollGap) worstRollGap = bothRoll;

    if (++n >= 600) {
        // ⚠️ The hint this line used to carry - "equal and both live = applied TWICE" - was
        // wrong, and wrong in the worst way for a diagnostic: it proposed a conclusion the
        // measurement could not reach. Both figures derive from the same head orientation, so
        // they are equal BY CONSTRUCTION and would be equal whether or not anything was doubled.
        // They say nothing about what the compositor does.
        //
        // What the pair IS good for is timing. The image carries the roll sampled when it was
        // drawn; the pose carries the roll at submit. A projection layer reprojects by the
        // difference, so a gap here is a frame of roll the compositor will correct against.
        Log("[eye] stereo geometry over %ld frames: eye cant fwd %.3f deg, up %.3f deg"
            "  |  roll: image %.2f deg, submitted pose %.2f deg  (these SHOULD match - a gap is"
            " a frame of roll the compositor reprojects against)",
            n, worstCantF * 57.29578f, worstCantU * 57.29578f,
            g_headRoll * 57.29578f, submittedRoll * 57.29578f);
        n = 0; worstCantF = worstCantU = worstRollGap = 0.0f;
    }
}

// Called once per Present. Buckets the interval since the last one by display periods.
static double g_lastPresentMs = 0.0;

// ⚠️ This used to bucket the interval by whole display periods, and that measurement was lying
// in the one direction that mattered.
//
// Rounding put everything from 0.5 to 1.5 periods into bucket 1 - 4.2 ms to 12.5 ms, which is
// 80 fps through 240 fps. At 60 into 120 that was fine and it correctly showed one bucket. At the
// 95-100 fps this now runs at, EVERY frame lands in bucket 1 whatever the jitter, and it printed
// "one bucket = even" directly beside an fps line reading 95.9. Both cannot be true.
//
// And 100 into 120 is the bad case, not a good one: a ratio of 1.2 means four frames shown once
// and every fifth shown twice, a beat at 20 Hz. That is worse than 60 into 120, and it was
// invisible to the metric built to find exactly it.
//
// So the interval is reported directly now, in milliseconds, with the count of frames that
// MISSED the display's cadence. No rounding, nothing to hide behind.
static double g_paceMin = 1e9, g_paceMax = 0.0, g_paceSum = 0.0;
static long   g_paceN = 0, g_paceLate = 0;

static void TickPacing()
{
    const double now = NowMs();
    if (g_lastPresentMs > 0.0 && g_predPeriod) {
        const double dt = now - g_lastPresentMs;
        const double periodMs = (double)g_predPeriod / 1.0e6;
        if (dt > 0.0 && dt < 1000.0 && periodMs > 0.1) {
            if (dt < g_paceMin) g_paceMin = dt;
            if (dt > g_paceMax) g_paceMax = dt;
            g_paceSum += dt;
            g_paceN++;
            // ⚠️ UNEVEN, not "slower than the display", and the distinction is the whole point.
            //
            // The first version counted any frame longer than one display period. At a locked 60
            // into 120 every frame is longer than one period, so it reported 100% - beside a
            // setting that had just been confirmed as the SMOOTHEST available. It was flagging
            // the good case as the worst one.
            //
            // Evenness is what the eye reacts to, not speed. A steady 16.67 ms is comfortable; a
            // mean of 13 ms wandering between 7.5 and 18.8 is not, and it is faster. So this
            // measures the deviation from the run's OWN recent mean, which is agnostic about
            // what rate the game is holding and only asks whether it is holding it.
            const double mean = g_paceSum / (double)g_paceN;
            if (fabs(dt - mean) > periodMs * 0.25) g_paceLate++;
        }
    }
    g_lastPresentMs = now;
}

// ⚠️ ReportPoseHonesty was here and is deliberately gone. Recorded because the mistake is worth
// more than the function was.
//
// It compared the CHANGE in head yaw against the change in the image's yaw, and reported the
// difference as "how much the compositor is being lied to". During a steady turn the head moves
// two degrees and the image moves two degrees, one frame apart - identical changes - so it
// reported zero while the view sat five degrees behind. It was measuring the derivative of the
// error and being read as a measure of the error.
//
// It ran for three rounds and its zero was quoted as an all-clear each time, including by me,
// while the thing it was supposed to detect was the actual fault. A metric that cannot see a
// constant offset should not be phrased as though a zero means there is none.
//
// The invariant measurement in the constant hook answers the same question without the blind
// spot: it reports the offset itself.

static void ReportPacing()
{
    if (!g_paceN) return;
    const double periodMs = g_predPeriod ? ((double)g_predPeriod / 1.0e6) : 0.0;
    Log("[xr] pacing over %ld frames: %.2f ms mean (%.0f fps), spread %.2f to %.2f  |  display"
        " period %.2f ms  |  %ld frames UNEVEN (%.1f%%)   <- unevenness is the judder; a steady"
        " slow rate beats a wandering fast one",
        g_paceN, g_paceSum / (double)g_paceN, 1000.0 / (g_paceSum / (double)g_paceN),
        g_paceMin, g_paceMax, periodMs,
        g_paceLate, 100.0 * (double)g_paceLate / (double)g_paceN);
    g_paceMin = 1e9; g_paceMax = 0.0; g_paceSum = 0.0; g_paceN = 0; g_paceLate = 0;
}

// The point every matrix rotation turns about. Prefers the position read inside the frame; falls
// back to the Present-sampled one, which is a frame stale but better than the world origin.
// Defined here, above all four users, rather than beside the one that needed it last.
static const float* RotationPivot()
{
    return g_livePivotValid ? g_livePivot : g_camCache;
}

// ---- correct the pitch in the matrix instead of waiting for the engine ----
//
// Writing pitch into Controller.Rotation happens once, in Present, and the engine has already
// rendered by then - so every disturbance shows for at least one frame before the write can
// answer it, and with the animation cancellation relaxed for stability it takes a fifth of a
// second more. That is the reported "rotates slightly before snapping back to the middle",
// whether the disturbance came from the mouse or from an animation starting.
//
// The write cannot be made faster; there is no hook between the engine's camera update and its
// render. But the matrix is read on the render thread, after all of it, so the error is knowable
// exactly where it can still be fixed: compare the pitch the matrix actually carries against the
// pitch it was supposed to have, and rotate away the difference.
//
// The write still happens and still matters - Controller.Rotation is what the GAME uses for
// aiming, movement and everything else that reads where the player is looking. This corrects
// only what is SEEN, which is the part that was a frame late.
static void ApplyPitchFix(float* m, bool rowStorage)
{
    if (!g_pitchFix || !g_pitchTargetValid || !g_camCacheValid || !g_pitchAbsolute) return;

    auto col = [&](int c, int i) { return rowStorage ? m[i * 4 + c] : m[c * 4 + i]; };

    // Camera forward is column 3; UE3 is Z-up, so its z component is the sine of the pitch.
    const float fx = col(3,0), fy = col(3,1), fz = col(3,2);
    const float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    if (fl < 1e-6f) return;
    float sinP = fz / fl;
    if (sinP >  1.0f) sinP =  1.0f;
    if (sinP < -1.0f) sinP = -1.0f;

    // The animation's share, from two values that both belong to THIS frame: the pitch the
    // matrix carries, and the controller's pitch read beside it. Their difference is by
    // definition everything the engine added on top of what was written - the camera animation
    // and the swan neck - with no sample from a previous frame anywhere in it.
    //
    // Followed, that share is part of where the view belongs, and the correction reduces to
    // (head - controller): the mouse is cancelled and the animation is left alone. Cancelled,
    // the target is the head alone and the correction is (head - matrix), which takes out the
    // animation as well. Two exact expressions from one line, and neither can go stale.
    const float matPitch = asinf(sinP);
    float anim = (g_animFollow && g_liveCtlValid) ? (matPitch - g_liveCtlPitch) : 0.0f;

    // ⚠️ This guard was CAUSING the failure it was written to prevent, and the fallback was the
    // reason. Zeroing the term does not mean "do less"; with animations followed the correction
    // is (target + anim - matrix), so setting anim to zero leaves (target - matrix) - which
    // during a steep animation is the whole animation, forty or fifty degrees of it, applied as
    // a rotation. The guard fired and then yanked the view further than anything it was
    // protecting against.
    //
    // Bailing out is the correct degradation. It leaves the engine's own view alone, which is
    // never catastrophic. Correcting on a value already judged untrustworthy always can be.
    //
    // The bound moves 45 -> 90 as well, for the same reason the pitch clamp moved 20 -> 60: it
    // was set from an idea of what was reasonable rather than from measurement, and real
    // contributions exceed it. This run recorded -46 degrees with the matrix and the camera
    // rotation in agreement - the direction test now confirms they describe the same view, so a
    // contribution that large is REAL, and the zip line is exactly where it showed.
    const float kAnimMax = 90.0f * (3.14159265f / 180.0f);
    if (anim > kAnimMax || anim < -kAnimMax) {
        static long lastFrame = -1;
        if (lastFrame != g_frames) {
            lastFrame = g_frames;
            static long frames = 0;
            if (++frames == 1 || (frames % 60) == 0)
                Log("[head] animation pitch %.1f deg beyond trust (matrix %.1f, controller %.1f)"
                    " - correction SKIPPED, %ld frames so far", anim * 57.29578f,
                    matPitch * 57.29578f, g_liveCtlPitch * 57.29578f, frames);
        }
        return;
    }

    float err = (g_pitchTarget + anim) - matPitch;

    // ---- ⚠️ is this correction even looking at the rendered pitch? ----
    //
    // Substitute the definition of anim and the matrix cancels out:
    //
    //   err = (target + (matPitch - liveCtl)) - matPitch  =  target - liveCtl
    //
    // With animations followed - the default - this correction never reads what was RENDERED. It
    // compares the head against the CONTROLLER, and the controller is written absolutely from the
    // head every frame, so the two agree by construction and the correction is near zero whatever
    // the picture is doing. Any lag between the controller and the matrix - the engine's own
    // render pipeline, which is what yaw needed five degrees of correction for - is invisible to
    // it.
    //
    // That is consistent with pitch juddering while yaw does not: yaw is measured against the
    // matrix, pitch is measured against a value that cannot disagree with its target.
    //
    // Consistent is not proven, so this reports both. `rendered` is how far the view actually is
    // from where the head points; `applied` is what the correction decided to do about it. If
    // rendered is degrees and applied is nothing, the reasoning above is right and the fix is to
    // measure pitch the way yaw is measured.
    {
        static long lastFrame = -1;
        if (lastFrame != g_frames) {
            lastFrame = g_frames;
            static float wRend = 0.0f, wAppl = 0.0f, sRend = 0.0f;
            static long  n = 0;
            const float rendered = fabsf(g_pitchTarget - matPitch);
            if (rendered > wRend) wRend = rendered;
            if (fabsf(err) > wAppl) wAppl = fabsf(err);
            sRend += rendered;
            if (++n >= 600) {
                Log("[head] pitch: view is %.2f deg from the head on average, %.2f worst;"
                    " correction applied %.2f worst  (a big gap means the fix is blind)",
                    (sRend / (float)n) * 57.29578f, wRend * 57.29578f, wAppl * 57.29578f);
                n = 0; wRend = wAppl = sRend = 0.0f;
            }
        }
    }

    // Clamped hard. A foreign matrix that slipped the scene test, or a target computed from a
    // stale sample, must not be able to throw the view somewhere the head is not - and a real
    // error is never more than a degree or two.
    // ⚠️ 60 degrees, not 20, and the log is what corrected it.
    //
    // 20 was picked from "a real error is never more than a degree or two", which is true of the
    // one-frame lag this started as and false of what it now has to cancel. A hard landing dips
    // the camera about 40 degrees - measured: "wanted 39.6 deg (target -41.4, matrix -81.0)" -
    // so the correction was cut in half and the remainder is exactly the split-second look-down
    // that survived locking the pitch. The guard was eating the feature.
    //
    // 60 clears the largest contribution seen with room to spare while still catching a garbage
    // matrix, which misses by a lot more than a landing does.
    // Beyond this, SKIP rather than clamp - the same lesson as the guard above. Applying 60
    // degrees of rotation because 75 was asked for is not a smaller version of the right answer,
    // it is a large wrong one, and leaving the engine's view alone is the safe degradation.
    //
    // 60 still admits everything real: a hard landing needs about 40 with the animation locked,
    // and with it followed the correction is only the frame of lag, a degree or two.
    const float kMax = 60.0f * (3.14159265f / 180.0f);
    if (err > kMax || err < -kMax) {
        // Counted in FRAMES, not calls. This runs once per draw per eye, so a per-call counter
        // reported ten thousand occurrences for a handful of frames and made a momentary event
        // look permanent.
        static long lastFrame = -1;
        if (lastFrame != g_frames) {
            lastFrame = g_frames;
            static long frames = 0;
            if (++frames == 1 || (frames % 60) == 0)
                Log("[head] pitch correction %.1f deg beyond trust (target %.1f, matrix %.1f,"
                    " anim %.1f) - SKIPPED, %ld frames so far", err * 57.29578f,
                    g_pitchTarget * 57.29578f, matPitch * 57.29578f, anim * 57.29578f, frames);
        }
        return;
    }

    const float right[3] = { col(0,0), col(0,1), col(0,2) };
    ApplyCameraRotation(m, rowStorage, right, err, RotationPivot());
}

// ---- lock the camera's own roll out of the view ----
//
// A separate mechanism from the pitch, and that is why locking the pitch left it behind. Roll
// never travels through Controller.Rotation at all: the decompiled UpdateRotation sets
// ViewRotation.Roll = 0 before writing back, so nothing written there can carry roll and nothing
// written there can cancel it. The camera animation composes its roll further downstream, and
// the first place it can be seen or touched is the matrix.
//
// Which makes it easy to measure exactly. The controller contributes no roll, so ANY roll in the
// matrix is the animation's. A camera with no roll has a level right axis, so the z component of
// the right axis is the whole signal - divided by cos(pitch), because a pitched camera's right
// axis is level while its up axis is not, and the tilt has to be measured in the plane it
// actually happens in.
//
// Cancelled by rotating about the camera's own forward axis, which leaves the forward direction
// untouched - so this runs BEFORE the pitch correction and cannot disturb its measurement, while
// the reverse order would have changed the cos(pitch) this depends on.
static void ApplyRollFix(float* m, bool rowStorage)
{
    if (g_animRollFollow || !g_camCacheValid) return;

    auto col = [&](int c, int i) { return rowStorage ? m[i * 4 + c] : m[c * 4 + i]; };

    const float fx = col(3,0), fy = col(3,1), fz = col(3,2);
    const float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    const float rx = col(0,0), ry = col(0,1), rz = col(0,2);
    const float rl = sqrtf(rx*rx + ry*ry + rz*rz);
    if (fl < 1e-6f || rl < 1e-6f) return;

    const float sinP = fz / fl;
    const float cosP = sqrtf(1.0f - sinP * sinP);
    if (cosP < 0.05f) return;            // near vertical: roll and yaw are the same thing there

    float s = (rz / rl) / cosP;
    if (s >  1.0f) s =  1.0f;
    if (s < -1.0f) s = -1.0f;

    const float fwd[3] = { fx, fy, fz };
    ApplyCameraRotation(m, rowStorage, fwd, asinf(s), RotationPivot());
}

// ---- take out the head turn the engine has not caught up with yet ----
//
// This is the judder. The game's camera is driven by writes made in Present, so the yaw it
// renders with belongs to the previous frame's head pose; by the time the frame reaches the
// headset the head has turned further, and the world appears to drag behind the turn. It scales
// with turn rate, which is why it is invisible when still and obvious when looking around.
//
// The write cannot be made to arrive sooner. But the size of the gap is knowable - measured
// above by asking the runtime for the head pose at two times - and the matrix can simply be
// turned by it. Same primitive as the animation locks, about world up for the same reason.
static void ApplyYawLag(float* m, bool rowStorage)
{
    if (!g_yawLagFix || fabsf(g_yawLagRad) < 1e-5f) return;
    if (!g_livePivotValid && !g_camCacheValid) return;
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    // g_yawLagRad is the deviation of (matrix yaw + head yaw) from its own mean. A positive
    // deviation means the matrix's yaw is that much too high for where the head now is, so the
    // camera's yaw has to come DOWN by it - and a positive angle here lowers the rotator's yaw.
    // Passed straight through, where the prediction version had to be negated because it named
    // a head movement rather than a matrix error.
    ApplyCameraRotation(m, rowStorage, up, g_yawLagRad, RotationPivot());
}

// ---- lock the animation's yaw out of the view ----
//
// Third axis, third mechanism. Yaw DOES travel through Controller.Rotation, like pitch, so what
// the animation added is the difference between the yaw the matrix carries and the yaw the
// controller holds - both read from this same frame, the same construction the pitch fix uses.
//
// Off by default and it should usually stay off. An animation that turns the player - climbing
// round the side of a building - is carrying them somewhere, and cancelling it leaves the body
// facing one way and the view another. It exists because it is the player's call, not because
// there is a case for it.
static void ApplyYawFix(float* m, bool rowStorage)
{
    if (g_animYawFollow || !g_liveCtlValid || !g_camCacheValid) return;

    auto col = [&](int c, int i) { return rowStorage ? m[i * 4 + c] : m[c * 4 + i]; };
    const float fx = col(3,0), fy = col(3,1);
    if (fabsf(fx) + fabsf(fy) < 1e-6f) return;      // looking straight up or down: no yaw to read

    // UE3 has X forward and Y right, so atan2(fwd.y, fwd.x) is the rotator's own yaw sense and
    // the two are directly comparable without a conversion.
    float d = atan2f(fy, fx) - g_liveCtlYaw;
    while (d >  3.14159265f) d -= 6.28318531f;
    while (d < -3.14159265f) d += 6.28318531f;

    const float kMax = 60.0f * (3.14159265f / 180.0f);
    if (d >  kMax) d =  kMax;
    if (d < -kMax) d = -kMax;

    // About the WORLD up axis, not the camera's. A yaw is a turn about vertical whatever the
    // camera is doing in pitch; turning about a pitched up axis would tilt the horizon.
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    ApplyCameraRotation(m, rowStorage, up, d, RotationPivot());
}

static void ApplyRoll(float* m, bool rowStorage)
{
    if (!g_rollEnabled || fabsf(g_headRoll) < 1e-4f) return;
    if (g_targetHalfFovX <= 0.0f || g_targetHalfFovY <= 0.0f) return;

    const float tanX = tanf(g_targetHalfFovX), tanY = tanf(g_targetHalfFovY);
    if (tanX < 1e-6f || tanY < 1e-6f) return;

    const float th = g_headRoll * g_rollSign;
    const float c = cosf(th), s = sinf(th);
    const float k0 = (tanY / tanX) * s;     // how much of column 1 mixes into column 0
    const float k1 = (tanX / tanY) * s;     // and column 0 into column 1

    for (int i = 0; i < 4; ++i) {
        const int i0 = rowStorage ? (i * 4 + 0) : (0 * 4 + i);
        const int i1 = rowStorage ? (i * 4 + 1) : (1 * 4 + i);
        const float a = m[i0], b = m[i1];
        m[i0] = a * c - b * k0;
        m[i1] = a * k1 + b * c;
    }
}

// Build one eye's matrix from the cached scene matrix: offset along its own right axis, then
// the FOV force. Same maths as the alternate-eye path, applied to a copy instead of in place.
static void BuildEyeMatrix(float* out, int eye)
{
    memcpy(out, g_sceneMat, sizeof(float) * 16);
    float* m = out;

    const float rx = m[0], ry = m[4], rz = m[8];
    const float rl = sqrtf(rx*rx + ry*ry + rz*rz);
    if (rl > 1e-6f && g_halfIpdUU > 0.0f) {
        const float s = ((eye == 0) ? -1.0f : +1.0f) * g_halfIpdUU * g_stereoStrength / rl;
        const float ox = rx * s, oy = ry * s, oz = rz * s;
        for (int c = 0; c < 4; ++c)
            m[12 + c] -= ox * m[0 + c] + oy * m[4 + c] + oz * m[8 + c];
    }
    // All world-space pre-multiplications, so they belong together and ahead of the projection.
    // Roll first: cancelling it turns about the forward axis, which leaves the forward direction
    // alone, so the pitch measured next is unaffected. The other order would have moved the
    // forward and changed the cos(pitch) the roll measurement divides by.
    ApplyRollFix(m, true);
    ApplyPitchFix(m, true);
    ApplyYawFix(m, true);    // last: it turns about world up, which the other two do not touch
    ApplyYawLag(m, true);
    ApplySixDof(m, true);   // a world-space offset, same stage as the per-eye one
    if (g_fovForce && g_gameFovValid && g_targetHalfFovX > 0.0f) {
        // ⚠️ Uses g_targetHalfFovX, the SAME number the projection layer submits.
        //
        // The first version computed its own horizontal target here from the half-width
        // aspect, while the layer went on submitting one derived from the full-frame aspect.
        // Rendering one frustum and declaring another is exactly the mistake that produced the
        // original double vision, repeated in a second place - and it presented as "too zoomed
        // in", which sounds like a scale problem rather than a frustum one.
        const float sx = tanf(g_gameHalfFovX) / tanf(g_targetHalfFovX);
        const float sy = tanf(g_gameHalfFovY) / tanf(g_targetHalfFovY);
        for (int r = 0; r < 4; ++r) { m[r * 4 + 0] *= sx; m[r * 4 + 1] *= sy; }
    }
    ApplyRoll(m, true);      // after the projection, so the tangents are the real frustum
}

// Issue one draw twice, once per eye, into its own half of the backbuffer.
template <typename FN>
static HRESULT DuplicateDraw(IDirect3DDevice9* dev, FN issue)
{
    D3DVIEWPORT9 vpWas{};
    if (FAILED(dev->GetViewport(&vpWas))) return issue();

    g_inDupDraw = true;
    float eyeMat[16];
    HRESULT hr = D3D_OK;

    for (int eye = 0; eye < 2; ++eye) {
        D3DVIEWPORT9 vp = vpWas;
        // ---- split the viewport the engine set, not the backbuffer ----
        //
        // The viewport is in the bound render target's own coordinates, so taking its width from
        // g_capW was only ever correct while the target was backbuffer-sized. Halving what is
        // already there is right for any target, and produces the identical rectangle in the case
        // that worked before - vpWas.X = 0 and vpWas.Width = g_capW.
        vp.X     = vpWas.X + ((eye == 0) ? 0 : vpWas.Width / 2);
        vp.Width = vpWas.Width / 2;
        dev->SetViewport(&vp);
        BuildEyeMatrix(eyeMat, eye);
        g_origSetVSConstF(dev, (UINT)g_vmReg, eyeMat, 4);
        hr = issue();
    }

    dev->SetViewport(&vpWas);
    g_origSetVSConstF(dev, (UINT)g_vmReg, g_sceneMat, 4);   // leave c0 as the engine left it
    g_inDupDraw = false;
    InterlockedIncrement(&g_dupDraws);
    InterlockedIncrement(&g_dupFrameDraws);
    return hr;
}

// ⚠️ g_c0IsScene is the load-bearing condition, and leaving it out was a real defect.
//
// DuplicateDraw uploads the scene matrix to c0 before each draw it handles. Applied to a draw
// that was using a DIFFERENT matrix - a shadow pass, a post-process, the HUD - that forces the
// scene view onto geometry which never asked for it, and the pass renders as garbage.
//
// c0 provably carries more than one matrix here: the per-upload validation already rejects a
// foreign one arriving at that register. The same signal says whether the CURRENT contents are
// the scene view, so a draw is only duplicated while it is.
static bool ShouldDuplicate()
{
    if (g_rtCurrent) {
        g_rtCurrent->draws++;
        // Counted BEFORE the gate, deliberately: this is what AdoptSceneTarget reads, and if it
        // only counted draws that were already being duplicated it could never discover a target
        // that duplication is not reaching - which is the entire situation it exists for.
        if (g_c0IsScene) {
            g_rtCurrent->sceneDraws++;
            // Per-frame split-flip detector feed. The 12-second census proved the split mode
            // engages mid-gameplay with no load, no posture, no settings change - but its
            // granularity hides the flip MOMENT. These are read and reset every Present.
            if (g_rtCurrent->w == g_capW && g_rtCurrent->h == g_capH)
                ++g_frameSceneOnBackbuffer;
            else
                ++g_frameSceneOffscreen;
        }
    }
    const bool currentTargetMatchesScene = g_rtCurrent
        ? (g_rtCurrent->w == (g_sceneW ? g_sceneW : g_capW) &&
           g_rtCurrent->h == (g_sceneH ? g_sceneH : g_capH))
        : g_rtIsScene;
    if (!(g_simulStereo && !g_sceneSplitMono && !g_inDupDraw && g_sceneMatValid &&
          g_c0IsScene && currentTargetMatchesScene &&
          g_vmReg >= 0 && g_halfIpdUU > 0.0f && g_capW > 0)) return false;

    // ---- INSERT bisects WHICH scene-sized target gets duplicated ----
    //
    // The census shows FOUR surfaces at 2560x1440 - two A8R8G8B8, two A16B16G16R16F - and two
    // of them take heavy draw counts. A size test cannot separate them, which is exactly what
    // the reference's run 27 records: UE3 allocates whole-scene dominant shadow maps at scene
    // resolution, so a full-res offscreen target passes as the scene.
    //
    // Rather than guess which is the scene colour target, cycle. -1 duplicates all of them,
    // which is the current behaviour; 0..N restricts it to one. Whichever setting stops the
    // flickering identifies the target that must not be split, and the answer is measured
    // rather than argued.
    if (g_dupOnlyTarget >= 0)
        return g_rtCurrent && g_rtCurrent == &g_rtSeen[g_dupOnlyTarget];
    return true;
}

// ================================================================ diag: the user-pointer draws
//
// The HUD and the in-game menus are unreadable in the headset - each eye gets half of them - and
// the first attempt to find out why measured nothing at all. That silence IS the finding.
//
// It hooked DrawPrimitive and DrawIndexedPrimitive, and the census had already shown 1.7M draws
// on the scene target with plenty carrying the scene matrix and ZERO undoubled candidates. Not
// "the HUD resembles a post-process pass" - the HUD never came through that door.
//
// DrawPrimitiveUP and DrawIndexedPrimitiveUP, slots 83 and 84, are not hooked. They appear once
// in this file, in a note wondering whether the occlusion boxes arrive that way. UE3's Canvas
// draws dynamic 2D geometry through exactly those calls, which would explain both symptoms with
// one cause: the probe cannot see them, and neither can duplication, so they are issued once at
// full width across both eyes' halves.
//
// This counts, and does nothing else. If the totals track the HUD being on screen then the fix
// is viewport-only duplication on these two entry points - both halves, without touching c0,
// because this geometry has its own matrix. Counting first because the last guess about where
// these draws came from was wrong, and a wrong guess here means splitting a pass that must not
// be split.
static volatile LONG g_upDraws      = 0;   // every UP draw
static volatile LONG g_uiDupDraws   = 0;   // ...that were duplicated into both eyes
static volatile LONG g_upOnScene    = 0;   // ...landing on the scene target while stereo is live
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT,
                                                    const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT,
                                                       UINT, UINT, const void*, D3DFORMAT,
                                                       const void*, UINT);
static PFN_DrawPrimUP    g_origDrawPrimUP    = nullptr;
static PFN_DrawIndexedUP g_origDrawIndexedUP = nullptr;

// ---- and now: which of them does the player actually SEE? ----
//
// 650 user-pointer draws a frame is not a HUD. Something far larger shares this path, and the
// candidate is named in this file already: the occlusion boxes, suspected of arriving here and
// drawing at full-frame coordinates against half-remapped geometry.
//
// COLORWRITEENABLE separates them, and does it by definition rather than by heuristic. An
// occlusion box writes NO colour - that is the whole point of it, geometry submitted only to be
// counted. The HUD writes colour or it would not be a HUD. So "does this draw put pixels on the
// screen" is exactly the question "must this be in both eyes", and it needs no guessing about
// what the geometry is for.
//
// Sampled one frame in 600: a GetRenderState per draw is 650 calls a frame, fine once and absurd
// continuously.
static bool  g_upSample = false;
static long  g_upSeenColour = 0, g_upSeenNoColour = 0;
static long  g_upPrimColour = 0, g_upPrimNoColour = 0;

// The colour-writing group, split by the states most likely to separate a HUD quad from a
// full-screen composite: blending (a HUD element is composited over the scene, a blit replaces
// it), depth, and the primitive count - a full-screen quad is exactly 2 triangles, and 12 was
// enough to identify the occlusion boxes outright.
struct UpBucket { DWORD blend, z; UINT prims; long count; long firstOrd, lastOrd; };
static UpBucket g_upB[12]{};
static int      g_upBCount = 0;
static long     g_upOrdinal = 0;

static inline void NoteUpDraw(IDirect3DDevice9* dev, UINT primCount)
{
    InterlockedIncrement(&g_upDraws);
    // The same filter the old probe used: on the scene target, while duplication is actually
    // running this frame. That is precisely the population being stretched across both eyes.
    const bool onScene = g_rtIsScene && !g_inDupDraw &&
                         InterlockedCompareExchange(&g_dupFrameDraws, 0, 0) > 0;
    if (!onScene) return;
    InterlockedIncrement(&g_upOnScene);

    if (!g_upSample) return;
    g_upOrdinal++;
    DWORD cw = 0;
    if (FAILED(dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw))) return;
    if (!cw) { g_upSeenNoColour++; g_upPrimNoColour += primCount; return; }

    g_upSeenColour++; g_upPrimColour += primCount;
    DWORD ab = 0, z = 0;
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
    dev->GetRenderState(D3DRS_ZENABLE, &z);
    for (int i = 0; i < g_upBCount; ++i) {
        if (g_upB[i].blend == ab && g_upB[i].z == z && g_upB[i].prims == primCount) {
            g_upB[i].count++; g_upB[i].lastOrd = g_upOrdinal; return;
        }
    }
    if (g_upBCount < 12)
        g_upB[g_upBCount++] = { ab, z, primCount, 1, g_upOrdinal, g_upOrdinal };
}

// ---- and the fix the measurement earned ----
//
// The split is not a judgement call. Six sampled frames, and EVERY no-colour draw was exactly
// 12 primitives - a cube, six times out of six. Those are axis-aligned bounding boxes submitted
// to be counted by an occlusion query, and they must not be touched: they write nothing the
// player sees, and splitting them would change what the query measures.
//
//     33-47 draws WRITE COLOUR, 494-946 prims   <- the HUD. Variable size, quads and text.
//     460-658 write none, exactly 12 prims each <- occlusion boxes. Leave alone.
//
// So the colour-writing ones are issued twice, once into each half viewport, and c0 is NOT
// touched. That last part matters: this geometry is in screen space with its own transform, and
// forcing the scene view onto it is the exact failure ShouldDuplicate's comment describes.
//
// ⚠️ KNOWN AND ACCEPTED: a screen-space overlay drawn into a half-width viewport is compressed
// 2:1 horizontally. The scene does not suffer this because its projection is rebuilt per eye to
// match; the HUD has no projection we control. So the HUD will read as narrow. That is a
// legible HUD in both eyes instead of half of one in each, which is the trade being made
// deliberately - undoing the squeeze needs the overlay's own transform and is its own rung.
//
// ⚠️ DEFAULT OFF, and colour-write alone is NOT the rule. Measured: with every colour-writing
// UP draw duplicated, the HUD became readable and THE WORLD TILED - eight or more copies instead
// of two. ~35 draws a frame were duplicated, exactly the group aimed at, so the rule fired as
// designed and the group is simply not only the HUD.
//
// The shape of that failure names the cause. A full-screen pass that RESAMPLES the scene - a
// composite or post-process blit, two triangles from a user pointer, colour writes on - is
// indistinguishable from a HUD quad by colour writes. Duplicating it draws the whole frame,
// which already holds two eyes side by side, squeezed into each half: four copies from one such
// pass, eight from two.
//
// So the group needs splitting again, and the breakdown below is there to do it rather than to
// guess a third time. NUMPAD0 turns duplication on for an A/B in the meantime - the HUD becomes
// readable and the world tiles, which is the trade until the composite passes can be excluded.
bool g_dupUI = false;   // ⚠️ see below - the filter approach did not converge; NUMPAD0 to try it

// ---- ⚠️ does this draw READ something the engine rendered into? ----
//
// The test that colour-writing could not make. An overlay samples a UI atlas; a composite or a
// post-process pass samples a RENDER TARGET - it is re-presenting the frame rather than drawing
// on top of it. That is exactly the distinction between "must appear in both eyes" and "must not
// be touched", and it is a property of the draw rather than a guess about its purpose.
//
// It is also the only rule that would have caught the eight-fold tiling: a two-triangle,
// depth-off, colour-writing draw at the very END of the frame - by every state the HUD uses -
// which samples the scene, as a HUD quad never does.
//
// ⚠️ AND IT STILL DOES NOT CATCH THEM ALL. Measured: it excludes 1-2 draws a frame and the world
// still appears TWICE per eye, so roughly one resampling pass a frame is getting through, plus
// flashing in the right eye. Most likely it samples the backbuffer - which is never entered in
// the census, because restoring it comes through SetRenderTarget with a null surface - or binds
// at a texture stage other than zero.
//
// The remaining hole could be plugged. It is deliberately NOT being plugged, because that would
// be the fifth filter on this path and the pattern is now the finding: each one separated the
// examples in hand rather than encoding the real distinction, and each was discovered wrong only
// after it reached a headset. Duplicating the game's own 2D pass is the wrong shape of solution.
// The right one is to stop sharing a surface with it - give the overlay its own render target and
// submit it as a second composition layer, which also drops the 2:1 squeeze this approach cannot
// avoid. That is a rung, not a filter.
//
// Left in, defaulted off, because the measurement infrastructure below is worth keeping and
// NUMPAD0 makes the near-miss inspectable.
static bool SamplesARenderTarget(IDirect3DDevice9* dev)
{
    IDirect3DBaseTexture9* base = nullptr;
    if (FAILED(dev->GetTexture(0, &base)) || !base) return false;

    bool hit = false;
    IDirect3DTexture9* tex = nullptr;
    if (SUCCEEDED(base->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&tex)) && tex) {
        IDirect3DSurface9* surf = nullptr;
        if (SUCCEEDED(tex->GetSurfaceLevel(0, &surf)) && surf) {
            for (int i = 0; i < g_rtSeenCount && !hit; ++i)
                if (g_rtSeen[i].surf == surf) hit = true;
            surf->Release();
        }
        tex->Release();
    }
    base->Release();
    return hit;
}

static volatile LONG g_uiSkippedResample = 0;

static bool ShouldDuplicateUI(IDirect3DDevice9* dev)
{
    if (!g_dupUI) return false;
    if (!(g_simulStereo && !g_inDupDraw && g_rtIsScene && g_capW > 0)) return false;
    // Only once the scene itself is being split this frame. Before that there is one image and
    // the HUD belongs across all of it.
    if (InterlockedCompareExchange(&g_dupFrameDraws, 0, 0) <= 0) return false;
    DWORD cw = 0;
    if (FAILED(dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw)) || cw == 0) return false;
    if (SamplesARenderTarget(dev)) { InterlockedIncrement(&g_uiSkippedResample); return false; }
    return true;
}

// Same halving as DuplicateDraw and deliberately NOT the same function: that one uploads an eye
// matrix to c0 before each issue, which is the one thing this must not do.
template <typename FN>
static HRESULT DuplicateViewportOnly(IDirect3DDevice9* dev, FN issue)
{
    D3DVIEWPORT9 vpWas{};
    if (FAILED(dev->GetViewport(&vpWas))) return issue();

    g_inDupDraw = true;
    HRESULT hr = D3D_OK;
    for (int eye = 0; eye < 2; ++eye) {
        D3DVIEWPORT9 vp = vpWas;
        vp.X     = vpWas.X + ((eye == 0) ? 0 : vpWas.Width / 2);
        vp.Width = vpWas.Width / 2;
        dev->SetViewport(&vp);
        hr = issue();
    }
    dev->SetViewport(&vpWas);
    g_inDupDraw = false;
    InterlockedIncrement(&g_uiDupDraws);
    return hr;
}

static void ReportUpSample()
{
    const long tot = g_upSeenColour + g_upSeenNoColour;
    if (tot > 0) {
        Log("[up] one sampled frame: %ld draws write colour (%ld prims), %ld write none"
            " (%ld prims, the occlusion boxes)",
            g_upSeenColour, g_upPrimColour, g_upSeenNoColour, g_upPrimNoColour);
        Log("[up]   the colour-writing ones, split - a full-screen blit is 2 prims and is what"
            " tiles the world when duplicated:");
        for (int i = 0; i < g_upBCount; ++i)
            Log("[up]     blend=%lu z=%lu  %u prims  x%ld draws   order %ld..%ld of %ld",
                g_upB[i].blend, g_upB[i].z, g_upB[i].prims, g_upB[i].count,
                g_upB[i].firstOrd, g_upB[i].lastOrd, g_upOrdinal);
    }
    g_upSeenColour = g_upSeenNoColour = g_upPrimColour = g_upPrimNoColour = 0;
    g_upBCount = 0; g_upOrdinal = 0;
}

static HRESULT STDMETHODCALLTYPE Hook_DrawPrimUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
                                                 UINT primCount, const void* data, UINT stride)
{
    NoteUpDraw(dev, primCount);
    if (ShouldDuplicateUI(dev))
        return DuplicateViewportOnly(dev, [&] {
            return g_origDrawPrimUP(dev, type, primCount, data, stride);
        });
    return g_origDrawPrimUP(dev, type, primCount, data, stride);
}

static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
                                                    UINT minVertex, UINT numVerts, UINT primCount,
                                                    const void* idx, D3DFORMAT idxFmt,
                                                    const void* verts, UINT stride)
{
    NoteUpDraw(dev, primCount);
    if (ShouldDuplicateUI(dev))
        return DuplicateViewportOnly(dev, [&] {
            return g_origDrawIndexedUP(dev, type, minVertex, numVerts, primCount, idx, idxFmt,
                                       verts, stride);
        });
    return g_origDrawIndexedUP(dev, type, minVertex, numVerts, primCount, idx, idxFmt,
                               verts, stride);
}

static void ReportUpDraws()
{
    const LONG all   = InterlockedExchange(&g_upDraws, 0);
    const LONG scene = InterlockedExchange(&g_upOnScene, 0);
    if (all == 0) {
        Log("[up] no user-pointer draws at all over 600 frames - the HUD is NOT coming through"
            " DrawPrimitiveUP, and the hypothesis is wrong");
        return;
    }
    Log("[up] user-pointer draws over 600 frames: %ld total, %ld onto the scene target while"
        " stereo was live, %ld DUPLICATED into both eyes  (%s)",
        all, scene, InterlockedExchange(&g_uiDupDraws, 0), g_dupUI ? "NUMPAD0 off" : "OFF");
    Log("[up]   %ld colour-writing draws were left alone because they SAMPLE a render target"
        " - composites and post passes, the ones that tiled the world",
        InterlockedExchange(&g_uiSkippedResample, 0));
}

static HRESULT STDMETHODCALLTYPE Hook_DrawPrim(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
                                               UINT start, UINT count)
{
    if (!ShouldDuplicate()) return g_origDrawPrim(dev, type, start, count);
    return DuplicateDraw(dev, [&] { return g_origDrawPrim(dev, type, start, count); });
}

static HRESULT STDMETHODCALLTYPE Hook_DrawIndexed(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
                                                  INT baseVertex, UINT minIndex, UINT numVerts,
                                                  UINT startIndex, UINT primCount)
{
    if (!ShouldDuplicate())
        return g_origDrawIndexed(dev, type, baseVertex, minIndex, numVerts, startIndex, primCount);
    return DuplicateDraw(dev, [&] {
        return g_origDrawIndexed(dev, type, baseVertex, minIndex, numVerts, startIndex, primCount);
    });
}

// ================================================================ camera-animation probe
//
// The feasibility assessment called Mirror's Edge's animated camera the project's longest pole
// - wall-run roll, landing dips, vaults - and nothing has measured it. This does, before
// anything is built to suppress it.
//
// ---- the signal is a subtraction ----
//
// TdPlayerPawn::CalcCamera composes the view as
//
//     out_Rotation  = GetViewRotation()            // the player's own look
//     out_Rotation += GetCameraAnimation()         // swizzled, see ENGINE_NOTES
//     PlayerCameraRotation = out_Rotation           // cached on the pawn
//
// and TdPlayerController::UpdateRotation writes the player's look into the controller's
// Rotation, zeroing roll on the way. So:
//
//     PlayerCameraRotation - Controller.Rotation  ==  the animation contribution
//
// and ALL roll in it is animation by definition, because the controller's is always zero. That
// makes roll the cleanest of the three axes and the one most likely to matter for comfort.
//
// ⚠️ Both sides go through YawDelta(). UE3 is 65536 units per turn and a raw int32 subtraction
// of two headings 0.3 degrees apart reads as 360.3 - the defect that cost the Singularity
// project months.

int  g_offMoveState = -1;
int  g_offWeapon = -1;
int  g_offWeaponAnimState = -1;
static float g_animPeak[3] = { 0, 0, 0 };       // degrees, |pitch| |yaw| |roll| since last report
float        g_animNow[3]  = { 0, 0, 0 };       // live, for the overlay and the pitch anchor
static int   g_animState   = -1;
// From TdPawn.EMovement in the decompiled script. Named rather than numbered because the whole
// point is to say WHICH moves throw the camera around, and "state 4 = 11.2 degrees" needs a
// lookup table on the reader's side that a log line can just carry.
static const char* kMoveNames[] = {
    "None","Walking","Falling","Grabbing","WallRunRight","WallRunLeft","WallClimbing",
    "SpringBoard","SpeedVault","VaultOver","GrabPullUp","Jump","WallRunJump","GrabJump",
    "IntoGrab","Crouch","Slide","Melee","Snatch","Barge","Landing","Climb","IntoClimb",
    "WallKick","180Turn","180TurnInAir","LayOnGround","IntoZipLine","ZipLine","Balance",
    "LedgeWalk","GrabTransfer","MeleeAir","DodgeJump","WallRunDodgeJump","Stumble","Snatched",
    "StepUp","RumpSlide","Interact","WallRun","BotStop","BotStartWalk","BotStartRun",
    "BotTurnRun","BotTurnStand","ExitCover","Vertigo","MeleeSlide","WallClimbDodgeJump",
    "WallClimb180Jump","WallClimbDodgeL","WallClimbDodgeR","MeleeVault","BotMelee2",
    "StumbleHard","BotRoll","BotFlip",
};
static const int kMoveCount = (int)(sizeof(kMoveNames) / sizeof(kMoveNames[0]));

// Shared spelling for a movement state in log lines: the table name when the byte is in
// range, "?" when it is not. Out-of-enum values are real - state 72 was logged at a fall
// entry - and must stay visibly distinct from every named state.
static const char* MoveName(int st)
{
    return (st >= 0 && st < kMoveCount) ? kMoveNames[st] : "?";
}

// The authoritative what-state-when timeline. Every other line that mentions a movement
// state does so as a side effect of its own gate, rate limit, or eligibility check; the
// transitions BETWEEN those moments were invisible, and the fall/grab freezes live exactly
// there. One byte read per frame, a line only on change - and a pawn swap is a change even
// at the same state, because a replacement pawn is a new rig generation.
static void LogMoveTransitions()
{
    static int lastState = -1;          // -1 = unreadable / nothing seen yet
    static uintptr_t lastPawn = 0;
    const uintptr_t pawn = g_playerPawn;
    uint8_t st = 0xFF;
    if (!pawn || g_offMoveState < 0 || !SafeRead(pawn + g_offMoveState, &st, 1)) {
        if (lastState != -1) {
            Log("[move] frame %ld t=%.2fs: %s(%d) -> UNREADABLE (pawn %p)",
                g_frames, LogSecs(), MoveName(lastState), lastState, (void*)pawn);
            lastState = -1;
            lastPawn = 0;
        }
        return;
    }
    if ((int)st != lastState || pawn != lastPawn) {
        Log("[move] frame %ld t=%.2fs: %s(%d) -> %s(%d)%s", g_frames, LogSecs(),
            MoveName(lastState), lastState, MoveName(st), (int)st,
            (lastPawn != 0 && pawn != lastPawn) ? "  (new pawn)" : "");
        lastState = (int)st;
        lastPawn = pawn;
    }
}
static float g_statePeakRoll[64] = { 0 };       // worst roll seen in each movement state
static float g_statePeakPitch[64] = { 0 };
static long  g_animSamples = 0;

static void ProbeCameraAnimation()
{
    if (g_offCamRot < 0 || g_offActorRotation < 0) return;
    const uintptr_t pawn = g_playerPawn, ctl = g_playerCtl;
    if (!pawn || !ctl) return;

    int32_t camRot[3], ctlRot[3];
    if (!SafeRead(pawn + g_offCamRot, camRot, sizeof(camRot))) return;
    if (!SafeRead(ctl + g_offActorRotation, ctlRot, sizeof(ctlRot))) return;

    const float kDeg = 360.0f / 65536.0f;
    g_animNow[0] = YawDelta(camRot[0], ctlRot[0]) * kDeg;   // pitch
    g_animNow[1] = YawDelta(camRot[1], ctlRot[1]) * kDeg;   // yaw
    g_animNow[2] = YawDelta(camRot[2], ctlRot[2]) * kDeg;   // roll
    g_animSamples++;

    for (int i = 0; i < 3; ++i) {
        const float a = fabsf(g_animNow[i]);
        if (a > g_animPeak[i]) g_animPeak[i] = a;
    }

    // Attribute the worst roll to the movement state that produced it. Wall-running and
    // landing are the states the assessment named; this says which actually move the view and
    // by how much, instead of leaving it as a description.
    if (g_offMoveState >= 0) {
        uint8_t st = 0;
        if (SafeRead(pawn + g_offMoveState, &st, 1) && st < 64) {
            g_animState = st;
            const float r = fabsf(g_animNow[2]);
            const float p = fabsf(g_animNow[0]);
            if (r > g_statePeakRoll[st])  g_statePeakRoll[st]  = r;
            if (p > g_statePeakPitch[st]) g_statePeakPitch[st] = p;
        }
    }
}

static void ReportCameraAnimation()
{
    Log("[anim] peaks since last report: pitch %.1f  yaw %.1f  ROLL %.1f degrees (%ld samples)",
        g_animPeak[0], g_animPeak[1], g_animPeak[2], g_animSamples);
    // Cumulative across the whole run, never reset: the question is "what is the worst this
    // move ever does", and a per-window peak would hide the one landing that mattered.
    for (int s = 0; s < 64; ++s) {
        if (g_statePeakRoll[s] < 0.5f && g_statePeakPitch[s] < 0.5f) continue;
        Log("[anim]   %-18s roll %5.1f   pitch %5.1f  degrees",
            s < kMoveCount ? kMoveNames[s] : "?", g_statePeakRoll[s], g_statePeakPitch[s]);
    }
    g_animPeak[0] = g_animPeak[1] = g_animPeak[2] = 0.0f;
    g_animSamples = 0;
}

// ================================================================ the game's side of the pad
//
// The counterpart to ReportPadState. That one says what the runtime handed us; this says what
// the game made of it, and whether it was allowed to act on it. See the block above
// DeriveBoolMaskOffset for why the pair exists.

static long  g_gateSamples = 0;
static long  g_gateReads[kGateMax] = {};
static long  g_gateSet[kGateMax]   = {};       // samples in which the flag read non-zero
static uint32_t g_gateLast[kGateMax] = {};
static float g_inputSizePeak = 0.0f;

static bool ReadFlag(uintptr_t obj, const FlagRef& f, uint32_t* out)
{
    if (f.off < 0) return false;
    if (f.mask) {
        uint32_t dw;
        if (!SafeRead(obj + f.off, &dw, 4)) return false;
        *out = (dw & f.mask) ? 1u : 0u;
        return true;
    }
    uint8_t b;
    if (!SafeRead(obj + f.off, &b, 1)) return false;
    *out = b;                                   // a byte counter, so the COUNT is the value
    return true;
}

static void ProbeInputGates()
{
    if (g_gateCount == 0 && g_offInputSize < 0) return;
    // Validated rather than merely non-null, for the reason recorded at the vertex-constant
    // hook: between a controller being destroyed and Present noticing, this reads a freed
    // object - and a freed dword masked to one bit still reads as a perfectly plausible flag.
    const uintptr_t ctl = g_playerCtl;
    if (!LooksLikePlayerController(ctl)) return;

    g_gateSamples++;
    for (int i = 0; i < g_gateCount; ++i) {
        uint32_t v = 0;
        if (!ReadFlag(ctl, g_gates[i].ref, &v)) continue;
        g_gateReads[i]++;
        g_gateLast[i] = v;
        if (v) g_gateSet[i]++;
    }
    if (g_offInputSize >= 0) {
        float sz = 0.0f;
        // Bounded, not merely read. A stick magnitude is 0..1 and anything far outside that is
        // a wrong offset or a dead object, which should not be reported as a peak.
        if (SafeRead(ctl + g_offInputSize, &sz, 4) && sz > g_inputSizePeak && sz < 100.0f)
            g_inputSizePeak = sz;
    }
}

static void ReportInputGates()
{
    if (g_gateSamples == 0) return;

    char line[768]; int n = 0;
    n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                     "[input] over %ld samples:", g_gateSamples);
    for (int i = 0; i < g_gateCount; ++i) {
        if (g_gateReads[i] == 0) continue;
        // Last value AND how often it was set: a gate that is set for every sample and one that
        // flickered once are the same number at the instant this line is written.
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "  %s=%u(set %ld/%ld)",
                         g_gates[i].name, g_gateLast[i], g_gateSet[i], g_gateReads[i]);
    }
    Log("%s", line);

    if (g_offInputSize >= 0)
        Log("[input]   InputSize peaked %.2f this window  <- the stick magnitude the GAME computed."
            " Zero while the player is pushing means the pad never reached it.", g_inputSizePeak);

    g_gateSamples = 0;
    g_inputSizePeak = 0.0f;
    for (int i = 0; i < kGateMax; ++i) { g_gateReads[i] = 0; g_gateSet[i] = 0; }
}

// ================================================================ the on-screen readout
//
// Ported from the Singularity mod, where it was added at run 127 for a reason this project has
// now hit repeatedly: every mode and setting goes to a log file, so checking state while
// wearing the headset means taking it off. That gap cost that project a run when "combo 2"
// meant different things in two sessions and nothing on screen could say so.
//
// The Windows SDK ships no D3DX, so there is no DrawText. The glyphs are a 5x7 bitmap and each
// run of set pixels in a row becomes one rectangle in a Clear() list. Merging adjacent pixels
// keeps a line of text to a few dozen rectangles rather than one per pixel. No vertex buffer,
// no shader, and the only device state touched is the scissor - saved and restored.
//
// Drawn BEFORE the frame is captured, which is what puts it in front of your eyes rather than
// only on the desktop mirror.
//
// ⚠️ The rows are per-TEST and meant to be edited. Anything here that stops earning its space
// should be deleted rather than left to accumulate - a readout nobody reads is worse than none,
// because it costs pixels and implies it matters.

static const int kGlyphW = 5, kGlyphH = 7;
static const unsigned char kFont[][kGlyphH] = {
    {0,0,0,0,0,0,0},                                    //  0 space
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},               //  1 A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},               //    B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},               //    C
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},               //    D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},               //    E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},               //    F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},               //    G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},               //    H
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},               //    I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},               //    J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},               //    K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},               //    L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},               //    M
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11},               //    N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},               //    O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},               //    P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},               //    Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},               //    R
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},               //    S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},               //    T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},               //    U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},               //    V
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},               //    W
    {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11},               //    X
    {0x11,0x0A,0x04,0x04,0x04,0x04,0x04},               //    Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},               // 26 Z
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},               // 27 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},               //    1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},               //    2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},               //    3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},               //    4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},               //    5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},               //    6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},               //    7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},               //    8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},               // 36 9
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},               // 37 +
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},               // 38 -
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02},               // 39 (
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08},               // 40 )
    {0x00,0x04,0x00,0x00,0x00,0x04,0x00},               // 41 :
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},               // 42 .
    {0x01,0x02,0x02,0x04,0x08,0x08,0x10},               // 43 /
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},               // 44 =
    // 45: the fallback. ⚠️ The reference returns SPACE for an unknown character, and it cost
    // that project two invisible arrows and a missing pair of asterisks before anyone noticed
    // - a silently dropped glyph is indistinguishable from a typo in the string. A filled box
    // is impossible to miss and says "add this glyph" rather than nothing at all.
    {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F},               // 45 unknown
};

static int GlyphIndex(char ch)
{
    if (ch == ' ') return 0;
    if (ch >= 'A' && ch <= 'Z') return 1 + (ch - 'A');
    if (ch >= 'a' && ch <= 'z') return 1 + (ch - 'a');
    if (ch >= '0' && ch <= '9') return 27 + (ch - '0');
    switch (ch) {
        case '+': return 37;  case '-': return 38;
        case '(': return 39;  case ')': return 40;
        case ':': return 41;  case '.': return 42;
        case '/': return 43;  case '=': return 44;
        default:  return 45;                    // deliberately visible, see above
    }
}

// Append the rectangles for one line. Adjacent set pixels in a row are merged, so a word costs
// tens of rectangles rather than hundreds.
static int TextRects(D3DRECT* r, int n, int cap, int x0, int y0, int px, const char* s)
{
    for (int ci = 0; s[ci]; ++ci) {
        const unsigned char* g = kFont[GlyphIndex(s[ci])];
        const int cx = x0 + ci * (kGlyphW + 1) * px;
        for (int row = 0; row < kGlyphH; ++row) {
            int col = 0;
            while (col < kGlyphW) {
                if (!(g[row] & (0x10 >> col))) { ++col; continue; }
                int run = 0;
                while (col + run < kGlyphW && (g[row] & (0x10 >> (col + run)))) ++run;
                if (n < cap) {
                    r[n].x1 = cx + col * px;
                    r[n].y1 = y0 + row * px;
                    r[n].x2 = cx + (col + run) * px;
                    r[n].y2 = y0 + (row + 1) * px;
                    ++n;
                }
                col += run;
            }
        }
    }
    return n;
}

static bool g_overlay = true;      // F3 toggles

static void FormatHandTuneValue(char* out, size_t cap, int value, bool selected)
{
    if (!out || cap == 0) return;
    if (selected) _snprintf_s(out, cap, _TRUNCATE, "(%+d)", value);
    else          _snprintf_s(out, cap, _TRUNCATE, "%+d", value);
}

static void DrawOverlay(IDirect3DDevice9* dev)
{
    if (!g_debug || !g_overlay || !dev) return;   // Debug=off takes the text off the screen

    // Sized with headroom and bounds-checked below. The rows are edited per test by design,
    // and this array had silently grown to nine entries in an eight-row buffer - /analyze
    // caught it, but the next row added would have been a stack overwrite in a Present hook.
    char lines[16][64];
    int nl = 0;

    // ---- rows for the CURRENT test. Delete freely; see the note at the top. ----
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "MEVR FPS %d GRAB %d MS",
                (int)(g_capSamples ? (1000.0 / (g_capMsTotal / g_capSamples + 0.001)) : 0),
                (int)(g_capSamples ? (g_capMsTotal / g_capSamples) : 0.0));
    // Put the active test at the top. TextRects intentionally caps work; late diagnostic rows
    // may truncate on a dense frame, but the controls the player is actively using must not.
    char rp[12], ry[12], rr[12], lp[12], ly[12], lr[12];
    FormatHandTuneValue(rp, sizeof(rp), g_wristCalibrationDeg[1][0],
                        g_handTuneSelected == 0);
    FormatHandTuneValue(ry, sizeof(ry), g_wristCalibrationDeg[1][1],
                        g_handTuneSelected == 1);
    FormatHandTuneValue(rr, sizeof(rr), g_wristCalibrationDeg[1][2],
                        g_handTuneSelected == 2);
    FormatHandTuneValue(lp, sizeof(lp), g_wristCalibrationDeg[0][0],
                        g_handTuneSelected == 3);
    FormatHandTuneValue(ly, sizeof(ly), g_wristCalibrationDeg[0][1],
                        g_handTuneSelected == 4);
    FormatHandTuneValue(lr, sizeof(lr), g_wristCalibrationDeg[0][2],
                        g_handTuneSelected == 5);
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "WRIST R P%s Y%s R%s", rp, ry, rr);
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "WRIST L P%s Y%s R%s", lp, ly, lr);
    FormatHandTuneValue(rr, sizeof(rr), g_forearmRollCalibrationDeg[1],
                        g_handTuneSelected == 6);
    FormatHandTuneValue(lr, sizeof(lr), g_forearmRollCalibrationDeg[0],
                        g_handTuneSelected == 7);
    _snprintf_s(lines[nl++], 64, _TRUNCATE,
                "FOREARM R R%s L R%s", rr, lr);
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "ARROWS L/R SELECT  U/D CHANGE 5 DEG");
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "OBJ %s  PROP %s",
                g_offName >= 0 ? "OK" : "NO", g_offPropOff >= 0 ? "OK" : "NO");
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "HEAD %s Y%+d P%+d  ROLL %s %+d DEG",
                g_headTracking ? "ON" : "OFF", g_yawSign, g_pitchSign,
                g_rollEnabled ? "ON" : "OFF", (int)(g_headRoll * g_rollSign * 57.2958f));
    if (g_vmReg >= 0)
        _snprintf_s(lines[nl++], 64, _TRUNCATE, "VM C%d %s MODE %d",
                    g_vmReg, g_vmRow ? "ROW" : "COL", g_vmMode);
    else
        _snprintf_s(lines[nl++], 64, _TRUNCATE, "VM NOT SCANNED  F6");
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "OFFSET %d %d %d",
                (int)g_vmOffset[0], (int)g_vmOffset[1], (int)g_vmOffset[2]);
    // ⚠️ This block showed SCALE 100 UU/M and no strength at all, because two edits to it were
    // made with a string replace that silently did nothing when it failed to match. World
    // scale is now a fixed measured constant, so that row could never change - reported as
    // "stuck at 100 no matter how many times I press F11", which was the readout being wrong
    // rather than the key. STRENGTH is what F11 moves and is what has to be on screen.
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "STEREO %s %s  STRENGTH %d PCT",
                g_stereoMode ? "ON" : "OFF", g_simulStereo ? "SIMUL" : "ALT",
                (int)(g_stereoStrength * 100.0f));
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "DUP %ld ONLY %d  OCC %s",
                InterlockedCompareExchange(&g_dupDraws, 0, 0), g_dupOnlyTarget,
                g_occlusionMode == 2 ? "REAL" : (g_occlusionMode == 1 ? "ALWAYS" : "AUTO"));
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "6DOF %s  R%+d U%+d F%+d UU",
                g_sixDof ? "ON" : "OFF", (int)g_dofOffset[0], (int)g_dofOffset[1],
                (int)-g_dofOffset[2]);
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "ANIM P%+d Y%+d R%+d ST%d",
                (int)g_animNow[0], (int)g_animNow[1], (int)g_animNow[2], g_animState);
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "FOV %d X %d  IPD %d/100",
                (int)(g_gameHalfFovX * 114.59f), (int)(g_gameHalfFovY * 114.59f),
                (int)(g_halfIpdUU * g_stereoStrength * 100.0f));

    // Static, not on the stack: 2048 D3DRECTs is 32 KB and this runs every frame on the
    // render thread, which /analyze flagged as C6262. Safe because DrawOverlay is only ever
    // called from Hook_Present, on one thread. TextRects stops at the cap rather than
    // overflowing, so a long line truncates instead of corrupting anything.
    if (nl > 16) nl = 16;   // belt and braces: the rows are edited freely and this is a hook
    static D3DRECT rects[2048];
    int n = 0;
    const int px = 3;                                   // pixel scale
    for (int i = 0; i < nl; ++i)
        n = TextRects(rects, n, 2048, 16, 16 + i * (kGlyphH + 2) * px, px, lines[i]);
    if (n <= 0) return;

    // Clear() honours the scissor, so a scissor left set by the engine would clip the text.
    // Saved and restored rather than assumed - this runs mid-frame inside someone else's
    // render state.
    DWORD scissorWas = 0;
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorWas);
    if (scissorWas) dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->Clear((DWORD)n, rects, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 96), 1.0f, 0);
    if (scissorWas) dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
}

// ================================================================ hold PAUSE to quit
//
// Ported from the Singularity mod, where it earned its place. Inside a headset you cannot
// see the monitor, and reaching the game's own quit menu by feel is worse than the test it
// is interrupting. PAUSE is one key, top right, findable without looking.
//
// ---- WM_CLOSE, not ExitProcess ----
//
// This is the same shutdown path as clicking the window's X, so the engine saves and exits
// normally. It matters more here than it looks: ENGINE_NOTES records that every copy of
// Mirror's Edge - Steam, GOG and the dev clone - reads and writes ONE save file, outside the
// install. Killing the process mid-write is a real way to lose it.
//
// ---- a HOLD, not a tap ----
//
// SCROLL LOCK sits directly beside PAUSE, so a mis-hit while wearing a headset is likely,
// and an accidental quit costs a whole run.
//
// ---- not gated behind any debug flag ----
//
// One of the few places a raw GetAsyncKeyState is legitimate. Quitting cleanly is not
// developer scaffolding: gate it and killing the process becomes the only way out from
// inside a headset, which is the exact failure this exists to prevent.
//
// Known tradeoff, inherited deliberately: GetAsyncKeyState is global, so holding PAUSE for a
// second while some other window has focus will also quit the game. The reference accepts
// this rather than add a foreground check, because a quit that silently stops working is a
// worse failure than one that occasionally fires when unwanted.

static HWND         g_gameWnd   = nullptr;
static volatile LONG g_quitPosted = 0;
static const ULONGLONG kQuitHoldMs = 1000;

static void CheckQuitHotkey()
{
    static ULONGLONG heldSince = 0;

    const bool down = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
    if (!down) { heldSince = 0; return; }

    const ULONGLONG now = GetTickCount64();
    if (heldSince == 0) { heldSince = now; return; }
    if (now - heldSince < kQuitHoldMs) return;

    if (InterlockedExchange(&g_quitPosted, 1) != 0) return;   // once only

    if (g_gameWnd && IsWindow(g_gameWnd)) {
        Log("*** PAUSE held %llu ms - posting WM_CLOSE to %p so the engine saves and exits cleanly",
            (unsigned long long)(now - heldSince), (void*)g_gameWnd);
        PostMessageW(g_gameWnd, WM_CLOSE, 0, 0);
    } else {
        // Deliberately does NOT fall back to ExitProcess. If the window is gone, an abrupt
        // exit is exactly the thing this feature exists to avoid.
        Log("*** PAUSE held, but no valid game window (%p) - NOT quitting. Nothing was killed.",
            (void*)g_gameWnd);
        InterlockedExchange(&g_quitPosted, 0);
    }
}

// ---------------------------------------------------------------- device hooks

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*,
                                                 HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD,
                                                       D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVolTex)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD,
                                                      D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**,
                                                      HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateCubeTex)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT,
                                                       D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVB)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL,
                                                  IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateIB)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                  IDirect3DIndexBuffer9**, HANDLE*);

static PFN_Present       g_origPresent = nullptr;
static PFN_Reset         g_origReset   = nullptr;
static PFN_CreateTexture g_origCreateTex = nullptr;
static PFN_CreateVB      g_origCreateVB  = nullptr;
static PFN_CreateIB      g_origCreateIB  = nullptr;
static PFN_CreateVolTex  g_origCreateVolTex  = nullptr;
static PFN_CreateCubeTex g_origCreateCubeTex = nullptr;

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
    // ---- ⛔ do nothing at all on a device that is not operational ----
    //
    // 2026-08-06: a Reset failed with D3DERR_INVALIDCALL after an alt-tab out of exclusive
    // fullscreen. Nothing here checked, so for the next two minutes this hook kept calling
    // Clear(), GetBackBuffer(), GetRenderTargetData() and LockRect() on a LOST device every
    // frame, while an OpenXR compositor held shared surfaces. The machine then hard-froze -
    // Kernel-Power 41, no TDR recorded, reboot required.
    //
    // Whether that sequence caused the freeze is not proven. That it is invalid is not in
    // question: every one of those calls has undefined behaviour on a lost device, and a mod
    // has no business issuing them. Check first, and when the device is not ready, forward
    // Present and touch nothing else.
    {
        const HRESULT coop = dev->TestCooperativeLevel();
        if (coop != D3D_OK) {
            if (!g_deviceLost) {
                g_deviceLost = true;
                Log("*** [dev] NOT OPERATIONAL (0x%08lX) - suspending all mod work until it"
                    " recovers. No Clear, no capture, no XR submit.", (unsigned long)coop);
            }
            return g_origPresent(dev, src, dst, wnd, dirty);
        }
        if (g_deviceLost) {
            g_deviceLost = false;
            ReleaseFrameCapture();      // anything sized to the old backbuffer is now wrong
            Log("*** [dev] operational again - resuming");
        }
    }

    long f = InterlockedIncrement(&g_frames);
    if (f == 1) {
        Log("*** first Present - the game is rendering. This is the frame hook everything later hangs off.");
        DescribeBackbuffer(dev);
    }

    // ⚠️ OUTSIDE the XR block, and before the capture.
    //
    // It was inside `if (g_xrReady)`, so it only drew once an OpenXR session existed - which
    // made "run it on the monitor, no headset needed" wrong, and sent the player into Virtual
    // Desktop looking for a readout that could not appear there. A diagnostic that requires
    // the subsystem it is diagnosing is not a diagnostic.
    DrawOverlay(dev);

    // ---- rung 2: drive the XR frame loop from the game's Present ----
    //
    // Initialised once and never retried: a failed init is expensive, and repeating it every
    // frame would turn "no headset" into a stutter. A failure here leaves the game entirely
    // unaffected, which is the requirement.
    //
    // NOTE this paces the game to the headset's frame rate, because xrWaitFrame blocks until
    // the compositor is ready. That is intended and is how the reference works, but it means
    // frame-rate numbers taken from here are not comparable with an unmodded run.
    if (!g_xrTried) {
        g_xrTried = true;
        if (!InitXR()) Log("[xr] VR unavailable this run - continuing as a plain pass-through");
    }
    if (g_xrReady) {
        PumpXREvents();
        // Capture BEFORE the real Present: at this point the backbuffer holds the finished
        // frame. If it fails, g_haveFrame goes false and the quad shows the cycling test
        // colour instead - which makes success and failure distinguishable from inside the
        // headset, without reading the log.
        g_haveFrame = CaptureFrame(dev);
        SubmitTestQuad();
    }

    // ---- the object model scan, once, well into the run ----
    //
    // Start at frame 120. The old frame-900 delay was useful when this was only a census of live
    // instances, but motion setup needs class/property/UFunction metadata, all of which exists
    // without a spawned pawn. Live rig discovery already waits independently for gameplay. If
    // a package is genuinely late, the targeted arm-layout retry below handles it off-thread.
    if (f == 120) RunObjectModelScan();

    // Cheap once the object model is up: re-resolves only when the cached pointer stops
    // looking like a TdSwanNeck, which is on level transitions.
    InstallXInputHook();

    // AutoArm last, so a manual keypress on this frame is seen before the sequence acts on it.
    // Move-transition log first, so a state change and the [swan] reaction it provokes land
    // in cause-then-effect order within the frame.
    if (g_offName >= 0) {
        LogMoveTransitions();
        CheckSwanHotkey(); ApplySwanNeck(); CheckHeadHotkeys(); CheckVMHotkey();
        CheckVMWatchdog(); AutoArm();
    }

    // Sampled every frame - the peaks are what matter and a landing lasts a few frames.
    ProbeCameraAnimation();
    // Same window, and deliberately adjacent in the log: [pad] is what the runtime delivered,
    // [input] is what the game made of it. A "cannot move" report is answered by reading the
    // two together, and splitting them across different windows would mean pairing them by
    // frame number in a text editor.
    ProbeInputGates();
    {
        // Frame-precise split-flip detector: the first frames where the world's draws migrate
        // off the backbuffer, logged with the frame number so whatever else the game did in
        // that instant sits adjacent in the log. Three consecutive majority frames filter
        // menus and loading screens, which legitimately carry no backbuffer scene draws.
        static int offscreenStreak = 0, backbufferStreak = 0;
        static bool offscreenMajority = false;
        const bool off = g_frameSceneOffscreen > 50 &&
                         g_frameSceneOffscreen > g_frameSceneOnBackbuffer;
        const bool bb = g_frameSceneOnBackbuffer > 50 &&
                        g_frameSceneOnBackbuffer >= g_frameSceneOffscreen;
        offscreenStreak = off ? offscreenStreak + 1 : 0;
        backbufferStreak = bb ? backbufferStreak + 1 : 0;
        if (!offscreenMajority && offscreenStreak == 3) {
            offscreenMajority = true;
            Log("*** [rt] scene draws FLIPPED off the backbuffer at frame %ld"
                " (this frame: offscreen %ld vs backbuffer %ld)",
                f, g_frameSceneOffscreen, g_frameSceneOnBackbuffer);
        } else if (offscreenMajority && backbufferStreak == 3) {
            offscreenMajority = false;
            Log("*** [rt] scene draws returned to the backbuffer at frame %ld"
                " (this frame: backbuffer %ld vs offscreen %ld)",
                f, g_frameSceneOnBackbuffer, g_frameSceneOffscreen);
        }
        g_frameSceneOnBackbuffer = 0;
        g_frameSceneOffscreen = 0;
    }

    if ((f % 900) == 0) {
        ReportCameraAnimation();
        ReportPadState();
        ReportInputGates();
        if (g_simulStereo) ReportRenderTargets();
        if (g_simulStereo) ReportClears();
        // Alongside the target census: if the engine ever falls back to half-size LDR buffers
        // again (run 14), the headroom trend here says whether VRAM pressure did it.
        Log("[vram] approx available texture memory: %u MB",
            dev->GetAvailableTextureMem() / (1024u * 1024u));
    }

    // Six times a second is fast enough to place a transition against the acceptance windows it
    // is meant to explain, and it costs three reads on the frames it runs. It logs only when the
    // target CHANGES, so the rate sets resolution, not volume.
    if ((f % 10) == 0) {
        RetryMotionArmInitialization();
        RetryViewTargetOffset();
        ReportViewTarget();
        UpdateMotionRigDiscovery();
    }

    // Every 120 frames rather than every frame: it walks the census and can only act on a
    // sustained pattern anyway. Deliberately NOT inside the g_simulStereo report at 900 - the
    // whole point is to run while duplication is not happening.
    if ((f % 120) == 0) AdoptSceneTarget();
    // Arm the detailed sample for the NEXT frame, and report it the frame after. One frame of
    // per-draw state reads, not six hundred.
    if ((f % 600) == 0) { ReportUpDraws(); g_upSample = true; }
    else if (g_upSample) { ReportUpSample(); g_upSample = false; }

    // ---- the engine's FOV now matters ONLY for CPU culling ----
    //
    // The projection is forced in the matrix, so what the engine believes its FOV to be no
    // longer decides what is drawn - but it still decides what is SUBMITTED. Geometry culled
    // against a 84 degree frustum simply is not there to be seen in a 126 degree one, and
    // widening the matrix cannot conjure draw calls that were never issued.
    //
    // ⚠️ Derived from the VERTICAL, and the horizontal-only version was losing geometry.
    //
    // FOVAngle is HORIZONTAL. The engine turns it into a frustum using the aspect of the
    // BACKBUFFER - it knows nothing about the half-width viewports the eyes are drawn into - so
    // asking for 107.3 across a 16:9 frame culls against only 74.8 degrees vertically. We render
    // 100. Everything between the two is culled before it is ever submitted, and it appears as
    // scenery vanishing off the top and bottom of the view: "geometry disappearing from the top
    // of the building when looking up".
    //
    // Confirmed in the log, which reported both halves of it and was not read together: the
    // engine's own matrix measured 107.3 x 74.8, and 107.3/74.8 is 16:9 exactly.
    //
    // It is NOT the occlusion override, which is why forcing everything visible did nothing.
    // Occlusion culling drops geometry that IS in the frustum but hidden behind something;
    // frustum culling drops what is outside the frustum entirely. Different stage, different
    // switch, and no setting of one can compensate for the other.
    //
    // So the requirement is stated as a tangent and the wider of the two axes wins: give the
    // engine a horizontal FOV whose derived VERTICAL covers what we draw. At 100 degrees
    // vertical on a 16:9 frame that is about 135 degrees horizontal, well beyond the 93.3 the
    // horizontal alone would ask for. Margin is applied in tangent space, not to the angle,
    // because 15% of an angle near the asymptote is not 15% of a frustum.
    //
    // Written every frame because the camera update pulls it back toward the default each tick.
    if (g_fovForce && g_offFOVAngle >= 0 && g_targetHalfFovX > 0.0f && SceneH() > 0) {
        const uintptr_t ctl = FindPlayerController();
        if (ctl) {
            // The engine culls against the frame IT renders, so the aspect has to be the scene's.
            const float aspectFull = (float)SceneW() / (float)SceneH();
            const float needVert = tanf(g_targetHalfFovY) * aspectFull;  // to cover our vertical
            const float needHorz = tanf(g_targetHalfFovX);               // to cover our horizontal
            const float t = ((needVert > needHorz) ? needVert : needHorz) * 1.15f;
            const float want = atanf(t) * 2.0f * 57.29578f;
            // ⚠️ DesiredFOV first, and it is the one that matters.
            //
            // AdjustFOV runs every tick and ends with `FOVAngle = DesiredFOV` whenever
            // FOVZoomRate is 0, which is its default. FOVAngle is an OUTPUT. Writing only that
            // put the value in a field the engine overwrote before the view was built, which is
            // why the measurement came back with 100% of scene matrices still at the engine's
            // own 58.7 degree vertical while the write log happily reported success every frame.
            //
            // FOVAngle is still written, so the current frame does not have to wait a tick for
            // AdjustFOV to propagate. DefaultFOV too, because the class initialises
            // DesiredFOV from it and anything re-running that would undo this.
            SIZE_T wrote = 0;
            auto put = [&](int off) {
                if (off >= 0)
                    WriteProcessMemory(GetCurrentProcess(), (LPVOID)(ctl + off),
                                       &want, sizeof(float), &wrote);
            };
            float cur = 0.0f;
            if (SafeRead(ctl + g_offFOVAngle, &cur, sizeof(float)) && fabsf(cur - want) > 0.5f) {
                put(g_offDesiredFOV);
                put(g_offDefaultFOV);
                put(g_offFOVAngle);
                if (++g_fovWrites == 1 || (g_fovWrites % 600) == 0)
                    Log("[fov] culling FOV write #%ld: engine had %.1f, asked for %.1f -> culls"
                        " %.1f vertical at %.2f aspect (we render %.1f x %.1f)"
                        "  [DesiredFOV %+d DefaultFOV %+d]",
                        g_fovWrites, cur, want,
                        atanf(t / aspectFull) * 114.5916f, aspectFull,
                        g_targetHalfFovX * 114.5916f, g_targetHalfFovY * 114.5916f,
                        g_offDesiredFOV, g_offDefaultFOV);
            }
        }
    }

    // ---- hold the frame cap where we want it ----
    //
    // Same shape as the FOV write and for the same reason: the config file is hash-checked and
    // cannot be edited, but the value the engine actually reads is a float in memory, and a
    // float in memory has no hash.
    //
    // Written every frame rather than once. Nothing observed pulls it back, but the FOV taught
    // that a field can be recomputed from somewhere else without warning, and a once-only write
    // is invisible when that happens - it reports success and then quietly stops being true.
    if (g_engineObj && g_offMaxSmoothFps >= 0 && g_fpsCap > 0.0f) {
        float cur = 0.0f;
        if (SafeRead(g_engineObj + g_offMaxSmoothFps, &cur, sizeof(float)) &&
            fabsf(cur - g_fpsCap) > 0.5f) {
            SIZE_T wrote = 0;
            WriteProcessMemory(GetCurrentProcess(), (LPVOID)(g_engineObj + g_offMaxSmoothFps),
                               &g_fpsCap, sizeof(float), &wrote);
            static long n = 0;
            if (++n == 1 || (n % 60) == 0)
                Log("*** [fps] cap %.0f -> %.0f (write %ld)", cur, g_fpsCap, n);
        }
    }

    // After the XR work, so a quit still gets posted on a frame where XR failed or stalled.
    CheckQuitHotkey();

    if (f > 1 && f % 600 == 0) {
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
    // Released before the Reset, not after. SYSTEMMEM does not block a Reset the way DEFAULT
    // does, but the backbuffer size is exactly what tends to change here, and a stale capture
    // chain sized to the old one is a format/size mismatch waiting to happen.
    ReleaseFrameCapture();
    HRESULT hr = g_origReset(dev, pp);
    Log("--- Reset returned hr=0x%08lX ---", (unsigned long)hr);
    if (FAILED(hr)) {
        // Loud, because this is the state the freeze happened in and it was previously just
        // another hex code in the log. A failed Reset leaves the device unusable, and the
        // engine will keep calling Present regardless.
        g_deviceLost = true;
        Log("*** [dev] RESET FAILED (0x%08lX%s) - the device is unusable. All mod work is",
            (unsigned long)hr, hr == D3DERR_INVALIDCALL ? " D3DERR_INVALIDCALL" : "");
        Log("*** [dev] suspended. If the game does not recover, quit with PAUSE.");
    }
    if (SUCCEEDED(hr)) {
        g_describedBackbuffer = false;   // re-measure, the surface is new
        DescribeBackbuffer(dev);
    }
    return hr;
}

// ================================================================ rung 8a: the D3D9Ex device
//
// The frame grab costs 4-5 ms of every frame, and the measured case for removing it is now
// concrete rather than assumed: uncapped the game reaches 75-80 fps, which is 12.5 ms a frame
// with roughly a third of it ours. Take the grab off the critical path and 8.3 ms - a true 120,
// one unique image per display period - is inside what the engine already demonstrates.
//
// Getting there needs the frame to stay on the GPU, which needs a SHARED surface, which needs a
// D3D9Ex device. The game asks for a plain one: rung 0 measured Direct3DCreate9 called twice per
// run and Direct3DCreate9Ex never.
//
// ---- ⚠️ the reason this was deferred for eight rungs ----
//
// D3D9Ex does not support D3DPOOL_MANAGED AT ALL. Every MANAGED create fails outright on an Ex
// device, and rung 1 counted 5176 of them in a single run. The reference project translated them
// and records the wrapper as a live source of bugs for a hundred runs.
//
// So this rung swaps the device and translates the pool, and CHANGES NOTHING ELSE. The frame grab
// stays exactly as slow as it is. The only question it asks is whether the game runs at all on an
// Ex device, because that is the risky half, and answering it while the payoff half is also new
// would leave any failure with two plausible causes.
//
// ---- the translation ----
//
// MANAGED means "keep a system-memory copy and restore it for me when the device is lost". An Ex
// device is NEVER lost - that is the point of Ex, and the reason the pool was dropped from it -
// so the service MANAGED provides has no customer here. DEFAULT is the honest equivalent.
//
// Textures additionally need D3DUSAGE_DYNAMIC, because a DEFAULT texture cannot be locked and the
// engine fills these by locking them. Vertex and index buffers in DEFAULT can be locked as they
// are, so they get the pool change alone.
//
// Every translation that FAILS is logged with its format, usage and dimensions. If the game
// misbehaves, the failures name the resource rather than leaving it to be guessed at from a
// black texture.

static IDirect3D9Ex* g_d3d9ExObj  = nullptr;   // non-null once we have swapped the factory
static bool          g_wantEx     = true;      // cleared by the opt-out file  (g_devIsEx is
                                               // declared with the frame grab, which also needs it)
static LONG          g_remapTex   = 0, g_remapVB = 0, g_remapIB = 0;
static LONG          g_remapVol   = 0, g_remapCube = 0;
static LONG          g_remapFails = 0;

static HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels,
                                                    DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                    IDirect3DTexture9** out, HANDLE* shared)
{
    CountPool(pool);
    if (g_devIsEx && pool == D3DPOOL_MANAGED) {
        InterlockedIncrement(&g_remapTex);
        HRESULT hr = g_origCreateTex(dev, w, h, levels, usage | D3DUSAGE_DYNAMIC, fmt,
                                     D3DPOOL_DEFAULT, out, shared);
        if (FAILED(hr)) {
            // DYNAMIC is refused for some usage and format combinations - a render target, an
            // auto-mip chain, some compressed formats on some drivers. A plain DEFAULT texture
            // still works for anything the engine fills by other means, so try that before
            // giving up, and only report the case where both are refused.
            hr = g_origCreateTex(dev, w, h, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
            if (FAILED(hr)) {
                if (InterlockedIncrement(&g_remapFails) <= 20)
                    Log("[ex] texture remap FAILED hr=0x%08lX  %ux%u levels=%u usage=0x%08lX fmt=%d",
                        (unsigned long)hr, w, h, levels, (unsigned long)usage, (int)fmt);
            }
        }
        return hr;
    }
    HRESULT hr = g_origCreateTex(dev, w, h, levels, usage, fmt, pool, out, shared);
    // Run 14 entered a half-resolution LDR scene mode with a byte-identical config to the
    // full-resolution HDR runs, which points at a RUNTIME resource fallback: a UE3 engine that
    // cannot allocate its full-size HDR targets quietly builds smaller LDR ones instead. If
    // that is what happens, the refusal lands exactly here - name it and the VRAM headroom.
    if (FAILED(hr) &&
        ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0 ||
         (w >= 512 && h >= 512))) {
        static LONG reported = 0;
        if (InterlockedIncrement(&reported) <= 30)
            Log("*** [vram] CreateTexture FAILED hr=0x%08lX  %ux%u levels=%u usage=0x%08lX"
                " fmt=%d pool=%d  |  approx available texture mem %u MB",
                (unsigned long)hr, w, h, levels, (unsigned long)usage, (int)fmt, (int)pool,
                dev->GetAvailableTextureMem() / (1024u * 1024u));
    }
    return hr;
}

// ⚠️ Cube and volume textures take a pool too, and missing them is what crashed the first Ex run.
//
// The translation covered CreateTexture, CreateVertexBuffer and CreateIndexBuffer - the three
// that were already hooked for pool COUNTING, which is the whole reason those three and no
// others. The counting existed to size the problem, so it only ever needed to be representative;
// the translation has to be exhaustive, and inheriting one list for the other job silently
// changed what "complete" meant.
//
// A MANAGED cube or volume create on an Ex device fails outright and returns a null pointer. The
// engine does not check, and dereferences it. That is a crash before the first Present, with 96
// textures translated and none refused - which is exactly what the run reported.
//
// The complete set of pool-taking entry points in D3D9 is these five. CreateOffscreenPlainSurface
// also takes a pool but MANAGED is invalid there on ANY device, so the engine cannot be asking
// for it and there is nothing to translate.
static HRESULT STDMETHODCALLTYPE Hook_CreateVolTex(IDirect3DDevice9* dev, UINT w, UINT h, UINT d,
                                                   UINT levels, DWORD usage, D3DFORMAT fmt,
                                                   D3DPOOL pool, IDirect3DVolumeTexture9** out,
                                                   HANDLE* shared)
{
    CountPool(pool);
    if (g_devIsEx && pool == D3DPOOL_MANAGED) {
        InterlockedIncrement(&g_remapVol);
        HRESULT hr = g_origCreateVolTex(dev, w, h, d, levels, usage | D3DUSAGE_DYNAMIC, fmt,
                                        D3DPOOL_DEFAULT, out, shared);
        if (FAILED(hr)) {
            hr = g_origCreateVolTex(dev, w, h, d, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
            if (FAILED(hr) && InterlockedIncrement(&g_remapFails) <= 20)
                Log("[ex] volume texture remap FAILED hr=0x%08lX  %ux%ux%u levels=%u usage=0x%08lX fmt=%d",
                    (unsigned long)hr, w, h, d, levels, (unsigned long)usage, (int)fmt);
        }
        return hr;
    }
    return g_origCreateVolTex(dev, w, h, d, levels, usage, fmt, pool, out, shared);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCubeTex(IDirect3DDevice9* dev, UINT edge, UINT levels,
                                                    DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                    IDirect3DCubeTexture9** out, HANDLE* shared)
{
    CountPool(pool);
    if (g_devIsEx && pool == D3DPOOL_MANAGED) {
        InterlockedIncrement(&g_remapCube);
        HRESULT hr = g_origCreateCubeTex(dev, edge, levels, usage | D3DUSAGE_DYNAMIC, fmt,
                                         D3DPOOL_DEFAULT, out, shared);
        if (FAILED(hr)) {
            hr = g_origCreateCubeTex(dev, edge, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
            if (FAILED(hr) && InterlockedIncrement(&g_remapFails) <= 20)
                Log("[ex] cube texture remap FAILED hr=0x%08lX  edge=%u levels=%u usage=0x%08lX fmt=%d",
                    (unsigned long)hr, edge, levels, (unsigned long)usage, (int)fmt);
        }
        return hr;
    }
    return g_origCreateCubeTex(dev, edge, levels, usage, fmt, pool, out, shared);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateVB(IDirect3DDevice9* dev, UINT len, DWORD usage, DWORD fvf,
                                               D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared)
{
    CountPool(pool);
    if (g_devIsEx && pool == D3DPOOL_MANAGED) {
        InterlockedIncrement(&g_remapVB);
        HRESULT hr = g_origCreateVB(dev, len, usage, fvf, D3DPOOL_DEFAULT, out, shared);
        if (FAILED(hr) && InterlockedIncrement(&g_remapFails) <= 20)
            Log("[ex] vertex buffer remap FAILED hr=0x%08lX  len=%u usage=0x%08lX",
                (unsigned long)hr, len, (unsigned long)usage);
        return hr;
    }
    return g_origCreateVB(dev, len, usage, fvf, pool, out, shared);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateIB(IDirect3DDevice9* dev, UINT len, DWORD usage, D3DFORMAT fmt,
                                               D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared)
{
    CountPool(pool);
    if (g_devIsEx && pool == D3DPOOL_MANAGED) {
        InterlockedIncrement(&g_remapIB);
        HRESULT hr = g_origCreateIB(dev, len, usage, fmt, D3DPOOL_DEFAULT, out, shared);
        if (FAILED(hr) && InterlockedIncrement(&g_remapFails) <= 20)
            Log("[ex] index buffer remap FAILED hr=0x%08lX  len=%u usage=0x%08lX",
                (unsigned long)hr, len, (unsigned long)usage);
        return hr;
    }
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
    // The other two pool-taking creates. Hooked for the translation, not for counting - a MANAGED
    // cube or volume texture returns null on an Ex device and the engine does not check.
    g_origCreateVolTex  = (PFN_CreateVolTex) PatchVTable(dev, DEV_CreateVolumeTexture,
                                                         (void*)&Hook_CreateVolTex);
    g_origCreateCubeTex = (PFN_CreateCubeTex)PatchVTable(dev, DEV_CreateCubeTexture,
                                                         (void*)&Hook_CreateCubeTex);
    g_origSetRenderTarget = (PFN_SetRenderTarget) PatchVTable(dev, DEV_SetRenderTarget, (void*)&Hook_SetRenderTarget);
    g_origClear           = (PFN_Clear)           PatchVTable(dev, DEV_Clear, (void*)&Hook_Clear);
    g_origCreateQuery     = (PFN_CreateQuery)     PatchVTable(dev, DEV_CreateQuery, (void*)&Hook_CreateQuery);
    g_origDrawPrim    = (PFN_DrawPrim)    PatchVTable(dev, DEV_DrawPrimitive, (void*)&Hook_DrawPrim);
    g_origDrawIndexed = (PFN_DrawIndexed) PatchVTable(dev, DEV_DrawIndexedPrimitive, (void*)&Hook_DrawIndexed);
    g_origDrawPrimUP    = (PFN_DrawPrimUP)   PatchVTable(dev, DEV_DrawPrimitiveUP,
                                                         (void*)&Hook_DrawPrimUP);
    g_origDrawIndexedUP = (PFN_DrawIndexedUP)PatchVTable(dev, DEV_DrawIndexedPrimitiveUP,
                                                         (void*)&Hook_DrawIndexedUP);
    // Slot 94, extracted from d3d9.h and cross-checked against the reference's working hooks.
    g_origSetVSConstF = (PFN_SetVSConstF) PatchVTable(dev, DEV_SetVertexShaderConstantF,
                                                      (void*)&Hook_SetVSConstF);

    Log("[patch] device vtable patched: Present=%d Reset=%d CreateTexture=%d CreateVB=%d CreateIB=%d",
        DEV_Present, DEV_Reset, DEV_CreateTexture, DEV_CreateVertexBuffer, DEV_CreateIndexBuffer);

    // A null original is a patch that silently did nothing, and calling through it would
    // crash on the first frame. Say so loudly rather than discovering it in a minidump.
    if (!g_origPresent || !g_origReset || !g_origCreateTex || !g_origCreateVB ||
        !g_origCreateIB || !g_origClear)
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

    // ---- force WINDOWED ----
    //
    // Deferred at rung 2 as "an engine behaviour change we do not need yet". The freeze on
    // 2026-08-06 is the argument for doing it now: exclusive fullscreen loses the device on
    // every alt-tab, and this game's Reset then failed outright with D3DERR_INVALIDCALL. A
    // windowed device does not take that path at all, so the whole class of problem goes away
    // rather than being survived.
    //
    // It is also required eventually - the finished mod renders to the headset and the desktop
    // window is only a mirror - so this is bringing forward work already on the list, not
    // adding any.
    if (pp && pp->Windowed == FALSE) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;      // must be 0 for a windowed device
        Log("[dev] forcing WINDOWED (was exclusive fullscreen) - removes device loss on alt-tab");
    }

    // ---- Ex, if we swapped the factory ----
    //
    // ⚠️ The fallback CANNOT be a runtime toggle, and asking for one is reasonable but the
    // hardware disagrees: a device's type is fixed when it is created and there is no way to
    // change it later without destroying every resource in the game. So the choice is made here,
    // once, and it falls back three ways instead:
    //
    //   * an opt-out file beside the DLL forces the old path with no rebuild
    //   * CreateDeviceEx failing falls through to the plain call automatically
    //   * the FRAME GRAB stays on its old path regardless, so this rung cannot change how the
    //     picture is produced even if the device swap succeeds
    //
    // The last one is the important one. It keeps this rung answering exactly one question.
    HRESULT hr = E_FAIL;
    if (g_d3d9ExObj && self == (IDirect3D9*)g_d3d9ExObj && pp) {
        // D3DSWAPEFFECT_COPY is not valid on Ex. DISCARD is what the engine would have been
        // given anyway for a windowed swapchain of one buffer.
        if (pp->SwapEffect == D3DSWAPEFFECT_COPY) {
            pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
            Log("[ex] SwapEffect COPY is not valid on an Ex device - using DISCARD");
        }
        if (pp->BackBufferCount == 0) pp->BackBufferCount = 1;

        IDirect3DDevice9Ex* exDev = nullptr;
        // Windowed, so the display mode argument is NULL - and windowed is forced above, so
        // there is no fullscreen case to get wrong here.
        hr = g_d3d9ExObj->CreateDeviceEx(adapter, type, focus, behavior, pp, nullptr, &exDev);
        if (SUCCEEDED(hr) && exDev) {
            g_devIsEx = true;
            if (out) *out = (IDirect3DDevice9*)exDev;
            Log("*** [ex] CreateDeviceEx SUCCEEDED - device %p is D3D9Ex", (void*)exDev);
            Log("[ex] MANAGED allocations will be translated to DEFAULT from here on");
        } else {
            Log("[ex] CreateDeviceEx FAILED hr=0x%08lX - falling back to a plain device",
                (unsigned long)hr);
        }
    }

    if (!g_devIsEx) {
        hr = g_origCreateDevice(self, adapter, type, focus, behavior, pp, out);
    }
    Log("*** CreateDevice returned hr=0x%08lX  device=%p", (unsigned long)hr, out ? (void*)*out : nullptr);

    // The engine may have had its request adjusted. Log what came back as well as what was asked.
    if (SUCCEEDED(hr)) {
        LogPresentParams("granted", pp);
        // The window WM_CLOSE will be posted to. hDeviceWindow is preferred because that is
        // the surface the device actually presents to; focus is the fallback. Measured here
        // they are the same handle, but they are not required to be.
        g_gameWnd = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : focus;
        Log("[quit] hold PAUSE for %llu ms to close the game cleanly (window %p)",
            (unsigned long long)kQuitHoldMs, (void*)g_gameWnd);
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

// The opt-out. A file beside this DLL, so the old path can be forced from a headset-side
// keypress-free position: create the file, run, delete it. No rebuild, no ini - and the game's
// own config is hash-checked, so a file of our own is the only place a startup switch can live.
static void CheckExOptOut()
{
    static bool done = false;
    if (done) return;
    done = true;

    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&CheckExOptOut, &self);
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    _snwprintf_s(slash + 1, MAX_PATH - (slash + 1 - path), _TRUNCATE, L"mevr_noex.txt");

    // ⚠️ This file predates mevr.ini and is now the SECOND way to say the same thing. Kept, and
    // kept as the one that WINS, because the two are not equivalent in the case that matters: if
    // the Ex device is what stops the game reaching a state where anything can be read or edited,
    // the escape has to work without the ini being parsed at all. An empty file is also something
    // that can be created blind, from a file manager, by someone who has never opened the ini.
    //
    // D3D9Ex=off in mevr.ini is the normal way. This is the one that still works when the normal
    // way cannot be reached.
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        g_wantEx = false;
        Log("[ex] mevr_noex.txt found - staying on the plain D3D9 device"
            " (overrides D3D9Ex in mevr.ini)");
    }
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
    CheckExOptOut();

    // ---- hand back an Ex factory in place of the plain one ----
    //
    // IDirect3D9Ex derives from IDirect3D9, so the game can hold it, call it and release it
    // without knowing. The CreateDevice slot is at the same index in both vtables - Ex only
    // appends - so the existing patch covers it unchanged, and CreateDeviceEx is reached through
    // the stored interface pointer rather than through a second hook.
    //
    // Both factories the game creates get the same treatment. Which one is real is still
    // reported by the `self` argument in the hook, exactly as before.
    IDirect3D9* d3d = nullptr;
    if (g_wantEx) {
        pfn_Create9Ex fnEx = (pfn_Create9Ex)Real("Direct3DCreate9Ex");
        IDirect3D9Ex* ex = nullptr;
        if (fnEx && SUCCEEDED(fnEx(SDKVersion, &ex)) && ex) {
            g_d3d9ExObj = ex;
            d3d = (IDirect3D9*)ex;
            Log("[ex] Direct3DCreate9Ex used in place of Direct3DCreate9 -> IDirect3D9Ex* %p",
                (void*)ex);
        } else {
            Log("[ex] Direct3DCreate9Ex unavailable - using the plain factory");
        }
    }
    if (!d3d) {
        pfn_Create9 fn = (pfn_Create9)Real("Direct3DCreate9");
        d3d = fn ? fn(SDKVersion) : nullptr;
    }

    long call = InterlockedIncrement(&g_createCalls);
    Log("*** Direct3DCreate9 CALLED (call #%ld) SDKVersion=%u -> IDirect3D9* %p%s",
        call, SDKVersion, (void*)d3d, g_d3d9ExObj ? "  (Ex)" : "");

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

    Log("=== Mirror's Edge VR %s attached %04u-%02u-%02u %02u:%02u:%02u - run starts here ===",
        MEVR_VERSION, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Log("[host] %s", exe);
    Log("[host] pid %lu, this DLL is %u-bit",
        GetCurrentProcessId(), (unsigned)(sizeof(void*) * 8));
    // Built here rather than written as a literal so it cannot drift from the compiler that
    // actually produced the binary - "which build is this" is the first question a bug report
    // has to answer, and a hand-maintained string answers it wrongly eventually.
    Log("[host] built %s %s with MSC %d", __DATE__, __TIME__, (int)_MSC_VER);
}

// ---------------------------------------------------------------- settings
//
// mevr.ini, looked for BESIDE THIS DLL first and in the log directory second. Absent from both,
// everything keeps its compiled default - the file can only ever move a setting the hotkeys
// could already move, so there is no state here that a run without it cannot reach.
//
// Beside the DLL is the primary location because that is what shipping looks like: drop
// d3d9.dll and mevr.ini into Binaries together and the mod is installed and configured. The log
// directory is kept as a fallback for anyone who would rather not put files in the game folder,
// and because the log already lives there.
//
// ⚠️ Binaries is NOT the hash-checked directory - that is TdGame\Config, whose files the game
// verifies and refuses to start without, already measured at two runs. Nothing here goes near
// it, and nothing in mevr.ini corresponds to anything the game reads.
//
// Parsed by hand rather than through GetPrivateProfileString for one reason: that API cannot
// tell you about a key it was not asked for. A typo would silently do nothing, which in a mod
// configured while wearing a headset is the worst possible failure. Every line is either applied
// and logged, or rejected and logged.
static bool SettingBool(const char* v, bool* out)
{
    if (_stricmp(v, "1") == 0 || _stricmp(v, "true") == 0 ||
        _stricmp(v, "on") == 0 || _stricmp(v, "yes") == 0) { *out = true;  return true; }
    if (_stricmp(v, "0") == 0 || _stricmp(v, "false") == 0 ||
        _stricmp(v, "off") == 0 || _stricmp(v, "no") == 0) { *out = false; return true; }
    return false;
}

// Replace the filename on a full path. Returns false rather than truncating.
static bool PathSibling(const wchar_t* full, const wchar_t* leaf, wchar_t* out, size_t cap)
{
    if (!full || !full[0]) return false;
    wcscpy_s(out, cap, full);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) return false;
    const size_t room = cap - (size_t)(slash + 1 - out);
    if (wcslen(leaf) + 1 > room) return false;
    wcscpy_s(slash + 1, room, leaf);
    return true;
}

static void LoadSettings()
{
    wchar_t beside[MAX_PATH] = L"", fallback[MAX_PATH] = L"";
    wchar_t self[MAX_PATH] = L"";
    if (g_selfModule && GetModuleFileNameW(g_selfModule, self, MAX_PATH))
        PathSibling(self, L"mevr.ini", beside, MAX_PATH);
    PathSibling(g_logPath, L"mevr.ini", fallback, MAX_PATH);

    const wchar_t* path = nullptr;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (const wchar_t* cand : { (const wchar_t*)beside, (const wchar_t*)fallback }) {
        if (!cand[0]) continue;
        h = CreateFileW(cand, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { path = cand; break; }
    }
    if (h == INVALID_HANDLE_VALUE) {
        // Both paths named, because "it is not reading my settings" is otherwise a guessing
        // game about which directory the file should have been in.
        Log("[cfg] no mevr.ini - every setting is at its compiled default. Looked beside the DLL");
        Log("[cfg]   and beside the log; either location works, the DLL's own folder wins.");
        return;
    }
    {
        char shown[MAX_PATH * 2] = "";
        WideCharToMultiByte(CP_UTF8, 0, path, -1, shown, sizeof(shown), nullptr, nullptr);
        Log("[cfg] %s", shown);
    }
    char buf[8192];
    DWORD got = 0;
    const bool ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) != 0;
    CloseHandle(h);
    if (!ok) { Log("[cfg] mevr.ini could not be read"); return; }
    buf[got] = 0;

    // Skip a UTF-8 BOM. Notepad and most Windows editors offer "UTF-8 with BOM" and some
    // default to it, so a user who edits this file can easily add one - and we do not control
    // their editor. The three bytes are not spaces or tabs, so the trim below leaves them, the
    // first line then matches neither the comment test nor a key, and the file's own opening
    // comment is reported as rejected. A file whose FIRST line is a setting would lose it.
    char* text = buf;
    if (got >= 3 && (unsigned char)buf[0] == 0xEF
                 && (unsigned char)buf[1] == 0xBB
                 && (unsigned char)buf[2] == 0xBF) text += 3;

    int applied = 0, rejected = 0;
    char* ctx = nullptr;
    for (char* line = strtok_s(text, "\r\n", &ctx); line; line = strtok_s(nullptr, "\r\n", &ctx)) {
        while (*line == ' ' || *line == '\t') ++line;
        if (!*line || *line == ';' || *line == '#' || *line == '[') continue;

        char* eq = strchr(line, '=');
        if (!eq) { Log("[cfg]   ignored (no '='): %s", line); rejected++; continue; }
        *eq = 0;
        char* key = line;
        char* val = eq + 1;
        // trim both sides of both halves
        for (char* e = key + strlen(key); e > key && (e[-1]==' '||e[-1]=='\t'); --e) e[-1] = 0;
        while (*val == ' ' || *val == '\t') ++val;
        for (char* e = val + strlen(val); e > val && (e[-1]==' '||e[-1]=='\t'); --e) e[-1] = 0;

        // ---- the table. One row per setting; adding another is one row. ----
        //
        // The three animation locks are expressed as LOCKS, matching how they are spoken about,
        // and inverted into the FOLLOW flags the code carries. Naming them "follow" in the file
        // would invert the meaning of `0` relative to every conversation about them.
        bool b = false;
        if (_stricmp(key, "FrameCap") == 0) {
            const float f = (float)atof(val);
            if (f >= 20.0f && f <= 1000.0f) { g_fpsCap = f; Log("[cfg]   FrameCap = %.0f", f); applied++; }
            else { Log("[cfg]   FrameCap '%s' out of range 20..1000 - ignored", val); rejected++; }
        } else if (_stricmp(key, "LockAnimPitch") == 0) {
            if (SettingBool(val, &b)) { g_animFollow = !b; Log("[cfg]   LockAnimPitch = %s", b?"on":"off"); applied++; }
            else { Log("[cfg]   LockAnimPitch '%s' is not a boolean - ignored", val); rejected++; }
        } else if (_stricmp(key, "LockAnimRoll") == 0) {
            if (SettingBool(val, &b)) { g_animRollFollow = !b; Log("[cfg]   LockAnimRoll = %s", b?"on":"off"); applied++; }
            else { Log("[cfg]   LockAnimRoll '%s' is not a boolean - ignored", val); rejected++; }
        } else if (_stricmp(key, "Debug") == 0) {
            if (SettingBool(val, &b)) {
                g_debug = b;
                Log("[cfg]   Debug = %s%s", b ? "on" : "off",
                    b ? "" : "  (no overlay, no hotkeys except PAGE UP, F6 and hold-PAUSE)");
                applied++;
            } else { Log("[cfg]   Debug '%s' is not a boolean - ignored", val); rejected++; }
        } else if (_stricmp(key, "MotionHands") == 0) {
            if (SettingBool(val, &b)) {
                g_motionHands = b;
                Log("[cfg]   MotionHands = %s%s", b ? "on" : "off",
                    b ? "  (Phase 1.3 two-hand position control enabled)" : "");
                applied++;
            } else { Log("[cfg]   MotionHands '%s' is not a boolean - ignored", val); rejected++; }
        } else if (_stricmp(key, "MotionHandsDebug") == 0) {
            if (SettingBool(val, &b)) {
                g_motionHandsDebug = b;
                Log("[cfg]   MotionHandsDebug = %s", b ? "on" : "off");
                applied++;
            } else { Log("[cfg]   MotionHandsDebug '%s' is not a boolean - ignored", val); rejected++; }
        } else if (_stricmp(key, "D3D9Ex") == 0) {
            // ⚠️ Not a hotkey, and cannot be. A device's TYPE is fixed when it is created, so
            // switching it later would mean destroying every resource in the game. This is the
            // only place the choice can be made, which is exactly why it belongs in the file
            // rather than behind Debug: a machine where the Ex device misbehaves needs the way
            // out to be reachable by someone who is playing, not developing.
            if (SettingBool(val, &b)) {
                g_wantEx = b;
                Log("[cfg]   D3D9Ex = %s%s", b ? "on" : "off",
                    b ? "" : "  (plain D3D9 - also forces the slow frame grab, which needs Ex)");
                applied++;
            } else { Log("[cfg]   D3D9Ex '%s' is not a boolean - ignored", val); rejected++; }
        } else if (_stricmp(key, "FastCapture") == 0) {
            // The grab path CAN change at runtime - it is chosen per frame and owns nothing the
            // game can see - so NUMPAD8 toggles it too. The file exists because the hotkey is
            // behind Debug, and "this machine tears with the shared surface" is a setting a
            // player needs, not a development affordance.
            if (SettingBool(val, &b)) {
                g_fastCapture = b;
                Log("[cfg]   FastCapture = %s%s", b ? "on" : "off",
                    b ? "  (shared surface, stays on the GPU)" : "  (CPU round trip, ~4 ms/frame)");
                applied++;
            } else { Log("[cfg]   FastCapture '%s' is not a boolean - ignored", val); rejected++; }
        } else if (_stricmp(key, "LockAnimYaw") == 0) {
            if (SettingBool(val, &b)) { g_animYawFollow = !b; Log("[cfg]   LockAnimYaw = %s", b?"on":"off"); applied++; }
            else { Log("[cfg]   LockAnimYaw '%s' is not a boolean - ignored", val); rejected++; }
        } else {
            Log("[cfg]   unknown key '%s' - ignored", key);
            rejected++;
        }
    }
    Log("[cfg] %d applied, %d ignored. Hotkeys still override anything set here.",
        applied, rejected);
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        g_selfModule = mod;
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            g_qpcFreq = (double)f.QuadPart;
        }
        BuildLogPath();
        ArchivePreviousLog();
        LogHeader();
        // Straight after the header, so what a run was configured with is the first thing in the
        // log rather than something to be inferred from behaviour further down.
        LoadSettings();
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("");
        Log("=== SUMMARY ===");
        Log("Direct3DCreate9 calls : %ld", g_createCalls);
        Log("frames presented      : %ld", g_frames);
        Log("device                : %s", g_devIsEx ? "D3D9Ex" : "plain D3D9");
        Log("pool DEFAULT          : %ld", g_poolDefault);
        Log("pool MANAGED          : %ld   <- translated to DEFAULT on an Ex device", g_poolManaged);
        Log("pool SYSTEMMEM        : %ld", g_poolSystemMem);
        Log("pool other            : %ld", g_poolScratch);
        if (g_devIsEx) {
            Log("MANAGED translated    : %ld textures, %ld cube, %ld volume, %ld VB, %ld IB",
                g_remapTex, g_remapCube, g_remapVol, g_remapVB, g_remapIB);
            Log("translations REFUSED  : %ld   <- anything above zero is a resource the game"
                " asked for and did not get", g_remapFails);
        }
        Log("=== detached ===");
    }
    return TRUE;
}
