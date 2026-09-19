// Explicit Backspace capture only. Keep paired images around the complete draw
// (including both eye draws) so a misplaced layer can be attributed to a shader.
static HRESULT SaveEffectSurface(IDirect3DDevice9* dev, IDirect3DSurface9* surface,
                                const wchar_t* leaf, bool raw=false)
{
    using SaveSurface = HRESULT (WINAPI*)(LPCWSTR,int,IDirect3DSurface9*,const PALETTEENTRY*,const RECT*);
    static SaveSurface save = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        wchar_t library[MAX_PATH]{};
        if (GetSystemDirectoryW(library,MAX_PATH) && wcscat_s(library,L"\\d3dx9_43.dll")==0) {
            HMODULE module = LoadLibraryW(library);
            if (module) save = (SaveSurface)GetProcAddress(module,"D3DXSaveSurfaceToFileW");
        }
    }
    wchar_t path[MAX_PATH]{};
    if (!save || !PathSibling(g_logPath,leaf,path,MAX_PATH)) return E_NOINTERFACE;
    D3DSURFACE_DESC desc{};
    HRESULT hr = surface->GetDesc(&desc);
    if (FAILED(hr) || !desc.Width || !desc.Height) return E_FAIL;
    // Bound disk size and compression/readback work; preserve HDR/depth colour
    // formats while resizing. Unsupported formats are reported, not guessed.
    const UINT width = (std::min)(desc.Width,1024u);
    const UINT height = (std::max)(1u,(UINT)((uint64_t)desc.Height*width/desc.Width));
    IDirect3DSurface9* reducedSurface = nullptr;
    hr = dev->CreateRenderTarget(width,height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&reducedSurface,nullptr);
    if (SUCCEEDED(hr)) {
        hr = dev->StretchRect(surface,nullptr,reducedSurface,nullptr,D3DTEXF_POINT);
        if (SUCCEEDED(hr)) hr = save(path,raw?4:3,reducedSurface,nullptr,nullptr);
        reducedSurface->Release();
    }
    return hr;
}

