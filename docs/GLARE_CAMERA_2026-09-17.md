# Glare, UI and camera follow-up — September 17

Evidence: `.analysis/glare-camera-20260917/user-run.log`, process 23944.
Use markers 1–4: captures 17275, 18849, 21266, 27393. Marker 5 is excluded
from issue attribution, as requested. Existing packed lighting UV correction and
the three disabled lens-flare materials are retained.

## Changes

- **Sun glare:** captured VS 1612477A / PS 2C531FCD places its sprite about 12
  game units from the original camera. Normal eye separation therefore creates
  excessive disparity. This material now retains corrected rotation/FOV while
  both eye matrices are reanchored to its original CameraPosition. Other particle
  materials keep positional stereo. State is restored after either draw fails.
- **UI:** the previous Canvas gate accepted only affine transforms. The tutorial
  panel actually uses a perspective pixel-coordinate transform (c7.w=1,
  c8.w=2113), so it was excluded. That measured transform family is now accepted,
  retaining exact shader, depth, output-target and scene-texture guards, and
  per-eye scissor handling. This targets the captured in-game panels; main-menu
  paths without an active stereo scene still need separate evidence if affected.
- **Left-eye artifact:** no-color UP geometry was always excluded, including
  stencil-writing shadow masks. Validated world geometry now duplicates when
  it writes stencil, including two-sided CCW operations. Occlusion-only geometry,
  read-only stencil, foreign transforms and non-scene targets remain excluded.
  This is a concrete rendering omission, but its connection to marker 2 is not
  yet proven. Marker-only stencil traces record the shader, route, viewport,
  masks, operations and scene gates for up to 48 stencil draws, including those
  previously invisible to the color-pass diagnostics.
- **Head yaw:** the fixed ~11-degree per-frame rejection threshold can discard
  valid turns during slow frames. The allowance now scales with elapsed pose
  time up to 45 degrees; invalid time and long pauses retain the original bound.
  This addresses a real logged rejection, not proof of marker 3's specific cause.
- **Head pitch:** markers 3 and 4 are Walking, not a scripted camera. Walking and
  Crouch now correct the rendered pitch directly to the headset target. The old
  matrix-minus-controller “animation” estimate also contained rendering delay.
  Parkour, melee, cinematic and unknown movement states retain their existing
  animation handling. Pitch effects during walking/crouching will consequently
  be suppressed along with that delay. Marker bursts now record target, matrix,
  controller, animation and correction separately every five frames.
- **Reticle:** set the native TdSPHUD.bDisableDrawCrossHair flag on the game
  thread. Both HUD fields resolve in the existing background metadata pass and
  were verified against the shipped packages. Class/type checks and masked
  writes preserve other HUD flags; no shared menu texture/shader is hidden.

## Performance and validation

Full Backspace image captures paused these marked frames for approximately
11–17 seconds. Capture work remains marker-only. Outside those pauses the run
also had periods around 60–64 fps against 72 Hz, so the capture stall is not a
complete explanation of pitch judder.

Passed production-code tests: captured perspective Canvas and lighting sampling;
sun/haze/shadow matrix transforms and state restoration; stencil routing vs
occlusion and non-scene targets; bounded effect capture; camera pitch and
elapsed-time yaw thresholds; shipped HUD/interaction reflection field types.
GPU device validation was explicitly skipped in the camera/shader harness;
headset verification is still required, especially for the left-eye artifact
and the exact marker-3 behavior.

Release x86 build and static analysis completed with existing advisories only.
Installed and hash-verified `<game>\Binaries\d3d9.dll`:
`8CA2F3F5B07615B32421C53397F28B1623C719D88C04F74E7C098DCB88B37B47`.
Previous DLL is preserved in the evidence directory as `d3d9-before.dll`.
INI settings and gun calibrations were not edited.
