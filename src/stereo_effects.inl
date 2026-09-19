#include "stereo_shader.h"
#include "stereo_math.h"
static bool PathSibling(const wchar_t* full,const wchar_t* leaf,wchar_t* out,size_t cap);
struct EffectShader {
    IDirect3DVertexShader9* shader=nullptr;
    int reg=-1;bool scene=false;uint32_t hash=0;
    IDirect3DVertexShader9* hazeStereo=nullptr;
    IDirect3DVertexShader9* glassStereo=nullptr;
};
static EffectShader g_effectShaders[256]{};
static unsigned g_effectShaderCursor=0;
static IDirect3DPixelShader9* g_hazePixelShader=nullptr;
static uint32_t g_effectPixelHash=0;
static void ReleaseEffectShader(EffectShader& entry)
{
    if(entry.shader)entry.shader->Release();
    if(entry.hazeStereo)entry.hazeStereo->Release();
    if(entry.glassStereo)entry.glassStereo->Release();
    entry={};
}
static void ResetEffectShaders()
{
    ResetScreenSamplers();
    for(auto& entry:g_effectShaders)ReleaseEffectShader(entry);
    if(g_hazePixelShader)g_hazePixelShader->Release();
    g_hazePixelShader=nullptr;g_effectPixelHash=0;
    g_effectShaderCursor=0;
}
static uint32_t EffectShaderHash(const std::vector<DWORD>& code)
{
    uint32_t hash=2166136261u;for(DWORD word:code){hash^=word;hash*=16777619u;}return hash;
}
static EffectShader* FindEffectShader(IDirect3DDevice9* dev)
{
    IDirect3DVertexShader9* shader=nullptr;
    if(FAILED(dev->GetVertexShader(&shader))||!shader)return nullptr;
    for(auto& entry:g_effectShaders)if(entry.shader==shader&&entry.reg==g_vmReg){
        shader->Release();return &entry;
    }
    UINT size=0;std::vector<DWORD> code;
    if(SUCCEEDED(shader->GetFunction(nullptr,&size))&&size>=8&&size<=65536&&!(size%4)){
        code.resize(size/4);if(FAILED(shader->GetFunction(code.data(),&size)))code.clear();
    }
    const bool scene=StereoShaderUsesViewProjection(code.data(),code.size(),(unsigned)g_vmReg);
    auto& entry=g_effectShaders[g_effectShaderCursor++%256];
    ReleaseEffectShader(entry);
    // Keep the COM reference: pointer reuse must never inherit a different shader's verdict.
    entry={shader,g_vmReg,scene,EffectShaderHash(code)};
    // Exact shaders captured from this executable: InvViewProj c5..8,
    // ViewOrigin c9, POSITION passed through, scene UV in TEXCOORD0.
    if(entry.hash==0x51B6BA4Au){
        std::vector<DWORD> patched;
        if(StereoPatchHazeUV(code.data(),code.size(),&patched)) {
            const HRESULT hr=dev->CreateVertexShader(patched.data(),&entry.hazeStereo);
            Log("[stereo-fx] sun haze per-eye shader creation: 0x%08lX",(unsigned long)hr);
        }
    }
    if(entry.hash==0x8A7B075Bu){
        std::vector<DWORD> patched;
        if(StereoPatchGlassProjection(code.data(),code.size(),&patched)) {
            const HRESULT hr=dev->CreateVertexShader(patched.data(),&entry.glassStereo);
            Log("[stereo-fx] glass capture projection shader creation: 0x%08lX",(unsigned long)hr);
        }
    }
    return &entry;
}
static bool WorldUPUsesSceneTransform(IDirect3DDevice9* dev)
{
    const auto* entry=FindEffectShader(dev);return entry&&entry->scene;
}
static uint32_t CurrentEffectPixelHash(IDirect3DDevice9* dev)
{
    IDirect3DPixelShader9* shader=nullptr;
    if(FAILED(dev->GetPixelShader(&shader))||!shader)return 0;
    if(shader==g_hazePixelShader){shader->Release();return g_effectPixelHash;}
    if(g_hazePixelShader)g_hazePixelShader->Release();
    g_hazePixelShader=shader;g_effectPixelHash=0;
    UINT size=0;
    if(SUCCEEDED(shader->GetFunction(nullptr,&size))&&size>=8&&size<=65536&&size%4==0){
        std::vector<DWORD> code(size/4);
        if(SUCCEEDED(shader->GetFunction(code.data(),&size)))g_effectPixelHash=EffectShaderHash(code);
    }
    return g_effectPixelHash;
}
static bool SunHazePixelMatches(IDirect3DDevice9* dev){return CurrentEffectPixelHash(dev)==0xF2935D14u;}

