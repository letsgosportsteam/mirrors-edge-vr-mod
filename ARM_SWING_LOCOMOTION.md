# Arm swing locomotion — implementation plan

## Goal

Move Faith with the body instead of the thumb. Swinging the arms drives forward speed, raising
both hands overhead jumps, and physically crouching crouches or slides. Everything is injected
into the pad the mod already synthesises, so the game itself is untouched and the scheme is a
setting rather than a fork.

`ArmSwing = off` in `mevr.ini` is the default. With it off, not one byte of the pad differs
from today.

## Contract

### In scope

- A per-frame 2-D movement vector derived from controller motion, composed with the physical
  thumbstick and written into the synthesised left stick.
- A jump gesture, synthesised as `MEVR_PAD_LSHOULDER` (`GBA_Jump`).
- A crouch gesture, synthesised as `bLeftTrigger` (`GBA_Crouch`, which the game resolves into
  crouch or slide by its own speed test).
- Per-gesture ini switches, live hotkey A/B, an overlay row, and bounded diagnostics.
- Decay-to-neutral on every kind of failure: tracking loss, focus loss, cinematic gates, pad
  disabled.

### Explicitly out of scope

- Any change to `TdPlayerController`, `TdPlayerPawn`, or the script VM. The whole feature lives
  inside `XrSyncInput`.
- Room-scale walking translated into game movement. 6-DOF already moves the camera; making it
  also move the pawn is a different feature with different collision problems.
- Turning by the body. Yaw stays where it is — the head, folded into `Controller.Rotation`.
- Replacing the thumbstick. The stick keeps working, keeps priority, and remains the way to walk
  backwards.
- Haptics, gesture-driven combat, and climbing by hand.

## Why the pad, again

The same argument that made the controllers a synthesised 360 pad applies unchanged here, and it
is worth restating because a body-driven scheme *looks* like it wants a deeper hook.

`GBA_Move_Gamepad` is bound, tested, and already accepts a continuous analogue magnitude that the
engine turns into walk, run and sprint through its own acceleration curve. `GBA_Jump` on
`LeftShoulder` is the same button the game's own contextual up-actions read — vault, climb-up,
pull-up. `GBA_Crouch` on `LeftTrigger` already decides crouch-versus-slide from the pawn's speed.

Every one of the three gestures therefore has a shipped, tuned destination. Nothing here needs a
new game-side mechanism; it needs a good measurement of the player's body and an honest mapping
onto three controls that already exist.

## Architectural shape

```text
grip pose actions (already created, currently sampled only for MotionHands)
        |
        v
locate both grips + the head in g_xrSpace (LOCAL), one predicted display time
        |
        v
head-relative hand position  r = grip - head      [room axes, metres]
        |
        v
finite difference -> per-hand swing speed -> envelope -> speed scalar 0..1
        |                                                  |
        +--> hand height vs head  -> jump edge              |
        +--> head height vs standing baseline -> crouch     |
                                                            v
                                              swing vector (forward, strafe)
                                                            |
                                                            v
                          compose with the physical stick, clamp to the unit disc
                                                            |
                                                            v
                                    MEVR_XINPUT_STATE, inside XrSyncInput
```

One function, called from one place, reading only OpenXR and its own state. That containment is
deliberate: it means the feature cannot break head tracking, stereo, the arm rig, or the object
model, and it means `ArmSwing = off` is provably inert.

## Non-negotiable design decisions

1. **Differentiate in a frame that does not rotate.** The grip poses are currently located
   against `g_viewSpace`, which turns with the head. Differentiating a view-relative position
   makes every head turn look like a hand movement. The locomotion path locates against
   `g_xrSpace` (LOCAL) instead and subtracts the head position, giving the true physical
   hand-relative-to-head velocity in room axes. No rotating basis appears anywhere in the speed
   metric.

2. **Choose the swing metric by measurement, not by argument.** Four candidates are defensible
   and cheap; rung AS.0 logs all four against the same physical actions and the separation
   decides. See *The swing metric* below.

3. **Use `predictedDisplayTime` deltas for dt.** The poses are located at that time;
   differentiating against QPC or frame count introduces jitter the envelope would then have to
   filter back out. Reject a frame whose dt falls outside 2..50 ms.

