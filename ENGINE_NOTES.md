# Engine notes — Mirror's Edge (UE3 536, x86, January 2009)

Reverse-engineering knowledge base. Findings are summarised; no game code is reproduced.

Addresses, when they appear, are **file/preferred** addresses with image base `0x00400000`.
The executable has **no ASLR** (`DYNAMIC_BASE` clear), so those map directly to runtime
addresses — convenient for debugging, but the mod still resolves by signature scan so it
survives patches and build variance.

Static analysis may target **either** the Steam or the GOG binary: their `.text` and `.rdata`
are byte-identical and plaintext. Only *running* differs — see `FEASIBILITY.md`.

---

## Renderer

### ✅ Direct3D 9 confirmed live — rung 0, 2026-08-04

Established by a pass-through `d3d9.dll` proxy that forwards every export and changes nothing.
This was a real question rather than a formality: the executable delay-loads `d3d10.dll` and
`dxgi.dll`, so it has a DX10 path, and a config value is not evidence that a call happens.

| Fact | Value |
|---|---|
| Entry point | **`Direct3DCreate9`** (non-Ex) |
| `SDKVersion` | 32 (`D3D_SDK_VERSION`) |
| Real library | `system32\d3d9.dll`, i.e. the SysWOW64 32-bit one under WOW64 |
| Config | `TdEngine.ini` → `AllowD3D10=False`, `Fullscreen=True`, `ResX=1600 ResY=1200` |

Non-Ex matters: shared-surface interop needs a **D3D9Ex** device, so the device will have to be
upgraded exactly as the Singularity mod does. `Direct3DCreate9Ex` was logged too and never fired.

### ⚠️ `IDirect3D9` is created TWICE per run, and the pointer is not an identity

Both runs called `Direct3DCreate9` twice. The returned pointers differed *between* runs in a way
that matters:

| run | call #1 | call #2 | |
|---|---|---|---|
| 21:39:00 | `028C1120` | `028C1120` | **same address reused** |
| 21:40:07 | `0286DFE0` | `028D5540` | distinct addresses |

Same address twice means the first object was **released before the second was created** and the
allocator handed back the same slot. So two consequences, and the second is the trap:

- **Do not assume a singleton.** Whatever wraps `IDirect3D9` must tolerate two live instances.
- **Do not key anything on the pointer value.** A wrapper map, a cache, or a "have I already
  seen this object" test would merge two genuinely distinct objects in the first run's pattern.
  Identity must come from the wrapper instance, not the address it happens to occupy.

### ✅ It is the SECOND instance that receives `CreateDevice` — settled 2026-08-04 (rung 2 run)

Rung 1 could not settle this: both calls returned `0278D960` and `CreateDevice` arrived on
`0278D960`, so the address identified nothing. A later run happened not to alias:

```
Direct3DCreate9 (call #1) -> IDirect3D9* 02717260
Direct3DCreate9 (call #2) -> IDirect3D9* 07539EC0
CreateDevice on IDirect3D9* 07539EC0        <- the second one
```

So the first instance is a throwaway — consistent with the standing guess of adapter and
display-mode enumeration for the video options — and the second is the real one.

**Do not build on this without keeping the aliasing in mind.** The answer arrived from a run
where the allocator did not reuse the slot; in a run where it does, no address-based test can
tell the two apart. Nothing currently depends on the distinction, because vtable patching
covers every instance regardless.

Method note worth keeping: *an instrument that reads an address cannot distinguish objects
that share one.* The trap was written down before the test was designed, and the test was
still built around reading an address. Repeating the run under different allocator conditions
is what answered it — not a better instrument.

### ✅ Device parameters — measured, rung 1, 2026-08-04

6,832 frames across real gameplay plus two resolution changes.

