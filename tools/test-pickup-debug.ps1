$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/combat-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$code=@'
#include <windows.h>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <algorithm>
struct MEVR_Vec3{float x=0,y=0,z=0;};
struct RenderedHeadFrame{MEVR_Vec3 position,forward,right,up;};
static bool g_pickupDebug=true;static float g_worldScale=100;
static double NowMs(){return 100;}
static float VecLength(MEVR_Vec3 v){return sqrtf(v.x*v.x+v.y*v.y+v.z*v.z);}
static bool FiniteVec(MEVR_Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
#define D3DCOLOR_XRGB(r,g,b) ((DWORD)(0xff000000u|((r)<<16)|((g)<<8)|(b)))
#include "../../src/pickup_debug.inl"
int main(){
    PickupDebugSnapshot s{};s.headValid=true;s.head.position={100,200,300};s.head.forward={1,0,0};
    s.hand[0]={110,220,330};s.hand[1]={140,250,360};s.handValid[0]=s.handValid[1]=true;
    s.count=8;for(auto& t:s.targets){t.position={123,234,345};t.color=0xff123456;}
    struct Buffer{PickupDebugVertex lines[2048];unsigned guard=0x12345678;}buffer;
    int n=0;BuildPickupDebugLines(s,buffer.lines,&n);assert(n>500&&n<=2048&&n%2==0&&buffer.guard==0x12345678);
    for(int i=0;i<n;++i){const auto& v=buffer.lines[i];assert(std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z));
        if(v.color!=D3DCOLOR_XRGB(30,210,220))continue;
        float forward=(v.x-100)/100,side=((v.y-200)*(v.y-200)+(v.z-300)*(v.z-300))/10000;
        float radius=fmaxf(.12f,forward*.267949f);assert(side<=radius*radius+.00001f&&side+forward*forward<=4.0001f);
    }
    n=2048;BuildPickupDebugLines(s,buffer.lines,&n);assert(n==2048&&buffer.guard==0x12345678);
    s.head.forward={0,0,1};n=0;BuildPickupDebugLines(s,buffer.lines,&n);assert(n>0);
    s.until=200;PublishPickupDebug(s);PickupDebugSnapshot out;assert(ReadPickupDebug(&out));
    g_pickupDebug=false;assert(!ReadPickupDebug(&out));g_pickupDebug=true;s.until=99;PublishPickupDebug(s);assert(!ReadPickupDebug(&out));
    puts("PASS: bounded cone/catch/target geometry, actual gaze boundary, vertical view and debug expiry/toggle");
}
'@
$cpp=Join-Path $out 'pickup-debug.cpp'
$code|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'pickup-debug.exe';$obj=Join-Path $out 'pickup-debug.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W3 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'pickup debug compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'pickup debug tests failed'}