4. **Inherit the 6-DOF discontinuity rejection.** `UpdateSixDof` already rejects a >0.25 m
   one-frame head step and rebases the centre, because OpenXR was measured moving the head 1.31 m
   between adjacent 120 Hz samples with the valid bit set. That same glitch through a
   differentiator is an instantaneous full sprint. Apply the same bound to the head and to both
   hands, and on rejection **hold** the previous velocity for that frame rather than decaying —
   a glitch is not evidence that the player stopped.

5. **Failure decays, it does not freeze.** The mod already lost a run to a stale 6-DOF offset
   held through a tracking dropout. A stale swing vector is worse: it is a stuck forward stick
   on a rooftop. Every loss path ramps the vector to zero.

6. **Thresholds are fractions of measured body geometry, not metres in an ini.** Standing head
   height and maximum observed arm reach are both measurable at runtime, and both are exactly
   what a per-player knob would otherwise be compensating for. Prefer self-calibration; promote a
   knob only when a measurement shows self-calibration failing.

7. **The physical stick always wins.** A player who cannot swing — seated, one controller, tired,
   in a menu — must still be able to play. The stick composes additively and, pushed backwards,
   suppresses the swing contribution outright.

## The swing metric

All candidates operate on `r_i(t) = grip_i(t) − head(t)`, expressed in LOCAL room axes, in
metres. None of them rotates a basis, so none of them can turn a head rotation into a velocity.

| | Metric | Immune to | Vulnerable to |
|---|---|---|---|
| **A** | `\|d r_i/dt\|` — full head-relative speed | room-scale translation, head yaw/pitch/roll | turning on the spot (hands orbit the head), large gestures |
| **B** | `\|d(r_i·up)/dt\|` — vertical only | all of A, plus turning on the spot | head bob while marching in place, reaching overhead |
| **C** | B, plus the horizontal component *along* `r_i`, discarding the perpendicular part | all of B; an orbit is purely tangential so a body turn contributes nothing | more state, more to get wrong; keeps only part of a swing |
| **D** | mean A × anti-phase, where anti-phase is `clamp01(−cos∠(v_L, v_R))` | all of B and C, by construction — a body turn carries both hands the same way round and D collapses | needs both controllers tracked and both hands moving |

A is the most sensitive and the most obviously correct-looking; B is the most robust and is what
several shipped arm-swingers settle on; C is the compromise.

**D was added during AS.0 and is not in the original three.** It is the plan's own stop/go gate A
fallback — "require left/right anti-phase" — promoted into the same measurement. A real swing
alternates, so the cosine between the two hands' velocities sits near −1; a body turn moves both
hands the same way round, cosine near +1. Logging it alongside the others means one headset
session answers both *which metric* and *what to do if all of them lose*, instead of two sessions
in series. It costs about ten floating-point operations.

The estimated failure case for A is worth stating in advance so the measurement can confirm or
kill it: arms hanging at the sides sit roughly 0.2 m from the head's vertical axis, so a brisk
180°/s turn on the spot moves them at about 0.6 m/s — plausibly above a swing deadband. If AS.0
measures that, A is out.

## Runtime data to add

No new OpenXR actions. The grip pose actions and spaces already exist and are already created
before `xrAttachSessionActionSets`.

- Three `xrLocateSpace` calls per frame against `g_xrSpace`: both grips **and the head**, all at
  `fs.predictedDisplayTime`, all inside `XrSyncInput`.

  ⚠️ Do not reuse the head pose `UpdateSixDof` already locates. It runs on the **render thread**,
  from inside the constant-injection hook, at `g_predTime + g_predPeriod` — a different thread and
  one frame further ahead than the pad is built for. Subtracting a head position sampled at one
  instant from a hand position sampled at another puts the difference between the two sample times
  straight into `r`, and `r` is about to be differentiated. At 120 Hz that is an 8 ms error in a
  quantity whose whole signal is a finite difference over 8 ms. Locate the head here, at the pad's
  own time, and accept the third call.
- `XrSampleMotionPoses` currently returns early unless `g_motionHands || g_motionHandsDebug`.
  Widen that gate to include `g_armSwing`, or — cleaner — give the locomotion sampler its own
  small function so the two features cannot break each other's early-out.
