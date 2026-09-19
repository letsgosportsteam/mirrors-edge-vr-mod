// Included after configuration/rig definitions. All UI work runs on Present;
// the game sees only a neutral pad or an explicit pause-button pulse.
#include "vr_menu_state.h"
#include "vr_defaults.inl"

static bool g_menuOpen = false, g_menuClosing = false, g_menuOwnsPause = false;
static bool g_menuPausePending = false, g_menuReady = false, g_menuRestart = false;
static bool g_menuArm = true, g_menuGuns = true, g_menuMelee = true, g_menuParkour = true;
static bool g_menuFps = false, g_menuEx = true, g_menuLens = false;
static bool g_showReticle=false;
static int g_menuHudOffset=-1,g_menuReticleOffset=-1;
static uint32_t g_menuReticleMask=0;
static float g_menuSize = 1.1f, g_menuDistance = 1.5f;
static std::string g_menuResolution = "auto";
static char g_menuStatus[96] = "HOLD Y FOR ONE SECOND TO OPEN";
static int g_menuPage = 0, g_menuRow = 0, g_menuGunHand = 1;
static uint64_t g_menuPulseUntil = 0, g_menuPauseDeadline = 0, g_menuYUntil = 0;
static mevr::MenuInputState g_menuInput;
static int g_menuWorldOffset = -1, g_menuPauserOffset = -1;
static int g_smoothFlagOffset = -1;
static uint32_t g_smoothFlagMask = 0;
static bool g_menuUnlimitedFailed = false;

