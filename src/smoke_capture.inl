// Explicit Backspace diagnostics for the captured rooftop soft-particle pair.
// Read state and input bytes only: never change shaders, render state or targets.
static bool SmokeCaptureSizes(D3DPRIMITIVETYPE type, UINT minimum, UINT vertices,
                              UINT primitives, D3DFORMAT format, UINT stride,
                              size_t* vertexBytes, size_t* indexBytes)
{
    if (type!=D3DPT_TRIANGLELIST || !vertices || !primitives || !stride || stride>256 ||
        (format!=D3DFMT_INDEX16 && format!=D3DFMT_INDEX32)) return false;
    const uint64_t v=((uint64_t)minimum+vertices)*stride;
    const uint64_t i=(uint64_t)primitives*3*(format==D3DFMT_INDEX16?2:4);
    if (v>1024*1024 || i>1024*1024) return false;
    *vertexBytes=(size_t)v;*indexBytes=(size_t)i;return true;
}

static bool SaveSmokeBytes(const wchar_t* prefix,const wchar_t* suffix,const void* bytes,size_t size)
{
    wchar_t leaf[160]{},path[MAX_PATH]{};
    _snwprintf_s(leaf,_TRUNCATE,L"%s.%s",prefix,suffix);
    if(!PathSibling(g_logPath,leaf,path,MAX_PATH))return false;
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;
    const bool ok=WriteFile(file,bytes,(DWORD)size,&written,nullptr)&&written==size;
    CloseHandle(file);return ok;
}