- Per hand: previous LOCAL position, previous sample time, filtered velocity, tracked flag.
- Envelope state: current speed scalar, time since the last above-deadband sample.
- Jump state: armed/asserted, assert start time.
- Crouch state: asserted, standing-height baseline, last update time.
- One counter per rejection class (dt out of range, discontinuity, untracked) for the bounded
  report.

Performance budget: two `xrLocateSpace` calls, about forty floating-point operations, and no
allocation. Well inside the existing four-locate-per-frame budget when aim diagnostics are off.

## Speed: swing to stick

### Envelope

The raw metric goes to zero twice per swing cycle, at each reversal. Fed straight to the stick
that produces a 2 Hz stutter and Faith never accelerates past a walk, because the engine's
acceleration curve keeps getting reset. The envelope is what makes the scheme work at all:

```text
raw    = mean of the metric over TRACKED hands only     (not over both, so losing one
                                                          controller mid-run halves nothing)
mapped = clamp01((raw - deadband) / (full - deadband))
speed  = max(mapped, speed_held)
```

`speed_held` follows a **hold-then-collapse** shape rather than a plain exponential:

- While any sample in the last `HOLD` ms was above the deadband, `speed_held` decays slowly
  (half-life ≈ 250 ms). This is what bridges the reversal.
- Once nothing has been above the deadband for `HOLD` ms (≈ 250 ms, about one swing period),
  `speed_held` collapses to zero over ≈ 120 ms.

A plain exponential decay cannot serve both ends. Slow enough to bridge a reversal is slow enough
to carry you off a roof after you have physically stopped, and in this game that is a death, not
a feel complaint. Two regimes with an explicit hold window is the shape that satisfies both.

Additionally: the physical stick pulled backwards past its dead zone collapses `speed_held`
immediately. That is the emergency stop, and it needs to be a reflex the player can trust.

### Composition

```text
swing   = (forward: speed, strafe: 0)                    steering = head   (default)
stick   = physical (LX, LY) as built today
final   = clamp_to_unit_disc(stick + swing)
```

Head steering needs no direction maths at all: head yaw is already folded into
`TdPlayerController::Rotation` as a delta, so the pawn faces where the player looks, and forward
on the left stick *is* head-forward. The vector is expressed as (forward, strafe) anyway so that a
second steering source can be swapped in at AS.5 without touching the envelope, the gestures, or
the composition.

## Jump: hands overhead

Signal: the higher of `r_i·up` against a threshold expressed as a fraction of the measured
standing head height, so a 1.6 m player and a 1.9 m player raise their hands the same distance in
body terms rather than the same distance in metres.

- **Assert** on the rising edge above `+RISE`, and hold `LSHOULDER` while above it, to a maximum
  of ≈ 400 ms. Holding matters: `GBA_Jump` is also climb-up, vault and pull-up, and those want the
  button held.
- **Re-arm** only after the hand drops below `+RISE − HYST`. Without hysteresis a hand hovering at
  the threshold machine-guns the button.
- **OR with the physical grip.** Left squeeze already synthesises `LSHOULDER`; it stays, and stays
  the reliable path when a gesture misfires.
- Log every synthesised press with the height that triggered it. A jump the player did not intend
  is the most likely early complaint and the log has to be able to answer it.

An arm swing never approaches head height, so the gesture and the locomotion metric do not
compete. Reaching for a headset strap does, which is why the rise threshold is above the head
rather than at it.

## Crouch: physical crouch

Signal: head height below a **standing baseline**, not below the 6-DOF centre. The 6-DOF centre is
captured at whatever height the head happened to be at the first valid pose — typically mid-air,
while the headset is being put on — and is therefore not a body measurement.

The baseline is an asymmetric tracker: it rises quickly toward a higher sample and falls very
slowly, so it settles at standing height within seconds and does not follow the player down into
a crouch. `PAGE UP` re-seeds it to the current head height, and the log says so, because
recentring while crouched would otherwise leave the baseline wrong until it drifted back.

