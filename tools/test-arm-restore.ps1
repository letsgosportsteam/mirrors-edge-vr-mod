$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root '.analysis/combat-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source = Get-Content (Join-Path $root 'src/d3d9.cpp') -Raw
$structs = foreach ($name in @('UE3Array32', 'DetachedControlSave', 'ArmRestoreIdentity',
    'DetachedOverrideState', 'DetachedHandSolver', 'WristSideFrame', 'WristOverrideState',
    'DedicatedRotationSideFrame', 'DedicatedRotationOverrideState')) {
    $match = [regex]::Match($source, "(?ms)^struct $name \{.*?^\};")
    if (-not $match.Success) { throw "Missing production struct $name" }
    $match.Value
}
$functions = foreach ($name in @('CaptureArmRestoreIdentity', 'SameArmRestoreIdentity',
    'ArmRestoreIdentityIsCurrent', 'RestoreDetachedArmOverridesBeforeGame',
    'RestoreOneWristSide', 'RestoreWristRotationOverridesBeforeGame',
    'RestoreOneDedicatedRotationSide', 'RestoreDedicatedHandForearmOverridesBeforeGame')) {
    $match = [regex]::Match($source, "(?ms)^static [^\r\n]+ $name\([^;{]*\)\s*\{.*?^\}")
    if (-not $match.Success) { throw "Missing production function $name" }
    $match.Value
}
$prefix = @'
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
struct MEVR_Vec3 { float x,y,z; };
'@
$mocks = @'
// Mock only engine memory access; execute production identity and restoration code.
static std::unordered_map<uintptr_t,uint32_t> words;
static std::unordered_map<uintptr_t,UE3Array32> arrays;
static std::unordered_map<uintptr_t,std::string> classes;
static std::vector<uintptr_t> writes;
static const int g_offMesh1p=4, g_offMeshAnimations=8,
    g_offLeftHandWorldIK=12, g_offRightHandWorldIK=16,
    g_offAnimTreeSkelControlLists=20, g_offMeshSkelControlIndex=24,
    g_offSkelNextControl=28;