| Property | Value |
|---|---|
| **Backbuffer** | **2560×1440, `D3DFMT_A8R8G8B8` (21)** — read from the *surface descriptor* |
| **MSAA** | **none** (`MultiSampleType=0, quality=0`) — **no resolve step needed before sharing** |
| `BehaviorFlags` | **`0x00000142`** = `HARDWARE_VERTEXPROCESSING \| FPU_PRESERVE \| MULTITHREADED` |
| Windowed | `0` — fullscreen. The mod will need windowed. |
| `SwapEffect` | `1` (`DISCARD`) |
| `PresentationInterval` | `0x80000000` (`IMMEDIATE`) — vsync off, matching `UseVsync=False` |
| `EnableAutoDepthStencil` | `0` — the engine owns its own depth buffer |
| `Flags` | **`0x00000001`** = `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` |
| Device type | `1` (`HAL`), adapter 0 |

The format was read from `GetBackBuffer`→`GetDesc`, not from `D3DPRESENT_PARAMETERS`. Here they
happen to agree, but in windowed mode `BackBufferFormat` is permitted to be `D3DFMT_UNKNOWN`, so
the request is not a reliable source — and the Singularity project's `D3DERR_INVALIDCALL` came
from a note recording `X8R8G8B8` where the answer was `A8R8G8B8`.

**`LOCKABLE_BACKBUFFER` is set**, which is unusual and potentially useful: the backbuffer can be
locked directly. Whether that is a cheaper frame-grab route than `GetRenderTargetData` is
untested and should be measured, not assumed — a lockable backbuffer also carries a known
performance cost on some drivers.

### Resource pools — measured over a full session

```
frame  600   DEFAULT=83     MANAGED=776
frame 3600   DEFAULT=255    MANAGED=10654
frame 4800   DEFAULT=5057   MANAGED=10871
final        DEFAULT=5160   MANAGED=11084   SYSTEMMEM=0   other=0
```

Against the Singularity mod, which had to translate `MANAGED`→`DEFAULT` because **D3D9Ex does
not support `D3DPOOL_MANAGED` at all**:

| | Singularity | Mirror's Edge |
|---|---|---|
| Backbuffer format | `A8R8G8B8` | `A8R8G8B8` — same |
| MSAA | none | none — same |
| `BehaviorFlags` | `0x142` | `0x142` — identical |
| `MANAGED` allocations | 10,454 | **11,084** — comparable |
| `DEFAULT` allocations | 47 | **5,160** — two orders of magnitude more |

The `MANAGED` count is what sizes the translation problem, and it is close enough that the
Singularity texture wrapper should transfer at similar cost. The `DEFAULT` count differing so
sharply is unexplained; the two figures may not be measured over comparable sessions, so it is
noted rather than concluded.

### Device `Reset` — clean, and survivable

Two resolution changes, both `hr=0`, backbuffer re-measured correctly each time
(2560×1440 → 1920×1200 → 2560×1440). `DEFAULT`-pool allocations rose across each reset as
resources were recreated. Reset is a path the VR frame plumbing must survive, and it is now
known to be exercised by an ordinary video-options change rather than only by alt-tab.

---

## OpenXR

### ✅ 32-bit OpenXR works in this process — rung 2, 2026-08-04

| Fact | Value |
|---|---|
| Runtime | **VirtualDesktopXR** (`virtualdesktop-openxr-32.json`) |
| API version | **1.0 only.** VDXR rejects a 1.1 instance with `-4`, so 1.0.34 is tried first |
| Recommended per eye | **2496×2688** (max 16384×16384) — identical to the Singularity figure |
| Side-by-side stereo would want | `-ResX=4992 -ResY=2688` |
| Session states reached | IDLE → READY → SYNCHRONIZED → VISIBLE → **FOCUSED** |
| Swapchain format chosen | `91` = `DXGI_FORMAT_B8G8R8A8_UNORM_SRGB` |

The 32-bit runtime situation is unchanged from the Singularity project: SteamVR ships no
32-bit runtime at all, and Meta's own crashes in `xrCreateSession`. **VDXR is the only usable
one, so Virtual Desktop must be streaming before the game is launched.**

### ⚠️ Swapchain textures are TYPELESS — name the view format explicitly

```
[xr] swapchain texture: 1024x1024 DXGI format=90 bind=0x000000A8  (requested 91)
```

`90` is `DXGI_FORMAT_B8G8R8A8_TYPELESS`; `91` is what was requested. The runtime allocates
typeless so the image can be viewed as either sRGB or UNORM.