- **Assert** `bLeftTrigger` at full when the head is more than `DROP` below the baseline
  (`DROP` as a fraction of the baseline, ≈ 0.15 → roughly 25 cm for an average player).
- **Release** at `DROP × 0.7`, hysteresis for the same reason as the jump.
- Slide falls out for free: the game reads the same `GBA_Crouch` and picks slide when the pawn is
  fast enough. Nothing here needs to know the difference.

### Two problems this gesture has that the others do not

**Leaning is not crouching.** Looking over a ledge drops the head just as far as a crouch does. The
proposed discriminator is the *horizontal* offset that comes with it: a crouch keeps the head over
the feet, a lean does not. Gate the crouch on the head's forward offset from the 6-DOF centre
staying below a bound, and measure whether that separates the two cleanly in AS.3. If it does not,
the fallback is to require the drop to be accompanied by both hands dropping with it — in a crouch
they do; in a lean-and-look they usually do not.

**The view drops twice.** A physical crouch already lowers the camera through 6-DOF; the game's
own crouch then lowers it again. The combined drop is roughly double what the body did. Options,
in the order they should be tried: accept it and measure whether it actually reads as wrong;
attenuate `g_dofOffset[1]` while a synthesised crouch is asserted, ramped over the same interval
the game's crouch takes. Do not build the attenuation before the measurement — it is a correction
to a problem that may not be one.

## Gating and failure

Hold at zero and clear all envelope state whenever:

- `g_padEnabled` is false (NUMPAD9), or `g_armSwing` is false.
- `xrSyncActions` returns not-focused — the runtime menu is up.
- A grip pose is not active/valid/tracked. One hand: fall back to single-arm and keep going. Both
  hands: ramp to zero.
- `IgnoreMoveInput` or `bCinematicMode` reads set. These are already discovered and already
  understood — UE3 zeroes `aForward`/`aStrafe` and touches nothing else — so a scripted sequence
  cannot be walked out of, but a stale envelope resuming the instant control returns is a real
  hazard and is what this gate prevents.

Deliberately **not** gated on `MovementState`. Faith needs stick input during `Falling` to steer in
the air and during `WallRun` to hold the wall, and a whitelist that guesses wrong there breaks
traversal in a way that is hard to attribute. Add a state gate only from a measured failure, and
name the state in the log when doing so.

## Development rungs

Each rung answers one question and is committed separately.

### AS.0 — is there a signal, and which one?

**Question:** Does any candidate metric separate a real arm swing from head turns, body turns,
gestures and crouches?

Work:

- Locate both grips and the head against `g_xrSpace`, at `fs.predictedDisplayTime`, in
  `XrSyncInput`. Not the render thread's head pose — see *Runtime data to add*.
- Compute all four candidate metrics per hand. Inject nothing.
- Log a bounded per-window report: min / mean / peak of each metric, plus the dt and
  discontinuity rejection counts.

### The measurement protocol uses the marker key, not a stopwatch

A fifteen-second report window cannot be lined up with a physical action from inside a headset,
and a window that straddles two of the six actions below averages them into a number that means
nothing. `BACKSPACE` already stamps `[mark] USER MARKER #N` with the frame and time, so the
protocol is: **press it once immediately before each action**, hold the action for about ten
seconds, and let the report windows fall where they like. Diagnosis then greps `[mark]` and reads
the metric lines between marker N and marker N+1, which is a clean per-action band rather than a
smear.

That requires the metric report to be denser than the fifteen-second pad and animation reports —
roughly one line per second while `ArmSwingDebug` is on — so each action yields ten samples rather
than one. It stays bounded, and it is off by default.

Actions, one marker press before each:

1. Stand still, arms at sides, look around fast — neck only.
2. Stand still, arms at sides, turn on the spot through 180° and back.
3. Walk in place, swinging at a walking cadence.
4. Swing hard at a running cadence.
5. Gesture, point, adjust the headset, reach overhead.
6. Crouch fully and stand, twice.

**Pass:** one metric puts 3 and 4 clearly above a threshold that 1, 2, 5 and 6 stay under, with
enough margin that the threshold does not have to be placed precisely.

**Stop/go gate A:** if no candidate separates them, do not build the rest on grip motion alone.
The next thing to try is requiring left/right **anti-phase** — a real swing alternates and a
gesture does not — before anything more elaborate.

