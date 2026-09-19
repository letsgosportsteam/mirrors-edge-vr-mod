$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/combat-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$sources=@((Get-Content (Join-Path $root 'src/d3d9.cpp') -Raw),(Get-Content (Join-Path $root 'src/combat_interaction.inl') -Raw),(Get-Content (Join-Path $root 'src/combat_spatial.inl') -Raw))
$functions=foreach($name in @('FiniteVec','VecLength','RotateByQuaternion','XrViewToUECamera','CameraVectorToWorld','CombatSampleRelease','CombatSelectGaze','CombatSpatialAfterScript')) {
    $found=$false
    foreach($source in $sources){$match=[regex]::Match($source,"(?ms)^static [^\r\n]+ $name\([^;{]*\)\s*\{.*?^\}");if($match.Success){$match.Value;$found=$true;break}}
    if(!$found){throw "Missing $name"}
}
$prefix=@'
#include <cmath>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "../../src/combat_gestures.h"
#include "../../src/combat_spatial_math.h"
struct MEVR_Vec3{float x,y,z;};
struct XrQuaternionf{float x,y,z,w;};
struct RenderedHeadFrame{MEVR_Vec3 position,forward,right,up;};
using XrTime=int64_t;using XrSpace=int;
enum{XR_NULL_HANDLE=0,XR_TYPE_SPACE_LOCATION=1,XR_TYPE_SPACE_VELOCITY=2,
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT=1,XR_SPACE_LOCATION_POSITION_TRACKED_BIT=2,
    XR_SPACE_VELOCITY_LINEAR_VALID_BIT=1,XR_SPACE_VELOCITY_ANGULAR_VALID_BIT=2};
