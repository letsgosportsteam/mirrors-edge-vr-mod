$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/combat-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content (Join-Path $root 'src/combat_interaction.inl') -Raw
$invoke=[regex]::Match($source,'(?ms)^static bool CombatInvoke\([^;{]*\)\s*\{.*?^\}').Value
if(!$invoke){throw 'Missing CombatInvoke'}
$prefix=@'
#include <windows.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
enum{CState,CFastTrace,CScript};
struct Function{uintptr_t fn;};static Function g_combatFunctions[3]{};
static bool g_combatFault=false,untouched=false;
static int g_funcOffsetCached=0xAC,nativeCalls=0,scriptCalls=0;
static uintptr_t g_textLo=1,g_textHi=0xffffffff;
using PFN_Update1pArms=void(__fastcall*)(void*,void*,void*,void*);
static bool SafeRead(uintptr_t p,void* out,size_t n){if(!p)return false;memcpy(out,(void*)p,n);return true;}
static bool SafeU32(uintptr_t p,uint32_t* out){return SafeRead(p,out,4);}
static void Log(const char*,...){}
static bool CombatCall(uintptr_t,uintptr_t,void*){++scriptCalls;return true;}
struct Frame{void* vtable;void* node;void* object;unsigned char* code;void* locals;void* previous;void* outParms;};
static void __fastcall Native(void* object,void*,void* raw,void* result){
    ++nativeCalls;auto* f=(Frame*)raw;assert(f->object==object);
    uint16_t index;memcpy(&index,(char*)f->node+0x94,2);
    if(index==548){
        for(int i=0;i<3;++i){assert(*f->code++==0x23);float v[3];memcpy(v,f->code,12);f->code+=12;
            assert(v[0]==float(i*3+1)&&v[1]==float(i*3+2)&&v[2]==float(i*3+3));}
        assert(*f->code++==0x28);
    }
    assert(*f->code++==0x16);assert(*f->code==0x0B);
    if(untouched)return;
    if(index==284){uint32_t state[2]={1234,0};memcpy(result,state,8);}else *(uint32_t*)result=1;
}
#include "../../src/combat_native.inl"
'@
$tests=@'
int main(){
    unsigned char functions[3][176]{};
    for(int i=0;i<3;++i){g_combatFunctions[i].fn=(uintptr_t)functions[i];uint32_t flags=0x400,exec=(uint32_t)&Native;
        uint16_t index=i==0?284:548;memcpy(functions[i]+0x90,&flags,4);memcpy(functions[i]+0x94,&index,2);memcpy(functions[i]+0xAC,&exec,4);}
    uint32_t state[2]{};assert(CombatInvoke(123,CState,state));assert(state[0]==1234&&state[1]==0&&scriptCalls==0);
    struct Trace{float v[9];uint32_t bullet,result;}trace{{1,2,3,4,5,6,7,8,9},0,0};
    assert(CombatInvoke(123,CFastTrace,&trace)&&trace.result==1&&scriptCalls==0);
    untouched=true;assert(!CombatInvoke(123,CState,state));assert(!CombatInvoke(123,CFastTrace,&trace));untouched=false;
    const int before=nativeCalls;functions[0][0x94]=0;assert(!CombatInvoke(123,CState,state)&&nativeCalls==before);
    assert(CombatInvoke(123,CScript,nullptr)&&scriptCalls==1);
    assert(!CombatInvoke(0,CFastTrace,&trace));
    puts("PASS: indexed state/trace queries bypass inert ProcessEvent, serialize VM constants, validate results, reject incompatible metadata; scripts retain ProcessEvent");
}
'@
$cpp=Join-Path $out 'pickup-native.cpp'
($prefix+"`n"+$invoke+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'pickup-native.exe';$obj=Join-Path $out 'pickup-native.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'pickup native compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'pickup native tests failed'}
