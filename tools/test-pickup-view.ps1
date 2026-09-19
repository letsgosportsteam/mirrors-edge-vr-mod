$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/combat-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content (Join-Path $root 'src/d3d9.cpp') -Raw
$functions=foreach($name in @('PickupEyeCamera','PublishPickupView','ReadPickupView')) {
    $match=[regex]::Match($source,"(?ms)^static [^\r\n]+ $name\([^;{]*\)\s*\{.*?^\}")
    if(!$match.Success){throw "missing production $name"};$match.Value
}
$prefix=@'
#include <windows.h>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
struct MEVR_Vec3{float x=0,y=0,z=0;};
struct RenderedHeadFrame{MEVR_Vec3 position,forward,right,up;};
static SRWLOCK g_pickupViewLock=SRWLOCK_INIT;
static RenderedHeadFrame g_pickupView{};
static long g_pickupViewFrame=-100,g_frames=100;
static uintptr_t g_pickupViewPawn=0,g_playerPawn=42;
static bool g_sceneMatValid=true,g_simulStereo=true,g_sceneSplitMono=false,g_scenePartialMono=false,g_vmRow=true;
static float matrices[2][16]{};
static void BuildEyeMatrix(float* out,int eye){memcpy(out,matrices[eye],64);}
static float VecLength(MEVR_Vec3 v){return sqrtf(v.x*v.x+v.y*v.y+v.z*v.z);}
static bool FiniteVec(MEVR_Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
'@
$tests=@'
static bool close(float a,float b){return fabsf(a-b)<.005f;}
int main(){
    // Looking +X, off-center XR frusta; eye origins (100,+/-3,150).
    for(int eye=0;eye<2;++eye){float* q=matrices[eye];const float cy=eye?3.f:-3.f;
        q[0]=.2f;q[4]=2;q[12]=-20-2*cy;
        q[1]=-.3f;q[9]=3;q[13]=30-450;
        q[3]=1;q[15]=-100;
    }
    RenderedHeadFrame head{};head.up.z=1;
    PublishPickupView();assert(ReadPickupView(42,&head));
    assert(close(head.position.x,100)&&close(head.position.y,0)&&close(head.position.z,150));
    assert(close(head.forward.x,1)&&close(head.forward.y,0)&&close(head.forward.z,0)&&head.up.z==1);
    assert(!ReadPickupView(43,&head));
    g_frames=103;assert(ReadPickupView(42,&head));g_frames=104;assert(!ReadPickupView(42,&head));
    for(auto& q:matrices)for(int r=0;r<4;++r)for(int c=r+1;c<4;++c){float v=q[r*4+c];q[r*4+c]=q[c*4+r];q[c*4+r]=v;}
    g_vmRow=false;PublishPickupView();assert(ReadPickupView(42,&head));
    assert(close(head.position.x,100)&&close(head.position.y,0)&&close(head.position.z,150)&&close(head.forward.x,1));
    g_frames+=4;g_sceneSplitMono=true;PublishPickupView();assert(!ReadPickupView(42,&head));g_sceneSplitMono=false;
    g_scenePartialMono=true;PublishPickupView();assert(!ReadPickupView(42,&head));g_scenePartialMono=false;
    memset(matrices,0,sizeof(matrices));PublishPickupView();assert(!ReadPickupView(42,&head));
    matrices[0][0]=NAN;PublishPickupView();assert(!ReadPickupView(42,&head));
    puts("PASS: asymmetric stereo pickup ray, matrix conventions, normalized forward and fresh same-pawn snapshots");
}
'@
$cpp=Join-Path $out 'pickup-view.cpp'
($prefix+"`n"+($functions -join "`n")+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'pickup-view.exe';$obj=Join-Path $out 'pickup-view.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W3 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'pickup view compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'pickup view tests failed'}