#define XR_FAILED(x) ((x)<0)
struct XrSpaceLocation{int type;void* next=nullptr;int locationFlags=3;struct{XrQuaternionf orientation{0,0,0,1};}pose;};
struct XrSpaceVelocity{int type;void* next=nullptr;int velocityFlags=3;MEVR_Vec3 linearVelocity{},angularVelocity{};};
static bool g_motionPoseHeadValid=true;
static RenderedHeadFrame g_motionPoseHead={{},{1,0,0},{0,1,0},{0,0,1}};
static XrSpace g_xrSpace=1,g_viewSpace=2,g_sLGripPose=3,g_sRGripPose=4;
static float g_worldScale=100;
static const char* g_combatPickupGate="idle";
static CombatGripDetector g_combatGripDetector;
static int holding=1,velocityFlags=3,locationFlags=3;static bool failLocate=false;
static XrQuaternionf headQ{0,0,0,1};static MEVR_Vec3 releaseLinear{1,2,-3},releaseAngular{0,1,0};
static int CombatHoldingHand(){return holding;}
static int xrLocateSpace(XrSpace space,XrSpace,XrTime,XrSpaceLocation* out){
    if(failLocate)return -1;out->locationFlags=locationFlags;
    if(space==g_viewSpace)out->pose.orientation=headQ;
    if(out->next){auto& v=*(XrSpaceVelocity*)out->next;v.linearVelocity=releaseLinear;v.angularVelocity=releaseAngular;v.velocityFlags=velocityFlags;}
    return 0;
}
struct CombatRequest {uintptr_t pawn=0,weapon=0;int action=0;long frame=0;MEVR_Vec3 velocity{},angular{};bool motionValid=false;double time=0;};
struct Entry{uint32_t object=0;MEVR_Vec3 position{};bool alive=true,eligible=true,visible=true;};
static Entry g_combatPickups[5]{};
static bool CombatPickupAlive(uint32_t id,uint32_t,MEVR_Vec3* position){*position=g_combatPickups[id-1].position;return g_combatPickups[id-1].alive;}
static bool CombatPickupEligible(uintptr_t,uint32_t id,uint32_t){return g_combatPickups[id-1].eligible;}
static bool CombatPickupVisible(uintptr_t,MEVR_Vec3,MEVR_Vec3 p){for(const auto& e:g_combatPickups)if(e.position.x==p.x&&e.position.y==p.y&&e.position.z==p.z)return e.visible;return false;}
static bool g_combatSpatialReady=true,g_combatWatchingDrop=false;
static uintptr_t g_combatPickupOverrideManager=100;
static uint32_t g_combatPickupOverride=55,registered=0;
static unsigned g_combatPickupOverrideHits=0;
enum{CSetPickupAmmo,CFindPickup};struct Func{uint32_t fn;};static Func g_combatFunctions[2]={{11},{12}};
static void CombatRegisterPickup(uint32_t object){registered=object;}
static bool WriteRigBytes(uintptr_t ptr,const void* data,size_t size){memcpy((void*)ptr,data,size);return true;}
'@
$tests=@'
static bool close(float a,float b){return fabsf(a-b)<0.001f;}
int main(){
    assert(CombatGazeScore(1,0,1)>=0);
    assert(CombatGazeScore(-1,0,1)<0);
    assert(CombatGazeScore(2.1f,0,2.1f)<0);
    assert(CombatGazeScore(1,0.04f,1.02f)>=0); // 11 degrees: usable gaze tolerance
    assert(CombatGazeScore(1,0.16f,1.08f)<0); // outside the 15 degree cone
    assert(CombatGazeScore(NAN,0,1)<0);
    float point[3]={0,0,0},a[3]={-1,0,0},b[3]={1,0,0};
    assert(close(CombatSegmentDistanceSquared(point,a,b),0)); // a fast toss crosses between ticks
    point[1]=0.19f;assert(CombatSegmentDistanceSquared(point,a,b)>0.18f*0.18f);
    point[0]=2;point[1]=0;assert(close(CombatSegmentDistanceSquared(point,a,b),1));
    g_combatGripDetector.previousTime=1;
    CombatRequest r;CombatSampleRelease(&r);
    assert(r.motionValid&&close(r.velocity.x,300)&&close(r.velocity.y,100)&&close(r.velocity.z,200));
    assert(close(r.angular.z,-1)); // handedness correction for axial velocity
    // A head turn and matching inverse view conversion leave a room-space throw unchanged.
    const float s=sqrtf(0.5f);headQ={0,s,0,s};
    g_motionPoseHead.forward={0,-1,0};g_motionPoseHead.right={1,0,0};
    r={};CombatSampleRelease(&r);
    assert(r.motionValid&&close(r.velocity.x,300)&&close(r.velocity.y,100)&&close(r.velocity.z,200));
    velocityFlags=0;r={};CombatSampleRelease(&r);assert(!r.motionValid);
    velocityFlags=3;releaseLinear={99,0,0};r={};CombatSampleRelease(&r);assert(!r.motionValid);
    releaseLinear={1,2,-3};locationFlags=0;r={};CombatSampleRelease(&r);assert(!r.motionValid);
    locationFlags=3;failLocate=true;r={};CombatSampleRelease(&r);assert(!r.motionValid);
    RenderedHeadFrame head={{},{1,0,0},{0,1,0},{0,0,1}};MEVR_Vec3 selected{};
    g_combatPickups[0]={1,{100,0,0},true,true,false}; // directly ahead, behind wall
    g_combatPickups[1]={2,{100,8,0},true,true,true};
    g_combatPickups[2]={3,{50,0,0},true,false,true}; // empty/ineligible
    g_combatPickups[3]={4,{-100,0,0},true,true,true};
    g_combatPickups[4]={5,{201,0,0},true,true,true};
    assert(CombatSelectGaze(1,2,3,head,&selected)==2&&close(selected.y,8));
    g_combatPickups[1].visible=false;assert(!CombatSelectGaze(1,2,3,head,&selected));
    uint32_t result=9;
    CombatSpatialAfterScript(101,12,&result);assert(result==9); // ordinary Y unaffected
    CombatSpatialAfterScript(100,11,&result);assert(result==9);
    CombatSpatialAfterScript(100,12,&result);assert(result==55);
    g_combatWatchingDrop=true;CombatSpatialAfterScript(4321,11,nullptr);assert(registered==4321);
    puts("combat spatial: throw direction/spin/head-turn invariance, tracking gates, gaze distance/occlusion, catch sweep and scoped pickup override passed");
}
'@
$cpp=Join-Path $out 'combat-spatial.cpp'
($prefix+"`n"+($functions -join "`n")+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'combat-spatial.exe';$obj=Join-Path $out 'combat-spatial.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'combat spatial compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'combat spatial tests failed'}