WorldGlassReflection::WorldGlassReflection(IDirect3DDevice9* device):dev(device)
{
    if(!g_vmRow || g_vmReg!=0)return;
    const auto* entry=FindEffectShader(dev);
    if(!entry || !entry->glassStereo || CurrentEffectPixelHash(dev)!=0x4577ECC3u)return;
    // The material's s4 is a separate mono reflection render target. Never
    // apply this rewrite to a material sampling a packed stereo scene target.
    IDirect3DBaseTexture9* texture=nullptr;
    if(FAILED(dev->GetTexture(4,&texture)) || !texture)return;
    D3DSURFACE_DESC desc{};
    const bool capture=texture->GetType()==D3DRTYPE_TEXTURE &&
        SUCCEEDED(((IDirect3DTexture9*)texture)->GetLevelDesc(0,&desc)) &&
        (desc.Usage&D3DUSAGE_RENDERTARGET) &&
        !(desc.Width==g_sceneW && desc.Height==g_sceneH) &&
        !(desc.Width==g_capW && desc.Height==g_capH);
    texture->Release();
    if(!capture || FAILED(dev->GetVertexShaderConstantF(252,constants,4)))return;
    if(FAILED(g_origSetVSConstF(dev,252,g_sceneMat,4))){g_origSetVSConstF(dev,252,constants,4);return;}
    if(FAILED(dev->SetVertexShader(entry->glassStereo))){g_origSetVSConstF(dev,252,constants,4);return;}
    original=entry->shader;original->AddRef();
}
WorldGlassReflection::~WorldGlassReflection()
{
    if(!original)return;
    dev->SetVertexShader(original);g_origSetVSConstF(dev,252,constants,4);original->Release();
}
static bool g_lensFlares=false;
static bool g_nativeLensFlareSuppression=false;
static bool g_stereoUI=true;
static bool SuppressLensFlare(IDirect3DDevice9* dev)
{
    if(g_lensFlares||g_inDupDraw)return false;
    const auto* shader=FindEffectShader(dev);
    if(!shader)return false;
    const uint32_t ps=CurrentEffectPixelHash(dev);
    // Captured glow/ring materials plus the sun's dot-chain atlas (11611,
    // draws 293..311). Keep the separate A1D87F1F sun glow correction intact.
    // Other billboards, particles, sun haze and bloom are intentionally retained.
    const bool suppress=(shader->hash==0x1612477Au&&
        (ps==0x1468B5C6u||ps==0x619A63AFu||ps==0x1F63E387u))||
        (shader->hash==0xE6318E88u&&ps==0x6942F53Du);
    if(suppress){static long next=0;if(g_frames>=next){
        Log("[stereo-fx] LensFlares=off suppressed captured flare material %08X",ps);next=g_frames+1800;}}
    return suppress;
}

struct CanvasPopupRun {
    long frame=-1;
    bool active=false;
    D3DVIEWPORT9 viewport{};
};
static CanvasPopupRun g_canvasPopup;

