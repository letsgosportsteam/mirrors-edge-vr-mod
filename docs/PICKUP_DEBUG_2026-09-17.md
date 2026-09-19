# Pickup debugger and indexed-native queries

Follow-up: user confirmed pickup works. Disabled PickupDebug in the installed INI,
retaining the normal blue ready-to-pick-up box. Removed the diagnostic Insert
binding because Insert already controls stereo. Diagnostic geometry is now INI-only;
the shortcut descriptions below describe the earlier test build.

## Failed run and root cause

Run 33528 is preserved in `.analysis/pickup-debug-20260917/user-run.log`.
Frame 6800: pistol 0.663 m from rendered head, 0.57 degrees off gaze, rejected by
`pickup state/cooldown`. Frame 7740: held/tracked right hand 0.040 m from pistol,
no catch. The geometry was sufficient at these points; widening it again was not
a solution. Existing global gate text could overwrite a catch rejection with the
later gaze rejection.

Disassembly of the user's executable proves `UObject::ProcessEvent` returns at
0114AC24/0114AC2B when UFunction.iNative (+94) is nonzero. GetStateName (284) and
FastTrace (548) are indexed natives. Calling them through the previous generic
ProcessEvent path returned success without executing the query; zero-initialized
output was then treated as real data. This blocks both state eligibility and
visibility. The existing mocked tests did not model that engine behavior.

Saved local evidence: `process-event.asm`, `fast-trace.asm`, `frame-constructor.asm`
in the same analysis folder. FFrame layout was checked against 0100D2C0; FastTrace
decodes bytecode at frame+0C, with object at +08. It requires a constant expression
stream, not the ProcessEvent parameter block.

## Implementation

- A restricted adapter in `src/combat_native.inl` calls the resolved indexed native
  exec for only GetStateName/FastTrace. Validates native flag, index, exec slot and
  module address. A local frame supplies vector/bool constants and EndFunctionParms.
  No UFunction metadata or executable code is patched. Calls stay on the game thread.
- Query outputs are poisoned first, and bytecode consumption/output checked afterward.
  Exceptions disable combat calls. Script functions retain the existing ProcessEvent path.
- State comparison is case insensitive, matching Unreal names. Failed query and
  disallowed state now have distinct rejection messages.
- Logs include raw state name/index, eligibility, visibility, lookup override count
  and resulting weapon. This can distinguish detection from actual acquisition.
- `PickupDebug = on` enables cyan gaze boundary, yellow out-of-cone/range boxes,
  orange blocked boxes, blue ready boxes, purple left and green right 25 cm catch
  spheres. Text reports actual state, nearest target distance/angle and rejection.
  Insert toggles it even with the general Debug flag off.
- The overlay consumes a bounded game-thread snapshot refreshed at 10 Hz. Geometry
  renders per eye, with depth disabled intentionally so diagnostic markers remain
  visible. Gameplay still enforces visibility. At most eight target boxes and 2048
  vertices; the cone includes the 12 cm minimum radius and two-metre spherical cap.

## Validation

Passed: test-pickup-native, test-pickup-debug, test-pickup-runtime,
test-combat-spatial, test-pickup-view, test-combat-fire.
The native adapter test verifies VM parameter serialization, routing away from
ProcessEvent, incompatible metadata rejection, and untouched-return rejection.
The geometry test verifies buffer bounds and that cone points match selection bounds.
Updated an old firing-test fixture to include the camera callback added previously.
Static analysis and x86 release build passed (existing warnings plus the deliberate
SEH containment warning in the new native adapter). `git diff --check` passed.
Tests do not replace an in-headset run of the new native dispatch and overlay.

Installed DLL SHA256:
`8F6B6CFC16E03350DA2CEB4CBDEE8CEC41E56684D787BA3C78636A483FE95488`.
Previous DLL and INI backed up in `.analysis/pickup-debug-20260917`.
Enabled PickupDebug in the installed INI. No sun/menu shader rules changed.

Next run: look at a nearby dropped pistol, note box color/status, grip with either
hand, then try a catch. Backspace on a failure records exact query and acquisition
results. Insert hides the visualization; set PickupDebug=off for subsequent normal runs.