enum MenuId {
    MHands, MArm, MGuns, MMelee, MParkour, MCrouch, MBalance, MPitch, MRoll, MYaw, MReticle,
    MFpsCap, MResolution, MLens, MFps, MRecenter, MGunPage, MSize, MDistance, MDefaults,
    MDebug, MLogging, MFast, MEx, MGunHand, MGunDown, MGunRight, MGunF, MGunR, MGunU,
    MController, MTurnMode, MTurnSpeed, MSnapAngle, MUiScale, MUiHeight, MPageControls,
    MBack, MDone, MConfirmReset, MPageHands, MPageComfort, MPageGraphics, MPageCalibration, MPageTrouble
};
struct MenuItem { MenuId id; const char* label; const char* help; };
static const MenuItem kMenuRoot[] = {
    {MPageHands,"HANDS AND MOVEMENT","CHOOSE YOUR MOTION CONTROL FEATURES"},
    {MPageControls,"CONTROLLERS AND TURNING","INPUT DEVICE, SNAP TURN AND SMOOTH TURN SPEED"},
    {MPageComfort,"COMFORT","CAMERA MOTION AND PHYSICAL CROUCHING"},
    {MPageGraphics,"GRAPHICS AND PERFORMANCE","FRAME CAP, RESOLUTION AND EFFECTS"},
    {MPageCalibration,"CALIBRATION AND MENU","RECENTER, GUN ALIGNMENT AND PANEL SIZE"},
    {MPageTrouble,"TROUBLESHOOTING","DIAGNOSTICS AND GRAPHICS COMPATIBILITY"},
    {MDone,"DONE","CLOSE SETTINGS AND RETURN TO THE GAME"}
};
static const MenuItem kMenuControls[] = {
    {MController,"CONTROLLER","AUTO SWITCHES ON INPUT. GAMEPAD DISABLES MOTION GESTURES"},
    {MTurnMode,"TURNING","SMOOTH OR ONE SNAP PER STICK DEFLECTION"},
    {MTurnSpeed,"SMOOTH SPEED","PERCENT OF THE ORIGINAL GAME SPEED. DEFAULT 100"},
    {MSnapAngle,"SNAP ANGLE","DEGREES PER TURN. RELEASE STICK TO CENTER TO REPEAT"},
    {MBack,"BACK","RETURN TO THE MAIN SETTINGS PAGE"}
};
static const MenuItem kMenuHands[] = {
    {MHands,"HAND TRACKING","GRIP MAKES A FIST. RIGHT STICK UP: JUMP; DOWN: TURN"},
    {MArm,"ARM SWING LOCOMOTION","INCLUDES RAISE BOTH HANDS TO JUMP"},
    {MGuns,"GUNS","TRACKED AIM, PICKUP AND THROW. SUPPORTED GUNS ONLY"},
    {MMelee,"MELEE","PHYSICAL PUNCHES AND TWO-HAND DISARM"},
    {MParkour,"MOTION PARKOUR","LEDGES, PIPES AND BARS WITH GUIDES AND CAMERA MOTION"},
    {MBack,"BACK","RETURN TO THE MAIN SETTINGS PAGE"}
};
static const MenuItem kMenuComfort[] = {
    {MCrouch,"PHYSICAL CROUCHING","REQUIRES ARM SWING. RECENTER WHILE STANDING"},
    {MBalance,"HEAD-TILT BEAM BALANCE","LEAN YOUR HEAD TO BALANCE ON NARROW BEAMS"},
    {MPitch,"LOCK ANIMATION PITCH","REMOVE ANIMATED UP/DOWN CAMERA ROTATION"},
    {MRoll,"LOCK ANIMATION ROLL","REMOVE ANIMATED CAMERA TILT"},
    {MYaw,"LOCK ANIMATION YAW","ADVANCED: CAN SEPARATE VIEW FROM BODY DIRECTION"},
    {MReticle,"RETICLE","SHOW THE NATIVE AIMING RETICLE. DEFAULT OFF"},
    {MBack,"BACK","RETURN TO THE MAIN SETTINGS PAGE"}
};
static const MenuItem kMenuGraphics[] = {
    {MFpsCap,"FRAME CAP","UNLIMITED REMOVES ENGINE CAP; XR STILL PACES FRAMES"},
    {MResolution,"RESOLUTION","RESTART REQUIRED. AUTO USES YOUR HEADSET"},
    {MLens,"LENS FLARES","RESTART REQUIRED"},
    {MFps,"FPS DISPLAY","SMALL HEADSET READOUT WITHOUT THE DEBUG OVERLAY"},
    {MUiScale,"GAME UI SIZE","TRAINING POP-UPS AND NATIVE MENUS. DEFAULT 65 PERCENT"},
    {MUiHeight,"GAME UI HEIGHT","MOVE NATIVE GAME UI UP OR DOWN. DEFAULT CENTERED"},
    {MBack,"BACK","RETURN TO THE MAIN SETTINGS PAGE"}
};
static const MenuItem kMenuCalibration[] = {
    {MRecenter,"RECENTER VIEW","LOOK FORWARD. STAND UP TO RESET CROUCH HEIGHT"},
    {MGunPage,"GUN ALIGNMENT","INDEPENDENT LEFT AND RIGHT GUN OFFSETS"},
    {MSize,"MENU WIDTH","METRES; DEFAULT 1.10"},
    {MDistance,"MENU DISTANCE","METRES; DEFAULT 1.50"},
    {MDefaults,"RESTORE SHIPPED DEFAULTS","REQUIRES CONFIRMATION. RESETS YOUR MEVR SETTINGS"},
    {MBack,"BACK","RETURN TO THE MAIN SETTINGS PAGE"}
};
static const MenuItem kMenuTrouble[] = {
    {MDebug,"DEBUG OVERLAY","DEVELOPMENT TEXT AND KEYBOARD HOTKEYS"},
    {MLogging,"DETAILED LOGGING","HAND, ARM SWING AND PARKOUR LOGGING"},
    {MFast,"FAST CAPTURE","GPU FRAME COPY. REQUIRES D3D9EX AT STARTUP"},
    {MEx,"D3D9EX","RESTART REQUIRED. OFF ALSO DISABLES FAST CAPTURE"},
    {MBack,"BACK","RETURN TO THE MAIN SETTINGS PAGE"}
};
static const MenuItem kMenuGun[] = {
    {MGunHand,"CALIBRATE HAND","CHOOSE LEFT OR RIGHT"},
    {MGunDown,"WRIST DOWN","DEGREES; ADJUST IN FIVE DEGREE STEPS"},
    {MGunRight,"WRIST RIGHT","DEGREES; ADJUST IN FIVE DEGREE STEPS"},
    {MGunF,"FORWARD OFFSET","MILLIMETRES; ADJUST IN FIVE MM STEPS"},
    {MGunR,"RIGHT OFFSET","MILLIMETRES; ADJUST IN FIVE MM STEPS"},
    {MGunU,"UP OFFSET","MILLIMETRES; ADJUST IN FIVE MM STEPS"},
    {MBack,"BACK","RETURN TO CALIBRATION"}
};
static const MenuItem kMenuReset[] = {
    {MBack,"CANCEL","KEEP YOUR CURRENT SETTINGS"},
    {MConfirmReset,"RESTORE DEFAULTS","REPLACE MEVR.INI WITH THE SHIPPED SETTINGS"}
};
static const MenuItem* MenuItems(int* count) {
#define MENU_PAGE(n, a) case n: *count = (int)(sizeof(a)/sizeof(a[0])); return a
    switch(g_menuPage) {
        MENU_PAGE(1,kMenuHands); MENU_PAGE(2,kMenuComfort); MENU_PAGE(3,kMenuGraphics);
        MENU_PAGE(4,kMenuCalibration); MENU_PAGE(5,kMenuTrouble); MENU_PAGE(6,kMenuGun);
        MENU_PAGE(7,kMenuReset); MENU_PAGE(8,kMenuControls); default: *count=7; return kMenuRoot;
    }
#undef MENU_PAGE
}
static void MenuMessage(const char* message) { strcpy_s(g_menuStatus,message); }
static bool MenuBlockedItem(MenuId id) {
    if(id==MArm || id==MGuns || id==MMelee || id==MParkour) return !g_motionHands||g_standardControllerActive;
    if(id==MCrouch) return !g_motionHands || !g_menuArm||g_standardControllerActive;
    if(id==MFast) return !g_devIsEx || !g_menuEx;
    if(id==MTurnSpeed)return g_snapTurning;
    if(id==MSnapAngle)return !g_snapTurning;
    return false;
}
static bool& MenuAnimPreference(int axis) {
    if(g_pkAnimSaved)return g_pkAnimPrev[axis];
    if(axis==0)return g_animFollow;
    if(axis==1)return g_animRollFollow;
    return g_animYawFollow;
}
static void MenuApplyFeatures() {
    const bool motion=g_motionHands&&!g_standardControllerActive;
    g_armSwing=motion && g_menuArm;
    g_armSwingJump=g_armSwing;
    g_pistolHands=motion && g_menuGuns;
    g_motionPunch=motion && g_menuMelee;
    g_parkour=motion && g_menuParkour;
    g_gripToGrip=motion;
    g_stickJumpTurn=motion;
}
static bool MenuParseNumber(const char* val,float lo,float hi,float* out) {
    char* end=nullptr; const float v=strtof(val,&end);
    if(end==val || *end || !std::isfinite(v) || v<lo || v>hi)return false;
    *out=v;return true;
}
static bool VrMenuLoadSetting(const char* key,const char* val) {
    bool b=false; float v=0;
    if(!_stricmp(key,"ShowReticle")){if(!SettingBool(val,&b))return false;g_showReticle=b;return true;}
    if(!_stricmp(key,"ControllerMode")) {
        const char* names[]={"auto","quest","gamepad"};for(int i=0;i<3;++i)if(!_stricmp(val,names[i])){g_controllerMode=i;return true;}return false;
    }
    if(!_stricmp(key,"TurnMode")) {if(!_stricmp(val,"smooth")){g_snapTurning=false;return true;}if(!_stricmp(val,"snap")){g_snapTurning=true;return true;}return false;}
    if(!_stricmp(key,"SmoothTurnSpeed")){if(!MenuParseNumber(val,.25f,2.f,&v))return false;g_smoothTurnSpeed=v;return true;}
    if(!_stricmp(key,"SnapTurnAngle")){if(!MenuParseNumber(val,15.f,90.f,&v))return false;g_snapTurnAngle=v;return true;}
    if(!_stricmp(key,"GameUIScale")){if(!MenuParseNumber(val,.4f,1.f,&v))return false;g_gameUiScale=v;return true;}
    if(!_stricmp(key,"GameUIHeight")){if(!MenuParseNumber(val,-.2f,.2f,&v))return false;g_gameUiHeight=v;return true;}
    if(_stricmp(key,"MenuWidth")==0 || _stricmp(key,"MenuDistance")==0) {
        const bool width=_stricmp(key,"MenuWidth")==0;
        if(MenuParseNumber(val,width?.7f:.8f,width?1.6f:2.5f,&v)) {
            (width?g_menuSize:g_menuDistance)=v;return true;
        }
        return false;
    }
    if(_stricmp(key,"ShowFPS")==0) { if(!SettingBool(val,&b))return false;g_menuFps=b;return true; }
    for(int h=0;h<2;++h)for(int axis=0;axis<5;++axis) {
        static const char* suffix[]={"WristDownDegrees","WristRightDegrees","ForwardMm","RightMm","UpMm"};
        char name[64];sprintf_s(name,"Gun%s%s",h==0?"Left":"Right",suffix[axis]);
        if(_stricmp(key,name)!=0)continue;
        const float limit=axis<2?90.0f:200.0f;
        if(!MenuParseNumber(val,-limit,limit,&v) || floorf(v)!=v)return false;
        if(axis==0)g_gunWristDownDeg[h]=(int)v;
        else if(axis==1)g_gunWristRightDeg[h]=(int)v;
        else g_gunPositionMm[h][axis-2]=(int)v;
        return true;
    }
    return false;
}
static void VrMenuInitializeSettings() {
    g_menuArm=g_armSwing;g_menuGuns=g_pistolHands;g_menuMelee=g_motionPunch;g_menuParkour=g_parkour;
    g_menuEx=g_wantEx;g_menuLens=g_lensFlares;
    if(g_resAuto)g_menuResolution="auto";
    else if(g_forceResW) {char v[48];sprintf_s(v,"%ux%u",g_forceResW,g_forceResH);g_menuResolution=v;}
    else g_menuResolution="off";
    MenuApplyFeatures();g_menuReady=true;
}
static bool MenuAtomicWrite(const std::string& text) {
    if(!g_settingsPath[0]) {MenuMessage("SAVE FAILED: NO SETTINGS PATH");return false;}
    wchar_t temp[MAX_PATH+48];swprintf_s(temp,L"%s.%lu.tmp",g_settingsPath,GetCurrentProcessId());
    HANDLE file=CreateFileW(temp,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) {MenuMessage("SAVE FAILED: MEVR.INI FOLDER IS NOT WRITABLE");return false;}
    DWORD wrote=0;
    bool ok=WriteFile(file,text.data(),(DWORD)text.size(),&wrote,nullptr)!=0 && wrote==text.size();
    if(ok)ok=FlushFileBuffers(file)!=0;
    CloseHandle(file);
    if(ok)ok=MoveFileExW(temp,g_settingsPath,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok) {DeleteFileW(temp);MenuMessage("SAVE FAILED: SETTINGS APPLY ONLY THIS SESSION");}
    else MenuMessage(g_menuRestart?"SAVED - RESTART REQUIRED FOR SOME CHANGES":"SAVED TO MEVR.INI");
    Log("[menu] settings save %s",ok?"OK":"FAILED");
    return ok;
}
static bool SaveMenuSettings() {
    if(!g_menuReady)return false;
    std::string source;
    HANDLE file=CreateFileW(g_settingsPath,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE) {
        LARGE_INTEGER length{};
        if(!GetFileSizeEx(file,&length) || length.QuadPart<0 || length.QuadPart>(1<<20)) {
            CloseHandle(file);MenuMessage("SAVE FAILED: INVALID INI SIZE");return false;
        }
        source.resize((size_t)length.QuadPart);DWORD got=0;
        const bool ok=ReadFile(file,source.data(),(DWORD)source.size(),&got,nullptr)!=0 && got==source.size();
        CloseHandle(file);if(!ok){MenuMessage("SAVE FAILED: COULD NOT READ MEVR.INI");return false;}
    } else if(GetLastError()==ERROR_FILE_NOT_FOUND)source=kVrShippedDefaults;
    else {MenuMessage("SAVE FAILED: COULD NOT READ MEVR.INI");return false;}
    std::vector<mevr::IniEdit> edits;
    auto boolean=[&](const char* k,bool b){edits.push_back({k,b?"on":"off"});};
    auto number=[&](const char* k,float v){char s[48];sprintf_s(s,"%.3g",(double)v);edits.push_back({k,s});};
    boolean("MotionHands",g_motionHands);boolean("ArmSwing",g_menuArm);boolean("ArmSwingJump",true);
    boolean("GripToGrip",g_motionHands);boolean("StickJumpTurn",g_motionHands);
    boolean("PistolHands",g_menuGuns);boolean("MotionPunch",g_menuMelee);boolean("Parkour",g_menuParkour);
    boolean("ArmSwingCrouch",g_armSwingCrouch);boolean("BalanceHeadRoll",g_balanceRoll);
    boolean("LockAnimPitch",!MenuAnimPreference(0));boolean("LockAnimRoll",!MenuAnimPreference(1));boolean("LockAnimYaw",!MenuAnimPreference(2));
    edits.push_back({"FrameCap",g_fpsCap==0?"unlimited":std::to_string((int)g_fpsCap)});
    edits.push_back({"Resolution",g_menuResolution});boolean("LensFlares",g_menuLens);boolean("D3D9Ex",g_menuEx);
    boolean("FastCapture",g_fastCapture);boolean("ShowFPS",g_menuFps);boolean("Debug",g_debug && g_overlay);
    boolean("MotionHandsDebug",g_motionHandsDebug);boolean("ArmSwingDebug",g_armSwingDebug);boolean("ParkourDebug",g_parkourDebug);
    number("MenuWidth",g_menuSize);number("MenuDistance",g_menuDistance);
    edits.push_back({"ControllerMode",g_controllerMode==0?"auto":g_controllerMode==1?"quest":"gamepad"});
    edits.push_back({"TurnMode",g_snapTurning?"snap":"smooth"});
    number("SmoothTurnSpeed",g_smoothTurnSpeed);number("SnapTurnAngle",g_snapTurnAngle);
    number("GameUIScale",g_gameUiScale);number("GameUIHeight",g_gameUiHeight);
    boolean("ShowReticle",g_showReticle);
    for(int h=0;h<2;++h)for(int a=0;a<5;++a) {
        static const char* suffix[]={"WristDownDegrees","WristRightDegrees","ForwardMm","RightMm","UpMm"};
        char name[64];sprintf_s(name,"Gun%s%s",h==0?"Left":"Right",suffix[a]);
        number(name,(float)(a==0?g_gunWristDownDeg[h]:a==1?g_gunWristRightDeg[h]:g_gunPositionMm[h][a-2]));
    }
    return MenuAtomicWrite(mevr::updateIni(source,edits));
}

