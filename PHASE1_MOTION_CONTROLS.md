# Phase 1 motion controls — implementation plan

## Goal

Add tracked first-person hands while preserving the game's authority during weapons,
parkour animations, cinematics, death cameras, and any state where controller tracking is
unsafe. Phase 1 changes presentation only: the physical controllers do not affect collision,
combat aim, ledge detection, or movement mechanics.

The shipped `Mesh1p` rig is the target. `TdPawn` already owns left/right world-space limb IK,
separate hand-rotation controls, forearm-roll controls, and weapon-aware arm logic. The parkour
moves already use the same world-space IK controllers to plant Faith's hands on ledges and vault
points. Phase 1 should add one more owner of those controls, not introduce replacement hand
meshes.

## Phase 1 contract

### In scope

- Read both controller grip poses from OpenXR.
- Convert controller poses into the exact UE3 world space used by `Mesh1p`.
- Drive the existing left and right arm/hand controls while the player is unarmed and in a safe
  movement state.
- Give control back to the game before parkour, weapon, or cinematic animation needs the hands.
- Blend back into tracked control when ordinary locomotion resumes.
- Handle tracking loss, recentering, pawn replacement, death, respawn, and level transitions.
- Add diagnostics and conservative configuration switches.

### Explicitly out of scope

- Bullets following the controller.
- Controller-driven melee, grabbing, climbing, vaulting, or zipline interaction.
- Physical hand collision.
- Manual reloads or weapon manipulation.
- Custom hand models.
- Full finger tracking. Simple trigger/grip-driven poses are optional polish, not a release gate.

## Architectural shape

```text
OpenXR grip pose actions
        |
        v
pose validity + predicted-time location
        |
        v
shared OpenXR -> rendered UE3 world transform
        |
        v
per-hand ownership arbiter <--- pawn / weapon / movement / ViewTarget state
        |
        v
post-Update1pArms rig writer
        |
        +--> Left/RightHandWorldIKController
        +--> Left/RightHandRotationController
        +--> forearm roll controls, if required
```

The DLL remains the authority for OpenXR and pose conversion. No custom UnrealScript package is
required for the first vertical slice. The existing runtime reflection system should discover
the relevant property offsets and live object pointers.

## Non-negotiable design decisions

1. **Use grip pose for hands and aim pose only for diagnostics/future Phase 2.** Grip pose is the
   controller-held-object transform. Creating aim actions now prevents another action-set layout
   change when weapon work starts.
2. **Create every pose and haptic action before `xrAttachSessionActionSets`.** OpenXR action sets
   cannot be extended after attachment.
3. **Use one shared pose conversion for the camera and hands.** Duplicating handedness, scale, or
   recenter math would eventually make the hands drift from the rendered head.
4. **Start with a conservative tracked-state whitelist.** Expanding tracking one movement at a
   time is safer than discovering during testing that a canned move needed a hand.
5. **Prototype through Present, ship through a game-update hook.** Present-time writes can prove
   coordinate conversion and rig quality, but are expected to appear one rendered frame late.
6. **Never write through an unvalidated cached pointer.** Pawn and component replacement is normal
   during death, respawn, cinematics, and level transitions.

## Runtime data to add

### OpenXR objects

- `left_grip_pose`, `right_grip_pose`: `XR_ACTION_TYPE_POSE_INPUT`
- `left_aim_pose`, `right_aim_pose`: `XR_ACTION_TYPE_POSE_INPUT`
- One action space for each pose action
- Optional left/right vibration outputs, created now even if unused in Phase 1
- Cached pose validity, tracked flags, position, orientation, and sample time per hand

Suggested bindings should include at least the existing Oculus Touch profile and the simple
controller fallback where a pose path is supported. An unbound or inactive pose is normal and
must not disable ordinary synthesized-pad input.

### UE3 properties and objects

Resolve and cache, with the same class-walking and validation discipline used by the camera path:

- `TdPlayerPawn.Mesh1p`
- `TdPawn.LeftHandWorldIKController`
- `TdPawn.RightHandWorldIKController`
- `TdPawn.LeftHandRotationController`
- `TdPawn.RightHandRotationController`
- `TdPawn.LeftForeArmRollRotationController`
- `TdPawn.RightForeArmRollRotationController`
- `TdPawn.Weapon`
- `TdPawn.WeaponAnimState`
- Existing `TdPawn.MovementState`
- `TdSkelControlLimb.EffectorLocation`
- `TdSkelControlLimb.EffectorLocationSpace`
- `SkelControlBase.ControlStrength`
- `SkelControlSingleBone.BoneRotation`
- Bone rotation/control-space fields required by this engine build

