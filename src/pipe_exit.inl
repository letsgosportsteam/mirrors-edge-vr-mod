// Engine-side pipe queries and exit, called only from Update1pArms on the game thread.
// The input thread consumes an identity-checked snapshot; it never calls Unreal.
enum PipeFieldId { PELadder,PEState,PECanExit,PEPlaying,PEPrecise,PELeft,PESliding,PEOwner,PEFieldCount };
static struct PipeField { const char* owner;const char* name;const char* kind;int offset=-1;uint32_t mask=0; } g_pipeFields[]={
    {"TdMove_Climb","Ladder","ObjectProperty"}, {"TdMove_Climb","ClimbState","ByteProperty"},
    {"TdLadderVolume","bCanExitAtTop","BoolProperty"}, {"TdMove_Climb","bIsPlayingAnimation","BoolProperty"},
    {"TdMove","bUsePreciseLocation","BoolProperty"}, {"TdPawn","bClimbLeftHand","BoolProperty"},
    {"TdPawn","bClimbDownFast","BoolProperty"}, {"TdMove","PawnOwner","ObjectProperty"}
};
enum PipeFunctionId { PELast,PELocation,PEClosest,PEExit,PEFunctionCount };
static struct PipeFunction {const char* owner;const char* name;uintptr_t fn=0;} g_pipeFunctions[]={
    {"TdLadderVolume","GetLastStep"}, {"TdLadderVolume","GetLadderLocation"},
    {"TdLadderVolume","GetClosestStep"}, {"TdMove_Climb","ExitAtTop"}
};
static volatile LONG g_pipeMetadataReady=0;
static bool g_pipeCallFault=false;

