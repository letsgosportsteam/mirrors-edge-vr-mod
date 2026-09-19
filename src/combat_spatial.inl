// Included by combat_interaction.inl. All UObject access stays on the game thread.
#include "combat_spatial_math.h"
static uintptr_t g_combatPickupOverrideManager=0;
static uint32_t g_combatPickupOverride=0;
static unsigned g_combatPickupOverrideHits=0;
static bool g_combatWatchingDrop=false;
static uint32_t g_combatPickupType=0;
struct CombatPickupEntry { uint32_t object=0; MEVR_Vec3 previous{}; double sampled=0; };
static CombatPickupEntry g_combatPickups[128]{};
static uint32_t g_combatPickupCursor=0,g_combatPickupWorld=0,g_combatGazePickup=0;
static double g_combatNextScan=0,g_combatNextGaze=0,g_combatCatchUntil[2]{};
static MEVR_Vec3 g_combatGazeLocation{};
static SRWLOCK g_combatHighlightLock=SRWLOCK_INIT;
static MEVR_Vec3 g_combatHighlightPosition{};
static double g_combatHighlightUntil=0;
static const char* g_combatPickupGate="idle";
static const char* g_combatPositionSource="actor";
struct CombatPickupBounds { MEVR_Vec3 origin,extent;float radius; };
static void CombatPublishHighlight(bool visible,MEVR_Vec3 position={})
{
    AcquireSRWLockExclusive(&g_combatHighlightLock);
    g_combatHighlightPosition=position;
    g_combatHighlightUntil=visible?NowMs()+250.0:0;
    ReleaseSRWLockExclusive(&g_combatHighlightLock);
}
static bool CombatReadHighlight(MEVR_Vec3* position)
{
    AcquireSRWLockShared(&g_combatHighlightLock);
    *position=g_combatHighlightPosition;
    const double until=g_combatHighlightUntil;
    ReleaseSRWLockShared(&g_combatHighlightLock);
    return NowMs()<until;
}

static bool CombatSpatialValidate()
{
    bool ok=true;
    for(int i=CBasicFieldCount;i<CFieldCount;++i) if(g_combatFields[i].offset<0) {
        Log("[combat-spatial] missing field %s",g_combatFields[i].name); ok=false;
    }
    for(int i=CBasicFunctionCount;i<CFunctionCount;++i) if(!g_combatFunctions[i].fn) {
        Log("[combat-spatial] missing function %s",g_combatFunctions[i].name); ok=false;
    }
    struct Param{int fn;const char *name,*kind;uint32_t offset;};
    const Param params[]={
        {CAmmo,"ReturnValue","IntProperty",0},{CState,"ReturnValue","NameProperty",0},
        {CFastTrace,"TraceEnd","StructProperty",0},{CFastTrace,"TraceStart","StructProperty",12},
        {CFastTrace,"BoxExtent","StructProperty",24},{CFastTrace,"bTraceBullet","BoolProperty",36},
        {CFastTrace,"ReturnValue","BoolProperty",40},
        {CBlueBox,"Center","StructProperty",0},{CBlueBox,"Extent","StructProperty",12},
        {CBlueBox,"R","ByteProperty",24},{CBlueBox,"G","ByteProperty",25},{CBlueBox,"B","ByteProperty",26},
        {CBlueBox,"timeToLive","FloatProperty",28},
        {CFreeSlot,"WeaponClass","ClassProperty",0},{CFreeSlot,"ReturnValue","ByteProperty",4},
        {CSetPickupAmmo,"Count","IntProperty",0}, {CMeshPosition,"ReturnValue","StructProperty",0},
        {CRootBody,"ReturnValue","ObjectProperty",0}, {CBodyTransform,"ReturnValue","StructProperty",0}
    };
    for(const auto& p:params) ok=CombatParameter(g_combatFunctions[p.fn].fn,p.name,p.kind,p.offset)&&ok;
    ok=SafeU32(g_combatFunctions[CAmmo].fn+g_offOuter,&g_combatPickupType)&&ok;
    Log("[combat-spatial] gaze pickup, blue highlight and hand catches: %s",ok?"ready":"DISABLED: layout validation failed");
    return ok;
}