**Consequence:** `CreateRenderTargetView(tex, nullptr, &rtv)` **fails**, because a null
description means "use the resource's own format" and a typeless format cannot be a view
format. The RTV description must name the format explicitly.

This failed in a way worth remembering: the session was FOCUSED, `shouldRender` was 1, and
`xrEndFrame` returned success for 3,000 consecutive frames. **A quad was submitted every
frame and was simply empty.** Every indicator except the headset itself said the pipeline
was healthy.

### ✅ The CPU frame path costs ~4.4–5.0 ms at 2560×1440 — rung 3, 2026-08-04

`backbuffer → GetRenderTargetData → SYSTEMMEM → lock → D3D11 dynamic texture → CopyResource`.
Measured as a mean over each 600-frame window, during gameplay:

```
frame  600   4.94 ms      frame 1800   4.45 ms
frame 1200   5.04 ms      frame 2400   4.36 ms
```

**This number cross-validates against the reference.** Singularity measured ~9.8 ms on the same
path at 4K. 3840×2160 is 8.29 MP against 2560×1440's 3.69 MP — a ratio of 2.25 — and
9.8 / 2.25 = 4.36 ms. The measurement lands on the prediction, so it is the real cost of the
path rather than an artefact of how it is timed here.

What it means for the D3D9Ex wrapper, which is the decision this number exists to inform:

| target | budget | frame grab | share |
|---|---|---|---|
| 2560×1440 @ 90 Hz | 11.1 ms | 4.4 ms | 40% |
| 2560×1440 @ 120 Hz | 8.33 ms | 4.4 ms | 53% |
| **4992×2688 @ 90 Hz** (full parity stereo, 13.4 MP) | 11.1 ms | **~16 ms projected** | **untenable** |

So the CPU path is **usable for the next several rungs and cannot survive full-resolution
stereo**. Deferring the wrapper past stereo verification remains right; taking it on before
that would have been premature. Zero-copy took the reference's frame copy to **0.00 ms**.

The `LOCKABLE_BACKBUFFER` flag noted above is still an unmeasured third option that might
avoid the wrapper entirely. Worth timing before committing to D3D9Ex.

### ⚠️ `DLL_PROCESS_DETACH` logging is unreliable, and it is an instrument gap only

The rung 1 run wrote `=== SUMMARY ===` and `=== detached ===`. Every run since — all of them
with OpenXR active — ends at its last in-frame line with no detach output, including the
rung 3 run that exited cleanly via the `WM_CLOSE` path.

The exit itself is fine and independently confirmed: `TdEngine.ini` was rewritten at the
moment of quit, which is the engine saving on its own shutdown path. Only our summary is
missing. Likely a teardown route that skips `DllMain`, or the detach write racing process
shutdown; not diagnosed, and low stakes because every figure in the summary is also reported
in the running per-600-frame lines.

**Do not treat a missing summary as evidence of an unclean exit.** Check whether the config
file was rewritten instead.

---

## The object model

### ✅ `GNames` — located and validated, 2026-08-05

```
GNames TArray @ .data 0x0204E7D8    Data=0x0D8C0000  Count=38211  Max=49641
GNames[0..4] = None, ByteProperty, IntProperty, BoolProperty, FloatProperty
```

Found by walking `.data` for the `{Data, Count, Max}` shape and validating each candidate's
first three entries against `None` / `ByteProperty` / `IntProperty` — the runtime array always
begins that way because the engine interns its own type names first. Three exact strings in
order is not something random data produces.

Only the **TArray address is stable**. `Data` is a heap allocation and moves between runs;
always read it at runtime.

### ⭐ `FNameEntry` — UTF-16, and different from the Singularity build

```
03D6FFC0:  01 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
03D6FFD0:  42 00 79 00  74 00 65 00  50 00 72 00  6F 00 70 00   B.y.t.e.P.r.o.p.
03D6FFE0:  65 00 72 00  74 00 79 00  00 00 ...                  e.r.t.y...
```

| Offset | Field |
|---|---|
| `+0x00` | **`Index`** — plain, not shifted. Reads `1` for `ByteProperty`, which is slot 1. |
| `+0x04`–`+0x0F` | zero in this sample; hash link and/or flags, not yet identified |
| **`+0x10`** | **the text, UTF-16** |

