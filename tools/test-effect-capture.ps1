$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/stereo-effects-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content (Join-Path $root 'src/effect_capture.inl') -Raw
$capture=$source.Substring($source.IndexOf('struct EffectPassCapture {'))
$prefix=@'
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <vector>
struct FakeSurface {
    D3DSURFACE_DESC desc{};int references=1,value=0;
    FakeSurface(){desc.Width=2048;desc.Height=512;desc.Format=D3DFMT_A8R8G8B8;}
    HRESULT GetDesc(D3DSURFACE_DESC* d){*d=desc;return S_OK;}
    void Release(){--references;}
};
struct FakeShader {void Release(){}};
struct FakeTexture {
    D3DRESOURCETYPE GetType(){return D3DRTYPE_TEXTURE;}
    HRESULT GetLevelDesc(UINT,D3DSURFACE_DESC*){return E_FAIL;}
    HRESULT GetSurfaceLevel(UINT,FakeSurface**){return E_FAIL;}
    void Release(){}
};
struct FakeDevice {
    FakeSurface surface;int calls=0;bool fail=false;
    HRESULT GetRenderState(D3DRENDERSTATETYPE s,DWORD* value){++calls;*value=s==D3DRS_COLORWRITEENABLE?15:0;return fail?E_FAIL:S_OK;}
    HRESULT GetRenderTarget(DWORD,FakeSurface** out){++calls;*out=&surface;++surface.references;return S_OK;}
    HRESULT GetViewport(D3DVIEWPORT9* out){++calls;*out={0,0,2048,512,0,1};return S_OK;}
    HRESULT GetScissorRect(RECT* out){*out={0,0,2048,512};return S_OK;}
    HRESULT GetPixelShader(FakeShader** out){++calls;*out=nullptr;return S_OK;}
    HRESULT GetVertexShaderConstantF(UINT,float* out,UINT count){++calls;memset(out,0,count*16);return S_OK;}
    HRESULT GetPixelShaderConstantF(UINT,float* out,UINT count){++calls;memset(out,0,count*16);return S_OK;}
    HRESULT GetTexture(DWORD,FakeTexture** out){++calls;*out=nullptr;return S_OK;}
};
#define IDirect3DDevice9 FakeDevice
#define IDirect3DSurface9 FakeSurface
#define IDirect3DTexture9 FakeTexture
#define IDirect3DBaseTexture9 FakeTexture
#define IDirect3DPixelShader9 FakeShader
static long g_effectCaptureUntil=103,g_frames=101;
static bool g_inDupDraw=false,g_c0IsScene=true;
static float g_sceneMat[16]{};
struct EffectShader {FakeShader* shader=nullptr;bool scene=true;uint32_t hash=0x5A947094;};
static EffectShader current;
static uint32_t pixel=0xC7EAFDDE;
static EffectShader* FindEffectShader(FakeDevice*){return &current;}
static uint32_t CurrentEffectPixelHash(FakeDevice*){return pixel;}
static bool ShouldDuplicate(){return true;}
static void BuildEyeMatrix(float* out,int){memcpy(out,g_sceneMat,sizeof(g_sceneMat));}
static double NowMs(){return 1;}
static void Log(const char*,...){}
template<class T> static uint32_t SaveEffectShader(T*,const wchar_t*){return 0;}
static std::vector<int> images;
static bool failSave=false;
static HRESULT SaveEffectSurface(FakeDevice*,FakeSurface* surface,const wchar_t*,bool=false){
    images.push_back(surface->value);return failSave?E_FAIL:S_OK;
}
'@
$tests=@'
int main(){
    FakeDevice device;
    g_frames=100;{EffectPassCapture p(&device,"test",2);assert(!p.target);}assert(device.calls==0);
    g_frames=101;g_inDupDraw=true;{EffectPassCapture p(&device,"test",2);assert(!p.target);}assert(device.calls==0);
    g_inDupDraw=false;
    {EffectPassCapture p(&device,"test",2);assert(p.target);assert(images.size()==1);device.surface.value=7;}
    assert(images.size()==2&&images[0]==0&&images[1]==7&&device.surface.references==1);
    for(int i=0;i<40;++i){EffectPassCapture p(&device,"test",2);}
    assert(images.size()==8); // four draws per shadow shader, paired on scope exit
    for(int i=0;i<40;++i){++pixel;EffectPassCapture p(&device,"test",2);}
    assert(images.size()==48); // known effects have an independent 24-pair budget
    current.hash=0x12345678;current.scene=false;
    for(int i=0;i<40;++i){++pixel;EffectPassCapture p(&device,"test",2);}
    assert(images.size()==64&&device.surface.references==1); // eight unknown passes
    g_frames=102;const int calls=device.calls;
    {EffectPassCapture p(&device,"test",2);assert(!p.target);}assert(device.calls==calls);
    g_effectCaptureUntil=203;g_frames=201;images.clear();failSave=true;
    {EffectPassCapture p(&device,"test",2);assert(p.target);}assert(images.size()==2&&device.surface.references==1);
    device.fail=true;{EffectPassCapture p(&device,"test",2);assert(!p.target);}assert(images.size()==2);
    puts("PASS: production pass capture pairs draw output, stays inactive outside marker, caps repeated and unique passes, resets per marker, releases targets on save failure");
}
'@
$cpp=Join-Path $out 'effect-capture.cpp'
($prefix+"`n"+$capture+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'effect-capture.exe';$obj=Join-Path $out 'effect-capture.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'effect capture compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'effect capture tests failed'}
