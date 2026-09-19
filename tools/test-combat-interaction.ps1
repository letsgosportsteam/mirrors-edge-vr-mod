$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/combat-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$main=Get-Content (Join-Path $root 'src/d3d9.cpp') -Raw
$interaction=Get-Content (Join-Path $root 'src/combat_interaction.inl') -Raw
$functions=foreach($name in @('RotateByQuaternion','MultiplyQuaternion','Mat3Multiply','Mat3Transpose',
    'CombatBeforeScript','CombatAfterScript','CombatRedirectWeaponBone')) {
    $source=if($name.StartsWith('Combat')){$interaction}else{$main}
    $match=[regex]::Match($source,"(?ms)^static [^\r\n]+ $name\([^;{]*\)\s*\{.*?^\}")
    if(!$match.Success){throw "Missing $name"}; $match.Value
}
$prefix=@'
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "../../src/combat_gestures.h"
struct MEVR_Vec3 {float x,y,z;};
struct XrQuaternionf {float x,y,z,w;};
struct Mat3 {float m[3][3];};
struct UE3Matrix44 {float m[4][4];};
static bool g_combatInteractionReady=true,g_combatDispatchConsumed=false,g_combatTriggerOverride=false;
static int g_combatDispatchHand=-1,g_combatQueuedHands[4]{},g_combatQueuedHandCount=0;
static CombatFourPunchCombo g_combatCombo;
static int32_t g_combatSavedQueued=0;
static int32_t melee[3]{0x80,0,0};
static uintptr_t g_combatMeleeMove=(uintptr_t)melee;
enum {CLeft,CComboCounter,CComboQueued,CZipAssist,CZipConstrain};
struct Field {int offset; uint32_t mask;};
static Field g_combatFields[5]={{0,4},{4,0},{8,0},{0,1},{4,2}};
enum {CTrigger,CStopMelee,CZipView,CAbortLook};
struct Function {uintptr_t fn;};
static Function g_combatFunctions[4]={{11},{12},{13},{14}};
static uint32_t zipline[2]={1,2};static uintptr_t g_vrZiplineMove=(uintptr_t)zipline;
static bool g_headTracking=true,g_vrZiplineViewOverride=false,g_vrZiplineSavedConstraint=false;
static int g_xrState=5,XR_SESSION_STATE_FOCUSED=5,abortedLook=0;
static bool CombatInvoke(uintptr_t object,int fn){assert(object==g_vrZiplineMove&&fn==CAbortLook);++abortedLook;return true;}
static double now=1000;
static double NowMs(){return now;}
static void Log(const char*,...){}
static bool SafeRead(uintptr_t p,void* out,size_t n){memcpy(out,(void*)p,n);return true;}
static bool SafeU32(uintptr_t p,uint32_t* out){return SafeRead(p,out,4);}
static int writes=0;
static bool WriteRigBytes(uintptr_t p,const void* data,size_t n){++writes;memcpy((void*)p,data,n);return true;}
static bool CombatReadField(uintptr_t obj,int f,void* out,size_t n){return SafeRead(obj+g_combatFields[f].offset,out,n);}
static bool CombatWriteBool(uintptr_t obj,int f,bool value){
    auto& bits=*(uint32_t*)(obj+g_combatFields[f].offset);
    bits=value ? bits|g_combatFields[f].mask : bits&~g_combatFields[f].mask;return true;
}
static bool CombatBoolField(uintptr_t obj,int f,bool* out){if(!g_combatFields[f].mask)return false;
    *out=(*(uint32_t*)(obj+g_combatFields[f].offset)&g_combatFields[f].mask)!=0;return true;}
