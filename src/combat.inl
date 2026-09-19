// Included after the reflection/rig helpers in d3d9.cpp. No game assets are modified.
#include "combat_gestures.h"

static bool g_combatReady = false, g_combatFault = false;
static int g_combatMesh1p = -1, g_combatMuzzleName = -1;
static uintptr_t g_combatSocketFn = 0, g_combatSpreadFn = 0;
static PFN_Update1pArms g_combatOriginalScript = nullptr;
static uintptr_t g_combatStartFn = 0, g_combatPawnStartFn = 0, g_combatAimFn = 0;
static uintptr_t g_combatPlayerWeapon = 0;
static long g_combatStartEntries = 0, g_combatAimEntries = 0;
static long g_combatStartCalls = 0, g_combatAimCalls = 0;
static volatile LONG g_combatMetadataState = 0; // 0 not started, 1 reading, 2 published, -1 failed

static bool CombatTraceDiagnosticDue(long call, long frame, long* lastFrame)
{
    // The trace getter runs during ordinary updates, often twice a frame. Keep
    // the first few samples, then at most one per 600 frames; counters stay exact.
    if (call <= 3 || frame - *lastFrame >= 600) {
        *lastFrame = frame;
        return true;
    }
    return false;
}

static bool CombatPistolClass(uintptr_t weapon)
{
    char name[96] = "";
    if (!ReadClassName(weapon, name, sizeof(name))) return false;
    // Both meshes and their +Z barrel axes were read from the shipped assets.
    return !strcmp(name, "TdWeapon_Pistol_Colt1911") ||
           !strcmp(name, "TdWeapon_Pistol_Glock18c");
}

static bool PistolCalibrationActive()
{
    uint32_t weapon = 0;
    return g_pistolHands && g_offWeapon >= 0 && g_playerPawn &&
        SafeU32(g_playerPawn + g_offWeapon, &weapon) && weapon && CombatPistolClass(weapon);
}

static bool CombatMuzzleRejected(const char* why)
{
    static const char* previous = nullptr;
    static long last = -600;
    if (why != previous || g_frames-last >= 600) {
        Log("[combat] shot override unavailable: %s", why);
        previous = why; last = g_frames;
    }
    return false;
}

static bool PistolAllowsHandControl(uintptr_t pawn, uintptr_t weapon, uint8_t anim)
{
    if (!g_motionHands || !g_pistolHands || !g_combatReady || g_combatFault ||
        !weapon || (anim != 1 && anim != 2) || !CombatPistolClass(weapon)) return false;
    uint8_t move = 0xFF;
    // Armed parkour, reload, throw, holster, melee and disarm retain authored ownership.
    return g_offMoveState >= 0 && SafeRead(pawn + g_offMoveState, &move, 1) &&
           (move == 1 || move == 2 || move == 11 || move == 15 || move == 16 ||
            move == 24 || move == 29 || move == 30);
}

// Verify each parameter against its Function owner, name, type and offset. UFunction's
// child chains differ from UClass's in this build (ENGINE_NOTES, ProcessEvent section).
static bool CombatParameter(uintptr_t fn, const char* name, const char* kind, uint32_t offset)
{
    if (!fn || g_offNext < 0 || g_offOuter < 0 || g_offPropOff < 0) {
        Log("[combat] cannot validate parameter %s: function=%p next=%d outer=%d propertyOffset=%d",
            name, (void*)fn, g_offNext, g_offOuter, g_offPropOff);
        return false;
    }
    for (int head : { 0x4C, 0x70 }) {
        uint32_t cur = 0;
        if (!SafeU32(fn + head, &cur)) continue;
        for (int i = 0; cur >= 0x10000 && i < 64; ++i) {
            uint32_t owner = 0, cls = 0, off = 0, next = 0;
            if (!SafeU32(cur + g_offOuter, &owner) || owner != fn) break;
            if (ObjNameIs(cur, name) && SafeU32(cur + kUObjectClassOff, &cls) &&
                ObjNameIs(cls, kind) && SafeU32(cur + g_offPropOff, &off) && off == offset)
                return true;
            if (!SafeU32(cur + g_offNext, &next) || next == cur) break;
            cur = next;
        }
    }
    Log("[combat] parameter validation failed: %s (%s at +%u)", name, kind, offset);
    return false;
}

