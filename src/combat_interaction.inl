// Game calls and UObject writes below run only from Update1pArms / ProcessInternal.
// The input thread publishes small requests; it never calls ProcessEvent.
static bool g_combatInteractionReady = false;
static volatile LONG g_combatHand = 1, g_combatGripAcquiredWeapon = 0, g_combatHeldMask = 0;
static bool g_combatGripBlocksPunch = false; // input-thread only
static uintptr_t g_combatMeleeMove = 0;
static uintptr_t g_vrZiplineMove=0;
static bool g_vrZiplineViewOverride=false,g_vrZiplineSavedConstraint=false;
static uintptr_t g_combatLeftBoneWeapon = 0;
static long g_combatLeftBoneFrame = -100;

struct CombatField { const char* owner; const char* name; const char* kind; int offset=-1; uint32_t mask=0; };
enum CombatFieldId { CInvManager, CAgainstWall, CMoveGroup, CAllowPickup, CCinematic,
    CWorld, CPauser, CActorVelocity, CHidden, CTarget, CMinRange, CMaxRange, CMaxAngle,
    CLeft, CComboCounter, CComboQueued, CPickupClass, CPickupInventory, CBasicFieldCount,
    CDeleted=CBasicFieldCount, CFade, CCanPickupInventory, CPickupInstigator, CPickMesh, CPickBounds, CFieldCount,
    CZipAssist=CFieldCount, CZipConstrain, CMyHUD, CDisableCrosshair, CAllFieldCount };
static CombatField g_combatFields[CAllFieldCount] = {
    {"Pawn","InvManager","ObjectProperty"}, {"TdPawn","AgainstWallState","ByteProperty"},
    {"TdMove","MovementGroup","ByteProperty"}, {"TdMove","bAllowPickup","BoolProperty"},
    {"PlayerController","bCinematicMode","BoolProperty"}, {"Actor","WorldInfo","ObjectProperty"},
    {"WorldInfo","Pauser","ObjectProperty"}, {"Actor","Velocity","StructProperty"},
    {"Actor","bHidden","BoolProperty"}, {"TdPlayerController","TargetPawn","ObjectProperty"},
    {"TdPlayerController","CloseCombatMinRange","FloatProperty"},
    {"TdPlayerController","CloseCombatMaxRange","FloatProperty"},
    {"TdPlayerController","CloseCombatMaxAngle","FloatProperty"},
    {"TdMove_Melee","bLeft","BoolProperty"}, {"TdMove_Melee","ComboCounter","IntProperty"},
    {"TdMove_Melee","ComboQueuedActions","IntProperty"},
    {"DroppedPickup","InventoryClass","ClassProperty"}, {"DroppedPickup","Inventory","ObjectProperty"},
    {"Actor","bDeleteMe","BoolProperty"}, {"DroppedPickup","bFadeOut","BoolProperty"},
    {"Pawn","bCanPickupInventory","BoolProperty"}, {"Actor","Instigator","ObjectProperty"},
    {"TdPickup","PickMesh","ComponentProperty"}, {"PrimitiveComponent","Bounds","StructProperty"},
    // Optional camera fields: failures must not disable weapon interactions.
    {"TdMove_ZipLine","bZipLineLookAssist","BoolProperty"}, {"TdMove","bConstrainLook","BoolProperty"},
    {"PlayerController","myHUD","ObjectProperty"}, {"TdSPHUD","bDisableDrawCrossHair","BoolProperty"}
};
struct CombatFunction { const char* owner; const char* name; uintptr_t fn=0; };
enum CombatFunctionId { CIgnore, CFindPickup, CPickup, CCanDrop, CCanThrow, CJoint,
    CDropFrom, CRemove, CLOIOff, CAttack, CTrigger, CStopMelee, CGetTarget, CCanDisarm,
    CHandleAction, CBasicFunctionCount, CAmmo=CBasicFunctionCount, CState, CFastTrace, CBlueBox,
    CFreeSlot, CSetPickupAmmo, CMeshPosition, CRootBody, CBodyTransform, CFunctionCount,
    CZipView=CFunctionCount, CAbortLook, CWeaponStartFire, CAllFunctionCount };