static void PkPipeResolveMetadata()
{
    uint32_t data=0,count=0;
    if(g_offOuter<0 || g_offPropOff<0 || g_offBoolMask<0 ||
       !SafeU32(g_gobjAddr,&data) || !SafeU32(g_gobjAddr+4,&count) || count>2000000)return;
    for(uint32_t i=0;i<count;++i) {
        uint32_t object=0,outer=0,cls=0,ownerClass=0;char name[80]{},owner[80]{},kind[48]{};
        if(!SafeU32(data+i*4,&object) || object<0x10000 || !ReadObjName(object,name,sizeof(name)))continue;
        bool wanted=false;
        for(const auto& f:g_pipeFields)if(!strcmp(f.name,name))wanted=true;
        for(const auto& f:g_pipeFunctions)if(!strcmp(f.name,name))wanted=true;
        if(!wanted || !SafeU32(object+g_offOuter,&outer) || !ReadObjName(outer,owner,sizeof(owner)) ||
           !SafeU32(outer+kUObjectClassOff,&ownerClass) || !ObjNameIs(ownerClass,"Class") ||
           !SafeU32(object+kUObjectClassOff,&cls) || !ReadObjName(cls,kind,sizeof(kind)))continue;
        for(auto& f:g_pipeFields) {
            if(strcmp(f.name,name)||strcmp(f.owner,owner)||strcmp(f.kind,kind))continue;
            uint32_t offset=0,mask=0;
            if(!SafeU32(object+g_offPropOff,&offset)||offset>=0x8000)continue;
            if(!strcmp(kind,"BoolProperty") &&
               (!SafeU32(object+g_offBoolMask,&mask)||!mask||(mask&(mask-1))))continue;
            f.offset=(int)offset;f.mask=mask;
        }
        for(auto& f:g_pipeFunctions)
            if(!strcmp(f.name,name)&&!strcmp(f.owner,owner)&&!strcmp(kind,"Function"))f.fn=object;
    }
    bool ready=true;
    for(const auto& f:g_pipeFields) {Log("[pipe-exit] %s::%s offset=%d mask=%08X",f.owner,f.name,f.offset,f.mask);ready=ready&&f.offset>=0;}
    for(const auto& f:g_pipeFunctions)ready=ready&&f.fn!=0;
    // Non-indexed native queries accept the normal ProcessEvent parameter buffer.
    for(int i=PELast;i<=PEClosest;++i) {
        uint32_t flags=0;uint16_t index=0;
        ready=SafeU32(g_pipeFunctions[i].fn+0x90,&flags) && (flags&0x400) &&
            SafeRead(g_pipeFunctions[i].fn+0x94,&index,2) && index==0 && ready;
    }
    ready=CombatParameter(g_pipeFunctions[PELast].fn,"ReturnValue","IntProperty",0)&&ready;
    ready=CombatParameter(g_pipeFunctions[PELocation].fn,"Index","IntProperty",0)&&ready;
    ready=CombatParameter(g_pipeFunctions[PELocation].fn,"ReturnValue","StructProperty",4)&&ready;
    ready=CombatParameter(g_pipeFunctions[PEClosest].fn,"LocationZ","FloatProperty",0)&&ready;
    ready=CombatParameter(g_pipeFunctions[PEClosest].fn,"ReturnValue","IntProperty",4)&&ready;
    ready=CombatParameter(g_pipeFunctions[PEExit].fn,"ClimbAnimIndex","IntProperty",0)&&ready;
    // Publish the actual byte property even when optional exit metadata is absent.
    if(g_pipeFields[PEState].offset>=0)g_offClimbState=g_pipeFields[PEState].offset;
    Log("[pipe-exit] authored top/roof metadata and function parameters: %s",ready?"ready":"unavailable; automatic exit disabled");
    InterlockedExchange(&g_pipeMetadataReady,ready?1:-1);
}
static bool PipeRead(uintptr_t object,int field,void* value,size_t bytes)
{
    return object && g_pipeFields[field].offset>=0 && SafeRead(object+g_pipeFields[field].offset,value,bytes);
}
static bool PipeBool(uintptr_t object,int field,bool* value)
{
    uint32_t bits=0;if(!g_pipeFields[field].mask||!PipeRead(object,field,&bits,4))return false;
    *value=(bits&g_pipeFields[field].mask)!=0;return true;
}
static bool PkPipeReadVolume(uintptr_t move,uintptr_t* volume,uint32_t* offset)
{
    if(InterlockedCompareExchange(&g_pipeMetadataReady,0,0)!=1)return false;
    *volume=0;*offset=(uint32_t)g_pipeFields[PELadder].offset;
    char kind[64]{};uint32_t active=0;
    if(ReadClassName(move,kind,sizeof(kind))&&!strcmp(kind,"TdMove_Climb")&&
       PipeRead(move,PELadder,&active,4)&&active&&IsAOfClass(active,"TdLadderVolume"))*volume=active;
    return true;
}
static bool PipeCall(uintptr_t object,int function,void* parameters)
{
    uint32_t vt=0,pe=0;
    if(g_pipeCallFault||g_peSlot<0||!g_pipeFunctions[function].fn||!SafeU32(object,&vt)||
       !SafeU32(vt+g_peSlot*4,&pe)||pe<g_textLo||pe>=g_textHi)return false;
    __try {((PkProcessEventFn)pe)((void*)object,(void*)g_pipeFunctions[function].fn,parameters,nullptr);}
    __except(EXCEPTION_EXECUTE_HANDLER){g_pipeCallFault=true;Log("[pipe-exit] engine call fault; automatic exit disabled");return false;}
    return true;
}
static void PipePublish(const mevr::PipeExitSnapshot& snapshot)
{
    if(!g_padLockReady)return;
    EnterCriticalSection(&g_padLock);g_pipeExitSnapshot=snapshot;LeaveCriticalSection(&g_padLock);
}
static void PkPipeGameTick(uintptr_t pawn)
{
    static mevr::PipeExitSnapshot cached;
    uint8_t movement=255;uintptr_t move=0;uint32_t volume=0,owner=0;char kind[64]{};
    if(!g_motionHands||!g_parkour||g_pkBypass||g_menuBlocksGameplay||g_xrState!=XR_SESSION_STATE_FOCUSED||
       pawn!=g_playerPawn||InterlockedCompareExchange(&g_pipeMetadataReady,0,0)!=1||
       g_offMoveState<0||!SafeRead(pawn+g_offMoveState,&movement,1)||movement!=21||
       !ReadMoveClassName(pawn,movement,kind,sizeof(kind),&move)||strcmp(kind,"TdMove_Climb")||
       !PipeRead(move,PEOwner,&owner,4)||owner!=pawn||
       !PipeRead(move,PELadder,&volume,4)||!volume||!IsAOfClass(volume,"TdLadderVolume")) {
        cached={};PipePublish(cached);return;
    }
    if(g_peSlot==-1)DumpVtableSlots(pawn,"pipe pawn",move,"pipe move",120);
    if(g_peSlot<0||g_pipeCallFault){PipePublish({});return;}
    if(!cached.valid||cached.pawn!=pawn||cached.move!=move||cached.volume!=volume) {
        cached={};int last=-1;
        if(!PipeCall(volume,PELast,&last)||last<0||last>4096){PipePublish({});return;}
        struct Location {int index;float value[3];} location{last,{NAN,NAN,NAN}};
        if(!PipeCall(volume,PELocation,&location)||!std::isfinite(location.value[0])||
           !std::isfinite(location.value[1])||!std::isfinite(location.value[2])){PipePublish({});return;}
        cached.pawn=pawn;cached.move=move;cached.volume=volume;cached.lastStep=last;cached.valid=true;
        memcpy(cached.top,location.value,12);
        Log("[pipe-exit] pawn=%08X ladder=%08X lastStep=%d authored exit=(%.1f %.1f %.1f)",
            (uint32_t)pawn,volume,last,location.value[0],location.value[1],location.value[2]);
    }
    if(!PipeBool(volume,PECanExit,&cached.canExit)||!PipeRead(move,PEState,&cached.state,1)) {PipePublish({});return;}
    cached.sampledAt=GetTickCount64();PipePublish(cached);
    if(!g_pkTopOpen||g_pkPipePawn!=pawn||g_pkPipeVol!=volume||g_pkTopRequestMove!=move||
       !cached.canExit||cached.state!=0)return;
    float position[3];bool playing=false,precise=false,left=false,sliding=false;
    if(g_offActorLocation<0||!SafeRead(pawn+g_offActorLocation,position,12)||
       !PipeBool(move,PEPlaying,&playing)||!PipeBool(move,PEPrecise,&precise)||
       !PipeBool(pawn,PELeft,&left)||!PipeBool(pawn,PESliding,&sliding))return;
    // Never start root motion from a teleported-above-top pose or another face
    // of the pipe. The input clamp stops at this authored step before requesting.
    const float dx=position[0]-cached.top[0],dy=position[1]-cached.top[1];
    if(dx*dx+dy*dy>32.f*32.f)return;
    struct Closest {float z;int value;} closest{position[2],-1};
    if(!PipeCall(volume,PEClosest,&closest)||!mevr::pipeExitReady(cached.canExit,cached.state,
        closest.value,cached.lastStep,position[2],cached.top[2],playing,precise,sliding))return;
    int animation=left?4:5; // Same hand-dependent choice as TdMove_Climb.HandleClimbAction.
    if(PipeCall(move,PEExit,&animation)) {
        PipeRead(move,PEState,&cached.state,1);cached.sampledAt=GetTickCount64();PipePublish(cached);
        Log("[pipe-exit] ExitAtTop at Z %.1f step=%d animation=%d confirmed state=%u",
            position[2],closest.value,animation,(unsigned)cached.state);
    }
}