static bool CombatCall(uintptr_t object, uintptr_t fn, void* parms)
{
    uint32_t vt = 0, pe = 0;
    if (g_combatFault || g_peSlot < 0 || !fn || !SafeU32(object, &vt) ||
        !SafeU32(vt + g_peSlot * 4, &pe) || pe < g_textLo || pe >= g_textHi) return false;
    __try {
        ((PkProcessEventFn)pe)((void*)object, (void*)fn, parms, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_combatFault = true;
        Log("*** [combat] engine call faulted; pistol tracking disabled for this run");
        return false;
    }
    return true;
}

#include "camera_visibility.inl"
#include "combat_interaction.inl"

static bool CombatMuzzle(uintptr_t pawn, uintptr_t* outWeapon, MEVR_Vec3* position,
                         int32_t aim[3])
{
    uint32_t weapon = 0, mesh = 0;
    uint8_t anim = 0;
    if (pawn != g_playerPawn || g_combatMesh1p < 0 || g_combatMuzzleName < 0 ||
        !CurrentViewTargetIsPawn(pawn) || g_offWeapon < 0 || g_offWeaponAnimState < 0 ||
        !SafeU32(pawn + g_offWeapon, &weapon) ||
        !SafeRead(pawn + g_offWeaponAnimState, &anim, 1) ||
        !PistolAllowsHandControl(pawn, weapon, anim) ||
        !(CombatHoldingHand() == 0 ? g_p13LeftState.owned : g_p13RightState.owned) ||
        (CombatHoldingHand() == 0 ? g_p13LeftState.ownedPawn : g_p13RightState.ownedPawn) != pawn ||
        g_wristWriteFault) return CombatMuzzleRejected("player/weapon/hand ownership");
    P13PoseSnapshot pose{};
    ReadP13PoseSnapshot(&pose);
    const auto flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
                       XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    const auto& hand = CombatHoldingHand() == 0 ? pose.left : pose.right;
    if (!hand.active || !hand.worldValid || (hand.flags & flags) != flags ||
        pose.presentFrame <= 0 || g_frames-pose.presentFrame < 0 ||
        g_frames-pose.presentFrame > 3) return CombatMuzzleRejected("fresh tracked holding-hand pose");
    if (CombatHoldingHand() == 0 && !CombatLeftBoneReady(weapon))
        return CombatMuzzleRejected("left-hand weapon bone not composed yet");
    if (!SafeU32(weapon + g_combatMesh1p, &mesh) ||
        !LooksLikeRigObject(mesh, "TdSkeletalMeshComponent")) return CombatMuzzleRejected("first-person weapon mesh");
    struct SocketParms {
        uint32_t name[2]; MEVR_Vec3 location; int32_t rotation[3]; uint32_t found;
    } p{};
    static_assert(sizeof(SocketParms) == 36, "socket ABI");
    if (!SafeRead(weapon + g_combatMuzzleName, p.name, sizeof(p.name)) ||
        !CombatCall(mesh, g_combatSocketFn, &p) || p.found != 1 || !FiniteVec(p.location))
        return CombatMuzzleRejected("muzzle socket result");
    MEVR_Vec3 pawnLocation{};
    if (!SafeRead(pawn + g_offActorLocation, &pawnLocation, sizeof(pawnLocation)) ||
        VecLength({p.location.x-pawnLocation.x, p.location.y-pawnLocation.y,
                   p.location.z-pawnLocation.z}) > 300.0f)
        return CombatMuzzleRejected("muzzle outside player reach");
    // Colt/Glock Muzzleflash sockets have identity relative rotation on Wep_Root.
    // Their local +Z points down the barrel. UE rotators use negative Y for pitch
    // and negative X for roll in the conventional quaternion representation.
    const float half = 3.14159265358979323846f / 65536.0f;
    const float hp = (float)(int16_t)p.rotation[0]*half;
    const float hy = (float)(int16_t)p.rotation[1]*half;
    const float hr = (float)(int16_t)p.rotation[2]*half;
    const XrQuaternionf q = MultiplyQuaternion(
        MultiplyQuaternion({0,0,sinf(hy),cosf(hy)}, {0,-sinf(hp),0,cosf(hp)}),
        {-sinf(hr),0,0,cosf(hr)});
    const MEVR_Vec3 barrel = RotateByQuaternion(q, {0,0,1});
    const float units = 32768.0f / 3.14159265358979323846f;
    aim[0] = (int32_t)lroundf(atan2f(barrel.z, sqrtf(barrel.x*barrel.x+barrel.y*barrel.y))*units);
    aim[1] = (int32_t)lroundf(atan2f(barrel.y, barrel.x)*units);
    aim[2] = 0;
    *position = p.location; *outWeapon = weapon;
    return true;
}

static void CombatStartTrace(void* self, void* result)
{
    MEVR_Vec3 muzzle{}; int32_t aim[3]{}; uintptr_t weapon = 0;
    if (result && CombatMuzzle((uintptr_t)self, &weapon, &muzzle, aim) &&
        WriteRigBytes((uintptr_t)result, &muzzle, sizeof(muzzle))) {
        ++g_combatStartCalls;
        static long lastReportFrame = 0;
        if (CombatTraceDiagnosticDue(g_combatStartCalls, g_frames, &lastReportFrame))
            Log("[combat] muzzle trace #%ld weapon=%p start=(%.2f %.2f %.2f) barrel P/Y=%d/%d",
                g_combatStartCalls, (void*)weapon, muzzle.x, muzzle.y, muzzle.z, aim[0], aim[1]);
    }
}

static void CombatAdjustedAim(void* self, void* result)
{
    MEVR_Vec3 muzzle{}; int32_t aim[3]{}; uintptr_t weapon = 0;
    if (!result || !CombatMuzzle(g_playerPawn, &weapon, &muzzle, aim) || weapon != (uintptr_t)self) return;
    struct SpreadParms { int32_t base[3], value[3]; } p{};
    memcpy(p.base, aim, sizeof(aim));
    memset(p.value, 0xCD, sizeof(p.value));
    // Keep the game's spread calculation, bypassing its head-based sticky/adjusted aim.
    if (CombatCall(weapon, g_combatSpreadFn, &p) && p.value[0] != (int32_t)0xCDCDCDCD &&
        WriteRigBytes((uintptr_t)result, p.value, sizeof(p.value))) {
        ++g_combatAimCalls;
        Log("[combat] barrel aim #%ld P/Y=%d/%d (+ game spread); muzzle traces=%ld",
            g_combatAimCalls, p.value[0], p.value[1], g_combatStartCalls);
    }
}

static bool CombatPrepareFire(uintptr_t object,uint32_t node,uintptr_t* restoreAddress,uint8_t* previous)
{
    if(!g_combatInteractionReady||object!=g_combatPlayerWeapon||
        node!=g_combatFunctions[CWeaponStartFire].fn||g_xrState!=XR_SESSION_STATE_FOCUSED)return false;
    uint8_t wall=0;
    if(!CombatReadField(g_playerPawn,CAgainstWall,&wall,1))return false;
    Log("[combat-fire] StartFire frame=%ld weapon=%p bodyWall=%u",g_frames,(void*)object,(unsigned)wall);
    if(wall!=1)return false;
    uintptr_t weapon=0;MEVR_Vec3 muzzle{};int32_t aim[3]{};P13PoseSnapshot pose{};
    if(!CombatMuzzle(g_playerPawn,&weapon,&muzzle,aim)||weapon!=object||
        !ReadP13PoseSnapshot(&pose)||!pose.sampledHeadValid)return false;
    // The stock lowered-weapon wall test follows Faith's authored gun position.
    // For a tracked pistol, use the actual unobstructed head-to-muzzle segment.
    struct Trace{MEVR_Vec3 end,start,extent;uint32_t bullet,result;} trace{muzzle,pose.sampledHead.position,{},0,0};
    if(!CombatInvoke(g_playerPawn,CFastTrace,&trace)||!trace.result){
        Log("[combat-fire] muzzle obstructed: stock firing gate retained");return false;
    }
    *restoreAddress=g_playerPawn+g_combatFields[CAgainstWall].offset;*previous=wall;
    const uint8_t clear=0;
    if(!WriteRigBytes(*restoreAddress,&clear,1))return false;
    Log("[combat-fire] clear tracked muzzle: body-wall firing block bypassed for this call");
    return true;
}

static void CombatLogAttack(uintptr_t object,uint32_t node)
{
    if(!g_combatInteractionReady||object!=g_playerCtl||node!=g_combatFunctions[CAttack].fn||
        !g_combatPlayerWeapon)return;
    uint8_t anim=255,wall=255;bool ignored=false;uint32_t state[2]{};char stateName[64]="unknown";
    SafeRead(g_playerPawn+g_offWeaponAnimState,&anim,1);CombatReadField(g_playerPawn,CAgainstWall,&wall,1);
    CombatBoolCall(g_playerCtl,CIgnore,&ignored);
    if(CombatInvoke(g_combatPlayerWeapon,CState,state))NameOf(state[0],stateName,sizeof(stateName));
    Log("[combat-fire] AttackPress frame=%ld anim=%u state=%s bodyWall=%u inputIgnored=%d",
        g_frames,(unsigned)anim,stateName,(unsigned)wall,ignored?1:0);
}

static void __fastcall CombatProcessScript(void* self, void* edx, void* stack, void* result)
{
    // CallFunction's script branch calls ProcessInternal directly (shipped executable
    // 0114AB09); changing UFunction::Func alone misses those calls. Preserve the shared
    // interpreter and filter EXACT function identities after it returns. No bytecode,
    // function flags, parameters, or unrelated script results are changed.
    uint32_t node = 0;
    const uintptr_t object = (uintptr_t)self;
    const bool relevant = object && (object == g_playerPawn || object == g_playerCtl || object == g_combatPlayerWeapon ||
        object == g_combatMeleeMove || object == g_vrZiplineMove ||
        object == g_combatPickupOverrideManager || g_combatWatchingDrop);
    if (relevant) SafeU32((uintptr_t)stack + 4, &node); // validated machine-code load below
    uintptr_t wallRestore=0;uint8_t previousWall=0;
    if(relevant)CombatLogAttack(object,node);
    const bool restoreWall=relevant&&CombatPrepareFire(object,node,&wallRestore,&previousWall);
    if (relevant) CombatBeforeScript(object, node);
    g_combatOriginalScript(self, edx, stack, result);
    if(relevant)CameraVisibilityAfterScript(object,node,stack,result);
    if(restoreWall)WriteRigBytes(wallRestore,&previousWall,1);
    if(relevant) CombatSpatialAfterScript(object,node,result);
    if (relevant) CombatAfterScript(object, node);
    if (!relevant) return;
    if (object == g_playerPawn && (node == g_combatStartFn || node == g_combatPawnStartFn)) {
        ++g_combatStartEntries;
        if (g_combatReady) CombatStartTrace(self, result);
    } else if (object == g_combatPlayerWeapon && node == g_combatAimFn) {
        ++g_combatAimEntries;
        if (g_combatReady) CombatAdjustedAim(self, result);
    }
}

static DWORD WINAPI CombatMetadataThread(LPVOID)
{
    const double started = NowMs();
    // Reflection metadata only, matching the existing background layout scan.
    // Never call game functions, install detours or read a live pawn here.
    g_combatStartFn = PkFindFunction("GetWeaponStartTraceLocation", "Pawn");
    g_combatPawnStartFn = PkFindFunction("GetWeaponStartTraceLocation", "TdPawn");
    g_combatAimFn = PkFindFunction("GetAdjustedAim", "TdWeapon");
    g_combatSocketFn = PkFindFunction("GetSocketWorldLocationAndRotation", "SkeletalMeshComponent");
    g_combatSpreadFn = PkFindFunction("AddSpread", "Weapon");
    DetachedPropRequest fields[] = {
        {"TdWeapon", "Mesh1p", "ComponentProperty", &g_combatMesh1p},
        {"TdWeapon", "MuzzleFlashSocket", "NameProperty", &g_combatMuzzleName}
    };
    LookupDetachedPropsBatch(fields, 2);
    CombatInteractionMetadata();
    CameraVisibilityMetadata();
    Log("[perf] pistol metadata lookup took %.2f ms (background thread)", NowMs() - started);
    InterlockedExchange(&g_combatMetadataState, 2);
    return 0;
}

static void InitializeCombatHooks(uintptr_t pawn)
{
    static bool attempted = false;
    if (attempted || !g_motionHands || (!g_pistolHands && !g_motionPunch) ||
        InterlockedCompareExchange(&g_objModelThreadFinished, 0, 0) != 1 ||
        !LooksLikePlayerPawn(pawn)) return;
    uintptr_t move = 0; char moveName[64] = ""; uint8_t state = 0;
    if (g_offMoveState < 0 || !SafeRead(pawn + g_offMoveState, &state, 1) ||
        !ReadMoveClassName(pawn, state, moveName, sizeof(moveName), &move) || !move) return;
    const LONG metadata = InterlockedCompareExchange(&g_combatMetadataState, 1, 0);
    if (metadata == 0) {
        HANDLE thread = CreateThread(nullptr, 0, CombatMetadataThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        else {
            InterlockedExchange(&g_combatMetadataState, -1);
            Log("[combat] cannot start metadata reader; pistol hooks remain disabled (error %lu)", GetLastError());
        }
        return;
    }
    if (metadata != 2) return; // Interlocked publication keeps partial metadata invisible.
    attempted = true;
    const double setupStarted = NowMs();
    // Derive the event dispatcher independently of the optional parkour census.
    if (g_peSlot == -1) DumpVtableSlots(pawn, "combat pawn", move, "combat move", 120);
    uintptr_t script = 0;
    const int funcOffset = DeriveUFunctionFuncOffset(&script);
    const uintptr_t start = g_combatStartFn;
    const uintptr_t pawnStart = g_combatPawnStartFn;
    const uintptr_t adjusted = g_combatAimFn;
    uint32_t startFunc = 0, aimFunc = 0;
    bool valid = g_peSlot >= 0 && funcOffset >= 0 && script &&
                 g_combatMesh1p >= 0 && g_combatMuzzleName >= 0;
    // Evaluate every check, even when an earlier one failed. The first test only
    // reported "layout failed", concealing all checks after the mesh lookup.
    valid = CombatParameter(start, "ReturnValue", "StructProperty", 4) && valid;
    valid = CombatParameter(pawnStart, "ReturnValue", "StructProperty", 4) && valid;
    valid = CombatParameter(adjusted, "StartFireLoc", "StructProperty", 0) && valid;
    valid = CombatParameter(adjusted, "ReturnValue", "StructProperty", 12) && valid;
    valid = CombatParameter(g_combatSocketFn, "InSocketName", "NameProperty", 0) && valid;
    valid = CombatParameter(g_combatSocketFn, "OutLocation", "StructProperty", 8) && valid;
    valid = CombatParameter(g_combatSocketFn, "OutRotation", "StructProperty", 20) && valid;
    valid = CombatParameter(g_combatSocketFn, "ReturnValue", "BoolProperty", 32) && valid;
    valid = CombatParameter(g_combatSpreadFn, "BaseAim", "StructProperty", 0) && valid;
    valid = CombatParameter(g_combatSpreadFn, "ReturnValue", "StructProperty", 12) && valid;
    const bool startDispatch = start && funcOffset >= 0 &&
        SafeU32(start + funcOffset, &startFunc) && startFunc == script;
    const bool aimDispatch = adjusted && funcOffset >= 0 &&
        SafeU32(adjusted + funcOffset, &aimFunc) && aimFunc == script;
    valid = valid && startDispatch && aimDispatch;
    Log("[combat] layout: eventSlot=%d funcOffset=%d mesh=%d muzzleName=%d"
        " start=%p/%p aim=%p/%p expectedScript=%p parametersAndFields=%s",
        g_peSlot, funcOffset, g_combatMesh1p, g_combatMuzzleName,
        (void*)start, (void*)(uintptr_t)startFunc, (void*)adjusted,
        (void*)(uintptr_t)aimFunc, (void*)script, valid ? "valid" : "FAILED");
    if (!valid) { Log("*** [combat] pistol hooks not installed: runtime layout validation failed"); return; }
    // This sequence reads argument 1 as FFrame*, then Node at +4 and FunctionFlags
    // at +0x90. The complete function was disassembled; both exits pop 8 bytes.
    // Resolve by the reflection census, and refuse an unrecognized ABI/build.
    const uint8_t frameLoad[] = {0x53,0x8B,0x5C,0x24,0x64,0x55,0x56,
        0x8B,0x74,0x24,0x68,0x8B,0x46,0x04,0x8B,0xA8,0x90,0,0,0};
    uint8_t actual[sizeof(frameLoad)]{};
    if (!SafeRead(script + 0x0E, actual, sizeof(actual)) ||
        memcmp(actual, frameLoad, sizeof(actual))) {
        Log("*** [combat] script dispatcher ABI unrecognized; refusing hook");
        return;
    }
    g_combatStartFn = start; g_combatPawnStartFn = pawnStart; g_combatAimFn = adjusted;
    const MH_STATUS create = MH_CreateHook((void*)script, (void*)&CombatProcessScript,
        (void**)&g_combatOriginalScript);
    if (create != MH_OK || MH_EnableHook((void*)script) != MH_OK) {
        if (create == MH_OK) MH_RemoveHook((void*)script);
        Log("*** [combat] script hook installation failed (%d)", (int)create);
        return;
    }
    // A harmless getter checks the measured FFrame.Node and return ABI on the
    // live game thread. With tracking still disabled, it cannot alter the result.
    struct StartParms { uint32_t weapon; MEVR_Vec3 location; } probe{};
    probe.location = {NAN, NAN, NAN};
    const long before = g_combatStartEntries;
    const bool called = CombatCall(pawn, pawnStart, &probe);
    if (!called || g_combatStartEntries == before || !FiniteVec(probe.location)) {
        MH_DisableHook((void*)script);
        MH_RemoveHook((void*)script);
        Log("*** [combat] live script dispatch check failed: called=%d entries=%ld finite=%d",
            (int)called, g_combatStartEntries-before, (int)FiniteVec(probe.location));
        return;
    }
    Log("[combat] live script dispatch check passed: frame Node and vector return verified");
    g_combatReady = true;
    g_combatInteractionReady = CombatInteractionValidate();
    CameraVisibilityValidate();
    Log("[perf] pistol hook installation took %.2f ms (game thread)", NowMs() - setupStarted);
    Log("*** [combat] pistol hooks installed: Colt1911/Glock18c, right-hand IK carries"
        " RightWeapon(49), bullets use Muzzleflash +Z and game spread; grip interactions=%s",
        g_combatInteractionReady ? "ready":"off");
}

static bool MotionPunchTick(XrTime when)
{
    static CombatPunchDetector hands[2];
    static XrTime lastPulse = 0;
    static uintptr_t lastPawn = 0;
    uint32_t weapon = 0; uint8_t anim = 0, move = 0;
    const bool eligible = !g_menuBlocksGameplay && g_combatInteractionReady && !g_combatGripBlocksPunch &&
        g_motionPunch && g_motionHands && g_gripToGrip &&
        g_padEnabled && g_xrState == XR_SESSION_STATE_FOCUSED &&
        g_playerPawn && CurrentViewTargetIsPawn(g_playerPawn) &&
        g_offWeapon >= 0 && g_offWeaponAnimState >= 0 && g_offMoveState >= 0 &&
        SafeU32(g_playerPawn+g_offWeapon, &weapon) && !weapon &&
        SafeRead(g_playerPawn+g_offWeaponAnimState, &anim, 1) && !anim &&
        SafeRead(g_playerPawn+g_offMoveState, &move, 1) && (move == 1 || move == 17);
    if (!eligible || lastPawn != g_playerPawn) {
        hands[0].reset(); hands[1].reset(); lastPulse = 0;
        lastPawn = g_playerPawn;
        return false;
    }
    const ArmSwingHand* samples[2] = {&g_swingL, &g_swingR};
    for (int h = 0; h < 2; ++h) {
        const auto& s = *samples[h];
        if (hands[h].sample((double)when*1e-9, s.tracked && s.havePrev,
                            g_gripValue[h] > 0.5f, {s.rel.x, s.rel.y, s.rel.z}) &&
            when-lastPulse >= 180000000LL) {
            lastPulse = when;
            g_combatGripDetector.pickupHand=-1; // a punch consumes this squeeze
            CombatQueueAction(h ? CombatPunchRight : CombatPunchLeft, 0);
            Log("[combat] %s punch gesture queued", h ? "RIGHT" : "LEFT");
        }
    }
    return false; // dispatched through the game's AttackPress on its own thread
}

static void ReportCombatState(uintptr_t pawn)
{
    static uintptr_t lastWeapon = 0;
    static int lastAnim = -1;
    static long lastReport = 0;
    uint32_t weapon = 0; uint8_t anim = 0;
    if (g_offWeapon < 0 || g_offWeaponAnimState < 0 ||
        !SafeU32(pawn+g_offWeapon, &weapon) ||
        !SafeRead(pawn+g_offWeaponAnimState, &anim, 1)) return;
    g_combatPlayerWeapon = weapon;
    if (weapon == lastWeapon && anim == lastAnim &&
        (!weapon || g_frames-lastReport < 600)) return;
    lastWeapon = weapon; lastAnim = anim; lastReport = g_frames;
    char name[96] = "None";
    if (weapon) ReadClassName(weapon, name, sizeof(name));
    Log("[combat] weapon=%s anim=%u hooks=%s hand=%s handIK=%s traces/aims=%ld/%ld dispatch=%ld/%ld",
        name, anim, g_combatReady && !g_combatFault ? "ready" : "off",
        CombatHoldingHand()==0 ? "LEFT":"RIGHT",
        (CombatHoldingHand()==0 ? g_p13LeftState.owned:g_p13RightState.owned) ? "tracked" : "game",
        g_combatStartCalls, g_combatAimCalls,
        g_combatStartEntries, g_combatAimEntries);
}
