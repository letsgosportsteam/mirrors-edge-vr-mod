param([string]$CaptureDirectory,[switch]$SkipDevice)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/stereo-effects-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content (Join-Path $root 'src/d3d9.cpp') -Raw
$functions=foreach($name in @('CameraFromVP','SceneCameraMatches','ApplyCameraRotation','RotationPivot','ApplyCinematicLook','AnimationOwnsYawReference','HigherEntryHand','HeadStepLimit','ApplyPitchFix','CinematicHeadLookRequested')) {
    $match=[regex]::Match($source,"(?ms)^static [^\r\n]+ $name\([^;{]*\)\s*\{.*?^\}")
    if(!$match.Success){throw "missing production $name"};$match.Value
}
$effectSource=Get-Content (Join-Path $root 'src/stereo_effects.inl') -Raw
$capture=[regex]::Match($effectSource,'(?ms)^static void CaptureFlareDetails\([^;{]*\)\s*\{.*?^\}')
if(!$capture.Success){throw 'Missing production flare capture'}
$functions += $capture.Value
$functions += Get-Content (Join-Path $root 'src/effect_capture.inl') -Raw
$prefix=@'
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include "../../src/stereo_shader.h"
#include "../../src/stereo_math.h"
static long g_effectCaptureUntil=100,g_frames=99;
static wchar_t g_logPath[MAX_PATH]{};
static void Log(const char*,...){}
static bool PathSibling(const wchar_t* full,const wchar_t* leaf,wchar_t* out,size_t cap){
    std::wstring path=full;path=path.substr(0,path.find_last_of(L"\\/")+1)+leaf;
    return wcscpy_s(out,cap,path.c_str())==0;
}
static bool SafeRead(uintptr_t address,void* data,size_t bytes){memcpy(data,(void*)address,bytes);return true;}
static bool g_matrixPivotValid=true,g_livePivotValid=true,g_cinematicLook=true;
static bool g_scriptedHeadInEngine=false;
static float g_matrixPivot[3]{},g_livePivot[3]{100,200,300},g_camCache[3]{};
static float g_cinematicPitch=0,g_cinematicYaw=0;
static bool g_pitchFix=true,g_pitchTargetValid=true,g_camCacheValid=true,g_pitchAbsolute=true,g_animFollow=true,g_liveCtlValid=true;
static float g_pitchTarget=0,g_liveCtlPitch=0,g_yawLagRad=0;
static uint8_t g_liveCameraMove=1;
static long g_camBurst=0;
static bool g_headTracking=true;
static uintptr_t g_playerCtl=1000,g_playerPawn=2000;
static int g_offMoveState=8;
struct Gate {const char* name;int ref;};
static Gate g_gates[]={{"bCinematicMode",0},{"bCinemaDisableInputLook",1},{"bIgnoreLookInput",2},{"bIgnoreMoveInput",3}};
static int g_gateCount=4;
static uint32_t gateValues[4]{};
static bool gateReadOK=true;
static bool ReadFlag(uintptr_t,int ref,uint32_t* value){*value=gateValues[ref];return gateReadOK;}
static bool g_inDupDraw=false,g_c0IsScene=true;
static float g_sceneMat[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
struct EffectShader {IDirect3DVertexShader9* shader=nullptr;bool scene=true;uint32_t hash=0x5A947094u;};
static EffectShader effect;
static EffectShader* FindEffectShader(IDirect3DDevice9*){return &effect;}
static uint32_t CurrentEffectPixelHash(IDirect3DDevice9*){return 0xC7EAFDDEu;}
static bool ShouldDuplicate(){return true;}
static void BuildEyeMatrix(float* out,int){memcpy(out,g_sceneMat,sizeof(g_sceneMat));}
static double NowMs(){return (double)GetTickCount64();}
template<class T> static uint32_t SaveEffectShader(T*,const wchar_t*){return 0;}
'@
$tests=@'
static void checkShader(const char* text,const char* target,bool expected) {
    ID3DBlob* shader=nullptr,*error=nullptr;
    HRESULT hr=D3DCompile(text,strlen(text),nullptr,nullptr,nullptr,"main",target,0,0,&shader,&error);
    if(FAILED(hr)){if(error)puts((char*)error->GetBufferPointer());assert(false);}
    const bool actual=StereoShaderUsesViewProjection((DWORD*)shader->GetBufferPointer(),shader->GetBufferSize()/4,0);
    assert(actual==expected);
    // A cached verdict must be recomputed if the camera register changes.
    assert(!StereoShaderUsesViewProjection((DWORD*)shader->GetBufferPointer(),shader->GetBufferSize()/4,8));
    shader->Release();if(error)error->Release();
}
static void checkCapture(const char* directory,const char* name,bool expected){
    std::ifstream input(std::string(directory)+"/mevr-effect-"+name+".vs.bin",std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)),{});
    assert(!bytes.empty()&&bytes.size()%4==0);
    assert(StereoShaderUsesViewProjection((DWORD*)bytes.data(),bytes.size()/4,0)==expected);
}
static void checkHazePatch(const char* directory,bool skipDevice){
    std::ifstream input(std::string(directory)+"/mevr-effect-51B6BA4A.vs.bin",std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)),{});
    assert(!bytes.empty()&&bytes.size()%4==0);
    std::vector<DWORD> patched;
    assert(StereoPatchHazeUV((DWORD*)bytes.data(),bytes.size()/4,&patched));
    ID3DBlob* text=nullptr;
    assert(SUCCEEDED(D3DDisassemble(patched.data(),patched.size()*4,0,nullptr,&text)));
    assert(strstr((const char*)text->GetBufferPointer(),"mad o0.xy, v1, c254, r2"));
    text->Release();
    if(skipDevice){puts("SKIP: graphics device validation explicitly disabled");return;}
    // Disassembly does not validate SM3 register read-port limits. A real device
    // rejected the former MAD with two distinct constant operands.
    HWND window=CreateWindowExW(0,L"STATIC",L"MEVR shader validation",WS_OVERLAPPED,0,0,32,32,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    assert(window);
    IDirect3D9* d3d=Direct3DCreate9(D3D_SDK_VERSION);assert(d3d);
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth=32;pp.BackBufferHeight=32;pp.hDeviceWindow=window;
    IDirect3DDevice9* device=nullptr;
    HRESULT hr=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device);
    printf("D3D9 test device HRESULT %08lX, adapters %u\n",(unsigned long)hr,d3d->GetAdapterCount());fflush(stdout);
    assert(SUCCEEDED(hr));IDirect3DVertexShader9* shader=nullptr;
    hr=device->CreateVertexShader(patched.data(),&shader);
    printf("Haze shader device validation HRESULT %08lX\n",(unsigned long)hr);assert(SUCCEEDED(hr));
    IDirect3DTexture9* texture=nullptr;
    assert(SUCCEEDED(device->CreateTexture(32,32,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr)));
    D3DLOCKED_RECT rect{};assert(SUCCEEDED(texture->LockRect(0,&rect,nullptr,0)));
    for(int y=0;y<32;++y)for(int x=0;x<32;++x)((DWORD*)((BYTE*)rect.pBits+y*rect.Pitch))[x]=0xff00c0ff;
    texture->UnlockRect(0);device->SetTexture(0,texture);
    GetModuleFileNameW(nullptr,g_logPath,MAX_PATH);
    const float vertices[3][4]={{1,2,3,4},{5,6,7,8},{9,10,11,12}};
    CaptureFlareDetails(device,0x1612477A,0x1468B5C6,vertices,3,16,"test");
    wchar_t capturePath[MAX_PATH];assert(PathSibling(g_logPath,L"mevr-flare-100-0.texture.png",capturePath,MAX_PATH));
    std::ifstream png(capturePath,std::ios::binary);unsigned char magic[8]{};png.read((char*)magic,8);
    assert(magic[0]==137&&magic[1]=='P'&&magic[2]=='N'&&magic[3]=='G');
    assert(PathSibling(g_logPath,L"mevr-flare-100-0.vertices.bin",capturePath,MAX_PATH));
    std::ifstream binary(capturePath,std::ios::binary);float captured[3][4]{};binary.read((char*)captured,sizeof(captured));
    assert(!memcmp(captured,vertices,sizeof(vertices)));
    IDirect3DBaseTexture9* after=nullptr;device->GetTexture(0,&after);assert(after==texture);after->Release();
    device->SetTexture(0,nullptr);texture->Release();
    puts("PASS: production marker capture saves a valid PNG and exact vertex data on the graphics device");
    IDirect3DSurface9* originalTarget=nullptr,*captureTarget=nullptr;
    assert(SUCCEEDED(device->GetRenderTarget(0,&originalTarget)));
    assert(SUCCEEDED(device->CreateRenderTarget(2048,512,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&captureTarget,nullptr)));
    assert(SUCCEEDED(device->SetRenderTarget(0,captureTarget)));
    assert(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0)));
    assert(SUCCEEDED(device->BeginScene()));
    {EffectPassCapture inactive(device,"test",2);assert(!inactive.target);}
    g_frames=98; // first complete frame of the marker
    wchar_t beforePath[MAX_PATH]{},afterPath[MAX_PATH]{};
    {
        EffectPassCapture pass(device,"test",2);assert(pass.target==captureTarget);
        wchar_t leaf[160]{};
        _snwprintf_s(leaf,_TRUNCATE,L"%s.before.png",pass.prefix);assert(PathSibling(g_logPath,leaf,beforePath,MAX_PATH));
        _snwprintf_s(leaf,_TRUNCATE,L"%s.after.png",pass.prefix);assert(PathSibling(g_logPath,leaf,afterPath,MAX_PATH));
        assert(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff0080ff,1,0)));
    }
    assert(SUCCEEDED(device->EndScene()));
    std::ifstream beforeFile(beforePath,std::ios::binary),afterFile(afterPath,std::ios::binary);
    std::vector<char> beforeBytes((std::istreambuf_iterator<char>(beforeFile)),{});
    std::vector<char> afterBytes((std::istreambuf_iterator<char>(afterFile)),{});
    assert(beforeBytes.size()>24&&afterBytes.size()>24&&beforeBytes!=afterBytes);
    assert((unsigned char)beforeBytes[0]==137&&beforeBytes[1]=='P');
    assert((unsigned char)beforeBytes[18]==4&&beforeBytes[19]==0); // PNG width 1024
    IDirect3DSurface9* retained=nullptr;device->GetRenderTarget(0,&retained);
    assert(retained==captureTarget);retained->Release();
    g_frames=99;{EffectPassCapture inactive(device,"test",2);assert(!inactive.target);}
    device->SetRenderTarget(0,originalTarget);captureTarget->Release();originalTarget->Release();
    puts("PASS: paired production pass images inside BeginScene, downsampled to 1024px, target retained, inactive outside marker frame");
    shader->Release();device->Release();d3d->Release();DestroyWindow(window);
    std::vector<DWORD> twice;
    assert(!StereoPatchHazeUV(patched.data(),patched.size(),&twice));
    assert(!StereoPatchHazeUV((DWORD*)bytes.data(),bytes.size()/4-1,&twice));
}
int main(int argc,char** argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    uint8_t state=1;g_playerPawn=(uintptr_t)&state;g_offMoveState=0;
    assert(!CinematicHeadLookRequested());
    gateValues[3]=1;assert(!CinematicHeadLookRequested());gateValues[3]=0;
    gateValues[2]=1;assert(CinematicHeadLookRequested()); // captured Walking + ignored look, no cinematic flags
    state=15;assert(CinematicHeadLookRequested());
    for(int move:{2,9,10,17,27,28,255}){state=(uint8_t)move;assert(!CinematicHeadLookRequested());}
    state=1;gateReadOK=false;assert(!CinematicHeadLookRequested());gateReadOK=true;
    gateValues[2]=0;state=28;gateValues[0]=1;assert(CinematicHeadLookRequested());
    gateValues[0]=0;gateValues[1]=1;assert(CinematicHeadLookRequested());
    g_headTracking=false;assert(!CinematicHeadLookRequested());g_headTracking=true;gateValues[1]=0;
    puts("PASS: non-cinematic scripted look gate; ordinary walking, move-only locks and parkour restrictions kept separate");
    assert(HeadStepLimit(1.0/90)==2000);
    assert(HeadStepLimit(.033)>5000&&HeadStepLimit(.05)==8192);
    assert(HeadStepLimit(11)==2000&&HeadStepLimit(-1)==2000&&HeadStepLimit(NAN)==2000);
    g_cinematicLook=false;g_pitchTarget=.15f;g_liveCtlPitch=.15f;
    const float pitchView[16]={0,0,1,1, 1,0,0,0, 0,1,0,0, 0,0,-1,0};
    memset(g_matrixPivot,0,sizeof(g_matrixPivot));
    for(int move:{1,15,9,10,27,28,17}){
        float m[16];memcpy(m,pitchView,64);g_liveCameraMove=(uint8_t)move;
        ApplyPitchFix(m,true);
        const float actual=asinf(m[11]/sqrtf(m[3]*m[3]+m[7]*m[7]+m[11]*m[11]));
        assert(fabsf(actual-((move==1||move==15)?.15f:0))<.00001f);
    }
    g_cinematicLook=true;g_pitchTarget=g_liveCtlPitch=0;
    puts("PASS: slow-frame turn budget, pause/nonfinite limits, rendered walking/crouch pitch correction, parkour animation pitch retained");
    const bool both[2]={true,true},leftOnly[2]={true,false},rightOnly[2]={false,true},neither[2]={false,false};
    const float leftHigher[2]={8430,8391},rightHigher[2]={8424,8430},equal[2]={8430,8430},invalid[2]={NAN,8430};
    assert(HigherEntryHand(both,leftHigher)==0); // user's failing ledge
    assert(HigherEntryHand(both,rightHigher)==1);
    assert(HigherEntryHand(both,equal)==0);
    assert(HigherEntryHand(leftOnly,rightHigher)==0); // stale other-hand sample excluded
    assert(HigherEntryHand(rightOnly,leftHigher)==1);
    assert(HigherEntryHand(neither,equal)==-1);
    assert(HigherEntryHand(both,invalid)==1);
    puts("PASS: higher settled hand chosen with either hand lower, ties, missing and invalid samples");
    for(int move:{9,10,27,28}){
        assert(AnimationOwnsYawReference(true,true,(uint8_t)move));
        assert(!AnimationOwnsYawReference(false,true,(uint8_t)move));
        assert(!AnimationOwnsYawReference(true,false,(uint8_t)move));
    }
    assert(!AnimationOwnsYawReference(true,true,1));
    const char* world="float4x4 vp:register(c0); float4 main(float4 p:POSITION):POSITION{return mul(p,vp);}";
    const char* screen="float4 main(float4 p:POSITION):POSITION{return p;}";
    const char* unused="float4x4 vp:register(c0); void main(float4 p:POSITION,out float4 pos:POSITION,out float4 uv:TEXCOORD0){pos=p;uv=mul(p,vp);}";
    for(const char* version:{"vs_2_0","vs_3_0"}){
        checkShader(world,version,true);checkShader(screen,version,false);checkShader(unused,version,false);
    }
    checkShader("float4x4 vp:register(c0);float4 axis[2]:register(c8);float index:register(c10);"
        "float4 main(float4 p:POSITION):POSITION{return mul(p+axis[(int)index],vp);}","vs_3_0",true);
    if(argc>1){
        checkCapture(argv[1],"23C40ECF",true); // sub-UV particle, indexed axis vectors
        checkCapture(argv[1],"746AB0D0",true); // regular particle/additive glow, same indexed lookup
        checkCapture(argv[1],"1612477A",true); // another sprite variant
        checkCapture(argv[1],"5A947094",true); // world shadow-volume geometry
        checkCapture(argv[1],"D74970B5",false); // screen-space fog reconstructs world position
        checkCapture(argv[1],"8C7E0467",false); // distortion composite
        checkCapture(argv[1],"5470E8CC",false); // HUD
        checkCapture(argv[1],"51B6BA4A",false); // sun haze needs ray reconstruction, not world geometry
        checkHazePatch(argv[1],argc>2&&!strcmp(argv[2],"--skip-device"));
        puts("PASS: the user's captured particle, sprite, shadow-volume and screen-composite shaders");
    }
    assert(!StereoShaderUsesViewProjection(nullptr,0,0));
    DWORD truncated[]={0xfffe0300,0x04000009,0x800f0000};
    assert(!StereoShaderUsesViewProjection(truncated,3,0));
    // +X forward, +Y screen right, +Z screen up; perspective camera at (500,20,30).
    float matrix[16]={0,0,1,1, 1,0,0,0, 0,1,0,0, -20,-30,-501,-500};
    float camera[3]={500,20,30},forward[3]={1,0,0},extracted[3]{};
    float inverse[16]{};
    assert(StereoInverseMatrix(matrix,inverse));
    for(int r=0;r<4;++r)for(int c=0;c<4;++c){
        float sum=0;for(int k=0;k<4;++k)sum+=matrix[r*4+k]*inverse[k*4+c];
        assert(fabsf(sum-(r==c?1.0f:0.0f))<0.001f);
    }
    // Perspective camera center is the preimage of (0,0,1,0).
    for(int i=0;i<3;++i)assert(fabsf(inverse[8+i]/inverse[11]-camera[i])<0.001f);
    float singular[16]{};assert(!StereoInverseMatrix(singular,inverse));
    singular[0]=NAN;assert(!StereoInverseMatrix(singular,inverse));
    assert(CameraFromVP(matrix,true,extracted));
    for(int i=0;i<3;++i)assert(fabsf(camera[i]-extracted[i])<0.001f);
    assert(SceneCameraMatches(matrix,true,camera,forward));
    float previous[3]={400,20,30};assert(!SceneCameraMatches(matrix,true,previous,forward));
    // Off-axis shadow matrix passed the old W-at-camera test despite being 200 UU away.
    float shadow[16];memcpy(shadow,matrix,sizeof(shadow));shadow[12]-=200;
    assert(!SceneCameraMatches(shadow,true,camera,forward));
    float turned[3]={0,1,0};assert(!SceneCameraMatches(matrix,true,camera,turned));
    float bad[16];memcpy(bad,matrix,sizeof(bad));bad[3]=NAN;
    assert(!SceneCameraMatches(bad,true,camera,forward));
    // Camera correction must not translate the camera when its game-thread sample is ahead.
    memcpy(g_matrixPivot,camera,sizeof(camera));assert(RotationPivot()==g_matrixPivot);
    g_cinematicYaw=0.3f;g_cinematicPitch=0.2f;
    ApplyCinematicLook(matrix,true);assert(CameraFromVP(matrix,true,extracted));
    for(int i=0;i<3;++i)assert(fabsf(camera[i]-extracted[i])<0.01f);
    assert(!SceneCameraMatches(matrix,true,camera,forward)); // free look actually turns
    memcpy(shadow,matrix,sizeof(shadow));g_scriptedHeadInEngine=true;ApplyCinematicLook(matrix,true);
    assert(!memcmp(shadow,matrix,sizeof(shadow))); // early camera must not rotate twice
    g_scriptedHeadInEngine=false;
    memcpy(shadow,matrix,sizeof(shadow));g_cinematicLook=false;ApplyCinematicLook(matrix,true);
    assert(!memcmp(shadow,matrix,sizeof(shadow)));
    // A rightward head turn stays screen-right for steep/overturned scripted cameras.
    // World-up yaw fails this below vertical; camera-local up keeps its sign.
    for(float pitch:{0.0f,-1.3f,-1.9f,1.9f}){
        const float c=cosf(pitch),s=sinf(pitch);
        float view[16]={0,-s,c,c, 1,0,0,0, 0,c,s,s, 0,0,-1,0};
        memset(g_matrixPivot,0,sizeof(g_matrixPivot));g_cinematicLook=true;
        g_cinematicYaw=-0.2f;g_cinematicPitch=0;
        ApplyCinematicLook(view,true);
        assert(view[7]>0.19f); // forward dot authored screen-right = sin(0.2)
        assert(fabsf(view[7]-sinf(0.2f))<0.0001f);
    }
    puts("PASS: compiled SM2/3 world shaders vs screen/unused VP; malformed input; moving cameras, off-axis shadows, cinematic rotation and pivot stability");
}
'@
$cpp=Join-Path $out 'stereo-effects.cpp'
($prefix+"`n"+($functions -join "`n")+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'stereo-effects.exe';$obj=Join-Path $out 'stereo-effects.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'" /link d3dcompiler.lib d3d9.lib user32.lib')
if($LASTEXITCODE -ne 0){throw 'stereo effects compile failed'}
if($CaptureDirectory){
    if($SkipDevice){& $exe (Resolve-Path -LiteralPath $CaptureDirectory).Path --skip-device}
    else{& $exe (Resolve-Path -LiteralPath $CaptureDirectory).Path}
}else{& $exe}
if($LASTEXITCODE -ne 0){throw 'stereo effects tests failed'}
