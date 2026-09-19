$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root '.analysis/combat-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source = Get-Content (Join-Path $root 'src/combat.inl') -Raw
$functions = foreach ($name in @('CombatTraceDiagnosticDue', 'CombatMetadataThread', 'InitializeCombatHooks')) {
    $match = [regex]::Match($source, "(?ms)^static [^\r\n]+ $name\([^;{]*\)\s*\{.*?^\}")
    if (-not $match.Success) { throw "Missing production function $name" }
    $match.Value
}
$prefix = @'
#include <Windows.h>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
struct MEVR_Vec3 { float x,y,z; };
static bool g_motionHands=true,g_pistolHands=true,g_motionPunch=false,g_combatReady=false;
static bool g_combatInteractionReady=false;
static void CombatInteractionMetadata() {}
static void CameraVisibilityMetadata() {}
static void CameraVisibilityValidate() {}
static bool CombatInteractionValidate() { return true; }
static volatile LONG g_objModelThreadFinished=1,g_combatMetadataState=0;
static int g_offMoveState=4,g_peSlot=-1,g_combatMesh1p=-1,g_combatMuzzleName=-1;
static uintptr_t g_combatStartFn=0,g_combatPawnStartFn=0,g_combatAimFn=0;
static uintptr_t g_combatSocketFn=0,g_combatSpreadFn=0;
static void* g_combatOriginalScript=nullptr;
static long g_combatStartEntries=0;
static DWORD mainThread=0;
static HANDLE allowMetadata=nullptr,enteredMetadata=nullptr;
static volatile LONG lookupCount=0,propertyPasses=0;
static int creates=0,hookCreates=0,gameCalls=0;
static bool failThread=false,badLayout=false,missingOnce=false,missingAlways=false;
static uintptr_t g_playerPawn=0x2000;
static double clockMs=0;
static double NowMs() { return clockMs; }
static void Log(const char*,...) {}
static bool LooksLikePlayerPawn(uintptr_t p) { return p==0x2000; }
static bool ReadMoveClassName(uintptr_t,int,char*,size_t,uintptr_t* out) { *out=0x3000; return true; }
static void DumpVtableSlots(uintptr_t,const char*,uintptr_t,const char*,int) { g_peSlot=61; }
static int DeriveUFunctionFuncOffset(uintptr_t* script) { *script=0x1000; return 172; }
static uintptr_t PkFindFunction(const char* name,const char*) {
    assert(GetCurrentThreadId()!=mainThread);
    SetEvent(enteredMetadata);
    assert(WaitForSingleObject(allowMetadata,10000)==WAIT_OBJECT_0);
    const LONG call=InterlockedIncrement(&lookupCount);
    if(!strcmp(name,"GetSocketWorldLocationAndRotation") && (missingAlways || (missingOnce && call<=5))) return 0;
    return 0x4000+0x100*call;
}
struct DetachedPropRequest { const char *owner,*name,*kind; int* offset; };
static void LookupDetachedPropsBatch(DetachedPropRequest* p,int n) {
    assert(GetCurrentThreadId()!=mainThread);
    InterlockedIncrement(&propertyPasses);
    for(int i=0;i<n;++i) *p[i].offset=badLayout ? -1 : 100+i;
}
static bool CombatParameter(uintptr_t fn,const char*,const char*,uint32_t) { return fn!=0; }
static bool SafeU32(uintptr_t,uint32_t* out) { *out=0x1000; return true; }
static bool SafeRead(uintptr_t p,void* out,size_t n) {
    if(p==0x2004 && n==1) { *(uint8_t*)out=1; return true; }
    const uint8_t bytes[]={0x53,0x8B,0x5C,0x24,0x64,0x55,0x56,
        0x8B,0x74,0x24,0x68,0x8B,0x46,0x04,0x8B,0xA8,0x90,0,0,0};
    assert(p==0x100E && n==sizeof(bytes)); memcpy(out,bytes,n); return true;
}
static void CombatProcessScript() {}
typedef int MH_STATUS;
static const int MH_OK=0;
static int MH_CreateHook(void*,void*,void**) { assert(GetCurrentThreadId()==mainThread); ++hookCreates; return MH_OK; }
static int MH_EnableHook(void*) { assert(GetCurrentThreadId()==mainThread); return MH_OK; }
static int MH_DisableHook(void*) { return MH_OK; }
static int MH_RemoveHook(void*) { return MH_OK; }
static bool CombatCall(uintptr_t,uintptr_t,void* parms) {
    assert(GetCurrentThreadId()==mainThread); ++gameCalls; ++g_combatStartEntries;
    *(MEVR_Vec3*)((char*)parms+4)={1,2,3}; return true;
}
static bool FiniteVec(const MEVR_Vec3& v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
static HANDLE TestCreateThread(LPSECURITY_ATTRIBUTES a,SIZE_T b,LPTHREAD_START_ROUTINE c,LPVOID d,DWORD e,LPDWORD f) {
    ++creates;
    if(failThread) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    return CreateThread(a,b,c,d,e,f);
}
#define CreateThread TestCreateThread
'@
$tests = @'
int main(int argc,char** argv) {
    mainThread=GetCurrentThreadId();
    failThread=argc>1 && !strcmp(argv[1],"thread-failure");
    badLayout=argc>1 && !strcmp(argv[1],"bad-layout");
    missingOnce=argc>1 && !strcmp(argv[1],"missing-once");
    missingAlways=argc>1 && !strcmp(argv[1],"missing-always");
    long last=0, reports=0;
    for(long call=1;call<=20670;++call)
        reports+=CombatTraceDiagnosticDue(call,call/2,&last) ? 1 : 0;
    assert(reports==20); // 20,670 getter calls -> 20 diagnostic writes
    allowMetadata=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    enteredMetadata=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    g_playerPawn=0x5000;
    InitializeCombatHooks(0x2000);
    assert(creates==0 && hookCreates==0); // not yet the published player pawn
    g_playerPawn=0x2000;
    InitializeCombatHooks(0x2000);
    assert(creates==1 && !g_combatReady && hookCreates==0 && gameCalls==0);
    if(failThread) {
        assert(g_combatMetadataState==-1);
        for(int i=0;i<100;++i) InitializeCombatHooks(0x2000);
        assert(creates==1 && lookupCount==0 && !g_combatReady);
        puts("combat performance: worker creation failure stays disabled without retries");
        return 0;
    }
    assert(WaitForSingleObject(enteredMetadata,10000)==WAIT_OBJECT_0);
    // Simulate many arm updates while the real background thread is blocked.
    for(int i=0;i<100;++i) InitializeCombatHooks(0x2000);
    assert(creates==1 && hookCreates==0 && gameCalls==0 && !g_combatReady);
    SetEvent(allowMetadata);
    const ULONGLONG deadline=GetTickCount64()+10000;
    while(InterlockedCompareExchange(&g_combatMetadataState,0,0)!=2 && GetTickCount64()<deadline) Sleep(1);
    assert(g_combatMetadataState==2 && lookupCount==5 && propertyPasses==1);
    InitializeCombatHooks(0x2000);
    const int expectedAttempts=(badLayout||missingAlways) ? 3 : missingOnce ? 2 : 1;
    for(int attempt=2;attempt<=expectedAttempts;++attempt) {
        assert(g_combatMetadataState==0 && hookCreates==0 && gameCalls==0);
        for(int i=0;i<100;++i) InitializeCombatHooks(0x2000);
        assert(creates==attempt-1); // no busy retry during cooldown
        clockMs+=2001;
        InitializeCombatHooks(0x2000);
        const ULONGLONG retryDeadline=GetTickCount64()+10000;
        while(InterlockedCompareExchange(&g_combatMetadataState,0,0)!=2 && GetTickCount64()<retryDeadline) Sleep(1);
        assert(g_combatMetadataState==2 && creates==attempt);
        InitializeCombatHooks(0x2000);
    }
    badLayout=badLayout||missingAlways;
    assert(g_combatReady==!badLayout);
    assert(hookCreates==(badLayout ? 0 : 1) && gameCalls==(badLayout ? 0 : 1));
    for(int i=0;i<100;++i) InitializeCombatHooks(0x2000);
    assert(creates==expectedAttempts && lookupCount==5*expectedAttempts && propertyPasses==expectedAttempts);
    CloseHandle(allowMetadata); CloseHandle(enteredMetadata);
    puts("combat performance: bounded logging, nonblocking discovery, single setup, game-thread-only hooks/calls passed");
}
'@
$cpp = Join-Path $out 'combat-performance.cpp'
($prefix + "`n" + ($functions -join "`n") + "`n" + $tests) | Set-Content $cpp
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe = Join-Path $out 'combat-performance.exe'
$obj = Join-Path $out 'combat-performance.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if ($LASTEXITCODE -ne 0) { throw 'combat performance compile failed' }
foreach ($scenario in @('success','thread-failure','bad-layout','missing-once','missing-always')) {
    & $exe $scenario
    if ($LASTEXITCODE -ne 0) { throw "combat performance test failed: $scenario" }
}