Against Singularity (UE3 584), which recorded `Index << 1` at `+0x08` with bit 0 as an
ANSI/wide flag, and **ANSI** text at `+0x10`:

| | Singularity 584 | Mirror's Edge 536 |
|---|---|---|
| Index | `+0x08`, shifted left 1 | **`+0x00`, plain** |
| Encoding | ANSI (flagged) | **UTF-16, unconditionally** |
| Text offset | `+0x10` | `+0x10` |

> ### ⚠️ The encoding cost three runs, and the reason is worth keeping
>
> The first scan tested ANSI only. The second tested UTF-16 **but only if the ANSI search
> returned zero hits** — and it never did, because package name tables held in memory and this
> DLL's own string literals always supplied some. A conditional fallback that is gated on the
> wrong signal is worse than no fallback: it looks like coverage.
>
> Two invented constraints were also rejecting correct answers while presented as validity
> tests:
>
> - **`Max <= Count*4 + 1024`.** A `TArray`'s `Max` is wherever the last growth left it. Made
>   up. (`GNames` reports `Max=49641` against `Count=38211`, so it happened not to bite — which
>   is exactly why it survived.)
> - **A 24-hit cap per search pattern.** `"None"` reported `x24 (capped)` in a run whose real
>   entry was past the cut, so the triangulation could not have succeeded **even with entirely
>   correct data**.
>
> Method, earned: *when a scan fails, make the failure carry evidence.* The hex dump around
> each hit is what eventually showed `01 00 00 00` twelve bytes before a UTF-16
> `"ByteProperty"` — index 1, its own slot — and that one detail ended the search. Every run
> that only said "not found" cost a launch and taught nothing.

### ✅ `GObjects` — located, 2026-08-05

```
GObjects TArray @ .data 0x0204A344   Data=0x460F0000  Count=114081  Max=123943
64/64 sampled objects have a vtable inside the module image
```

114,081 objects, against Singularity's 112,565 — the right magnitude for a UE3 title.
`GObjects − GNames` is **−0x4494**, so the "adjacent globals" adjacency that held in Singularity
(`+0x30`) does **not** transfer. Both must be found independently.

> **⚠️ Vtables are in `.rdata`, not `.text`.** MSVC emits them there, and in this image `.rdata`
> sits *above* `.text`:
> ```
> .text   00401000-01A8B000
> .rdata  ~01A8C000-01F54000    <- vtables
> .data    01F53000-02095000
> ```
> A scan testing `.text` only rejects every real object. The Singularity notes describe their
> first entry's vtable as "inside `.text`", which was taken as a fact about this binary and is
> not one. **Test the whole module image.**

> **⚠️ A false positive got through first, and the reason is instructive.** An earlier version
> reported `Data=0x01F9C9BC Count=5100 Max=0xFFFFFFFF` — a static `.data` structure whose bytes
> fit the `TArray` shape. It passed because an invented `Max` constraint had been removed and
> **nothing put in its place**. Rejecting a made-up rule is not a reason to stop sanity-checking
> a field. Real constraints: `Max` a plausible allocation count, and `Data` **outside** the
> module image, because it is a heap allocation.

### ✅ `UObject::Name` at `+0x2C` — same as Singularity, despite `FNameEntry` differing

```
+0x2C sample names: TextBufferFactory Factory Object TextBuffer System Subsystem
                    StructProperty Property Field StrProperty
```

That is UE3's bootstrap class registration order, which is what confirms it — the same
sequence the Singularity notes record.

**Verified end to end**: walking all 113,310 live objects and matching names finds every class
the project needs.

```
walked 113310 live objects of 113310 slots
  TdPlayerPawn        FOUND (2)     TdPlayerController  FOUND (2)
  TdPlayerCamera      FOUND (2)     TdSwanNeck          FOUND (2)
  TdGameInfo          FOUND (1)
```

Two hits per class is expected — the `UClass` and its instance share a name. Telling them
apart needs `Outer` and `Class`, which are not yet derived; the Singularity rule was that a
live actor has `Outer == PersistentLevel` and a name without the `Default__` prefix.