// Resolve reflection metadata once on the existing discovery thread, not in Present.
static void VrMenuResolveEngine() {
    // Actor's Children chain is unavailable in this cooked build. Walk the
    // object table once and validate each property's declaring class instead.
    // Smoothing belongs to GameEngine, not its Engine base class.
    g_menuWorldOffset=g_menuPauserOffset=g_smoothFlagOffset=-1;g_smoothFlagMask=0;
    g_menuHudOffset=g_menuReticleOffset=-1;g_menuReticleMask=0;
    uint32_t data=0,count=0;
    if(g_offOuter<0 || g_offPropOff<0 || g_offBoolMask<0 ||
       !SafeU32(g_gobjAddr,&data) || !SafeU32(g_gobjAddr+4,&count) || count>2000000)return;
    for(uint32_t i=0;i<count;++i) {
        uint32_t obj=0,outer=0,cls=0,ownerClass=0,offset=0;
        char name[64]{},owner[64]{},kind[48]{};
        if(!SafeU32(data+i*4,&obj) || obj<0x10000 || !ReadObjName(obj,name,sizeof(name)) ||
           (strcmp(name,"WorldInfo") && strcmp(name,"Pauser") && strcmp(name,"bSmoothFrameRate")&&
            strcmp(name,"myHUD")&&strcmp(name,"bDisableDrawCrossHair")))continue;
        if(!SafeU32(obj+g_offOuter,&outer) || !ReadObjName(outer,owner,sizeof(owner)) ||
           !SafeU32(outer+kUObjectClassOff,&ownerClass) || !ObjNameIs(ownerClass,"Class") ||
           !SafeU32(obj+kUObjectClassOff,&cls) || !ReadObjName(cls,kind,sizeof(kind)) ||
           !SafeU32(obj+g_offPropOff,&offset) || offset>=0x8000)continue;
        if(!strcmp(name,"WorldInfo") && !strcmp(owner,"Actor") && !strcmp(kind,"ObjectProperty"))g_menuWorldOffset=(int)offset;
        if(!strcmp(name,"Pauser") && !strcmp(owner,"WorldInfo") && !strcmp(kind,"ObjectProperty"))g_menuPauserOffset=(int)offset;
        if(!strcmp(name,"myHUD")&&!strcmp(owner,"PlayerController")&&!strcmp(kind,"ObjectProperty"))g_menuHudOffset=(int)offset;
        if(!strcmp(name,"bDisableDrawCrossHair")&&!strcmp(owner,"TdSPHUD")&&!strcmp(kind,"BoolProperty")){
            g_menuReticleOffset=(int)offset;SafeU32(obj+g_offBoolMask,&g_menuReticleMask);
        }
        if(!strcmp(name,"bSmoothFrameRate") && !strcmp(owner,"GameEngine") && !strcmp(kind,"BoolProperty")) {
            g_smoothFlagOffset=(int)offset;SafeU32(obj+g_offBoolMask,&g_smoothFlagMask);
        }
    }
    if(!g_smoothFlagMask || (g_smoothFlagMask&(g_smoothFlagMask-1)))g_smoothFlagMask=0;
    if(!g_menuReticleMask||(g_menuReticleMask&(g_menuReticleMask-1)))g_menuReticleMask=0;
    Log("[hud] reticle metadata HUD=%d flag=%d mask=%08X",g_menuHudOffset,g_menuReticleOffset,g_menuReticleMask);
    Log("[menu] pause world=%d pauser=%d; frame smoothing offset=%d mask=%08X",
        g_menuWorldOffset,g_menuPauserOffset,g_smoothFlagOffset,g_smoothFlagMask);
}
static void VrMenuReticleTick() {
    // HUD visibility must work without combat hooks, hand tracking or Quest input.
    uint32_t hud=0,bits=0;
    if(!g_playerCtl||g_menuHudOffset<0||g_menuReticleOffset<0||!g_menuReticleMask||
       !SafeU32(g_playerCtl+g_menuHudOffset,&hud)||!LooksLikeRigObject(hud,"TdSPHUD")||
       !SafeU32(hud+g_menuReticleOffset,&bits))return;
    const uint32_t wanted=g_showReticle?bits&~g_menuReticleMask:bits|g_menuReticleMask;
    if(wanted==bits)return;
    SIZE_T wrote=0;
    if(WriteProcessMemory(GetCurrentProcess(),(void*)(hud+g_menuReticleOffset),&wanted,4,&wrote)&&wrote==4)
        Log("[hud] native aiming reticle %s",g_showReticle?"enabled":"disabled");
}
static int MenuPaused() {
    if(!g_playerCtl)return -1;
    uint32_t world=0,pauser=0;
    if(g_menuWorldOffset<0 || g_menuPauserOffset<0 ||
       !SafeU32(g_playerCtl+g_menuWorldOffset,&world) || !world ||
       !SafeU32(world+g_menuPauserOffset,&pauser))return -1;
    return pauser?1:0;
}
static void VrMenuFrameCapTick() {
    if(!g_engineObj || g_smoothFlagOffset<0 || !g_smoothFlagMask) {
        if(g_fpsCap==0)g_menuUnlimitedFailed=true;
        return;
    }
    uint32_t current=0;
    if(!SafeU32(g_engineObj+g_smoothFlagOffset,&current))return;
    const uint32_t wanted=g_fpsCap==0?current&~g_smoothFlagMask:current|g_smoothFlagMask;
    SIZE_T wrote=0;
    const bool ok=current==wanted || (WriteProcessMemory(GetCurrentProcess(),
        (void*)(g_engineObj+g_smoothFlagOffset),&wanted,sizeof(wanted),&wrote) && wrote==sizeof(wanted));
    g_menuUnlimitedFailed=g_fpsCap==0 && !ok;
}
static void MenuResetGestures() {
    ArmSwingCollapse("VR menu");
    g_swingL={};g_swingR={};g_swingJumpAsserted=false;g_swingJumpArmed=false;
    g_swingJumpSince=g_swingJumpGraceUntil=0;g_swingCrouching=false;
    g_pkAnchor=PK_HAND_NONE;g_pkHavePrevLoc=false;g_pkLeadPrevOk=false;g_pkAnchorRelOk=false;
    g_pkHoldGripped=false;g_pkHoldEmpty=0;g_pkHoldDrop=0;g_pkSwipeHold=0;g_pkPullUpHold=0;g_pkBarJumpHold=0;
    g_pkHandAnchored[0]=g_pkHandAnchored[1]=false;
    g_pkDirectHave=false;g_pkDirectLive=false;
    g_pkShimmy=g_pkBarShimmy=g_pkPumpStick=0;
    g_gripValue[0]=g_gripValue[1]=0;
    g_padSentLX=g_padSentLY=g_padSentRX=0;
    g_stickJump.asserted=g_stickTurn.asserted=false;
    g_stickJump.armed=g_stickTurn.armed=false;
    g_combatGripDetector.reset();
    InterlockedExchange(&g_combatGripAcquiredWeapon,0); // require a fresh squeeze before release-to-drop
    InterlockedExchange(&g_combatHeldMask,0);
    AcquireSRWLockExclusive(&g_combatRequestLock);g_combatRequestCount=0;ReleaseSRWLockExclusive(&g_combatRequestLock);
}
static void MenuFinishClose() {
    g_menuOpen=false;g_menuClosing=false;g_menuOwnsPause=false;g_menuPausePending=false;
    g_menuInput.quarantine();MenuResetGestures();
}
static void MenuClose() {
    if(g_menuPausePending) {MenuMessage("WAITING FOR THE GAME TO PAUSE");return;}
    if(g_menuOwnsPause && MenuPaused()==1) {
        g_menuClosing=true;g_menuPulseUntil=GetTickCount64()+120;g_menuPauseDeadline=GetTickCount64()+2500;
        MenuMessage("RESUMING GAME");
    } else MenuFinishClose();
}
static void MenuOpen(uint64_t now) {
    InterlockedExchange(&g_menuBlocksGameplay,1);
    g_menuOpen=true;g_menuClosing=false;g_menuPage=g_menuRow=0;
    g_menuYUntil=0;MenuResetGestures();
    const int paused=MenuPaused();
    g_menuOwnsPause=false;g_menuPausePending=paused==0;
    if(g_menuPausePending) {
        g_menuPulseUntil=now+120;g_menuPauseDeadline=now+2500;MenuMessage("PAUSING GAME");
    } else MenuMessage(paused==1?"GAME WAS ALREADY PAUSED":"PAUSE UNAVAILABLE - GAME MAY STILL BE RUNNING");
    Log("[menu] opened; previous pause state %d",paused);
}
static void MenuRestoreDefaults() {
    if(!MenuAtomicWrite(kVrShippedDefaults))return;
    // Restart-only and hidden experimental settings take effect next launch.
    g_motionHands=true;g_menuArm=true;g_menuGuns=g_menuMelee=g_menuParkour=true;
    g_armSwingCrouch=g_balanceRoll=true;
    MenuAnimPreference(0)=MenuAnimPreference(1)=MenuAnimPreference(2)=true;
    g_fpsCap=72;g_menuResolution="auto";g_menuLens=false;g_menuEx=true;g_fastCapture=true;
    g_debug=false;g_overlay=true;g_motionHandsDebug=g_armSwingDebug=g_parkourDebug=false;
    g_menuSize=1.1f;g_menuDistance=1.5f;g_menuFps=false;
    g_controllerMode=0;g_snapTurning=false;g_smoothTurnSpeed=1;g_snapTurnAngle=45;
    g_gameUiScale=.65f;g_gameUiHeight=0;
    g_showReticle=false;
    g_gunWristDownDeg[0]=g_gunWristDownDeg[1]=40;
    g_gunWristRightDeg[0]=g_gunWristRightDeg[1]=-10;
    memset(g_gunPositionMm,0,sizeof(g_gunPositionMm));
    g_gunPositionMm[0][1]=40;
    MenuApplyFeatures();g_menuRestart=true;g_menuPage=4;g_menuRow=0;
    MenuMessage("DEFAULTS SAVED - RESTART TO APPLY ALL SETTINGS");
}
static void MenuActivate(MenuId id,int delta) {
    const bool activate=delta==0;
    if(activate)delta=1;
    const auto choose=[&](bool current){return activate?!current:delta>0;};
    if(MenuBlockedItem(id)){MenuMessage("ENABLE THE REQUIRED FEATURE FIRST");return;}
    if(id>=MPageHands && id<=MPageTrouble){g_menuPage=1+(id-MPageHands);g_menuRow=0;return;}
    if(id==MBack){g_menuPage=g_menuPage==6||g_menuPage==7?4:0;g_menuRow=0;return;}
    if(id==MPageControls){g_menuPage=8;g_menuRow=0;return;}
    if(id==MDone){MenuClose();return;}
    if(id==MGunPage){g_menuPage=6;g_menuRow=0;return;}
    if(id==MDefaults){g_menuPage=7;g_menuRow=0;return;}
    if(id==MConfirmReset){MenuRestoreDefaults();return;}
    if(id==MRecenter){RecenterSixDof();MenuMessage("VIEW RECENTERED");return;}
    if(id==MGunHand){g_menuGunHand=activate?1-g_menuGunHand:delta>0?1:0;return;}
    switch(id) {
        case MController:g_controllerMode=(g_controllerMode+delta+3)%3;break;
        case MTurnMode:g_snapTurning=choose(g_snapTurning);InterlockedExchange(&g_pendingSnapYaw,0);break;
        case MTurnSpeed:g_smoothTurnSpeed=(std::max)(.25f,(std::min)(2.f,g_smoothTurnSpeed+delta*.25f));break;
        case MSnapAngle:g_snapTurnAngle=(std::max)(15.f,(std::min)(90.f,g_snapTurnAngle+delta*15.f));break;
        case MUiScale:g_gameUiScale=(std::max)(.4f,(std::min)(1.f,g_gameUiScale+delta*.05f));break;
        case MUiHeight:g_gameUiHeight=(std::max)(-.2f,(std::min)(.2f,g_gameUiHeight+delta*.025f));break;
        case MHands:g_motionHands=choose(g_motionHands);break;
        case MArm:g_menuArm=choose(g_menuArm);break;
        case MGuns:g_menuGuns=choose(g_menuGuns);break;
        case MMelee:g_menuMelee=choose(g_menuMelee);break;
        case MParkour:g_menuParkour=choose(g_menuParkour);break;
        case MCrouch:g_armSwingCrouch=choose(g_armSwingCrouch);break;
        case MBalance:g_balanceRoll=choose(g_balanceRoll);break;
        case MPitch:MenuAnimPreference(0)=!choose(!MenuAnimPreference(0));break;
        case MRoll:MenuAnimPreference(1)=!choose(!MenuAnimPreference(1));break;
        case MYaw:MenuAnimPreference(2)=!choose(!MenuAnimPreference(2));break;
        case MReticle:g_showReticle=choose(g_showReticle);break;
        case MFps:g_menuFps=choose(g_menuFps);break;
        case MFpsCap: {
            static const float values[]={30,36,60,72,90,120,144,0};int index=-1;
            for(int i=0;i<8;++i)if(g_fpsCap==values[i])index=i;
            const float next=values[(index+delta+8)%8];
            if(next==0 && !g_smoothFlagMask){MenuMessage("UNLIMITED UNAVAILABLE: ENGINE LIMITER NOT FOUND");return;}
            g_fpsCap=next;break;
        }
        case MResolution: {
            static const char* values[]={"off","auto","1920x1080","2560x1440","3200x1800","3840x2160","4224x2376"};
            int index=0;for(int i=0;i<7;++i)if(g_menuResolution==values[i])index=i;
            g_menuResolution=values[(index+delta+7)%7];g_menuRestart=true;break;
        }
        case MLens:g_menuLens=choose(g_menuLens);g_menuRestart=true;break;
        case MEx:g_menuEx=choose(g_menuEx);g_menuRestart=true;break;
        case MFast:g_fastCapture=choose(g_fastCapture);break;
        case MDebug:g_debug=choose(g_debug&&g_overlay);g_overlay=g_debug;break;
        case MLogging:g_motionHandsDebug=g_armSwingDebug=g_parkourDebug=choose(g_motionHandsDebug||g_armSwingDebug||g_parkourDebug);break;
        case MSize:g_menuSize=(std::max)(.7f,(std::min)(1.6f,g_menuSize+delta*.1f));break;
        case MDistance:g_menuDistance=(std::max)(.8f,(std::min)(2.5f,g_menuDistance+delta*.1f));break;
        case MGunDown:case MGunRight:case MGunF:case MGunR:case MGunU: {
            int* value=id==MGunDown?&g_gunWristDownDeg[g_menuGunHand]:id==MGunRight?&g_gunWristRightDeg[g_menuGunHand]:
                &g_gunPositionMm[g_menuGunHand][id-MGunF];const int limit=id<=MGunRight?90:200;
            *value=(std::max)(-limit,(std::min)(limit,*value+delta*5));break;
        }
        default:return;
    }
    MenuApplyFeatures();SaveMenuSettings();
}
static void MenuValue(MenuId id,char* out,size_t cap) {
    const char* word=nullptr;bool b=false;bool boolean=true;
    switch(id) {
        case MHands:b=g_motionHands;break;case MArm:b=g_menuArm;break;
        case MGuns:b=g_menuGuns;break;case MMelee:b=g_menuMelee;break;case MParkour:b=g_menuParkour;break;
        case MCrouch:b=g_armSwingCrouch;break;case MBalance:b=g_balanceRoll;break;
        case MReticle:b=g_showReticle;break;
        case MPitch:b=!MenuAnimPreference(0);break;case MRoll:b=!MenuAnimPreference(1);break;case MYaw:b=!MenuAnimPreference(2);break;
        case MLens:b=g_menuLens;break;case MFps:b=g_menuFps;break;case MEx:b=g_menuEx;break;case MFast:b=g_fastCapture;break;
        case MDebug:b=g_debug&&g_overlay;break;case MLogging:b=g_motionHandsDebug||g_armSwingDebug||g_parkourDebug;break;
        default:boolean=false;break;
    }
    if(boolean){sprintf_s(out,cap,"%s%s",b?"ON":"OFF",MenuBlockedItem(id)?" (INACTIVE)":"");return;}
    switch(id) {
        case MController:word=g_controllerMode==0?"AUTO":g_controllerMode==1?"QUEST":"GAMEPAD";break;
        case MTurnMode:word=g_snapTurning?"SNAP":"SMOOTH";break;
        case MTurnSpeed:sprintf_s(out,cap,"%.0f%%",g_smoothTurnSpeed*100.);break;
        case MSnapAngle:sprintf_s(out,cap,"%.0f DEG",(double)g_snapTurnAngle);break;
        case MUiScale:sprintf_s(out,cap,"%.0f%%",g_gameUiScale*100.);break;
        case MUiHeight:sprintf_s(out,cap,"%+.0f%%",g_gameUiHeight*100.);break;
        case MFpsCap:if(g_fpsCap==0)word=g_menuUnlimitedFailed?"UNAVAILABLE":"UNLIMITED";else sprintf_s(out,cap,"%.0f",(double)g_fpsCap);break;
        case MResolution:word=g_menuResolution.c_str();break;
        case MSize:sprintf_s(out,cap,"%.2f M",(double)g_menuSize);break;
        case MDistance:sprintf_s(out,cap,"%.2f M",(double)g_menuDistance);break;
        case MGunHand:word=g_menuGunHand==0?"LEFT":"RIGHT";break;
        case MGunDown:sprintf_s(out,cap,"%+d DEG",g_gunWristDownDeg[g_menuGunHand]);break;
        case MGunRight:sprintf_s(out,cap,"%+d DEG",g_gunWristRightDeg[g_menuGunHand]);break;
        case MGunF:case MGunR:case MGunU:sprintf_s(out,cap,"%+d MM",g_gunPositionMm[g_menuGunHand][id-MGunF]);break;
        default:word="";break;
    }
    if(word)strcpy_s(out,cap,word);
}