The first implementation may write the controller fields directly. Calling
`SetHandsWorldIKLocation` through the script VM is not a prerequisite and should not be allowed to
hold up the position-only proof.

## Coordinate conversion

For each predicted display time:

1. Locate the HMD/view pose and both grip action spaces in the same OpenXR reference space.
2. Compute each hand relative to the tracked head:

   `hand_from_head = inverse(head_in_local) * hand_in_local`

3. Build the rendered head transform in UE3 world space using the same camera anchor,
   handedness conversion, recenter transform, and `100 UU/m` scale as head tracking.
4. Compose:

   `hand_world = rendered_head_world * convert(hand_from_head)`

Using a head-relative transform is important: animation may move the pawn and `EyeJoint`, while
the user's controller remains physically fixed relative to the headset. The hand must follow the
rendered body anchor without inheriting head rotation twice.

The conversion should live in a small testable set of quaternion/transform helpers. Add debug
checks for normalized quaternions, finite values, plausible controller distance, and the expected
left/right ordering.

### Initial calibration policy

- Use the real 1.0 position scale initially.
- Apply small per-hand grip-to-palm orientation constants after measuring the shipped hand bones.
- Do not introduce virtual arm-length scaling until the unscaled result has been judged in-headset.
- Recenter must reset head and hand reference state on the same frame.
- Optional offsets belong in `mevr.ini` only after a repeatable mismatch is measured.

## Hand ownership state machine

Maintain ownership separately for the left and right hands even though the first pass can make the
same decision for both.

```text
GAME -> ACQUIRE_TRACKED -> TRACKED -> RELEASE_TRACKED -> GAME
                    \                         /
                     +---- tracking loss -----+
```

### Tracked eligibility — first conservative version

All conditions must be true:

- OpenXR session focused and the relevant grip pose is valid and tracked.
- Live, validated player pawn and `Mesh1p` exist.
- Current `ViewTarget` is the player pawn.
- Not in cinematic mode, death, respawn, menu-only view, or scripted camera.
- No weapon is equipped and the weapon animation state is unarmed.
- Movement state is explicitly allowed.

Initial movement whitelist:

- `Walking`
- `Falling`
- `Crouch`, only after an in-headset test confirms the rig remains stable

Everything else is game-owned initially, including jump transition, slide, wall-run, wall-climb,
springboard, vaults, grab/into-grab, pull-up, climb, zipline, balance, ledge walk, melee, stumble,
snatch, interaction, and all unknown/out-of-range states. Later rungs may add states based on
evidence.

### Transitions

- **Acquire tracked:** ramp VR control strength from 0 to 1 over approximately 150 ms while the
  target follows the current physical controller.
- **Release tracked:** stop asserting the VR effector and rotation immediately when the game
  enters a hand-using move. Let the move's existing `SetSkelControlStrength` calls perform its
  intended blend. If testing reveals a snap before the move takes ownership, add a short release
  ramp without delaying the game target.
- **Tracking loss:** release only the affected hand. Never freeze a hand in mid-air indefinitely.
- **Pawn replacement:** discard all cached rig pointers and ownership state before rediscovery.

Log ownership only on transitions; a per-frame ownership log would hide the useful signal.

## Update timing strategy

### Prototype path

After the OpenXR frame wait in the current Present path:

- Locate hand poses at the same `predictedDisplayTime` used by head tracking.
- Apply property writes for the next game frame.
- Use this only to prove transform signs, world scale, IK reach, and hand orientation.

Expected limitation: the visible hands lag by roughly one game frame because the engine has
already evaluated and rendered `Mesh1p`.

### Production path

Run the rig writer after the game's native `TdPlayerPawn.Update1pArms` for the current simulation
tick. Preferred investigation order:

1. Derive the `UFunction` native-function pointer layout and detour `Update1pArms`, calling the
   original before applying tracked targets.
2. If that is not viable, find a stable pawn/game tick hook and order the write after arm update.
3. Use Present-time predicted/extrapolated writes only as a documented fallback.

The hook must consume an atomically published pose snapshot; it must not call `xrWaitFrame` or
perform an object walk on the game thread.

## Development rungs

Each rung answers one question and should be committed separately.

### P1.0 — pose actions exist

**Question:** Does VDXR expose valid left and right grip/aim poses through the mod's existing action
set?

