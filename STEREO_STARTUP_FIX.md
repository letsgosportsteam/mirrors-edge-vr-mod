# Stereo startup failure, 2026-09-09

The 17:24:34, 17:25:40 and 17:27:03 runs all found the correct view matrix at c0,
armed simultaneous stereo, submitted a stereo projection, then fell back to a flat
quad because `AdoptSceneTarget` reported a 1280x720 world against a 4224x2376
backbuffer. The preceding 17:22:52 run followed the same path. The two intervening
short runs are different: 17:24:30 exited before Direct3DCreate9, and 17:23:54
ended before stereo armed.

The later two failed runs accepted 46637/46728 and 40326/40354 matrix uploads.
The matrix scan and its watchdog were working. Full-resolution render-target
creation succeeded, and gameplay StretchRect records remained 4224x2376. There
was no logged allocation failure explaining a resolution downgrade.

## Defect

`Hook_SetRenderTarget` called `GetDesc` on every bind, but its census retained the
first width, height and format seen at each raw COM pointer for the entire run.
The engine creates 1280x720 startup surfaces and then full-resolution surfaces.
If the allocator reuses a released surface's address, the census continues to
describe the previous object.

This was not merely a logging problem: `ShouldDuplicate` preferred those stale
dimensions over `g_rtIsScene`, which had just been calculated from the live
descriptor. Thus a full-size HDR pass could be excluded from duplication and
counted as a reduced scene. After 120 frames the mono guard acted on those same
incorrect counters and disabled all stereo duplication. The reported onset after
stereo arming was when scene classification started counting, not proof that the
engine changed resolution at that moment.

The failed startup fingerprints omit a separate full-size HDR working target;
the successful 17:13:07 run records one. Reuse of a startup address explains that
omission because the old census only logged previously unseen addresses.

The earlier MinDesiredFrameRate pin was based on a performance-scaler hypothesis.
It was already active in the failed runs (35 -> 1). It cannot repair stale surface
metadata or override the mod's own mono decision.

## Fix

- Validate cached dimensions and format against the live descriptor on every
  successful bind. On a mismatch, log both descriptors and discard old counts.
- Keep failed binds from changing the tracked active target.
- Replace the 16-entry census with stable growable storage. Startup alone binds
  more than 16 targets; later targets must participate in the guard's decision.
- Clear target tracking, size overrides and mono flags after a successful Reset.
- Recognize the fixed 1280x720 reduced buffer as well as exact half-size buffers.
  Previously the `[mode]` diagnostic printed `healthy` at 4224x2376 because its
  check required dimensions to be exactly half of the backbuffer.

The mono safety guard remains in place for genuinely reduced rendering.

## Verification

`tools/test-render-targets.ps1` compiles the production hook, mono guard and draw
gate with a fake D3D device. It exercises address reuse in both directions,
discarding stale counts, mono entry and recovery, failed binds, secondary MRT
slots, more than 16 targets, device-reset tracking and reduced-size detection at
both 2560x1440 and 4224x2376. All checks pass. Running with
`-SimulateStaleCache` removes only descriptor refresh and fails at the first
stereo draw after address reuse, reproducing the defect.

The x86 build and the project's static-analysis checks pass (existing advisory
warnings remain). The fixed DLL is installed with the previous DLL backed up in
the ignored diagnostic directory. Three live startups allocated and rendered
full-resolution targets without entering the mono fallback; all three used
distinct addresses for the startup and full-size HDR buffers, so they did not
directly reproduce address reuse. The deterministic regression covers that case.
The live startup logs and original failing logs are preserved in the diagnostic
directory. A headset gameplay check is still needed to verify sustained stereo
visually; a successful startup alone does not establish that result.