static CombatFunction g_combatFunctions[CAllFunctionCount] = {
    {"TdPlayerController","IsButtonInputIgnored"}, {"TdInventoryManager","FindNearbyPickup"},
    {"TdInventoryManager","TryToPickUpWeapon"}, {"TdPawn","CanDropWeapon"},
    {"Weapon","CanThrow"}, {"TdPawn","GetWeaponJointPosition"},
    {"TdWeapon","DropFromEx"}, {"TdPawn","RemoveWeaponAfterDrop"},
    {"TdPawn","WeaponLOINotifyOFF"}, {"TdPlayerController","AttackPress"},
    {"TdMove_Melee","TriggerMove"}, {"TdMove_Melee","StopMove"},
    {"TdPlayerController","GetHumanTarget"}, {"TdMOVE_Disarm","CanDoMove"},
    {"TdPawn","HandleMoveAction"}, {"TdPickup","GetAmmoCount"}, {"Object","GetStateName"},
    {"Actor","FastTrace"}, {"Actor","DrawDebugBoxTime"}, {"TdInventoryManager","FindFreeSlotForWeaponClass"},
    {"TdPickup","SetAmmoCount"}, {"PrimitiveComponent","GetPosition"},
    {"PrimitiveComponent","GetRootBodyInstance"}, {"RB_BodyInstance","GetUnrealWorldTM"},
    {"TdMove_ZipLine","UpdateViewRotation"}, {"TdMove","AbortLookAtTarget"},
    {"TdWeapon","StartFire"}
};
static bool g_combatSpatialReady=false;
static bool CombatSpatialValidate();

static void CombatInteractionMetadata()
{
    uint32_t data=0,count=0;
    if (g_offOuter<0 || g_offPropOff<0 || g_offBoolMask<0 ||
        !SafeU32(g_gobjAddr,&data) || !SafeU32(g_gobjAddr+4,&count)) return;
    // One background table walk for all new functions and fields, not one per name.
    for(uint32_t i=0;i<count;++i) {
        uint32_t obj=0,outer=0,cls=0,ownerClass=0; char name[96]{},owner[96]{},kind[64]{};
        if(!SafeU32(data+i*4,&obj) || obj<0x10000 || !ReadObjName(obj,name,sizeof(name))) continue;
        bool wanted=false;
        for(const auto& f:g_combatFields) if(!strcmp(f.name,name)) wanted=true;
        for(const auto& f:g_combatFunctions) if(!strcmp(f.name,name)) wanted=true;
        if(!wanted || !SafeU32(obj+g_offOuter,&outer) || !ReadObjName(outer,owner,sizeof(owner)) ||
            !SafeU32(outer+kUObjectClassOff,&ownerClass) || !ObjNameIs(ownerClass,"Class") ||
            !SafeU32(obj+kUObjectClassOff,&cls) || !ReadObjName(cls,kind,sizeof(kind))) continue;
        for(auto& f:g_combatFunctions)
            if(!strcmp(f.name,name) && !strcmp(f.owner,owner) && !strcmp(kind,"Function")) f.fn=obj;
        for(auto& f:g_combatFields) {
            if(strcmp(f.name,name) || strcmp(f.owner,owner) || strcmp(f.kind,kind)) continue;
            uint32_t offset=0,mask=0;
            if(!SafeU32(obj+g_offPropOff,&offset) || offset>=0x8000) continue;
            if(!strcmp(kind,"BoolProperty") &&
                (!SafeU32(obj+g_offBoolMask,&mask) || !mask || (mask&(mask-1)))) continue;
            f.offset=(int)offset; f.mask=mask;
        }
    }
}

static bool CombatInteractionValidate()
{
    bool ok=true;
    for(int i=0;i<CBasicFunctionCount;++i) { const auto& f=g_combatFunctions[i]; if(!f.fn) { Log("[combat-grab] missing %s::%s",f.owner,f.name); ok=false; } }
    for(int i=0;i<CBasicFieldCount;++i) { const auto& f=g_combatFields[i]; if(f.offset<0) { Log("[combat-grab] missing %s::%s",f.owner,f.name); ok=false; } }
    struct Param { int fn; const char *name,*kind; uint32_t offset; };
    const Param params[]={
        {CIgnore,"ReturnValue","BoolProperty",0}, {CFindPickup,"ReturnValue","ObjectProperty",0},
        {CCanDrop,"ReturnValue","BoolProperty",0}, {CCanThrow,"ReturnValue","BoolProperty",0},
        {CJoint,"JointLoc","StructProperty",0}, {CJoint,"JointRot","StructProperty",12},
        {CDropFrom,"StartLocation","StructProperty",0}, {CDropFrom,"StartVelocity","StructProperty",12},
        {CDropFrom,"StartRotation","StructProperty",24}, {CDropFrom,"StartAngularVelocity","StructProperty",36},
        {CGetTarget,"MaxDistance","FloatProperty",0}, {CGetTarget,"MaxAngle","FloatProperty",4},
        {CGetTarget,"ReturnValue","ObjectProperty",8}, {CCanDisarm,"ReturnValue","BoolProperty",0},
        {CHandleAction,"Action","ByteProperty",0}
    };
    for(const auto& p:params) ok=CombatParameter(g_combatFunctions[p.fn].fn,p.name,p.kind,p.offset) && ok;
    uint32_t exec=0;
    ok=SafeU32(g_combatFunctions[CTrigger].fn+g_funcOffsetCached,&exec) &&
        exec==g_scriptTargetCached && ok;
    Log("[combat-grab] grip pickup/drop, left-hand pistol and hand-matched fourth-punch combo: %s",
        ok ? "ready" : "DISABLED: metadata/ABI validation failed");
    g_combatSpatialReady=ok && CombatSpatialValidate();
    return ok;
}

