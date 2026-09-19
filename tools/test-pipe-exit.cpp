// Production pipe code with an in-process Unreal object/dispatcher fixture.
#include "../src/d3d9.cpp"
#include <cassert>
static uint32_t pawnA[256]{},pawnB[256]{},moveObject[256]{},volumeObject[256]{};
static int exitCalls=0,queryCalls=0,lastAnimation=-1;
static void Metadata() {
    // No UClass Children lists: the cooked owner-table route must resolve both
    // ClimbState and capability. Function parameter chains remain available.
    uint32_t objects[64][64]{},pointers[64]{},namePointers[64]{};
    std::vector<std::string> names;names.reserve(64);int used=0;
    auto make=[&](const char* name,int cls,int owner) {
        const int i=used++;names.emplace_back(name);namePointers[i]=(uint32_t)(uintptr_t)names.back().c_str();
        pointers[i]=(uint32_t)(uintptr_t)objects[i];objects[i][4]=i;
        objects[i][0x34/4]=pointers[cls];objects[i][0x20/4]=pointers[owner];return i;
    };
    make("Class",0,0);objects[0][0x34/4]=pointers[0];
    auto cls=[&](const char* name){for(int i=0;i<used;++i)if(names[i]==name)return i;return make(name,0,0);};
    const int functionClass=cls("Function");
    g_offName=16;g_offOuter=0x20;g_offPropOff=0x78;g_offBoolMask=0x7c;g_offNext=0x44;
    g_nameTextOff=0;g_nameWide=false;
    int stateOffset=-1;
    for(int i=0;i<PEFieldCount;++i){auto& f=g_pipeFields[i];const int owner=cls(f.owner),kind=cls(f.kind);
        const int obj=make(f.name,kind,owner);objects[obj][0x78/4]=0x180+i*4;
        if(!strcmp(f.kind,"BoolProperty"))objects[obj][0x7c/4]=4;
        if(i==PEState)stateOffset=0x180+i*4;
    }
    int badParam=-1;
    for(int i=0;i<PEFunctionCount;++i){const auto& f=g_pipeFunctions[i];const int owner=cls(f.owner);
        const int obj=make(f.name,functionClass,owner);if(i<=PEClosest)objects[obj][0x90/4]=0x400;
        int previous=-1;
        auto param=[&](const char* name,const char* kind,int offset){const int type=cls(kind);const int p=make(name,type,obj);
            objects[p][0x78/4]=offset;
            if(previous<0)objects[obj][0x4c/4]=pointers[p];else objects[previous][0x44/4]=pointers[p];previous=p;return p;};
        if(i==PELast)param("ReturnValue","IntProperty",0);
        if(i==PELocation){param("Index","IntProperty",0);param("ReturnValue","StructProperty",4);}
        if(i==PEClosest){param("LocationZ","FloatProperty",0);param("ReturnValue","IntProperty",4);}
        if(i==PEExit)badParam=param("ClimbAnimIndex","IntProperty",0);
    }
    uint32_t nameArray[]={(uint32_t)(uintptr_t)namePointers,(uint32_t)used};
    uint32_t objectArray[]={(uint32_t)(uintptr_t)pointers,(uint32_t)used};
    g_gnamesAddr=(uintptr_t)nameArray;g_gobjAddr=(uintptr_t)objectArray;
    PkPipeResolveMetadata();assert(g_pipeMetadataReady==1&&g_offClimbState==stateOffset);
    assert(g_pipeFields[PECanExit].mask==4);
    objects[badParam][0x78/4]=4;PkPipeResolveMetadata();assert(g_pipeMetadataReady==-1);
    puts("PASS cooked owner metadata, live state offset publication, native query and script-call parameter validation");
}
static void __fastcall Engine(void* self,void*,void* function,void* parameters,void*) {
    const int which=(int)(uintptr_t)function-1;
    if(which==PELast){++queryCalls;*(int*)parameters=20;}
    else if(which==PELocation){++queryCalls;assert(*(int*)parameters==20);const float v[]={100,200,800};memcpy((char*)parameters+4,v,12);}
    else if(which==PEClosest){const float z=*(float*)parameters;*(int*)((char*)parameters+4)=fabsf(z-800)<16?20:19;}
    else {assert(which==PEExit&&self==moveObject);++exitCalls;lastAnimation=*(int*)parameters;*(uint8_t*)((char*)self+g_pipeFields[PEState].offset)=1;}
}
static void Position(uint32_t* pawn,float z) {const float v[]={100,200,z};memcpy((char*)pawn+g_offActorLocation,v,12);}
static void SetState(uint32_t* pawn,uint8_t state) {*(uint8_t*)((char*)pawn+g_offMoveState)=state;}
static void ResetClimb(uint32_t* pawn) {
    PkPipeResetExit();g_playerPawn=(uintptr_t)pawn;g_pkMode=PK_CLIMB;g_pkPipePawn=g_playerPawn;
    g_pkPipeVol=(uintptr_t)volumeObject;g_pkPipeSrc=(uintptr_t)moveObject;g_pkPipeOff=g_pipeFields[PELadder].offset;
    moveObject[g_pipeFields[PEOwner].offset/4]=(uint32_t)g_playerPawn;
    *(uint8_t*)((char*)moveObject+g_pipeFields[PEState].offset)=0;
    SetState(pawn,21);Position(pawn,700);g_pkPipeOk=g_pkPipeGuard=true;g_pkPipeZMin=0;g_pkPipeZMax=1000;
    PkPipeGameTick(g_playerPawn);
}
int wmain(int argc,wchar_t** argv) {
    assert(argc==2);swprintf_s(g_logPath,L"%s\\pipe-exit.log",argv[1]);
    InitializeCriticalSection(&g_padLock);g_padLockReady=true;
    Metadata();
    uint32_t classes[2][24]{};
    const char* names[]={"TdMove_Climb","TdLadderVolume"};
    uint32_t namesArray[]={(uint32_t)(uintptr_t)names,2};g_gnamesAddr=(uintptr_t)namesArray;g_offName=16;g_nameWide=false;g_nameTextOff=0;
    classes[0][4]=0;classes[1][4]=1;
    moveObject[0x34/4]=(uint32_t)(uintptr_t)classes[0];volumeObject[0x34/4]=(uint32_t)(uintptr_t)classes[1];
    uint32_t vtable[]={(uint32_t)(uintptr_t)&Engine};moveObject[0]=volumeObject[0]=(uint32_t)(uintptr_t)vtable;
    g_peSlot=0;g_textLo=(uintptr_t)&Engine;g_textHi=g_textLo+4096;
    for(int i=0;i<PEFunctionCount;++i)g_pipeFunctions[i].fn=i+1;
    g_pipeFields[PELadder].offset=0x184;g_pipeFields[PEState].offset=0x180;
    g_pipeFields[PEOwner].offset=0x60;g_pipeFields[PECanExit].offset=0x194;g_pipeFields[PECanExit].mask=1;
    g_pipeFields[PEPlaying].offset=g_pipeFields[PEPrecise].offset=0x188;
    g_pipeFields[PEPlaying].mask=1;g_pipeFields[PEPrecise].mask=2;
    g_pipeFields[PELeft].offset=g_pipeFields[PESliding].offset=0x190;
    g_pipeFields[PELeft].mask=1;g_pipeFields[PESliding].mask=2;
    moveObject[0x184/4]=(uint32_t)(uintptr_t)volumeObject;volumeObject[0x194/4]=1;
    g_offMoves=0x80;g_offMoveState=0x64;g_offActorLocation=0x70;
    uint32_t moves[22]{};moves[21]=(uint32_t)(uintptr_t)moveObject;
    for(auto pawn:{pawnA,pawnB}){pawn[0x80/4]=(uint32_t)(uintptr_t)moves;pawn[0x84/4]=pawn[0x88/4]=22;}
    g_pipeMetadataReady=1;g_motionHands=g_parkour=true;g_xrState=XR_SESSION_STATE_FOCUSED;g_pkBypass=false;
    ResetClimb(pawnA);assert(queryCalls==2);
    uintptr_t source=0;uint32_t offset=0;
    assert(PkFindClimbVolumeWhere(&source,&offset)==(uintptr_t)volumeObject&&source==(uintptr_t)moveObject&&offset==0x184);
    moveObject[0x184/4]=0;pawnA[0xe4/4]=(uint32_t)(uintptr_t)volumeObject;
    assert(PkFindClimbVolumeWhere(&source,&offset)==0); // stale pawn contact is not the active Ladder
    moveObject[0x184/4]=(uint32_t)(uintptr_t)volumeObject;
    for(int i=0;i<5;++i)PkPipeGameTick(g_playerPawn);assert(queryCalls==2); // no repeated geometry queries
    assert(PkPipeClampZ(790)==790&&!g_pkTopOpen);
    assert(PkPipeClampZ(810)==800&&g_pkTopOpen); // authored 800, not guessed 910
    PkPipeGameTick(g_playerPawn);assert(exitCalls==0); // direct-drive write has not arrived yet
    Position(pawnA,800);PkPipeGameTick(g_playerPawn);assert(exitCalls==1&&lastAnimation==5);
    g_pkTopDeadline=1;g_pkTopOpenFrames=5000;PkPipeExtentTick();assert(g_pkTopOpen&&g_pkTopDeadline==0);
    for(int i=0;i<5;++i)PkPipeGameTick(g_playerPawn);assert(exitCalls==1); // never re-enter root motion
    SetState(pawnA,1);PkPipeExtentTick();PkPipeGameTick(g_playerPawn);assert(!g_pkTopOpen&&!g_pkPipeOk);
    // Death/new pawn, same volume: capability remains authored and immediately usable.
    ResetClimb(pawnB);pawnB[0x190/4]=1;assert(PkPipeClampZ(805)==800&&g_pkTopOpen);
    Position(pawnB,800);PkPipeGameTick(g_playerPawn);assert(exitCalls==2&&lastAnimation==4);
    // A refused/timed-out request is local to this climb, never a permanent no-roof verdict.
    ResetClimb(pawnB);moveObject[0x188/4]=1;PkPipeClampZ(805);Position(pawnB,800);
    PkPipeGameTick(g_playerPawn);assert(exitCalls==2);
    g_pkTopDeadline=1;PkPipeExtentTick();assert(!g_pkTopOpen&&g_pkTopRetryBlocked);
    PkPipeClampZ(810);assert(!g_pkTopOpen);
    SetState(pawnB,2);PkPipeExtentTick();PkPipeGameTick(g_playerPawn);moveObject[0x188/4]=0;
    ResetClimb(pawnB);PkPipeClampZ(810);Position(pawnB,800);PkPipeGameTick(g_playerPawn);assert(exitCalls==3);
    // No-roof pipe: keep the measured physical stop and never run an exit probe.
    SetState(pawnB,1);PkPipeGameTick(g_playerPawn);volumeObject[0x194/4]=0;ResetClimb(pawnB);
    assert(PkPipeClampZ(990)==910&&!g_pkTopOpen);Position(pawnB,800);PkPipeGameTick(g_playerPawn);assert(exitCalls==3);
    // Stale or mismatched snapshots cannot arm the game call.
    volumeObject[0x194/4]=1;PkPipeGameTick(g_playerPawn);
    auto good=PkPipeSnapshot();auto stale=good;stale.sampledAt=0;PipePublish(stale);assert(PkPipeClampZ(990)==910&&!g_pkTopOpen);
    stale=good;stale.pawn=(uintptr_t)pawnA;PipePublish(stale);assert(PkPipeClampZ(990)==910&&!g_pkTopOpen);
    PipePublish(good);PkPipeClampZ(810);g_pkTopRequestMove=0;PkPipeGameTick(g_playerPawn);assert(exitCalls==3);
    g_menuBlocksGameplay=1;PkPipeGameTick(g_playerPawn);assert(!PkPipeSnapshot().valid&&exitCalls==3);
    // Readiness uses native step identity and waits for both animation/precise-location ownership.
    using mevr::pipeExitReady;
    assert(pipeExitReady(true,0,20,20,800,800,false,false,false));
    assert(!pipeExitReady(true,0,19,20,800,800,false,false,false));
    assert(!pipeExitReady(true,0,20,20,820,800,false,false,false));
    assert(!pipeExitReady(true,0,20,20,800,800,false,true,false));
    assert(!pipeExitReady(true,0,20,20,800,800,false,false,true));
    assert(!pipeExitReady(true,0,20,20,NAN,800,false,false,false));
    puts("PASS authored top, engine exit call, both hands, committed animation, same-pipe respawn, timeout recovery");
    puts("PASS no-roof, stale/foreign snapshots, pending animation, precise location, menu, no repeated geometry queries");
}
