# Exercise the production haze draw helper against a recording device.
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/stereo-effects-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content (Join-Path $root 'src/stereo_effects.inl') -Raw
$start=$source.IndexOf('template<class FN> static bool DrawStereoShadowProjection(')
$end=$source.IndexOf('template<class Shader>', $start)
if($start -lt 0 -or $end -le $start){throw 'Missing production haze helper'}
$helper=$source.Substring($start,$end-$start)
$main=Get-Content (Join-Path $root 'src/d3d9.cpp') -Raw
$scissor=[regex]::Match($main,'(?ms)^struct WorldDrawScissor \{.*?^\};').Value
if(!$scissor){throw 'Missing world scissor guard'}
$prefix=@'
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <cstdint>
#include "../../src/stereo_math.h"
#define D3D_OK S_OK
struct Shader{};
static Shader originalShader,patchedShader;
struct EffectShader {Shader* shader;Shader* hazeStereo;uint32_t hash=0;};
static EffectShader effect{&originalShader,&patchedShader};
struct FakeDevice {
    D3DVIEWPORT9 viewport{0,0,2000,1000,0,1};float constants[256][4]{},pixelConstants[224][4]{};
    Shader* shader=&originalShader;bool readFailure=false;
    DWORD scissorEnabled=1;
    HRESULT GetRenderState(D3DRENDERSTATETYPE,DWORD* out){*out=scissorEnabled;return S_OK;}
    HRESULT SetRenderState(D3DRENDERSTATETYPE,DWORD value){scissorEnabled=value;return S_OK;}
    HRESULT GetViewport(D3DVIEWPORT9* p){*p=viewport;return S_OK;}
    HRESULT SetViewport(const D3DVIEWPORT9* p){viewport=*p;return S_OK;}
    HRESULT GetVertexShaderConstantF(UINT reg,float* data,UINT count){memcpy(data,constants[reg],count*16);return readFailure?E_FAIL:S_OK;}
    HRESULT SetVertexShader(Shader* s){shader=s;return S_OK;}
    HRESULT GetPixelShaderConstantF(UINT reg,float* data,UINT count){memcpy(data,pixelConstants[reg],count*16);return readFailure?E_FAIL:S_OK;}
    HRESULT SetPixelShaderConstantF(UINT reg,const float* data,UINT count){memcpy(pixelConstants[reg],data,count*16);return S_OK;}
};
#define IDirect3DDevice9 FakeDevice
static bool g_simulStereo=true,g_inDupDraw=false,g_sceneMatValid=true,g_vmRow=true;
static bool g_c0IsScene=true;
static bool matchPixel=true,validTarget=true,singular=false;
static long g_frames=0;
static int g_vmReg=0;
static float g_sceneMat[16]={0,0,1,1, 1,0,0,0, 0,1,0,0, -20,-30,-501,-500};
static uint32_t pixelHash=0;
static uint32_t CurrentEffectPixelHash(IDirect3DDevice9*){return pixelHash;}
static bool turnEye=false;
static EffectShader* FindEffectShader(IDirect3DDevice9*){return &effect;}
static bool SunHazePixelMatches(IDirect3DDevice9*){return matchPixel;}
static bool ShouldDuplicate(){return validTarget;}
static void Log(const char*,...){}
static HRESULT g_origSetVSConstF(IDirect3DDevice9* dev,UINT reg,const float* data,UINT count){memcpy(dev->constants[reg],data,count*16);return S_OK;}
static void BuildEyeMatrix(float* out,int eye){
    // Camera center (500,20 +/- 3,30), +X forward and +Y screen right.
    const float matrix[16]={0,0,1,1, 1,0,0,0, 0,1,0,0, -20-(eye?3.0f:-3.0f),-30,-501,-500};
    memcpy(out,matrix,sizeof(matrix));if(singular)memset(out,0,sizeof(matrix));
    if(turnEye){
        const float c=cosf(0.4f),s=sinf(0.4f),y=eye?23.0f:17.0f;
        const float rotated[16]={-s*0.8f,0,c,c, c*0.8f,0,s,s, 0,1.2f,0,0,
            (500*s-y*c)*0.8f,-36,-500*c-y*s-1,-500*c-y*s};
        memcpy(out,rotated,sizeof(rotated));
    }
}
'@
$tests=@'
int main(){
    IDirect3DDevice9 dev;
    for(int r=0;r<256;++r)for(int c=0;c<4;++c)dev.constants[r][c]=(float)(r*4+c);
    dev.constants[9][3]=1;
    float saved[256][4];memcpy(saved,dev.constants,sizeof(saved));
    const auto viewport=dev.viewport;
    auto restored=[&](){assert(!g_inDupDraw&&dev.shader==&originalShader);
        assert(!memcmp(saved,dev.constants,sizeof(saved)));assert(!memcmp(&viewport,&dev.viewport,sizeof(viewport)));};
    for(int failEye:{-1,0,1}){
        int draws=0;HRESULT result=S_FALSE;
        auto draw=[&](){
            const int eye=draws++;assert(g_inDupDraw&&dev.shader==&patchedShader);
            assert(dev.viewport.Width==1000&&dev.viewport.X==(DWORD)(eye*1000));
            assert(fabsf(dev.constants[9][0]-500)<0.01f);
            assert(fabsf(dev.constants[9][1]-(eye?23:17))<0.01f);
            assert(fabsf(dev.constants[9][2]-30)<0.01f&&dev.constants[9][3]==1);
            assert(dev.constants[254][0]==0.5f&&dev.constants[254][1]==1);
            assert(dev.constants[255][0]==eye*0.5f);
            // Project an off-center clip ray back through the inverse and forward again.
            const float clip[4]={0.2f,-0.4f,0.8f,1};float world[4]{},vp[16]{};
            const float* inverse=dev.constants[5];BuildEyeMatrix(vp,eye);
            for(int c=0;c<4;++c)for(int k=0;k<4;++k)world[c]+=clip[k]*inverse[k*4+c];
            for(int c=0;c<4;++c){float value=0;for(int k=0;k<4;++k)value+=world[k]*vp[k*4+c];
                assert(fabsf(value-clip[c])<0.001f);}
            return eye==failEye?E_FAIL:S_OK;
        };
        assert(DrawStereoSunHaze(&dev,draw,&result)&&draws==2);
        assert(result==(failEye<0?S_OK:E_FAIL));restored();
    }
    auto noDraw=[]()->HRESULT{assert(false);return E_FAIL;};HRESULT result=S_FALSE;
    g_simulStereo=false;assert(!DrawStereoSunHaze(&dev,noDraw,&result));g_simulStereo=true;
    g_inDupDraw=true;assert(!DrawStereoSunHaze(&dev,noDraw,&result));g_inDupDraw=false;
    g_vmRow=false;assert(!DrawStereoSunHaze(&dev,noDraw,&result));g_vmRow=true;
    g_sceneMatValid=false;assert(!DrawStereoSunHaze(&dev,noDraw,&result));g_sceneMatValid=true;
    matchPixel=false;assert(!DrawStereoSunHaze(&dev,noDraw,&result));matchPixel=true;
    validTarget=false;assert(!DrawStereoSunHaze(&dev,noDraw,&result));validTarget=true;
    effect.hazeStereo=nullptr;assert(!DrawStereoSunHaze(&dev,noDraw,&result));effect.hazeStereo=&patchedShader;
    singular=true;assert(!DrawStereoSunHaze(&dev,noDraw,&result));singular=false;
    dev.readFailure=true;assert(!DrawStereoSunHaze(&dev,noDraw,&result));dev.readFailure=false;
    restored();assert(result==S_FALSE);
    // Both captured shadow variants: actual world point must reconstruct into
    // the same world/light position from either eye, despite yaw, FOV and IPD.
    turnEye=true;effect.hash=0x5A947094u;
    memcpy(dev.constants[0],g_sceneMat,sizeof(g_sceneMat));
    float inverse[16],world[16];assert(StereoInverseMatrix(g_sceneMat,inverse));
    const float depthToClip[16]={1,0,0,0, 0,1,0,0, 0,0,1,1, 0,0,-1,0};
    StereoMultiplyMatrix(depthToClip,inverse,world);
    const float light[16]={0.01f,0,0,0, 0,0.02f,0,0, 0,0,0.03f,0, -4,2,-1,1};
    float shadow[16];StereoMultiplyMatrix(world,light,shadow);
    dev.pixelConstants[1][0]=0.5f;dev.pixelConstants[1][1]=-0.5f;
    dev.pixelConstants[1][2]=dev.pixelConstants[1][3]=0.50025f;
    for(uint32_t hash:{0x00693E34u,0xC7EAFDDEu}){
        pixelHash=hash;
        memcpy(dev.pixelConstants[10],hash==0x00693E34u?world:shadow,64);
        memcpy(dev.pixelConstants[14],shadow,64);
        float savedPixels[224][4];memcpy(savedPixels,dev.pixelConstants,sizeof(savedPixels));
        for(int failEye:{-1,0,1}){
            int draws=0;
            auto draw=[&](){
                const int eye=draws++;assert(dev.scissorEnabled==0);const float p[4]={650,80,35,1};float clip[4]{};
                float vp[16];BuildEyeMatrix(vp,eye);
                for(int c=0;c<4;++c)for(int k=0;k<4;++k)clip[c]+=p[k]*vp[k*4+c];
                const float screen[4]={clip[0],clip[1],clip[3],1};
                for(int block=0;block<(hash==0x00693E34u?2:1);++block){
                    const float* m=dev.pixelConstants[10+4*block];
                    for(int c=0;c<4;++c){float actual=0,expected=0;
                        for(int k=0;k<4;++k){actual+=screen[k]*m[k*4+c];expected+=p[k]*light[k*4+c];}
                        if(hash==0x00693E34u&&block==0)expected=p[c];
                        assert(fabsf(actual-expected)<0.01f);
                    }
                }
                assert(dev.viewport.Width==1000&&dev.viewport.X==(DWORD)(eye*1000));
                assert(dev.pixelConstants[1][0]==0.25f);
                assert(fabsf(dev.pixelConstants[1][3]-(0.250125f+eye*0.5f))<0.00001f);
                return eye==failEye?E_FAIL:S_OK;
            };
            assert(DrawStereoShadowProjection(&dev,draw,&result)&&draws==2);
            assert(result==(failEye<0?S_OK:E_FAIL));
            assert(dev.scissorEnabled==1);
            assert(!memcmp(savedPixels,dev.pixelConstants,sizeof(savedPixels)));
            assert(!memcmp(dev.constants[0],g_sceneMat,sizeof(g_sceneMat)));
            assert(!memcmp(&viewport,&dev.viewport,sizeof(viewport))&&!g_inDupDraw);
        }
    }
    validTarget=false;assert(!DrawStereoShadowProjection(&dev,noDraw,&result));validTarget=true;
    pixelHash=0;assert(!DrawStereoShadowProjection(&dev,noDraw,&result));
    pixelHash=0x00693E34u;effect.hash=0;assert(!DrawStereoShadowProjection(&dev,noDraw,&result));
    effect.hash=0x1612477A;pixelHash=0x2C531FCD;
    dev.constants[4][0]=500;dev.constants[4][1]=20;dev.constants[4][2]=30;dev.constants[4][3]=1;
    for(int sunVariant:{0,1})for(bool turn:{false,true})for(int failEye:{-1,0,1}){
        effect.hash=sunVariant?0xE6318E88u:0x1612477Au;
        pixelHash=sunVariant?0xA1D87F1Fu:0x2C531FCDu;
        turnEye=turn;int calls=0;float first[16]{};
        assert(DrawStereoDistantSun(&dev,[&]{
            const int eye=calls++;float* m=dev.constants[0];float inverse[16];
            assert(StereoInverseMatrix(m,inverse));
            for(int a=0;a<3;++a)assert(fabsf(inverse[8+a]/inverse[11]-dev.constants[4][a])<.02f);
            if(!eye)memcpy(first,m,64);else for(int i=0;i<16;++i)assert(fabsf(first[i]-m[i])<.002f);
            // A sprite near the original camera has identical eye-local clip
            // coordinates, retaining the rotated projection rather than IPD.
            float originalEye[16];BuildEyeMatrix(originalEye,eye);
            for(int i=0;i<12;++i)assert(m[i]==originalEye[i]);
            assert(dev.viewport.X==(DWORD)(eye*1000)&&dev.viewport.Width==1000);
            return eye==failEye?E_FAIL:S_OK;
        },&result)&&calls==2);
        assert(result==(failEye<0?S_OK:E_FAIL));
        assert(!memcmp(dev.constants[0],g_sceneMat,64)&&!g_inDupDraw);
        assert(!memcmp(&viewport,&dev.viewport,sizeof(viewport)));
    }
    turnEye=false;singular=true;assert(!DrawStereoDistantSun(&dev,noDraw,&result));singular=false;
    pixelHash=0x1468B5C6;assert(!DrawStereoDistantSun(&dev,noDraw,&result));
    effect.hash=0xE6318E88u;pixelHash=0x6942F53Du;
    assert(!DrawStereoDistantSun(&dev,noDraw,&result)); // other billboards retain normal stereo
    puts("PASS: sun proxy has zero positional disparity with retained eye rotation/FOV; failed draw state restored; other materials untouched");
    puts("PASS: haze per-eye inverse, origins, packed UVs, gates and complete state restoration on success/failure");
    puts("PASS: both shadow shaders reconstruct invariant world/light positions with eye yaw, FOV, IPD; state restored on failed draws");
}
'@
$cpp=Join-Path $out 'stereo-haze.cpp'
("#include <initializer_list>`n"+$prefix+"`n"+$scissor+"`n"+$helper+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'stereo-haze.exe';$obj=Join-Path $out 'stereo-haze.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if($LASTEXITCODE -ne 0){throw 'stereo haze compile failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'stereo haze tests failed'}