static bool CombatReadField(uintptr_t object,int id,void* out,size_t size)
{
    return object && g_combatFields[id].offset>=0 && SafeRead(object+g_combatFields[id].offset,out,size);
}
static bool CombatBoolField(uintptr_t object,int id,bool* out)
{
    uint32_t value=0;
    if(!g_combatFields[id].mask || !CombatReadField(object,id,&value,4)) return false;
    *out=(value&g_combatFields[id].mask)!=0; return true;
}
static bool CombatWriteBool(uintptr_t object,int id,bool value)
{
    uint32_t bits=0;
    if(!g_combatFields[id].mask || !CombatReadField(object,id,&bits,4)) return false;
    bits=value ? bits|g_combatFields[id].mask : bits&~g_combatFields[id].mask;
    return WriteRigBytes(object+g_combatFields[id].offset,&bits,4);
}
#include "combat_native.inl"
static bool CombatInvoke(uintptr_t object,int fn,void* parms=nullptr)
{
    if(fn==CState||fn==CFastTrace)return CombatIndexedQuery(object,fn,parms);
    uint32_t empty=0;
    return CombatCall(object,g_combatFunctions[fn].fn,parms ? parms : &empty);
}
static bool CombatBoolCall(uintptr_t object,int fn,bool* out)
{
    uint32_t value=0xCDCDCDCD;
    if(!CombatInvoke(object,fn,&value) || value>1) return false;
    *out=value!=0; return true;
}
static int CombatHoldingHand() { return (int)InterlockedCompareExchange(&g_combatHand,0,0); }
static bool CombatLeftHandGun() { return CombatHoldingHand()==0 && PistolCalibrationActive(); }
static bool CombatLeftBoneReady(uintptr_t weapon)
{
    return g_combatLeftBoneWeapon==weapon && g_frames-g_combatLeftBoneFrame>=0 &&
        g_frames-g_combatLeftBoneFrame<=3;
}

struct CombatRequest { uintptr_t pawn=0, weapon=0; int action=CombatNone; long frame=0;
    MEVR_Vec3 velocity{},angular{}; bool motionValid=false; double time=0; };
static SRWLOCK g_combatRequestLock=SRWLOCK_INIT;
static CombatRequest g_combatRequests[8]{};
static int g_combatRequestCount=0;
static CombatGripDetector g_combatGripDetector;
static void CombatSampleRelease(CombatRequest* request)
{
    const int hand=CombatHoldingHand();
    if(!g_motionPoseHeadValid || g_xrSpace==XR_NULL_HANDLE || g_viewSpace==XR_NULL_HANDLE) return;
    const XrSpace space=hand==0?g_sLGripPose:g_sRGripPose;
    if(space==XR_NULL_HANDLE) return;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION},location{XR_TYPE_SPACE_LOCATION};
    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY}; location.next=&velocity;
    // Base-space velocity includes real head/body movement; view-space velocity
    // would subtract head motion and would throw sideways when looking around.
    const XrTime when=g_combatGripDetector.previousTime>0 ? (XrTime)(g_combatGripDetector.previousTime*1e9):0;
    if(!when || XR_FAILED(xrLocateSpace(g_viewSpace,g_xrSpace,when,&head)) ||
        XR_FAILED(xrLocateSpace(space,g_xrSpace,when,&location)) ||
        !(head.locationFlags&XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
        !(location.locationFlags&XR_SPACE_LOCATION_POSITION_TRACKED_BIT) ||
        !(velocity.velocityFlags&XR_SPACE_VELOCITY_LINEAR_VALID_BIT)) return;
    const auto q=head.pose.orientation;
    const XrQuaternionf inverse{-q.x,-q.y,-q.z,q.w};
    auto v=RotateByQuaternion(inverse,{velocity.linearVelocity.x,velocity.linearVelocity.y,velocity.linearVelocity.z});
    if(!FiniteVec(v) || VecLength(v)>12.0f) return;
    v=XrViewToUECamera(v);
    request->velocity=CameraVectorToWorld(g_motionPoseHead,{v.x*g_worldScale,v.y*g_worldScale,v.z*g_worldScale});
    if(velocity.velocityFlags&XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
        v=RotateByQuaternion(inverse,{velocity.angularVelocity.x,velocity.angularVelocity.y,velocity.angularVelocity.z});
        // Angular velocity is an axial vector: the XR -> UE reflection changes its sign.
        v=XrViewToUECamera({-v.x,-v.y,-v.z});
        if(FiniteVec(v) && VecLength(v)<30.0f) request->angular=CameraVectorToWorld(g_motionPoseHead,v);
    }
    request->motionValid=true;
}
static void CombatQueueAction(int action,uintptr_t weapon)
{
    CombatRequest request{g_playerPawn,weapon,action,g_frames}; request.time=NowMs();
    if(action==CombatDrop) CombatSampleRelease(&request);
    AcquireSRWLockExclusive(&g_combatRequestLock);
    if(g_combatRequestCount<8) g_combatRequests[g_combatRequestCount++]=request;
    ReleaseSRWLockExclusive(&g_combatRequestLock);
}

