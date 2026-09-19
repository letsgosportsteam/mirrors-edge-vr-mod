# Pistol and melee work

## Current status — 2026-09-16, pickup and camera correction build

The user confirmed throwing, smoke/bird stereo, and the 0.75-second combo gap.
The previous gaze pickup, blue marker and airborne catch implementation did not
work in the headset. The changes below address discovery latency, visual mesh
position, marker rendering and the catch window; their final headset behavior
still needs verification. See [PICKUP_CAMERA_EFFECTS_2026-09-16.md](PICKUP_CAMERA_EFFECTS_2026-09-16.md).

Supports Colt1911 and Glock18c. Existing pistol angles (40° down, 10° left)
initialize both hands, and future adjustments are saved separately.

The four-punch combo now resets after a gap longer than **0.75 seconds** between
accepted attack starts (shortened from 1.5 seconds at the user's request).

| While holding a pistol | Adjustment (only that holding hand) |
| --- | --- |
| Ctrl + arrows | Pitch/yaw in 5° steps |
| Ctrl + Shift + Left/Right | Move left/right in 5 mm steps |
| Ctrl + Shift + Up/Down | Move up/down in 5 mm steps |
| Ctrl + Shift + Page Up/Down | Move forward/back in 5 mm steps |

Debug and MotionHands must be enabled for the hotkeys. Position moves the gun
relative to the controller without moving the hand IK. Trim remains gun-only.
The overlay identifies LEFT/RIGHT and F/R/U position in millimetres. Each edit
saves to `%LOCALAPPDATA%\MirrorsEdgeVR\mevr-gun.ini`, `[PistolLeft]` and
`[PistolRight]`: `WristDownDegrees`, `WristRightDegrees`, `ForwardMm`, `RightMm`,
`UpMm`. The legacy `[Pistol]` section remains intact for migration; position
starts at zero. Rotation is limited to ±90° and position to ±200 mm.

Release now passes tracked controller linear and angular velocity into the
native `DropFromEx` physics path, plus pawn locomotion velocity. Velocity is
sampled in OpenXR base space and transformed into the game world so head turns
do not alter a throw. Invalid tracking or an implausible velocity produces a
plain drop. A drop deferred more than 150 ms by a game animation also becomes a
plain drop. The game still controls rigid-body collision, gravity and lifetime.

For ground pickup, look at a pistol within two metres, then squeeze either grip.
Selection uses an eight-degree cone (with an 8 cm minimum aiming tolerance),
line of sight, pickup state/cooldown, ammo and inventory-slot checks. A temporary
blue wire box highlights the selected eligible pistol; this is a world marker,
not a material tint. The 0.25 s chord delay remains for ground pickup. Normal Y
keeps its original behavior. Selection is rechecked on the pickup request.

A tracked, closed hand can catch within 18 cm for as long as grip stays held,
including a hand closed just before the other hand releases a throw. Catch
checks use the pistol's swept position between ticks to reduce missed catches
on fast tosses. Drop-created pickups are registered immediately. Other pickups
are seeded by the native nearby-pickup lookup every 100 ms; an incremental
object scan covers pickups beyond the native touching cylinder. Selection uses
the visible pickup mesh's world position. The blue box is drawn directly into
both eye views, with an explicit D3D scene and complete state restoration.
Native initial pickup cooldown and empty-gun restrictions still apply.
First-squeeze/release latching, same-hand firing, melee and disarm are retained.

The new spatial functions have separate runtime ABI checks; a mismatch disables
gaze pickup/highlighting/catches without disabling the previous melee or pistol
aim hooks. This build still needs headset validation. Build/static analysis and
regression tests passed for per-hand calibration and persistence, position
isolation, throw direction/spin/head-turn invariance, tracking loss, gaze
selection/occlusion, catch sweep and scoping of the selected-pickup override.
The previous desktop startup attempt was blocked by “Failed to create DXT
texture” before device creation with both comparison DLLs; the user subsequently
ran that build successfully in VR. The current correction build has not been
run in the headset. The new DLL is installed; the save and gun defaults were
not edited.

The smoke rendering change and its limits are documented in
[SMOKE_STEREO_2026-09-16.md](SMOKE_STEREO_2026-09-16.md).

## Earlier grip interaction build — 2026-09-16

Pistol hand tracking, muzzle-based bullet direction and live calibration were
confirmed by the user in the previous build. Their saved settings remain 40°
down and 10° left (`WristRightDegrees=-10`). This pass still supports Colt1911
and Glock18c; other weapon classes retain button-based handling.

New in the grip interaction build:

- Auto-acquired/Y-acquired pistols start in the right hand and stay equipped
  without gripping. The first fresh squeeze of that hand enables release-to-drop.
- While unarmed, squeeze either grip near a pistol that the normal Y pickup
  lookup accepts. After a 0.25-second chord window, the game picks it up into
  that hand. Its trigger fires; left trigger no longer crouches while holding
  a left-hand pistol. Grip-acquired pistols are already armed for release.
- Release drops directly from the weapon bone using the game's dropped-pickup
  physics and inventory cleanup, without its canned toss animation or impulse.
  It can be grabbed again using the same Y proximity/ammo eligibility. Empty
  pickups and pickups removed by the game's lifetime rules remain ineligible.
  Release during a busy state waits for an allowed state; regripping cancels it.
  Tracking/focus loss cancels pending drops rather than simulating a release.
- Left-hand holding rebases the instance's RightWeapon bone onto LeftHand.
  Muzzle/effects and gun-only wrist trim follow the selected hand. Reload,
  disarm, holster and parkour retain game animations and may temporarily use
  the original right-hand weapon pose.
- Grip-held motion punches now carry the physical hand through the game's
  accepted/queued attacks. Normal animations match that hand. Every fourth
  accepted attack requests the native two-hand shove, with a 1.5-second gap
  resetting the custom counter. These are accepted attacks, not confirmed
  damage hits. The contextual bent-over-enemy soccer kick still takes priority.
- For disarm, extend both hands forward and squeeze both grips within 0.25 s.
  Both hands must be at least 25 cm forward of the headset, within 70 cm
  horizontally and from 55 cm below to 25 cm above it. The game must accept
  the target/range/disarm checks. An invalid chord does not fall back to Y's
  pickup, forced-miss or drop behavior. Release before trying again.

Requires `MotionHands` and `GripToGrip`; pistol tracking requires `PistolHands`,
and punching requires `MotionPunch`. Grip curl remains unchanged. Held grips
continue to suppress arm-swing running and hands-up jumping. The game owns the
arms during melee animations. Direct collision-driven hand damage, physical
weapon collision/anti-clipping and touching a pickup with a hand are not added.

Validation: x86 build and required static analysis passed. Production-code
regressions cover grip latching/pickup/drop, tracking interruption, two-hand
chords, fourth-attack counting, queued hand selection, masked flag writes,
left weapon transforms, reload/ownership gates, calibration/persistence,
arm restoration and nonblocking metadata setup. A live desktop startup reached
training and accepted all interaction function/property layouts plus the pistol
script-getter check. This does not verify physical controller gestures, left-hand
visuals or combat timing in a headset. The test save was restored afterward.

Performance work retains background metadata discovery and bounded logging;
see [PERFORMANCE_2026-09-16.md](PERFORMANCE_2026-09-16.md). The startup test spent
4.9 s on background metadata discovery and about 77 ms installing hooks. Idle
interaction ticks avoid Y eligibility calls when no action/drop is pending.
Headset performance improvement is still unverified.

## Earlier implementation history (superseded by current status)

## First implementation

`PistolHands=on`, with `MotionHands=on`, admits Colt1911 and Glock18c in ordinary
movement and weapon animation states Relaxed/Ready. The existing position and
wrist controls move RightHand, which carries its child RightWeapon and the
attached pistol. Left-hand tracking remains available. Reload, throw, holster,
melee, disarm and armed parkour retain the game's arm ownership. Grip release
keeps the gun equipped; existing finger curl is unchanged.

The shipped SK_UpperBody skeleton has RightHand at 48 and RightWeapon at 49,
parent 48. WeaponSocket uses RightWeapon with rotation (16384,16384,0).
TdWeapon.AttachWeaponComponentsToPlayer attaches Mesh1p to that socket. The
Colt/Glock Muzzleflash socket sits on Wep_Root, with identity relative rotation
and a positive Z muzzle offset. The barrel direction is therefore socket +Z.

Engine.Weapon.InstantFire gets its origin from
Pawn.GetWeaponStartTraceLocation (the camera) and its direction from
TdWeapon.GetAdjustedAim. The prototype filters the shared script interpreter by
those UFunction identities, preserving interpreter argument handling, and replaces
their results with the first-person muzzle location and barrel rotation.
Weapon.AddSpread still supplies spread. Head/sticky aim no longer supplies the
returned pistol direction. The runtime validates function ownership, parameter
offsets, dispatcher identity/ABI and a live getter call before enabling pistol IK.
Fresh tracked position/orientation and live player ownership gate firing changes.
Other weapons retain the existing behavior.

`MotionPunch=on`, with MotionHands and GripToGrip, recognizes a held-grip outward
thrust while unarmed and Walking or in ground Melee. It requires at least 10 cm
outward travel and 1.2 m/s radial speed; slow reaches, retraction, pose jumps,
stale samples and open hands do not trigger it. Retraction rearms the fist. It
emits a 65 ms normal attack input, preserving the game's move/target checks and
animation selection. This first pass lets the game animate the arms. Either
held grip blocks arm-swing running and the hands-up jump gesture; manual stick
movement remains available. Running requires a new swing after grip release.

## What the game's melee code actually does

Read from the local shipped TdGame.u, Engine.u and TdSharedContent.u using the
existing UELib tooling. Decompiled assets stay in the ignored analysis directory.

* TdMove_Melee.StartMove normally toggles bLeft. When MeleeState is normal attack
  and the absolute leg/body yaw difference exceeds 4000 rotator units, it chooses
  bLeft from that difference's sign instead. Normal animation names are
  MeleeStartLeft/Right, followed by matching hit/miss animations.
* TriggerMove opens a 0.33-second input window. Attack actions in that window
  increment ComboCounter and ComboQueuedActions, capped at counter 2.
* At recovery completion, a queued attack calls **TdMove_MeleeBase.StartMove**,
  decrements the queue, toggles bLeft, and calls TriggerMove. The bytecode's
  final-function reference is export 14245 (one-based), whose owner is the base
  class. UELib prints bare StartMove here; interpreting that as a recursive call
  to the derived method would incorrectly imply that it resets the combo.
* With counter 2 and no queued actions left, TriggerMove selects MeleeStartShove
  and then MeleeHitShove: the two-hand third-attack finisher in a properly queued
  three-hit combo. Missed windows/end of the move reset the combo; this is not a
  global every-third-press cycle. A suitable bent-over enemy takes precedence and
  selects the soccer kick.
* Left/right selection can be controlled by writing the reflected bLeft flag at
  the correct point before TriggerMove. A third/fourth gesture finisher can be
  implemented with a separate gesture combo counter. Neither override is wired
  in this first pass; gesture attacks currently use the original selection.

## Pickup, direct hits and disarm

PressedSwitchWeapon (Y) drops an equipped weapon. Unarmed, it first attempts a
snatch/disarm, then asks TdInventoryManager for a nearby pickup. FindNearbyPickup
requires CanBePickedUpBy and ammo greater than zero. TdPickup's Pickup state
implements the valid-touch check, including obstruction and movement conditions.
Dropped weapons have a lifetime, so recovery is conditional. A grip policy can
leave a newly acquired gun latched, arm release-to-drop on the first squeeze,
then issue a single drop request on release. A physical hand-near-gun pickup
gesture would need its own proximity check. These grip drop/pickup gestures are
not implemented here; Y retains its normal behavior.

Animation-free hits are feasible: TdMove_Melee.TestHit ultimately calls
DeliverDamage, which calls TargetPawn.TakeDamage with the player controller,
impact location/momentum, directional melee damage type and TraceHitInfo.
It is not enough to run TestHit as a hand collision detector: its normal test
uses pawn facing (dot > 0.8) and distance (< 170 UU), with an assumed Neck hit.
Physical punching needs a swept hand trace, target validation, one hit per
stroke, impact metadata and appropriate melee reaction setup. That path is
investigated, not implemented.

A grab gesture can request the existing SnatchAttempt/PressedSwitchWeapon path.
The game then checks target, range, angle, movement and disarm timing. A spatial
grab detector must validate enemy contact and require a new grip edge; a bare
grip-to-Y mapping would also drop guns or collect pickups. This detector is not
implemented in the first pass.

## Validation and next headset check

The second test confirmed visible gun tracking, but traces/aims stayed at 0/0.
Disassembly found that CallFunction's script branch calls ProcessInternal directly
(0114AB09 calls 01148D50), bypassing the per-UFunction dispatch pointers. The fix
detours the census-resolved shared interpreter, runs the original body, and changes
only the exact player trace/weapon aim return values. FFrame.Node at +4 is validated
against the shipped machine instructions. Raw dispatch counts and specific muzzle
rejection reasons now distinguish missed calls from failed tracking/socket gates.
The start-location override includes TdPawn as well as Pawn.
The rebuilt DLL passed its live script-dispatch getter check in the training
level. The next headset test confirmed that bullet trajectory and downward
wrist adjustment worked.

CTRL+DOWN adds 5 degrees of gun-only downward wrist trim; CTRL+UP subtracts 5.
CTRL+RIGHT adds 5 degrees of rightward trim; CTRL+LEFT subtracts 5.
It rotates in controller space toward the pinky, before the mirrored hand rest
correction, and is shared by the borrowed and dedicated wrist/forearm paths.
Only the right wrist with a supported pistol gets the trim. Each edit saves
automatically to `%LOCALAPPDATA%\MirrorsEdgeVR\mevr-gun.ini`, section `[Pistol]`,
keys `WristDownDegrees` and `WristRightDegrees` (-90..90 each). Both angles are
written together on every adjustment and loaded as defaults on the next launch.
An existing downward-only file retains that value and starts rightward trim at 0.
The overlay reports both values and save status. The user's 40-degree downward
setting was present on disk after the crash and is preserved by this update.
`tools/test-gun-calibration.ps1` compiles the production conversion and checks
downward and left/right direction, combined offsets, a rolled controller,
left/unarmed isolation, invalid input, live persistence, save failure and
reloading in a separate process.

The third test crashed after a player/arm rig transition. Its last log entries
showed changed limb controllers at the same pawn address and failed dedicated
rotation/shoulder restoration. Windows reported c0000005 at executable RVA
00D31473; no retained dump was available, so the exact cause is unconfirmed.
Restoration now checks the saved pawn, mesh, animation tree, controller arrays,
array counts and world-IK controller pointers, then validates both sides' control
objects before any write. A replaced rig discards stale restoration state.
`tools/test-arm-restore.ps1` compiles production restoration code against mocked
engine memory and verifies normal one-time restoration, changed identities,
unreadable memory and no partial writes when either side's controls are invalid.
This hardens the transition seen in the log; it does not prove the crash resolved.

The first pistol test failed before enabling either feature: `TdWeapon.Mesh1p`
is a `ComponentProperty`, but the initial lookup requested `ObjectProperty`.
Both Backspace markers therefore reported `hooks=off` and `rightIK=game`.
The lookup now uses the shipped property's actual type. A regression check in
`tools/test-combat-layout.ps1` compares production property requests against the
local game package; `-SimulateOldMeshType` reproduces the rejected lookup.

After rebuilding and installing the correction, a live startup run validated
all required fields and parameters and logged `pistol hooks installed`.
The resolved Mesh1p offset is 1136, MuzzleFlashSocket is 824, and ProcessEvent
is independently derived at slot 61. This verifies initialization in the game;
it does not yet verify tracked gun placement or firing in the headset.

The x86 build and required static analysis pass. Gesture regression tests cover
a punch, held extension, retraction/repeated punch, slow reaching, jitter, open
hand, tracking jump/loss/reacquisition, and a pause gap. Run them with
`tools/test-combat-gestures.ps1`.

The user subsequently confirmed tuning the pistol successfully; see current status above.
Check `[combat] pistol hooks installed`, then obtain a Colt
or Glock: hold the head still and move/rotate the gun; hold the gun still and look
aside; fire at two separated targets. The log should show paired `muzzle trace`
and `barrel aim` entries. Verify reload/drop/parkour handoffs, then unarmed punch
and retract while holding grip, confirming no unwanted forward locomotion.
No physical weapon collision/anti-clipping layer is added in this pass.


# Gunplay follow-up: physics pickup position and first trigger

The 22:42 run (preserved in `.analysis/gunplay-followup/user-run.log`) found a
dropped pickup but repeatedly rejected it as outside the gaze cone/range, with
some cooldown rejections. No eligible target meant no blue highlight. The user
also clarified that the initial trigger pulls did nothing, rather than firing
shots that failed to hit.

Pickup position now prefers `PrimitiveComponent.GetRootBodyInstance` followed
by `RB_BodyInstance.GetUnrealWorldTM`. A skeletal physics body can move away from
the actor/component origins previously used by the selection and catch tests.
The component position remains a fallback for pickups without a live rigid body.
Ground selection still requires the Pickup state; a deliberate tracked-hand
catch can also accept CoolDown. Both the initial catch check and final pickup
check use that rule. Other eligibility checks remain in place. Context rejection
logs now separate inventory, world, absent head pose and stale head pose.

The shipped `TdWeapon.StartFire` rejects a pistol when the pawn's
AgainstWallState is 1, based on the authored weapon pose. For a currently tracked
pistol with a clear head-to-muzzle trace, that byte is cleared only around the
StartFire invocation and restored immediately afterward. Obstructed/untracked
pistols, other weapons and unfocused VR retain the game behavior. This addresses
a concrete incompatible gate; the old log does not prove it caused every missed
trigger. New `[combat-fire]` entries record AttackPress, weapon state, input-ignore
status and StartFire wall state to locate any remaining failure.

Validation: x86 build/static analysis, reflected function identities from the
shipped packages, pickup runtime tests using independent actor/component/body
positions, real production gaze selection and highlight eligibility, left/right
ground pickup requests, pre-held catches during cooldown, occlusion/tracking
gates, combat interaction/spatial tests, and scoped fire-hook restoration tests.
Headset confirmation is still required. No calibration/configuration was edited.

Installed DLL SHA-256:
`912350FFCD5AD9177B73CABA2C25DE8D1B2D9691A38637D4E9B18436D7AEFFA2`.
Previous DLL: `.analysis/gunplay-followup/d3d9-before.dll` (`47D8E4CE...`).
