// Captured shader regression plus real D3D9 validation of the production guard.
#include "../src/d3d9.cpp"
#include <cassert>
#include <fstream>
#include <array>
using Vec=std::array<float,4>;
struct Registers { Vec temp[32]{},input[16]{},output[12]{},constant[256]{}; };
static unsigned RegisterType(DWORD t) {return ((t>>28)&7)|((t>>8)&24);}
static Vec Source(const Registers& r,DWORD t) {
    const unsigned index=t&0x7ff,type=RegisterType(t);
    const Vec& v=type==0?r.temp[index]:type==1?r.input[index]:r.constant[index];
    assert(type<=2);Vec value{};
    for(int c=0;c<4;++c)value[c]=v[(t>>(16+c*2))&3]*((t&0x0f000000)==0x01000000?-1.f:1.f);
    return value;
}
static Registers Execute(const std::vector<DWORD>& code,Registers r) {
    for(size_t at=1;at<code.size();) {
        DWORD op=code[at]&0xffff;unsigned n=(code[at]>>24)&15;
        if(op==D3DSIO_END)return r;
        if(op==D3DSIO_COMMENT){at+=1+((code[at]>>16)&0x7fff);continue;}
        if(op==D3DSIO_DCL){at+=1+n;continue;}
        DWORD dest=code[at+1];Vec value{};
        if(op==D3DSIO_DEF)memcpy(value.data(),&code[at+2],16);
        else {
            Vec a=Source(r,code[at+2]),b{},c{};
            if(n>=3)b=Source(r,code[at+3]);if(n>=4)c=Source(r,code[at+4]);
            for(int i=0;i<4;++i) {
                if(op==D3DSIO_MOV)value[i]=a[i];
                else if(op==D3DSIO_MUL)value[i]=a[i]*b[i];
                else if(op==D3DSIO_MAD)value[i]=a[i]*b[i]+c[i];
                else {assert(op==D3DSIO_DP3);value[i]=a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
            }
        }
        const unsigned type=RegisterType(dest),index=dest&0x7ff;
        assert(type==0||type==2||type==6);
        Vec& out=type==0?r.temp[index]:type==2?r.constant[index]:r.output[index];
        for(int i=0;i<4;++i)if(dest&(1<<(16+i)))out[i]=value[i];
        at+=1+n;
    }
    assert(false);return r;
}
static std::vector<DWORD> Load(const wchar_t* directory,const wchar_t* name) {
    std::wstring path=std::wstring(directory)+L"/mevr-effect-"+name+L".bin";
    std::ifstream in(path,std::ios::binary|std::ios::ate);assert(in);
    const auto size=(size_t)in.tellg();assert(size%4==0);std::vector<DWORD> code(size/4);
    in.seekg(0);in.read((char*)code.data(),size);assert(in);return code;
}
int wmain(int argc,wchar_t** argv) {
    assert(argc==2);auto code=Load(argv[1],L"8A7B075B.vs"),ps=Load(argv[1],L"4577ECC3.ps");
    std::vector<DWORD> patch;assert(StereoPatchGlassProjection(code.data(),code.size(),&patch));
    std::vector<DWORD> other;assert(!StereoPatchGlassProjection(patch.data(),patch.size(),&other));
    assert(!StereoPatchGlassProjection(code.data(),code.size()-1,&other));
    // Execute actual original/patched bytecode for different vertices, stereo
    // translations, rotations, and projection scales. Geometry must be identical
    // to the eye shader; the reflected UV must match the original mono camera.
    for(int trial=0;trial<100;++trial) {
        Registers r;
        for(int k=0;k<14;++k)for(int c=0;c<4;++c)r.constant[k][c]=sinf((float)(k*4+c+trial)*.19f);
        for(int k=0;k<7;++k)for(int c=0;c<4;++c)r.input[k][c]=(float)(k+c+trial)*.13f;
        for(int k=0;k<4;++k)r.constant[252+k]=r.constant[k];
        const auto mono=Execute(code,r);
        for(int k=0;k<4;++k)for(int c=0;c<4;++c)r.constant[k][c]+=cosf((float)(k*4+c+trial))*.1f;
        const auto eye=Execute(code,r),fixed=Execute(patch,r);
        for(int k=0;k<7;++k)for(int c=0;c<4;++c)
            assert(fabsf(fixed.output[k][c]-(k==3?mono.output[k][c]:eye.output[k][c]))<.0001f);
    }
    puts("PASS captured bytecode: reflection projection isolated; eye geometry and all other outputs unchanged");
    HMODULE library=LoadLibraryExW(L"d3d9.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);assert(library);
    auto create=(IDirect3D9*(WINAPI*)(UINT))GetProcAddress(library,"Direct3DCreate9");assert(create);
    auto api=create(D3D_SDK_VERSION);assert(api);
    HWND window=CreateWindowExW(0,L"STATIC",L"MEVR reflection test",WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);assert(window);
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
    pp.BackBufferWidth=320;pp.BackBufferHeight=240;
    IDirect3DDevice9* dev=nullptr;
    assert(SUCCEEDED(api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&dev)));
    IDirect3DVertexShader9* vs=nullptr;IDirect3DPixelShader9* pixel=nullptr;IDirect3DTexture9* texture=nullptr;
    assert(SUCCEEDED(dev->CreateVertexShader(code.data(),&vs)));
    assert(SUCCEEDED(dev->CreatePixelShader(ps.data(),&pixel)));
    assert(SUCCEEDED(dev->CreateTexture(1280,720,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&texture,nullptr)));
    g_vmReg=0;g_vmRow=true;g_sceneW=g_capW=320;g_sceneH=g_capH=240;
    g_origSetVSConstF=[](IDirect3DDevice9* d,UINT reg,const float* values,UINT count)->HRESULT{return d->SetVertexShaderConstantF(reg,values,count);};
    float saved[16],got[16];for(int i=0;i<16;++i){saved[i]=float(i+10);g_sceneMat[i]=float(i+1);}
    dev->SetVertexShader(vs);dev->SetPixelShader(pixel);dev->SetTexture(4,texture);dev->SetVertexShaderConstantF(252,saved,4);
    auto current=[&](){IDirect3DVertexShader9* value=nullptr;dev->GetVertexShader(&value);if(value)value->Release();return value;};
    {
        WorldGlassReflection guard(dev);assert(guard.original==vs);assert(current()!=vs);
        dev->GetVertexShaderConstantF(252,got,4);assert(!memcmp(got,g_sceneMat,64));
    }
    assert(current()==vs);dev->GetVertexShaderConstantF(252,got,4);assert(!memcmp(got,saved,64));
    dev->SetPixelShader(nullptr);{WorldGlassReflection guard(dev);assert(!guard.original);assert(current()==vs);}
    dev->SetPixelShader(pixel);dev->SetTexture(4,nullptr);{WorldGlassReflection guard(dev);assert(!guard.original);}
    dev->SetTexture(4,texture);g_sceneW=1280;g_sceneH=720;{WorldGlassReflection guard(dev);assert(!guard.original);}
    g_sceneW=320;g_sceneH=240;g_vmRow=false;{WorldGlassReflection guard(dev);assert(!guard.original);}
    dev->SetVertexShader(nullptr);dev->SetPixelShader(nullptr);dev->SetTexture(4,nullptr);
    ResetEffectShaders();vs->Release();pixel->Release();texture->Release();dev->Release();api->Release();DestroyWindow(window);FreeLibrary(library);
    puts("PASS real D3D9 shader creation, exact material/texture guards, shader and constant restoration");
}