static void CombatGripTick(XrTime when)
{
    auto& detector=g_combatGripDetector;
    static uintptr_t previousPawn=0;
    uint32_t weapon=0;
    const bool live=g_combatInteractionReady && g_motionHands && g_gripToGrip && g_padEnabled &&
        g_xrState==XR_SESSION_STATE_FOCUSED && g_playerPawn && CurrentViewTargetIsPawn(g_playerPawn) &&
        SafeU32(g_playerPawn+g_offWeapon,&weapon);
    if(!live || previousPawn!=g_playerPawn) {
        detector.reset(); previousPawn=g_playerPawn;
        InterlockedExchange(&g_combatHeldMask,0); g_combatGripBlocksPunch=false;
        return;
    }
    const MotionPoseSample* pose[2]={&g_poseLGrip,&g_poseRGrip};
    bool valid[2]{},forward[2]{};
    const XrSpaceLocationFlags required=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    for(int h=0;h<2;++h) {
        valid[h]=pose[h]->active && (pose[h]->flags&required)==required;
        const auto& p=pose[h]->pose.position; // head/view frame, metres, -Z forward
        forward[h]=valid[h] && p.z < -0.25f && p.y>-0.55f && p.y<0.25f && fabsf(p.x)<0.70f;
    }
    const bool gripAcquired=(uint32_t)InterlockedCompareExchange(&g_combatGripAcquiredWeapon,0,0)==weapon;
    const bool wasHeld[2]={detector.held[0],detector.held[1]},wasSeen[2]={detector.seen[0],detector.seen[1]};
    const int action=detector.sample((double)when*1e-9,weapon,CombatHoldingHand(),gripAcquired,
        valid,g_gripValue,forward);
    InterlockedExchange(&g_combatHeldMask,(detector.held[0] ? 1:0)|(detector.held[1] ? 2:0)|
        (valid[0] ? 4:0)|(valid[1] ? 8:0));
    g_combatGripBlocksPunch=g_motionPunch && action==CombatDisarm;
    if(g_pistolHands && !weapon && action!=CombatDisarm) for(int h=0;h<2;++h)
        if(wasSeen[h] && !wasHeld[h] && detector.held[h]) CombatQueueAction(h==0?CombatCatchLeft:CombatCatchRight,0);
    if(action!=CombatNone && (action==CombatDisarm ? g_motionPunch:g_pistolHands)) CombatQueueAction(action,weapon);
}

// These gates mirror the non-weapon subset of PressedSwitchWeapon. Calls below
// intentionally use its pickup/disarm branches separately, never the Y fallback
// that plays ForceMiss or drops a weapon when the intended target is absent.
static bool CombatCanUseY(uintptr_t pawn,uint8_t* movement,uintptr_t* move)
{
    bool ignored=true,cinematic=true; uint32_t world=0,pauser=0;
    uint8_t against=255,anim=255,state=255,group=255; char name[64]{};
    if(!CurrentViewTargetIsPawn(pawn) || !CombatBoolCall(g_playerCtl,CIgnore,&ignored) || ignored ||
        !CombatBoolField(g_playerCtl,CCinematic,&cinematic) || cinematic ||
        !CombatReadField(pawn,CWorld,&world,4) || !CombatReadField(world,CPauser,&pauser,4) || pauser ||
        !CombatReadField(pawn,CAgainstWall,&against,1) || against ||
        !SafeRead(pawn+g_offWeaponAnimState,&anim,1) || (anim!=0 && anim!=1 && anim!=2) ||
        !SafeRead(pawn+g_offMoveState,&state,1) || state==2 || state==20 ||
        !ReadMoveClassName(pawn,state,name,sizeof(name),move) ||
        !CombatReadField(*move,CMoveGroup,&group,1) || group) return false;
    *movement=state; return true;
}

static uintptr_t g_combatObservedWeapon=0, g_combatInteractionPawn=0;
static int g_combatPendingPickupHand=-1;
static long g_combatPendingPickupFrame=0;
static uintptr_t g_combatPendingDrop=0;
static CombatRequest g_combatRelease{};
static CombatFourPunchCombo g_combatCombo;
static int g_combatQueuedHands[4]{},g_combatQueuedHandCount=0,g_combatDispatchHand=-1;
static bool g_combatDispatchConsumed=false,g_combatTriggerOverride=false;
static int32_t g_combatSavedQueued=0;

