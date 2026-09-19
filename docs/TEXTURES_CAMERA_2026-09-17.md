# Disappearing surface detail and scripted head look

Evidence preserved in `.analysis/textures-camera-20260917/`, process 18640,
built September 17 at 09:56. The log contains three markers:

| Marker | Frame / capture | Report |
|---|---|---|
| 1 | 8122 / 8125 | Reversed left/right look while the game controls the camera |
| 2 | 14085 / 14088 | Ground detail visible |
| 3 | 14244 / 14247 | Ground detail missing |

The UI and ordinary walking pitch fixes are confirmed working by the user.

## Surface detail

The final two paired captures show the detail discrepancy before the captured
post-processing passes. Their stencil traces show scene-transformed, depth-tested
surface draws using scissor clipping, including VS 003BD5B4 / PS 7D03F671 and
VS 2E5B7775. `DuplicateDraw` changed the viewport, projection and head pose but
left the engine's mono scissor rectangle active. That rectangle can cut away a
surface layer in one or both eyes as the camera moves. The first image also
contains a sharp vertical boundary through a dark ground detail layer.

World draws now disable that mono optimization for their two stereo draws and
restore its enable state afterward. The eye viewport, depth and stencil still
bound rasterization. The same guard covers the specialized shadow projection
path. UI keeps its separate per-eye content clipping, unchanged. This can
increase raster work for scissored world surfaces; it avoids guessing a depth
for reprojecting a two-dimensional rectangle after head rotation/FOV changes.

Either-eye draw failures are now retained in the returned HRESULT. A successful
second eye no longer hides a failed first eye.

Marker diagnostics additionally record exact scissor bounds and include paired
captures of the three observed scissored surface shader families. This will
identify any remaining detail issue rather than only capturing later effects.
Headset confirmation is still needed to establish that this explains every
reported disappearing texture.

## Scripted walking camera

Marker 1 is Walking. The surrounding input census records `bIgnoreLookInput=1`
and `bIgnoreMoveInput=1`, while cinematic flags remain zero. The old cinematic
head-look gate deliberately ignored this non-cinematic case, so controller yaw
writes and the yaw-lag correction could compete with the script.

Walking/Crouch with ignored look input now uses the existing rendered head-look
overlay: controller head writes pause, yaw-lag correction pauses, and relative
head rotation is composed with the authored camera. Normal walking and the
existing temporary parkour input restrictions retain their current behavior.
This is deliberately scoped to the observed ownership condition; unrelated
look-at mechanics are not broadly disabled.

## Validation

Passed tests: production stereo world draw/scissor state restoration and failed
draws; shadow/haze/sun matrices; actual cinematic gate with scripted walking,
cinematic flags, parkour and failed reads; walking pitch and head-step guards;
Canvas UI clipping; lighting sampling; render-target/stencil routing; bounded
effect captures. Camera/shader harness graphics-device checks explicitly skipped.

Release x86 build and static analysis completed with existing advisories only.
Visual results require the next headset run.

Installed and hash-verified `<game>\Binaries\d3d9.dll`:
`C09FF949238CFBBC0B404D6A71DC472D874635B7D2C678F28B7DBF1FD72BC6C0`.
Backup: `.analysis/textures-camera-20260917/d3d9-before.dll`.
No INI or gun calibration changes.
