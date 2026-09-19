#include "stereo_shader.h"
#include <unordered_map>
struct ScreenSamplerShader {
    IDirect3DPixelShader9* shader=nullptr;
    int scale=-1,sampler=-1;
};
static std::unordered_map<IDirect3DPixelShader9*,ScreenSamplerShader> g_screenSamplers;
static void ResetScreenSamplers()
{
    for(auto& pair:g_screenSamplers)if(pair.second.shader)pair.second.shader->Release();
    g_screenSamplers.clear();
}
static ScreenSamplerShader* FindScreenSampler(IDirect3DDevice9* dev)
{
    IDirect3DPixelShader9* shader=nullptr;
    if(FAILED(dev->GetPixelShader(&shader))||!shader)return nullptr;
    auto found=g_screenSamplers.find(shader);
    if(found!=g_screenSamplers.end()){shader->Release();return &found->second;}
    // Cache every material in a level; a tiny FIFO reparsed hundreds of shaders
    // every frame as soon as its working set exceeded the cache size.
    if(g_screenSamplers.size()>=8192)ResetScreenSamplers();
    auto& s=g_screenSamplers[shader];s.shader=shader;
    UINT bytes=0;
    if(SUCCEEDED(shader->GetFunction(nullptr,&bytes))&&bytes>=8&&bytes<=65536&&!(bytes%4)){
        std::vector<DWORD> code(bytes/4);
        if(SUCCEEDED(shader->GetFunction(code.data(),&bytes))){
            s.scale=StereoConstantRegister(code.data(),code.size(),"ScreenPositionScaleBias",2);
            s.sampler=StereoConstantRegister(code.data(),code.size(),"LightAttenuationTexture",3);
            if(s.sampler<0)s.sampler=StereoConstantRegister(code.data(),code.size(),"SceneColorTexture",3);
        }
    }
    return &s;
}
struct WorldScreenSampling {
    IDirect3DDevice9* dev;int reg=-1;float original[4]{};
    explicit WorldScreenSampling(IDirect3DDevice9* device):dev(device)
    {
        auto* shader=FindScreenSampler(dev);
        if(!shader||shader->scale<0||shader->scale>=224||shader->sampler<0||shader->sampler>=16)return;
        IDirect3DBaseTexture9* base=nullptr;
        if(FAILED(dev->GetTexture(shader->sampler,&base))||!base)return;
        bool screen=false;
        if(base->GetType()==D3DRTYPE_TEXTURE){
            D3DSURFACE_DESC desc{};
            screen=SUCCEEDED(static_cast<IDirect3DTexture9*>(base)->GetLevelDesc(0,&desc))&&
                (desc.Usage&D3DUSAGE_RENDERTARGET)&&desc.Width==(g_sceneW?g_sceneW:g_capW)&&
                desc.Height==(g_sceneH?g_sceneH:g_capH);
        }
        base->Release();
        if(screen&&SUCCEEDED(dev->GetPixelShaderConstantF(shader->scale,original,1)))reg=shader->scale;
    }
    void Eye(int eye) {
        if(reg<0)return;
        // ScaleBias.z is Y bias and .w is X bias in the shipped shaders.
        // Preserve the original half-texel offset in full-texture texels.
        const float uv[4]={original[0]*0.5f,original[1],original[2],original[3]-0.25f+eye*0.5f};
        dev->SetPixelShaderConstantF(reg,uv,1);
    }
    ~WorldScreenSampling(){if(reg>=0)dev->SetPixelShaderConstantF(reg,original,1);}
};
