$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root '.analysis/sun-trace-device-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$capture=Get-Content (Join-Path $root 'src/effect_capture.inl') -Raw
$save=$capture.Substring(0,$capture.IndexOf('struct EffectPassCapture {'))
$trace=Get-Content (Join-Path $root 'src/sun_trace.inl') -Raw
$prefix=@'
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <algorithm>
#include <cassert>
static long g_effectCaptureUntil=103,g_frames=101;
static bool g_inDupDraw=false;
static wchar_t g_logPath[MAX_PATH]{};
static bool PathSibling(const wchar_t*,const wchar_t* leaf,wchar_t* out,size_t size){return wcscpy_s(out,size,leaf)==0;}
static void Log(const char* f,...){va_list a;va_start(a,f);vprintf(f,a);va_end(a);puts("");}
struct EffectShader {IDirect3DVertexShader9* shader;bool scene;uint32_t hash;};
static EffectShader* FindEffectShader(IDirect3DDevice9*){return nullptr;}
static uint32_t CurrentEffectPixelHash(IDirect3DDevice9*){return 0;}
static bool ShouldDuplicate(){return true;}
static bool SuppressLensFlare(IDirect3DDevice9*){return false;}
template<class T> static uint32_t SaveEffectShader(T*,const wchar_t*){return 0;}
'@
$tests=@'
static bool succeeded(HRESULT hr,const char* what){if(FAILED(hr)){printf("FAIL %s: %08lX\n",what,(unsigned long)hr);return false;}return true;}
int main(){
    HWND window=CreateWindowW(L"STATIC",L"Hidden VR capture test",WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    IDirect3D9* d3d=Direct3DCreate9(D3D_SDK_VERSION);if(!window||!d3d)return 2;
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
    pp.BackBufferWidth=320;pp.BackBufferHeight=240;pp.BackBufferFormat=D3DFMT_UNKNOWN;
    IDirect3DDevice9* dev=nullptr;
    if(!succeeded(d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&dev),"CreateDevice"))return 3;
    IDirect3DSurface9* original=nullptr;dev->GetRenderTarget(0,&original);
    for(D3DFORMAT fmt:{D3DFMT_A8R8G8B8,D3DFMT_A16B16G16R16F}){
        IDirect3DSurface9* surface=nullptr;
        if(!succeeded(dev->CreateRenderTarget(1024,512,fmt,D3DMULTISAMPLE_NONE,0,FALSE,&surface,nullptr),"CreateRenderTarget"))return 4;
        if(!succeeded(dev->SetRenderTarget(0,surface),"SetRenderTarget"))return 5;
        dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xFF0000FF,1,0);
        if(!succeeded(dev->BeginScene(),"BeginScene"))return 6;
        {SunDrawCapture draw(dev,0,2);if(!draw.target)return 7;
            dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xFFFF0000,1,0);}
        auto& r=g_sunTrace.records[0];
        if(!succeeded(r.beforeResult,"copy before")||!succeeded(r.afterResult,"copy after"))return 8;
        // Verify real GPU output: the two tiles differ at their centers.
        IDirect3DSurface9* readback=nullptr;
        if(!succeeded(dev->CreateOffscreenPlainSurface(2048,2048,fmt,D3DPOOL_SYSTEMMEM,&readback,nullptr),"readback allocation"))return 9;
        dev->EndScene();
        if(!succeeded(dev->GetRenderTargetData(g_sunTrace.atlas,readback),"readback"))return 10;
        D3DLOCKED_RECT locked{};if(!succeeded(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY),"lock"))return 11;
        unsigned bytes=fmt==D3DFMT_A8R8G8B8?4:8;
        auto* row=(unsigned char*)locked.pBits+64*locked.Pitch;
        if(!memcmp(row+128*bytes,row+384*bytes,bytes)){puts("FAIL: GPU before/after tiles identical");return 12;}
        readback->UnlockRect();readback->Release();
        g_sunTrace.Finish();
        dev->SetRenderTarget(0,original);surface->Release();
        g_effectCaptureUntil+=100;g_frames+=100;
    }
    g_sunTrace.Discard();original->Release();dev->Release();d3d->Release();DestroyWindow(window);
    puts("PASS: actual D3D9 GPU before/after copies, atlas pixels, PNG save and metadata for ordinary and HDR scene formats");
}
'@
$cpp=Join-Path $out 'sun-trace-device.cpp'
($prefix+"`n"+$save+"`n"+$trace+"`n"+$tests)|Set-Content $cpp
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe=Join-Path $out 'sun-trace-device.exe';$obj=Join-Path $out 'sun-trace-device.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'" /link d3d9.lib user32.lib')
if($LASTEXITCODE -ne 0){throw 'device test compile failed'}
Push-Location $out
try { & $exe; if($LASTEXITCODE -ne 0){throw "device tests failed: $LASTEXITCODE"} }
finally { Pop-Location }