template<class FN> static bool DrawStereoCanvas(IDirect3DDevice9* dev,FN issue,HRESULT* result,UINT primitives=0)
{
    if(g_inDupDraw)return false;
    // Marker 20954: the lower tutorial strip starts with a 9-slice 1024x128
    // backdrop, followed by its icon/font batches. Marker 23176's Back menu
    // starts with a different backdrop. Scope placement to this contiguous run,
    // never to all Canvas text (the two panels share their text shader/atlases).
    const bool wasPopup=g_canvasPopup.active&&g_canvasPopup.frame==g_frames;
    g_canvasPopup.active=false; // any rejected/non-Canvas draw terminates the run
    if(!g_stereoUI||!g_simulStereo||g_inDupDraw||g_sceneSplitMono||g_scenePartialMono||
        !g_sceneMatValid||InterlockedCompareExchange(&g_dupFrameDraws,0,0)<=0)return false;
    const auto* shader=FindEffectShader(dev);
    if(!shader)return false;
    // The pause menu's red material panel uses the ordinary mesh vertex
    // factory with a Canvas projection at c0; text uses Canvas c5 instead.
    // Exact material match prevents routing other DF014469 world meshes here.
    const bool materialPanel=shader->hash==0xDF014469u&&CurrentEffectPixelHash(dev)==0x33DB2A77u;
    if(shader->hash!=0x5470E8CCu&&!materialPanel)return false;
    DWORD depth=1,color=0,scissor=0;
    if(FAILED(dev->GetRenderState(D3DRS_ZENABLE,&depth))||depth||
        FAILED(dev->GetRenderState(D3DRS_COLORWRITEENABLE,&color))||!color)return false;
    // Canvas uses both affine HUD transforms and a perspective pixel-coordinate
    // projection for tutorial/menu panels (captured c7.w=1, c8.w=2113).
    // Its perspective transform is NOT a world camera. Keep the exact factory,
    // target, depth and scene-texture guards for both forms.
    float transform[16]{};
    if(FAILED(dev->GetVertexShaderConstantF(materialPanel?0:5,transform,4))||fabsf(transform[3])>1e-6f||
        fabsf(transform[7])>1e-6f)return false;
    for(float value:transform)if(!std::isfinite(value))return false;
    const bool affine=fabsf(transform[11])<1e-6f&&fabsf(transform[15]-1)<1e-5f;
    const bool canvasPerspective=fabsf(transform[11]-1)<1e-6f&&transform[15]>1&&
        transform[0]>0&&transform[5]<0&&fabsf(transform[1])+fabsf(transform[2])+
        fabsf(transform[4])+fabsf(transform[6])+fabsf(transform[8])+fabsf(transform[9])<1e-6f;
    if(materialPanel?!canvasPerspective:(!affine&&!canvasPerspective))return false;
    IDirect3DSurface9* target=nullptr;D3DSURFACE_DESC desc{};
    if(FAILED(dev->GetRenderTarget(0,&target))||!target)return false;
    const bool valid=SUCCEEDED(target->GetDesc(&desc))&&desc.Width==g_capW&&desc.Height==g_capH;
    target->Release();if(!valid)return false;
    // A full scene image must not be squeezed into each eye a second time.
    D3DSURFACE_DESC atlas{};
    for(DWORD slot=0;slot<8;++slot){IDirect3DBaseTexture9* base=nullptr;
        if(SUCCEEDED(dev->GetTexture(slot,&base))&&base){bool scene=false;
            if(base->GetType()==D3DRTYPE_TEXTURE){D3DSURFACE_DESC td{};
                if(SUCCEEDED(static_cast<IDirect3DTexture9*>(base)->GetLevelDesc(0,&td))){
                    if(slot==0)atlas=td;
                    scene=(td.Usage&D3DUSAGE_RENDERTARGET)&&td.Width==g_capW&&td.Height==g_capH;}}
            base->Release();if(scene)return false;}}
    D3DVIEWPORT9 original{};RECT originalClip{};
    if(FAILED(dev->GetViewport(&original))||original.Width<2)return false;
    if(FAILED(dev->GetRenderState(D3DRS_SCISSORTESTENABLE,&scissor))||
        (scissor&&FAILED(dev->GetScissorRect(&originalClip))))return false;
    bool popupThisDraw=false;
    if(canvasPerspective&&shader->hash==0x5470E8CCu&&CurrentEffectPixelHash(dev)==0xB3B12B1Du&&
        atlas.Format==D3DFMT_DXT5&&!(atlas.Usage&D3DUSAGE_RENDERTARGET)) {
        const bool backdrop=primitives==18&&atlas.Width==1024&&atlas.Height==128&&
            fabsf(transform[15]-(original.Width*.5f+1.f))<.05f;
        const bool text=atlas.Width==256&&(atlas.Height==128||atlas.Height==256)&&
            fabsf(transform[15]-original.Width*.5f)<.05f;
        g_canvasPopup.active=backdrop||(wasPopup&&text&&!memcmp(&original,&g_canvasPopup.viewport,sizeof(original)));
        popupThisDraw=g_canvasPopup.active;
    }
    // Marker 4591 draws an affine HUD reticle between the backdrop and text.
    // Keep the pending group, without applying its placement to that HUD draw.
    if(affine&&wasPopup&&!memcmp(&original,&g_canvasPopup.viewport,sizeof(original)))g_canvasPopup.active=true;
    g_canvasPopup.frame=g_frames;g_canvasPopup.viewport=original;
    // A modest angular reduction suggests more distance; lifting the whole run
    // brings text out of the lower lens area. Native menu size/height still apply.
    const float uiScale=g_gameUiScale*(popupThisDraw?.9f:1.f);
    const float uiHeight=g_gameUiHeight+(popupThisDraw?.12f:0.f);
    *result=D3D_OK;g_inDupDraw=true;
    for(int eye=0;eye<2;++eye){D3DVIEWPORT9 vp=original;
        const DWORD eyeWidth=original.Width/2;
        vp.Width=(DWORD)(std::max)(1.f,floorf(eyeWidth*uiScale));
        vp.Height=(DWORD)(std::max)(1.f,floorf(original.Height*uiScale));
        vp.X=original.X+eye*eyeWidth+(eyeWidth-vp.Width)/2;
        const int y=(int)((original.Height-vp.Height)/2)-(int)lroundf(uiHeight*original.Height);
        vp.Y=original.Y+(DWORD)(std::max)(0,(std::min)((int)(original.Height-vp.Height),y));
        dev->SetViewport(&vp);
        if(scissor){RECT clip=originalClip;
            const double sx=(double)vp.Width/original.Width,sy=(double)vp.Height/original.Height;
            clip.left=(LONG)vp.X+(LONG)floor((originalClip.left-(LONG)original.X)*sx);
            clip.right=(LONG)vp.X+(LONG)ceil((originalClip.right-(LONG)original.X)*sx);
            clip.top=(LONG)vp.Y+(LONG)floor((originalClip.top-(LONG)original.Y)*sy);
            clip.bottom=(LONG)vp.Y+(LONG)ceil((originalClip.bottom-(LONG)original.Y)*sy);
            dev->SetScissorRect(&clip);}
        const HRESULT hr=issue();if(FAILED(hr))*result=hr;
    }
    if(scissor)dev->SetScissorRect(&originalClip);
    dev->SetViewport(&original);g_inDupDraw=false;
    static long next=0;if(g_frames>=next){Log("[stereo-fx] Canvas UI duplicated with per-eye clipping");next=g_frames+1800;}
    return true;
}

