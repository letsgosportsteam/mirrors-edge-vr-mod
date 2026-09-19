#include "vr_controls_state.h"
static mevr::SnapTurn g_snapInput;
static int g_controlInputOffset=-1,g_controlYawScaleOffset=-1;
static volatile LONG g_controlMetadataReady=0;
static bool VrPollStandardController(MEVR_XINPUT_STATE* state)
{
    static bool connected=false;static uint64_t nextScan=0;
    if(!g_origXiGetState)return false;
    if(connected && g_origXiGetState(g_standardControllerIndex,state)==ERROR_SUCCESS)return true;
    connected=false;
    const uint64_t now=GetTickCount64();if(now<nextScan)return false;
    nextScan=now+1000; // disconnected XInput probes can be expensive
    for(DWORD i=0;i<4;++i)if(g_origXiGetState(i,state)==ERROR_SUCCESS) {
        g_standardControllerIndex=i;connected=true;return true;
    }
    return false;
}
static void VrSelectController(bool connected,const MEVR_XINPUT_STATE& state,bool questActivity)
{
    const auto& p=state.Gamepad;
    const bool activity=p.wButtons||p.bLeftTrigger>30||p.bRightTrigger>30||
        abs((int)p.sThumbLX)>10000||abs((int)p.sThumbLY)>10000||abs((int)p.sThumbRX)>10000||abs((int)p.sThumbRY)>10000;
    static bool previousPhysical=false,previousQuest=false;
    const bool physicalEdge=connected&&activity&&!previousPhysical;
    const bool questEdge=questActivity&&!previousQuest;
    previousPhysical=connected&&activity;previousQuest=questActivity;
    bool next=g_standardControllerActive;
    if(g_controllerMode==1||!connected)next=false;
    else if(g_controllerMode==2)next=true;
    else if(physicalEdge)next=true;
    else if(questEdge)next=false;
    if(next==g_standardControllerActive)return;
    g_standardControllerActive=next;
    MenuApplyFeatures();g_menuInput.quarantine();MenuResetGestures();
    g_snapInput={};InterlockedExchange(&g_pendingSnapYaw,0);
    Log("[controls] active device: %s%s",next?"standard XInput controller":"Quest controllers",
        g_controllerMode==0?" (automatic)":" (selected)");
}
static bool VrTurningAllowed()
{
    return g_padEnabled&&g_headTracking&&!g_menuBlocksGameplay&&g_xrState==XR_SESSION_STATE_FOCUSED&&
        g_playerPawn&&g_playerCtl&&MenuPaused()==0&&!CinematicHeadLookRequested();
}
static float VrTurnAxis(float axis)
{
    if(!std::isfinite(axis)){g_snapInput.sample(axis,false);return 0.f;}
    axis=(std::max)(-1.f,(std::min)(1.f,axis));
    const bool allowed=VrTurningAllowed();
    const int step=g_snapInput.sample(axis,g_snapTurning&&allowed);
    if(step)InterlockedExchange(&g_pendingSnapYaw,(LONG)lroundf(step*g_snapTurnAngle*65536.f/360.f));
    return g_snapTurning&&allowed?0.f:axis;
}
static int VrTakeSnapTurn()
{
    const int step=(int)InterlockedExchange(&g_pendingSnapYaw,0);
    if(!g_snapTurning||!VrTurningAllowed())return 0;
    if(step)g_artificialTurnUntil=GetTickCount64()+180;
    return step;
}
static void VrPublishStandardController(MEVR_XINPUT_STATE state,bool shortY)
{
    state.Gamepad.wButtons&=~MEVR_PAD_Y;if(shortY)state.Gamepad.wButtons|=MEVR_PAD_Y;
    const float rx=state.Gamepad.sThumbRX/32767.f;
    state.Gamepad.sThumbRX=(SHORT)(VrTurnAxis(rx)*32767.f);
    g_padSentLX=state.Gamepad.sThumbLX/32767.f;g_padSentLY=state.Gamepad.sThumbLY/32767.f;g_padSentRX=rx;
    EnterCriticalSection(&g_padLock);state.dwPacketNumber=g_pad.dwPacketNumber+1;g_pad=state;LeaveCriticalSection(&g_padLock);
}
static void VrControlsResolveMetadata()
{
    DetachedPropRequest fields[]={
        {"PlayerController","PlayerInput","ObjectProperty",&g_controlInputOffset},
        {"PlayerInput","LookRightScale","FloatProperty",&g_controlYawScaleOffset}
    };
    LookupDetachedPropsBatch(fields,2);
    Log("[controls] PlayerInput offset=%d LookRightScale offset=%d",g_controlInputOffset,g_controlYawScaleOffset);
    InterlockedExchange(&g_controlMetadataReady,1);
}
static void VrControlsTick()
{
    const bool allowed=VrTurningAllowed();
    if(!allowed){g_snapInput.sample(0,false);InterlockedExchange(&g_pendingSnapYaw,0);}
    if(!InterlockedCompareExchange(&g_controlMetadataReady,0,0)||g_controlInputOffset<0||g_controlYawScaleOffset<0)return;
    uint32_t input=0;
    if(!g_playerCtl||!SafeU32(g_playerCtl+g_controlInputOffset,&input)||!input)return;
    static uintptr_t previous=0;static float base=0,last=0;
    float current=0;const uintptr_t address=input+g_controlYawScaleOffset;
    if(!SafeRead(address,&current,4)||!std::isfinite(current)||current<=0)return;
    if(previous!=input||fabsf(current-last)>.01f){previous=input;base=current;}
    const float wanted=base*(allowed&&!g_snapTurning?g_smoothTurnSpeed:1.f);
    SIZE_T wrote=0;
    if(current==wanted||(WriteProcessMemory(GetCurrentProcess(),(void*)address,&wanted,4,&wrote)&&wrote==4))last=wanted;
    else last=current;
}
