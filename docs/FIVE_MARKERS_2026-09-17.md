# Five-marker rendering follow-up

Preserved run and captures: `.analysis/five-markers-20260917/` (process 25384).

| User marker | Frame / capture | Report |
|---|---|---|
| 1 | 3595 / 3598 | menu/UI |
| 2 | 4222 / 4225 | menu/UI |
| 3 | 6913 / 6916 | misplaced dark layer |
| 4 | 12129 / 12132 | misplaced dark layer |
| 5 | 18779 / 18782 | lens flare |

The PNG and floating-point DDS readbacks succeeded in the game. Early shadow-mask PNGs were white; several before/after pairs showed no visible change. This does not prove there are no shadow contributions: HDR PNGs clamp bright values. The sampled DDS files retain correct floating-point scene depth. Their RGB/depth inspection and before/after contact sheets are in the analysis directory.

## Lens flare option

`LensFlares = off` is the new default and is installed in mevr.ini. Set it to `on` and restart to restore the effect. The filter targets the captured sprite vertex shader 1612477A with the three glow/ring/starburst materials 1468B5C6, 619A63AF, 1F63E387. All four draw entry points honor it. It does not blanket-disable the sprite factory: other billboards, smoke, birds, sun haze and bloom remain enabled. This is a render-material filter, not a change to the game's packaged files.

## Screen-space lighting sampling

The captured world material 361557F6 declares `ScreenPositionScaleBias` at pixel c1 and `LightAttenuationTexture` at s0. Its vertex shader emits eye-transformed projected position, but the ordinary stereo draw path had left the pixel shader's full-width sampling transform unchanged. Both eyes therefore sampled a full-width screen texture instead of their own halves. The earlier projected-shadow correction covered shadow creation, not the materials that consume the lighting mask.

The world draw path now reads the shader's bounded SM2/3 CTAB metadata and adjusts ScreenPositionScaleBias for each eye when its named LightAttenuationTexture or SceneColorTexture is a scene-sized render target. It preserves the full-texture half-texel bias and restores constants after both draws. Ordinary texture atlases, small shadow maps and shaders without those named inputs are excluded. Material metadata is cached by retained shader identity, avoiding repeated shader inspection as material counts increase. Device reset releases the cache.

This fixes a verified coordinate mismatch and is a candidate correction for the reported silhouette. The relationship to the user's complete visual artifact still needs headset verification; it is not claimed resolved from these captures alone.

## Canvas UI

`StereoUI = on` duplicates the recognized Canvas vertex shader 5470E8CC into both eyes, with per-eye viewport and scissor rectangles. It requires an affine screen transform, disabled depth, scene-sized output and an active stereo frame. It rejects scene-texture composites across texture slots 0–7. World matrices are not substituted into UI. Set `StereoUI = off` to disable this targeted path.

The first two capture budgets were consumed by blended world meshes before the menu draws. The new capture filter reserves unknown-pass slots for screen-space work and explicitly includes the Canvas shader. HDR before/after pairs now also save DDS so a clamped PNG cannot hide a lighting change. This allows remaining UI variants or final-image artifacts to be identified on the next marked run.

## Validation

- Release x86 build and mandatory static analysis pass; existing advisories only.
- Production screen-sampling and Canvas/flare tests pass using actual captured shader bytecode: CTAB register resolution, malformed input rejection, per-eye UVs, half-texel bias, state restoration, cache reuse, target/texture exclusions, bilateral clipping, failed-draw restoration and settings gates.
- Render-target routing, haze/projected-shadow restoration, diagnostic capture bounds and existing shader/camera tests pass. Standalone graphics-device validation was explicitly skipped; the user's run has now verified the previous capture implementation's actual PNG/DDS output. New visual changes still require the headset run.
- Existing gun calibration and parkour settings are preserved. No new changes to combat or parkour behavior are included.