The scoring table also flags three other varying offsets worth identifying later: `+0x04`
(132/400, 64 distinct), `+0x24` (330/400, 64 distinct) and `+0x50` (135/400, 55 distinct).

> **⚠️ Hit rate alone cannot find this offset, and it took a wasted run to see why.** In the
> first scoring pass, `+0x18`, `+0x1C`, `+0x2C`, `+0x30`, `+0x40`, `+0x44` and `+0x48` all
> scored **400/400** and the detector correctly refused to choose.
>
> Those other offsets hold **zero** for every object — null pointers and unset `FName`s — and
> index 0 decodes as `"None"`. A constant field scores a *perfect* hit rate. Worse, any small
> integer below the name count indexes some valid entry, and real structures are full of small
> integers.
>
> **The discriminator is DISTINCTNESS.** The `Name` field varies across objects; a null field
> does not. Scoring now requires both a high hit rate and many distinct values, and prints the
> actual names each leading offset produces — a human separates class names from an assortment
> of unrelated words instantly, and no statistic available here does that as reliably.

### Cost

The memory-wide search was **20.4 s** across 1.5 GB and stalled level loading when it ran on
the render thread. Once the encoding was known it was unnecessary: the array points at the
entries, so a `.data` walk finds them in **1.4 s**. The wide search is retained only as a
fallback, because it is what produced the evidence when the fast path had nothing to say.

Run it off the render thread regardless — it is read-only, and nothing should wait on it.

---

## Save data

Stored **outside the install**, keyed by the game's identity, so every copy — Steam, GOG, and the
dev clone — reads and writes the same file:

```
Documents\EA Games\Mirror's Edge\TdGame\Savefiles\<user>.dat     (~9 MB)
Documents\EA Games\Mirror's Edge\TdGame\Config\TdEngine.ini
Documents\EA Games\Mirror's Edge\TdGame\Config\TdInput.ini
```

Same hazard the Singularity project recorded: **re-take the backup before any run that installs
a hook which changes engine behaviour.** Observation-only shims like rung 0 do not touch game
state, but a crash inside a write-capable hook could damage the one save file every copy shares.
Backups live in the gitignored `save_backups/`.

---

## Script — ✅ opened and read, 2026-08-04

`TdGame.u` is 29 MB, `CompressionFlags = 2` (`COMPRESS_LZO`), 55 chunks of 128 KB blocks,
20,592 names / 52,410 exports / 2,246 imports. Expanded to 58.9 MB by `tools/uedecompress3`,
then decompiled with **UELib 1.13.7** (NuGet `Eliot.UELib`), which has an explicit
`Build:MirrorsEdge` profile. 54,656 objects, 1,341 classes.

> **Tooling note:** a decompressed package must have its compression markers cleared *and* the
> fields that followed the chunk table moved up into its place. Zeroing `CompressionFlags` and
> the chunk count alone leaves 880 stale bytes, so `PackageSource` and everything after it is
> misaligned — UELib read `PackageSource` as `109` and then died with an
> `OutOfMemoryException` on an array length taken from garbage. `uedecompress3` relocates the
> trailer; the check is that `PackageSource` reads `1972846046`.
>
> UELib emits `__NFUN_nnn__` placeholders for native operators in this build, so structure is
> readable but arithmetic is not. Meanings below are **inferred from context**, not verified:
> `161` `+=`, `143` unary minus, `119` `!= None`, `129` `!`, `130` `&&`, `132` `||`.

### ⭐ `TdPlayerCamera` is a thin dispatcher and is NOT where the camera is built

158 lines, two functions. `UpdateViewTarget` selects between camera styles — `Fixed`,
`FreeFlight`, `FixedPerson`, `ThirdPerson`, `ThirdPerson360`, `FreeCam`, `FirstPerson` — but
**the entire switch is skipped in normal gameplay**, because it is guarded on
`Pawn(OutVT.Target).CalcCamera(...)` returning false, and the pawn's returns
`IsFirstPerson()`.