static void CombatCancelCatch(){g_combatCatchUntil[0]=g_combatCatchUntil[1]=0;}
static void CombatSpatialReset()
{
    memset(g_combatPickups,0,sizeof(g_combatPickups));g_combatPickupCursor=0;g_combatPickupWorld=0;
    g_combatGazePickup=0;g_combatNextScan=g_combatNextGaze=0;CombatCancelCatch();
}
static void CombatRegisterPickup(uint32_t object)
{
    if(!object) return;
    for(const auto& p:g_combatPickups) if(p.object==object) return;
    for(auto& p:g_combatPickups) if(!p.object){p={object};
        if(g_combatWatchingDrop)Log("[combat-spatial] registered thrown pickup %p",(void*)(uintptr_t)object);
        return;
    }
}
static void CombatSpatialAfterScript(uintptr_t object,uint32_t node,void* result)
{
    if(!g_combatSpatialReady) return;
    if(g_combatWatchingDrop && node==g_combatFunctions[CSetPickupAmmo].fn) CombatRegisterPickup((uint32_t)object);
    // Confined to our explicit pickup invocation. Normal Y retains its own lookup.
    if(object==g_combatPickupOverrideManager && node==g_combatFunctions[CFindPickup].fn && result){
        if(WriteRigBytes((uintptr_t)result,&g_combatPickupOverride,4))++g_combatPickupOverrideHits;
    }
}
static bool CombatPickupAlive(uint32_t object,uint32_t world,MEVR_Vec3* position)
{
    uint32_t cls=0,ownerWorld=0;bool deleted=true,hidden=true,fade=true;
    const bool alive=object && SafeU32(object+kUObjectClassOff,&cls) && cls==g_combatPickupType &&
        CombatReadField(object,CWorld,&ownerWorld,4) && ownerWorld==world &&
        CombatBoolField(object,CDeleted,&deleted)&&!deleted && CombatBoolField(object,CHidden,&hidden)&&!hidden &&
        CombatBoolField(object,CFade,&fade)&&!fade &&
        SafeRead(object+g_offActorLocation,position,sizeof(*position))&&FiniteVec(*position);
    if(!alive){g_combatPickupGate="pickup class/world/deleted/hidden/fade";return false;}
    g_combatPositionSource="actor";
    // A physics-driven skeletal body can move inside an unchanged component
    // LocalToWorld. Query its actual root rigid body before using component origin.
    uint32_t mesh=0;MEVR_Vec3 physical{};
    if(CombatReadField(object,CPickMesh,&mesh,4)&&mesh){
        // A skeletal body's root/pivot need not be at the visible pistol.
        // The renderer's small, finite world bounds provide its visible center.
        CombatPickupBounds bounds{};
        if(CombatReadField(mesh,CPickBounds,&bounds,sizeof(bounds))&&FiniteVec(bounds.origin)&&
            FiniteVec(bounds.extent)&&std::isfinite(bounds.radius)&&bounds.radius>0&&
            bounds.radius<g_worldScale&&bounds.extent.x>0&&bounds.extent.y>0&&bounds.extent.z>0&&
            bounds.extent.x<=bounds.radius&&bounds.extent.y<=bounds.radius&&bounds.extent.z<=bounds.radius){
            *position=bounds.origin;g_combatPositionSource="render bounds";return true;
        }
        uint32_t body=0;alignas(16) float transform[16]{};
        if(CombatInvoke(mesh,CRootBody,&body)&&body&&LooksLikeRigObject(body,"RB_BodyInstance")&&
            CombatInvoke(body,CBodyTransform,transform)){
            physical={transform[12],transform[13],transform[14]};
            if(FiniteVec(physical)&&fabsf(transform[15]-1.0f)<0.01f){*position=physical;g_combatPositionSource="rigid body";return true;}
        }
        if(CombatInvoke(mesh,CMeshPosition,&physical)&&FiniteVec(physical)){*position=physical;g_combatPositionSource="component";}
    }
    return true;
}
static bool CombatPickupEligible(uintptr_t pawn,uint32_t object,uint32_t manager,bool catching=false)
{
    uint32_t cls=0,state[2]{0xCDCDCDCD,0xCDCDCDCD};char name[96]{};int32_t ammo=0;bool allowed=false;
    g_combatPickupGate="pawn inventory disabled";
    if(!CombatBoolField(pawn,CCanPickupInventory,&allowed)||!allowed)return false;
    g_combatPickupGate="not a supported pistol";
    if(!CombatReadField(object,CPickupClass,&cls,4)||!ReadObjName(cls,name,sizeof(name)) ||
        (strcmp(name,"TdWeapon_Pistol_Colt1911")&&strcmp(name,"TdWeapon_Pistol_Glock18c")))return false;
    g_combatPickupGate="state query failed";
    if(!CombatInvoke(object,CState,state)||!NameOf(state[0],name,sizeof(name)))return false;
    g_combatPickupGate="pickup state/cooldown";
    if(_stricmp(name,"Pickup")&&!(catching&&!_stricmp(name,"CoolDown")))return false;
    g_combatPickupGate="empty ammunition";
    if(!CombatInvoke(object,CAmmo,&ammo)||ammo<=0)return false;
    g_combatPickupGate="inventory slot occupied";
    struct Slot{uint32_t cls;uint8_t value;uint8_t pad[3];} slot{cls,0,{}};
    const bool ok=CombatInvoke(manager,CFreeSlot,&slot)&&slot.value>0&&slot.value<=2;
    if(ok)g_combatPickupGate="eligible";
    return ok;
}
static bool CombatPickupVisible(uintptr_t pawn,MEVR_Vec3 start,MEVR_Vec3 end)
{
    struct Trace{MEVR_Vec3 end,start,extent;uint32_t bullet,result;} trace{end,start,{},0,0};
    const bool visible=CombatInvoke(pawn,CFastTrace,&trace)&&trace.result!=0;
    if(!visible)g_combatPickupGate="occluded";
    return visible;
}
static bool CombatSpatialContext(uintptr_t pawn,uint32_t* manager,uint32_t* world,P13PoseSnapshot* pose,uintptr_t* move)
{
    uint8_t state=0;bool allowed=false;
    g_combatPickupGate="feature/tracking inactive";
    if(!g_combatSpatialReady||!g_pistolHands||!g_motionHands||!g_gripToGrip||g_xrState!=XR_SESSION_STATE_FOCUSED)return false;
    g_combatPickupGate="normal Y/movement gate";
    if(!CombatCanUseY(pawn,&state,move)||!CombatBoolField(*move,CAllowPickup,&allowed)||!allowed)return false;
    g_combatPickupGate="inventory manager unavailable";
    if(!CombatReadField(pawn,CInvManager,manager,4)||!LooksLikeRigObject(*manager,"TdInventoryManager"))return false;
    g_combatPickupGate="world unavailable";
    if(!CombatReadField(pawn,CWorld,world,4))return false;
    g_combatPickupGate="head pose unavailable";
    if(!ReadP13PoseSnapshot(pose)||!pose->sampledHeadValid)return false;
    g_combatPickupGate="head pose stale";
    if(pose->presentFrame>g_frames||g_frames-pose->presentFrame>3)return false;
    // The arm anchor omits render-only yaw/pitch corrections. Gaze must use
    // the center of the actual eye views, not that earlier animation camera.
    ReadPickupView(pawn,&pose->sampledHead);
    return true;
}
static void CombatSeedNearby(uint32_t manager)
{
    // Native touching-actor lookup is fast and knows which level pickups are live.
    // The object-table walk remains a fallback for catches beyond that cylinder.
    uint32_t nearby=0;
    if(CombatInvoke(manager,CFindPickup,&nearby)&&nearby)CombatRegisterPickup(nearby);
}
static void CombatReportPickup(bool force=false)
{
    static double next=0;
    static long lastMarkerCapture=-1;
    if(g_effectCaptureUntil>0&&g_effectCaptureUntil!=lastMarkerCapture){
        lastMarkerCapture=g_effectCaptureUntil;force=true;
    }
    const double now=NowMs();
    if(!force&&now<next)return;
    next=now+2000;
    int count=0;for(const auto& p:g_combatPickups)if(p.object)++count;
    Log("[combat-spatial] frame %ld candidates=%d selected=%p gate=%s catches=%d/%d",g_frames,count,
        (void*)(uintptr_t)g_combatGazePickup,g_combatPickupGate,
        now<g_combatCatchUntil[0]?1:0,now<g_combatCatchUntil[1]?1:0);
    P13PoseSnapshot pose{};
    if(!ReadP13PoseSnapshot(&pose)||!pose.sampledHeadValid)return;
    const bool rendered=ReadPickupView(g_playerPawn,&pose.sampledHead);
    unsigned logged=0;
    for(const auto& p:g_combatPickups){
        if(!p.object||logged>=4)continue;
        MEVR_Vec3 position{},actor{};
        if(!CombatPickupAlive(p.object,g_combatPickupWorld,&position))continue;
        ++logged;SafeRead(p.object+g_offActorLocation,&actor,sizeof(actor));
        const MEVR_Vec3 d{position.x-pose.sampledHead.position.x,position.y-pose.sampledHead.position.y,position.z-pose.sampledHead.position.z};
        const float distance=VecLength(d),forward=d.x*pose.sampledHead.forward.x+d.y*pose.sampledHead.forward.y+d.z*pose.sampledHead.forward.z;
        const float cosine=distance>0?(std::max)(-1.0f,(std::min)(1.0f,forward/distance)):1.0f;
        const MEVR_Vec3 dl{position.x-pose.left.worldPosition.x,position.y-pose.left.worldPosition.y,position.z-pose.left.worldPosition.z};
        const MEVR_Vec3 dr{position.x-pose.right.worldPosition.x,position.y-pose.right.worldPosition.y,position.z-pose.right.worldPosition.z};
        Log("[pickup-position] frame=%ld object=%p source=%s pos=(%.1f %.1f %.1f) actor=(%.1f %.1f %.1f)"
            " view=%s head=(%.1f %.1f %.1f) distance=%.3fm angle=%.2fdeg hands=%.3f/%.3fm valid=%d/%d age=%ld grip=%ld",
            g_frames,(void*)(uintptr_t)p.object,g_combatPositionSource,position.x,position.y,position.z,actor.x,actor.y,actor.z,
            rendered?"rendered":"animation",pose.sampledHead.position.x,pose.sampledHead.position.y,pose.sampledHead.position.z,
            distance/g_worldScale,acosf(cosine)*57.29578f,VecLength(dl)/g_worldScale,VecLength(dr)/g_worldScale,
            pose.left.worldValid?1:0,pose.right.worldValid?1:0,g_frames-pose.presentFrame,
            InterlockedCompareExchange(&g_combatHeldMask,0,0));
        uint32_t state[2]{0xCDCDCDCD,0xCDCDCDCD},manager=0;char stateName[64]="QUERY FAILED";
        const bool stateOK=CombatInvoke(p.object,CState,state)&&NameOf(state[0],stateName,sizeof(stateName));
        const char* savedGate=g_combatPickupGate;
        CombatReadField(g_playerPawn,CInvManager,&manager,4);
        const bool eligible=manager&&CombatPickupEligible(g_playerPawn,p.object,manager,true);
        const char* eligibility=g_combatPickupGate;
        const bool visible=CombatPickupVisible(g_playerPawn,pose.sampledHead.position,position);
        Log("[pickup-check] object=%p state=%s raw=%08X/%08X query=%d catchEligible=%d reason=%s headVisible=%d",
            (void*)(uintptr_t)p.object,stateName,state[0],state[1],stateOK?1:0,eligible?1:0,eligibility,visible?1:0);
        g_combatPickupGate=savedGate;
    }
}
static bool CombatPickSelected(uintptr_t pawn,int hand,uint32_t target,uint32_t manager,uint32_t world,bool catching=false)
{
    MEVR_Vec3 position{};
    if(!CombatPickupAlive(target,world,&position)||!CombatPickupEligible(pawn,target,manager,catching)) return false;
    g_combatPendingPickupHand=hand;g_combatPendingPickupFrame=g_frames;
    g_combatPickupOverrideManager=manager;g_combatPickupOverride=target;g_combatPickupOverrideHits=0;
    const bool invoked=CombatInvoke(manager,CPickup);
    g_combatPickupOverrideManager=0;g_combatPickupOverride=0;
    if(!invoked) g_combatPendingPickupHand=-1;
    g_combatGazePickup=0;CombatCancelCatch();CombatPublishHighlight(false);
    uint32_t weapon=0;SafeU32(pawn+g_offWeapon,&weapon);
    Log("[combat-spatial] %s selected pickup %p invoked=%d lookupOverrides=%u resultingWeapon=%p",hand==0?"LEFT":"RIGHT",
        (void*)(uintptr_t)target,invoked?1:0,g_combatPickupOverrideHits,(void*)(uintptr_t)weapon);
    return invoked;
}
static void CombatScanPickups(uint32_t world)
{
    uint32_t data=0,count=0;
    if(!SafeU32(g_gobjAddr,&data)||!SafeU32(g_gobjAddr+4,&count)||!count||count>2000000) return;
    if(g_combatPickupCursor>=count)g_combatPickupCursor=0;
    uint32_t objects[1024]{};
    const uint32_t n=(std::min)(count-g_combatPickupCursor,(uint32_t)1024);
    if(!SafeRead(data+g_combatPickupCursor*4,objects,n*4))return;
    g_combatPickupCursor+=n;
    for(uint32_t i=0;i<n;++i){uint32_t cls=0;MEVR_Vec3 position{};
        if(objects[i]>0x10000&&SafeU32(objects[i]+kUObjectClassOff,&cls)&&cls==g_combatPickupType &&
            CombatPickupAlive(objects[i],world,&position)) CombatRegisterPickup(objects[i]);
    }
}
static uint32_t CombatSelectGaze(uintptr_t pawn,uint32_t manager,uint32_t world,const RenderedHeadFrame& head,MEVR_Vec3* selectedPosition)
{
    uint32_t selected=0;float best=1000;
    for(auto& p:g_combatPickups) {
        if(!p.object)continue;
        MEVR_Vec3 position{};
        if(!CombatPickupAlive(p.object,world,&position)){p={};continue;}
        const MEVR_Vec3 delta{(position.x-head.position.x)/g_worldScale,(position.y-head.position.y)/g_worldScale,(position.z-head.position.z)/g_worldScale};
        const float forward=delta.x*head.forward.x+delta.y*head.forward.y+delta.z*head.forward.z;
        const float distance=VecLength(delta);
        const float score=CombatGazeScore(forward,(std::max)(0.0f,distance*distance-forward*forward),distance);
        g_combatPickupGate="outside gaze cone/range";
        if(score<0||score>=best||!CombatPickupEligible(pawn,p.object,manager)||
            !CombatPickupVisible(pawn,head.position,position))continue;
        best=score;selected=p.object;*selectedPosition=position;
    }
    return selected;
}
static void CombatTryPickup(uintptr_t pawn,int hand,uintptr_t)
{
    uint32_t manager=0,world=0;P13PoseSnapshot pose{};uintptr_t move=0;MEVR_Vec3 position{};
    if(!CombatSpatialContext(pawn,&manager,&world,&pose,&move)){CombatReportPickup(true);return;}
    CombatSeedNearby(manager);
    // Re-evaluate on squeeze, so a highlight from the previous tick cannot pick up
    // a different gun or reach through a wall after the user turns away.
    const uint32_t target=CombatSelectGaze(pawn,manager,world,pose.sampledHead,&position);
    if(target)CombatPickSelected(pawn,hand,target,manager,world);
    else CombatReportPickup(true);
}
static void CombatBeginCatch(int hand){if(g_combatSpatialReady)g_combatCatchUntil[hand]=DBL_MAX;}
static void CombatUpdatePickupDebug(uintptr_t pawn,uint32_t weapon)
{
    static double next=0;
    if(!g_pickupDebug||NowMs()<next)return;next=NowMs()+100;
    const char* savedGate=g_combatPickupGate;
    PickupDebugSnapshot debug{};debug.until=NowMs()+350;
    P13PoseSnapshot pose{};ReadP13PoseSnapshot(&pose);
    uint32_t manager=0,world=0;uintptr_t move=0;
    const bool context=CombatSpatialContext(pawn,&manager,&world,&pose,&move);
    _snprintf_s(debug.status,sizeof(debug.status),_TRUNCATE,"%s",weapon?"ARMED":context?"NO TARGET":g_combatPickupGate);
    if(!world)CombatReadField(pawn,CWorld,&world,4);
    debug.headValid=ReadPickupView(pawn,&pose.sampledHead);
    debug.head=pose.sampledHead;
    const bool fresh=pose.presentFrame<=g_frames&&g_frames-pose.presentFrame<=3;
    debug.hand[0]=pose.left.worldPosition;debug.hand[1]=pose.right.worldPosition;
    debug.handValid[0]=fresh&&pose.left.worldValid;debug.handValid[1]=fresh&&pose.right.worldValid;
    float best=FLT_MAX;
    if(g_combatSpatialReady&&world)for(const auto& pickup:g_combatPickups){
        MEVR_Vec3 position{};if(!pickup.object||debug.count>=8||!CombatPickupAlive(pickup.object,world,&position))continue;
        MEVR_Vec3 delta{(position.x-debug.head.position.x)/g_worldScale,(position.y-debug.head.position.y)/g_worldScale,(position.z-debug.head.position.z)/g_worldScale};
        const float distance=VecLength(delta),forward=delta.x*debug.head.forward.x+delta.y*debug.head.forward.y+delta.z*debug.head.forward.z;
        const bool cone=debug.headValid&&CombatGazeScore(forward,(std::max)(0.f,distance*distance-forward*forward),distance)>=0;
        bool eligible=false,visible=false;const char* reason="OUTSIDE CONE/RANGE";
        if(cone&&context){eligible=CombatPickupEligible(pawn,pickup.object,manager);
            reason=g_combatPickupGate;if(eligible){visible=CombatPickupVisible(pawn,debug.head.position,position);reason=visible?"READY - SQUEEZE EITHER GRIP":g_combatPickupGate;}}
        debug.targets[debug.count++]={position,!cone?D3DCOLOR_XRGB(255,230,20):eligible&&visible?D3DCOLOR_XRGB(20,120,255):D3DCOLOR_XRGB(255,80,30)};
        if(distance<best){best=distance;
            if(context&&!weapon)_snprintf_s(debug.status,sizeof(debug.status),_TRUNCATE,"%s",reason);
            uint32_t state[2]{};char stateName[24]="QUERY FAILED";
            if(CombatInvoke(pickup.object,CState,state))NameOf(state[0],stateName,sizeof(stateName));
            const float angle=acosf((std::max)(-1.f,(std::min)(1.f,distance>.001f?forward/distance:1.f)))*57.29578f;
            _snprintf_s(debug.detail,sizeof(debug.detail),_TRUNCATE,"%s  %d CM  %d DEG  N=%u",stateName,(int)(distance*100),(int)angle,debug.count);
        }
    }
    PublishPickupDebug(debug);g_combatPickupGate=savedGate;
}
static void CombatSpatialTick(uintptr_t pawn,uint32_t weapon)
{
    CombatUpdatePickupDebug(pawn,weapon);
    const double now=NowMs();
    const int held=(int)InterlockedCompareExchange(&g_combatHeldMask,0,0);
    for(int h=0;h<2;++h) {
        if(!(held&(1<<h))||!(held&(4<<h)))g_combatCatchUntil[h]=0;
        else if(!weapon)g_combatCatchUntil[h]=DBL_MAX;
    }
    const bool catching=now<g_combatCatchUntil[0]||now<g_combatCatchUntil[1];
    if(weapon||!g_combatSpatialReady){g_combatGazePickup=0;CombatCancelCatch();CombatPublishHighlight(false);return;}
    if(!catching&&now<g_combatNextGaze)return;
    uint32_t manager=0,world=0;P13PoseSnapshot pose{};uintptr_t move=0;
    if(!CombatSpatialContext(pawn,&manager,&world,&pose,&move)){
        g_combatGazePickup=0;CombatPublishHighlight(false);CombatReportPickup();return;
    }
    if(world!=g_combatPickupWorld){CombatSpatialReset();g_combatPickupWorld=world;}
    if(now>=g_combatNextScan){CombatSeedNearby(manager);CombatScanPickups(world);g_combatNextScan=now+100;}
    if(catching) for(auto& p:g_combatPickups) {
        if(!p.object)continue;
        MEVR_Vec3 position{};
        if(!CombatPickupAlive(p.object,world,&position)){p={};continue;}
        const auto previous=p.sampled>0&&now-p.sampled<80?p.previous:position;
        const float a[3]={previous.x,previous.y,previous.z},b[3]={position.x,position.y,position.z};
        p.previous=position;p.sampled=now;
        for(int h=0;h<2;++h) {
            const auto& hand=h==0?pose.left:pose.right;
            const float point[3]={hand.worldPosition.x,hand.worldPosition.y,hand.worldPosition.z};
            if(now>=g_combatCatchUntil[h]||!hand.worldValid||
                CombatSegmentDistanceSquared(point,a,b)>0.25f*0.25f*g_worldScale*g_worldScale ||
                !CombatPickupEligible(pawn,p.object,manager,true)||!CombatPickupVisible(pawn,hand.worldPosition,position))continue;
            if(CombatPickSelected(pawn,h,p.object,manager,world,true))return;
        }
    }
    if(now<g_combatNextGaze)return;
    g_combatNextGaze=now+100;
    g_combatGazePickup=CombatSelectGaze(pawn,manager,world,pose.sampledHead,&g_combatGazeLocation);
    CombatPublishHighlight(g_combatGazePickup!=0,g_combatGazeLocation);
    CombatReportPickup();
}
