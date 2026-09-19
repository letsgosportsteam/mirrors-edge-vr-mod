# Missing release smoke: deeper capture analysis — 2026-09-19

Follow-up to [the second four-run comparison](RELEASE_PARITY_2026-09-19.md).
**The disappearance is confirmed, but its cause is not yet established.** No
smoke rendering fix or headset verification is claimed here.

Update after the diagnostic run: the follow-up below establishes the immediate
failure (displaced particle positions and zero depth-fade opacity). Why the game
produces those positions remains under investigation.

**Current status: tabled at the user's request after the PhysX-off follow-up.**
The user also reproduced the issue in flat mode and confirmed that the working
test installation is based on GOG, while the failing installation is Steam.
There is no confirmed fix or demonstrated storefront-specific root cause.

## What the existing evidence establishes

The corresponding rooftop draw uses VS `23C40ECF`, PS `FABE3964`, and 244
primitives in both captures. Steam PID 24092/frame 8553/draw 1593 does not add
visible smoke; test PID 16104/frame 5257/draw 1644 does. Both use
DrawIndexedPrimitiveUP, are classified as scene geometry, duplicated for stereo,
and unsuppressed. Both have alpha blending, source-alpha/inverse-source-alpha
factors, depth testing, no depth writes, and all color channels enabled.

The pixel shader calculates opacity as:

```text
depthUV = (projectedPosition.xy / projectedPosition.w) * c1.xy + c1.wz
fade = saturate((sceneTexture(depthUV).alpha - projectedPosition.w) * 0.004)
opacity = fade * particleTexture(uv).green * particleColor.alpha
```

Both scene samplers are full-size 4224x2376 render-target textures, format
A16B16G16R16F. Existing DDS readbacks contain meaningful scene depth in alpha,
including the rooftop geometry. They are not empty textures. This does **not**
prove the depth sampled at each particle fragment is correct: existing readbacks
were downsampled and do not retain this draw's exact particle inputs.

The different VS c13/c14 billboard axes mentioned in the earlier note are not
evidence of the cause. Both draws have c19=(1,0,0,0), selecting the camera-facing
c16/c17 basis; the other basis contributions have zero weights. The shader still
executes their arithmetic, so a non-finite intermediate would require separate
evidence rather than assuming multiplication by zero makes it harmless.

All 1,570 shared files compared beneath the game Effects, Materials, and Maps
directories have matching sizes and SHA-256 hashes. The test installation has
799 additional files in those directories. This rules out mismatched bytes in
those shared assets, not all game-installation differences: the executables
differ, and this comparison does not establish which package each executable
loads at runtime.

The rendering modules are unchanged between a9442b4 and release 3200db0. The
known combat/interpreter initialization failure explains the camera-culling
failure, but the smoke draw is present; no evidence yet links that initialization
failure to the smoke's missing pixels.

Local evidence remains under ignored `.analysis/release-parity-20260919/`,
including the suntrace binaries/atlases, `depth-comparison.png`, and
`game-asset-differences.json`. Shader disassemblies are retained in the earlier
glass-reflection and shadow-ledge capture directories.

## Focused diagnostic candidate

`src/smoke_capture.inl` adds a read-only capture immediately before the existing
IndexedUP diagnostic scopes. It only runs on the selected Backspace frame and
the shader pair above, with a maximum of two matching draws per marker (the
distant plume and rooftop plume in the supplied captures). It records:

- Exact UP vertex/index bytes, vertex declaration, original shader constants,
  scene matrix, and the matrices calculated for both eyes.
- Viewport/scissor, depth/alpha/stencil/culling/blend state and both sampler states.
- Full-resolution DDS copies of scene-depth sampler 0 and opacity sampler 1.

Files are named `mevr-smoke-PID-SERIAL-DRAW.*` beside `mevr.log`. The state file
has a versioned x86 binary schema declared in the source; HRESULTs distinguish
failed state reads from legitimate zeroes. Buffer reads are capped at 1 MiB each,
and texture captures at 16 megapixels. Existing general captures remain enabled.
This adds capture-time disk/memory cost; it is an unpublished diagnostic, not a
release artifact. It does not read back the hardware depth/stencil surface, so
that remains a possible further investigation if the new inputs are inconclusive.

An isolated diagnostic build is based on release **3200db0 plus only this capture
module and its include/call**. It excludes the pending startup/combat fixes so
they do not confound this comparison. Source, build log, DLL/PDB and manifest
are retained under `.analysis/smoke-root-cause-20260919/`. No installed DLL or INI
was changed. The current working-source build also passes compilation and the
required static analysis, with existing advisory warnings.

`tools/test-smoke-capture.ps1` passes the production module against a device with
read methods only: marker/shader gates, exact saved bytes, two-draw budget,
per-marker reset, missing input handling, and declaration release. The menu
harness passes buffer-bound/overflow checks and the inactive-capture gate. These
are automated checks, not verification of real-device DDS capture or smoke.

Next evidence needed: reproduce the missing rooftop smoke using the isolated
diagnostic DLL and press Backspace once while looking at that smoke location.
Keep the same INI and loader. Verify the new `[smoke-capture]` results before
interpreting the saved particle opacity and depth calculation. A matching good
capture may still be needed depending on what the failing inputs show.