static void CombatBeforeScript(uintptr_t object,uint32_t node)
{
    if(object==g_vrZiplineMove&&node==g_combatFunctions[CZipView].fn&&
        g_headTracking&&g_xrState==XR_SESSION_STATE_FOCUSED&&g_combatInteractionReady&&
        !g_vrZiplineViewOverride&&g_combatFunctions[CAbortLook].fn) {
        bool assist=false;
        if(CombatBoolField(object,CZipAssist,&assist)&&
            CombatBoolField(object,CZipConstrain,&g_vrZiplineSavedConstraint)) {
            if(assist&&CombatWriteBool(object,CZipAssist,false)&&CombatInvoke(object,CAbortLook))
                Log("[head] zipline automatic look target cancelled for headset control");
            // The native stick constraint clamps Controller.Rotation each tick,
            // competing with headset writes. Scope its bypass to this one call.
            g_vrZiplineViewOverride=CombatWriteBool(object,CZipConstrain,false);
        }
    }
    if(!g_combatInteractionReady || object!=g_combatMeleeMove ||
        node!=g_combatFunctions[CTrigger].fn) return;
    int hand=-1;
    if(g_combatDispatchHand>=0) { hand=g_combatDispatchHand; g_combatDispatchConsumed=true; }
    else if(g_combatQueuedHandCount>0) {
        hand=g_combatQueuedHands[0];
        for(int i=1;i<g_combatQueuedHandCount;++i) g_combatQueuedHands[i-1]=g_combatQueuedHands[i];
        --g_combatQueuedHandCount;
    }
    if(hand<0) { g_combatCombo.reset(); return; }
    if(!CombatReadField(object,CComboQueued,&g_combatSavedQueued,4) ||
        g_combatSavedQueued<0 || g_combatSavedQueued>2 || !CombatWriteBool(object,CLeft,hand==0)) return;
    const bool finisher=g_combatCombo.started(NowMs()*0.001);
    const int32_t counter=finisher ? 2:0, zero=0;
    g_combatTriggerOverride=WriteRigBytes(object+g_combatFields[CComboCounter].offset,&counter,4) &&
        WriteRigBytes(object+g_combatFields[CComboQueued].offset,&zero,4);
    Log("[combat-melee] accepted %s attack %s",hand==0 ? "LEFT":"RIGHT",
        finisher ? "4: two-hand finisher requested":"with matching hand");
}
static void CombatAfterScript(uintptr_t object,uint32_t node)
{
    if(object==g_vrZiplineMove&&node==g_combatFunctions[CZipView].fn&&g_vrZiplineViewOverride){
        CombatWriteBool(object,CZipConstrain,g_vrZiplineSavedConstraint);
        g_vrZiplineViewOverride=false;
    }
    if(!g_combatInteractionReady || object!=g_combatMeleeMove) return;
    if(node==g_combatFunctions[CTrigger].fn && g_combatTriggerOverride) {
        const int32_t zero=0;
        WriteRigBytes(object+g_combatFields[CComboCounter].offset,&zero,4);
        WriteRigBytes(object+g_combatFields[CComboQueued].offset,&g_combatSavedQueued,4);
        g_combatTriggerOverride=false;
    }
    if(node==g_combatFunctions[CStopMelee].fn) g_combatQueuedHandCount=0;
}

#include "combat_spatial.inl"

static void CombatTryDrop(uintptr_t pawn,uintptr_t weapon)
{
    bool canDrop=false,canThrow=false;
    if(!CombatPistolClass(weapon) || !CombatBoolCall(pawn,CCanDrop,&canDrop) || !canDrop ||
        !CombatBoolCall(weapon,CCanThrow,&canThrow) || !canThrow) return;
    if(CombatHoldingHand()==0 && !CombatLeftBoneReady(weapon)) return;
    struct Joint { MEVR_Vec3 location; int32_t rotation[3]; } joint{};
    if(!CombatInvoke(pawn,CJoint,&joint) || !FiniteVec(joint.location)) return;
    MEVR_Vec3 player{};
    if(!SafeRead(pawn+g_offActorLocation,&player,sizeof(player)) ||
        VecLength({joint.location.x-player.x,joint.location.y-player.y,joint.location.z-player.z})>300) return;
    struct Drop { MEVR_Vec3 location,velocity; int32_t rotation[3]; MEVR_Vec3 angular; } drop{};
    static_assert(sizeof(Drop)==48,"drop ABI");
    drop.location=joint.location; memcpy(drop.rotation,joint.rotation,sizeof(drop.rotation));
    if(g_combatRelease.motionValid && NowMs()-g_combatRelease.time<150.0) {
        drop.velocity=g_combatRelease.velocity; drop.angular=g_combatRelease.angular;
        MEVR_Vec3 locomotion{};
        if(CombatReadField(pawn,CActorVelocity,&locomotion,sizeof(locomotion)) && FiniteVec(locomotion) && VecLength(locomotion)<2000) {
            drop.velocity.x+=locomotion.x;drop.velocity.y+=locomotion.y;drop.velocity.z+=locomotion.z;
        }
    }
    if(!CombatInvoke(pawn,CLOIOff)) return;
    uint32_t world=0;
    if(CombatReadField(pawn,CWorld,&world,4) && world!=g_combatPickupWorld) {
        CombatSpatialReset();g_combatPickupWorld=world;
    }
    g_combatWatchingDrop=true;
    const bool dropped=CombatInvoke(weapon,CDropFrom,&drop);
    g_combatWatchingDrop=false;
    if(!dropped) return;
    uint32_t stillEquipped=0; bool hidden=false;
    if(SafeU32(pawn+g_offWeapon,&stillEquipped) && stillEquipped==weapon &&
        CombatBoolField(weapon,CHidden,&hidden) && hidden) CombatInvoke(pawn,CRemove);
    g_combatPendingDrop=0;
    Log("[combat-grab] %s-hand throw at (%.1f %.1f %.1f), velocity (%.1f %.1f %.1f) UU/s spin %.1f rad/s",
        CombatHoldingHand()==0 ? "LEFT":"RIGHT",joint.location.x,joint.location.y,joint.location.z,
        drop.velocity.x,drop.velocity.y,drop.velocity.z,VecLength(drop.angular));
}