### AS.1 — speed

**Question:** Does swing-driven speed actually move Faith the way the stick does?

Work:

- Envelope, mapping, composition, clamp, injection into the left stick.
- Self-calibrate `full` from the maximum observed swing speed over the session, floored so an
  early gentle swing does not set an unreachably low ceiling.
- Overlay row and a live on/off hotkey for the A/B.

**Pass:**

- A walking swing walks; a running swing reaches full sprint.
- No stutter at swing reversal, and the engine's acceleration curve is reached.
- Stopping stops within roughly 400 ms, and stick-back stops immediately.
- The physical stick still moves the player, forwards and backwards, with the feature on.

**Stop/go gate B:** if swing-driven speed is measurably worse than the stick for real traversal —
a timed run of a known route is the test — keep it optional and do not let jump and crouch depend
on it.

### AS.2 — jump

**Question:** Is the overhead gesture reliable enough to be the primary jump?

Work: threshold as a fraction of standing head height, hysteresis, hold, re-arm, OR with the grip.

**Pass:** a deliberate raise jumps every time; ten minutes of ordinary swinging, gesturing and
looking around produces no unintended jump; the physical grip still jumps.

### AS.3 — crouch and slide

**Question:** Can a physical crouch be told apart from a lean, and does slide fall out?

Work: standing baseline tracker, recentre re-seed, hysteresis, the lean discriminator, and the
measurement of whether the double view-drop reads as wrong.

**Pass:** crouching crouches; crouching at speed slides; leaning over a ledge to look down does
not crouch; standing up releases within one frame of the body doing so.

### AS.4 — gating, failure and endurance

**Question:** Can it be left on for a real session without producing a stuck control?

Work: every gate above, the discontinuity guard, the ramp-to-zero paths, and the bounded
rejection report.

**Pass:** 30 minutes of mixed traversal including deaths, respawns, cutscenes, a level transition,
a controller put down and picked up, and a headset removed and replaced — with no stuck stick, no
stuck crouch, no phantom jump, and no effect on head tracking or stereo.

### AS.5 — steering source, only if needed

**Question:** Is gaze-coupled movement uncomfortable enough to justify decoupling it?

Head steering means you cannot look sideways while running forward, which in this game — where
you look at the next ledge before you reach it — may matter. If AS.4 says it does, add
`ArmSwingSteering = hands`: estimate a torso yaw from the mean of the two grip poses, express
the swing vector in the pawn frame, and inject into both stick axes. The vector shape from AS.1
already accommodates this; nothing else changes.

Do not build this speculatively. It is the one part of the scheme whose need is genuinely
unknown until the rest is in a headset.

## Configuration

```ini
; Body-driven movement. Swing your arms to run, raise both hands to jump, crouch to crouch.
ArmSwing = off

; The two gestures are independently switchable, because they fail differently. Crouch is the
; one to turn off when playing seated.
ArmSwingJump   = on
ArmSwingCrouch = on

; Bounded per-window metric reports in mevr.log.
ArmSwingDebug = off
```

Development-only keys, parsed but deliberately absent from `mevr.ini.example` until a measurement
argues for them: `ArmSwingDeadband`, `ArmSwingFullSpeed`, `ArmSwingCrouchDrop`,
`ArmSwingJumpRise`, `ArmSwingSteering`.

The Phase 1 rule applies unchanged — a knob goes public when different bodies or hardware
demonstrate a need that self-calibration cannot meet, not because it was easy to add. The two most
likely to graduate are `ArmSwingFullSpeed` and `ArmSwingCrouchDrop`, and both should first
be attempted as fractions of measured reach and measured standing height.

## Hotkeys

Every F-key, every numpad digit, and the whole navigation cluster is already assigned. The numpad
operators are free, and the arrow keys are only live behind `g_overlay && g_motionHands`.

| Key | Effect |
|---|---|
| `NUMPAD *` | arm swing locomotion on/off, for the A/B without a restart |
| `NUMPAD /` | cycle the gesture set: all / swing only / swing + jump / off |
| `NUMPAD .` | select which tuning value the overlay is editing |
| `NUMPAD +` / `NUMPAD -` | change the selected value |

