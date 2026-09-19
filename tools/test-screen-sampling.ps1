param([string]$CaptureDirectory='.analysis/five-markers-20260917', [string]$PopupCaptureDirectory='')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/stereo-effects-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$effects=Get-Content (Join-Path $root 'src/stereo_effects.inl') -Raw
$begin=$effects.IndexOf('static bool g_lensFlares=')
$end=$effects.IndexOf('template<class FN> static bool DrawStereoShadowProjection', $begin)
$canvas=$effects.Substring($begin,$end-$begin)
$prefix=@'
#include <windows.h>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include "../../src/stereo_shader.h"
struct FakePS {
    std::vector<DWORD> code;int refs=1,reads=0;
    HRESULT GetFunction(void* data,UINT* size){++reads;*size=(UINT)code.size()*4;if(data)memcpy(data,code.data(),*size);return S_OK;}
    void Release(){--refs;}
};
struct FakeTexture {
    D3DSURFACE_DESC desc{};int refs=1;
    FakeTexture(){desc.Width=2000;desc.Height=1000;desc.Usage=D3DUSAGE_RENDERTARGET;}
    D3DRESOURCETYPE GetType(){return D3DRTYPE_TEXTURE;}
    HRESULT GetLevelDesc(UINT,D3DSURFACE_DESC* out){*out=desc;return S_OK;}
    void Release(){--refs;}
};
struct FakeSurface {
    D3DSURFACE_DESC desc{};int refs=1;
    FakeSurface(){desc.Width=2000;desc.Height=1000;}
    HRESULT GetDesc(D3DSURFACE_DESC* out){*out=desc;return S_OK;}
    void Release(){--refs;}
};
struct FakeDevice {
    FakePS* ps=nullptr;FakeTexture* texture=nullptr;DWORD textureSlot=0;FakeSurface surface;
    float constants[224][4]{},transform[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    D3DVIEWPORT9 viewport{0,0,2000,1000,0,1};RECT clip{200,50,1800,900};
    int sets=0;bool fail=false;DWORD depth=0;UINT transformRegister=999;
    HRESULT GetPixelShader(FakePS** out){*out=ps;if(ps)++ps->refs;return S_OK;}
    HRESULT GetTexture(DWORD slot,FakeTexture** out){*out=slot==textureSlot?texture:nullptr;if(*out)++(*out)->refs;return S_OK;}
    HRESULT GetPixelShaderConstantF(UINT reg,float* out,UINT count){memcpy(out,constants[reg],count*16);return fail?E_FAIL:S_OK;}
    HRESULT SetPixelShaderConstantF(UINT reg,const float* in,UINT count){++sets;memcpy(constants[reg],in,count*16);return S_OK;}
    HRESULT GetRenderState(D3DRENDERSTATETYPE state,DWORD* out){*out=state==D3DRS_ZENABLE?depth:state==D3DRS_COLORWRITEENABLE?15:1;return S_OK;}
    HRESULT GetVertexShaderConstantF(UINT reg,float* out,UINT){transformRegister=reg;memcpy(out,transform,64);return S_OK;}
    HRESULT GetRenderTarget(UINT,FakeSurface** out){*out=&surface;++surface.refs;return S_OK;}
    HRESULT GetViewport(D3DVIEWPORT9* out){*out=viewport;return S_OK;}
    HRESULT SetViewport(const D3DVIEWPORT9* in){viewport=*in;return S_OK;}
    HRESULT GetScissorRect(RECT* out){*out=clip;return S_OK;}
    HRESULT SetScissorRect(const RECT* in){clip=*in;return S_OK;}
};
#define IDirect3DDevice9 FakeDevice
#define IDirect3DPixelShader9 FakePS
#define IDirect3DBaseTexture9 FakeTexture
#define IDirect3DTexture9 FakeTexture
#define IDirect3DSurface9 FakeSurface
static UINT g_sceneW=2000,g_sceneH=1000,g_capW=2000,g_capH=1000;
static bool g_simulStereo=true,g_inDupDraw=false,g_sceneSplitMono=false,g_scenePartialMono=false,g_sceneMatValid=true;
static volatile LONG g_dupFrameDraws=1;
static long g_frames=10;
static void Log(const char*,...){}
struct EffectShader {uint32_t hash=0x5470E8CCu;};
static float g_gameUiScale=1.f,g_gameUiHeight=0;
static EffectShader currentEffect;
static uint32_t pixelHash=0;
static EffectShader* FindEffectShader(FakeDevice*){return &currentEffect;}
static uint32_t CurrentEffectPixelHash(FakeDevice*){return pixelHash;}
#include "../../src/stereo_sampling.inl"
'@
$tests=@'
#include "../../tools/test-tutorial-popup.inl"
static FakePS load(const char* folder,const char* hash){
    std::ifstream in(std::string(folder)+"/mevr-effect-"+hash+".ps.bin",std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),{});assert(!bytes.empty()&&bytes.size()%4==0);
    FakePS shader;shader.code.resize(bytes.size()/4);memcpy(shader.code.data(),bytes.data(),bytes.size());return shader;
}
int main(int argc,char** argv){
    assert(argc==2||argc==3);
    auto light=load(argv[1],"361557F6"),flare=load(argv[1],"1468B5C6"),depth=load(argv[1],"C7EAFDDE");
    assert(StereoConstantRegister(light.code.data(),light.code.size(),"ScreenPositionScaleBias",2)==1);
    assert(StereoConstantRegister(light.code.data(),light.code.size(),"LightAttenuationTexture",3)==0);
    assert(StereoConstantRegister(light.code.data(),light.code.size(),"LightAttenuationTexture",2)==-1);
    assert(StereoConstantRegister(depth.code.data(),depth.code.size(),"SceneColorTexture",3)==0);
    assert(StereoConstantRegister(flare.code.data(),flare.code.size(),"ScreenPositionScaleBias",2)==-1);
    assert(StereoConstantRegister(nullptr,0,"ScreenPositionScaleBias",2)==-1);
    DWORD malformed[]={0xffff0300,0x7ffffffe,0x42415443};
    assert(StereoConstantRegister(malformed,3,"ScreenPositionScaleBias",2)==-1);
    FakeDevice dev;FakeTexture texture;dev.ps=&light;dev.texture=&texture;
    const float uv[4]={0.5f,-0.5f,0.5005f,0.50025f};memcpy(dev.constants[1],uv,16);
    {WorldScreenSampling sample(&dev);assert(sample.reg==1);
        sample.Eye(0);assert(fabsf(dev.constants[1][0]-0.25f)<1e-6f&&fabsf(dev.constants[1][3]-0.25025f)<1e-6f);
        sample.Eye(1);assert(fabsf(dev.constants[1][3]-0.75025f)<1e-6f);
        assert(dev.constants[1][1]==uv[1]&&dev.constants[1][2]==uv[2]);}
    assert(!memcmp(dev.constants[1],uv,16)&&texture.refs==1);
    const int reads=light.reads;{WorldScreenSampling sample(&dev);assert(sample.reg==1);}assert(light.reads==reads);
    texture.desc.Usage=0;{WorldScreenSampling sample(&dev);assert(sample.reg==-1);}
    texture.desc.Usage=D3DUSAGE_RENDERTARGET;texture.desc.Width=512;{WorldScreenSampling sample(&dev);assert(sample.reg==-1);}
    texture.desc.Width=2000;dev.fail=true;{WorldScreenSampling sample(&dev);assert(sample.reg==-1);}dev.fail=false;
    dev.ps=&flare;{WorldScreenSampling sample(&dev);assert(sample.reg==-1);}
    ResetScreenSamplers();assert(light.refs==1&&flare.refs==1);
    puts("PASS: captured shader metadata, malformed CTAB, per-eye lighting UV with preserved texel bias, state restoration, atlas/small-target exclusion and shader caching");
    dev.texture=nullptr;int calls=0;HRESULT result=S_OK;
    const RECT originalClip=dev.clip;const D3DVIEWPORT9 originalView=dev.viewport;
    assert(DrawStereoCanvas(&dev,[&]{assert(g_inDupDraw);assert(dev.viewport.Width==1000);
        assert(dev.viewport.X==(DWORD)(calls*1000));assert(dev.clip.left==100+calls*1000);
        assert(dev.clip.right==900+calls*1000);++calls;return calls==1?E_FAIL:S_OK;},&result));
    assert(calls==2&&result==E_FAIL&&!g_inDupDraw&&!memcmp(&dev.clip,&originalClip,sizeof(RECT)));
    assert(!memcmp(&dev.viewport,&originalView,sizeof(originalView))&&dev.surface.refs==1);
    // Tutorial/menu text and material panels share this inset and clip transform.
    g_gameUiScale=.65f;g_gameUiHeight=.1f;int insetCalls=0;
    assert(DrawStereoCanvas(&dev,[&]{
        assert(dev.viewport.Width==650&&dev.viewport.Height==650);
        assert(dev.viewport.X==175+insetCalls*1000&&dev.viewport.Y==75);
        assert(dev.clip.left==240+insetCalls*1000&&dev.clip.right==760+insetCalls*1000);
        assert(dev.clip.top==107&&dev.clip.bottom==660);++insetCalls;return E_FAIL;
    },&result)&&insetCalls==2&&result==E_FAIL);
    assert(!memcmp(&dev.viewport,&originalView,sizeof(originalView))&&!memcmp(&dev.clip,&originalClip,sizeof(RECT)));
    g_gameUiScale=1;g_gameUiHeight=0;
    auto issue=[&]{++calls;return S_OK;};
    dev.texture=&texture;dev.textureSlot=3;assert(!DrawStereoCanvas(&dev,issue,&result)); // full scene sampled at nonzero slot
    dev.texture=nullptr;dev.transform[3]=0.1f;assert(!DrawStereoCanvas(&dev,issue,&result));dev.transform[3]=0;
    dev.depth=1;assert(!DrawStereoCanvas(&dev,issue,&result));dev.depth=0;
    g_sceneSplitMono=true;assert(!DrawStereoCanvas(&dev,issue,&result));g_sceneSplitMono=false;
    g_stereoUI=false;assert(!DrawStereoCanvas(&dev,issue,&result));g_stereoUI=true;
    currentEffect.hash=0x8C7E0467;assert(!DrawStereoCanvas(&dev,issue,&result));assert(calls==2);
    currentEffect.hash=0x5470E8CC;
    const float menu[16]={1,0,0,0, 0,-1.77777779f,0,0, 0,0,.999000013f,1, -2112.5f,2112.88892f,2100.89697f,2113};
    memcpy(dev.transform,menu,64);assert(DrawStereoCanvas(&dev,issue,&result)&&calls==4);
    assert(!memcmp(&dev.clip,&originalClip,sizeof(RECT))&&!memcmp(&dev.viewport,&originalView,sizeof(originalView)));
    dev.transform[15]=NAN;assert(!DrawStereoCanvas(&dev,issue,&result));
    memcpy(dev.transform,menu,64);dev.transform[8]=.5f;assert(!DrawStereoCanvas(&dev,issue,&result));
    puts("PASS: actual captured perspective Canvas menu accepted; arbitrary/nonfinite transforms rejected");
    currentEffect.hash=0xDF014469;pixelHash=0x33DB2A77;memcpy(dev.transform,menu,64);
    dev.transform[14]=2099.89795f;dev.transform[15]=2112; // red-panel capture, c0..3
    int panelCalls=0;
    assert(DrawStereoCanvas(&dev,[&]{
        assert(dev.transformRegister==0&&dev.viewport.Width==1000&&dev.viewport.X==(DWORD)(panelCalls*1000));
        assert(dev.clip.left==100+panelCalls*1000&&dev.clip.right==900+panelCalls*1000);
        ++panelCalls;return panelCalls==2?E_FAIL:S_OK;
    },&result)&&panelCalls==2&&result==E_FAIL);
    assert(!g_inDupDraw&&!memcmp(&dev.clip,&originalClip,sizeof(RECT))&&!memcmp(&dev.viewport,&originalView,sizeof(originalView)));
    pixelHash=0x2A5A0149;assert(!DrawStereoCanvas(&dev,issue,&result)); // sky mesh sharing the VS
    pixelHash=0x33DB2A77;dev.transform[3]=.5f;assert(!DrawStereoCanvas(&dev,issue,&result)); // world perspective
    memcpy(dev.transform,menu,64);dev.depth=1;assert(!DrawStereoCanvas(&dev,issue,&result));dev.depth=0;
    puts("PASS: captured red material panel uses c0 and per-eye clipping, restores state on failure, excludes shared world shader and depth-enabled meshes");
    currentEffect.hash=0x1612477A;
    for(uint32_t p:{0x1468B5C6u,0x619A63AFu,0x1F63E387u}){pixelHash=p;assert(SuppressLensFlare(&dev));}
    pixelHash=0x2C531FCD;assert(!SuppressLensFlare(&dev));pixelHash=0x1468B5C6;
    g_lensFlares=true;assert(!SuppressLensFlare(&dev));g_lensFlares=false;
    currentEffect.hash=0x746AB0D0;assert(!SuppressLensFlare(&dev));
    currentEffect.hash=0xE6318E88;pixelHash=0x6942F53D;assert(SuppressLensFlare(&dev));
    g_lensFlares=true;assert(!SuppressLensFlare(&dev));g_lensFlares=false;
    pixelHash=0xA1D87F1F;assert(!SuppressLensFlare(&dev)); // working sun glow retained
    currentEffect.hash=0xDF014469;pixelHash=0x6942F53D;assert(!SuppressLensFlare(&dev));
    puts("PASS: Canvas stereo scissor/viewport and failed-draw restoration, composite/3D/mono gates, exact flare materials and on/off setting");
    TestTutorialPopup(argc==3?argv[2]:nullptr);
}
'@
$cpp=Join-Path $out 'screen-sampling.cpp'
($prefix+"`n"+$canvas+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'screen-sampling.exe';$obj=Join-Path $out 'screen-sampling.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'screen sampling compile failed'}
if($PopupCaptureDirectory){& $exe (Resolve-Path -LiteralPath $CaptureDirectory).Path (Resolve-Path -LiteralPath $PopupCaptureDirectory).Path}
else {& $exe (Resolve-Path -LiteralPath $CaptureDirectory).Path}
if($LASTEXITCODE -ne 0){throw 'screen sampling tests failed'}
