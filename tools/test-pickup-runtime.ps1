$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/combat-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content (Join-Path $root 'src/combat_spatial.inl') -Raw
$functions=foreach($name in @('CombatPickupAlive','CombatPickupEligible','CombatPickupVisible','CombatSpatialContext','CombatSeedNearby','CombatSelectGaze','CombatTryPickup','CombatSpatialTick')) {
    $match=[regex]::Match($source,"(?ms)^static [^\r\n]+ $name\([^;{]*\)\s*\{.*?^\}")
    if(!$match.Success){throw "missing production $name"};$match.Value
}
$prefix=@'
#include <windows.h>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include "../../src/combat_spatial_math.h"
struct MEVR_Vec3{float x=0,y=0,z=0;};
struct CombatPickupBounds { MEVR_Vec3 origin,extent;float radius; };
struct RenderedHeadFrame{MEVR_Vec3 position,forward;};
struct Hand{MEVR_Vec3 worldPosition{};bool worldValid=true;};
struct P13PoseSnapshot{bool sampledHeadValid=true;long presentFrame=42;RenderedHeadFrame sampledHead{};Hand left{},right{};};
struct Entry{uint32_t object=0;MEVR_Vec3 previous{};double sampled=0;};
static Entry g_combatPickups[2]{};
enum{CWorld,CDeleted,CHidden,CFade,CPickMesh,CCanPickupInventory,CPickupClass,CAllowPickup,CInvManager,
    CState,CAmmo,CFreeSlot,CFastTrace,CMeshPosition,CFindPickup,CRootBody,CBodyTransform,CPickBounds};
