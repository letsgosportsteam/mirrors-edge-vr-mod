#include "../src/d3d9.cpp"
#include <cassert>
static MEVR_XINPUT_STATE hardware{};
static bool connected=true;
static int polls=0,capsIndex=-1;
static DWORD WINAPI HardwareState(DWORD index,MEVR_XINPUT_STATE* out) {
    ++polls;if(!connected||index!=2)return ERROR_DEVICE_NOT_CONNECTED;*out=hardware;return ERROR_SUCCESS;
}
static DWORD WINAPI HardwareCaps(DWORD index,DWORD,MEVR_XINPUT_CAPABILITIES* out) {
    capsIndex=(int)index;memset(out,0,sizeof(*out));out->Type=1;out->SubType=1;return ERROR_SUCCESS;
}
int wmain(int argc,wchar_t** argv) {
    assert(argc==2);InitializeCriticalSection(&g_padLock);g_padLockReady=true;
    LARGE_INTEGER hz;QueryPerformanceFrequency(&hz);g_qpcFreq=(double)hz.QuadPart;
    swprintf_s(g_settingsPath,L"%s\\mevr.ini",argv[1]);swprintf_s(g_logPath,L"%s\\mevr.log",argv[1]);
    VrMenuInitializeSettings();
    mevr::SnapTurn snap;
    assert(snap.sample(1,true)==0);assert(snap.sample(0,true)==0);assert(snap.sample(.71f,true)==1);
    for(int i=0;i<1000;++i)assert(snap.sample(1,true)==0);
    assert(snap.sample(-1,true)==0);snap.sample(0,true);assert(snap.sample(-.8f,true)==-1);
    snap.sample(0,false);assert(snap.sample(1,true)==0);snap.sample(0,true);assert(snap.sample(NAN,true)==0);
    puts("PASS one snap per centered deflection, held/reversed stick, focus/menu rearm, invalid input");
    g_origXiGetState=HardwareState;g_origXiGetCaps=HardwareCaps;g_actionsReady=true;
    MEVR_XINPUT_STATE state{};assert(VrPollStandardController(&state)&&g_standardControllerIndex==2);
    const int firstPolls=polls;assert(VrPollStandardController(&state)&&polls==firstPolls+1);
    g_motionHands=true;g_menuArm=g_menuGuns=g_menuMelee=g_menuParkour=true;MenuApplyFeatures();
    VrSelectController(true,state,true);assert(!g_standardControllerActive);
    state.Gamepad.wButtons=MEVR_PAD_A;VrSelectController(true,state,true);
    assert(g_standardControllerActive&&!g_armSwing&&!g_pistolHands&&!g_motionPunch&&!g_parkour);
    assert(g_motionHands&&g_menuArm&&g_menuGuns&&g_menuMelee&&g_menuParkour); // preferences survive
    state.Gamepad.wButtons=0;VrSelectController(true,state,true);assert(g_standardControllerActive); // held Quest input cannot steal back
    VrSelectController(true,state,false);VrSelectController(true,state,true);assert(!g_standardControllerActive&&g_armSwing);
    g_controllerMode=2;VrSelectController(true,state,false);assert(g_standardControllerActive);
    // Production hardware packet path and hook: all native gameplay controls pass through.
    state.Gamepad={MEVR_PAD_A|MEVR_PAD_B|MEVR_PAD_LSHOULDER|MEVR_PAD_RTHUMB,123,234,21000,-23000,12000,-9000};
    VrPublishStandardController(state,false);MEVR_XINPUT_STATE delivered{};
    assert(Hook_XInputGetState(0,&delivered)==ERROR_SUCCESS);
    assert(!memcmp(&delivered.Gamepad,&state.Gamepad,sizeof(state.Gamepad)));
    assert(Hook_XInputGetState(2,&delivered)==ERROR_DEVICE_NOT_CONNECTED); // mapped once to player 0
    MEVR_XINPUT_CAPABILITIES caps{};assert(Hook_XInputGetCaps(0,0,&caps)==ERROR_SUCCESS&&capsIndex==2);
    state.Gamepad.wButtons=MEVR_PAD_Y;VrPublishStandardController(state,false);assert(!(g_pad.Gamepad.wButtons&MEVR_PAD_Y));
    VrPublishStandardController(state,true);assert(g_pad.Gamepad.wButtons&MEVR_PAD_Y);
    connected=false;VrSelectController(false,{},false);assert(!g_standardControllerActive&&g_parkour);
    assert(!VrPollStandardController(&state));const int failedPolls=polls;assert(!VrPollStandardController(&state)&&polls==failedPolls);
    puts("PASS physical input/caps routing from slot 2, native buttons/triggers/axes, hot switching, disconnect, no ghost gestures");
    // Actual reflected speed write, using an in-process controller/world/input graph.
    uint32_t paused=0;float yawScale=12000;
    uint32_t ctl[]={(uint32_t)(uintptr_t)&paused,(uint32_t)(uintptr_t)&yawScale};
    g_playerCtl=(uintptr_t)ctl;g_playerPawn=123;g_menuWorldOffset=g_menuPauserOffset=0;
    g_controlInputOffset=4;g_controlYawScaleOffset=0;g_controlMetadataReady=1;g_gateCount=0;
    g_xrState=XR_SESSION_STATE_FOCUSED;g_headTracking=true;g_menuBlocksGameplay=0;
    g_smoothTurnSpeed=1;VrControlsTick();assert(yawScale==12000);
    g_smoothTurnSpeed=2;VrControlsTick();assert(yawScale==24000);VrControlsTick();assert(yawScale==24000);
    g_smoothTurnSpeed=.25f;VrControlsTick();assert(yawScale==3000);
    g_snapTurning=true;VrControlsTick();assert(yawScale==12000);
    VrTurnAxis(0);assert(VrTurnAxis(NAN)==0&&VrTakeSnapTurn()==0);
    assert(VrTurnAxis(1)==0&&VrTakeSnapTurn()==0);VrTurnAxis(0);VrTurnAxis(1);
    assert(VrTakeSnapTurn()==8192&&VrTakeSnapTurn()==0); // 45 degrees
    VrTurnAxis(0);VrTurnAxis(-1);g_menuBlocksGameplay=1;VrControlsTick();assert(VrTakeSnapTurn()==0);
    g_menuBlocksGameplay=0;assert(VrTurnAxis(-1)==0&&VrTakeSnapTurn()==0);
    VrTurnAxis(0);VrTurnAxis(-1);paused=1;assert(VrTakeSnapTurn()==0);
    assert(VrTurnAxis(.9f)==.9f);paused=0;g_snapTurning=false;g_smoothTurnSpeed=1;
    puts("PASS original smooth speed preserved, 25-200 percent without compounding, snap yaw, paused/menu rejection");
    assert(VrMenuLoadSetting("TurnMode","snap"));assert(VrMenuLoadSetting("ControllerMode","gamepad"));
    assert(VrMenuLoadSetting("SmoothTurnSpeed","1.5"));assert(VrMenuLoadSetting("SnapTurnAngle","30"));
    assert(VrMenuLoadSetting("GameUIScale","0.6"));assert(VrMenuLoadSetting("GameUIHeight","0.1"));
    assert(!VrMenuLoadSetting("SmoothTurnSpeed","nan"));assert(!VrMenuLoadSetting("SnapTurnAngle","360"));
    assert(!VrMenuLoadSetting("GameUIScale","0"));assert(!VrMenuLoadSetting("ControllerMode","banana"));
    assert(MenuAtomicWrite("; test\nUnknown = keep\n"));assert(SaveMenuSettings());
    g_snapTurning=false;g_controllerMode=0;g_smoothTurnSpeed=1;g_snapTurnAngle=45;g_gameUiScale=1;g_gameUiHeight=0;
    LoadSettings();assert(g_snapTurning&&g_controllerMode==2&&g_smoothTurnSpeed==1.5f&&g_snapTurnAngle==30);
    assert(g_gameUiScale==.6f&&g_gameUiHeight==.1f);
    MenuActivate(MPageControls,0);assert(g_menuPage==8);MenuActivate(MBack,0);assert(g_menuPage==0);
    MenuRestoreDefaults();assert(!g_snapTurning&&g_controllerMode==0&&g_smoothTurnSpeed==1&&g_snapTurnAngle==45&&g_gameUiScale==.65f);
    puts("PASS controller/turn/UI settings roundtrip, invalid values, page navigation, shipped defaults");
}