template<class FN> static bool DrawStereoShadowProjection(IDirect3DDevice9* dev,FN issue,HRESULT* result)
{
    if(!g_simulStereo||g_inDupDraw||!g_sceneMatValid||!g_vmRow)return false;
    const auto* shader=FindEffectShader(dev);
    if(!shader||shader->hash!=0x5A947094u)return false;
    const uint32_t pixel=CurrentEffectPixelHash(dev);
    if((pixel!=0x00693E34u&&pixel!=0xC7EAFDDEu)||!ShouldDuplicate())return false;
    const UINT registers=pixel==0x00693E34u?8:4;
    D3DVIEWPORT9 original{};float uv[4]{},saved[32]{},matrices[2][16]{},rebased[2][32]{};
    if(FAILED(dev->GetViewport(&original))||original.Width<2||
        FAILED(dev->GetPixelShaderConstantF(1,uv,1))||
        FAILED(dev->GetPixelShaderConstantF(10,saved,registers)))return false;
    for(int eye=0;eye<2;++eye){
        float rebase[16];BuildEyeMatrix(matrices[eye],eye);
        if(!StereoDepthRebase(g_sceneMat,matrices[eye],rebase))return false;
        for(UINT offset=0;offset<registers*4;offset+=16)
            StereoMultiplyMatrix(rebase,saved+offset,rebased[eye]+offset);
    }
    WorldDrawScissor scissor(dev);
    g_inDupDraw=true;*result=D3D_OK;
    for(int eye=0;eye<2;++eye){
        D3DVIEWPORT9 viewport=original;viewport.Width/=2;viewport.X+=eye*viewport.Width;
        dev->SetViewport(&viewport);g_origSetVSConstF(dev,(UINT)g_vmReg,matrices[eye],4);
        const float packedUV[4]={uv[0]*0.5f,uv[1],uv[2],uv[3]*0.5f+eye*0.5f};
        dev->SetPixelShaderConstantF(1,packedUV,1);
        dev->SetPixelShaderConstantF(10,rebased[eye],registers);
        const HRESULT hr=issue();if(FAILED(hr))*result=hr;
    }
    dev->SetPixelShaderConstantF(1,uv,1);dev->SetPixelShaderConstantF(10,saved,registers);
    g_origSetVSConstF(dev,(UINT)g_vmReg,g_sceneMat,4);dev->SetViewport(&original);g_inDupDraw=false;
    static long next=0;
    if(g_frames>=next){Log("[stereo-fx] shadow projection %08X rebased for both eyes at frame %ld",pixel,g_frames);next=g_frames+1800;}
    return true;
}
template<class FN> static bool DrawStereoSunHaze(IDirect3DDevice9* dev,FN issue,HRESULT* result)
{
    if(!g_simulStereo||g_inDupDraw||!g_sceneMatValid||!g_vmRow)return false;
    auto* shader=FindEffectShader(dev);
    if(!shader||!shader->hazeStereo||!SunHazePixelMatches(dev)||!ShouldDuplicate())return false;
    D3DVIEWPORT9 original{};float saved[20]{},savedUV[8]{},inverse[2][16]{},eyeOrigin[2][4]{};
    if(FAILED(dev->GetViewport(&original))||original.Width<2||
        FAILED(dev->GetVertexShaderConstantF(5,saved,5))||
        FAILED(dev->GetVertexShaderConstantF(254,savedUV,2)))return false;
    for(int eye=0;eye<2;++eye){
        float matrix[16];BuildEyeMatrix(matrix,eye);
        if(!StereoInverseMatrix(matrix,inverse[eye])||fabsf(inverse[eye][11])<1e-8f)return false;
        for(int c=0;c<3;++c)eyeOrigin[eye][c]=inverse[eye][8+c]/inverse[eye][11];
        eyeOrigin[eye][3]=saved[19];
    }
    g_inDupDraw=true;dev->SetVertexShader(shader->hazeStereo);
    *result=D3D_OK;
    for(int eye=0;eye<2;++eye){
        D3DVIEWPORT9 viewport=original;viewport.Width/=2;viewport.X+=eye*viewport.Width;
        dev->SetViewport(&viewport);
        g_origSetVSConstF(dev,5,inverse[eye],4);g_origSetVSConstF(dev,9,eyeOrigin[eye],1);
        const float uv[8]={0.5f,1,1,1,eye*0.5f,0,0,0};g_origSetVSConstF(dev,254,uv,2);
        const HRESULT hr=issue();if(FAILED(hr))*result=hr;
    }
    g_origSetVSConstF(dev,5,saved,5);g_origSetVSConstF(dev,254,savedUV,2);
    dev->SetVertexShader(shader->shader);dev->SetViewport(&original);g_inDupDraw=false;
    static long next=0;
    if(g_frames>=next){Log("[stereo-fx] sun haze drawn with per-eye rays and packed scene UVs at frame %ld",g_frames);next=g_frames+1800;}
    return true;
}
// Sky/glow proxies must not acquire near-object stereo disparity. The ordered
// captures identify E6318E88/A1D87F1F as the extra sun (4686 draw 339, 10322 draw
// 234); the earlier 1612477A/2C531FCD pair was not its producing draw.
// Keep head rotation/FOV, but anchor both eye cameras at the sprite's original
// CameraPosition. Other particle materials retain their full positional stereo.
template<class FN> static bool DrawStereoDistantSun(IDirect3DDevice9* dev,FN issue,HRESULT* result)
{
    if(!g_simulStereo||g_inDupDraw||!g_sceneMatValid||!g_vmRow||!g_c0IsScene||g_vmReg!=0)return false;
    const auto* shader=FindEffectShader(dev);
    if(!shader)return false;
    const uint32_t ps=CurrentEffectPixelHash(dev);
    const bool sun=(shader->hash==0xE6318E88u&&ps==0xA1D87F1Fu)||
        (shader->hash==0x1612477Au&&ps==0x2C531FCDu);
    if(!sun||!ShouldDuplicate())return false;
    D3DVIEWPORT9 original{};float camera[4]{},matrices[2][16]{};
    if(FAILED(dev->GetViewport(&original))||original.Width<2||
        FAILED(dev->GetVertexShaderConstantF(4,camera,1)))return false;
    for(float v:camera)if(!std::isfinite(v))return false;
    for(int eye=0;eye<2;++eye){
        BuildEyeMatrix(matrices[eye],eye);float inverse[16];
        if(!StereoInverseMatrix(matrices[eye],inverse)||fabsf(inverse[11])<1e-8f)return false;
        float delta[3];for(int a=0;a<3;++a)delta[a]=inverse[8+a]/inverse[11]-camera[a];
        for(int c=0;c<4;++c)for(int a=0;a<3;++a)matrices[eye][12+c]+=delta[a]*matrices[eye][a*4+c];
    }
    g_inDupDraw=true;*result=D3D_OK;
    for(int eye=0;eye<2;++eye){
        D3DVIEWPORT9 vp=original;vp.Width/=2;vp.X+=eye*vp.Width;dev->SetViewport(&vp);
        g_origSetVSConstF(dev,0,matrices[eye],4);
        const HRESULT hr=issue();if(FAILED(hr))*result=hr;
    }
    g_origSetVSConstF(dev,0,g_sceneMat,4);dev->SetViewport(&original);g_inDupDraw=false;
    static long next=0;if(g_frames>=next){Log("[stereo-fx] distant sun sprite: rotation-only stereo");next=g_frames+1800;}
    return true;
}

