# Scripted-camera visibility and native sun flares

User confirmed textures and scripted head tracking are correct in the previous build, but looking away from the authored view reveals missing geometry. A separate sun reflection was marked with Backspace.

## Evidence

Latest captured run: PID 28120, marker 1 at frame 6032 / 96.45 seconds; effect capture frame 6035. Saved under `.analysis/culling-sun-20260917/`.

The existing head-look correction happened during rendering, after engine visibility selection. The CPU frustum was already wider than the forced eye view (135.4 x 107.8 degrees versus 93.3 x 100); increasing that margin cannot cover looking behind the authored camera. Occlusion query overrides were already returning visible.

The flare capture shows a sky starburst and a bright secondary spot displaced differently between eyes. The secondary spots already exist before captured draw 4. That draw's 2C531FCD pixel shader has no texture sampler, so its bound starburst texture is not proof that it creates the flare. The exact producing draw remains unconfirmed; no additional material/shader hash is suppressed on that assumption.

## Changes

- Reuse the existing interpreter hook at the return from `TdPlayerPawn.CalcCamera`. Apply the same camera-local headset rotation to its output and cached pawn camera rotation before engine culling. Script/controller rotation remains unchanged.
- Restrict this to the existing scripted-look conditions and a first-person pawn view. Verify function/property identities, output/cache agreement, and the shipped executable's FFrame/OutParmRec instructions before writing. Failed checks retain the existing render correction.
- Recognize the early correction in the renderer and skip applying the head offset twice. Keep the existing local-axis math, stereo scissor fix, and texture/shadow/UI paths.
- Extend `LensFlares=off` to call the native `LensFlareComponent.SetIsActive(false)` on active flare components. This covers native flare materials beyond the three existing captured shader filters, while preserving light actors and atmospheric/bloom paths.
- Scan at most 512 object slots per game frame; reflection discovery stays on the existing background worker. Startup/level changes or reactivation may take a few seconds to reach a component. Configuration changes require restarting the game.
- Log early-camera readiness/application and the names of disabled native flare components for verification on the next run.

The early camera hook and native flare scan currently share the existing motion-hand/pistol hook initialization. Both features are enabled in this setup.

## Verification

- Mandatory build static analysis and x86 release build passed; existing advisory warnings remain.
- Production camera math tested across steep, rolled, and backward views against the renderer's rotation; guarded outputs, malformed records, mismatch rejection, and write rollback passed.
- Native flare API, class/active-bit filtering, setting, and per-frame bound tests passed.
- Stereo regression tests passed, including preventing double head rotation. GPU device checks were explicitly skipped; no headset visual validation was performed.
- Combat initialization/performance tests passed, including nonblocking metadata discovery and game-thread-only calls.
- Shipped package checks passed for camera and flare function identities and parameter/property types. Runtime validation checks their offsets and native call layout.

## Deployment

Game was closed. Backed up the prior DLL to `.analysis/culling-sun-20260917/d3d9-before.dll` and installed to `<game>\Binaries\d3d9.dll`. Installed hash matches the build:

`54DD8B442913F7246E2DBAC28DF701A1E71EC4C7B01B8DC660E9BB58C211E264`

Prior hash: `C09FF949238CFBBC0B404D6A71DC472D874635B7D2C678F28B7DBF1FD72BC6C0`.

Existing game INI already has `LensFlares = off`; no user calibration/settings were changed. The missing geometry and marked sun reflection still require in-game validation.