## Diagnostic run, 15:29–15:31

The user ran the isolated diagnostic in the Steam installation. PID 31928,
marker 1 at frame 6407, captured serial 6410. The log identifies the diagnostic
build timestamp, and both matching draws successfully saved state, vertices,
indices and both full-resolution textures. The final image still lacks the
rooftop plume. Evidence is preserved in ignored
`.analysis/smoke-diagnostic-run-20260919/`.

The captured declaration specifies POSITION float3 at offset 0, previous position
at offset 12, size at offset 24, rotation at offset 44, particle color/alpha at
offset 48, and quad corner coordinates at offsets 80/84, with an 88-byte stride.
All four vertices of each quad agree on its centre. LocalToWorld is identity.

| Draw | Quads | Centres near world origin | Previous position exactly zero |
|---|---:|---:|---:|
| Distant plume | 208 | 206 | 206 |
| Rooftop plume | 119 | 118 | 118 |

The 118 displaced rooftop centres range from approximately
(-0.45, 0.91, 1.81) to (0.04, 2.70, 2.71) world units. The remaining centre is
approximately (2858.69, -6750.20, 5397.14). The camera is approximately
(2899.67, -6639.64, 5549.59). Particle alpha is not uniformly zero: the displaced
rooftop particles reach 0.2895, and the opacity texture contains nonzero values.
Alpha test, stencil test and scissor test are disabled; color writes and blending
are enabled.

`replay_positions.py` decodes the captured declaration, evaluates the active
camera-facing billboard position arithmetic, projects through the captured eye
matrices, and checks the full-resolution scene-depth DDS across each complete
displaced-geometry bounding rectangle, including a two-pixel margin. It does not
approximate the scene depth from the older downsampled captures.

For the displaced rooftop quads, particle depth is at least 6822.37 units while
scene depth throughout those bounds is at most 213.125. Consequently the shader's
`saturate((sceneDepth-particleDepth)*0.004)` term is zero throughout those quads in
both eyes. The distant plume has the same failure (at least 6820.72 versus at
most 214.25). Results are retained in `position-depth-evidence.json`.

This establishes an immediate cause for the disappearing particles: the draw
receives displaced geometry before stereo duplication, and the depth fade hides
it behind the nearby scene. It does not establish which engine/simulation step
displaced the particles, or exclude an upstream interaction with another mod
hook. A shader patch that merely bypasses depth fading would not restore the
particles to the rooftop.

### Simulation/configuration lead, not yet confirmed

The shared user engine INI, saved at the end of this Steam run, has
`PhysXEnhanced=True`. The test installation's local engine INI has
`PhysXEnhanced=False`; that local file was last saved September 16, so its mere
presence does not establish what the successful September 19 test run consumed.
Both files set `bDisablePhysXHardwareSupport=True`.

The test Binaries directory also has a top-level PhysXLoader.dll (version
2.8.0.7), while the Steam Binaries directory has its loader in PhysXLocal only.
Other shared top-level DLLs match apart from d3d9.dll. The actual loaded PhysX
module paths were not recorded, so directory contents alone cannot establish
a runtime difference. No loader or engine configuration was changed.

PhysX is a relevant lead because DICE/NVIDIA's
[GDC presentation](https://developer.download.nvidia.com/presentations/2009/GDC/APEX_Destruction%20_and_MirrorsEdge_GDC09.pdf)
explicitly describes physical sprite particles and smoke in Mirror's Edge
(slides 15 and 41). That source does not diagnose this capture.

The next controlled check is the same diagnostic Steam DLL/INI/loader, with only
the game's PhysX option disabled, followed by a new marker at the missing plume.
This is a diagnostic comparison, not a proposed permanent default or confirmed
fix. It will distinguish the enhanced-physics path from a rendering-only change.

## PhysX-off follow-up and deferral

The next Steam run began at 15:56:06, PID 11428. Marker 1 at frame 6461 produced
capture serial 6464 using the same diagnostic DLL timestamp. The shared user
engine INI now has `PhysXEnhanced=False`; hardware support remains disabled.
The final captured image still lacks the rooftop plume. The previously captured
`23C40ECF/FABE3964` smoke pair is absent from the ordered draw trace, so no focused
`mevr-smoke-*` files were produced for this run. Other particle materials remain
in the trace. Turning enhanced PhysX off removes the matching smoke draws; it
does not restore this plume. This is not validation of a smoke fix.

The user reports the failure in flat mode as well and identifies the working
test installation as GOG-based. Record this as user verification of flat-mode
reproduction, not an instrumented mod-free comparison: the exact flat-mode DLL
loading conditions were not captured. A Steam/GOG game or physics difference is
plausible, but the captures do not establish that all Steam copies have the bug,
or which executable/library/configuration difference causes it.

Evidence is retained in ignored `.analysis/smoke-physx-off-20260919/`, alongside
the earlier complete particle inputs. Per the user's instruction, stop this
investigation for now. No speculative renderer patch, loader replacement, or
additional headset test is required. The diagnostic DLL remains what the latest
Steam log reports; this investigation has not restored or replaced it. No game
settings were changed by the agent. Resume from these captures if the user
chooses to revisit the issue.
