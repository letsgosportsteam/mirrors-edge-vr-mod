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
    DEV_CreateQuery            = 118,
    DEV_DrawPrimitive          = 81,
    DEV_DrawIndexedPrimitive  = 82,
    DEV_SetVertexShaderConstantF = 94,
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

    Log("*** [xr] OpenXR session created. 32-bit OpenXR works in this process.");
    g_xrReady = true;
    return true;
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
// The SLOW path on purpose: backbuffer -> GetRenderTargetData -> SYSTEMMEM -> lock -> D3D11
// dynamic texture -> CopyResource into the XR swapchain image.
//
// It cost the Singularity project roughly 9.8 ms of a 16 ms frame at 4K, and it is still the
// right thing to build first. The fast route needs a D3D9Ex device, and D3D9Ex does not
// support D3DPOOL_MANAGED at all - so it drags in translating the 11,084 MANAGED allocations
// rung 1 counted, with a SYSTEMMEM shadow behind each one. That wrapper was a live source of
// bugs for a hundred runs in the reference. Proving the pipe end to end without it means any
// problem that shows up later has one plausible cause instead of two.
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
static bool CaptureFrame(IDirect3DDevice9* dev)
{
    if (!g_dev11) return false;
    const double t0 = NowMs();

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;

    D3DSURFACE_DESC d{};
    if (FAILED(bb->GetDesc(&d))) { bb->Release(); return false; }
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
static void ReportPoseHonesty();
static void TickPacing();

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
struct RtSeen { IDirect3DSurface9* surf; UINT w, h; D3DFORMAT fmt; long draws; };
extern RtSeen            g_rtSeen[16];
extern int               g_rtSeenCount;
extern float             g_sceneMat[16];
extern volatile LONG     g_dupDraws;
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
float                    g_yawLagRad  = 0.0f;    // head turn between the write and display
bool                     g_yawLagFix  = true;    // NUMPAD6
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
static XrView            g_views[2]{};
static bool              g_viewsValid = false;

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
    if (eye < 0 || eye > 1 || g_eyeSwap[eye] == XR_NULL_HANDLE || !g_upload) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage(g_eyeSwap[eye], &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    bool ok = false;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(g_eyeSwap[eye], &wi))) {
        if (g_simulStereo) {
            // Both eyes live in one frame side by side, so each swapchain takes its own half.
            D3D11_BOX box{};
            box.left  = (UINT)(eye == 0 ? 0 : g_capW / 2);
            box.right = box.left + g_capW / 2;
            box.top = 0; box.bottom = g_capH;
            box.front = 0; box.back = 1;
            g_ctx11->CopySubresourceRegion(g_eyeImages[eye][idx], 0, 0, 0, 0, g_upload, 0, &box);
        } else {
            g_ctx11->CopyResource(g_eyeImages[eye][idx], g_upload);
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
              if (g_haveFrame && g_upload) {
                // Legal despite the swapchain texture being TYPELESS: B8G8R8A8_UNORM and
                // B8G8R8A8_TYPELESS share a type group, so CopyResource is a raw bit copy.
                g_ctx11->CopyResource(g_scImages[idx], g_upload);
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

    // ---- stereo: locate the eyes, fill the one this frame rendered, submit a projection ----
    XrCompositionLayerProjection      proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView  projViews[2]{};
    bool stereoSubmitted = false;

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
            if (halfY > 0.1f && g_capH > 0) {
                g_targetHalfFovY = halfY;
                // Square pixels: the horizontal follows from the aspect of what ONE EYE
                // actually renders into. Under draw duplication that is half the backbuffer,
                // so the aspect halves - and this single value is used both to force the
                // matrix and to tell the compositor, which is the only way they can agree.
                const float aspect = g_simulStereo ? ((float)g_capW * 0.5f / (float)g_capH)
                                                   : ((float)g_capW / (float)g_capH);
                g_targetHalfFovX = atanf(tanf(halfY) * aspect);
                if (!g_fovLogged) {
                    g_fovLogged = true;
                    Log("[fov] headset wants %.1f deg vertical; targeting %.1f x %.1f at %.2f aspect",
                        halfY * 114.5916f, g_targetHalfFovX * 114.5916f,
                        g_targetHalfFovY * 114.5916f, aspect);
                }
            }
        }

        const uint32_t eyeW = g_simulStereo ? (g_capW / 2) : g_capW;
        if (g_viewsValid && EnsureEyeSwapchains(eyeW, g_capH)) {
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
                stereoSubmitted = true;
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
    TickPacing();
    ReportPoseHonesty();

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
        // The capture cost is the number that decides whether the D3D9Ex wrapper is worth
        // taking on. Reported as a mean over the window, and reset, so it tracks the current
        // scene rather than being flattened by the menu at startup.
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
float     g_fpsCap          = 120.0f;   // NUMPAD7 cycles; matches the headset by default
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
extern int g_offActorRotation;
extern int g_offActorLocation;
extern int g_offFOVAngle;
extern int g_offDesiredFOV;
extern int g_offDefaultFOV;
extern int g_offCamLoc;
extern int g_offCamRot;
extern int g_offMoveState;

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
                DumpClassProperties("TdPlayerController", 60);
                FindEngineObject();
            }
        }
    }
    Log("======== layout derivation took %.1f ms (off the render thread) ========",
        NowMs() - t0);
    Log("");
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
static int  g_swanMode = 0;          // 0 = original, 1 = zeroed, 2 = exaggerated 8x
static int  g_swanAppliedMode = -1;  // -1 = nothing written yet
static float g_swanSaved[4] = { 0, 0, 0, 0 };
static bool  g_swanSavedOk = false;

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

