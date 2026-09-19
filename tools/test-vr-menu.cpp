// Exercise the actual implementation, including Win32 atomic saves and reloads.
// No game, OpenXR session, hooks, or DllMain startup is run in this executable.
#include "../src/d3d9.cpp"
#include <cassert>
#include <fstream>

static std::string ReadTestFile(const wchar_t* path) {
    std::ifstream stream(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),{});
}
static void WriteEffectiveSettings(const wchar_t* input) {
    // Inspect a copy in the test directory, never the live INI or headset cache.
    assert(!g_headsetCachePath[0]);
    InitializeCriticalSection(&g_lock);g_lockReady=true;
    FILE* log=nullptr;_wfopen_s(&log,g_logPath,L"wb");assert(log);fclose(log);
    assert(CopyFileW(input,g_settingsPath,FALSE));
    LoadSettings();VrMenuInitializeSettings();
    assert(MenuAtomicWrite(""));assert(SaveMenuSettings());
    std::vector<mevr::IniEdit> extra;
    auto boolean=[&](const char* k,bool b){extra.push_back({k,b?"on":"off"});};
    auto number=[&](const char* k,float v){char s[48];sprintf_s(s,"%.3g",(double)v);extra.push_back({k,s});};
    boolean("NativeLensFlareSuppression",g_nativeLensFlareSuppression);boolean("StereoUI",g_stereoUI);
    boolean("PickupDebug",g_pickupDebug);boolean("ParkourLedgeSnap",g_pkSnapOn);
    boolean("ParkourCamLeadUp",g_pkCamLeadUpOn);boolean("ParkourCamLeadFwd",g_pkCamLeadFwdOn);
    boolean("ParkourAskTheGame",g_pkAsk);boolean("ParkourLetGoDrops",g_pkLetGoDrops);
    number("ParkourLetGoFrames",(float)g_pkLetGoFrames);boolean("ParkourSwipeJump",g_pkSwipeJump);
    number("ParkourSwipeSpeed",g_pkSwipeSpeed);boolean("ParkourSwingPump",g_pkSwingPump);
    number("ParkourSwingPumpFull",g_pkPumpFull);number("ParkourSwingPumpSign",g_pkPumpSign);
    number("ParkourBarWrap",g_pkBarWrap);boolean("ParkourBarShimmy",g_pkBarShimmyOn);
    number("ParkourBarShimmyFull",g_pkBarShimmyFull);number("ParkourSwingExitBoost",g_pkExitBoost);
    boolean("ParkourPullUp",g_pkPullUp);number("ParkourPullUpDrop",g_pkPullUpDrop);
    boolean("ParkourGhostHands",g_pkGhost);number("ParkourGhostAlpha",(float)g_pkGhostAlpha);
    number("ParkourGhostRadius",g_pkGhostRadius);boolean("ParkourGeomCensus",g_geomCensus);
    boolean("ParkourLockAnim",g_pkLockAnim);boolean("ParkourDirectBody",g_pkDirectBody);
    number("TestStall",(float)g_testStallFrame);boolean("TestWideFov",g_targetHalfFovX>0);
    number("OcclusionMode",(float)g_occlusionMode);boolean("PinMinDesiredFps",g_pinMinDesiredFps);
    assert(MenuAtomicWrite(mevr::updateIni(ReadTestFile(g_settingsPath),extra)));
    puts("PASS effective settings snapshot written without launching the game");
}
static UINT STDMETHODCALLTYPE TestModeCount(IDirect3D9*, UINT, D3DFORMAT fmt) {
    return fmt == D3DFMT_X8R8G8B8 ? 2 : 0;
}
static HRESULT STDMETHODCALLTYPE TestEnumMode(IDirect3D9*, UINT, D3DFORMAT fmt, UINT index,
                                               D3DDISPLAYMODE* mode) {
    if (!mode || fmt != D3DFMT_X8R8G8B8 || index >= 2) return D3DERR_INVALIDCALL;
    *mode = {index ? 1920u : 1600u, index ? 1080u : 1200u, 60, fmt};
    return D3D_OK;
}
static void AutoModeTests() {
    g_origGetAdapterModeCount=TestModeCount;g_origEnumAdapterModes=TestEnumMode;
    g_resAuto=true;g_forceResW=4224;g_forceResH=2376;
    D3DDISPLAYMODE mode{};
    assert(Hook_GetAdapterModeCount(nullptr,0,D3DFMT_X8R8G8B8)==1);
    assert(SUCCEEDED(Hook_EnumAdapterModes(nullptr,0,D3DFMT_X8R8G8B8,0,&mode)));
    assert(mode.Width==4224&&mode.Height==2376&&mode.RefreshRate==60);
    assert(FAILED(Hook_EnumAdapterModes(nullptr,0,D3DFMT_X8R8G8B8,1,&mode)));
    assert(FAILED(Hook_EnumAdapterModes(nullptr,0,D3DFMT_X8R8G8B8,0,nullptr)));
    assert(Hook_GetAdapterModeCount(nullptr,0,D3DFMT_UNKNOWN)==0);
    g_resAuto=false;
    assert(Hook_GetAdapterModeCount(nullptr,0,D3DFMT_X8R8G8B8)==3);
    assert(SUCCEEDED(Hook_EnumAdapterModes(nullptr,0,D3DFMT_X8R8G8B8,0,&mode))&&mode.Width==1600);
    assert(SUCCEEDED(Hook_EnumAdapterModes(nullptr,0,D3DFMT_X8R8G8B8,2,&mode))&&mode.Width==4224);
    g_forceResW=g_forceResH=0;
    assert(Hook_GetAdapterModeCount(nullptr,0,D3DFMT_X8R8G8B8)==2);
    g_resAuto=true; // no cache on the first launch still offers the native list
    assert(Hook_GetAdapterModeCount(nullptr,0,D3DFMT_X8R8G8B8)==2);
    g_origGetAdapterModeCount=nullptr;g_origEnumAdapterModes=nullptr;g_modeInjectCount=0;
}
static void MotionSamplingGateTests() {
    g_armSwing=false;g_armSwingDebug=false;g_motionHands=true;g_motionPunch=true;g_gripToGrip=true;
    g_swingL.havePrev=g_swingR.havePrev=true;
    ArmSwingSample(1000000000LL,false);
    assert(!g_swingL.havePrev&&!g_swingR.havePrev); // punch sampling survives locomotion Off
    g_motionPunch=false;g_armSwing=true;
    g_swingL.havePrev=g_swingR.havePrev=true;
    ArmSwingSample(1000000000LL,false);
    assert(!g_swingL.havePrev&&!g_swingR.havePrev); // normal sampling survives logging Off
    g_motionPunch=true;
    g_padEnabled=true;g_sweepActive=false;g_pkMode=PK_NONE;g_moveInputBlocked=false;
    g_gripValue[0]=g_gripValue[1]=0;g_swingHeadVert=0;g_swingJumpGraceUntil=0;
    g_swingAltValid=true;g_swingAltSmooth=-0.5f;
    g_swingL.tracked=g_swingR.tracked=true;
    g_swingDt=1.0f/72;g_swingNow=g_swingFull=2.0f;
    float result=0;
    for(int i=0;i<144;++i) {
        const XrTime when=2000000000LL+i*13888889LL;
        g_swingHandLoud[0]=g_swingHandLoud[1]=when;
        result=ArmSwingDeflection(when,0,0);
    }
    assert(result==1.0f); // sustained opposed swings drive forward with debug Off
    assert(ArmSwingDeflection(4100000000LL,0,-1)==0); // stick-back emergency stop
    g_gripValue[0]=1;
    assert(ArmSwingDeflection(4200000000LL,0,0)==0); // a punch must not drive locomotion
    g_gripValue[0]=0;
}
static void ResolutionTests() {
    AutoModeTests();MotionSamplingGateTests();
    // No real headset cache is configured in this harness, so auto cannot touch
    // the user's engine INI. Exercise first-run fallback and parser transitions.
    assert(!g_headsetCachePath[0]);
    assert(MenuAtomicWrite("Resolution = off\n"));
    LoadSettings();VrMenuInitializeSettings();
    assert(!g_resAuto&&!g_forceResW&&!g_forceResH&&g_menuResolution=="off");
    assert(MenuAtomicWrite("Resolution = auto\n"));
    LoadSettings();VrMenuInitializeSettings();
    assert(g_resAuto&&!g_forceResW&&!g_forceResH&&g_menuResolution=="auto");
    assert(MenuAtomicWrite("Resolution = 2560x1440\n"));
    LoadSettings();VrMenuInitializeSettings();
    assert(!g_resAuto&&g_forceResW==2560&&g_forceResH==1440&&g_menuResolution=="2560x1440");
    assert(SaveMenuSettings());LoadSettings();
    assert(!g_resAuto&&g_forceResW==2560&&g_forceResH==1440);
    assert(MenuAtomicWrite("; Resolution omitted\n"));
    LoadSettings();VrMenuInitializeSettings();
    assert(g_resAuto&&!g_forceResW&&!g_forceResH&&g_menuResolution=="auto");
    assert(DeleteFileW(g_settingsPath)); // only the harness INI beside its test log
    g_resAuto=false;g_forceResW=2560;g_forceResH=1440;
    LoadSettings();VrMenuInitializeSettings();
    assert(g_resAuto&&!g_forceResW&&!g_forceResH&&g_menuResolution=="auto");
    puts("PASS resolution auto default without INI/key/cache, explicit off/custom, and save/reload");
}
static void InputTests() {
    using namespace mevr;
    MenuInputState s;MenuInput in;in.y=true;
    assert(!s.sample(100,in,false).open);
    assert(!s.sample(1099,in,false).open);
    assert(s.sample(1100,in,false).open);
    assert(!s.sample(4000,in,true).open);
    in.y=false;assert(!s.sample(4010,in,true).shortY);
    in.accept=true;assert(s.sample(4020,in,true).accept);
    assert(!s.sample(4030,in,true).accept);
    in.accept=false;in.y=true;s.sample(4100,in,true);
    assert(!s.sample(5099,in,true).close);
    assert(s.sample(5100,in,true).close);
    assert(!s.sample(7000,in,false).open); // held closing press cannot reopen
    in.y=false;assert(!s.sample(7010,in,false).shortY);
    in.y=true;s.sample(7100,in,false);assert(s.sample(8100,in,false).open);
    s={};in={};in.y=true;s.sample(100,in,false);in.y=false;
    assert(s.sample(1099,in,false).shortY);assert(!s.sample(1100,in,false).shortY);
    s={};in={};in.y=true;s.sample(100,in,false);in.focused=false;
    assert(!s.sample(2000,in,false).open);in.focused=true;
    assert(!s.sample(3000,in,false).open);in.y=false;s.sample(3010,in,false);
    in.y=true;s.sample(3020,in,false);assert(s.sample(4020,in,false).open);
    // Closing never forwards held sticks, grips, or triggers to gameplay.
    s={};s.quarantine();in={};in.x=.9f;s.sample(1,in,false);assert(s.waitNeutral);
    in.x=0;in.other=true;s.sample(2,in,false);assert(s.waitNeutral);
    in.other=false;in.accept=true;s.sample(3,in,false);assert(s.waitNeutral);
    in.accept=false;s.sample(4,in,false);assert(!s.waitNeutral);
    // Navigation edges and repeat use time, not frame count / selected cap.
    s={};in.z=-1;assert(s.sample(100,in,true).row==1);
    assert(s.sample(499,in,true).row==0);assert(s.sample(500,in,true).row==1);
    assert(s.sample(679,in,true).row==0);assert(s.sample(680,in,true).row==1);
    in.z=0;in.x=-1;assert(s.sample(681,in,true).value==-1);
    puts("PASS hold Y, short Y, focus loss, neutral rearm, input repeat");
}
static void IniTests() {
    const std::string source="\xEF\xBB\xBF; keep this\r\n FrameCap = 60 ; user's note\r\nUnknown = 17\r\nframecap=90\r\n[other]\r\n";
    const auto result=mevr::updateIni(source,{{"FrameCap","72"},{"ShowFPS","on"}});
    assert(result.find("\xEF\xBB\xBF; keep this\r\n")==0);
    assert(result.find("FrameCap = 72 ; user's note")!=std::string::npos);
    assert(result.find("framecap= 72")!=std::string::npos);
    assert(result.find("Unknown = 17\r\n")!=std::string::npos);
    assert(result.find("ShowFPS = on\r\n")!=std::string::npos);
    assert(mevr::updateIni("x=1",{{"y","2"}})=="x=1\ny = 2\n");
    assert(MenuAtomicWrite(source));
    g_motionHands=true;g_menuArm=true;g_menuGuns=false;g_menuMelee=true;g_menuParkour=true;
    MenuApplyFeatures();assert(g_armSwing&&g_armSwingJump&&g_motionPunch&&g_parkour&&!g_pistolHands);
    g_fpsCap=36;g_gunWristDownDeg[0]=25;g_gunPositionMm[1][2]=-35;
    assert(SaveMenuSettings());auto saved=ReadTestFile(g_settingsPath);
    assert(saved.find("Unknown = 17")!=std::string::npos);
    assert(saved.find("GunLeftWristDownDegrees = 25")!=std::string::npos);
    assert(saved.find("GunRightUpMm = -35")!=std::string::npos);
    g_motionHands=false;MenuApplyFeatures();assert(!g_armSwing&&!g_motionPunch&&!g_parkour&&!g_gripToGrip);
    assert(g_menuArm&&g_menuMelee&&g_menuParkour);assert(SaveMenuSettings());
    // Run the real parser against the saved file using its normal fallback location.
    LoadSettings();VrMenuInitializeSettings();
    assert(!g_motionHands && g_menuArm && !g_menuGuns && g_menuMelee && g_menuParkour);
    assert(g_fpsCap==36 && g_gunWristDownDeg[0]==25 && g_gunPositionMm[1][2]==-35);
    MenuActivate(MHands,1);assert(g_armSwing&&g_motionPunch&&g_parkour&&!g_pistolHands);
    // Saving on a ledge must not persist the temporary forced camera locks.
    g_pkAnimSaved=true;g_pkAnimPrev[0]=true;g_animFollow=false;
    assert(SaveMenuSettings());assert(ReadTestFile(g_settingsPath).find("LockAnimPitch = off")!=std::string::npos);
    MenuActivate(MPitch,1);assert(!g_pkAnimPrev[0]&&!g_animFollow);
    g_pkAnimSaved=false;
    // A sharing violation must leave the original intact and report failure.
    saved=ReadTestFile(g_settingsPath);
    HANDLE blocker=CreateFileW(g_settingsPath,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    assert(blocker!=INVALID_HANDLE_VALUE);g_fpsCap=144;assert(!SaveMenuSettings());
    assert(strstr(g_menuStatus,"SAVE FAILED"));CloseHandle(blocker);
    assert(ReadTestFile(g_settingsPath)==saved);
    // Runtime keyboard calibration uses the same persistent file now.
    SaveGunCalibration();assert(!g_gunCalibrationSaveFailed);
    assert(ReadTestFile(g_settingsPath).find("FrameCap = 144")!=std::string::npos);
    assert(!VrMenuLoadSetting("MenuWidth","nan"));assert(!VrMenuLoadSetting("MenuDistance","99"));
    assert(!VrMenuLoadSetting("ShowReticle","maybe"));
    for(bool visible:{true,false}) {
        MenuActivate(MReticle,visible?1:-1);assert(g_showReticle==visible);
        g_showReticle=!visible;LoadSettings();assert(g_showReticle==visible);
    }
    assert(!VrMenuLoadSetting("GunLeftUpMm","1.5"));assert(!VrMenuLoadSetting("GunLeftUpMm","201"));
    // Unlimited changes only its reflected bit; choosing a cap restores it.
    uint32_t engineFlags=0xC5;g_engineObj=(uintptr_t)&engineFlags;g_smoothFlagOffset=0;g_smoothFlagMask=4;
    g_fpsCap=0;VrMenuFrameCapTick();assert(engineFlags==0xC1 && !g_menuUnlimitedFailed);
    g_fpsCap=72;VrMenuFrameCapTick();assert(engineFlags==0xC5);
    for(float cap:{30.f,36.f,60.f,72.f,90.f,120.f,144.f,0.f}) {
        g_fpsCap=cap;assert(SaveMenuSettings());LoadSettings();VrMenuInitializeSettings();assert(g_fpsCap==cap);
    }
    MenuRestoreDefaults();assert(ReadTestFile(g_settingsPath)==kVrShippedDefaults);
    assert(g_menuResolution=="auto"&&g_menuRestart);
    assert(g_motionHands&&g_menuArm&&g_armSwing&&g_armSwingJump&&g_gripToGrip&&g_stickJumpTurn);
    LoadSettings();VrMenuInitializeSettings();assert(g_fpsCap==72&&g_motionHands&&g_menuArm&&!g_debug);
    assert(g_motionHandsDebug&&g_armSwingDebug&&g_parkourDebug);
    assert(g_resAuto&&g_menuResolution=="auto");
    assert(g_armSwing&&g_armSwingJump&&g_gripToGrip&&g_stickJumpTurn);
    // Existing explicit opt-outs survive save/reload despite the enabled defaults.
    MenuActivate(MArm,-1);MenuActivate(MHands,-1);
    LoadSettings();VrMenuInitializeSettings();
    assert(!g_motionHands&&!g_menuArm&&!g_armSwing&&!g_gripToGrip&&!g_stickJumpTurn);
    MenuRestoreDefaults();
    assert(!g_showReticle);
    assert(g_gunWristDownDeg[0]==40&&g_gunWristDownDeg[1]==40);
    assert(g_gunWristRightDeg[0]==-10&&g_gunWristRightDeg[1]==-10);
    assert(g_gunPositionMm[0][1]==40&&g_gunPositionMm[1][1]==0&&g_gunPositionMm[1][2]==0);
    LoadSettings();assert(g_pkExitBoost==1.5f&&g_pkPumpFull==.15f&&g_pkBarWrap==63.f);
    puts("PASS preferences, all cap roundtrips, gun calibration, atomic failure, comments, defaults");
}
static void PauseTests() {
    // A small fake reflected object graph exercises production pause ownership.
    uint32_t pauser=0,world=(uint32_t)(uintptr_t)&pauser;
    g_playerCtl=(uintptr_t)&world;g_menuWorldOffset=g_menuPauserOffset=0;
    assert(MenuPaused()==0);MenuOpen(GetTickCount64());assert(g_menuPausePending&&!g_menuOwnsPause);
    bool shortY=false;pauser=123;
    VrMenuInputTick(0,true,false,false,false,false,0,0,&shortY);
    assert(g_menuOwnsPause&&!g_menuPausePending);g_menuPulseUntil=0;
    MenuClose();assert(g_menuClosing&&g_menuOpen);
    pauser=0;VrMenuInputTick(0,true,false,false,false,false,0,0,&shortY);
    assert(!g_menuOpen&&!g_menuOwnsPause);
    // Existing pause survives opening and closing; no Start pulse gets sent.
    pauser=123;g_menuPulseUntil=0;MenuOpen(GetTickCount64());assert(!g_menuOwnsPause&&!g_menuPausePending);
    MenuClose();assert(!g_menuOpen&&pauser==123&&g_menuPulseUntil==0);
    // Failure to pause is visible, never treated as ownership.
    pauser=0;MenuOpen(GetTickCount64());g_menuPauseDeadline=0;
    VrMenuInputTick(0,true,false,false,false,false,0,0,&shortY);
    assert(!g_menuPausePending&&!g_menuOwnsPause&&strstr(g_menuStatus,"DID NOT CONFIRM"));
    MenuClose();assert(!g_menuOpen);
    g_playerCtl=0;
    puts("PASS pause ownership, resume, pre-existing pause, failure visibility");
}
static void MetadataTests() {
    // Cooked classes may have no usable Children chain. Include same-named
    // fields on unrelated owners and an invalid boolean mask in the table.
    const char* names[]={"Class","Actor","WorldInfo","GameEngine","ObjectProperty","BoolProperty","Pauser","bSmoothFrameRate","Unrelated",
        "PlayerController","TdSPHUD","myHUD","bDisableDrawCrossHair","LiveHUD","Default__TdSPHUD"};
    uint32_t namePointers[15];for(int i=0;i<15;++i)namePointers[i]=(uint32_t)(uintptr_t)names[i];
    uint32_t nameArray[]={(uint32_t)(uintptr_t)namePointers,15};
    uint32_t objects[18][40]{};uint32_t pointers[18];
    for(int i=0;i<18;++i)pointers[i]=(uint32_t)(uintptr_t)objects[i];
    g_offName=0x10;g_offOuter=0x20;g_offPropOff=0x80;g_offBoolMask=0x84;
    g_nameTextOff=0;g_nameWide=false;g_gnamesAddr=(uintptr_t)nameArray;
    auto object=[&](int i,int name,int cls,int owner,int offset=0,int mask=0){
        objects[i][g_offName/4]=name;objects[i][kUObjectClassOff/4]=pointers[cls];
        objects[i][g_offOuter/4]=pointers[owner];objects[i][g_offPropOff/4]=offset;objects[i][g_offBoolMask/4]=mask;
    };
    object(0,0,0,0);object(1,1,0,0);object(2,2,0,0);object(3,3,0,0);
    object(4,4,0,0);object(5,5,0,0);object(6,2,4,1,0x150);object(7,6,4,2,0xc54);
    object(8,7,5,3,0x47c,4);object(9,8,0,0);object(10,2,4,9,0x400);
    object(11,9,0,0);object(12,10,0,0);object(13,11,4,11,0x40);object(14,12,5,12,0x44,0x100);
    object(15,13,12,0);object(16,8,11,0);object(17,13,12,0);
    uint32_t table[]={(uint32_t)(uintptr_t)pointers,18};g_gobjAddr=(uintptr_t)table;
    VrMenuResolveEngine();assert(g_menuWorldOffset==0x150&&g_menuPauserOffset==0xc54);
    assert(g_smoothFlagOffset==0x47c&&g_smoothFlagMask==4);
    assert(g_menuHudOffset==0x40&&g_menuReticleOffset==0x44&&g_menuReticleMask==0x100);
    g_imgLo=0x100000;g_imgHi=0x110000;objects[15][0]=objects[17][0]=g_imgLo;
    objects[16][0x40/4]=pointers[15];objects[15][0x44/4]=0xA5;
    g_playerCtl=pointers[16];g_combatFault=true;g_standardControllerActive=true;g_menuBlocksGameplay=1;
    g_showReticle=false;VrMenuReticleTick();assert(objects[15][0x44/4]==0x1A5);
    g_showReticle=true;VrMenuReticleTick();assert(objects[15][0x44/4]==0xA5);
    objects[16][0x40/4]=pointers[17];objects[17][0x44/4]=0x42;
    g_showReticle=false;VrMenuReticleTick();assert(objects[17][0x44/4]==0x142); // HUD replaced on respawn
    objects[17][g_offName/4]=14;objects[17][0x44/4]=0x42;
    VrMenuReticleTick();assert(objects[17][0x44/4]==0x42); // never modify a default object
    objects[14][g_offBoolMask/4]=3;VrMenuResolveEngine();assert(g_menuReticleMask==0);
    g_combatFault=false;g_standardControllerActive=false;g_menuBlocksGameplay=0;g_playerCtl=0;
    objects[8][g_offBoolMask/4]=3;VrMenuResolveEngine();assert(g_smoothFlagMask==0);
    g_gobjAddr=g_gnamesAddr=0;g_offName=g_offOuter=g_offPropOff=g_offBoolMask=-1;
    puts("PASS pause/cap/reticle metadata, native reticle toggles with combat unavailable/gamepad/menu active, respawn, unrelated bits and default-object preservation");
}
static void WritePreview(const wchar_t* directory,int page) {
    g_menuPage=page;g_menuRow=1;g_menuOpen=true;g_scFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
    g_motionHands=true;g_menuArm=true;g_menuGuns=true;g_menuMelee=true;g_menuParkour=true;
    int count=0;const MenuItem* items=MenuItems(&count);char values[8][64]{};
    for(int i=0;i<count;++i)MenuValue(items[i].id,values[i],sizeof(values[i]));
    g_menuPixels.resize(kMenuW*kMenuH);MenuMessage("GAME PAUSED - CHANGES SAVE AUTOMATICALLY");
    MenuRasterize(count,items,values,"72 FPS");
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};file.bfType=0x4D42;
    file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+kMenuW*kMenuH*4;
    info.biSize=sizeof(info);info.biWidth=kMenuW;info.biHeight=-kMenuH;info.biPlanes=1;info.biBitCount=32;
    wchar_t path[MAX_PATH];swprintf_s(path,L"%s\\menu-%d.bmp",directory,page);
    FILE* out=nullptr;_wfopen_s(&out,path,L"wb");assert(out);
    fwrite(&file,sizeof(file),1,out);fwrite(&info,sizeof(info),1,out);fwrite(g_menuPixels.data(),4,g_menuPixels.size(),out);fclose(out);
}
static void SmokeCaptureTests() {
    size_t vb=0,ib=0;
    assert(SmokeCaptureSizes(D3DPT_TRIANGLELIST,4,8,4,D3DFMT_INDEX16,32,&vb,&ib));
    assert(vb==384 && ib==24); // include the minimum-index offset in the UP buffer
    assert(SmokeCaptureSizes(D3DPT_TRIANGLELIST,0,8,4,D3DFMT_INDEX32,32,&vb,&ib));
    assert(vb==256 && ib==48);
    assert(!SmokeCaptureSizes(D3DPT_TRIANGLELIST,UINT_MAX,1,4,D3DFMT_INDEX16,32,&vb,&ib));
    assert(!SmokeCaptureSizes(D3DPT_TRIANGLELIST,0,8,UINT_MAX,D3DFMT_INDEX32,32,&vb,&ib));
    assert(!SmokeCaptureSizes(D3DPT_TRIANGLELIST,0,8,4,D3DFMT_INDEX16,257,&vb,&ib));
    assert(!SmokeCaptureSizes(D3DPT_TRIANGLELIST,0,8,4,D3DFMT_UNKNOWN,32,&vb,&ib));
    assert(!SmokeCaptureSizes(D3DPT_TRIANGLESTRIP,0,8,4,D3DFMT_INDEX16,32,&vb,&ib));
    assert(!SmokeCaptureSizes(D3DPT_TRIANGLELIST,0,0,4,D3DFMT_INDEX16,32,&vb,&ib));
    // Outside an explicit capture, even the device/input pointers are untouched.
    g_effectCaptureUntil=0;
    CaptureSmokeIndexedUP(nullptr,D3DPT_TRIANGLELIST,0,8,4,nullptr,D3DFMT_INDEX16,nullptr,32);
    puts("PASS smoke capture buffer bounds and inactive capture gate");
}
int wmain(int argc,wchar_t** argv) {
    const bool snapshot=argc==4&&!wcscmp(argv[1],L"--snapshot");
    assert(argc==2||snapshot);InitializeCriticalSection(&g_padLock);g_padLockReady=true;
    LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);g_qpcFreq=(double)frequency.QuadPart;
    const wchar_t* directory=snapshot?argv[3]:argv[1];
    swprintf_s(g_settingsPath,L"%s\\mevr.ini",directory);swprintf_s(g_logPath,L"%s\\mevr.log",directory);
    if(snapshot){WriteEffectiveSettings(argv[2]);return 0;}
    VrMenuInitializeSettings();
    assert(g_resAuto&&g_menuResolution=="auto");
    assert(g_motionHands&&g_menuArm&&g_armSwing&&g_armSwingJump&&g_gripToGrip&&g_stickJumpTurn);
    ResolutionTests();InputTests();IniTests();MetadataTests();PauseTests();SmokeCaptureTests();
    for(int page=0;page<9;++page)WritePreview(argv[1],page);
    puts("PASS all 9 production menu page previews rendered");
    return 0;
}