struct EffectPassCapture {
    static void TraceStencil(IDirect3DDevice9* device,const char* route,UINT primitives)
    {
        static long serial=-1;static unsigned count=0;
        if(serial!=g_effectCaptureUntil){serial=g_effectCaptureUntil;count=0;}
        if(count>=48)return;
        DWORD enabled=0;if(FAILED(device->GetRenderState(D3DRS_STENCILENABLE,&enabled))||!enabled)return;
        ++count;
        const D3DRENDERSTATETYPE states[]={D3DRS_COLORWRITEENABLE,D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,
            D3DRS_STENCILFUNC,D3DRS_STENCILREF,D3DRS_STENCILMASK,D3DRS_STENCILWRITEMASK,
            D3DRS_STENCILFAIL,D3DRS_STENCILZFAIL,D3DRS_STENCILPASS,D3DRS_TWOSIDEDSTENCILMODE,
            D3DRS_CCW_STENCILFAIL,D3DRS_CCW_STENCILZFAIL,D3DRS_CCW_STENCILPASS,
            D3DRS_SCISSORTESTENABLE,D3DRS_CULLMODE};
        DWORD s[16]{};for(unsigned i=0;i<16;++i)device->GetRenderState(states[i],s+i);
        const auto* shader=FindEffectShader(device);D3DVIEWPORT9 vp{};device->GetViewport(&vp);
        RECT clip{};if(s[14])device->GetScissorRect(&clip);
        Log("[stencil-marker] capture=%ld index=%u route=%s prims=%u VS=%08X PS=%08X"
            " color=%lu z=%lu writeZ=%lu func=%lu ref=%lu readMask=%08lX writeMask=%08lX"
            " ops=%lu,%lu,%lu twoSided=%lu ccw=%lu,%lu,%lu scissor=%lu cull=%lu"
            " viewport=%lu,%lu %lux%lu sceneC0=%d usesVP=%d sceneTarget=%d",
            serial,count,route,primitives,shader?shader->hash:0,CurrentEffectPixelHash(device),
            s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7],s[8],s[9],s[10],s[11],s[12],s[13],s[14],s[15],
            vp.X,vp.Y,vp.Width,vp.Height,g_c0IsScene?1:0,shader&&shader->scene?1:0,ShouldDuplicate()?1:0);
        if(s[14])Log("[stencil-marker] capture=%ld index=%u monoScissor=%ld,%ld..%ld,%ld",
            serial,count,clip.left,clip.top,clip.right,clip.bottom);
    }
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9* target = nullptr;
    wchar_t prefix[128]{};
    unsigned draw = 0;
    double started = 0;
    EffectPassCapture(IDirect3DDevice9* device,const char* route,UINT primitives)
    {
        // Start on the NEXT complete frame; never keep taking images throughout
        // play or at an automatic parkour event. No device calls outside a marker.
        if (!g_effectCaptureUntil || g_frames != g_effectCaptureUntil-2 || g_inDupDraw) return;
        TraceStencil(device,route,primitives);
        static long serial = -1;
        static unsigned knownCount=0,otherCount=0,seenCount=0,samplerImages=0;
        static uint64_t keys[32]{};
        static unsigned occurrences[32]{};
        if (serial != g_effectCaptureUntil) {
            serial=g_effectCaptureUntil;knownCount=otherCount=seenCount=samplerImages=0;
            memset(occurrences,0,sizeof(occurrences));
        }
        if (knownCount>=24 && otherCount>=8) return;
        DWORD color=0,depth=0,blend=0;
        if (FAILED(device->GetRenderState(D3DRS_COLORWRITEENABLE,&color)) || !color) return;
        device->GetRenderState(D3DRS_ZENABLE,&depth);
        device->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend);
        const auto* effect=FindEffectShader(device);
        if (!effect) return;
        const uint32_t vs=effect->hash,ps=CurrentEffectPixelHash(device);
        const bool known=vs==0x5A947094u || vs==0x51B6BA4Au || vs==0xD74970B5u ||
            vs==0x8C7E0467u || vs==0x1612477Au || vs==0x5470E8CCu ||
            vs==0x003BD5B4u || vs==0x2E5B7775u || vs==0xD34339EDu ||
            (vs==0xE6318E88u&&ps==0xA1D87F1Fu);
        // Ordinary blended meshes previously used every unknown slot before the
        // menu and final image passes. Reserve those slots for screen-space work.
        if (known ? knownCount>=24 : (otherCount>=8 || depth || effect->scene)) return;
        IDirect3DSurface9* surface=nullptr;D3DSURFACE_DESC desc{};
        if (FAILED(device->GetRenderTarget(0,&surface)) || !surface) return;
        if (FAILED(surface->GetDesc(&desc)) || desc.Width<512 || desc.Height<256) {
            surface->Release();return;
        }
        const uint64_t key=((uint64_t)vs<<32)|ps;
        unsigned index=0;
        for (;index<seenCount && keys[index]!=key;++index) {}
        if (index==seenCount) {
            if (seenCount>=32) {surface->Release();return;}
            keys[seenCount++]=key;
        }
        // More than one light can use the same shadow shader. Keep the first
        // four shadow draws and two instances of other effects, not just a hash.
        if (occurrences[index]>=(vs==0x5A947094u?4u:2u)) {surface->Release();return;}
        ++occurrences[index];
        draw=knownCount+otherCount;
        if (known) ++knownCount; else ++otherCount;
        dev=device;target=surface;started=NowMs();
        _snwprintf_s(prefix,_TRUNCATE,L"mevr-pass-%lu-%ld-%u-%08X-%08X",
            GetCurrentProcessId(),serial,draw,vs,ps);
        D3DVIEWPORT9 viewport{};dev->GetViewport(&viewport);
        Log("[pass-marker] capture=%ld draw=%u frame=%ld route=%s VS=%08X PS=%08X prims=%u"
            " target=%p %ux%u fmt=%u viewport=%lu,%lu %lux%lu depth=%lu blend=%lu"
            " sceneC0=%d usesVP=%d duplicate=%d prefix=%ls",serial,draw,g_frames,route,vs,ps,primitives,
            target,desc.Width,desc.Height,(unsigned)desc.Format,viewport.X,viewport.Y,viewport.Width,viewport.Height,
            depth,blend,g_c0IsScene?1:0,effect->scene?1:0,ShouldDuplicate()?1:0,prefix);
        SaveEffectShader(effect->shader,L"vs");
        IDirect3DPixelShader9* pixel=nullptr;
        if (SUCCEEDED(dev->GetPixelShader(&pixel)) && pixel) {SaveEffectShader(pixel,L"ps");pixel->Release();}
        float constants[128]{};
        for (int kind=0;kind<2;++kind) {
            HRESULT hr=kind?dev->GetPixelShaderConstantF(0,constants,32):dev->GetVertexShaderConstantF(0,constants,32);
            if (SUCCEEDED(hr)) for (int r=0;r<32;++r)
                Log("[pass-marker] draw=%u %s c%d %.9g %.9g %.9g %.9g",draw,kind?"PS":"VS",r,
                    constants[r*4],constants[r*4+1],constants[r*4+2],constants[r*4+3]);
        }
        for (int eye=-1;eye<2;++eye) {
            float matrix[16];
            if (eye<0) memcpy(matrix,g_sceneMat,sizeof(matrix));else BuildEyeMatrix(matrix,eye);
            for (int r=0;r<4;++r) Log("[pass-marker] draw=%u eye=%d VP%d %.9g %.9g %.9g %.9g",draw,eye,r,
                matrix[r*4],matrix[r*4+1],matrix[r*4+2],matrix[r*4+3]);
        }
        const D3DRENDERSTATETYPE states[]={D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,D3DRS_SRCBLEND,
            D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_STENCILENABLE,D3DRS_STENCILFUNC,D3DRS_STENCILREF,
            D3DRS_CULLMODE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE};
        for (auto state:states) {DWORD value=0;dev->GetRenderState(state,&value);
            Log("[pass-marker] draw=%u state=%u value=%lu",draw,(unsigned)state,value);}
        for (DWORD slot=0;slot<8;++slot) {
            IDirect3DBaseTexture9* texture=nullptr;
            if (FAILED(dev->GetTexture(slot,&texture)) || !texture) continue;
            if (texture->GetType()==D3DRTYPE_TEXTURE) {
                auto* tex=static_cast<IDirect3DTexture9*>(texture);D3DSURFACE_DESC td{};
                if (SUCCEEDED(tex->GetLevelDesc(0,&td))) {
                    Log("[pass-marker] draw=%u sampler=%lu texture=%p %ux%u fmt=%u usage=%lu",
                        draw,slot,tex,td.Width,td.Height,(unsigned)td.Format,td.Usage);
                    if (known && occurrences[index]==1 && samplerImages<12 && (td.Usage&D3DUSAGE_RENDERTARGET)) {
                        IDirect3DSurface9* sampled=nullptr;
                        if (SUCCEEDED(tex->GetSurfaceLevel(0,&sampled)) && sampled) {
                            ++samplerImages;wchar_t leaf[160]{};
                            _snwprintf_s(leaf,_TRUNCATE,L"%s.sampler%lu.png",prefix,slot);
                            const HRESULT hr=SaveEffectSurface(dev,sampled,leaf);
                            Log("[pass-marker] draw=%u sampler=%lu image=%08lX",draw,slot,(unsigned long)hr);
                            // Scene depth lives in the colour texture's floating-point
                            // alpha. PNG clamps it; DDS retains the numerical samples.
                            _snwprintf_s(leaf,_TRUNCATE,L"%s.sampler%lu.dds",prefix,slot);
                            const HRESULT raw=SaveEffectSurface(dev,sampled,leaf,true);
                            Log("[pass-marker] draw=%u sampler=%lu raw=%08lX",draw,slot,(unsigned long)raw);
                            sampled->Release();
                        }
                    }
                }
            }
            texture->Release();
        }
        Save(L"before");
    }
    void Save(const wchar_t* stage) {
        wchar_t leaf[160]{};_snwprintf_s(leaf,_TRUNCATE,L"%s.%s.png",prefix,stage);
        const HRESULT hr=SaveEffectSurface(dev,target,leaf);
        Log("[pass-marker] draw=%u stage=%ls image=%08lX file=%ls",draw,stage,(unsigned long)hr,leaf);
        D3DSURFACE_DESC desc{};
        if(SUCCEEDED(target->GetDesc(&desc))&&desc.Format==D3DFMT_A16B16G16R16F){
            _snwprintf_s(leaf,_TRUNCATE,L"%s.%s.dds",prefix,stage);
            const HRESULT raw=SaveEffectSurface(dev,target,leaf,true);
            Log("[pass-marker] draw=%u stage=%ls raw=%08lX",draw,stage,(unsigned long)raw);
        }
    }
    ~EffectPassCapture() {
        if (!target) return;
        Save(L"after");target->Release();
        Log("[pass-marker] draw=%u capture cost %.2f ms (Backspace only)",draw,NowMs()-started);
    }
    EffectPassCapture(const EffectPassCapture&)=delete;
    EffectPassCapture& operator=(const EffectPassCapture&)=delete;
};