static bool SafeU32(uintptr_t address,uint32_t* out) {
    auto it=words.find(address); if(it==words.end()) return false;
    *out=it->second; return true;
}
static bool LooksLikePlayerPawn(uintptr_t p) { return p==1000; }
static bool LooksLikeRigObject(uintptr_t p,const char* name) {
    auto it=classes.find(p); if(it==classes.end()) return false;
    return it->second==name || (strcmp(name,"SkelControlBase")==0 &&
        it->second=="SkelControlSingleBone");
}
static bool ReadUE3Array(uintptr_t p,int off,int,UE3Array32* out) {
    auto it=arrays.find(p+off); if(it==arrays.end()) return false;
    *out=it->second; return true;
}
static bool WriteRigBytes(uintptr_t p,const void*,size_t) { writes.push_back(p); return true; }
static bool RestoreDetachedControl(uintptr_t p,const DetachedControlSave&) {
    writes.push_back(p); return true;
}
static void Log(const char*,...) {}
static DetachedOverrideState g_detachedOverride{};
static DetachedHandSolver g_leftDetach{},g_rightDetach{};
static WristOverrideState g_wristOverride{};
static DedicatedRotationOverrideState g_dedicatedRotationOverride{};
static bool g_detachedWriteFault=false,g_wristWriteFault=false,g_dedicatedRotationWriteFault=false;
'@
$tests = @'
static void seed() {
    words={{1004,2000},{2008,3000},{1012,8000},{1016,9000}};
    arrays={{3020,{4000,10,10}},{2024,{5000,74,74}}};
    classes={{2000,"TdSkeletalMeshComponent"},{3000,"AnimTree"}};
    for(uintptr_t p=6000;p<6007;++p) classes[p]="SkelControlSingleBone";
    writes.clear();
    ArmRestoreIdentity id{}; assert(CaptureArmRestoreIdentity(1000,&id));
    g_detachedOverride={}; g_detachedOverride.active=true; g_detachedOverride.pawn=1000;
    g_detachedOverride.identity=id;
    g_detachedOverride.leftTopology=g_detachedOverride.leftControl=g_detachedOverride.rightControl=true;
    g_detachedOverride.left=6000; g_detachedOverride.right=6001; g_detachedOverride.swing=6002;
    g_detachedOverride.leftMapAddress=5001; g_detachedOverride.spareHeadAddress=4004;
    g_wristOverride={}; g_wristOverride.active=true; g_wristOverride.pawn=1000;
    g_wristOverride.identity=id;
    g_wristOverride.left.active=g_wristOverride.right.active=true;
    g_wristOverride.left.control=6000; g_wristOverride.left.handTail=6001;
    g_wristOverride.right.control=6002; g_wristOverride.right.handTail=6003;
    g_dedicatedRotationOverride={}; g_dedicatedRotationOverride.active=true;
    g_dedicatedRotationOverride.pawn=1000; g_dedicatedRotationOverride.identity=id;
    auto& l=g_dedicatedRotationOverride.left; auto& r=g_dedicatedRotationOverride.right;
    l.active=r.active=true;
    l.donor=6000; l.forearm=6001; l.handTail=6002;
    r.donor=6003; r.forearm=6004; r.handTail=6005;
}
static void restoreAll(uintptr_t pawn) {
    RestoreDedicatedHandForearmOverridesBeforeGame(pawn);
    RestoreWristRotationOverridesBeforeGame(pawn);
    RestoreDetachedArmOverridesBeforeGame(pawn);
    assert(!g_dedicatedRotationOverride.active && !g_wristOverride.active && !g_detachedOverride.active);
    assert(!g_dedicatedRotationWriteFault && !g_wristWriteFault && !g_detachedWriteFault);
}
int main() {
    seed(); restoreAll(1000); assert(writes.size()==25);
    writes.clear(); restoreAll(1000); assert(writes.empty()); // one-time restore
    // Each identity component can change while the pawn address stays the same.
    for(int change=0;change<11;++change) {
        seed();
        switch(change) {
        case 0: words[1004]=2001; classes[2001]="TdSkeletalMeshComponent"; break;
        case 1: words[2008]=3001; classes[3001]="AnimTree"; break;
        case 2: arrays[3020].data++; break;
        case 3: arrays[2024].data++; break;
        case 4: arrays[3020].count++; break;
        case 5: arrays[2024].count++; break;
        case 6: words[1012]++; break;
        case 7: words[1016]++; break;
        case 8: arrays.clear(); break;
        case 9: words.clear(); break;
        case 10: classes[2000]="Destroyed"; break;
        }
        restoreAll(1000); assert(writes.empty());
    }
    seed(); restoreAll(1001); assert(writes.empty());
    // A bad right-side target must prevent even the FIRST left-side write.
    for(uintptr_t bad : {6000,6001,6002,6003,6004,6005}) {
        seed(); classes[bad]="Destroyed";
        RestoreDedicatedHandForearmOverridesBeforeGame(1000); assert(writes.empty());
    }
    for(uintptr_t bad : {6000,6001,6002,6003}) {
        seed(); classes[bad]="Destroyed";
        RestoreWristRotationOverridesBeforeGame(1000); assert(writes.empty());
    }
    for(uintptr_t bad : {6000,6001,6002}) {
        seed(); classes[bad]="Destroyed";
        RestoreDetachedArmOverridesBeforeGame(1000); assert(writes.empty());
    }
    puts("arm restore: intact rig, one-time restore, rig replacement, unreadable memory, bilateral prevalidation passed");
}
'@
$cpp = Join-Path $out 'arm-restore.cpp'
($prefix + "`n" + ($structs -join "`n") + "`n" + $mocks + "`n" + ($functions -join "`n") + "`n" + $tests) | Set-Content $cpp
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe = Join-Path $out 'arm-restore.exe'
$obj = Join-Path $out 'arm-restore.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if ($LASTEXITCODE -ne 0) { throw 'arm restore compile failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'arm restore tests failed' }
