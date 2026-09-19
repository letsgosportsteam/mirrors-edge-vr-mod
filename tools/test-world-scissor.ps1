$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/stereo-effects-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content (Join-Path $root 'src/d3d9.cpp') -Raw
$guard=[regex]::Match($source,'(?ms)^struct WorldDrawScissor \{.*?^\};').Value
$draw=[regex]::Match($source,'(?ms)^template <typename FN>\s*static HRESULT DuplicateDraw\([^;{]*\)\s*\{.*?^\}').Value
if(!$guard -or !$draw){throw 'Missing production world draw helpers'}
$prefix=@'
#include <windows.h>
#include <d3d9.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
struct FakeDevice {
    D3DVIEWPORT9 vp{0,0,4224,2376,0,1};RECT rect{2600,700,3800,1600};
    DWORD scissor=1;bool failViewport=false;int stateWrites=0;
    float matrix[16]{};
    HRESULT GetViewport(D3DVIEWPORT9* out){*out=vp;return failViewport?E_FAIL:S_OK;}
    HRESULT SetViewport(const D3DVIEWPORT9* in){vp=*in;return S_OK;}
    HRESULT GetRenderState(D3DRENDERSTATETYPE state,DWORD* out){assert(state==D3DRS_SCISSORTESTENABLE);*out=scissor;return S_OK;}
    HRESULT SetRenderState(D3DRENDERSTATETYPE state,DWORD value){assert(state==D3DRS_SCISSORTESTENABLE);scissor=value;++stateWrites;return S_OK;}
};
#define IDirect3DDevice9 FakeDevice
static bool g_inDupDraw=false;
static int g_vmReg=0;
static volatile LONG g_dupDraws=0,g_dupFrameDraws=0;
static float g_sceneMat[16]{};
struct WorldScreenSampling {explicit WorldScreenSampling(FakeDevice*){} void Eye(int){}};
struct WorldGlassReflection {explicit WorldGlassReflection(FakeDevice*){}};
static void BuildEyeMatrix(float* out,int eye){memcpy(out,g_sceneMat,64);out[12]=(float)eye;}
static HRESULT g_origSetVSConstF(FakeDevice* dev,UINT,const float* in,UINT){memcpy(dev->matrix,in,64);return S_OK;}
'@
$tests=@'
int main(){
    FakeDevice dev;const auto vp=dev.vp;const auto rect=dev.rect;
    // This mono rectangle excludes the entire left eye and much of the right.
    // Both eye viewports must be able to render projected ground detail there.
    for(DWORD enabled:{0u,1u})for(int failEye:{-1,0,1}){
        dev.scissor=enabled;dev.stateWrites=0;int calls=0;
        const HRESULT hr=DuplicateDraw(&dev,[&]{const int eye=calls++;
            assert(g_inDupDraw&&dev.scissor==0);
            assert(dev.vp.X==(DWORD)(eye*2112)&&dev.vp.Width==2112);
            assert(dev.matrix[12]==(float)eye);
            return eye==failEye?E_FAIL:S_OK;
        });
        assert(calls==2&&hr==(failEye<0?S_OK:E_FAIL));
        assert(dev.scissor==enabled&&!memcmp(&dev.rect,&rect,sizeof(rect)));
        assert(!memcmp(&dev.vp,&vp,sizeof(vp))&&!memcmp(dev.matrix,g_sceneMat,64)&&!g_inDupDraw);
        assert(dev.stateWrites==(enabled?2:0));
    }
    dev.failViewport=true;dev.scissor=1;int calls=0;
    assert(DuplicateDraw(&dev,[&]{++calls;assert(dev.scissor==1&&!g_inDupDraw);return E_FAIL;})==E_FAIL);
    assert(calls==1);
    puts("PASS: world/decal draw bypasses mono scissor for both eyes, restores clip/viewport/matrix, retains either-eye failure and mono fallback");
}
'@
$cpp=Join-Path $out 'world-scissor.cpp'
($prefix+"`n"+$guard+"`n"+$draw+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'world-scissor.exe';$obj=Join-Path $out 'world-scissor.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'world scissor compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'world scissor tests failed'}
