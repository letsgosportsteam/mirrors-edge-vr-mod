$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/smoke-capture-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$prefix=@'
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <vector>
struct FakeDeclaration {
    int references=0;
    HRESULT GetDeclaration(D3DVERTEXELEMENT9* out,UINT* count){out[0]=D3DDECL_END();*count=1;return S_OK;}
    void Release(){--references;}
};
struct FakeTexture {
    D3DRESOURCETYPE GetType(){return D3DRTYPE_TEXTURE;}
    HRESULT GetLevelDesc(UINT,D3DSURFACE_DESC*){return E_FAIL;}
    void Release(){}
};
// No mutation methods: the production capture must compile using reads only.
struct FakeDevice {
    int calls=0;FakeDeclaration declaration;
    HRESULT GetViewport(D3DVIEWPORT9* out){++calls;*out={0,0,4224,2376,0,1};return S_OK;}
    HRESULT GetScissorRect(RECT* out){++calls;*out={0,0,4224,2376};return S_OK;}
    HRESULT GetVertexShaderConstantF(UINT,float* out,UINT n){++calls;memset(out,0,n*16);return S_OK;}
    HRESULT GetPixelShaderConstantF(UINT,float* out,UINT n){++calls;memset(out,0,n*16);return S_OK;}
    HRESULT GetVertexDeclaration(FakeDeclaration** out){++calls;++declaration.references;*out=&declaration;return S_OK;}
    HRESULT GetRenderState(D3DRENDERSTATETYPE,DWORD* out){++calls;*out=0;return S_OK;}
    HRESULT GetSamplerState(DWORD,D3DSAMPLERSTATETYPE,DWORD* out){++calls;*out=0;return S_OK;}
    HRESULT GetTexture(DWORD,FakeTexture** out){++calls;*out=nullptr;return S_OK;}
};
#define IDirect3DDevice9 FakeDevice
#define IDirect3DVertexDeclaration9 FakeDeclaration
#define IDirect3DBaseTexture9 FakeTexture
#define IDirect3DTexture9 FakeTexture
static wchar_t g_logPath[MAX_PATH];
static long g_effectCaptureUntil=103,g_frames=101;
static bool g_inDupDraw=false;
static float g_sceneMat[16]{};
struct Effect {uint32_t hash=0x23C40ECF;};
static Effect effect;static uint32_t pixel=0xFABE3964;
static Effect* FindEffectShader(FakeDevice*){return &effect;}
static uint32_t CurrentEffectPixelHash(FakeDevice*){return pixel;}
static void BuildEyeMatrix(float* out,int eye){memset(out,0,64);out[0]=(float)(eye+1);}
static bool PathSibling(const wchar_t* directory,const wchar_t* leaf,wchar_t* out,size_t capacity){
    return _snwprintf_s(out,capacity,_TRUNCATE,L"%s\\%s",directory,leaf)>=0;
}
static bool SafeRead(uintptr_t source,void* out,size_t bytes){if(!source)return false;memcpy(out,(void*)source,bytes);return true;}
static void Log(const char*,...){}
'@
$tests=@'
static bool Exists(long serial,unsigned draw,const wchar_t* suffix){
    wchar_t path[MAX_PATH];swprintf_s(path,L"%s\\mevr-smoke-%lu-%ld-%u.%s",g_logPath,GetCurrentProcessId(),serial,draw,suffix);
    return GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;
}
static void CheckBytes(unsigned draw,const wchar_t* suffix,const void* expected,size_t size){
    wchar_t path[MAX_PATH];swprintf_s(path,L"%s\\mevr-smoke-%lu-103-%u.%s",g_logPath,GetCurrentProcessId(),draw,suffix);
    FILE* file=nullptr;_wfopen_s(&file,path,L"rb");assert(file);
    std::vector<BYTE> bytes(size);assert(fread(bytes.data(),1,size,file)==size);assert(fgetc(file)==EOF);fclose(file);
    assert(memcmp(bytes.data(),expected,size)==0);
}
int wmain(int argc,wchar_t** argv){
    assert(argc==2);wcscpy_s(g_logPath,argv[1]);FakeDevice dev;
    BYTE vertices[384];for(unsigned i=0;i<sizeof(vertices);++i)vertices[i]=(BYTE)i;
    WORD indices[12]={4,5,6,5,6,7,8,9,10,9,10,11};
    auto capture=[&]{CaptureSmokeIndexedUP(&dev,D3DPT_TRIANGLELIST,4,8,4,indices,D3DFMT_INDEX16,vertices,32);};
    g_frames=100;capture();assert(dev.calls==0);
    g_frames=101;g_inDupDraw=true;capture();assert(dev.calls==0);g_inDupDraw=false;
    pixel=0;capture();assert(dev.calls==0);pixel=0xFABE3964;
    effect.hash=0;capture();assert(dev.calls==0);effect.hash=0x23C40ECF;
    capture();assert(dev.calls>0 && dev.declaration.references==0);
    CheckBytes(0,L"vertices.bin",vertices,sizeof(vertices));CheckBytes(0,L"indices.bin",indices,sizeof(indices));
    assert(Exists(103,0,L"state.bin"));
    capture();const int calls=dev.calls;capture();assert(dev.calls==calls);
    assert(Exists(103,1,L"state.bin") && !Exists(103,2,L"state.bin"));
    g_effectCaptureUntil=203;g_frames=201;
    CaptureSmokeIndexedUP(&dev,D3DPT_TRIANGLELIST,0,8,4,nullptr,D3DFMT_INDEX16,nullptr,32);
    assert(Exists(203,0,L"state.bin") && !Exists(203,0,L"vertices.bin") && !Exists(203,0,L"indices.bin"));
    assert(dev.declaration.references==0);
    puts("PASS smoke capture: marker/shader gates, exact UP bytes, two-draw cap, marker reset, failed input reads, declaration release, read-only device API");
}
'@
$cpp=Join-Path $out 'smoke-capture.cpp'
($prefix+"`n"+(Get-Content (Join-Path $root 'src/smoke_capture.inl') -Raw)+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'smoke-capture.exe';$obj=Join-Path $out 'smoke-capture.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'smoke capture compile failed'}
& $exe $out
if($LASTEXITCODE -ne 0){throw 'smoke capture tests failed'}
