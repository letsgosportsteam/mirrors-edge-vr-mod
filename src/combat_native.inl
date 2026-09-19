// This game's ProcessEvent returns without invoking UFunctions with iNative!=0
// (0114AC24). These two indexed natives consume bytecode, not a parms buffer.
// Keep this adapter restricted to the verified, read-only queries below.
static bool CombatIndexedQuery(uintptr_t object,int fn,void* parms)
{
    if(!object||!parms||g_combatFault||(fn!=CState&&fn!=CFastTrace))return false;
    const uintptr_t function=g_combatFunctions[fn].fn;
    uint32_t flags=0,exec=0;uint16_t index=0;
    if(g_funcOffsetCached!=0xAC||!SafeU32(function+0x90,&flags)||!(flags&0x400)||
        !SafeRead(function+0x94,&index,2)||index!=(fn==CState?284:548)||
        !SafeU32(function+g_funcOffsetCached,&exec)||exec<g_textLo||exec>=g_textHi)return false;
    unsigned char code[48]{};unsigned used=0;
    if(fn==CFastTrace){
        // EX_VectorConst: TraceEnd, TraceStart, BoxExtent; EX_True/False.
        for(int i=0;i<3;++i){code[used++]=0x23;memcpy(code+used,(char*)parms+i*12,12);used+=12;}
        uint32_t bullet=0;memcpy(&bullet,(char*)parms+36,4);code[used++]=bullet?0x27:0x28;
    }
    code[used++]=0x16; // EX_EndFunctionParms
    code[used]=0x0B;   // EX_Nothing, readable after P_FINISH's optional debug-info check
    // FFrame prefix verified against 0100D2C0 / execFastTrace in the shipped EXE.
    // No local/property expressions or nested calls occur in this constant stream.
    struct Frame{void* vtable;void* node;void* object;unsigned char* code;void* locals;void* previous;void* outParms;};
    Frame frame{nullptr,(void*)function,(void*)object,code,nullptr,nullptr,nullptr};
    void* result=(char*)parms+(fn==CState?0:40);
    memset(result,0xCD,fn==CState?8:4);
    __try { ((PFN_Update1pArms)exec)((void*)object,nullptr,&frame,result); }
    __except(EXCEPTION_EXECUTE_HANDLER){g_combatFault=true;Log("[pickup-native] indexed query fault; interactions disabled");return false;}
    uint32_t value=0;memcpy(&value,result,4);
    const bool ok=frame.code==code+used && value!=0xCDCDCDCD && (fn==CState||value==0||value==1);
    static bool logged[2]{};const int which=fn==CState?0:1;
    if(!logged[which]){logged[which]=true;Log("[pickup-native] %s index=%u exec=%p wrote=%d value=%08X consumed=%u/%u",
        fn==CState?"GetStateName":"FastTrace",index,(void*)(uintptr_t)exec,ok?1:0,value,(unsigned)(frame.code-code),used);}
    return ok;
}
