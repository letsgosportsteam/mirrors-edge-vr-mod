// Included by the screen-sampling harness; exercises production DrawStereoCanvas.
static void TestTutorialPopup(const char* captureDirectory)
{
    FakeDevice dev;FakeTexture texture;dev.texture=&texture;
    texture.desc.Usage=0;texture.desc.Format=D3DFMT_DXT5;
    g_gameUiScale=.65f;g_gameUiHeight=0;g_canvasPopup={};++g_frames;
    currentEffect.hash=0x5470E8CC;pixelHash=0xB3B12B1D;
    const float projection[16]={1,0,0,0,0,-2,0,0,0,0,.999f,1,-1000.5f,1001,989,1001};
    memcpy(dev.transform,projection,64);
    const D3DVIEWPORT9 original=dev.viewport;const RECT clip=dev.clip;
    auto draw=[&](UINT prims,bool popup){
        int eyes=0;HRESULT result=S_OK;
        assert(DrawStereoCanvas(&dev,[&]{
            const DWORD width=popup?585:650;
            assert(dev.viewport.Width==width&&dev.viewport.Height==width);
            assert(dev.viewport.X==(1000-width)/2+eyes*1000);
            assert(dev.viewport.Y==(1000-width)/2-(popup?120:0));
            assert(dev.clip.top==(LONG)dev.viewport.Y+(LONG)floor(50.*width/1000));
            ++eyes;return E_FAIL;
        },&result,prims)&&eyes==2&&result==E_FAIL);
        assert(!memcmp(&original,&dev.viewport,sizeof(original))&&!memcmp(&clip,&dev.clip,sizeof(clip)));
    };
    auto background=[&]{texture.desc.Width=1024;texture.desc.Height=128;dev.transform[15]=1001;};
    auto text=[&]{texture.desc.Width=256;texture.desc.Height=128;dev.transform[15]=1000;};
    background();draw(18,true);text();draw(142,true);texture.desc.Height=256;draw(6,true);
    background();draw(18,true);
    const float reticle[16]={.001f,0,0,0,0,-.002f,0,0,0,0,1,0,-1,1,0,1};
    memcpy(dev.transform,reticle,64);texture.desc.Width=texture.desc.Height=16;
    draw(2,false);assert(g_canvasPopup.active); // intervening HUD is not part of the popup
    memcpy(dev.transform,projection,64);text();draw(106,true);texture.desc.Height=256;draw(6,true);
    ++g_frames;draw(6,false); // no text carryover into another frame
    background();draw(18,true);text();currentEffect.hash=0x12345678;HRESULT result=S_OK;
    assert(!DrawStereoCanvas(&dev,[]{return S_OK;},&result));currentEffect.hash=0x5470E8CC;draw(6,false);
    background();draw(16,false);text();draw(6,false); // unrelated atlas use cannot start a run
    background();texture.desc.Format=D3DFMT_DXT1;draw(18,false);texture.desc.Format=D3DFMT_DXT5;
    background();draw(18,true);
    texture.desc.Width=128;texture.desc.Height=32;dev.transform[15]=1000;draw(18,false); // Back-menu panel
    text();draw(34,false); // its shared font is unchanged, even in the same frame
    puts("PASS: tutorial background/icons/text move together; frame/draw/asset boundaries preserve other UI; clipping and state restored after failures");
    if(!captureDirectory||!*captureDirectory)return;

    struct Record {
        uint32_t draw,vs,ps,primitives,route,width,height,format;
        uint32_t depth,writeDepth,blend,src,dst,op,color,scene,duplicate,suppressed;
        int32_t beforeResult,afterResult,vsResult,psResult;
        D3DVIEWPORT9 viewport;
        float vertexConstants[512],pixelConstants[256];
        uint32_t textures[8][5];
    };
    static_assert(sizeof(Record)==3344,"capture layout");
    g_capW=4224;g_capH=2376;
    for(int cap:{20954,23176,4591}) {
        if(cap==4591&&!std::ifstream(std::string(captureDirectory)+"/mevr-suntrace-6560-4591-0.bin").good())continue;
        ++g_frames;g_canvasPopup={};unsigned changed=0,draws=0;
        for(int page=0;page<(cap==20954?35:cap==4591?30:32);++page) {
            const std::string name=std::string(captureDirectory)+"/mevr-suntrace-"+(cap==4591?"6560-":"33480-")+std::to_string(cap)+"-"+std::to_string(page)+".bin";
            std::ifstream file(name,std::ios::binary);assert(file.good());
            uint32_t header[4];file.read((char*)header,sizeof(header));
            assert(header[0]==0x53554E31&&header[1]==1&&header[2]==sizeof(Record));
            for(unsigned i=0;i<header[3];++i) {
                Record r;file.read((char*)&r,sizeof(r));assert(file.good());++draws;
                currentEffect.hash=r.vs;pixelHash=r.ps;dev.depth=r.depth;dev.viewport=r.viewport;
                dev.surface.desc.Width=r.width;dev.surface.desc.Height=r.height;
                texture.desc.Width=r.textures[0][2];texture.desc.Height=r.textures[0][3];
                texture.desc.Format=(D3DFORMAT)r.textures[0][4];
                texture.desc.Usage=texture.desc.Width==g_capW&&texture.desc.Height==g_capH?D3DUSAGE_RENDERTARGET:0;
                dev.texture=r.textures[0][0]?&texture:nullptr;
                memcpy(dev.transform,r.vertexConstants+(r.vs==0xDF014469?0:20),64);
                const bool expected=(cap==20954&&r.draw>=1770&&r.draw<=1774)||
                    (cap==4591&&(r.draw==1562||(r.draw>=1564&&r.draw<=1567)));
                int eyes=0;
                const bool handled=DrawStereoCanvas(&dev,[&]{
                    ++eyes;
                    const DWORD w=(DWORD)floorf(r.viewport.Width*.5f*(.65f*(expected?.9f:1.f)));
                    assert(dev.viewport.Width==w);return S_OK;
                },&result,r.primitives);
                assert(!expected||handled);
                if(expected){assert(eyes==2);++changed;}
            }
        }
        assert(changed==(cap==23176?0:5));
        printf("PASS: marker %d replayed %u draws, %u tutorial batches adjusted; Back/Select menu unchanged\n",cap,draws,changed);
    }
}