So the class named after the camera does nothing during play. Its `FirstPerson`/default branch
merely calls `GetActorEyesViewPoint`. Worth knowing anyway: `FreeFlight` is a complete
mouse-and-stick free camera already in the shipped build, and `FreeflightScale` is the
`DefaultCamera.ini` value.

### ⭐⭐ `TdPlayerPawn.CalcCamera` IS the first-person camera

This is the single most important finding so far, and it relocates the project's biggest risk
from "somewhere in an animation system" to one function. Composition order:

| step | source |
|---|---|
| **Position** | `Mesh1p.GetBoneLocation('EyeJoint')` — the camera rides a **bone on the animated first-person mesh** |
| **Rotation** | `GetViewRotation()` — the player's own look input |
| **+ camera animation** | `GetCameraAnimation()`, added with an **axis swizzle** (below) |
| **+ swan neck** | `SwanNeck1p.GetSwanNeckPos()`, a positional offset derived from yaw only |
| **collision** | `Moves[MovementState].CheckForCameraCollision()`, skipped when `bCinematicMode` |
| **cached** | into `PlayerCameraLocation` / `PlayerCameraRotation` on the pawn |
| **FOV** | `TdPlayerController.FOVAngle` |

The swizzle is not a typo — animation axes are remapped into view axes:

```
view.Pitch += -anim.Roll        view.Yaw += anim.Pitch        view.Roll += -anim.Yaw
```

### What this means for VR

**Good, and better than feared.** The camera-animation contribution — the wall-run roll,
landing dips and bob that made this the project's headline risk — is **not diffuse**. It
arrives through *one* call and *three* add-assignments. Neutralising or attenuating it is a
local change, not a fight with an animation system.

Consequences to design around, in order of awkwardness:

1. **Position rides an animated bone.** 6-DOF head tracking has to add to a moving anchor, and
   the bone's own motion *is* the bob. This is the harder half of the comfort problem — head
   rotation can be made authoritative easily, position cannot simply be overridden without
   detaching the view from Faith's body.
2. **`GetViewRotation()` is the natural injection point** for head orientation, and it composes
   with the animation term rather than replacing it.
3. **`SwanNeck1p` is separable** and can be neutralised independently of the animation term.
4. **FOV has exactly one source**, so the projection work has a single lever.
5. `bCinematicMode` already gates camera collision — a ready-made signal for cutscene handling,
   which the Singularity project had to discover the hard way.

### ⭐ Every channel that moves the view without the player asking — enumerated

| # | channel | where | script? | notes |
|---|---|---|---|---|
| 1 | `GetCameraAnimation` | `TdPawn` | ❌ **`native final`, no body** | the bob and roll. Opaque. |
| 2 | `Moves[MovementState].UpdateViewRotation` | `TdMove` + ~148 subclasses | ✅ | forced look-at, and per-move view clamping |
| 3 | `CurrentForcedLookAtPoint` / `CurrentLookAtPoint` | `TdPlayerController` | ✅ | scripted view pulls |
| 4 | `ViewShake(DeltaTime)` | `TdPlayerController` | ✅ | shakes |
| 5 | `TdSwanNeck` | script maths, native getter | ✅ **and ini-configurable** | pitch-driven lean |
| 6 | `bCinematicMode` | `TdPlayerController.UpdateRotation` | ✅ | forces the view to `Pawn.Rotation` |

**⚠️ `GetCameraAnimation` is `native final` with no script body.** This is the one real limit
the script route hit: the *source* of the bob and roll is C++ and cannot be read here. It does
**not** block the work — `CalcCamera` shows the result arriving as a delta at exactly one site,
so it can be scaled or dropped without knowing how it was computed — but understanding *why* a
particular motion happens would need the binary.

**Roll is contributed only by channel 1.** `TdPlayerController.UpdateRotation` sets
`ViewRotation.Roll = 0` before writing it back, so the roll seen during a wall-run cannot come
from the controller. That is a clean separation and a cheap thing to verify.

### `TdSwanNeck` — a pitch-driven lean, and free to disable

Fully readable, and driven **only by controller pitch** — it is not a bob. Look down past
`StartTranslateAtDegree` and the camera translates forward and down, modelling leaning over to
look at your feet. In a headset the player's real neck already does this, so the engine doing
it too is likely to feel wrong.

