# Pickup, scripted camera and effect follow-up

User log preserved at `.analysis/pickup-effects-camera-20260916/user-run.log`.
Markers: frame 1907 / 38.11 s (gameplay cutscene), 7880 / 122.23 s and
15075 / 222.30 s (head-following layer), 9730 / 147.93 s (flare-like effect).
Zipline is frames 16132–16468 / approximately 237–242 s.

## Findings and changes

- Spatial metadata was ready in the user run, but no selected-pickup request
  appeared. The old log did not record rejection reasons, so it does not prove
  which eligibility gate failed. Native nearby lookup now seeds candidates at
  10 Hz instead of relying solely on an approximately nine-second object-table
  sweep. The thrown-object hook and bounded fallback sweep remain.
- Pickup targeting now uses `TdPickup.PickMesh.GetPosition()` when available.
  The package declares PickMesh as ComponentProperty; the return value of
  PrimitiveComponent.GetPosition is a vector. Both are validated at runtime.
  Visibility accepts any nonzero native UBOOL. Pickup still checks state, ammo,
  inventory room, gameplay input eligibility, head distance and occlusion.
- Catching lasts while grip and hand tracking are valid, including a grip
  already held as the other hand releases the pistol. The former 400 ms expiry
  could miss a normal toss. The 18 cm swept-distance check remains.
- Blue selection is drawn by the mod in both eyes. It does not depend on the
  retail engine's debug-line renderer. It expires after 250 ms without a new
  eligible selection, and restores D3D state after its own scene pair.
- `[combat-spatial]` now reports candidate count and rejection stage at most
  once per two seconds while idle, plus failed pickup requests and registrations
  made during a throw. This distinguishes discovery from eligibility failures.
- The gameplay cutscene has cinematic/look-disable flags set even though its
  view target is the player pawn. During those flags, headset yaw/pitch are
  applied relative to cutscene entry in the render matrix. Controller rotation
  writes and normal yaw/pitch correction do not compete with the script.
- Render-time camera sampling now refreshes both position and direction used
  for matrix acceptance. Acceptance checks full reconstructed position and
  forward direction against current/previous samples. Previously a matrix
  displaced along screen-right/up could pass the W-at-camera test.
- Camera rotations pivot around the accepted matrix's own camera position.
  The old pivot could belong to a newer game tick, especially on the zipline.
- UP draw classification now follows vertex-shader constant dependencies into
  POSITION. An inherited c0 is insufficient to duplicate screen composites.
  Depth-disabled geometry qualifies when its position actually uses the scene
  VP, covering that class of world flare without broadly duplicating UI.

## Validation and remaining uncertainty

The x86 build/static analysis and existing combat, calibration, restoration,
performance and render-target harnesses pass. New tests compile real SM2/SM3
world and screen shaders, exercise malformed input and unused VP constants,
moving-camera acceptance, rejection of an off-axis shadow matrix, and cinematic
rotation without camera translation. Production pickup context/eligibility and
tick functions are tested against mocked engine calls, including both hands,
pre-held grips, expiry of the former catch window, tracking loss and occlusion.

These are code-level checks, not headset confirmation. The original effect log
contains draw-state counts but no shader identities, so the exact head-following
layer and flare remain unconfirmed. Backspace now captures bounded shader/state
evidence over three frames: `[effect-marker]` entries plus deduplicated
`mevr-effect-*.vs.bin` / `.ps.bin` files beside mevr.log. Unsupported shader
control flow, preprojected screen flares and depth reconstruction may still need
an effect-specific correction. External CameraActor cutscenes are not covered
by the pawn-camera acceptance change.

Throw velocity/spin, separate-hand gun calibration, normal Y, disarm gesture and
the 0.75-second combo gap are retained. No save or calibration edit is needed.

Installed DLL and build output SHA-256 both match
`4AF0946FCED5493CFC6DA835BAE50855052436A9624A7D953DA9F6E6CA038A24`.
The previous installed DLL is preserved as
`.analysis/pickup-effects-camera-20260916/d3d9-before.dll`.
The saved legacy pistol calibration still reads 40 degrees down / -10 right.

