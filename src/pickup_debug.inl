struct PickupDebugTarget{MEVR_Vec3 position{};DWORD color=0;};
struct PickupDebugSnapshot{
    RenderedHeadFrame head{};MEVR_Vec3 hand[2]{};bool headValid=false,handValid[2]{};
    PickupDebugTarget targets[8]{};unsigned count=0;
    char status[64]{},detail[64]{};double until=0;
};
static SRWLOCK g_pickupDebugLock=SRWLOCK_INIT;
static PickupDebugSnapshot g_pickupDebugSnapshot{};
static void PublishPickupDebug(const PickupDebugSnapshot& snapshot)
{
    AcquireSRWLockExclusive(&g_pickupDebugLock);g_pickupDebugSnapshot=snapshot;ReleaseSRWLockExclusive(&g_pickupDebugLock);
}
static bool ReadPickupDebug(PickupDebugSnapshot* out)
{
    if(!g_pickupDebug)return false;
    AcquireSRWLockShared(&g_pickupDebugLock);*out=g_pickupDebugSnapshot;ReleaseSRWLockShared(&g_pickupDebugLock);
    return NowMs()<out->until;
}
struct PickupDebugVertex{float x,y,z;DWORD color;};
static void PickupDebugLine(PickupDebugVertex* lines,int* n,MEVR_Vec3 a,MEVR_Vec3 b,DWORD color)
{
    if(*n+2>2048||!FiniteVec(a)||!FiniteVec(b))return;
    lines[(*n)++]={a.x,a.y,a.z,color};lines[(*n)++]={b.x,b.y,b.z,color};
}
static void PickupDebugBox(PickupDebugVertex* lines,int* n,MEVR_Vec3 center,MEVR_Vec3 extent,DWORD color)
{
    MEVR_Vec3 corners[8];for(int i=0;i<8;++i)corners[i]={center.x+((i&1)?1:-1)*extent.x,
        center.y+((i&2)?1:-1)*extent.y,center.z+((i&4)?1:-1)*extent.z};
    for(int i=0;i<8;++i)for(int bit=1;bit<=4;bit*=2)if(!(i&bit))PickupDebugLine(lines,n,corners[i],corners[i|bit],color);
}
static MEVR_Vec3 PickupDebugPoint(MEVR_Vec3 origin,MEVR_Vec3 forward,MEVR_Vec3 right,MEVR_Vec3 up,float d,float radius,float angle)
{
    const float c=cosf(angle)*radius,s=sinf(angle)*radius;
    return {origin.x+forward.x*d+right.x*c+up.x*s,origin.y+forward.y*d+right.y*c+up.y*s,origin.z+forward.z*d+right.z*c+up.z*s};
}
static void BuildPickupDebugLines(const PickupDebugSnapshot& s,PickupDebugVertex* lines,int* n)
{
    if(s.headValid){
        const auto f=s.head.forward;
        MEVR_Vec3 right{-f.y,f.x,0};float length=VecLength(right);
        if(length<.001f)right={1,0,0};else right={right.x/length,right.y/length,0};
        MEVR_Vec3 up{f.y*right.z-f.z*right.y,f.z*right.x-f.x*right.z,f.x*right.y-f.y*right.x};
        MEVR_Vec3 previous[32]{};bool first=true;
        // Same 15 degree/minimum radius gate; final cap follows the 2 m sphere.
        for(float metres:{.15f,.45f,1.0f,1.9318517f,1.98f,2.0f}){
            const float radius=(std::min)((std::max)(.12f,metres*.267949f),sqrtf((std::max)(0.0f,4.0f-metres*metres)))*g_worldScale;
            MEVR_Vec3 ring[32];for(int i=0;i<32;++i)ring[i]=PickupDebugPoint(s.head.position,f,right,up,metres*g_worldScale,radius,i*6.2831853f/32);
            for(int i=0;i<32;++i){PickupDebugLine(lines,n,ring[i],ring[(i+1)%32],D3DCOLOR_XRGB(30,210,220));
                if(!first&&i%4==0)PickupDebugLine(lines,n,previous[i],ring[i],D3DCOLOR_XRGB(30,210,220));previous[i]=ring[i];}
            first=false;
        }
    }
    for(int hand=0;hand<2;++hand)if(s.handValid[hand]){
        const DWORD color=hand?D3DCOLOR_XRGB(40,255,80):D3DCOLOR_XRGB(230,80,255);
        for(int plane=0;plane<3;++plane)for(int i=0;i<32;++i){
            MEVR_Vec3 axes[3]={{1,0,0},{0,1,0},{0,0,1}};
            const auto a=PickupDebugPoint(s.hand[hand],{},axes[plane],axes[(plane+1)%3],0,.25f*g_worldScale,i*6.2831853f/32);
            const auto b=PickupDebugPoint(s.hand[hand],{},axes[plane],axes[(plane+1)%3],0,.25f*g_worldScale,(i+1)*6.2831853f/32);
            PickupDebugLine(lines,n,a,b,color);
        }
    }
    for(unsigned i=0;i<s.count&&i<8;++i)PickupDebugBox(lines,n,s.targets[i].position,{8,8,8},s.targets[i].color);
}