static void CaptureSmokeIndexedUP(IDirect3DDevice9* dev,D3DPRIMITIVETYPE type,
    UINT minimum,UINT vertices,UINT primitives,const void* indices,D3DFORMAT format,
    const void* data,UINT stride)
{
    if(!g_effectCaptureUntil || g_frames!=g_effectCaptureUntil-2 || g_inDupDraw)return;
    const auto* effect=FindEffectShader(dev);
    if(!effect || effect->hash!=0x23C40ECFu || CurrentEffectPixelHash(dev)!=0xFABE3964u)return;
    static long serial=-1;static unsigned draws=0;
    if(serial!=g_effectCaptureUntil){serial=g_effectCaptureUntil;draws=0;}
    if(draws>=2)return; // first distant plume and the second rooftop plume
    wchar_t prefix[120]{};
    _snwprintf_s(prefix,_TRUNCATE,L"mevr-smoke-%lu-%ld-%u",GetCurrentProcessId(),serial,draws++);
    // Fixed x86 structure, schema 1. HRESULTs make failed reads distinguishable
    // from legitimate zero state. Preserve original and both injected matrices.
    struct State {
        uint32_t magic=0x534D4B31,version=1,bytes=sizeof(State);
        uint32_t type,minimum,vertices,primitives,format,stride;
        HRESULT viewportResult,scissorResult,declResult,vsResult,psResult;
        D3DVIEWPORT9 viewport{};RECT scissor{};
        UINT declarationCount=MAXD3DDECLLENGTH+1;
        D3DVERTEXELEMENT9 declaration[MAXD3DDECLLENGTH+1]{};
        float scene[16]{},eyes[2][16]{},vs[256*4]{},ps[224*4]{};
        struct Value { DWORD key=0,value=0;HRESULT result=E_FAIL; } render[40],sampler[2][13];
        UINT renderCount=0;
    } state{};
    state.type=type;state.minimum=minimum;state.vertices=vertices;state.primitives=primitives;
    state.format=format;state.stride=stride;
    state.viewportResult=dev->GetViewport(&state.viewport);state.scissorResult=dev->GetScissorRect(&state.scissor);
    state.vsResult=dev->GetVertexShaderConstantF(0,state.vs,256);
    state.psResult=dev->GetPixelShaderConstantF(0,state.ps,224);
    memcpy(state.scene,g_sceneMat,sizeof(state.scene));
    BuildEyeMatrix(state.eyes[0],0);BuildEyeMatrix(state.eyes[1],1);
    IDirect3DVertexDeclaration9* declaration=nullptr;
    state.declResult=dev->GetVertexDeclaration(&declaration);
    if(SUCCEEDED(state.declResult)&&declaration){
        state.declResult=declaration->GetDeclaration(state.declaration,&state.declarationCount);declaration->Release();
    }else {state.declarationCount=0;if(SUCCEEDED(state.declResult))state.declResult=E_FAIL;}
    const D3DRENDERSTATETYPE states[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,
        D3DRS_ALPHATESTENABLE,D3DRS_ALPHAFUNC,D3DRS_ALPHAREF,D3DRS_ALPHABLENDENABLE,
        D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_SEPARATEALPHABLENDENABLE,
        D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,D3DRS_BLENDOPALPHA,D3DRS_BLENDFACTOR,
        D3DRS_COLORWRITEENABLE,D3DRS_STENCILENABLE,D3DRS_STENCILFUNC,D3DRS_STENCILREF,
        D3DRS_STENCILMASK,D3DRS_STENCILWRITEMASK,D3DRS_STENCILFAIL,D3DRS_STENCILZFAIL,
        D3DRS_STENCILPASS,D3DRS_TWOSIDEDSTENCILMODE,D3DRS_CCW_STENCILFUNC,
        D3DRS_CCW_STENCILFAIL,D3DRS_CCW_STENCILZFAIL,D3DRS_CCW_STENCILPASS,
        D3DRS_CULLMODE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_CLIPPLANEENABLE,
        D3DRS_CLIPPING,D3DRS_MULTISAMPLEMASK,D3DRS_DEPTHBIAS,D3DRS_SLOPESCALEDEPTHBIAS};
    static_assert(sizeof(states)/sizeof(states[0])<=40,"smoke state capacity");
    for(auto key:states){auto& v=state.render[state.renderCount++];v.key=key;v.result=dev->GetRenderState(key,&v.value);}
    for(DWORD slot=0;slot<2;++slot)for(DWORD key=1;key<=13;++key){
        auto& v=state.sampler[slot][key-1];v.key=key;v.result=dev->GetSamplerState(slot,(D3DSAMPLERSTATETYPE)key,&v.value);
    }
    size_t vb=0,ib=0;bool savedVertices=false,savedIndices=false;
    if(SmokeCaptureSizes(type,minimum,vertices,primitives,format,stride,&vb,&ib)){
        std::vector<BYTE> v(vb),i(ib);
        if(SafeRead((uintptr_t)data,v.data(),v.size()))savedVertices=SaveSmokeBytes(prefix,L"vertices.bin",v.data(),v.size());
        if(SafeRead((uintptr_t)indices,i.data(),i.size()))savedIndices=SaveSmokeBytes(prefix,L"indices.bin",i.data(),i.size());
    }
    Log("[smoke-capture] %ls state=%d vertices=%d/%u indices=%d/%u prims=%u stride=%u",
        prefix,SaveSmokeBytes(prefix,L"state.bin",&state,sizeof(state))?1:0,
        savedVertices?1:0,(unsigned)vb,savedIndices?1:0,(unsigned)ib,primitives,stride);
    // Full-resolution DDS: downsampling a depth map can change the fade test.
    // Only s0 (scene depth in alpha) and s1 (opacity texture) are used by this PS.
    using SaveTexture=HRESULT(WINAPI*)(LPCWSTR,int,IDirect3DBaseTexture9*,const PALETTEENTRY*);
    static SaveTexture save=nullptr;static bool tried=false;
    if(!tried){tried=true;wchar_t library[MAX_PATH]{};
        if(GetSystemDirectoryW(library,MAX_PATH)&&wcscat_s(library,L"\\d3dx9_43.dll")==0){
            HMODULE module=LoadLibraryW(library);if(module)save=(SaveTexture)GetProcAddress(module,"D3DXSaveTextureToFileW");
        }
    }
    for(DWORD slot=0;slot<2;++slot){
        IDirect3DBaseTexture9* texture=nullptr;HRESULT hr=dev->GetTexture(slot,&texture);
        if(SUCCEEDED(hr)&&texture){
            hr=E_FAIL;D3DSURFACE_DESC desc{};
            if(texture->GetType()==D3DRTYPE_TEXTURE&&
                SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0,&desc))&&
                (uint64_t)desc.Width*desc.Height<=16*1024*1024){
                wchar_t leaf[160]{},path[MAX_PATH]{};
                _snwprintf_s(leaf,_TRUNCATE,L"%s.sampler%lu.dds",prefix,slot);
                if(save&&PathSibling(g_logPath,leaf,path,MAX_PATH))hr=save(path,4,texture,nullptr);
            }
            texture->Release();
        }else if(SUCCEEDED(hr))hr=E_FAIL;
        Log("[smoke-capture] %ls sampler%lu result=%08lX",prefix,slot,(unsigned long)hr);
    }
}
