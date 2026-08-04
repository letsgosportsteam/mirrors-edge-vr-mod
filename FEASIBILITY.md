# Mirror's Edge (2008) VR Mod — Feasibility Assessment

Scoping pass 2026-08-04. No code written yet. Everything below was established by direct
inspection of the binaries and shipped data on this machine, or is marked as inference.

Prior project: the **Singularity VR mod** — a working, wearable VR mod for Singularity (2010,
UE3, D3D9, x86), kept in a separate repository. Read its `STATUS.md` before starting here.

## Verdict

**Feasible, and materially easier than Singularity was — with one problem Singularity did not
have.**

The make-or-break hurdle of the last project (getting a D3D9 frame into OpenXR) is **already
solved and shipping**. The target binary is friendlier in every measurable way: no DRM, no ASLR,
an editor-enabled executable, and proven third-party UnrealScript modding.

The novel risk is not technical. **Mirror's Edge drives its camera from animation, constantly and
by design** — wall-run roll, landing dips, vaults, slides, the skill roll. That system is the
game's identity and it fights head tracking directly. Getting a head-tracked stereo image should
be fast; making it *playable* is the project.

## Target identification

Established by direct inspection of both installs on this machine.

| Property | Value | How established |
|---|---|---|
| Engine | Unreal Engine 3, **package version 536 / licensee 43** | `TdGame\CookedPC\Core.u` header |
| Architecture | x86 32-bit | PE machine type `0x014C` |
| Build date | **2009-01-08 13:30:25 UTC** | PE header timestamp, identical in both copies |
| ImageBase | `0x00400000` | PE optional header |
| ASLR | **off** (`DYNAMIC_BASE` clear) | PE DllCharacteristics = `0x0000` |
| LARGE_ADDRESS_AWARE | **already set** | PE Characteristics = `0x0122` |
| Renderer | Direct3D 9 — imports `d3d9.dll` → **`Direct3DCreate9` (non-Ex)** | import table |
| Secondary renderer | `d3d10.dll` + `dxgi.dll` as **delay imports** | delay-import directory |
| Physics | PhysX — `PhysXExtensions.dll` linked, `PhysXLoader.dll` delay-loaded | import + delay-import |
| Gamepad | `XINPUT1_3.dll` (2 fns by ordinal) **and** `DINPUT8.dll` | import table |
| Game module | `TdGame` (DICE's internal name, "Trace Dee") | `TdGame\CookedPC\TdGame.u` |
| Script packages | 12: `Ts`, `Tp`, `TdGame`, `TdSharedContent`, `TdSpContent`, `TdSpBossContent`, `TdMpContent`, `TdTuContent`, `TdTTContent`, `TdMenuContent`, `TdEditor`, `Fp` | `DefaultEngine.ini` `[Editor.EditorEngine]` |
| Video | Bink (`binkw32.dll`) · Audio: Ogg Vorbis + OpenAL | import table |

Every property that mattered for the Singularity architecture — x86, D3D9, non-Ex device,
`XINPUT1_3`, `ImageBase 0x400000`, no ASLR — **matches exactly**. This is an unusually clean
port target.

## The three things that are better here than in Singularity

### 1. ⭐ There is no DRM problem. At all.

Singularity's Steam `.text` had Shannon entropy of exactly 8.000 — fully encrypted — which made
Ghidra impossible on the shipped binary and forced a GOG purchase just to do static analysis.

Mirror's Edge does not have this. Measured section entropy:

| Section | Steam | GOG |
|---|---|---|
| `.text` | **6.480** | **6.480** |
| `.rdata` | 5.502 | 5.502 |
| `.data` | 5.621 | 5.621 |
| `.reloc` | 6.651 | 6.651 |
| `.bind` | 7.986 | *(absent)* |

And the sections are **byte-identical**, not merely similar:

```
.text  sha256   Steam = GOG = 9F74B055D2A0AC7019FDE9A3DB12CDE55502AD19FDE694CC76108813A6DC8893
.rdata sha256   Steam = GOG = E6BB376BBC803094F470B2951AF6ED515062038DA8B53D965E1208B5B56F958A
```

Steam's copy is the GOG image with a 344 KB `.bind` SteamStub section appended and **nothing
encrypted** — the older SteamStub variant that only wraps the entry point. File sizes differ by
349,528 bytes ≈ the `.bind` section plus alignment.

**Consequence: static analysis targets either binary, including the Steam one.** No DRM-free
purchase required, no unpacking step, and no "do these addresses hold on Steam" question — they
provably do, because it is the same bytes.

### 2. ⭐ The shipped executable is an editor build

This is the largest single difference from Singularity, and it changes what kind of project this is.

| Evidence | Count / detail |
|---|---|
| `UnrealEd` string | **2,597** UTF-16 · **2,816** ASCII occurrences |
| `MakeCommandlet` (the UnrealScript compiler) | present |
| `LVT_LockSelectedToCamera` | UnrealEd viewport-type enum |
| wxWidgets (UnrealEd's UI toolkit) shipped in `Binaries\` | 24 DLLs, **including debug builds** (`wxmsw28ud_*`) |
| `TdGame\Config\DefaultEditor.ini` | ships with the game |
| `TdEditor.u` in `CookedPC\` | the editor script package |

And `DefaultEditor.ini` contains UE3's official mod-package plumbing, verbatim:

```ini
[ModPackages]
ModPackagesInPath=..\TDGame\Src
ModOutputDir=..\TDGame\Unpublished\CookedPC\Script
```

**⚠️ One command verifies whether the editor actually launches** (the strings prove it was
*compiled in*, not that the entry point is reachable):

```bash
MirrorsEdge.exe editor
```

Also worth trying: `MirrorsEdge.exe make`.

### 3. ⭐ Custom UnrealScript packages are proven prior art

[softsoundd/MirrorsEdgeTweaks](https://github.com/softsoundd/MirrorsEdgeTweaks) is an actively
maintained modding tool that ships **a custom UnrealScript package** into Mirror's Edge
(described as "cheats and trainer functions, Softimer"), plus built-in UE3 package decompression.
It uses [UELib](https://github.com/EliotVU/Unreal-Library) for package reading.

This matters enormously because of how the Singularity project went. There, the script route died
outright:

- `ProcessEvent` was never located (no symbol, and `FFrame::Step` is inlined at every call site,
  so the call graph could not be walked upward).
- Hooking `GNatives` was tried and **abandoned** — a runtime dump showed only opcodes `0x36`–`0x5A`
  plus `0x60` registered, with `EX_VirtualFunction` (`0x1B`) and `EX_FinalFunction` (`0x1C`) both
  pointing at `execUndefined`. Script calls do not travel that path in that build.
- Everything then had to be done by hardware write breakpoints and binary detours — seven failed
  guesses before a breakpoint found the rotation source.

Here, **replacing script may simply be possible.** Subclassing or patching `TdPlayerCamera` and
`TdPlayerController` in UnrealScript is a far cheaper lever than binary archaeology — if it works.
It is not yet proven for *this* purpose; it is proven that *somebody ships script into this game*.

### Also nice

- **`Engine\Shaders\` ships 115 `.usf` shader source files**, uncooked and editable, including
  `Common.usf`, `BasePassPixelShader.usf`, and a set of `CompilingShader-*.usf` intermediates.
- **PhysX DLLs ship inside `Binaries\`** rather than relying on a system install. The
  `PhysXLoader.dll` null-deref crash that cost the Singularity project a full session is much less
  likely — though `PhysXLoader.dll` is still a *delay* import, which is the same failure shape.

## ⚠️ The one thing that is much harder here

### Mirror's Edge animates the camera. That is the whole game.

Singularity is a conventional corridor shooter: the camera is a POV struct fed from
`PlayerController.Rotation`, and once that one value was controllable the view was controllable.

Mirror's Edge is built the opposite way. The camera is continuously driven by animation and
physics — wall-run roll, landing compression, vaulting, sliding, the coil, the skill roll (a full
pitch rotation), sprint bob, and the first-person body animation that made the game famous. Each
of those is a **forced rotation or translation the player did not ask for**, which is
simultaneously:

- the single most reliable way to induce simulator sickness, and
- in direct conflict with a head-tracked view, which must be authoritative over rotation.

This is visible in the shipped config. `TdGame\Config\DefaultAnimation.ini` exposes an entire
camera-spring subsystem:

```ini
[TdGame.TdSkelControlSpring]
SpringYawInterpVel   = 5.0     SpringPitchInterpVel = 10.0    SpringRollInterpVel = 5.0
MaxPitchDeltaOffset  = 45      MaxYawDeltaOffset    = 45      MaxRollDeltaOffset  = 45
DefaultAngularRotationLimiter = (Pitch=7282, Yaw=4551, Roll=4551)
```

plus `TdAnimNodeInAir`, `TdAnimNodeWallJump`, `TdAnimNodeLanding`, `TdAnimNodeMovementState`,
`TdAnimNodeGrabbing`, `TdSkelControlAim1p`. And `DefaultCamera.ini` confirms the camera class:

```ini
[TdGame.TdPlayerCamera]
FreeflightScale = 1.0
```

**The good news is that this is a tunable, ini-exposed subsystem, not hardcoded.** Several comfort
levers may be reachable without touching code at all — which is a cheap, early experiment worth
running before any of the hard work.

**The bad news is that you cannot simply switch it off.** The animations drive the visible body,
and traversal readability depends on them. Deciding *which* camera motion to suppress, which to
attenuate, and which to keep is a design problem with no reference implementation, and it is the
longest pole in this project.

Related, and unavoidable: **the game is fast.** Rooftop sprinting and long falls are a much larger
comfort problem than Singularity's walking pace. Snap turn and vignetting will not fully solve
free-running. Expect this to constrain how much of the game is actually playable in a headset,
and expect that to be the honest limit of the mod rather than a bug to fix.

## What transfers from the Singularity mod

The live build is one file — `spikes/view_matrix/d3d9.cpp`, ~14,000 lines. Game-specific
identifier counts in it:

| Identifier | Occurrences |
|---|---|
| `RvPlayerCamera` / `RvPlayerController` | 1 each |
| the rotation-source address `0104e390` | 4 |
| `mCurrentPOV` | 21 |
| the hardcoded field offsets `0x0438` / `0x0060` | 3 / 2 |

**The overwhelming majority of that file is engine-agnostic.** Transferring wholesale:

- **The entire D3D9 → OpenXR pipe.** `d3d9.dll` proxy, full device and texture wrapper,
  `D3DPOOL_MANAGED`→`DEFAULT` translation, D3D9Ex upgrade, shared surface → D3D11 → XR swapchain,
  **zero-copy at 0.00 ms**. This was the "nothing else matters until it works" item last time.
- **The stereo mechanism** — draw-call duplication with a per-eye view-matrix offset.
- **View-matrix interception** at `SetVertexShaderConstantF` (device vtable slot 94), including
  the self-validating scan that determines ROW vs COL storage from the camera's forward vector and
  detects translated-world space. That scan is *generic UE3*, not Singularity-specific.
- **Forced projection in the matrix** — the fix for "the engine accepts a wide FOV but will not
  hold it", which is a UE3 camera-interpolator behaviour and will almost certainly recur here.
- **Automatic resolution** inherited from the headset via command-line injection in `DllMain`.
- **Full OpenXR Touch input → synthetic XInput**, menus, haptics, snap/smooth turn.
- **HUD stereo** via rewriting the UI stage-to-clip matrix per eye.
- The in-headset settings panel, ini handling, and the log-rotation discipline.
- Vendored MinHook; the `GObjects`/`GNames` walker and `UProperty` offset resolution *method*.
- **The 32-bit OpenXR runtime finding**: VDXR (Virtual Desktop) is the only usable one — Meta's own
  32-bit runtime crashes in `xrCreateSession` and SteamVR has no 32-bit runtime at all. Same
  machine, same constraint, no re-derivation needed.

### What does not transfer

- **All engine offsets.** UE3 **536** here vs **584** in Singularity — `UObject`, `FNameEntry` and
  `UProperty` layouts must be re-derived. The *method* transfers (offset scoring against `GNames`
  index validity across sampled objects), and that code is already written.
- **The view-rotation source.** `FUN_0104e390` is Singularity-specific. Worse, the ME analogue may
  not be a clean native function at all, since `TdPlayerCamera` is a script class — which is
  exactly why the script route should be investigated *before* the breakpoint route.
- **Aim decoupling.** Singularity solved this (aim mode 11) against `RvWeapon`. Mirror's Edge is
  mostly unarmed, so the problem is smaller — but the *hands* matter far more, because Faith has a
  fully animated first-person body that the player will expect to follow the controllers.

## Package decompression — a small known gap

`TdGame.u` is 29 MB, `PackageFlags = 0x02A80009` (`PKG_Compressed` set), 20,592 names / 52,410
exports / 2,246 imports. The name table at header offset `0x6D` does not read as plaintext, so
the tables live inside compressed chunks.

The Singularity `tools/uedecompress` handles *fully-compressed* packages only and fails on this
file (`chunk 0 runs past end of file`) — different compression layout, standard UE3 chunked LZO.

Solved elsewhere: UE Viewer's `decompress`, MirrorsEdgeTweaks' built-in decompressor
(NativeSharpLzo), or extend the existing tool to read the `CompressedChunks` table. Then
[UELib](https://github.com/EliotVU/Unreal-Library) or [unhood](https://github.com/yole/unhood)
decompiles the script.

**Reading `TdPlayerCamera` and `TdPlayerController` is the highest-information-per-hour task in
this whole assessment.** It tells you what the camera system actually is before anything is built
against a guess.

## Prior art — no OpenXR mod exists

| What | Status |
|---|---|
| vorpX | Works, geometry-3D + head tracking. Paid, closed, injector-based. Not a native mod. |
| Vireio Perception + FOV mod (2013) | Oculus Rift DK1-era hack. Long dead. |
| MirrorsEdgeTweaks | Active. FOV persistence, config unlocking, custom UnrealScript package. **Not VR** — but the most relevant technical precedent. |
| RTX Remix fan mod | Active. Confirms the D3D9 pipeline is externally interceptable. |
| A native OpenXR VR mod | **Does not exist.** This would be the first. |

UEVR still does not help — UE4/UE5 only.

## ⚠️ Housekeeping: the GOG install is currently modified

Two anomalies in the GOG copy's `Binaries\` folder, both worth resolving before it is used as a
reference copy:

- **`PhysXCore.dll` is renamed to `PhysXCore.bak`** — present and correct in the Steam copy. This
  is a known community workaround (forcing the system PhysX), but it means this install is not
  pristine.
- **`steam_api.dll` is present in a GOG install**, along with `PhysXLoader.dll`, which Steam's copy
  lacks.

Since the `.text` sections are byte-identical, **either copy works for static analysis**. Pick one
install to be the disposable dev copy, as `ENVIRONMENT.md` did last time.

## Recommended sequence

The risk profile has inverted relative to Singularity. There, the graphics pipe was the unknown and
had to go first. Here it is solved, and the unknowns are engine offsets and camera authority.

1. **Port the pipe.** Fork `spikes/view_matrix/d3d9.cpp`, strip the ~30 `Rv`-specific lines, get a
   frame into the headset on Mirror's Edge. This is the cheapest possible proof that the
   architecture holds, and it should take days rather than weeks.
2. **Decompress `TdGame.u` and read `TdPlayerCamera` / `TdPlayerController`.** Independent of
   step 1, and it is what decides step 5. Do it early.
3. **Re-derive the object model for UE3 536** — `GNames`, `GObjects`, `UObject`/`UProperty`
   layouts — using the existing scoring probe.
4. **Locate the view matrix.** Very likely the same `SetVertexShaderConstantF` `c0` pattern; the
   existing self-validating scan reports ROW/COL and translated-world without being told.
5. **Choose the camera-control route** — binary detour (Singularity's) vs custom UnrealScript
   package (Mirror's Edge-specific). Step 2 answers this; do not guess it in advance.
6. **The camera-animation comfort policy.** The longest pole, and the one with no reference
   implementation. Start with the free experiment: sweep `DefaultAnimation.ini`'s spring and
   limiter values and see how much camera motion is suppressible without touching code.

Steps 1–3 are independent and can proceed in any order. None require the camera problem to be
solved first.

## Ground rules

Carried over unchanged from the Singularity project, and still correct:

- Never commit game-derived content — no decompiled UnrealScript, no extracted assets, no RenderDoc
  captures. Findings get summarised in notes, not pasted.
- No code from UEVR (all rights reserved). Concepts only. REFramework is MIT and may be adapted
  with attribution.
- 32-bit (Win32) only. The game is x86; a 64-bit DLL cannot be injected into it.
- Engine addresses and signatures live in exactly one per-game file, each documented with how it
  was derived.
- Signature-scan rather than hardcode addresses — for patch and build robustness, not because
  Steam/GOG parity is in doubt (it is proven byte-identical here).

## Realistic scope

**Faster to a wearable image than Singularity was, slower to something genuinely playable.**

Singularity took ~180 runs across many sessions to reach a complete ladder, and most of that cost
was in the two places this project inherits for free: the D3D9→OpenXR pipe and the stereo/projection
machinery. Expect to reach head-tracked stereo in a small fraction of that.

Then expect the camera-animation problem to consume more of the schedule than everything else
combined, and to end in a judgement call about how much of Mirror's Edge is comfortable in a
headset at all — rather than in a clean technical solution.