Work:

- Create all pose spaces before action-set attachment.
- Locate poses next to the current head and button samples.
- Add a gated diagnostic report with validity/tracked flags and head-relative positions.
- Destroy action spaces during normal XR teardown.

Pass:

- Both grip poses remain valid while controllers are active.
- Losing one controller invalidates only that controller.
- Existing synthesized Xbox input is unchanged.

### P1.1 — pose conversion is internally honest

**Question:** Can controller poses be expressed in the rendered UE3 world frame without sign,
scale, or recenter drift?

Work:

- Factor shared transform/quaternion helpers.
- Compute head-relative and UE3-world controller poses.
- Add plausibility diagnostics: controller distance, left/right relation, finite values, and pose
  age.

Pass:

- Moving a controller right/up/forward changes the matching UE axis.
- A 1 m physical displacement measures approximately 100 UE units.
- Head rotation with physically stationary head-to-hand geometry does not orbit or mirror the hand.
- Page Up recenters head and hands together with no relative jump.

### P1.2 — rig discovery survives lifecycle changes

**Question:** Can the DLL safely find and validate all first-person hand controls?

Work:

- Extend property discovery for the pawn, limb controllers, and single-bone controls.
- Validate every nested object by class/ancestry before caching it.
- Add one-time discovery logs and rejection logs that name the failed object/class.
- Invalidate on pawn, mesh, controller, or level replacement.

Pass:

- Correct rig is found in tutorial and ordinary levels.
- Death/respawn and level load reacquire without stale writes or crashes.
- No full `GObjects` walk occurs per frame.

### P1.3 — position-only hands

**Question:** Can the existing arms reach stable controller positions while standing?

Work:

- Enable world-space limb IK for one hand first, then both.
- Keep game hand rotation and fingers temporarily.
- Add debug toggles for left, right, and both hands.
- Clamp/reject non-finite or physically implausible targets.

Pass:

- Correct hand follows the correct controller.
- Arm bends consistently across the useful reach volume.
- No mesh explosion, NaNs, or persistent stretched arm after disabling the feature.

**Stop/go gate A:** If the shipped limb controllers cannot produce acceptable shoulder/elbow
behavior in the normal reach volume, evaluate a hybrid/custom-hand representation before adding
rotation or ownership logic.

Test result (2026-08-21): the left shipped limb follows the tracked controller reliably once the
post-`Update1pArms` writer updates `ControlStrength`, `StrengthTarget`, and `BlendTimeToGo`
together. Wrist orientation remains game-controlled, as intended for this rung. At full physical
extension the rendered hand saturates at the skeletal arm's maximum reach and falls short of the
controller. Gate A therefore remains open pending the two-hand test; do not proceed automatically
to shipped-rig wrist orientation without choosing how to handle this positional mismatch.

### P1.4 — hand orientation

**Question:** Can the shipped rotation controls match controller orientation without wrist flips?

Work:

- Determine each hand control's required bone/control space.
- Calibrate left/right grip-to-palm quaternion offsets.
- Use forearm-roll controls only if the wrist cannot cover controller roll naturally.
- Add quaternion continuity handling around 180-degree transitions.

Pass:

- Palm direction and wrist roll remain visually correct through a full comfortable controller
  range.
- Left and right offsets are independently correct.
- No discontinuous flip appears during ordinary play.

### P1.5 — game/VR ownership handoff

**Question:** Can parkour, cinematics, and weapons reclaim the hands without fighting VR writes?

Work:

- Implement the conservative eligibility whitelist and ownership states.
- Gate by ViewTarget, weapon, movement, tracking, and rig validity.
- Log transitions with the exact reason.
- Exercise every representative movement group.

Pass:

- Tracked hands are active only during allowed unarmed locomotion.
- Grab, vault, pull-up, zipline, wall-run, climb, balance, melee, and weapon pickup all regain
  original game hand behavior.
- Tracking resumes after the move without a stuck controller strength or stale effector.
- Cutscenes and death cameras receive no VR rig writes.

**Stop/go gate B:** If writes ordered after `Update1pArms` still fight movement IK, move ownership
arbitration earlier in the tick or detour the specific game IK entry points before expanding scope.

### P1.6 — timing, tracking loss, and polish

**Question:** Is the result comfortable and robust enough for normal play?

Work:

- Land the production update hook.
- Measure pose age and visible hand latency.
- Add prediction only from measured need; do not guess a fixed lead blindly.
- Add optional simple grip/trigger finger pose if the existing rig exposes a stable route.
- Add configuration and release-mode diagnostics.