static void CombatTryDisarm(uintptr_t pawn)
{
    float minimum=0,maximum=0,angle=0; MEVR_Vec3 velocity{}; int32_t rotation[3]{};
    if(!CombatReadField(g_playerCtl,CMinRange,&minimum,4) || !CombatReadField(g_playerCtl,CMaxRange,&maximum,4) ||
        !CombatReadField(g_playerCtl,CMaxAngle,&angle,4) || !CombatReadField(pawn,CActorVelocity,&velocity,sizeof(velocity)) ||
        !SafeRead(pawn+g_offActorRotation,rotation,sizeof(rotation)) || !FiniteVec(velocity) ||
        !std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(angle) || minimum<0 || maximum<minimum) return;
    const float radians=3.14159265358979323846f/32768.0f;
    const float p=rotation[0]*radians,y=rotation[1]*radians;
    const float speed=velocity.x*cosf(p)*cosf(y)+velocity.y*cosf(p)*sinf(y)+velocity.z*sinf(p);
    struct Target { float distance,angle; uint32_t result; } target{};
    target.distance=(std::min)(maximum,(std::max)(minimum,(std::max)(0.0f,speed)*0.4f));
    target.angle=angle;
    if(!CombatInvoke(g_playerCtl,CGetTarget,&target) || !LooksLikeRigObject(target.result,"TdPawn")) return;
    uintptr_t disarm=0; char name[64]{};
    uint32_t previousTarget=0;
    if(!ReadMoveClassName(pawn,18,name,sizeof(name),&disarm) || !LooksLikeRigObject(disarm,"TdMOVE_Disarm") ||
        !CombatReadField(g_playerCtl,CTarget,&previousTarget,4) ||
        !WriteRigBytes(g_playerCtl+g_combatFields[CTarget].offset,&target.result,4)) return;
    bool allowed=false;
    if(CombatBoolCall(disarm,CCanDisarm,&allowed) && allowed) {
        uint32_t action=4;
        CombatInvoke(pawn,CHandleAction,&action);
        Log("[combat-grab] two-hand forward grab -> game-validated disarm attempt");
    } else {
        WriteRigBytes(g_playerCtl+g_combatFields[CTarget].offset,&previousTarget,4);
        Log("[combat-grab] two-hand grab: target rejected by normal disarm checks");
    }
}

static void CombatObserveWeapon(uintptr_t pawn,uint32_t weapon)
{
    if(pawn!=g_combatInteractionPawn) {
        g_combatInteractionPawn=pawn; g_combatObservedWeapon=0;
        g_combatPendingPickupHand=-1; g_combatPendingDrop=0;
        g_combatQueuedHandCount=0; g_combatCombo.reset();
        CombatSpatialReset();
        InterlockedExchange(&g_combatHand,1); InterlockedExchange(&g_combatGripAcquiredWeapon,0);
    }
    if(weapon!=g_combatObservedWeapon) {
        const bool grabbed=weapon && g_combatPendingPickupHand>=0 && g_frames-g_combatPendingPickupFrame<180;
        InterlockedExchange(&g_combatHand,grabbed ? g_combatPendingPickupHand:1);
        InterlockedExchange(&g_combatGripAcquiredWeapon,grabbed ? (LONG)weapon:0);
        g_combatPendingPickupHand=-1; g_combatPendingDrop=0; g_combatObservedWeapon=weapon;
        g_combatLeftBoneWeapon=0; g_combatQueuedHandCount=0; g_combatCombo.reset();
        if(weapon) Log("[combat-grab] weapon %p assigned to %s hand (%s)",(void*)(uintptr_t)weapon,
            CombatHoldingHand()==0 ? "LEFT":"RIGHT",grabbed ? "grip release armed":"latched until first squeeze");
    }
    if(g_combatPendingPickupHand>=0 && g_frames-g_combatPendingPickupFrame>=180) g_combatPendingPickupHand=-1;
}

