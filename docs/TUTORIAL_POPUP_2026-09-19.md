# Lower tutorial prompt placement

## Follow-up: text left behind and reticle control

User testing found the box moved but its text stayed behind. PID 6560 marker 1,
capture 4591, preserved under `.analysis/popup-text-reticle-20260919`, explains why:
draw 1562 is the backdrop, draw 1563 is an affine 16x16 HUD reticle, and draws
1564–1567 are the prompt text/icon batches. The earlier captures had no intervening
reticle, so the original regression checks missed this order.

Popup grouping now survives affine Canvas HUD draws while leaving those draws at
their ordinary placement. Frame/viewport/non-Canvas/new-panel boundaries still
end the group. Replaying all three captures changes exactly five popup draws in
each tutorial capture and zero in the working Back/Select menu capture.

Added **Comfort > Reticle**, persisted as `ShowReticle`, default Off. The old
unconditional hide in CombatGameplayTick depended on combat readiness and was
skipped in gamepad mode. Its native HUD flag write now runs independently from
Present, with separately validated PlayerController.myHUD and
TdSPHUD.bDisableDrawCrossHair metadata. It handles replacement HUDs and preserves
adjacent flag bits. Enabling restores the game's normal reticle rules.

Build/static analysis, screen-sampling replay, VR menu, and controller tests pass.
The Comfort page preview was checked. Native reticle tests cover both toggle
directions, gamepad/menu/combat-unavailable states, HUD replacement, invalid masks,
default-object exclusion, INI roundtrips, and shipped defaults. Headset verification
is still pending.

Latest installed SHA256: `70939A64654F74692D123B8B9C48469ABCA1C11766030D9984A8B228BAC09D41`.
Backup: `.analysis/popup-text-reticle-20260919/installed-backup-20260919-084319`.
Active INI preserved; shipped example updated.

## Original adjustment

The user verified snap turning, standard controllers, and Auto input selection.
The Back/Select menu (Quest left-stick click) also looks good. Only the lower
training prompts needed repositioning.

Preserved run PID 33480 in `.analysis/tutorial-popup-20260919`:

- Marker 1 at frame 20951; capture 20954 contains 1775 draws, no truncation.
  Draws 1770–1774 are the lower prompt: a nine-slice 1024x128 DXT5 background,
  then icon/font/outline batches using 256x256 and 256x128 DXT5 atlases.
- Marker 2 at frame 23173; capture 23176 contains 1783 draws, no truncation.
  The Back/Select panel starts with a 128x32 background and shares the same
  Canvas shader and font atlases. A shader-wide text adjustment would affect it.

`DrawStereoCanvas` recognizes the captured background using the exact shader
pair 5470E8CC/B3B12B1D, atlas, primitive count, and pixel-coordinate perspective
projection (background W = viewport width/2 + 1). Only the contiguous matching
icon/font draws at W = width/2 inherit its placement. State is shared across draw
entry points and expires at a frame, rejected draw, new panel, or viewport change.
Existing stereo, target, depth, and scene-texture guards still apply.

The prompt group gets 90% of the existing UI scale and an additional 0.12 upward
offset in screen-height units. This gives a modestly more distant angular size;
it does not introduce a new physical-depth OpenXR layer. Scissor and viewport
use the same transform so the panel, icon, and text remain together.

Validation: release build and static analysis succeeded. Screen-sampling tests
exercise grouping boundaries, clipping, failure restoration, and existing menu
paths. Replaying both captures' draw metadata through the production placement
code changes exactly the five prompt draws in marker 1 and zero draws in marker 2.
This is a metadata/device-harness test; headset readability still needs user testing.

Installed SHA256: `5549FFE3287A3F8BEA9A2012E64C01B2218164CDECDB1D03AEC357112C5A9519`.
Previous DLL: `.analysis/tutorial-popup-20260919/installed-backup-20260919-082751`.
The active `mevr.ini` was not changed (hash verified).
