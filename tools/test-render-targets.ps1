# Compile the production render-target hook, mono guard and duplication gate against a
# fake D3D device. No game, GPU or headset is needed; no copy of the decision logic lives here.
param([switch]$SimulateStaleCache)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Get-Content (Join-Path $root 'src/d3d9.cpp') -Raw
function Slice-Source([string]$start, [string]$end) {
    $a = $source.IndexOf($start)
    $b = $source.IndexOf($end, $a)
    if ($a -lt 0 -or $b -le $a) { throw "production test boundary missing: $start" }
    $source.Substring($a, $b - $a)
}
$tracking = Slice-Source 'typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderTarget)' 'static void ReportRenderTargets()'
if ($SimulateStaleCache) {
    $a = $tracking.IndexOf('                if (g_rtCurrent && (g_rtCurrent->w != d.Width')
    $b = $tracking.IndexOf('                if (!g_rtCurrent)', $a)
    if ($a -lt 0 -or $b -lt 0) { throw 'descriptor-refresh boundary missing' }
    $tracking = $tracking.Remove($a, $b - $a)
}
$gate = Slice-Source 'static bool ShouldDuplicate()' '// ================================================================ diag: the user-pointer draws'
$worldUP = Slice-Source 'static volatile LONG g_worldUpStereoDraws=0;' 'static void ReportUpDraws()'
$preamble = @'
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <deque>
typedef int D3DFORMAT;
#define D3D_OK S_OK
struct D3DSURFACE_DESC { UINT Width, Height; D3DFORMAT Format; };
struct IDirect3DSurface9 {
    D3DSURFACE_DESC desc{};
    HRESULT result = S_OK;
    HRESULT GetDesc(D3DSURFACE_DESC* out) { *out = desc; return result; }
};
using D3DRENDERSTATETYPE=int;
enum { D3DRS_COLORWRITEENABLE,D3DRS_ZENABLE,D3DRS_STENCILENABLE,D3DRS_STENCILWRITEMASK,
    D3DRS_TWOSIDEDSTENCILMODE,D3DRS_STENCILFAIL,D3DRS_STENCILZFAIL,D3DRS_STENCILPASS,
    D3DRS_CCW_STENCILFAIL,D3DRS_CCW_STENCILZFAIL,D3DRS_CCW_STENCILPASS };