Pass:

- No obvious full-frame hand lag during normal controller motion.
- Controller sleep/wake and OpenXR focus loss recover without stuck hands.
- At least 30 minutes of mixed traversal, death/respawn, weapon pickup/drop, and level transitions
  completes without a crash or persistent rig fault.

## Configuration

Start disabled until P1.5 is stable:

```ini
MotionHands = off
```

Potential debug-only settings during development:

```ini
MotionHandsDebug = off
MotionHandsPositionOnly = off
MotionHandsLeft = on
MotionHandsRight = on
```

Do not expose calibration and prediction knobs merely because they are easy to add. Promote a
setting into the public ini only when different hardware or body geometry demonstrates a real
need that cannot be handled automatically.

## Diagnostics

Use the existing log and overlay discipline.

Required log events:

- Pose action/space creation and suggested-binding results.
- First valid pose for each hand and tracking loss/recovery.
- Rig property offsets and live controller classes.
- Pawn/mesh replacement and cache invalidation.
- Ownership transitions: hand, old owner, new owner, movement state, weapon state, and reason.
- Rejected pose/write with a bounded counter, not one line per frame.
- Update-hook installation and observed call frequency.

Debug overlay, compact form:

```text
HANDS L:T R:G  POSE VT/VT  AGE 8ms  ST Walking
```

Where `T/G` means tracked/game and pose flags distinguish valid from tracked.

## Test matrix

### Pose and calibration

- Controllers at rest, crossed, overhead, behind the head, and near the floor.
- Head yaw/pitch/roll while controllers remain physically fixed.
- Page Up recenter with hands visible.
- One controller sleeping, disconnected, and waking.

### Locomotion and handover

- Walk, run, fall, crouch.
- Jump, slide, wall-run left/right, wall-climb, springboard.
- Speed vault, vault over, high vault.
- Into-grab, hanging, shimmy, pull-up, transfer, drop.
- Ladder/pipe climb, zipline, balance, ledge walk.
- Melee, stumble, snatch, interaction.

### Lifecycle

- Menu to level, tutorial pawn, normal player pawn.
- Weapon pickup, fire/reload animation, drop, and return to unarmed.
- Cinematic in/out, death camera, respawn, checkpoint reload, level transition.
- OpenXR focus loss and runtime/controller reconnection where supported.

## Performance and safety budgets

- At most four `xrLocateSpace` calls per rendered frame for grip/aim, fewer when diagnostics are
  disabled and aim is not yet consumed.
- No per-frame object census.
- Cached safe property writes only after object/class validation.
- Rig-update CPU target below 0.2 ms on the game's main thread.
- Invalid data fails to game ownership; it never leaves the last tracked target asserted.
- Hand tracking failure must not affect stereo, head tracking, or synthesized-pad input.

## Main risks

| Risk | Early proof | Fallback |
|---|---|---|
| `Update1pArms` overwrites VR controls | P1.3 and hook call-order log | Detour native updater or later pawn tick |
| Hand rotation controls use awkward local spaces | P1.4 one-hand calibration | Position-only arms plus limited wrist rotation |
| First-person arm proportions stretch in VR | P1.3 headset reach-volume test | Hybrid/custom hands decision gate |
| Present sampling produces visible lag | Pose-age telemetry and P1.6 | Publish pose snapshot to game-update hook |
| Parkour and VR both write the same controllers | P1.5 transition logs | Conservative state whitelist and immediate release |
| Camera/body/head spaces drift apart | P1.1 recenter and head-rotation tests | One shared camera/hand transform implementation |

## Suggested commit sequence

1. `hands: create and report OpenXR grip/aim pose actions`
2. `hands: convert controller poses into rendered UE3 world space`
3. `hands: discover and validate the first-person IK rig`
4. `hands: drive position-only IK behind debug toggles`
5. `hands: add wrist orientation and per-hand calibration`
6. `hands: arbitrate game versus tracked hand ownership`
7. `hands: move rig writes to the post-arm-update hook`
8. `hands: add config, diagnostics, and Phase 1 regression tests`

## Completion criteria

Phase 1 is complete when an unarmed player can see both existing hands follow the controllers during
ordinary locomotion, all tested parkour/weapon/cinematic states regain their original hand behavior,
tracking resumes cleanly afterward, and the feature survives normal lifecycle transitions without
affecting head tracking, stereo, or gamepad synthesis.
