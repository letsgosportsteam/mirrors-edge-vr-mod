$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/combat-tests'
$source=Get-Content (Join-Path $root 'src/combat.inl') -Raw
$functions=foreach($name in @('CombatPrepareFire','CombatProcessScript')){
    $m=[regex]::Match($source,"(?ms)^static [^\r\n]+ $name\([^;{]*\)\s*\{.*?^\}")
    if(!$m.Success){throw "missing production $name"};$m.Value
}
$prefix=@'
#include <windows.h>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <cstdio>
struct MEVR_Vec3{float x,y,z;};
struct P13PoseSnapshot{bool sampledHeadValid=true;struct{MEVR_Vec3 position{1,2,3};}sampledHead;};
static uint8_t wall=1;
static uintptr_t g_playerPawn=(uintptr_t)&wall,g_playerCtl=2,g_combatPlayerWeapon=3;
static uintptr_t g_combatMeleeMove=4,g_vrZiplineMove=5,g_combatPickupOverrideManager=0;
static bool g_combatInteractionReady=true,g_combatReady=true,g_combatWatchingDrop=false;
static long g_frames=50,g_combatStartEntries=0,g_combatAimEntries=0;
static uintptr_t g_combatStartFn=8,g_combatPawnStartFn=9,g_combatAimFn=10;
enum{CWeaponStartFire,CAgainstWall,CFastTrace};
struct{uintptr_t fn;}g_combatFunctions[3]={{7},{0},{0}};
struct{int offset;}g_combatFields[3]{};
static int g_xrState=5;static constexpr int XR_SESSION_STATE_FOCUSED=5;
static bool tracked=true,clear=true,validHead=true;static int called=0,expectedWall=0;
static void Log(const char*,...){}
static bool CombatReadField(uintptr_t,int,void* out,size_t){*(uint8_t*)out=wall;return true;}
static bool CombatMuzzle(uintptr_t,uintptr_t* weapon,MEVR_Vec3* muzzle,int32_t*){
    *weapon=g_combatPlayerWeapon;*muzzle={4,5,6};return tracked;
}
static bool ReadP13PoseSnapshot(P13PoseSnapshot* p){*p={};p->sampledHeadValid=validHead;return true;}
static bool CombatInvoke(uintptr_t,int fn,void* p){
    assert(fn==CFastTrace);const float* v=(float*)p;assert(v[0]==4&&v[3]==1);
    *(uint32_t*)((uint8_t*)p+40)=clear?0xffffffff:0;return true;
}
static bool WriteRigBytes(uintptr_t address,const void* p,size_t n){memcpy((void*)address,p,n);return true;}
static bool SafeU32(uintptr_t address,uint32_t* p){memcpy(p,(void*)address,4);return true;}
static void CombatLogAttack(uintptr_t,uint32_t){}
static void CombatBeforeScript(uintptr_t,uint32_t){}
static void CombatAfterScript(uintptr_t,uint32_t){}
static void CombatSpatialAfterScript(uintptr_t,uint32_t,void*){}
static void CameraVisibilityAfterScript(uintptr_t,uint32_t,void*,void*){}
static void CombatStartTrace(void*,void*){}
static void CombatAdjustedAim(void*,void*){}
static void Original(void*,void*,void*,void*){++called;assert(wall==expectedWall);}
static auto g_combatOriginalScript=Original;
'@
$tests=@'
int main(){
    uint32_t stack[2]={0,7};
    auto fire=[&](){int before=called;CombatProcessScript((void*)g_combatPlayerWeapon,nullptr,stack,nullptr);
        assert(called==before+1&&wall==1);};
    fire(); // clear tracked pistol temporarily bypasses body-relative wall blocking
    expectedWall=1;clear=false;fire();clear=true;
    tracked=false;fire();tracked=true;
    validHead=false;fire();validHead=true;
    g_xrState=0;fire();g_xrState=5;
    stack[1]=123;fire();stack[1]=7;
    g_combatInteractionReady=false;fire();g_combatInteractionReady=true;
    uintptr_t address=0;uint8_t previous=0;
    assert(!CombatPrepareFire(99,7,&address,&previous));
    puts("PASS: tracked clear muzzle firing, blocked/stale/foreign/desktop gates, original dispatch and wall-state restoration");
}
'@
$cpp=Join-Path $out 'combat-fire.cpp'
($prefix+"`n"+($functions -join "`n")+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'combat-fire.exe';$obj=Join-Path $out 'combat-fire.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'combat fire compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'combat fire tests failed'}
