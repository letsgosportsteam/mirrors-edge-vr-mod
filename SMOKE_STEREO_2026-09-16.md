# Smoke / dynamic world geometry stereo

Follow-up: the user confirmed smoke and birds are fixed, but reported a new
head-following layer and one flare-like effect. The current build replaces the
depth-test-only UP gate with shader POSITION dependency checks and adds fresh
camera validation. See [PICKUP_CAMERA_EFFECTS_2026-09-16.md](PICKUP_CAMERA_EFFECTS_2026-09-16.md)
for the changes and unverified effect-specific limits. The remainder records
the preceding build.

The user marked smoke at frame 18293, 275.38 seconds into the September 16 run.
The surrounding log keeps the full-resolution stereo scene active, with stable
camera/matrix motion. Nearby UP draw samples include transparent, depth-tested
geometry (8–112 primitives per draw). The log does not identify a particular
smoke shader, so those draws are evidence of a missing rendering path, not proof
that every smoke effect uses it.

The ordinary DrawPrimitive / DrawIndexedPrimitive hooks duplicate recognized
world geometry using the two eye matrices. DrawPrimitiveUP and
DrawIndexedPrimitiveUP previously only offered the disabled experimental UI
duplication path. Dynamic world geometry submitted through UP therefore escaped
world stereo duplication.

Both UP hooks now first test the existing scene-matrix, render-target, stereo and
mono-safety gates, plus color writing, depth testing and an active vertex shader.
Eligible draws use the same per-eye matrix and viewport routine as ordinary
world geometry. No-color occlusion boxes, depth-disabled HUD/composite draws,
foreign transforms, fixed-function draws and reduced-scene mono states are
excluded. The experimental UI rule remains separate and unchanged.

`[stereo-fx]` records the count of world UP draws duplicated over each 600-frame
window. This gives the next smoke test a direct way to verify that the newly
covered rendering path was exercised.

`tools/test-render-targets.ps1` passes the original render-target lifetime and
mono regression tests plus both UP entry-point routing tests, with exclusions
for occlusion geometry, UI/postprocess, foreign transforms and nested draws.
The x86 build and required static analysis pass (existing advisory warnings
remain). No headset visual verification was possible: both the new DLL and the
previous working DLL hit the same DXT texture startup error before device creation
in the current desktop session.

This change covers a class of dynamic world draws rather than a single smoke
instance. It does not establish that all smoke is fixed: reduced-resolution
particle passes, screen-space depth sampling, distortion or a different shader
matrix layout can require separate treatment after a headset test.

Original user log, comparison startup logs, build output and the previous DLL
are preserved in the ignored `.analysis/throw-gaze-smoke-20260916` directory.