enum {D3DZB_FALSE=0,D3DSTENCILOP_KEEP=1,D3DSTENCILOP_INCR=7};
using D3DPRIMITIVETYPE=int;
enum {D3DPT_TRIANGLELIST=4};
struct IDirect3DVertexShader9 {void Release(){}};
struct IDirect3DDevice9 {
    DWORD color=15,depth=1;bool hasShader=true,usesVP=true;IDirect3DVertexShader9 shader;
    DWORD stencil=0,mask=255,twoSided=0,front=D3DSTENCILOP_KEEP,back=D3DSTENCILOP_KEEP;
    HRESULT GetRenderState(int state,DWORD* out){
        switch(state){case D3DRS_COLORWRITEENABLE:*out=color;break;case D3DRS_ZENABLE:*out=depth;break;
        case D3DRS_STENCILENABLE:*out=stencil;break;case D3DRS_STENCILWRITEMASK:*out=mask;break;
        case D3DRS_TWOSIDEDSTENCILMODE:*out=twoSided;break;case D3DRS_STENCILPASS:*out=front;break;
        case D3DRS_CCW_STENCILZFAIL:*out=back;break;default:*out=D3DSTENCILOP_KEEP;break;}return S_OK;}
    HRESULT GetVertexShader(IDirect3DVertexShader9** out){*out=hasShader?&shader:nullptr;return S_OK;}
};
static int primCalls=0,indexedCalls=0,eyeSplits=0;
static void NoteUpDraw(IDirect3DDevice9*,UINT){}
struct EffectPassCapture {EffectPassCapture(IDirect3DDevice9*,const char*,UINT){}};
struct SunDrawCapture {SunDrawCapture(IDirect3DDevice9*,unsigned,UINT){}};
static void CaptureEffectDraw(IDirect3DDevice9*,UINT,UINT,const void* =nullptr,UINT=0,const char* ="UP"){}
template<class F> static bool DrawStereoSunHaze(IDirect3DDevice9*,F,HRESULT*){return false;}
template<class F> static bool DrawStereoDistantSun(IDirect3DDevice9*,F,HRESULT*){return false;}
template<class F> static bool DrawStereoShadowProjection(IDirect3DDevice9*,F,HRESULT*){return false;}
template<class F> static bool DrawStereoCanvas(IDirect3DDevice9*,F,HRESULT*){return false;}
static bool SuppressLensFlare(IDirect3DDevice9*){return false;}
static bool WorldUPUsesSceneTransform(IDirect3DDevice9* dev){return dev->hasShader&&dev->usesVP;}
static bool ShouldDuplicateUI(IDirect3DDevice9*){return false;}
template<class F> static HRESULT DuplicateDraw(IDirect3DDevice9*,F issue){++eyeSplits;issue();return issue();}
template<class F> static HRESULT DuplicateViewportOnly(IDirect3DDevice9*,F issue){return issue();}
static HRESULT g_origDrawPrimUP(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,const void*,UINT){++primCalls;return S_OK;}
static HRESULT g_origDrawIndexedUP(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT){++indexedCalls;return S_OK;}
struct RtSeen { IDirect3DSurface9* surf; UINT w, h; D3DFORMAT fmt; long draws; long sceneDraws; };
UINT g_capW = 4224, g_capH = 2376, g_sceneW = 0, g_sceneH = 0;
bool g_sceneSplitMono = false, g_scenePartialMono = false;
bool g_simulStereo = true, g_inDupDraw = false, g_sceneMatValid = true, g_c0IsScene = true;
int g_vmReg = 0;
float g_halfIpdUU = 3.17f;
long g_frames = 0, g_frameSceneOnBackbuffer = 0, g_frameSceneOffscreen = 0;
struct Hand { bool reachEngaged; double diagnosticExcess, diagnosticDistance; };
Hand g_leftDetach{}, g_rightDetach{};
void Log(const char*, ...) {}
HRESULT bindResult = S_OK;
HRESULT STDMETHODCALLTYPE Bind(IDirect3DDevice9*, DWORD, IDirect3DSurface9*) { return bindResult; }
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (0)
'@
$tests = @'
int main() {
    g_origSetRenderTarget = Bind;
    IDirect3DSurface9 hdr{{1280,720,113}}, ldr{{4224,2376,21}};
    Hook_SetRenderTarget(nullptr, 0, &hdr);
    CHECK(!ShouldDuplicate());
    g_rtCurrent->draws = 500000;
    g_rtCurrent->sceneDraws = 5000;
    // Same COM address, new surface lifetime: the allocator reuse seen at startup.
    hdr.desc = {4224,2376,113};
    Hook_SetRenderTarget(nullptr, 0, &hdr);
    CHECK(ShouldDuplicate());
    CHECK(g_rtCurrent->draws == 1 && g_rtCurrent->sceneDraws == 1);
    g_rtCurrent->sceneDraws = 23368;
    Hook_SetRenderTarget(nullptr, 0, &ldr);
    g_rtCurrent->sceneDraws = 7255;
    AdoptSceneTarget();
    CHECK(!g_sceneSplitMono && !g_scenePartialMono);

    // The reverse reuse must stop duplication, and real reduced rendering must stay mono.
    hdr.desc = {1280,720,113};
    Hook_SetRenderTarget(nullptr, 0, &hdr);
    CHECK(!ShouldDuplicate());
    g_rtCurrent->sceneDraws = 23368;
    Hook_SetRenderTarget(nullptr, 0, &ldr);
    g_rtCurrent->sceneDraws = 7255;
    AdoptSceneTarget();
    CHECK(g_sceneSplitMono);
    CHECK(!ShouldDuplicate());
    hdr.desc = {4224,2376,113};
    Hook_SetRenderTarget(nullptr, 0, &hdr);
    g_rtCurrent->sceneDraws = 23000;
    AdoptSceneTarget();
    CHECK(!g_sceneSplitMono && ShouldDuplicate());

    // Failed binds and secondary MRT slots must not replace the current target.
    RtSeen* current = g_rtCurrent;
    bindResult = E_FAIL;
    CHECK(FAILED(Hook_SetRenderTarget(nullptr, 0, &ldr)));
    CHECK(current == g_rtCurrent);
    bindResult = S_OK;
    Hook_SetRenderTarget(nullptr, 1, &ldr);
    CHECK(current == g_rtCurrent);

    // Startup fills the old 16-entry table. A later target still needs a census entry.
    IDirect3DSurface9 later[40];
    for (auto& s : later) {
        s.desc = {4224,2376,21};
        Hook_SetRenderTarget(nullptr, 0, &s);
        CHECK(g_rtCurrent && g_rtCurrent->surf == &s && ShouldDuplicate());
    }
    CHECK(g_rtSeenCount == 42);
    CHECK(current->surf == &hdr); // stable storage while the table grows

    RtSeen reduced{nullptr,1280,720,113,0,0};
    CHECK(IsReducedSceneColorTarget(reduced,4224,2376));
    CHECK(IsReducedSceneColorTarget(reduced,2560,1440));
    CHECK(!IsReducedSceneColorTarget(reduced,1280,720));
    reduced.fmt = 36;
    CHECK(!IsReducedSceneColorTarget(reduced,4224,2376));
    reduced.fmt = 21;
    CHECK(IsReducedSceneColorTarget(reduced,4224,2376));
    reduced.w = 2112; reduced.h = 1188;
    CHECK(IsReducedSceneColorTarget(reduced,4224,2376));

    ResetRenderTargetTracking();
    CHECK(!g_rtCurrent && g_rtSeenCount == 0 && g_rtSeen.empty());
    CHECK(!g_sceneSplitMono && !g_scenePartialMono && !g_sceneW && !g_sceneH);
    CHECK(!ShouldDuplicate());
    // A genuine partial reduced scene at auto resolution must still engage the guard.
    hdr.desc = {1280,720,113};
    Hook_SetRenderTarget(nullptr, 0, &hdr);
    g_rtCurrent->sceneDraws = 3000;
    Hook_SetRenderTarget(nullptr, 0, &ldr);
    g_rtCurrent->sceneDraws = 9000;
    AdoptSceneTarget();
    CHECK(g_scenePartialMono && !ShouldDuplicate());
    for (int i = 0; i < 2; ++i) {
        g_rtCurrent->sceneDraws = 9000;
        AdoptSceneTarget();
    }
    CHECK(!g_scenePartialMono && ShouldDuplicate());
    IDirect3DDevice9 dev;
    CHECK(ShouldDuplicateWorldUP(&dev));
    Hook_DrawPrimUP(&dev,0,8,nullptr,32);CHECK(primCalls==2&&eyeSplits==1);
    Hook_DrawIndexedUP(&dev,0,0,16,8,nullptr,0,nullptr,32);CHECK(indexedCalls==2&&eyeSplits==2);
    dev.color=0;CHECK(!ShouldDuplicateWorldUP(&dev)); // occlusion geometry
    dev.stencil=1;CHECK(!ShouldDuplicateWorldUP(&dev)); // read-only stencil
    dev.front=D3DSTENCILOP_INCR;CHECK(ShouldDuplicateWorldUP(&dev)); // shadow mask
    dev.mask=0;CHECK(!ShouldDuplicateWorldUP(&dev));dev.mask=255;
    dev.front=D3DSTENCILOP_KEEP;dev.back=D3DSTENCILOP_INCR;
    CHECK(!ShouldDuplicateWorldUP(&dev));dev.twoSided=1;CHECK(ShouldDuplicateWorldUP(&dev));
    dev.usesVP=false;CHECK(!ShouldDuplicateWorldUP(&dev));dev.usesVP=true;
    dev.stencil=0;dev.twoSided=0;
    dev.color=15;dev.depth=0;CHECK(ShouldDuplicateWorldUP(&dev)); // depth-disabled world flare
    dev.usesVP=false;CHECK(!ShouldDuplicateWorldUP(&dev)); // HUD/postprocess inheriting c0
    dev.depth=1;CHECK(!ShouldDuplicateWorldUP(&dev)); // same false positive with depth enabled
    dev.usesVP=true;
    dev.depth=1;dev.hasShader=false;CHECK(!ShouldDuplicateWorldUP(&dev));
    dev.hasShader=true;g_c0IsScene=false;CHECK(!ShouldDuplicateWorldUP(&dev)); // foreign matrix
    Hook_DrawPrimUP(&dev,0,8,nullptr,32);CHECK(primCalls==3&&eyeSplits==2);
    g_c0IsScene=true;g_sceneSplitMono=true;CHECK(!ShouldDuplicateWorldUP(&dev));
    g_sceneSplitMono=false;g_inDupDraw=true;CHECK(!ShouldDuplicateWorldUP(&dev));
    std::puts("PASS: render-target lifetime/mono guards and world UP stereo routing; UI, occlusion and foreign transforms excluded");
}
'@
$output = Join-Path ([System.IO.Path]::GetTempPath()) ('mevr-rt-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$cpp = Join-Path $output 'render-target-test.cpp'
Set-Content -LiteralPath $cpp -Value ($preamble + "`n" + $tracking + "`n" + $gate + "`n" + $worldUP + "`n" + $tests) -Encoding utf8
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
Push-Location $output
try {
    cmd /c "`"$vcvars`" x86 >nul && cl /nologo /EHsc /W3 /std:c++17 render-target-test.cpp /Fe:render-target-test.exe"
    if ($LASTEXITCODE -ne 0) { throw 'render-target regression harness failed to compile' }
    & (Join-Path $output 'render-target-test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'render-target regression failed' }
} finally { Pop-Location }
