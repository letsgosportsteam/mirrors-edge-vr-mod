// One complete Backspace frame, including shaders the selective pass capture
// does not recognize. GPU thumbnails are packed into pages instead of doing a
// readback/compression for every draw. No render states or game targets are set.
struct SunTracePage {
    enum : unsigned { TileWidth=256,TileHeight=128,Columns=8,Pairs=64,Limit=2048,LateLimit=256 };
    struct Record {
        uint32_t draw,vs,ps,primitives,route,width,height,format;
        uint32_t depth,writeDepth,blend,src,dst,op,color,scene,duplicate,suppressed;
        int32_t beforeResult,afterResult,vsResult,psResult;
        D3DVIEWPORT9 viewport;
        float vertexConstants[128*4],pixelConstants[64*4];
        uint32_t textures[8][5]; // pointer, type, width, height, format (2D only)
    };
    IDirect3DDevice9* dev=nullptr;
    IDirect3DSurface9* atlas=nullptr;
    long serial=-1;
    unsigned count=0,total=0,page=0;
    bool capped=false,finished=false;
    D3DFORMAT format=D3DFMT_UNKNOWN;
    Record records[Pairs]{};

    void Flush() {
        if(atlas&&count){
            wchar_t leaf[128]{},path[MAX_PATH]{};
            _snwprintf_s(leaf,_TRUNCATE,L"mevr-suntrace-%lu-%ld-%u.png",GetCurrentProcessId(),serial,page);
            const HRESULT hr=SaveEffectSurface(dev,atlas,leaf);
            // SaveEffectSurface reduces width to 1024: each output tile is
            // 128x64, consecutive even/odd tiles are before/after one draw.
            Log("[sun-trace] capture=%ld page=%u pairs=%u first=%u last=%u image=%08lX file=%ls",
                serial,page,count,records[0].draw,records[count-1].draw,(unsigned long)hr,leaf);
            _snwprintf_s(leaf,_TRUNCATE,L"mevr-suntrace-%lu-%ld-%u.bin",GetCurrentProcessId(),serial,page);
            if(PathSibling(g_logPath,leaf,path,MAX_PATH)){
                HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(file!=INVALID_HANDLE_VALUE){
                    const uint32_t header[]={0x53554E31,1,sizeof(Record),count};DWORD written=0;
                    const bool ok=WriteFile(file,header,sizeof(header),&written,nullptr)&&written==sizeof(header)&&
                        WriteFile(file,records,count*sizeof(Record),&written,nullptr)&&written==count*sizeof(Record);
                    Log("[sun-trace] capture=%ld page=%u metadata=%s recordBytes=%u",serial,page,ok?"ok":"failed",(unsigned)sizeof(Record));
                    CloseHandle(file);
                }else Log("[sun-trace] metadata open failed error=%lu",GetLastError());
            }
            ++page;
        }
        if(atlas){atlas->Release();atlas=nullptr;}
        count=0;dev=nullptr;format=D3DFMT_UNKNOWN;
    }
    void Reset() { Flush();serial=-1;total=page=0;capped=finished=false; }
    void Discard() {
        if(atlas){atlas->Release();atlas=nullptr;}
        count=total=page=0;dev=nullptr;format=D3DFMT_UNKNOWN;serial=-1;capped=finished=false;
    }
    void Finish() {
        if(serial<0||finished)return;
        Flush();finished=true;
        Log("[sun-trace] capture=%ld finished draws=%u pages=%u capped=%d",serial,total,page,capped?1:0);
    }
    HRESULT Tile(IDirect3DSurface9* source,unsigned index) {
        RECT rect={(LONG)(index%Columns*TileWidth),(LONG)(index/Columns*TileHeight),
            (LONG)((index%Columns+1)*TileWidth),(LONG)((index/Columns+1)*TileHeight)};
        return dev->StretchRect(source,nullptr,atlas,&rect,D3DTEXF_POINT);
    }
};
static SunTracePage g_sunTrace;

