// Reuse the verified ProcessInternal hook, at CalcCamera's output boundary.
// The renderer can only draw objects the engine submitted; rotating c0 alone
// cannot recover geometry rejected by the original scripted camera frustum.
static uintptr_t g_visibilityCalc=0,g_visibilityRotationProperty=0,g_flareSetActive=0;
static uint32_t g_flareClass=0,g_flareActiveMask=0;
static int g_flareActiveOffset=-1;
static bool g_visibilityCameraReady=false,g_visibilityFlareReady=false;

static uintptr_t CameraOutRotationProperty(uintptr_t fn)
{
    for(int head:{0x4C,0x70}){
        uint32_t p=0;if(!SafeU32(fn+head,&p))continue;
        for(int n=0;p>=0x10000&&n<64;++n){
            uint32_t owner=0,cls=0,offset=0,next=0;
            if(!SafeU32(p+g_offOuter,&owner)||owner!=fn)break;
            if(ObjNameIs(p,"out_Rotation")&&SafeU32(p+kUObjectClassOff,&cls)&&
                ObjNameIs(cls,"StructProperty")&&SafeU32(p+g_offPropOff,&offset)&&offset==16)return p;
            if(!SafeU32(p+g_offNext,&next)||next==p)break;p=next;
        }
    }
    return 0;
}

static void CameraVisibilityMetadata()
{
    g_visibilityCalc=PkFindFunction("CalcCamera","TdPlayerPawn");
    if(g_visibilityCalc)g_visibilityRotationProperty=CameraOutRotationProperty(g_visibilityCalc);
    g_flareSetActive=PkFindFunction("SetIsActive","LensFlareComponent");
    if(g_flareSetActive)SafeU32(g_flareSetActive+g_offOuter,&g_flareClass);
    FlagRef ref{};
    if(LookupFlag("LensFlareComponent","bIsActive",&ref)){
        g_flareActiveOffset=ref.off;g_flareActiveMask=ref.mask;
    }
}

static void CameraVisibilityValidate()
{
    // Shipped CallFunction constructs FFrame at ebp-38h, OutParms at ebp-20h
    // (+0x18). Each record stores Property/+0, PropAddr/+4 and Next/+8.
    // Check those actual instructions relative to the independently derived
    // interpreter, rather than assuming a UE3 layout on another executable.
    const BYTE frame[]={0x89,0x7D,0xDC,0x89,0x45,0xE0,0x8B,0x57,0x0C};
    const BYTE record[]={0x89,0x41,0x04,0x89,0x31,0x8B,0xD8,0x8B,0x02,
        0x85,0xC0,0x74,0x0D,0x89,0x48,0x08};
    BYTE actualFrame[sizeof(frame)]{},actualRecord[sizeof(record)]{};
    const bool abi=g_scriptTargetCached&&
        SafeRead(g_scriptTargetCached+(0x114A8EF-0x1148D50),actualFrame,sizeof(frame))&&
        SafeRead(g_scriptTargetCached+(0x114A96A-0x1148D50),actualRecord,sizeof(record))&&
        !memcmp(frame,actualFrame,sizeof(frame))&&!memcmp(record,actualRecord,sizeof(record));
    g_visibilityCameraReady=abi&&g_visibilityRotationProperty&&
        CombatParameter(g_visibilityCalc,"out_Rotation","StructProperty",16)&&
        CombatParameter(g_visibilityCalc,"ReturnValue","BoolProperty",32);
    g_visibilityFlareReady=g_flareClass&&g_flareActiveOffset>=0&&g_flareActiveMask&&
        CombatParameter(g_flareSetActive,"bInIsActive","BoolProperty",0);
    Log("[visibility] early scripted camera=%s (out-parameter ABI=%d), native lens-flare control=%s",
        g_visibilityCameraReady?"ready":"unavailable",abi?1:0,g_visibilityFlareReady?"ready":"unavailable");
}

