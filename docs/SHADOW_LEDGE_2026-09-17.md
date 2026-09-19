# Shadow capture and first-ledge correction

The user's 07:18 run is preserved in `.analysis/shadow-ledge-20260917/user-run.log`, with its shader, vertex and texture captures. Backspace captured frame 7911 (capture ID 7914). The flare textures include a glow, ring and starburst; this identifies the sprite family but does not yet explain the incorrect eye placement. No additional speculative rendering correction is included in this build.

## Ledge height

The game supplies the ledge location; the VR mod measures an offset from that ledge line to the authored hand pose, then uses it to place grip anchors. Previously it sampled left then right, unconditionally overwriting the shared offset. The log announced the left sample even though the right sample became the actual value.

In this run the left settled offset was Z -11.4 and the right was Z -46.2. This placed both anchors about 35 game units too low. The code now gathers both valid samples in the same arm update, selects the higher world-space hand, and only then publishes the shared offset and its log. It retains the existing 30-frame settling window, with no requirement for equal hand heights. Pipes retain their separate per-hand offsets. Missing/invalid samples cannot borrow the other hand's stale height.

## First-grab stall

Frame 9097 entered Grabbing at t=140.28; frame 9098 arrived at t=143.00. Between them the old `PkVerifyProcessEvent` experiment ran full object-table lookups for GetGrabType and four more functions and dumped their parameters. Arm-update timing measured a maximum 2699.36 ms of mod work. ParkourGeomCensus and ParkourDebug were both off, but this verification call was unconditional.

Both obsolete verification/comparison calls now require ParkourGeomCensus. Their results do not drive parkour movement. Combat retains its independent metadata setup and validation. The next headset run must confirm the first-grab stall is gone.

## Backspace rendering diagnostics

`effect_capture.inl` captures the next complete frame after a marker. It records before/after images around complete draw calls across all four drawing APIs, including both-eye rendering. Known shadow, fog, haze, distortion and flare shader families get a separate budget from unknown blended/depth-disabled passes, so ordinary opaque building materials cannot exhaust the capture before the effects arrive.

Limits: 24 known draw pairs, eight other draw pairs, four repetitions per shadow shader or two per other shader, and 12 sampled render-target textures. Images are reduced to 1024 pixels wide. Shader binaries, both sets of constants, scene/eye matrices, render states, target/texture formats and route are logged with each image. Sampled render targets are also saved as DDS to preserve floating-point alpha depth that PNG would clamp.

Files are beside mevr.log, prefixed `mevr-pass-<process>-<capture>-<draw>-<VS>-<PS>`. `.before.png` / `.after.png` attribute a visible change to a draw; `.samplerN.dds` retains its input depth/colour values. Each capture logs its elapsed time and failed readbacks explicitly. No image capture/device queries occur outside the marked frame. A marker can pause the game while images are saved; ordinary ledge grabs do not activate it.

The shadow-like artifact is **not yet confirmed fixed**. Capture it visibly displaced with Backspace; separately mark the sun flare if necessary. The old shadow and haze corrections remain unchanged for this diagnostic run.

## Validation

- Release x86 DLL build and mandatory static analysis passed; existing analyzer advisories remain, with no new warnings from these changes.
- Production higher-hand helper: either hand lower, equal heights, missing samples, non-finite samples passed.
- Production pass-capture mock: before/after pairing, inactive-frame and recursive-draw gates, repeat/total budgets, new-marker reset, failed-save resource release passed.
- Captured shader classification, camera/math checks, haze/shadow state restoration, render-target routing and arm restoration checks passed.
- Real graphics-device capture test compiled but could not create a HAL D3D9 device in this session (`D3DERR_NOTAVAILABLE`, 0x8876086A), including outside the sandbox. It remains an explicit unverified check; CPU tests were run with `-SkipDevice` after recording the failure. Actual new image output needs the next game run.