struct SunDrawCapture {
    IDirect3DSurface9* target=nullptr;
    unsigned slot=0;
    SunDrawCapture(IDirect3DDevice9* device,unsigned route,UINT primitives) {
        if(!g_effectCaptureUntil||g_frames!=g_effectCaptureUntil-2||g_inDupDraw)return;
        auto& trace=g_sunTrace;
        if(trace.serial!=g_effectCaptureUntil){trace.Reset();trace.serial=g_effectCaptureUntil;}
        if(trace.finished)return;
        DWORD color=0;if(FAILED(device->GetRenderState(D3DRS_COLORWRITEENABLE,&color))||!color)return;
        if(trace.total>=SunTracePage::Limit){
            if(!trace.capped){Log("[sun-trace] capture=%ld TRUNCATED after %u draws",trace.serial,trace.total);trace.capped=true;}
            // Reserve independent room for late menu/composite draws. Thousands
            // of repeated world decals must not hide the pause menu again.
            DWORD depth=1;device->GetRenderState(D3DRS_ZENABLE,&depth);
            const auto* shader=FindEffectShader(device);
            if(trace.total>=SunTracePage::Limit+SunTracePage::LateLimit||depth||
                (shader&&shader->scene))return;
        }
        D3DSURFACE_DESC desc{};
        if(FAILED(device->GetRenderTarget(0,&target))||!target)return;
        if(FAILED(target->GetDesc(&desc))||desc.Width<512||desc.Height<256){target->Release();target=nullptr;return;}
        if(trace.atlas&&(trace.format!=desc.Format||trace.dev!=device||trace.count==SunTracePage::Pairs))trace.Flush();
        if(!trace.atlas){
            trace.dev=device;trace.format=desc.Format;
            const HRESULT hr=device->CreateRenderTarget(SunTracePage::Columns*SunTracePage::TileWidth,
                SunTracePage::Pairs*2/SunTracePage::Columns*SunTracePage::TileHeight,desc.Format,
                D3DMULTISAMPLE_NONE,0,FALSE,&trace.atlas,nullptr);
            if(FAILED(hr)){
                Log("[sun-trace] atlas allocation failed %08lX; stopping this marker",(unsigned long)hr);
                trace.total=SunTracePage::Limit+SunTracePage::LateLimit;target->Release();target=nullptr;return;
            }
            device->ColorFill(trace.atlas,nullptr,0);
        }
        slot=trace.count++;auto& r=trace.records[slot];memset(&r,0,sizeof(r));
        r.draw=trace.total++;r.route=route;r.primitives=primitives;r.width=desc.Width;r.height=desc.Height;r.format=desc.Format;
        const auto* effect=FindEffectShader(device);r.vs=effect?effect->hash:0;r.ps=CurrentEffectPixelHash(device);
        r.scene=effect&&effect->scene;r.duplicate=ShouldDuplicate();r.suppressed=SuppressLensFlare(device);
        const D3DRENDERSTATETYPE states[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,
            D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP};
        uint32_t* values[]={&r.depth,&r.writeDepth,&r.blend,&r.src,&r.dst,&r.op};
        for(unsigned i=0;i<6;++i){DWORD value=0;device->GetRenderState(states[i],&value);*values[i]=value;}
        r.color=color;device->GetViewport(&r.viewport);
        r.vsResult=device->GetVertexShaderConstantF(0,r.vertexConstants,128);
        r.psResult=device->GetPixelShaderConstantF(0,r.pixelConstants,64);
        for(unsigned i=0;i<8;++i){IDirect3DBaseTexture9* texture=nullptr;
            if(SUCCEEDED(device->GetTexture(i,&texture))&&texture){
                r.textures[i][0]=(uint32_t)(uintptr_t)texture;r.textures[i][1]=texture->GetType();
                if(texture->GetType()==D3DRTYPE_TEXTURE){D3DSURFACE_DESC td{};
                    if(SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0,&td))){
                        r.textures[i][2]=td.Width;r.textures[i][3]=td.Height;r.textures[i][4]=td.Format;}}
                texture->Release();
            }
        }
        // Save each shader pair once per marker, independently of selective
        // capture quotas. Constants/state above are retained for every draw.
        static long shaderSerial=-1;static uint64_t keys[512]{};static unsigned keysCount=0;
        if(shaderSerial!=trace.serial){shaderSerial=trace.serial;keysCount=0;}
        const uint64_t key=((uint64_t)r.vs<<32)|r.ps;bool seen=false;
        for(unsigned i=0;i<keysCount;++i)if(keys[i]==key){seen=true;break;}
        if(!seen&&keysCount<512){keys[keysCount++]=key;
            if(effect)SaveEffectShader(effect->shader,L"vs");
            IDirect3DPixelShader9* ps=nullptr;
            if(SUCCEEDED(device->GetPixelShader(&ps))&&ps){SaveEffectShader(ps,L"ps");ps->Release();}
        }
        r.beforeResult=trace.Tile(target,slot*2);
        Log("[sun-trace] capture=%ld draw=%u page=%u pair=%u VS=%08X PS=%08X route=%u prims=%u"
            " target=%p %ux%u fmt=%u z=%u writeZ=%u blend=%u/%u/%u suppressed=%u",
            trace.serial,r.draw,trace.page,slot,r.vs,r.ps,route,primitives,target,r.width,r.height,r.format,
            r.depth,r.writeDepth,r.blend,r.src,r.dst,r.suppressed);
    }
    ~SunDrawCapture(){
        if(!target)return;
        auto& trace=g_sunTrace;
        trace.records[slot].afterResult=trace.Tile(target,slot*2+1);
        target->Release();
        if(trace.count==SunTracePage::Pairs)trace.Flush();
    }
    SunDrawCapture(const SunDrawCapture&)=delete;
    SunDrawCapture& operator=(const SunDrawCapture&)=delete;
};