static uintptr_t FindSwanNeck()
{
    if (LooksLikeSwanInstance(g_swanNeck)) return g_swanNeck;

    if (g_swanNeck) {          // had one, lost it - a level transition, so try again promptly
        Log("[swan] cached instance %p is no longer valid, re-resolving", (void*)g_swanNeck);
        g_swanNeck = 0;
        g_swanAppliedMode = -1;
        g_swanSavedOk = false;
        g_swanNextTry = 0;
    }
    if (g_frames < g_swanNextTry) return 0;

    if (!g_gobjAddr || g_offName < 0) { g_swanNextTry = g_frames + kSwanBackoffFrames; return 0; }
    uint32_t data, count;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) {
        g_swanNextTry = g_frames + kSwanBackoffFrames; return 0;
    }

    const double t0 = NowMs();
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t obj;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        if (!LooksLikeSwanInstance(obj)) continue;
        g_swanNeck = obj;
        g_swanMissLogged = false;
        Log("[swan] live instance at %p (search took %.1f ms over %u slots)",
            (void*)obj, NowMs() - t0, count);
        return obj;
    }

    // Cost is reported the first time, because a search that finds nothing still costs the
    // full walk and that number is the reason the backoff exists.
    g_swanNextTry = g_frames + kSwanBackoffFrames;
    if (!g_swanMissLogged) {
        g_swanMissLogged = true;
        Log("[swan] no live TdSwanNeck yet - %.1f ms wasted over %u slots; backing off %ld frames",
            NowMs() - t0, count, kSwanBackoffFrames);
    }
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
            Log("[swan] original values: linear %.1f/%.1f  quadratic %.1f/%.1f",
                tmp[0], tmp[1], tmp[2], tmp[3]);
        }
    }

    if (g_swanMode == g_swanAppliedMode) return;
    if (!g_swanSavedOk) return;

    // ---- three states, not two, and the third is the one that proves anything ----
    //
    // Zeroing produces a subtle ABSENCE: the player is asked to notice that something they
    // never consciously noticed has stopped. That is a test which struggles to fail, and this
    // project has already lost runs to exactly that shape - it is why the ini experiment used
    // an absurd value rather than zero.
    //
    // The exaggerated state makes the channel's existence unmistakable. Once an 8x lean is
    // obviously there, "zeroed" becomes trustworthy, because the lever is known to reach the
    // camera. Reporting "I could tell something happened" is honest and soft; this turns it
    // into a yes or no.
    const float mul = (g_swanMode == 1) ? 0.0f : (g_swanMode == 2 ? 8.0f : 1.0f);
    const bool ok = WriteF32(obj + SWAN_LinearFwd,  g_swanSaved[0] * mul) &&
                    WriteF32(obj + SWAN_LinearDown, g_swanSaved[1] * mul) &&
                    WriteF32(obj + SWAN_QuadFwd,    g_swanSaved[2] * mul) &&
                    WriteF32(obj + SWAN_QuadDown,   g_swanSaved[3] * mul);

    const char* what = (g_swanMode == 1) ? "ZEROED - looking down should feel flat"
                     : (g_swanMode == 2) ? "EXAGGERATED 8x - looking down should LURCH forward and down"
                                         : "ORIGINAL values restored";
    Log("*** [swan] %s  (linear %.1f/%.1f quadratic %.1f/%.1f) %s",
        what, g_swanSaved[0] * mul, g_swanSaved[1] * mul,
        g_swanSaved[2] * mul, g_swanSaved[3] * mul,
        ok ? "" : "  <- A WRITE FAILED");
    g_swanAppliedMode = g_swanMode;
}

