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
static XrSwapchain          g_swapchain  = XR_NULL_HANDLE;
static ID3D11Texture2D**    g_scImages   = nullptr;
static uint32_t             g_scImageCount = 0;
static uint32_t             g_scW = 0, g_scH = 0;
static int64_t              g_scFormat = 0;      // the format we ASKED for; the texture may be typeless
static bool                 g_rtvFailLogged = false;

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

    const XrCompositionLayerBaseHeader* layers[1];
    uint32_t layerCount = 0;
    if (submitted) {
        quad.space      = g_xrSpace;
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

    XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
    fei.displayTime          = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount           = layerCount;
    fei.layers               = layerCount ? layers : nullptr;
    XrResult er = xrEndFrame(g_xrSession, &fei);

    if (n == 1)        Log("*** [xr] first XR frame submitted, xrEndFrame -> %d", (int)er);
    else if (n % 600 == 0) {
        // The capture cost is the number that decides whether the D3D9Ex wrapper is worth
        // taking on. Reported as a mean over the window, and reset, so it tracks the current
        // scene rather than being flattened by the menu at startup.
        const double mean = g_capSamples ? (g_capMsTotal / g_capSamples) : 0.0;
        Log("[xr] frame %ld  state=%d shouldRender=%d endFrame=%d  |  frame grab %.2f ms mean over %ld",
            n, (int)g_xrState, (int)fs.shouldRender, (int)er, mean, g_capSamples);
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
    Log("[obj] image base %p  .text %p-%p  .data %p-%p",
        (void*)g_imgBase, (void*)g_textLo, (void*)g_textHi, (void*)g_dataLo, (void*)g_dataHi);
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

static bool FindGObjects()
{
    // Same TArray shape, but the elements are UObject* whose first dword is a vtable pointing
    // into .text. Requiring that of a sample is what separates it from any other pointer array.
    for (uintptr_t a = g_dataLo; a + 12 < g_dataHi; a += 4) {
        if (a == g_gnamesAddr) continue;
        uint32_t data, count, maxn;
        if (!SafeU32(a, &data) || !SafeU32(a + 4, &count) || !SafeU32(a + 8, &maxn)) continue;
        if (data < 0x10000) continue;
        if (count < 1000 || count > 4000000) continue;
        if (maxn < count || maxn > count * 4 + 1024) continue;

        int checked = 0, vtblOk = 0;
        for (uint32_t i = 0; i < count && checked < 64; ++i) {
            uint32_t obj;
            if (!SafeU32(data + i * 4, &obj)) break;
            if (obj == 0) continue;             // UE3 leaves holes where objects were destroyed
            checked++;
            uint32_t vtbl;
            if (SafeU32(obj, &vtbl) && vtbl >= g_textLo && vtbl < g_textHi) vtblOk++;
        }
        if (checked >= 32 && vtblOk >= checked * 9 / 10) {
            g_gobjAddr = a;
            Log("*** [obj] GObjects at %p  Data=%p Count=%u Max=%u  (%d/%d sampled objects have"
                " a vtable inside .text)", (void*)a, (void*)data, count, maxn, vtblOk, checked);
            if (g_gnamesAddr)
                Log("[obj]     GObjects - GNames = 0x%X   (was 0x30 in the Singularity build)",
                    (unsigned)(a - g_gnamesAddr));
            return true;
        }
    }
    Log("[obj] GObjects NOT FOUND");
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
    int hits[kMaxOff / 4] = {};
    int sampled = 0;

    for (uint32_t i = 0; i < count && sampled < 400; ++i) {
        uint32_t obj;
        if (!SafeU32(data + i * 4, &obj) || obj < 0x10000) continue;
        uint32_t vtbl;
        if (!SafeU32(obj, &vtbl) || vtbl < g_textLo || vtbl >= g_textHi) continue;
        sampled++;
        for (int off = 4; off < kMaxOff; off += 4) {
            uint32_t v;
            if (!SafeU32(obj + off, &v)) continue;
            char nm[128];
            if (NameOf(v, nm, sizeof(nm))) hits[off / 4]++;
        }
    }

    int best = -1, bestOff = -1, second = -1;
    for (int off = 4; off < kMaxOff; off += 4) {
        int h = hits[off / 4];
        if (h > best) { second = best; best = h; bestOff = off; }
        else if (h > second) second = h;
    }
    Log("[obj] UObject layout scored over %d objects:", sampled);
    for (int off = 4; off < kMaxOff; off += 4)
        if (hits[off / 4] > 0)
            Log("[obj]     +0x%02X decoded as a name %d/%d", off, hits[off / 4], sampled);

    if (bestOff > 0 && best >= sampled * 9 / 10 && best > second) {
        g_offName = bestOff;
        Log("*** [obj] UObject::Name at +0x%02X (%d/%d, next best %d)", bestOff, best, sampled, second);
    } else {
        Log("[obj] Name offset NOT decisive (best +0x%02X %d/%d, second %d) - do not trust it",
            bestOff, best, sampled, second);
    }
}

// Reports whether the classes we actually care about are present and findable by name.
// A layout that scores well but cannot find TdPlayerPawn has not proven anything useful.
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
        if (!SafeU32(obj, &vtbl) || vtbl < g_textLo || vtbl >= g_textHi) continue;
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
static DWORD WINAPI ObjectModelThread(LPVOID)
{
    Log("");
    Log("======== object model scan (rung 4a, background thread) ========");
    const double t0 = NowMs();
    FindSections();
    if (FindGNames()) {
        char nm[128];
        for (uint32_t i = 0; i < 5; ++i)
            if (NameOf(i, nm, sizeof(nm))) Log("[obj]     GNames[%u] = \"%s\"", i, nm);
        if (FindGObjects()) {
            DetectUObjectLayout();
            ProbeKnownObjects();
        }
    }
    Log("======== scan took %.1f ms (off the render thread) ========", NowMs() - t0);
    Log("");
    return 0;
}

static void RunObjectModelScan()
{
    if (g_objModelDone) return;
    g_objModelDone = true;
    HANDLE h = CreateThread(nullptr, 0, ObjectModelThread, nullptr, 0, nullptr);
    if (h) { CloseHandle(h); Log("[obj] scan started on a background thread"); }
    else   { Log("[obj] CreateThread failed (%lu) - scan skipped", GetLastError()); }
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
    long f = InterlockedIncrement(&g_frames);
    if (f == 1) {
        Log("*** first Present - the game is rendering. This is the frame hook everything later hangs off.");
        DescribeBackbuffer(dev);
    }

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