static bool ComposeScriptedCameraRotation(const int32_t base[3],float yaw,float pitch,int32_t out[3])
{
    if(!std::isfinite(yaw)||!std::isfinite(pitch))return false;
    const float rad=3.14159265358979323846f/32768.0f;
    const float p=(float)(int16_t)base[0]*rad,y=(float)(int16_t)base[1]*rad,r=(float)(int16_t)base[2]*rad;
    const float sp=sinf(p),cp=cosf(p),sy=sinf(y),cy=cosf(y),sr=sinf(r),cr=cosf(r);
    const float right[3]={sr*sp*cy-cr*sy,sr*sp*sy+cr*cy,-sr*cp};
    const float up[3]={-(cr*sp*cy+sr*sy),cy*sr-cr*sp*sy,cr*cp};
    const float forward[3]={cp*cy,cp*sy,sp},pivot[3]={0,0,0};
    float m[16]{};for(int i=0;i<3;++i){m[i*4]=right[i];m[i*4+1]=up[i];m[i*4+3]=forward[i];}
    ApplyCameraRotation(m,true,right,pitch,pivot);
    ApplyCameraRotation(m,true,up,yaw,pivot);
    const float angles[3]={atan2f(m[11],sqrtf(m[3]*m[3]+m[7]*m[7])),atan2f(m[7],m[3]),atan2f(-m[8],m[9])};
    for(int i=0;i<3;++i){if(!std::isfinite(angles[i]))return false;out[i]=(int32_t)lroundf(angles[i]/rad);}
    return true;
}

static void CameraVisibilityAfterScript(uintptr_t object,uint32_t node,void* stack,void* result)
{
    if(!g_visibilityCameraReady||node!=g_visibilityCalc||object!=g_playerPawn)return;
    AcquireSRWLockExclusive(&g_scriptedCameraLock);
    g_scriptedCameraPawn=0;
    ReleaseSRWLockExclusive(&g_scriptedCameraLock);
    uint32_t firstPerson=0;
    if(!g_simulStereo||g_sceneSplitMono||g_scenePartialMono||!g_headTracking||
        !CinematicHeadLookRequested()||!CurrentViewTargetIsPawn(object)||
        !SafeU32((uintptr_t)result,&firstPerson)||firstPerson!=1||g_offCamRot<0)return;
    uint32_t link=0,address=0;
    if(!SafeU32((uintptr_t)stack+0x18,&link))return;
    for(int i=0;link>=0x10000&&i<16;++i){uint32_t rec[3]{};
        if(!SafeRead(link,rec,sizeof(rec)))return;
        if(rec[0]==g_visibilityRotationProperty){address=rec[1];break;}
        if(rec[2]==link)return;link=rec[2];
    }
    int32_t base[3]{},cached[3]{},corrected[3]{};
    if(address<0x10000||(address&3)||!SafeRead(address,base,12)||
        !SafeRead(object+g_offCamRot,cached,12)||memcmp(base,cached,12))return;
    if(!SampleScriptedHeadAngles(true)||
        !ComposeScriptedCameraRotation(base,g_cinematicYaw,g_cinematicPitch,corrected))return;
    if(!WriteRigBytes(address,corrected,12))return;
    if(!WriteRigBytes(object+g_offCamRot,corrected,12)){WriteRigBytes(address,base,12);return;}
    AcquireSRWLockExclusive(&g_scriptedCameraLock);
    g_scriptedCameraPawn=object;g_scriptedCameraFrame=g_frames;
    memcpy(g_scriptedCameraRotation,corrected,12);
    ReleaseSRWLockExclusive(&g_scriptedCameraLock);
    static long next=0;if(g_frames>=next){
        Log("[visibility] head look applied before engine culling frame=%ld yaw=%.2f pitch=%.2f deg",
            g_frames,g_cinematicYaw*57.29578f,g_cinematicPitch*57.29578f);next=g_frames+300;}
}

static void NativeLensFlareTick()
{
    if(g_lensFlares||!g_nativeLensFlareSuppression||!g_visibilityFlareReady)return;
    // Incremental object reads, never a full name/reflection scan on a frame.
    static uint32_t cursor=0;static long last=-1;
    if(last==g_frames)return;last=g_frames;
    uint32_t data=0,count=0;
    if(!SafeU32(g_gobjAddr,&data)||!SafeU32(g_gobjAddr+4,&count)||!count||count>2000000)return;
    if(cursor>=count)cursor=0;
    uint32_t objects[512]{};const uint32_t n=(std::min)(count-cursor,512u);
    if(!SafeRead(data+cursor*4,objects,n*4))return;cursor+=n;
    for(uint32_t i=0;i<n;++i){uint32_t cls=0,bits=0;const uintptr_t obj=objects[i];
        if(!obj||!SafeU32(obj+kUObjectClassOff,&cls)||cls!=g_flareClass||
            !SafeU32(obj+g_flareActiveOffset,&bits)||!(bits&g_flareActiveMask)||
            !LooksLikeRigObject(obj,"LensFlareComponent"))continue;
        uint32_t active=0;
        if(CombatCall(obj,g_flareSetActive,&active)){
            char name[96]{};ReadObjName(obj,name,sizeof(name));
            Log("[visibility] LensFlares=off deactivated native component %s (%p)",name,(void*)obj);
        }
    }
}