template<class Shader> static uint32_t SaveEffectShader(Shader* shader,const wchar_t* kind)
{
    if(!shader)return 0;
    UINT bytes=0;
    if(FAILED(shader->GetFunction(nullptr,&bytes))||bytes<8||bytes>65536||bytes%4)return 0;
    std::vector<DWORD> code(bytes/4);
    if(FAILED(shader->GetFunction(code.data(),&bytes)))return 0;
    const uint32_t hash=EffectShaderHash(code);
    wchar_t name[80]{},path[MAX_PATH]{};
    _snwprintf_s(name,_TRUNCATE,L"mevr-effect-%08X.%s.bin",hash,kind);
    if(PathSibling(g_logPath,name,path,MAX_PATH)){
        HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE){DWORD written=0;WriteFile(file,code.data(),bytes,&written,nullptr);CloseHandle(file);}
    }
    return hash;
}
// Only runs during a user marker. These are the captured additive sprite/flare
// vertex factories; record texture identity and input geometry to distinguish a
// reflected flare element from ordinary world particles using the same shaders.
static void CaptureFlareDetails(IDirect3DDevice9* dev,uint32_t vertexHash,uint32_t pixelHash,
                                const void* vertices,UINT vertexCount,UINT stride,const char* route)
{
    static long serial=-1;static unsigned draws=0,textures=0,regular=0,flares=0;static uintptr_t seenTextures[16]{};
    if(serial!=g_effectCaptureUntil){serial=g_effectCaptureUntil;draws=textures=regular=flares=0;}
    // A flock of birds must not consume the entire budget before flare sprites.
    if(vertexHash==0x1612477Au||vertexHash==0xE6318E88u){if(flares>=16)return;++flares;}
    else {if(regular>=8)return;++regular;}
    const unsigned draw=draws++;
    Log("[flare-marker] capture=%ld draw=%u route=%s frame=%ld VS=%08X PS=%08X vertices=%u stride=%u",
        serial,draw,route,g_frames,vertexHash,pixelHash,vertexCount,stride);
    float constants[128]{};
    if(SUCCEEDED(dev->GetVertexShaderConstantF(0,constants,32)))
        for(unsigned i=0;i<32;++i)Log("[flare-marker] VS c%u %.9g %.9g %.9g %.9g",i,
            constants[i*4],constants[i*4+1],constants[i*4+2],constants[i*4+3]);
    if(SUCCEEDED(dev->GetPixelShaderConstantF(0,constants,16)))
        for(unsigned i=0;i<16;++i)Log("[flare-marker] PS c%u %.9g %.9g %.9g %.9g",i,
            constants[i*4],constants[i*4+1],constants[i*4+2],constants[i*4+3]);
    IDirect3DVertexDeclaration9* declaration=nullptr;
    if(SUCCEEDED(dev->GetVertexDeclaration(&declaration))&&declaration){
        D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT count=MAXD3DDECLLENGTH+1;
        if(SUCCEEDED(declaration->GetDeclaration(elements,&count)))
            for(UINT i=0;i<count&&i<MAXD3DDECLLENGTH&&elements[i].Stream!=0xff;++i){const auto& e=elements[i];
                Log("[flare-marker] decl stream=%u offset=%u type=%u usage=%u index=%u",e.Stream,e.Offset,e.Type,e.Usage,e.UsageIndex);}
        declaration->Release();
    }
    if(vertices&&vertexCount&&stride&&stride<=256){
        const UINT count=vertexCount<16?vertexCount:16;std::vector<BYTE> bytes(count*stride);
        if(SafeRead((uintptr_t)vertices,bytes.data(),bytes.size())){
            wchar_t leaf[128]{},path[MAX_PATH]{};
            _snwprintf_s(leaf,_TRUNCATE,L"mevr-flare-%ld-%u.vertices.bin",serial,draw);
            if(PathSibling(g_logPath,leaf,path,MAX_PATH)){
                HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(file!=INVALID_HANDLE_VALUE){DWORD written=0;WriteFile(file,bytes.data(),(DWORD)bytes.size(),&written,nullptr);CloseHandle(file);}
            }
        }
    }
    IDirect3DBaseTexture9* texture=nullptr;
    if(FAILED(dev->GetTexture(0,&texture))||!texture)return;
    Log("[flare-marker] texture0=%p",texture);
    bool seen=false;for(unsigned i=0;i<textures;++i)if(seenTextures[i]==(uintptr_t)texture)seen=true;
    if(!seen&&textures<16&&texture->GetType()==D3DRTYPE_TEXTURE){
        seenTextures[textures++]=(uintptr_t)texture;
        auto* tex=static_cast<IDirect3DTexture9*>(texture);D3DSURFACE_DESC desc{};
        if(SUCCEEDED(tex->GetLevelDesc(0,&desc))&&desc.Width<=1024&&desc.Height<=1024&&
            !(desc.Usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL))){
            // Use the installed system helper; no new DLL beside the game.
            using SaveTexture=HRESULT (WINAPI*)(LPCWSTR,int,IDirect3DBaseTexture9*,const PALETTEENTRY*);
            static SaveTexture save=nullptr;static bool tried=false;
            if(!tried){tried=true;wchar_t library[MAX_PATH]{};
                if(GetSystemDirectoryW(library,MAX_PATH)&&wcscat_s(library,L"\\d3dx9_43.dll")==0){
                    HMODULE module=LoadLibraryW(library);
                    if(module)save=(SaveTexture)GetProcAddress(module,"D3DXSaveTextureToFileW");
                }
            }
            wchar_t leaf[128]{},path[MAX_PATH]{};
            _snwprintf_s(leaf,_TRUNCATE,L"mevr-flare-%ld-%u.texture.png",serial,draw);
            HRESULT hr=E_NOINTERFACE;
            if(save&&PathSibling(g_logPath,leaf,path,MAX_PATH))hr=save(path,3,texture,nullptr); // D3DXIFF_PNG
            Log("[flare-marker] texture capture %ux%u format=%u result=%08lX file=%ls",desc.Width,desc.Height,
                (unsigned)desc.Format,(unsigned long)hr,leaf);
        }
    }
    texture->Release();
}
#include "effect_capture.inl"
#include "sun_trace.inl"