It is `config(Game)`, and `DefaultGame.ini` already carries the section:

```ini
[TdGame.TdSwanNeck]
StartTranslateAtDegree = 15
QuadraticForwardTranslation = 35
QuadraticDownwardTranslation = 30
Type = ESNT_Quadratic
```

> ### ⛔ RETRACTED: editing the ini is NOT free — the game hash-checks its config
>
> This section used to claim that zeroing the translations disables the swan neck "with no
> code at all", and called it the cheapest comfort experiment available. **That is wrong**, and
> it cost two runs to find out.
>
> **Measured 2026-08-05.** With a byte-faithful edit to `DefaultGame.ini` — only the digits
> changed, CRLF count identical, no BOM — the game **refuses to start**. The mod log shows it
> loading, calling `Direct3DCreate9` twice, and then exiting *before* `CreateDevice`. Not a
> crash: a deliberate early exit during startup. Restoring the pristine file byte-for-byte and
> the game starts normally, which is the control that pins it.
>
> The warning in every `Default*.ini` header — *"Don't modify this file. If you do, your game
> may not start"* — is **literal**. Community sources agree: Mirror's Edge hash-checks its
> config files, and the known workarounds are MirrorsEdgeTweaks' "Allow config mods" patch or
> MEMLA.
>
> **Two runs were also wasted on my own edit faults before the real cause was visible**, and
> both were invisible without a byte-level comparison:
> - `Set-Content -Encoding utf8` wrote a **UTF-8 BOM**, so the first attempt tested "does a BOM
>   break the parser".
> - `(?m)...\s*$` consumed the `\r`, silently converting five lines from CRLF to LF.
>
> **Compare the bytes, not the text, after editing a file the game validates.** The check is
> `pristine size + expected delta`, and an unchanged CRLF count.
>
> ### The route that avoids the problem entirely
>
> The hash is over the **file**. It says nothing about the values in memory. Since this mod
> will have `GObjects`/`UProperty` access anyway, the swan neck can be neutralised by **writing
> `TdSwanNeck`'s properties at runtime** — no file touched, no hash to defeat, no dependency on
> an external patcher. Same applies to every other `config` value worth tuning.
>
> That makes the object-model work a prerequisite for *all* ini-level tuning, not just for
> camera control. Worth knowing before planning around "just change the ini".

### ⚠️ `CameraAnimationEnabled=false` in `DefaultGame.ini` is a dead line

It appears under `[Engine.UIDataStore_GameResource]`, which is the wrong section for it, and
the name has **zero occurrences** in `TdGame.u`. It is either an `Engine.u` property or
vestigial. **Do not treat it as a working toggle** — it looked like a free win and is not one.

### `TdPlayerController.UpdateRotation` — the other half of the picture

Order of operations: take the controller's own `Rotation`; compute a speed modifier from the
pitch difference against the pawn; apply stick delta *only* when
`bRightThumbStickPassedDeadZone`; then either force the view to `Pawn.Rotation` when
`bCinematicMode`, or run the look-at points, the per-move `UpdateViewRotation`,
`ProcessViewRotation`, zero the roll, and write it back. `ViewShake` runs last.

`bCinematicMode` forcing the view to the pawn is the cutscene head-lock. The Singularity
project found the same behaviour by experiment and solved it at run 138; here it is visible in
the source before any run.

**Still not read:** `GetViewRotation` (inherited from `Engine.u`, a different package that also
needs decompressing), `ProcessViewRotation`, and the individual `TdMove_*` overrides —
`TdMove_WallRun` is dumped and unread.

### Consequence for the architecture choice

The first-person camera is **UnrealScript, not native**, so both routes are live:

- **Binary detour** — hook the native `ProcessEvent`/script VM, or intercept the view matrix as
  the reference does, and correct downstream.
- **Custom UnrealScript package** — subclass or replace `TdPlayerPawn.CalcCamera`, which is
  proven possible in this game by MirrorsEdgeTweaks.

The second is now clearly worth pricing, because the whole camera is 40 readable lines.