## Follow-up: reversed horizontal look, zipline and effect regressions

The next user run confirmed the shadow-like head-following layer and first
cutscene camera glitches were fixed, but reported reversed **left/right** look
near cutscene entry, zipline glitches, and regressed smoke/birds. The flare-like
effect remained incorrect. The run and shader captures are preserved locally in
`.analysis/effects-cutscene-followup/`; game shader assets are not distributed.

Changes in the follow-up build:

- Cinematic head offsets now rotate about the authored camera's right/up axes.
  World-up yaw could reverse screen direction for steep/overturned camera poses.
  The reference headset pose is captured when the view actually hands back from
  a CameraActor to the pawn, instead of during the preceding external shot.
- The shipped `TdMove_ZipLine.UpdateViewRotation` cancels automatic look-at only
  when `DeltaRot.Yaw` is nonzero. Headset controller writes bypass that stick
  delta, leaving the automatic target active. For focused VR head tracking, the
  script hook cancels that target and disables the move's stick look constraint
  for the duration of this call, then restores the constraint. Field/function
  identities come from reflection, with optional entries so a missing camera
  field does not disable weapon interaction metadata.
- The shader dependency parser now consumes relative constant-address operands.
  Captured particle shaders `23C40ECF` and `746AB0D0` use `c13[a0.x]` before their
  explicit c0..3 view-projection transform; the former parser rejected the shader
  immediately. These shaders now qualify, including with depth disabled, while
  fullscreen composites still fail the world-POSITION dependency check.
- Captured sun-haze VS/PS pair `51B6BA4A` / `F2935D14` reconstructs viewing rays
  from InvViewProj c5..8 and ViewOrigin c9. It now draws with each eye's inverse
  matrix/origin and samples the corresponding half of the scene texture. A
  fingerprint-specific shader rewrite adjusts TEXCOORD0 using c254/c255.
  The original shader, constants and viewport are restored after both draws.
  This is a plausible source of the reported flare-like effect; matching the
  exact visible artifact still requires a headset test.
- Effect-marker output now records render-target and viewport dimensions.

Validation passed: x86 DLL build and its required static analysis (existing
advisory warnings only), captured/compiled shader tests, steep-camera horizontal
look tests, matrix inverse tests, haze draw state restoration on success and
failed draws, render-target/mono guards, combat interaction and performance
tests, and all 25 reflected field types plus the zipline function identities
against the shipped packages. The matrix acceptance and pivot fixes confirmed
by the user remain in place. No new in-headset validation was performed here.

Installed `<game>\Binaries\d3d9.dll` matches the built SHA-256:
`2D907777EA4E368267DBD7F3FE6D83B5CF4E90093F2AB8B1AA1E9986CCBF72AB`.
Backup: `.analysis/effects-cutscene-followup/d3d9-before.dll` (the preceding
`4AF0946F...` build). No calibration, save, or gameplay configuration was edited.

Next headset check: turn left/right immediately after the opening gameplay
cutscene begins; ride the zipline; view smoke/birds, the flare-like effect and
the formerly head-following layer. Mark any remaining error with Backspace.
Expected new log entries include `[head] zipline automatic look target cancelled`
and `[stereo-fx] sun haze drawn with per-eye rays and packed scene UVs`.

## Follow-up: screenshot of displaced silhouette and vault after marker 4

The 22:26–22:34 run confirms horizontal cutscene look is fixed and particles are
stereo again. The user reports a remaining displaced dark silhouette, improved
but imperfect zipline camera, and a vault glitch shortly after marker 4. Log and
captures: `.analysis/shadow-ledge-followup/`. Marker 4 is frame 27962 at 404.60 s;
the subsequent authored movement is VaultOver. The zipline's automatic-look
cancellation is confirmed in the runtime log.

The haze shader did **not** run in the previous build: CreateVertexShader returned
`0x8876086C` (invalid call). Disassembly alone had not validated D3D9 shader read
ports. The injected MAD read two distinct constant registers; it now loads c255
into unused r2 first, then reads only c254 in the MAD. The corrected captured
shader passed CreateVertexShader on a real local HAL device (`S_OK`). The test
uses a hidden 32-pixel window; device creation requires running outside this
machine's restricted sandbox.