static bool VrMenuInputTick(XrTime, bool focused,bool y,bool a,bool b,bool other,float x,float z,bool* shortY) {
    const uint64_t now=GetTickCount64();
    const mevr::MenuInput input{focused,y,a,b,other,x,z};
    const auto events=g_menuInput.sample(now,input,g_menuOpen);
    if(events.open)MenuOpen(now);
    if(events.close && !g_menuClosing)MenuClose();
    if(events.shortY)g_menuYUntil=now+100;
    if(g_menuOpen && !g_menuClosing) {
        int count=0;const auto* items=MenuItems(&count);
        if(events.back) {if(g_menuPage==0)MenuClose();else {g_menuPage=g_menuPage==6||g_menuPage==7?4:0;g_menuRow=0;}}
        else if(events.row)g_menuRow=(g_menuRow+events.row+count)%count;
        else if(events.accept)MenuActivate(items[g_menuRow].id,0);
        else if(events.value) {
            const auto id=items[g_menuRow].id;
            if(id<=MFps || id==MSize || id==MDistance || (id>=MDebug && id<=MGunU) || (id>=MController&&id<=MUiHeight))MenuActivate(id,events.value);
        }
    }
    if(g_menuPausePending || g_menuClosing) {
        const int paused=MenuPaused();
        if(g_menuPausePending && paused==1) {
            g_menuPausePending=false;g_menuOwnsPause=true;MenuMessage("GAME PAUSED - CHANGES SAVE AUTOMATICALLY");
        } else if(g_menuClosing && paused==0)MenuFinishClose();
        else if(now>=g_menuPauseDeadline) {
            g_menuPausePending=false;g_menuClosing=false;
            MenuMessage("PAUSE DID NOT CONFIRM - DONE TO CLOSE OR RETRY");
        }
    }
    const bool blocked=g_menuOpen || g_menuInput.waitNeutral || !focused || now<g_menuPulseUntil;
    InterlockedExchange(&g_menuBlocksGameplay,blocked?1:0);
    if(blocked) {
        MEVR_XINPUT_STATE state{};
        if(focused && now<g_menuPulseUntil)state.Gamepad.wButtons=MEVR_PAD_START;
        EnterCriticalSection(&g_padLock);state.dwPacketNumber=g_pad.dwPacketNumber+1;g_pad=state;LeaveCriticalSection(&g_padLock);
        g_padSentLX=g_padSentLY=g_padSentRX=0;
    }
    *shortY=!blocked && now<g_menuYUntil;
    return blocked;
}