static uint32_t g_combatPickupType=55,g_combatPickupWorld=7,g_combatGazePickup=0;
static constexpr int kUObjectClassOff=52,g_offActorLocation=100;
static bool deleted=false,hidden=false,fade=false,canPickup=true,allowMove=true,canUseY=true;
static bool poseValid=true,headValid=true,spatialReady=true,visible=true;
static int ammo=10,slot=1;static const char* state="Pickup";
static MEVR_Vec3 actorPosition{10,20,30},meshPosition{80,90,100},g_combatGazeLocation{};
static MEVR_Vec3 bodyPosition{180,190,200};static bool hasBody=true;
static float handOffset=0;
static bool hasBounds=false,hasView=false;
static CombatPickupBounds bounds{{180,190,200},{12,6,5},15};
static RenderedHeadFrame renderedView{};
static const char* g_combatPositionSource="actor";
static bool ReadPickupView(uintptr_t,RenderedHeadFrame* out){if(!hasView)return false;*out=renderedView;return true;}
static RenderedHeadFrame headSnapshot{};
static bool g_combatSpatialReady=true,g_pistolHands=true,g_motionHands=true,g_gripToGrip=true;
static const int XR_SESSION_STATE_FOCUSED=5;static int g_xrState=5;
static long g_frames=42,poseFrame=42;static volatile LONG g_combatHeldMask=0;
static double clockNow=1000,g_combatCatchUntil[2]{},g_combatNextGaze=0,g_combatNextScan=0;
static float g_worldScale=100;static const char* g_combatPickupGate="idle";
static int picked=-1,registered=0;static bool highlighted=false;
static double NowMs(){return clockNow;}
static bool FiniteVec(MEVR_Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
static float VecLength(MEVR_Vec3 v){return sqrtf(v.x*v.x+v.y*v.y+v.z*v.z);}
static bool SafeU32(uintptr_t address,uint32_t* out){if(address!=1000+kUObjectClassOff)return false;*out=55;return true;}
static bool SafeRead(uintptr_t address,void* out,size_t size){if(address!=1000+g_offActorLocation||size!=12)return false;memcpy(out,&actorPosition,12);return true;}
static bool CombatReadField(uintptr_t object,int id,void* out,size_t){
    if(id==CPickBounds){if(!hasBounds)return false;memcpy(out,&bounds,sizeof(bounds));return true;}
    uint32_t v=0;switch(id){case CWorld:v=7;break;case CPickMesh:v=2000;break;case CPickupClass:v=88;break;
        case CInvManager:v=3000;break;default:return false;}memcpy(out,&v,4);return true;
}
static bool CombatBoolField(uintptr_t,int id,bool* out){switch(id){case CDeleted:*out=deleted;break;
    case CHidden:*out=hidden;break;case CFade:*out=fade;break;case CCanPickupInventory:*out=canPickup;break;
    case CAllowPickup:*out=allowMove;break;default:return false;}return true;}
static bool CombatInvoke(uintptr_t,int fn,void* out){switch(fn){case CMeshPosition:memcpy(out,&meshPosition,12);break;
    case CRootBody:*(uint32_t*)out=hasBody?6000:0;break;
    case CBodyTransform:{float* matrix=(float*)out;memset(matrix,0,64);matrix[0]=matrix[5]=matrix[10]=matrix[15]=1;
        matrix[12]=bodyPosition.x;matrix[13]=bodyPosition.y;matrix[14]=bodyPosition.z;break;}
    case CState:*(uint32_t*)out=1;break;case CAmmo:*(int*)out=ammo;break;
    case CFreeSlot:((uint8_t*)out)[4]=(uint8_t)slot;break;
    case CFastTrace:*(uint32_t*)((uint8_t*)out+40)=visible?0xffffffff:0;break;
    case CFindPickup:*(uint32_t*)out=1000;break;default:return false;}return true;
}
static bool ReadObjName(uint32_t,char* out,size_t n){strcpy_s(out,n,"TdWeapon_Pistol_Colt1911");return true;}
static bool NameOf(uint32_t,char* out,size_t n){strcpy_s(out,n,state);return true;}
static bool CombatCanUseY(uintptr_t,uint8_t*,uintptr_t* move){*move=4000;return canUseY;}
static bool LooksLikeRigObject(uintptr_t object,const char*){return object==3000||object==6000;}
static bool ReadP13PoseSnapshot(P13PoseSnapshot* out){*out={};out->sampledHeadValid=headValid;out->presentFrame=poseFrame;
    out->sampledHead=headSnapshot;
    out->left.worldPosition=out->right.worldPosition=bodyPosition;
    out->left.worldPosition.x+=handOffset;out->right.worldPosition.x+=handOffset;return poseValid;}
static void CombatRegisterPickup(uint32_t object){registered=object;g_combatPickups[0].object=object;}
static void CombatScanPickups(uint32_t){}
static void CombatCancelCatch(){g_combatCatchUntil[0]=g_combatCatchUntil[1]=0;}
static void CombatSpatialReset(){CombatCancelCatch();g_combatPickupWorld=0;}
static void CombatPublishHighlight(bool show,MEVR_Vec3={}){highlighted=show;}
static void CombatReportPickup(bool=false){}
static void CombatUpdatePickupDebug(uintptr_t,uint32_t){}
static bool CombatPickSelected(uintptr_t,int hand,uint32_t target,uint32_t,uint32_t,bool=false){assert(target==1000);picked=hand;return true;}
'@
$tests=@'
int main(){
    MEVR_Vec3 position{};assert(CombatPickupAlive(1000,7,&position));assert(position.x==180&&position.z==200);
    hasBody=false;assert(CombatPickupAlive(1000,7,&position));assert(position.x==80&&position.z==100);hasBody=true;
    hasBounds=true;bounds.origin={195,190,205};assert(CombatPickupAlive(1000,7,&position));assert(position.x==195&&position.z==205);
    bounds.radius=NAN;assert(CombatPickupAlive(1000,7,&position));assert(position.x==180);bounds.radius=15;
    bounds.extent.x=150;assert(CombatPickupAlive(1000,7,&position));assert(position.x==180);bounds.extent.x=12;hasBounds=false;
    assert(!CombatPickupAlive(1000,8,&position));hidden=true;assert(!CombatPickupAlive(1000,7,&position));hidden=false;
    fade=true;assert(!CombatPickupAlive(1000,7,&position));fade=false;
    assert(CombatPickupEligible(5000,1000,3000));state="CoolDown";assert(!CombatPickupEligible(5000,1000,3000));
    assert(CombatPickupEligible(5000,1000,3000,true));state="FadeOut";assert(!CombatPickupEligible(5000,1000,3000,true));
    state="Pickup";ammo=0;assert(!CombatPickupEligible(5000,1000,3000));ammo=10;
    slot=0;assert(!CombatPickupEligible(5000,1000,3000));slot=1;
    canPickup=false;assert(!CombatPickupEligible(5000,1000,3000));canPickup=true;
    assert(CombatPickupVisible(5000,{},{}));visible=false;assert(!CombatPickupVisible(5000,{},{}));visible=true;
    uint32_t manager=0,world=0;P13PoseSnapshot pose{};uintptr_t move=0;
    assert(CombatSpatialContext(5000,&manager,&world,&pose,&move));assert(manager==3000&&world==7);
    hasView=true;renderedView={{1,2,3},{0,0,-1}};assert(CombatSpatialContext(5000,&manager,&world,&pose,&move));
    assert(pose.sampledHead.position.x==1&&pose.sampledHead.forward.z==-1);hasView=false;
    poseFrame=38;assert(!CombatSpatialContext(5000,&manager,&world,&pose,&move));poseFrame=42;
    headValid=false;assert(!CombatSpatialContext(5000,&manager,&world,&pose,&move));headValid=true;
    allowMove=false;assert(!CombatSpatialContext(5000,&manager,&world,&pose,&move));allowMove=true;
    CombatSeedNearby(3000);assert(registered==1000); // native result available on first scan
    g_combatHeldMask=1|4;g_combatCatchUntil[0]=1;clockNow=5000;CombatSpatialTick(5000,0);
    assert(picked==0); // left hand can catch long after the old 400 ms window, even pre-held
    picked=-1;g_combatHeldMask=2|8;clockNow+=20;CombatSpatialTick(5000,0);assert(picked==1);
    picked=-1;state="CoolDown";clockNow+=20;CombatSpatialTick(5000,0);assert(picked==1);state="Pickup";
    picked=-1;handOffset=23;clockNow+=20;CombatSpatialTick(5000,0);assert(picked==1);
    picked=-1;handOffset=30;clockNow+=20;CombatSpatialTick(5000,0);assert(picked==-1);handOffset=0;
    picked=-1;g_combatHeldMask=2;clockNow+=20;CombatSpatialTick(5000,0);assert(picked==-1); // lost tracking
    g_combatHeldMask=0;clockNow+=20;CombatSpatialTick(5000,0);assert(picked==-1); // open hand
    g_combatHeldMask=1|4;clockNow+=20;CombatSpatialTick(5000,99);assert(picked==-1); // already armed
    // A ground pistol follows the rigid body, not its stale component/actor origin.
    g_combatHeldMask=0;headSnapshot={{180,190,370},{0,0,-1}};clockNow+=120;
    CombatSpatialTick(5000,0);assert(highlighted&&g_combatGazePickup==1000);
    assert(g_combatGazeLocation.x==180&&g_combatGazeLocation.z==200);
    CombatTryPickup(5000,0,0);assert(picked==0);CombatTryPickup(5000,1,0);assert(picked==1);
    headSnapshot.forward={0,0,1};picked=-1;clockNow+=120;CombatSpatialTick(5000,0);assert(!highlighted);
    CombatTryPickup(5000,0,0);assert(picked==-1);
    headSnapshot.forward={0,0,-1};visible=false;clockNow+=120;CombatSpatialTick(5000,0);assert(!highlighted);
    visible=true;headSnapshot.forward={0,.224951f,-.974370f};clockNow+=120;CombatSpatialTick(5000,0);assert(highlighted); // 13 degree look-down mismatch
    puts("PASS: production pickup eligibility/context, mesh position, native discovery, occlusion and sustained/pre-held left/right catches");
}
'@
$cpp=Join-Path $out 'pickup-runtime.cpp'
($prefix+"`n"+($functions -join "`n")+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'pickup-runtime.exe';$obj=Join-Path $out 'pickup-runtime.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W3 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'pickup runtime compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'pickup runtime tests failed'}
