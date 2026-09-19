$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/sun-trace-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content (Join-Path $root 'src/sun_trace.inl') -Raw
$prefix=@'
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <vector>
static int liveSurfaces=0,saves=0;
struct FakeSurface {
    D3DSURFACE_DESC desc{};int references=1,value=0;std::vector<int> tiles;
    FakeSurface(){++liveSurfaces;desc.Width=2048;desc.Height=1024;desc.Format=D3DFMT_A8R8G8B8;}
    HRESULT GetDesc(D3DSURFACE_DESC* out){*out=desc;return S_OK;}
    void Release(){if(!--references){--liveSurfaces;delete this;}}
};
struct FakeShader {void Release(){}};
struct FakeTexture {
    D3DRESOURCETYPE GetType(){return D3DRTYPE_TEXTURE;}
    HRESULT GetLevelDesc(UINT,D3DSURFACE_DESC*){return E_FAIL;}
    void Release(){}
};
struct FakeDevice {
    FakeSurface* target=new FakeSurface;int calls=0;bool failAllocate=false,failCopy=false;DWORD color=15;
    ~FakeDevice(){target->Release();}
    HRESULT GetRenderState(D3DRENDERSTATETYPE s,DWORD* out){++calls;*out=s==D3DRS_COLORWRITEENABLE?color:0;return S_OK;}
    HRESULT GetRenderTarget(DWORD,FakeSurface** out){++calls;*out=target;++target->references;return S_OK;}
    HRESULT CreateRenderTarget(UINT w,UINT h,D3DFORMAT fmt,D3DMULTISAMPLE_TYPE,DWORD,BOOL,FakeSurface** out,HANDLE*){
        ++calls;if(failAllocate)return E_OUTOFMEMORY;*out=new FakeSurface;(*out)->desc.Width=w;(*out)->desc.Height=h;(*out)->desc.Format=fmt;
        (*out)->tiles.resize(128,-1);return S_OK;
    }
    HRESULT ColorFill(FakeSurface*,const RECT*,D3DCOLOR){++calls;return S_OK;}
    HRESULT StretchRect(FakeSurface* from,const RECT*,FakeSurface* to,const RECT* rect,D3DTEXTUREFILTERTYPE){
        ++calls;if(failCopy)return E_FAIL;assert(rect);int tile=rect->top/128*8+rect->left/256;
        assert(tile>=0&&tile<128&&rect->right-rect->left==256&&rect->bottom-rect->top==128);
        to->tiles[tile]=from->value;return S_OK;
    }
    HRESULT GetViewport(D3DVIEWPORT9* out){++calls;*out={0,0,2048,1024,0,1};return S_OK;}
    HRESULT GetVertexShaderConstantF(UINT,float* out,UINT count){++calls;memset(out,0,count*16);return S_OK;}
    HRESULT GetPixelShaderConstantF(UINT,float* out,UINT count){++calls;memset(out,0,count*16);return S_OK;}
    HRESULT GetTexture(DWORD,FakeTexture** out){++calls;*out=nullptr;return S_OK;}
    HRESULT GetPixelShader(FakeShader** out){++calls;*out=nullptr;return S_OK;}
};
#define IDirect3DDevice9 FakeDevice
#define IDirect3DSurface9 FakeSurface
#define IDirect3DTexture9 FakeTexture
#define IDirect3DBaseTexture9 FakeTexture
#define IDirect3DPixelShader9 FakeShader
static long g_effectCaptureUntil=103,g_frames=100;
static bool g_inDupDraw=false,suppressed=false,unknown=false;
struct EffectShader {FakeShader* shader=nullptr;bool scene=true;uint32_t hash=123;};
static EffectShader effect;
static EffectShader* FindEffectShader(FakeDevice*){return unknown?nullptr:&effect;}
static uint32_t CurrentEffectPixelHash(FakeDevice*){return 456;}
static bool ShouldDuplicate(){return true;}
static bool SuppressLensFlare(FakeDevice*){return suppressed;}
static void Log(const char*,...){}
static wchar_t g_logPath[MAX_PATH]{};
static bool PathSibling(const wchar_t*,const wchar_t*,wchar_t*,size_t){return false;}
template<class T> static uint32_t SaveEffectShader(T*,const wchar_t*){return 0;}
static std::vector<int> saved;
static HRESULT SaveEffectSurface(FakeDevice*,FakeSurface* surface,const wchar_t*,bool=false){
    ++saves;saved=surface->tiles;return S_OK;
}
'@
$tests=@'
int main(){
    FakeDevice dev;
    {SunDrawCapture draw(&dev,0,2);}assert(dev.calls==0);
    g_frames=101;g_inDupDraw=true;{SunDrawCapture draw(&dev,0,2);}assert(dev.calls==0);g_inDupDraw=false;
    dev.target->value=17;unknown=true;
    {SunDrawCapture draw(&dev,0,2);assert(draw.target);dev.target->value=23;}
    assert(g_sunTrace.count==1&&g_sunTrace.records[0].vs==0&&g_sunTrace.records[0].draw==0);
    assert(g_sunTrace.atlas->tiles[0]==17&&g_sunTrace.atlas->tiles[1]==23);
    assert(g_sunTrace.records[0].beforeResult==S_OK&&g_sunTrace.records[0].afterResult==S_OK);
    suppressed=true;{SunDrawCapture draw(&dev,1,2);}assert(g_sunTrace.records[1].suppressed==1);
    for(unsigned i=2;i<64;++i){SunDrawCapture draw(&dev,3,2);}
    assert(saves==1&&g_sunTrace.page==1&&g_sunTrace.count==0&&liveSurfaces==1);
    assert(saved[0]==17&&saved[1]==23);
    {SunDrawCapture draw(&dev,2,2);}assert(g_sunTrace.records[0].draw==64);
    dev.target->desc.Format=D3DFMT_A16B16G16R16F;
    {SunDrawCapture draw(&dev,2,2);}assert(g_sunTrace.page==2&&g_sunTrace.records[0].draw==65);
    g_sunTrace.Finish();assert(liveSurfaces==1&&saves==3&&g_sunTrace.finished);
    int calls=dev.calls;{SunDrawCapture draw(&dev,2,2);}assert(dev.calls==calls);
    g_effectCaptureUntil=203;g_frames=201;unknown=false;
    for(unsigned i=0;i<2050;++i){SunDrawCapture draw(&dev,3,2);}
    assert(g_sunTrace.total==2048&&g_sunTrace.page==32&&g_sunTrace.capped&&liveSurfaces==1);
    unknown=true;
    for(unsigned i=0;i<260;++i){SunDrawCapture draw(&dev,1,2);}
    assert(g_sunTrace.total==2304&&g_sunTrace.page==36&&liveSurfaces==1); // reserved screen/menu budget
    g_sunTrace.Finish();assert(g_sunTrace.finished);
    g_effectCaptureUntil=303;g_frames=301;dev.failCopy=true;
    {SunDrawCapture draw(&dev,3,2);}assert(g_sunTrace.records[0].beforeResult==E_FAIL&&g_sunTrace.records[0].afterResult==E_FAIL);
    int prior=saves;g_sunTrace.Discard();assert(liveSurfaces==1&&saves==prior);dev.failCopy=false;
    dev.failAllocate=true;{SunDrawCapture draw(&dev,3,2);assert(!draw.target);}
    assert(liveSurfaces==1&&g_sunTrace.total==2304);g_sunTrace.Discard();
    dev.failAllocate=false;dev.color=0;{SunDrawCapture draw(&dev,3,2);assert(!draw.target);}assert(liveSurfaces==1);
    dev.color=15;dev.target->desc.Width=256;{SunDrawCapture draw(&dev,3,2);assert(!draw.target);}assert(liveSurfaces==1);
    puts("PASS: marker-only unknown/suppressed draws, ordered before/after tiles, page/format rollover, 2048-draw cap, finish/reset, copy/allocation failures and COM cleanup");
}
'@
$cpp=Join-Path $out 'sun-trace.cpp'
($prefix+"`n"+$source+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'sun-trace.exe';$obj=Join-Path $out 'sun-trace.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'sun trace compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'sun trace tests failed'}