static void CaptureEffectDraw(IDirect3DDevice9* dev,UINT primitives,UINT stride,
                              const void* vertices=nullptr,UINT vertexCount=0,const char* route="UP")
{
    if(g_effectCaptureUntil<g_frames||!g_effectCaptureUntil||g_inDupDraw)return;
    static long serial=-1;static uint64_t seen[96]{};static unsigned seenCount=0;
    if(serial!=g_effectCaptureUntil){serial=g_effectCaptureUntil;seenCount=0;}
    const auto* effect=FindEffectShader(dev);
    if(effect&&(effect->hash==0x1612477Au||effect->hash==0x746AB0D0u||
        (effect->hash==0xE6318E88u&&CurrentEffectPixelHash(dev)==0xA1D87F1Fu)))
        CaptureFlareDetails(dev,effect->hash,CurrentEffectPixelHash(dev),vertices,vertexCount,stride,route);
    if(seenCount>=96)return;
    IDirect3DVertexShader9* vs=nullptr;IDirect3DPixelShader9* ps=nullptr;
    dev->GetVertexShader(&vs);dev->GetPixelShader(&ps);
    DWORD depth=0,color=0,blend=0,src=0,dst=0;
    dev->GetRenderState(D3DRS_ZENABLE,&depth);dev->GetRenderState(D3DRS_COLORWRITEENABLE,&color);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend);dev->GetRenderState(D3DRS_SRCBLEND,&src);
    dev->GetRenderState(D3DRS_DESTBLEND,&dst);
    const uint64_t key=((uint64_t)(uintptr_t)vs<<32)^(uintptr_t)ps^
        ((uint64_t)depth<<1)^((uint64_t)color<<3)^((uint64_t)blend<<8)^((uint64_t)stride<<10);
    bool found=false;for(unsigned i=0;i<seenCount;++i)if(seen[i]==key)found=true;
    if(!found){
        seen[seenCount++]=key;
        const uint32_t vh=SaveEffectShader(vs,L"vs"),ph=SaveEffectShader(ps,L"ps");
        Log("[effect-marker] frame %ld VS=%08X PS=%08X prims=%u stride=%u z=%lu color=%lu blend=%lu/%lu/%lu sceneC0=%d positionUsesVP=%d route=%s",
            g_frames,vh,ph,primitives,stride,depth,color,blend,src,dst,g_c0IsScene?1:0,WorldUPUsesSceneTransform(dev)?1:0,route);
        D3DVIEWPORT9 viewport{};dev->GetViewport(&viewport);
        Log("[effect-marker] target=%ux%u viewport=%lu,%lu %lux%lu",g_rtCurrent?g_rtCurrent->w:0,
            g_rtCurrent?g_rtCurrent->h:0,viewport.X,viewport.Y,viewport.Width,viewport.Height);
        // Constants distinguish preprojected flares from screen/depth reconstruction.
        float constants[32]{};
        if(SUCCEEDED(dev->GetVertexShaderConstantF(0,constants,8)))
            for(int r=0;r<8;++r)Log("[effect-marker] VS c%d %.7g %.7g %.7g %.7g",r,
                constants[r*4],constants[r*4+1],constants[r*4+2],constants[r*4+3]);
    }
    if(vs)vs->Release();if(ps)ps->Release();
}