// CPU-rasterized version of the existing debug font, uploaded only when text changes.
// Independent swapchain: never split across eyes or scaled with game resolution.
static XrSwapchain g_menuSwap=XR_NULL_HANDLE;
static std::vector<ID3D11Texture2D*> g_menuImages;
static ID3D11Texture2D* g_menuUpload=nullptr;
static const int kMenuW=1200,kMenuH=1000;
static std::vector<uint32_t> g_menuPixels;
static std::string g_menuLastImage;
static void MenuRect(int x,int y,int w,int h,uint32_t colour) {
    for(int row=(std::max)(0,y);row<(std::min)(kMenuH,y+h);++row)
        for(int col=(std::max)(0,x);col<(std::min)(kMenuW,x+w);++col)g_menuPixels[row*kMenuW+col]=colour;
}
static void MenuText(int x,int y,const char* text,int scale,uint32_t colour) {
    for(int i=0;text[i];++i) {
        const auto* glyph=kFont[GlyphIndex(text[i])];
        for(int r=0;r<7;++r)for(int c=0;c<5;++c)
            if(glyph[r]&(0x10>>c))MenuRect(x+i*6*scale+c*scale,y+r*scale,scale,scale,colour);
    }
}
static bool MenuEnsureSwapchain() {
    if(g_menuSwap && g_menuUpload)return true;
    if(!g_dev11 || !g_xrSession)return false;
    if(g_menuSwap){xrDestroySwapchain(g_menuSwap);g_menuSwap=XR_NULL_HANDLE;g_menuImages.clear();}
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format=g_scFormat;info.sampleCount=1;info.width=kMenuW;info.height=kMenuH;info.faceCount=info.arraySize=info.mipCount=1;
    if(XR_FAILED(xrCreateSwapchain(g_xrSession,&info,&g_menuSwap)))return false;
    uint32_t count=0;
    if(XR_FAILED(xrEnumerateSwapchainImages(g_menuSwap,0,&count,nullptr)) || !count)return false;
    std::vector<XrSwapchainImageD3D11KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if(XR_FAILED(xrEnumerateSwapchainImages(g_menuSwap,count,&count,(XrSwapchainImageBaseHeader*)images.data())))return false;
    g_menuImages.clear();for(auto& i:images)g_menuImages.push_back(i.texture);
    D3D11_TEXTURE2D_DESC desc{};desc.Width=kMenuW;desc.Height=kMenuH;desc.MipLevels=desc.ArraySize=1;
    desc.Format=(DXGI_FORMAT)g_scFormat;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
    if(FAILED(g_dev11->CreateTexture2D(&desc,nullptr,&g_menuUpload)))return false;
    g_menuPixels.resize(kMenuW*kMenuH);g_menuLastImage.clear();return true;
}
static void MenuRasterize(int count,const MenuItem* items,const char values[][64],const char* fpsText) {
        const bool bgra=g_scFormat==DXGI_FORMAT_B8G8R8A8_UNORM || g_scFormat==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const uint32_t green=bgra?0xFF00FF60:0xFF60FF00,black=0xFF000000,dim=bgra?0xFF007030:0xFF307000;
        std::fill(g_menuPixels.begin(),g_menuPixels.end(),black);
        if(g_menuOpen) {
            static const char* titles[]={"MIRROR'S EDGE VR","HANDS AND MOVEMENT","COMFORT","GRAPHICS AND PERFORMANCE",
                "CALIBRATION AND MENU","TROUBLESHOOTING","GUN ALIGNMENT","RESTORE DEFAULTS?","CONTROLLERS AND TURNING"};
            MenuText(40,40,titles[g_menuPage],5,green);MenuRect(40,98,1120,3,green);
            for(int i=0;i<count;++i) {
                const int y=140+i*82;const bool selected=i==g_menuRow;
                if(selected)MenuRect(28,y-16,1144,68,green);
                const uint32_t ink=selected?black:MenuBlockedItem(items[i].id)?dim:green;
                MenuText(48,y,items[i].label,4,ink);
                MenuText(1144-(int)strlen(values[i])*24,y,values[i],4,ink);
            }
            MenuRect(40,747,1120,2,green);
            // Wrap help at a word boundary; three-pixel glyphs fit 62 characters.
            const MenuId selectedId=items[g_menuRow].id;
            std::string help=items[g_menuRow].help;
            if(MenuBlockedItem(selectedId)) {
                if(selectedId==MTurnSpeed)help="SELECT SMOOTH TURNING TO ADJUST ITS SPEED.";
                else if(selectedId==MSnapAngle)help="SELECT SNAP TURNING TO ADJUST ITS ANGLE.";
                else if(g_standardControllerActive&&selectedId!=MFast)help="MOTION FEATURES RESUME WHEN YOU SWITCH BACK TO QUEST.";
                else help="INACTIVE: ENABLE THE PARENT FEATURE FIRST. YOUR CHOICE IS REMEMBERED.";
            }
            size_t split=help.size()>61?help.rfind(' ',61):std::string::npos;
            if(split!=std::string::npos){MenuText(40,774,help.substr(0,split).c_str(),3,green);MenuText(40,805,help.substr(split+1).c_str(),3,green);}
            else MenuText(40,774,help.c_str(),3,green);
            MenuText(40,861,g_menuStatus,2,green);
            MenuText(40,909,"STICK: SELECT / ADJUST   A / TRIGGER: CONFIRM",3,green);
            MenuText(40,949,"B: BACK   HOLD Y: CLOSE",3,green);MenuText(960,949,fpsText,3,green);
        } else MenuText(20,20,fpsText,5,green);
}
static bool VrMenuLayer(XrCompositionLayerQuad* layer) {
    static double fpsAt=0;static int fpsFrames=0;static float fps=0;
    const double now=NowMs();++fpsFrames;
    if(now-fpsAt>=500){fps=(float)(fpsFrames*1000.0/(now-fpsAt));fpsFrames=0;fpsAt=now;}
    if(!g_menuOpen && !g_menuFps)return false;
    if(!MenuEnsureSwapchain())return false;
    int count=0;const auto* items=MenuItems(&count);
    std::string signature=g_menuStatus;signature+=std::to_string(g_menuPage)+":"+std::to_string(g_menuRow);
    signature+=g_menuOpen?"OPEN":"FPS";
    char fpsText[48];sprintf_s(fpsText,"%.0f FPS",(double)fps);signature+=fpsText;
    char values[8][64]{};for(int i=0;i<count;++i){MenuValue(items[i].id,values[i],sizeof(values[i]));signature+=values[i];}
    if(signature!=g_menuLastImage) {
        MenuRasterize(count,items,values,fpsText);
        g_ctx11->UpdateSubresource(g_menuUpload,0,nullptr,g_menuPixels.data(),kMenuW*4,0);
        g_menuLastImage=signature;
    }
    uint32_t index=0;XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if(XR_FAILED(xrAcquireSwapchainImage(g_menuSwap,&ai,&index)))return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wi.timeout=XR_INFINITE_DURATION;
    const bool ready=XR_SUCCEEDED(xrWaitSwapchainImage(g_menuSwap,&wi));
    if(ready && index<g_menuImages.size())g_ctx11->CopyResource(g_menuImages[index],g_menuUpload);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const bool released=XR_SUCCEEDED(xrReleaseSwapchainImage(g_menuSwap,&ri));
    if(!ready || !released)return false;
    *layer={XR_TYPE_COMPOSITION_LAYER_QUAD};layer->space=g_viewSpace;layer->eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    layer->pose.orientation.w=1;layer->subImage.swapchain=g_menuSwap;
    if(g_menuOpen) {
        layer->pose.position={0,-.10f,-g_menuDistance};layer->size={g_menuSize,g_menuSize*kMenuH/kMenuW};
        layer->subImage.imageRect={{0,0},{kMenuW,kMenuH}};
    } else {
        layer->pose.position={.45f,-.4f,-1.5f};layer->size={.22f,.065f};
        layer->subImage.imageRect={{0,0},{300,85}};
    }
    return true;
}