static void CombatGameplayTick(uintptr_t pawn)
{
    if(g_menuBlocksGameplay || !g_combatInteractionReady || g_combatFault || pawn!=g_playerPawn) return;
    NativeLensFlareTick();
    uint32_t weapon=0;
    if(!SafeU32(pawn+g_offWeapon,&weapon)) return;
    CombatObserveWeapon(pawn,weapon);
    char name[64]{};
    // Seed during ordinary gameplay, before ZipLine.UpdateViewRotation starts.
    if(!ReadMoveClassName(pawn,28,name,sizeof(name),&g_vrZiplineMove)||
        !LooksLikeRigObject(g_vrZiplineMove,"TdMove_ZipLine"))g_vrZiplineMove=0;
    if(!ReadMoveClassName(pawn,17,name,sizeof(name),&g_combatMeleeMove) ||
        !LooksLikeRigObject(g_combatMeleeMove,"TdMove_Melee")) g_combatMeleeMove=0;
    CombatRequest requests[8]{}; int count=0;
    AcquireSRWLockExclusive(&g_combatRequestLock);
    count=g_combatRequestCount; memcpy(requests,g_combatRequests,sizeof(CombatRequest)*count);
    g_combatRequestCount=0;
    ReleaseSRWLockExclusive(&g_combatRequestLock);
    CombatSpatialTick(pawn,weapon);
    if(!SafeU32(pawn+g_offWeapon,&weapon))return;
    CombatObserveWeapon(pawn,weapon);
    if(!count && !g_combatPendingDrop) return;
    uint8_t state=255; uintptr_t move=0;
    const bool usable=g_xrState==XR_SESSION_STATE_FOCUSED && CombatCanUseY(pawn,&state,&move);
    for(int i=0;i<count;++i) {
        const auto& r=requests[i];
        if(r.pawn!=pawn || r.frame>g_frames || g_frames-r.frame>15 || r.weapon!=weapon) continue;
        if(r.action==CombatDrop) { g_combatPendingDrop=weapon; g_combatRelease=r; continue; }
        if(weapon || g_xrState!=XR_SESSION_STATE_FOCUSED || !CurrentViewTargetIsPawn(pawn)) continue;
        if(r.action==CombatCatchLeft || r.action==CombatCatchRight) {
            CombatBeginCatch(r.action==CombatCatchLeft?0:1); continue;
        }
        if(usable && (r.action==CombatPickupLeft || r.action==CombatPickupRight)) CombatTryPickup(pawn,r.action==CombatPickupLeft ? 0:1,move);
        else if(usable && r.action==CombatDisarm) { CombatCancelCatch(); CombatTryDisarm(pawn); }
        else if((r.action==CombatPunchLeft || r.action==CombatPunchRight) && g_combatMeleeMove &&
            SafeRead(pawn+g_offMoveState,&state,1) && (state==1 || state==17)) {
            int32_t before=0,after=0;
            if(!CombatReadField(g_combatMeleeMove,CComboQueued,&before,4) || g_combatQueuedHandCount>=2) continue;
            g_combatDispatchHand=r.action==CombatPunchLeft ? 0:1; g_combatDispatchConsumed=false;
            CombatInvoke(g_playerCtl,CAttack);
            if(!g_combatDispatchConsumed && CombatReadField(g_combatMeleeMove,CComboQueued,&after,4) && after>before)
                g_combatQueuedHands[g_combatQueuedHandCount++]=g_combatDispatchHand;
            g_combatDispatchHand=-1;
        }
        // A request may synchronously equip a weapon or change movement.
        break;
    }
    const int held=(int)InterlockedCompareExchange(&g_combatHeldMask,0,0);
    if(g_combatPendingDrop && (g_combatPendingDrop!=weapon || (held&(1<<CombatHoldingHand())) ||
        !(held&(4<<CombatHoldingHand())) || g_xrState!=XR_SESSION_STATE_FOCUSED)) g_combatPendingDrop=0;
    if(usable && weapon && g_combatPendingDrop==weapon) CombatTryDrop(pawn,weapon);
    if(SafeU32(pawn+g_offWeapon,&weapon)) CombatObserveWeapon(pawn,weapon);
}