// F7 toggles it. A raw key read, like PAUSE - this is a test lever, and an A/B the player can
// perform without relaunching is worth far more than one that needs two runs to compare.
static void CheckSwanHotkey()
{
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
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t obj;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        if (!LooksLikePlayerController(obj)) continue;
        g_playerCtl = obj;
        g_ctlMissLogged = false;
        Log("[head] TdPlayerController instance at %p", (void*)obj);
        return obj;
    }
    // Backoff, for the reason recorded at FindSwanNeck: an uncached full walk on the render
    // thread costs hundreds of thousands of syscalls a frame.
    g_ctlNextTry = g_frames + 600;
    if (!g_ctlMissLogged) { g_ctlMissLogged = true; Log("[head] no live TdPlayerController yet"); }
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
        // Blended toward the measurement by the weight, not assigned. At weight 1 - anywhere
        // below about 72 degrees of pitch - this is exactly the old assignment. Near vertical
        // the weight falls to zero and the previous value simply holds, with no step at the
        // boundary. Taken the short way round the circle so a wrap does not spin the image.
        float r = 0.0f, w = 0.0f;
        if (GetHeadRoll(when, &r, &w)) {
            float d = r - g_headRoll;
            while (d >  3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            g_headRoll += w * d;
        }
        UpdateSixDof(when);

        // ---- trace the steep-pitch rotation, rather than guess at it a fourth time ----
        //
        // Three plausible causes have been fixed so far - the yaw singularity, the roll's
        // sensitivity near vertical, and the written pitch going over the top - and the image
        // still rotates when looking all the way up or down. Each of those was a real defect and
        // none was this one, so this stops proposing mechanisms and records the state instead.
        //
        // The decisive column is matRightZ: the z component of the camera's right axis, taken
        // from the ENGINE's own matrix before anything of ours touches it. UE3 is Z-up, so a
        // camera with no roll has a level right axis and this reads ~0. If it stays at 0 through
        // the event, the engine is not rolling and the rotation is ours - g_headRoll, applied by
        // ApplyRoll. If it swings, the roll is coming from the game and no amount of work on the
        // OpenXR side will touch it.
        //
        // headRoll and its weight are logged beside it so the two candidates can be told apart
        // in one run instead of one per hypothesis.
        {
            static int decim = 0;
            int32_t ty, tp;
            if (GetHeadYawPitch(when, &ty, &tp)) {
                const float pitchDeg = tp * (360.0f / 65536.0f);
                if (fabsf(pitchDeg) > 55.0f && (++decim % 6) == 0) {
                    float rz = 0.0f, fz = 0.0f, gamePitch = 0.0f;
                    if (g_sceneMatValid) {
                        const float rx = g_sceneMat[0], ry = g_sceneMat[4], rzz = g_sceneMat[8];
                        const float rl = sqrtf(rx*rx + ry*ry + rzz*rzz);
                        if (rl > 1e-6f) rz = rzz / rl;
                        const float fx = g_sceneMat[3], fy = g_sceneMat[7], fzz = g_sceneMat[11];
                        const float fl = sqrtf(fx*fx + fy*fy + fzz*fzz);
                        if (fl > 1e-6f) { fz = fzz / fl; gamePitch = asinf(fz) * 57.29578f; }
                    }
                    Log("[tilt] headPitch %+6.1f  headRoll %+7.1f (w %.2f)  |  camPitch %+6.1f"
                        "  matRightZ %+.4f  <- 0 means the ENGINE is not rolling",
                        pitchDeg, g_headRoll * 57.29578f, w, gamePitch, rz);
                }
            }
        }
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

        // Where the HEAD is pointing, and nothing else. The animation's share used to be added
        // here and that was the flaw: g_animNow is sampled in Present, so it described the
        // previous frame. With animations cancelled the term is zero and the target was exact -
        // which is why the mouse came right at 0% - but with them followed a stale term rode
        // along, and it is largest exactly when the animation moves fastest. Hence a mouse that
        // still juddered at 100%, and a hard landing that juddered worst of all.
        //
        // ApplyPitchFix derives the animation's share from live values instead.
        const float kRadPerUnit = 6.28318531f / 65536.0f;
        g_pitchTarget = (float)(hp * g_pitchSign) * kRadPerUnit;
        g_pitchTargetValid = true;
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

    if (d6 && !p6) { g_autoDone = true; ArmVMScan(false); }
    p6 = d6;

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
        else {
            Log("*** [vm] BEST: c%d %s in %s space  (score %.4f, %d candidates seen)",
                g_vmBestReg, g_vmBestRow ? "ROW" : "COL",
                g_vmBestWorld ? "WORLD" : "TRANSLATED-WORLD", g_vmBestScore, g_vmCandidates);
            // Committed only on a successful scan, so injection can never run against a
            // register nothing validated.
            g_vmReg = g_vmBestReg;
            g_vmRow = g_vmBestRow;
            Log("[vm] committed. F2 cycles the injection: off / forward / right / up (300 UU)");
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
static void AutoArm()
{
    if (g_autoDone) return;

    static int stage = 0;         // 0 = waiting for the world, 1 = scan armed
    static int wait = 0;
    static int retries = 0;
    static bool announced = false;

    if (!g_xrReady) return;       // no session, nothing to be stereo on
    if (wait > 0) { --wait; return; }

    if (stage == 0) {
        if (!ArmVMScan(true)) {   // quiet: at the main menu this fails every time by design
            if (!announced) { Log("[auto] waiting for a player pawn before arming the scan"); announced = true; }
            wait = 30;
            return;
        }
        stage = 1;
        wait = 10;                // the scan itself needs 2 frames; this is slack, not a poll
        return;
    }

    if (g_vmReg >= 0) {
        g_stereoMode = 1;
        g_simulStereo = true;
        g_eyeFilled[0] = g_eyeFilled[1] = false;
        InterlockedExchange(&g_dupDraws, 0);
        g_autoDone = true;
        Log("*** [auto] ready - stereo ON, simultaneous. No keypresses needed.");
        Log("[auto] F1 turns stereo off, F10 falls back to alternate-eye, F6 rescans.");
        return;
    }

    // The scan ran and committed nothing. Almost always a camera pose that was not usable yet,
    // which fixes itself a second later - so retry, and give up loudly rather than silently.
    if (++retries >= 10) {
        g_autoDone = true;
        Log("[auto] the scan committed no register after %d attempts - press F6 by hand", retries);
        return;
    }
    stage = 0;
    wait = 120;
}

extern bool g_overlay;
extern int   g_renderedEye;
extern float g_halfIpdUU;

static void CheckHeadHotkeys()
{
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
        static const float kCaps[] = { 120.0f, 60.0f, 90.0f, 250.0f, 62.0f };
        static int ci = 0;
        ci = (ci + 1) % (int)(sizeof(kCaps) / sizeof(kCaps[0]));
        g_fpsCap = kCaps[ci];
        Log("*** [fps] NUMPAD7 -> cap %.0f%s", g_fpsCap,
            (g_fpsCap == 60.0f) ? "  (divides 120 exactly - even cadence)" :
            (g_fpsCap == 62.0f) ? "  (the game's own default, for comparison)" : "");
    }
    pN7 = dN7;

    static bool pN6 = false;
    const bool dN6 = (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0;
    if (dN6 && !pN6) {
        g_yawLagFix = !g_yawLagFix;
        Log("*** [head] NUMPAD6 -> turn-lag compensation %s (last measured %.2f deg)",
            g_yawLagFix ? "ON" : "OFF", g_yawLagRad * 57.29578f);
    }
    pN6 = dN6;

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
    static bool pPgUp = false, pPgDn = false;
    const bool dPgUp = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
    const bool dPgDn = (GetAsyncKeyState(VK_NEXT)  & 0x8000) != 0;
    if (dPgUp && !pPgUp) { g_haveCentre = false; Log("*** [6dof] PAGE UP -> recentring"); }
    if (dPgDn && !pPgDn) {
        g_sixDof = !g_sixDof;
        Log("*** [6dof] PAGE DOWN -> %s", g_sixDof ? "ON" : "OFF (decaying to neutral)");
    }
    pPgUp = dPgUp; pPgDn = dPgDn;

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

uintptr_t g_playerPawn = 0;
static long      g_pawnNextTry = 0;
static bool      g_pawnMissLogged = false;

// Non-static: declared above, where the rung 5b code drives the pawn search.
bool LooksLikePlayerPawn(uintptr_t obj)
{
    if (!obj || g_offName < 0) return false;
    uint32_t vt;
    if (!SafeU32(obj, &vt) || !InModule(vt)) return false;
    if (!ObjNameIs(obj, "TdPlayerPawn")) return false;
    uint32_t cls;
    if (!SafeU32(obj + 0x34, &cls) || cls < 0x10000) return false;
    return ObjNameIs(cls, "TdPlayerPawn");
}

uintptr_t FindPlayerPawn()
{
    if (LooksLikePlayerPawn(g_playerPawn)) return g_playerPawn;
    if (g_playerPawn) { g_playerPawn = 0; g_pawnNextTry = 0; }
    if (g_frames < g_pawnNextTry) return 0;
    if (!g_gobjAddr || g_offName < 0) { g_pawnNextTry = g_frames + 600; return 0; }

    uint32_t data, count;
    if (!SafeU32(g_gobjAddr, &data) || !SafeU32(g_gobjAddr + 4, &count)) {
        g_pawnNextTry = g_frames + 600; return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t obj;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        if (!LooksLikePlayerPawn(obj)) continue;
        g_playerPawn = obj;
        g_pawnMissLogged = false;
        Log("[vm] TdPlayerPawn instance at %p", (void*)obj);
        return obj;
    }
    g_pawnNextTry = g_frames + 600;   // same backoff discipline as the other searches
    if (!g_pawnMissLogged) { g_pawnMissLogged = true; Log("[vm] no live TdPlayerPawn yet"); }
    return 0;
}

PFN_SetVSConstF g_origSetVSConstF = nullptr;

static volatile LONG g_vmScanArmed = 0;
volatile LONG g_vmWindowsTested = 0;
volatile LONG g_vmPoseFailures  = 0;
static int  g_vmBestReg = -1;
static bool g_vmBestRow = false;
bool  g_vmBestWorld = false;
float g_vmBestScore = -1e9f;
static int  g_vmCandidates = 0;

// Camera pose straight from the pawn's cached values - the same numbers CalcCamera produced.
static bool GetCameraPose(float* loc, float* fwd)
{
    if (g_offCamLoc < 0 || g_offCamRot < 0) return false;
    if (!LooksLikePlayerPawn(g_playerPawn)) return false;
    if (!SafeRead(g_playerPawn + g_offCamLoc, loc, 12)) return false;
    int32_t rot[3];
    if (!SafeRead(g_playerPawn + g_offCamRot, rot, 12)) return false;

    const float kToRad = 3.14159265f / 32768.0f;
    const float pitch = rot[0] * kToRad, yaw = rot[1] * kToRad;
    fwd[0] = cosf(pitch) * cosf(yaw);
    fwd[1] = cosf(pitch) * sinf(yaw);
    fwd[2] = sinf(pitch);
    return true;
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
                if (isScene) acc++; else rej++;
                if (lastFrame != g_frames) {
                    lastFrame = g_frames;
                    static long frames = 0;
                    if (++frames % 600 == 0) {
                        Log("[vm] over 600 frames: %ld accepted, %ld rejected (%.1f%%)  |"
                            " camera step %.0f UU/frame, tolerance %.0f",
                            acc, rej, 100.0f * (float)rej / (float)((acc + rej) ? (acc + rej) : 1),
                            g_pivotStep, tol);
                        acc = rej = 0;
                    }
                }
            }

            if (g_vmValidate && !isScene) {
                InterlockedIncrement(&g_vmRejected);
                return g_origSetVSConstF(dev, startReg, data, count);
            }
            InterlockedIncrement(&g_vmAccepted);

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

                // ---- how much further will the head have turned by the time this is seen ----
                //
                // The write in Present used the head pose at the predicted display time for the
                // frame BEFORE this one. This frame is displayed a period later, and the head
                // kept moving - that gap is the judder, and it is proportional to how fast the
                // player is turning, which is why it only shows during a turn.
                //
                // Asking the runtime the same question at two times measures it directly: no
                // stored state, no cross-thread handoff, and the runtime's own prediction rather
                // than a difference taken between frames and called a velocity.
                g_yawLagRad = 0.0f;
                if (g_yawLagFix && g_predTime && g_predPeriod) {
                    float y0 = 0.0f, y1 = 0.0f;
                    if (GetHeadYawRaw(g_predTime, &y0) &&
                        GetHeadYawRaw(g_predTime + g_predPeriod, &y1)) {
                        float d = y1 - y0;
                        while (d >  3.14159265f) d -= 6.28318531f;
                        while (d < -3.14159265f) d += 6.28318531f;
                        // Into UE3's yaw sense, the same conversion the write uses.
                        d *= (float)g_yawSign;
                        const float kMax = 20.0f * (3.14159265f / 180.0f);
                        if (d >  kMax) d =  kMax;
                        if (d < -kMax) d = -kMax;

                        // ---- is this correction steady, or is it the snapping? ----
                        //
                        // A lag correction should track head SPEED: near zero when still,
                        // growing smoothly with the turn. If instead it jumps frame to frame
                        // while the head is barely moving, the runtime's two predictions
                        // disagree by more than the head actually moved, and this rotates the
                        // whole world by that disagreement every frame. That would show as
                        // scenery snapping at slow head speeds - which is the report, and is
                        // exactly what a lag fix should NOT do.
                        //
                        // So both the value and its frame-to-frame jump are reported. A large
                        // jump against a small value is this; a small jump is not.
                        static float prev = 0.0f;
                        static float worstJump = 0.0f, worstVal = 0.0f;
                        static long  n = 0;
                        const float jump = fabsf(d - prev);
                        prev = d;
                        if (jump > worstJump) worstJump = jump;
                        if (fabsf(d) > worstVal) worstVal = fabsf(d);
                        if (++n >= 600) {
                            Log("[head] turn-lag over %ld frames: worst correction %.2f deg,"
                                " worst frame-to-frame jump %.2f deg", n,
                                worstVal * 57.29578f, worstJump * 57.29578f);
                            n = 0; worstJump = worstVal = 0.0f;
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
// Each eye gets half the width: 1280x1440. That costs horizontal resolution and keeps the
// frame grab at 3.7 MP / ~4.4 ms, so the D3D9Ex wrapper stays deferred. Widening the
// backbuffer to restore per-eye resolution doubles the pixels grabbed and is what finally
// forces that work - a separate decision, made when the geometry is known to be right.
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
                g_rtIsScene = (d.Width == g_capW && d.Height == g_capH);
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

static void ReportRenderTargets()
{
    Log("[rt] distinct render targets seen this window:");
    for (int i = 0; i < g_rtSeenCount; ++i) {
        Log("[rt]   %p  %4ux%-4u fmt %-3d  %8ld draws  %s",
            (void*)g_rtSeen[i].surf, g_rtSeen[i].w, g_rtSeen[i].h, (int)g_rtSeen[i].fmt,
            g_rtSeen[i].draws,
            (g_rtSeen[i].w == g_capW && g_rtSeen[i].h == g_capH) ? "<- scene-sized, DUPLICATED"
                                                                 : "");
        g_rtSeen[i].draws = 0;
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
        return;
    }

    const XrVector3f p = loc.pose.position;
    if (!g_haveCentre) {
        g_centre[0] = p.x; g_centre[1] = p.y; g_centre[2] = p.z;
        g_haveCentre = true;
        Log("[6dof] centre set at (%.3f, %.3f, %.3f) m", p.x, p.y, p.z);
        return;
    }

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

// Called once per Present. Buckets the interval since the last one by display periods.
static double g_lastPresentMs = 0.0;
static long   g_pace[6] = { 0, 0, 0, 0, 0, 0 };   // 0 = <1 period, 5 = 5 or more

static void TickPacing()
{
    const double now = NowMs();
    if (g_lastPresentMs > 0.0 && g_predPeriod) {
        const double periodMs = (double)g_predPeriod / 1.0e6;
        if (periodMs > 0.1) {
            int b = (int)((now - g_lastPresentMs) / periodMs + 0.5);
            if (b < 0) b = 0;
            if (b > 5) b = 5;
            g_pace[b]++;
        }
    }
    g_lastPresentMs = now;
}

// How far the head turned this frame, against how far the rendered image turned.
//
// If the compositor is told the view moved 3 degrees and the picture only moved 1, it will
// synthesise the missing 2 on every display frame it fills in - motion that was never rendered.
// That is indistinguishable from judder, it scales with turn rate, and it vanishes when still.
static void ReportPoseHonesty()
{
    if (!g_sceneMatValid || !g_predTime) return;

    float headYaw = 0.0f;
    if (!GetHeadYawRaw(g_predTime, &headYaw)) return;

    // The rendered image's yaw, from the matrix the frame was actually drawn with.
    const float fx = g_vmRow ? g_sceneMat[3] : g_sceneMat[12];
    const float fy = g_vmRow ? g_sceneMat[7] : g_sceneMat[13];
    if (fabsf(fx) + fabsf(fy) < 1e-6f) return;
    const float matYaw = atan2f(fy, fx);

    static bool  have = false;
    static float pHead = 0.0f, pMat = 0.0f;
    static float worst = 0.0f, sum = 0.0f;
    static long  n = 0;
    if (!have) { pHead = headYaw; pMat = matYaw; have = true; return; }

    auto wrap = [](float a) {
        while (a >  3.14159265f) a -= 6.28318531f;
        while (a < -3.14159265f) a += 6.28318531f;
        return a;
    };
    // Into a common sense: OpenXR yaw grows anticlockwise, UE3's clockwise.
    const float dHead = wrap(headYaw - pHead) * (float)g_yawSign;
    const float dMat  = wrap(matYaw  - pMat);
    pHead = headYaw; pMat = matYaw;

    const float gap = fabsf(dHead - dMat);
    if (gap > worst) worst = gap;
    sum += gap;
    if (++n >= 600) {
        Log("[xr] pose honesty over %ld frames: the image turned %.2f deg less than the"
            " compositor was told, worst frame %.2f deg  (0 = the layer is truthful)",
            n, (sum / (float)n) * 57.29578f, worst * 57.29578f);
        n = 0; worst = 0.0f; sum = 0.0f;
    }
}

static void ReportPacing()
{
    long total = 0;
    for (int i = 0; i < 6; ++i) total += g_pace[i];
    if (!total) return;
    Log("[xr] cadence over %ld frames - display periods per delivered frame:"
        "  <1:%ld  1:%ld  2:%ld  3:%ld  4:%ld  5+:%ld   (one bucket = even)",
        total, g_pace[0], g_pace[1], g_pace[2], g_pace[3], g_pace[4], g_pace[5]);
    for (int i = 0; i < 6; ++i) g_pace[i] = 0;
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
    // Negated for the same reason ApplyYawFix negates: a positive turn about world up in this
    // primitive's sense LOWERS the rotator's yaw.
    ApplyCameraRotation(m, rowStorage, up, -g_yawLagRad, RotationPivot());
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
        vp.X     = (eye == 0) ? 0 : g_capW / 2;
        vp.Width = g_capW / 2;
        dev->SetViewport(&vp);
        BuildEyeMatrix(eyeMat, eye);
        g_origSetVSConstF(dev, (UINT)g_vmReg, eyeMat, 4);
        hr = issue();
    }

    dev->SetViewport(&vpWas);
    g_origSetVSConstF(dev, (UINT)g_vmReg, g_sceneMat, 4);   // leave c0 as the engine left it
    g_inDupDraw = false;
    InterlockedIncrement(&g_dupDraws);
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
    if (g_rtCurrent) g_rtCurrent->draws++;
    if (!(g_simulStereo && !g_inDupDraw && g_sceneMatValid && g_c0IsScene && g_rtIsScene &&
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

static void DrawOverlay(IDirect3DDevice9* dev)
{
    if (!g_overlay || !dev) return;

    // Sized with headroom and bounds-checked below. The rows are edited per test by design,
    // and this array had silently grown to nine entries in an eight-row buffer - /analyze
    // caught it, but the next row added would have been a stack overwrite in a Present hook.
    char lines[16][64];
    int nl = 0;

    // ---- rows for the CURRENT test. Delete freely; see the note at the top. ----
    _snprintf_s(lines[nl++], 64, _TRUNCATE, "MEVR FPS %d GRAB %d MS",
                (int)(g_capSamples ? (1000.0 / (g_capMsTotal / g_capSamples + 0.001)) : 0),
                (int)(g_capSamples ? (g_capMsTotal / g_capSamples) : 0.0));
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
    // Frame 900 rather than a low number, on the Singularity project's evidence: it dumped at
    // frame 120 for three runs, which lands in the MAIN MENU before any level is loaded, and
    // saw only UClass objects and zero live instances. GNames and GObjects themselves exist
    // from startup, so the scan is still valid there - but the class probe is far more useful
    // once a level is up, and 900 frames is cheap insurance either way.
    if (f == 900) RunObjectModelScan();

    // Cheap once the object model is up: re-resolves only when the cached pointer stops
    // looking like a TdSwanNeck, which is on level transitions.
    // AutoArm last, so a manual keypress on this frame is seen before the sequence acts on it.
    if (g_offName >= 0) {
        CheckSwanHotkey(); ApplySwanNeck(); CheckHeadHotkeys(); CheckVMHotkey(); AutoArm();
    }

    // Sampled every frame - the peaks are what matter and a landing lasts a few frames.
    ProbeCameraAnimation();
    if ((f % 900) == 0) { ReportCameraAnimation(); if (g_simulStereo) ReportRenderTargets(); }

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
    if (g_fovForce && g_offFOVAngle >= 0 && g_targetHalfFovX > 0.0f && g_capH > 0) {
        const uintptr_t ctl = FindPlayerController();
        if (ctl) {
            const float aspectFull = (float)g_capW / (float)g_capH;
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
    g_origSetRenderTarget = (PFN_SetRenderTarget) PatchVTable(dev, DEV_SetRenderTarget, (void*)&Hook_SetRenderTarget);
    g_origCreateQuery     = (PFN_CreateQuery)     PatchVTable(dev, DEV_CreateQuery, (void*)&Hook_CreateQuery);
    g_origDrawPrim    = (PFN_DrawPrim)    PatchVTable(dev, DEV_DrawPrimitive, (void*)&Hook_DrawPrim);
    g_origDrawIndexed = (PFN_DrawIndexed) PatchVTable(dev, DEV_DrawIndexedPrimitive, (void*)&Hook_DrawIndexed);
    // Slot 94, extracted from d3d9.h and cross-checked against the reference's working hooks.
    g_origSetVSConstF = (PFN_SetVSConstF) PatchVTable(dev, DEV_SetVertexShaderConstantF,
                                                      (void*)&Hook_SetVSConstF);

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

    HRESULT hr = g_origCreateDevice(self, adapter, type, focus, behavior, pp, out);
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
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            g_qpcFreq = (double)f.QuadPart;
        }
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