Captured shadow-projection PS `00693E34` and `C7EAFDDE` use screen-depth samples,
ScreenPositionScaleBias, and ScreenToShadowMatrix. The former also reconstructs
world position for local-light attenuation. Duplicating only the volume's VS
projection left these pixel calculations in the original camera's coordinates
and sampled the full packed depth image for either eye. These exact shader pairs
now receive per-eye UV scale/bias and rebased screen-depth-to-world/shadow
matrices. The rebase accounts for linear scene depth rather than treating it as
clip Z, and preserves the game's original world-to-light mapping. All constants
and viewport are restored. This is a strong candidate for the screenshot's
displaced silhouette, but the visual match remains to be confirmed in VR.

During VaultOver, GrabPullUp, IntoZipLine and ZipLine with animation yaw enabled,
head-lag estimation now uses the controller/head pair. Previously the rendered
matrix added authored animation yaw to that estimate, causing the slow mean to
fight and later release an intentional camera turn. The mean resets at the
reference transition. Ordinary movement retains the existing render-matrix
estimate; the corrected cutscene path remains unchanged. This addresses one
concrete source of camera conflict, not a claim that every remaining glitch has
been reproduced or resolved.

Passed: build/static analysis (existing advisories), actual device shader
creation, captured shader and camera tests, haze/shadow draw harness, and
render-target routing tests. Both shadow variants reconstruct the same world
and light positions with changed yaw, FOV and IPD; successful and failed draws
restore their rendering state. No in-headset visual test was performed.

Installed DLL SHA-256:
`A2822EC9F451B9BC8FF7FD8E7E87ED154AE67131CC8CFABE97ED84E826802C0E`.
Backup: `.analysis/shadow-ledge-followup/d3d9-before.dll` (`2D907777...`).
Calibration/configuration were not edited. The user's separate phone photo of
the lens flare is still pending; its identity is not yet confirmed.

## Phone photo: small cyan flare spot

The subsequent phone photo circles a small cyan spot over the blue roof. This is
visually distinct from the broad sun-haze pass. The user believes it was visible
at marker 3. In the preserved 22:26 run, VS `1612477A` with three additive pixel
materials appears at markers 2 and 4, while marker 3 contains a regular additive
sprite (`746AB0D0` / `AF162196`). None of the captures includes the textures or
vertex inputs needed to assign the pictured spot to an exact draw. Do not claim
that the haze change fixes this specific flare.

A bounded Backspace diagnostic now records sprite textures as PNGs, vertex
declarations, VS/PS constants, draw route and up to 16 input vertices for UP
draws. It reserves separate budgets for regular particles and flare-style
sprites (8 and 16 draw records), with at most 16 textures no larger than
1024x1024. Render-target/depth textures are excluded from image export. Shader
capture now also covers buffered Primitive/Indexed draws; their vertex buffers
are not read back. No texture export or extra GPU query runs outside the marker
window. Export may cause a brief hitch at the marker itself.

Files are local beside `mevr.log`: `mevr-flare-<capture>-<draw>.texture.png` and
`.vertices.bin`, matched to `[flare-marker]` records. PNG export uses the already
installed system D3DX9 helper, without adding a DLL to the game directory. A real
HAL-device test verified a usable PNG, exact vertex bytes and an unchanged bound
texture. Rendering behavior is unchanged by this diagnostic update; the cyan
flare remains unresolved pending a marker with it visible in this build.

The newer log copied into `.analysis/lens-flare-photo/latest-run.log` is from
22:42, still identifies the 22:24 build and records the old haze shader failure.
It cannot validate the later shadow/haze/camera fixes. Preserve that distinction
when interpreting the next run.

Diagnostic build installed and hash-verified:
`47D8E4CE50A9FAD8190A00D17B664F72CD3CD6A0EA52B70669E20449A2472457`.
Previous DLL: `.analysis/lens-flare-photo/d3d9-before.dll` (`A2822EC9...`).
Build/static analysis and render-target/haze/shadow regression checks passed.
