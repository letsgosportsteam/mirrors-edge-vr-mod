$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/stereo-effects-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$main=Get-Content (Join-Path $root 'src/d3d9.cpp') -Raw
$camera=Get-Content (Join-Path $root 'src/camera_visibility.inl') -Raw
$functions=foreach($name in @('ApplyCameraRotation','ComposeScriptedCameraRotation','CameraVisibilityAfterScript','NativeLensFlareTick')) {
    $source=if($name -eq 'ApplyCameraRotation'){$main}else{$camera}
    $match=[regex]::Match($source,"(?ms)^static [^\r\n]+ $name\([^;{]*\)\s*\{.*?^\}")
    if(!$match.Success){throw "Missing production function $name"};$match.Value
}
$prefix=@'
#include <windows.h>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <initializer_list>
static bool g_visibilityCameraReady=true,g_simulStereo=true,g_sceneSplitMono=false,g_scenePartialMono=false,g_headTracking=true;
static uintptr_t g_visibilityCalc=10,g_visibilityRotationProperty=20,g_playerPawn=0,g_scriptedCameraPawn=0;
static int g_offCamRot=0;
static long g_frames=100,g_scriptedCameraFrame=-100;
static float g_cinematicYaw=0,g_cinematicPitch=0;
static int32_t g_scriptedCameraRotation[3]{};
static SRWLOCK g_scriptedCameraLock=SRWLOCK_INIT;
static bool requested=true,targetPawn=true,poseValid=true;
static uintptr_t failWrite=0;
static int writes=0;
static bool SafeRead(uintptr_t a,void* out,size_t n){if(!a||a==0xDEADBEEF)return false;memcpy(out,(void*)a,n);return true;}
static bool SafeU32(uintptr_t a,uint32_t* out){return SafeRead(a,out,4);}
static bool WriteRigBytes(uintptr_t a,const void* in,size_t n){if(a==failWrite)return false;++writes;memcpy((void*)a,in,n);return true;}
static bool CinematicHeadLookRequested(){return requested;}
static bool CurrentViewTargetIsPawn(uintptr_t){return targetPawn;}
static bool SampleScriptedHeadAngles(bool){return poseValid;}
static void Log(const char*,...){}
static bool g_lensFlares=false,g_visibilityFlareReady=true;
static bool g_nativeLensFlareSuppression=true;
static uint32_t g_flareClass=77,g_flareActiveMask=4;
static int g_flareActiveOffset=4;
static uintptr_t g_gobjAddr=0,g_flareSetActive=88;
static constexpr int kUObjectClassOff=0;
static int nativeCalls=0;
static bool LooksLikeRigObject(uintptr_t,const char*){return true;}
static bool ReadObjName(uintptr_t,char* out,size_t n){strncpy_s(out,n,"LensFlareComponent",_TRUNCATE);return true;}
static bool CombatCall(uintptr_t obj,uintptr_t fn,void* parms){
    assert(fn==88&&*(uint32_t*)parms==0);++nativeCalls;((uint32_t*)obj)[1]&=~4u;return true;
}
'@
$tests=@'
static void basis(const int32_t rot[3],float* m){
    const float k=3.14159265358979323846f/32768;
    float p=rot[0]*k,y=rot[1]*k,r=rot[2]*k,cp=cosf(p),sp=sinf(p),cy=cosf(y),sy=sinf(y),cr=cosf(r),sr=sinf(r);
    const float f[]={cp*cy,cp*sy,sp},right[]={sr*sp*cy-cr*sy,sr*sp*sy+cr*cy,-sr*cp};
    const float up[]={-(cr*sp*cy+sr*sy),cy*sr-cr*sp*sy,cr*cp};
    memset(m,0,64);for(int a=0;a<3;++a){m[a*4]=right[a];m[a*4+1]=up[a];m[a*4+3]=f[a];}
}
int main(){
    for(int p:{-16000,-8000,0,8000,16000})for(int y:{-24000,0,24000})for(int r:{-7000,0,7000})
    for(float hy:{-2.7f,0.f,2.7f})for(float hp:{-.8f,0.f,.8f}){
        int32_t b[]={p,y,r},out[3];float rendered[16],early[16];basis(b,rendered);
        float right[]={rendered[0],rendered[4],rendered[8]},up[]={rendered[1],rendered[5],rendered[9]},zero[]={0,0,0};
        ApplyCameraRotation(rendered,true,right,hp,zero);ApplyCameraRotation(rendered,true,up,hy,zero);
        assert(ComposeScriptedCameraRotation(b,hy,hp,out));basis(out,early);
        for(int i=0;i<12;++i)assert(fabsf(rendered[i]-early[i])<.0003f);
        // The engine's culling basis and the rendered head basis coincide,
        // even when looking behind a stationary scripted camera.
    }
    int32_t base[]={1200,2000,-500},cached[3],returned[3],want[3];
    g_playerPawn=(uintptr_t)cached;uint32_t result=1,record[3]={20,(uint32_t)(uintptr_t)returned,0},frame[7]{};
    frame[6]=(uint32_t)(uintptr_t)record;
    g_cinematicYaw=2;g_cinematicPitch=.3f;
    auto reset=[&]{memcpy(cached,base,12);memcpy(returned,base,12);g_scriptedCameraPawn=0;writes=0;};
    reset();ComposeScriptedCameraRotation(base,2,.3f,want);
    CameraVisibilityAfterScript(g_playerPawn,10,frame,&result);
    assert(writes==2&&!memcmp(returned,want,12)&&!memcmp(cached,want,12));
    assert(g_scriptedCameraPawn==g_playerPawn&&g_scriptedCameraFrame==g_frames&&!memcmp(g_scriptedCameraRotation,want,12));
    reset();requested=false;CameraVisibilityAfterScript(g_playerPawn,10,frame,&result);assert(!writes);requested=true;
    reset();targetPawn=false;CameraVisibilityAfterScript(g_playerPawn,10,frame,&result);assert(!writes);targetPawn=true;
    reset();result=0;CameraVisibilityAfterScript(g_playerPawn,10,frame,&result);assert(!writes);result=1;
    reset();record[0]=99;record[2]=(uint32_t)(uintptr_t)record;
    CameraVisibilityAfterScript(g_playerPawn,10,frame,&result);assert(!writes);record[0]=20;record[2]=0;
    reset();returned[0]++;CameraVisibilityAfterScript(g_playerPawn,10,frame,&result);assert(!writes);
    reset();poseValid=false;CameraVisibilityAfterScript(g_playerPawn,10,frame,&result);assert(!writes);poseValid=true;
    reset();failWrite=g_playerPawn;CameraVisibilityAfterScript(g_playerPawn,10,frame,&result);
    assert(!memcmp(returned,base,12)&&!memcmp(cached,base,12)&&!g_scriptedCameraPawn);failWrite=0;
    reset();g_scenePartialMono=true;CameraVisibilityAfterScript(g_playerPawn,10,frame,&result);assert(!writes);g_scenePartialMono=false;
    puts("PASS: early camera equals rendered head rotation including steep/rolled/backward views; validated output/cache, gates, malformed records and rollback");
    uint32_t flare[2]={77,0x84},light[2]={78,0x84},inactive[2]={77,0x80};
    uint32_t objects[3]={(uint32_t)(uintptr_t)flare,(uint32_t)(uintptr_t)light,(uint32_t)(uintptr_t)inactive};
    uint32_t table[2]={(uint32_t)(uintptr_t)objects,3};g_gobjAddr=(uintptr_t)table;
    NativeLensFlareTick();assert(nativeCalls==1&&flare[1]==0x80&&light[1]==0x84&&inactive[1]==0x80);
    NativeLensFlareTick();assert(nativeCalls==1);++g_frames;g_lensFlares=true;flare[1]=0x84;
    NativeLensFlareTick();assert(nativeCalls==1&&flare[1]==0x84);
    ++g_frames;g_lensFlares=false;g_nativeLensFlareSuppression=false;
    NativeLensFlareTick();assert(nativeCalls==1&&flare[1]==0x84);
    puts("PASS: native flare API disables active lens-flare components only, preserves lights/other bits, obeys setting and per-frame bound");
}
'@
$cpp=Join-Path $out 'camera-visibility.cpp'
($prefix+"`n"+($functions -join "`n")+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'camera-visibility.exe';$obj=Join-Path $out 'camera-visibility.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'camera visibility compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'camera visibility tests failed'}