static uint32_t pawnData[2]={0xDEF,1};
static uint32_t& equipped=pawnData[0];
static uintptr_t g_playerPawn=(uintptr_t)pawnData,g_probeMesh1p=0xABC,g_combatLeftBoneWeapon=0;
static long g_frames=10,g_combatLeftBoneFrame=-1;
static int g_offWeapon=0,g_offWeaponAnimState=4,holding=0;
static int CombatHoldingHand(){return holding;}
static bool PistolCalibrationActive(){return true;}
static bool PistolAllowsHandControl(uintptr_t,uintptr_t,uint8_t anim){return anim==1 || anim==2;}
struct Ownership {bool owned;uintptr_t ownedPawn;};
static Ownership g_p13LeftState={true,g_playerPawn};
static Ownership g_p13RightState={true,g_playerPawn};
static int g_gunPositionMm[2][3]{};
static float g_worldScale=100;
static int g_wristCalibrationDeg[2][3]={{-30,0,0},{150,0,0}};
static float VecLength(MEVR_Vec3 v){return sqrtf(v.x*v.x+v.y*v.y+v.z*v.z);}
static bool FiniteVec(MEVR_Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
'@
$tests=@'
static bool close(float a,float b){return fabsf(a-b)<0.0001f;}
int main(){
    CombatBeforeScript(g_vrZiplineMove,13);assert(abortedLook==1&&zipline[0]==0&&zipline[1]==0);
    CombatAfterScript(g_vrZiplineMove,13);assert(zipline[1]==2&&!g_vrZiplineViewOverride);
    CombatBeforeScript(g_vrZiplineMove,13);assert(abortedLook==1&&zipline[1]==0);
    CombatAfterScript(g_vrZiplineMove,13);assert(zipline[1]==2);
    zipline[0]=1;g_xrState=0;CombatBeforeScript(g_vrZiplineMove,13);assert(zipline[0]==1&&zipline[1]==2);
    g_xrState=5;g_headTracking=false;CombatBeforeScript(g_vrZiplineMove,13);assert(zipline[0]==1&&zipline[1]==2);
    g_headTracking=true;CombatBeforeScript(g_vrZiplineMove,12);assert(zipline[0]==1&&zipline[1]==2);
    for(int i=0;i<8;++i){
        now+=400;g_combatDispatchHand=i%2;g_combatDispatchConsumed=false;melee[2]=1;
        CombatBeforeScript(g_combatMeleeMove,11);
        assert(g_combatDispatchConsumed && g_combatTriggerOverride);
        assert((melee[0]&0x80)!=0 && ((melee[0]&4)!=0)==(i%2==0));
        assert(melee[1]==((i%4==3)?2:0) && melee[2]==0);
        CombatAfterScript(g_combatMeleeMove,11); assert(melee[1]==0 && melee[2]==1);
    }
    g_combatDispatchHand=-1;g_combatQueuedHands[0]=0;g_combatQueuedHands[1]=1;g_combatQueuedHandCount=2;
    CombatBeforeScript(g_combatMeleeMove,11); assert(melee[0]&4);CombatAfterScript(g_combatMeleeMove,11);
    CombatBeforeScript(g_combatMeleeMove,11); assert(!(melee[0]&4));CombatAfterScript(g_combatMeleeMove,11);
    assert(g_combatQueuedHandCount==0);
    melee[1]=2;CombatBeforeScript(g_combatMeleeMove,11);assert(melee[1]==2); // manual input left alone
    g_combatQueuedHandCount=2;CombatAfterScript(g_combatMeleeMove,12);assert(g_combatQueuedHandCount==0);
    UE3Matrix44 bones[74]{};
    for(auto& b:bones) for(int j=0;j<4;++j)b.m[j][j]=1;
    bones[19].m[3][0]=10;bones[19].m[3][1]=20;bones[19].m[3][2]=30;
    bones[48].m[3][0]=bones[49].m[3][0]=100;bones[49].m[3][2]=5;
    const auto original=bones[49],left=bones[19],right=bones[48];
    writes=0;CombatRedirectWeaponBone(g_probeMesh1p,(uint32_t)(uintptr_t)bones,74);
    assert(writes==1 && g_combatLeftBoneWeapon==equipped && g_combatLeftBoneFrame==10);
    assert(close(bones[49].m[3][0],10)&&close(bones[49].m[3][1],20)&&close(bones[49].m[3][2],25));
    assert(close(bones[49].m[0][0],-1)&&close(bones[49].m[1][1],1)&&close(bones[49].m[2][2],-1));
    assert(memcmp(&bones[19],&left,sizeof(left))==0 && memcmp(&bones[48],&right,sizeof(right))==0);
    bones[49]=original;bones[48].m[3][0]+=10;bones[49].m[3][0]+=10;
    CombatRedirectWeaponBone(g_probeMesh1p,(uint32_t)(uintptr_t)bones,74);
    assert(close(bones[49].m[3][0],10)); // free right-hand movement does not move the left gun
    pawnData[1]=3;writes=0;CombatRedirectWeaponBone(g_probeMesh1p,(uint32_t)(uintptr_t)bones,74);assert(writes==0);
    pawnData[1]=1;
    holding=1;writes=0;CombatRedirectWeaponBone(g_probeMesh1p,(uint32_t)(uintptr_t)bones,74);assert(writes==0);
    holding=0;g_gunPositionMm[0][1]=35;bones[49]=original;bones[48]=right;
    CombatRedirectWeaponBone(g_probeMesh1p,(uint32_t)(uintptr_t)bones,74);
    assert(close(bones[49].m[3][1],23.5f)); // left-only 35 mm rightward shift
    assert(memcmp(&bones[19],&left,sizeof(left))==0);
    holding=1;bones[49]=original;writes=0;
    CombatRedirectWeaponBone(g_probeMesh1p,(uint32_t)(uintptr_t)bones,74);assert(writes==0);
    g_gunPositionMm[1][1]=-20;
    CombatRedirectWeaponBone(g_probeMesh1p,(uint32_t)(uintptr_t)bones,74);
    assert(close(bones[49].m[3][1],-2)); // independent right shift
    writes=0;
    holding=0;g_p13LeftState.owned=false;CombatRedirectWeaponBone(g_probeMesh1p,(uint32_t)(uintptr_t)bones,74);assert(writes==0);
    g_p13LeftState.owned=true;bones[19].m[0][0]=2;
    CombatRedirectWeaponBone(g_probeMesh1p,(uint32_t)(uintptr_t)bones,74);assert(writes==0);
    puts("combat interaction: left/right selection, accepted fourth attack, queue preservation, left weapon transform and ownership gates passed");
}
'@
$cpp=Join-Path $out 'combat-interaction.cpp'
($prefix+"`n"+($functions -join "`n")+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'combat-interaction.exe';$obj=Join-Path $out 'combat-interaction.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'combat interaction compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'combat interaction tests failed'}