All four sit behind `g_debug`, like everything else that is not a way out of a stuck state.

## Diagnostics

Overlay row — note that the glyph set is `A-Z 0-9 space + - ( ) : . / =`, so no `%` and no `*`:

```text
SWING 072 RAW 19 J. C-  BASE 162
```

the injected speed as hundredths, the raw swing metric in tenths of m/s, jump and crouch as
asserted or not, and the standing baseline in centimetres.

Required log events:

- One line at startup naming the chosen metric and the self-calibrated thresholds.
- Every jump and crouch assert and release, with the value that caused it.
- Standing baseline re-seed, on recentre and on first settle.
- Gate transitions — focus, tracking, cinematic — with the reason, on change only.
- Bounded counters for dt-out-of-range, discontinuity rejection and untracked frames, reported on
  the same window as the pad and animation reports so a "it ran off on its own" report arrives
  with all three ends of the chain on adjacent lines.

Never one line per frame. The existing reports are on a fifteen-second window and this belongs on
the same one.

## Test matrix

### Signal

- The six AS.0 physical actions, repeated with the headset re-seated between runs.
- One controller asleep, disconnected, and waking mid-swing.
- Recentre mid-swing and mid-crouch.
- Play seated, with the crouch gesture on, to confirm it is the setting that needs turning off
  rather than a bug.

### Traversal

- Walk, run, sprint, stop.
- Jump a gap by gesture; jump a gap by grip; jump while sprinting.
- Slide under an obstacle from a sprint. Crouch under one from a standstill.
- Wall-run left and right, wall-climb, springboard — all of which need sustained forward input
  through a state the mod does not gate.
- Speed vault, vault over, into-grab, hang, pull-up, drop.
- Zipline, balance beam, ledge walk.
- A timed run of a known route, swing versus stick, for gate B.

### Lifecycle

- Menu to level and back with the feature on.
- Cutscene entry and exit; death and respawn; checkpoint reload; level transition.
- OpenXR focus loss and return.
- 30 minutes continuous, for AS.4.

## Main risks

| Risk | Early proof | Fallback |
|---|---|---|
| No metric separates swing from turning | AS.0, before anything is injected | Require left/right anti-phase; failing that, the scheme does not ship |
| Envelope decay carries the player off a roof | AS.1 stop-distance measurement | Hold-then-collapse plus stick-back emergency stop |
| Reversal stutter prevents the acceleration curve | AS.1 | Lengthen the hold window before touching the decay |
| Tracking glitch differentiates into full sprint | AS.4, and the existing 1.31 m precedent | Same 0.25 m bound as 6-DOF; hold rather than decay on rejection |
| Leaning to look down reads as a crouch | AS.3 | Horizontal-offset gate, then the both-hands-dropped test |
| Physical crouch drops the view twice | AS.3, measured before it is corrected | Ramped attenuation of `g_dofOffset[1]` while crouch is asserted |
| Gaze-coupled movement is uncomfortable | AS.4 endurance run | AS.5 hand-derived steering |
| Seated players get spurious crouches | AS.3 seated test | `ArmSwingCrouch = off`, which is why it is its own key |
| Interaction with MotionHands arm writes | Run both on together at AS.4 | None expected — they share pose data but write different destinations |

## Suggested commit sequence

1. `swing: locate grips in room space and report four candidate swing metrics`
2. `swing: choose the metric the measurement supports and name it in the log`
3. `swing: drive the left stick from a hold-then-collapse swing envelope`
4. `swing: compose the swing vector with the physical stick`
5. `swing: jump on a hands-overhead edge, held for the contextual up-actions`
6. `swing: crouch and slide from head height against a standing baseline`
7. `swing: gate on focus, tracking and cinematic input, and decay every failure to zero`
8. `swing: add config, hotkeys, overlay and bounded diagnostics`

## Completion criteria

Complete when a player can walk, run, sprint, jump and slide through a level using only their
body, with the thumbstick still available and still authoritative; when every failure path leaves
the pad neutral rather than stuck; when 30 minutes of mixed play produces no unintended jump,
crouch or sprint; and when `ArmSwing = off` produces a pad byte-identical to today's.