// Rebase the per-instance RightWeapon bone onto the tracked left hand. The
// weapon remains attached to its original socket, so its muzzle/effects follow.
// SpaceBases is rebuilt by the game each pose; no shared socket asset is edited.
static void CombatRedirectWeaponBone(uintptr_t mesh,uint32_t bones,int count)
{
    const int hand=CombatHoldingHand();
    const auto& ownership=hand==0?g_p13LeftState:g_p13RightState;
    if(!g_combatInteractionReady || count<=49 || !ownership.owned ||
        ownership.ownedPawn!=g_playerPawn || mesh!=g_probeMesh1p || !PistolCalibrationActive()) return;
    if(hand==1 && !g_gunPositionMm[1][0] && !g_gunPositionMm[1][1] && !g_gunPositionMm[1][2]) return;
    uint32_t currentWeapon=0; uint8_t anim=0;
    if(!SafeU32(g_playerPawn+g_offWeapon,&currentWeapon) ||
        !SafeRead(g_playerPawn+g_offWeaponAnimState,&anim,1) ||
        !PistolAllowsHandControl(g_playerPawn,currentWeapon,anim)) return;
    UE3Matrix44 left{},right{},weapon{};
    if(!SafeRead(bones+19*sizeof(left),&left,sizeof(left)) ||
        !SafeRead(bones+48*sizeof(right),&right,sizeof(right)) ||
        !SafeRead(bones+49*sizeof(weapon),&weapon,sizeof(weapon))) return;
    Mat3 l{},r{},w{};
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) { l.m[i][j]=left.m[i][j]; r.m[i][j]=right.m[i][j]; w.m[i][j]=weapon.m[i][j]; }
    for(const auto* basis : {&l,&r,&w}) for(int i=0;i<3;++i) {
        const float len=VecLength({basis->m[i][0],basis->m[i][1],basis->m[i][2]});
        if(!std::isfinite(len) || fabsf(len-1)>0.03f) return;
    }
    // Same mirrored rest-frame convention as the wrist solver. Derive both
    // calibration quaternions without the gun trim, which is already in LeftHand.
    const auto rest=[](const int c[3]) {
        const float half=3.14159265358979323846f/360.0f;
        return MultiplyQuaternion(MultiplyQuaternion({0,0,sinf(c[1]*half),cosf(c[1]*half)},
            {0,-sinf(c[0]*half),0,cosf(c[0]*half)}),{sinf(c[2]*half),0,0,cosf(c[2]*half)});
    };
    const auto ql=rest(g_wristCalibrationDeg[0]),qr=rest(g_wristCalibrationDeg[1]);
    const auto delta=MultiplyQuaternion({-ql.x,-ql.y,-ql.z,ql.w},qr);
    Mat3 adjust{};
    for(int i=0;i<3;++i) {
        MEVR_Vec3 axis{}; if(i==0) axis.x=1; else if(i==1) axis.y=1; else axis.z=1;
        const auto v=RotateByQuaternion(delta,axis);
        adjust.m[i][0]=v.x; adjust.m[i][1]=v.y; adjust.m[i][2]=v.z;
    }
    const Mat3 virtualRight=hand==0?Mat3Multiply(adjust,l):r;
    const Mat3 relative=Mat3Multiply(w,Mat3Transpose(r));
    const Mat3 orientation=Mat3Multiply(relative,virtualRight);
    float offset[3]{},local[3]{};
    for(int j=0;j<3;++j) offset[j]=weapon.m[3][j]-right.m[3][j];
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) local[i]+=offset[j]*r.m[i][j];
    if(!FiniteVec({local[0],local[1],local[2]}) || VecLength({local[0],local[1],local[2]})>50) return;
    for(int j=0;j<3;++j) {
        weapon.m[3][j]=(hand==0?left:right).m[3][j];
        for(int i=0;i<3;++i) { weapon.m[i][j]=orientation.m[i][j]; weapon.m[3][j]+=local[i]*virtualRight.m[i][j]; }
    }
    // Remove the right hand's authored rest rotation to obtain the controller's
    // forward/right/up axes. Position trim moves only the gun, not the hand IK.
    Mat3 restRight{};
    for(int i=0;i<3;++i) {
        MEVR_Vec3 axis{}; if(i==0)axis.x=1; else if(i==1)axis.y=1; else axis.z=1;
        const auto v=RotateByQuaternion(qr,axis);
        restRight.m[i][0]=v.x;restRight.m[i][1]=v.y;restRight.m[i][2]=v.z;
    }
    const Mat3 controller=Mat3Multiply(Mat3Transpose(restRight),virtualRight);
    for(int j=0;j<3;++j) for(int i=0;i<3;++i)
        weapon.m[3][j]+=g_gunPositionMm[hand][i]*(g_worldScale/1000.0f)*controller.m[i][j];
    uint32_t equipped=0;
    if(SafeU32(g_playerPawn+g_offWeapon,&equipped) &&
        WriteRigBytes(bones+49*sizeof(weapon),&weapon,sizeof(weapon))) {
        g_combatLeftBoneWeapon=equipped; g_combatLeftBoneFrame=g_frames;
    }
}
