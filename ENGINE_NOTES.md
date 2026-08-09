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

### ✅ The full `UObject` header — identical to Singularity's, and derived not assumed

Found by scoring which dwords point at other `UObject`s, then logging what they point *at*:

```
+0x28  400/400 -> Core Core Core Core Core Core            Outer
+0x34  400/400 -> Class Class Class Class Class Class      Class
+0x38  332/400 -> Default__Class Default__Class ...        ObjectArchetype
+0x3C  399/400 -> Factory Object Object Subsystem ...      SuperField (UStruct)
```

| Offset | Field |
|---|---|
| `+0x00` | vtable (in `.rdata`) |
| `+0x28` | `Outer` |
| `+0x2C` | `Name` (FName index) |
| `+0x30` | `Name.Number` — always 0 in samples |
| `+0x34` | `Class` |
| `+0x38` | `ObjectArchetype` |
| `+0x3C` | `SuperField` (on `UStruct`/`UClass`) |

**The same layout as UE3 584**, even though `FNameEntry` differs in both index placement and
encoding. That is a useful asymmetry to remember: the object header transferred, the name entry
did not, and there was no way to know which without deriving both.

**Distinguishing an instance from its `UClass`**: the class object's `Class` (`+0x34`) points at
the object named `Class`; an instance's points at its own `UClass`. Cheaper and more robust
than the Singularity rule of `Outer == PersistentLevel` plus a `Default__` prefix check.

> **⚠️ The selection was unstable before it validated itself.** Two consecutive runs of the same
> build picked different arrays — `0x0204A344` (Count 113310, 64/64) then `0x01FFA644`
> (Count 57020, 58/64) — because the scan took the *first* candidate passing a threshold while
> walking `.data` in address order, and the loser's contents vary between runs.
>
> Fixed by making `GObjects` and the layout confirm **each other**: candidates are collected,
> ordered, and the winner is the one for which a `Name` offset exists that decodes for nearly
> every object *and varies across them*. Neither fact is now assumed in order to check the
> other.

### ✅ `TdSwanNeck` instance layout — found by its default VALUES

Located without walking a single `UProperty` chain, by searching each object named
`TdSwanNeck` for its shipped defaults. 7/7 present identifies the live instance; the `UClass`
scored 0/7, as it must.

| Offset | Property | Default |
|---|---|---|
| `+0x3C` | `WantedTranslation` (FVector) | — |
| `+0x48` | `Translation` (FVector) | — |
| `+0x54` | `PreviousTranslation` (FVector) | — |
| **`+0x60`** | **`LinearForwardTranslation`** | 25.0 |
| **`+0x64`** | **`LinearDownwardTranslation`** | 25.0 |
| **`+0x68`** | **`QuadraticForwardTranslation`** | 35.0 |
| **`+0x6C`** | **`QuadraticDownwardTranslation`** | 30.0 |
| `+0x70` | `StartTranslateAtDegree` | 15.0 |
| `+0x74` | `ForwardPitchWorld` (int) | 65536 |
| `+0x78` | `DownwardPitchWorld` (int) | 48151 |
| `+0x7C` | `DegToUnDeg` | 182.0440063 |
| `+0x80` | `Type` | `ESNT_Quadratic` |

**The offsets reproduce the declaration order in the decompiled class exactly** — three
`FVector`s at 12 bytes each from `+0x3C`, then the config floats, then the consts. Two
independent methods agreeing on the same layout is stronger evidence than either alone, and it
also confirms the decompilation is faithful.

⚠️ `+0x60` and `+0x64` both hold 25.0, so the probe cannot tell `LinearForward` from
`LinearDownward` by value. Order is taken from the declaration, not measured. It does not
matter for zeroing both, but do not cite it as measured.

### ✅✅ CONFIRMED IN THE GAME, 2026-08-05 — runtime property writes work

Writing `+0x60`/`+0x64`/`+0x68`/`+0x6C` at runtime visibly changes the camera. Verified by an
**exaggerated** value rather than by zeroing: at 8× (linear 200/200, quadratic 280/240) the
lean on looking down is unmistakable, which zeroing could never have established.

```
[swan] live instance at 384D94C0 (search took 1474.6 ms over 114701 slots)
[swan] original values: linear 25.0/25.0  quadratic 35.0/30.0
*** [swan] EXAGGERATED 8x ... (linear 200.0/200.0 quadratic 280.0/240.0)
```

**The general result is much larger than the swan neck.** Any UnrealScript property in this
game can now be read and written at runtime, located by class name, with **no file modified, no
hash to defeat and no external patcher**. Everything the config check closed is reopened:
`DefaultAnimation.ini`'s spring and limiter values, `TdPlayerController.FOVAngle`, and
eventually the camera-animation attenuation that is the project's longest pole.

Whether *zeroing* the swan neck is desirable for VR is a separate question and needs a headset.
The mechanism is proven; the comfort judgement is not made.

> **⚠️ Two costs recorded rather than glossed.**
>
> **A failed object search must arm a backoff.** The search walks ~115,000 objects with several
> `ReadProcessMemory` calls each. Cached it is three reads a frame and free; uncached it is
> hundreds of thousands of syscalls **per frame** on the render thread. With no instance present
> the framerate collapsed *and* the hotkey appeared dead — one cause, two symptoms that look
> unrelated. The Singularity project measured the identical failure at 69–82 ms/frame in its
> run 35 and wrote down the fix; it was read during this port and not applied.
>
> **The successful search still costs 1474 ms**, one-off, on the render thread. Cached
> afterwards, but a 1.5-second hitch. It should move to the background thread that already
> walks every object, or be spread across frames.

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

## ✅ Head tracking — working, 2026-08-05

```
TdPlayerController::Rotation        +0x00F4    the write target
TdPlayerController::Location        +0x00E8
TdPlayerController::FOVAngle        +0x030C
TdPlayerPawn::PlayerCameraRotation  +0x09B8
TdPlayerPawn::SwanNeck1p            +0x04F0
```

Found by walking `UProperty` chains, not by guessing. `Location` is an `FVector` ending exactly
where `Rotation` begins, and `PlayerCameraRotation` reproduces the number a bulk dump reported
by a different path.

### ⭐ The Singularity premise does NOT transfer — no detour is needed here

That project could not steer the view by writing the controller's rotation: a native source
recomputed it every frame, and after seven failed attempts a hardware write breakpoint and a
detour on `FUN_0104e390` were required.

**In Mirror's Edge a plain write to `TdPlayerController::Rotation` is consumed by the engine.**
The decompiled `UpdateRotation` opens with `ViewRotation = Rotation`, so the field is the base
the engine reads each frame rather than an output it overwrites. Confirmed in play: writes land
and the view follows.

This is the single largest labour saving found so far, and it was only visible because the
script was read first. Reading the source turned "seven failed guesses and a breakpoint
session" into one function's opening line.

### Sign convention — measured, not derived

Both yaw and pitch are **inverted** relative to a naive OpenXR→UE3 conversion. OpenXR is
right-handed, Y up, −Z forward; UE3 is left-handed, Z up. Rather than argue it out, the first
build shipped `+1/+1` with F5 and F4 to flip them at runtime, and both came back wrong. The
defaults are now `−1/−1`.

### Design points worth keeping

- **A delta is folded in, not an absolute pose.** An absolute write discards the engine's own
  input delta each frame; adding only the change since the last frame lets head tracking and
  mouse compose. Same conclusion the reference project reached.
- **⚠️ The delta goes through an `int16` cast.** UE3 uses 65536 units per turn, so a raw
  `int32` subtraction of two headings 0.3° apart reads as **360.3°**. That defect cost the
  Singularity project months, with two sites carrying comments asserting the wrap was correct.
  65536 *is* 2¹⁶, so truncation gives the shortest signed path for free.
- **Roll cannot travel this path.** `UpdateRotation` sets `ViewRotation.Roll = 0` before writing
  back. Head roll needs the view matrix.
- The controller search carries the same failed-search backoff as the swan neck, for the reason
  measured there.

---

## ⭐ The view matrix — found, and in a DIFFERENT SPACE from Singularity

```
[vm] c0  ROW  w(cam)=0.000  w(origin)=20669.789  dotFwd=+1.0000  <- world space
```

| Fact | Value |
|---|---|
| Where | `SetVertexShaderConstantF`, device vtable slot **94** |
| Register | **`c0`**, a 4-register block |
| Storage | **ROW** — registers are the rows of a row-vector matrix (`clip = v * M`) |
| Space | **WORLD** — *not* translated-world |
| Residual | `w(cam) = 0.000` exactly, `dotFwd = +1.0000` exactly |

### ⚠️ The space differs, and it inverts which probe is trustworthy

Singularity renders in **translated-world**: UE3 pre-subtracts the view origin on the CPU, so
there the camera sits at the shader-space origin and only an origin probe finds the matrix. Its
notes warn that the origin probe *degenerates* — `clip.w` at `(0,0,0)` collapses to `r[3].w`
under both storage conventions, so it cannot separate ROW from COL and admits any matrix with a
small `w` constant. That project saw 18 spurious candidates.

**Mirror's Edge uploads WORLD-space matrices.** The camera's real world position is what maps to
`w ≈ 0`, and the *origin* probe is now the noise generator — this scan produced ~20 candidates
per frame, all origin hits at `dotFwd` 0.90–0.98 with `|w(cam)|` in the hundreds of thousands.
They are a block of `float3x4` transforms the sliding 4-register window straddles.

**The real matrix is not marginally better, it is exact.** `w` of 0.000 and `dotFwd` of 1.0000
against 0.90–0.98 and enormous `w`. Candidates are therefore ranked on both rather than accepted
on a threshold, and the first match is never taken — the real one was not first in the list.

### What this changes, and what it does not

- **The injection maths is unaffected.** To move the camera by a world offset `o`:
  `row3 -= o.x*row0 + o.y*row1 + o.z*row2`. That is `M' = T(−o)·M` in row-vector form and holds
  in either space; the space only decided which probe point finds the matrix.
- **Per-eye offset, head roll and 6-DOF all come from this one interception**, exactly as in the
  reference — roll in particular has nowhere else to go, since `UpdateRotation` zeroes
  `ViewRotation.Roll` before writing it back.
### ✅✅ Injection CONFIRMED in the game, 2026-08-06

`M' = T(−o)·M`, applied to the covering upload:
`row3 -= o.x*row0 + o.y*row1 + o.z*row2`. Cycling a 300 UU offset through forward / right / up
moves the camera visibly on every change, over 20,000 injections.

```
*** [vm] BEST: c0 ROW in WORLD space  (score 1.0000, 208 candidates seen)
```

**208 candidates, and the winner scored exactly 1.0000.** Ranking rather than first-match was
load-bearing: the real matrix was not first in the list, so taking the first would have injected
into an unrelated transform.

The engine's buffer is `const` and belongs to it, so a covering call is copied, modified and
forwarded. The guard ahead of that is two comparisons, so non-covering calls — the overwhelming
majority — are untouched.

**Everything stereo needs now exists.** Per-eye parallax is this same offset at ±half IPD along
the camera's right axis; head roll and 6-DOF are the same mechanism. The right axis should be
read from the matrix itself rather than recomputed from the pawn's rotation — the matrix cannot
disagree with itself.

Expect a black band at the frame edges for large offsets: the engine culls on the CPU against
its *original* frustum, so newly-visible geometry was never submitted. Singularity measured
1.4–2.9% at 300 UU and **0 px at 100 UU and below**, and a real IPD is a few UU — so it does not
apply to stereo, only to tests like this one.

---

## ✅ Per-eye stereo — working, 2026-08-06

Alternate-eye: one eye rendered per frame, each image one frame stale. Two swapchains, a
projection layer with real per-eye poses from `xrLocateViews`, and the rung 6b injection applied
at ±half IPD along the **matrix's own right axis** — read from the unmodified incoming data in
the same call that modifies it, so it cannot be stale and cannot disagree with what it adjusts.

Chosen over simultaneous stereo deliberately: the engine renders once per frame, so two genuinely
different images need draw-call duplication or engine re-entry, both large and both able to fail
in ways that look like a geometry bug. Alternating proves the offset, the swapchains, the layer
and the FOV maths first — **and costs no extra frame grab**, so the ~4.4 ms copy is unchanged and
the D3D9Ex wrapper stays deferred.

### ⚠️ Submitting the headset's FOV for the game's image is a lie that shows as DOUBLE VISION

The first attempt submitted `g_views[e].fov` — the headset's per-eye frustum — for an image the
game rendered with its own. The compositor places and warps each eye by the frustum it is told,
so two images rendered one way and described another **cannot fuse**. Reported as double vision.

The reference project records the identical mistake with a different symptom: there it read as
everything stretched by one uniform factor, so head movement, world scale and object size were
all wrong together and none could be judged against another.

**Read the FOV out of the matrix instead of assuming it.** For a row-vector world→clip matrix,
column 0 is the right axis scaled by `1/tan(fovX/2)` and column 3 is the unit forward axis:

```
tan(halfFovX) = |col3| / |col0|        tan(halfFovY) = |col3| / |col1|
```

Nothing has to be assumed about UE3's `FOVAngle` convention or how it folds in aspect — the
answer comes out of what actually reached the GPU. **Measured: roughly 84° horizontal**, which is
notably *not* the 65° the config default would have suggested. That gap is the argument for
reading rather than deriving.

### The image is a correct rectangle inside black, and that is the honest state

The game renders 16:9; a per-eye view is nearly square (2496×2688). At ~84×54° against a headset
wanting ~95–100° on both axes, the vertical falls well short and shows as letterboxing.

**Match the headset's VERTICAL FOV, not its horizontal** — the reference is explicit about this.
Equal vertical leaves the game's horizontal comfortably wider than needed, so both axes are
covered with the surplus falling outside the eye. Matching horizontally leaves top and bottom
short, which is the visible half of the aspect mismatch.

⚠️ And the engine **accepts a wide FOV but will not hold it** — it interpolates back toward its
default every tick, which the reference measured as a 128°→80° swing producing a visible zoom and
flickering black bars. The fix there was to force the projection **in the matrix** (rescaling the
x and y columns, which is exactly an FOV change and composes after the positional offset) and
demote the engine's own FOV to CPU culling, asked ~15% wider than what is rendered.
`TdPlayerController::FOVAngle` is at **+0x030C** for that.

### ✅ World scale — **100 UU per metre**, and 1 UU = 1 cm

**Confirmed independently of anything visual**, from the movement speeds the developers tuned in
`DefaultAnimation.ini`:

| setting | value | typical human | implies |
|---|---|---|---|
| `WalkVel` | 200 UU/s | 2.0 m/s brisk walk | **100 UU/m** |
| `RunVel` | 380 UU/s | 3.8 m/s run | **100 UU/m** |
| `FullSprint` | 700 UU/s | 7.0 m/s sprint | **100 UU/m** |

Three numbers, tuned for feel rather than for scale, all landing on the same figure. That is a
far stronger answer than any judgement in a headset, and it happens to agree with the one made
there.

> ### ⚠️ Raising the scale does NOT improve the stereo — it is hyperstereo
>
> Reported while sweeping F11 upward: *"I could see more depth in the distance, but things up
> close looked doubled."* That is the correct and complete description of a widened
> interocular baseline, and both halves are the same effect:
>
> - a wider baseline extends stereopsis further out — the principle behind long-baseline survey
>   rigs;
> - near objects then exceed what the eyes can converge on, and double.
>
> **The distant depth gained this way is the artefact, not the fix.** It also makes the world
> read as a model village, because a human-sized brain interprets excess parallax as
> small-and-close.
>
> **Flat distance at the correct scale is correct.** Human stereopsis gives out past roughly
> 30 m; Mirror's Edge's skyline is hundreds of metres away and has no parallax in reality
> either. Do not tune toward depth in the distance — tune toward comfortable fusion up close,
> which the measured 100 UU/m delivers by construction.

> ### The desktop mirror is doubled too, and that part IS expected
>
> Under alternate-eye the monitor shows every frame, alternating left-eye and right-eye renders.
> Those are offset by design, so the flat image is necessarily doubled — and the flicker is a
> useful confirmation that alternate-eye is running at all. Nothing to read into it, and it
> disappears once stereo becomes simultaneous.
>
> Distinct from the headset doubling below, which is a real finding.

> ### ⚠️ IN THE HEADSET, close objects double at 100% separation
>
> Measured in the headset, not on the mirror. 25–50% reads well; at 100% near objects fail to
> fuse. That should not happen at a true 6.3 cm separation — real eyes fuse close objects
> without effort — so something beyond the separation is consuming the fusion budget.
>
> **Leading explanation: alternate-eye temporal disparity.** Each eye's image is one frame
> stale. Near objects sweep across the view fastest, so the inter-eye difference contains a
> *time* offset as well as a spatial one. Add correct spatial disparity on top and the total
> exceeds what the eyes can converge on — which is exactly "fine at 50%, doubles at 100%".
>
> If that is right, reducing strength is compensating for a timing problem with a spatial
> correction. It works, and **50% is a justified interim default**, but the real fix is
> simultaneous stereo. This is the strongest argument yet for doing that work.
>
> **Not yet ruled out and cheap to test: swapped eyes.** Inverted stereo also presents as
> difficulty fusing. One toggle would settle it and it would be embarrassing to miss.

### Judged in the headset too

F11 swept 25/35/50/70/100/140 and **100 read as life-sized**. At a 6.3 cm IPD that is a
half-offset of ~3.15 UU, close to the 3.32 UU Singularity measured for the same separation. The
agreement is reassuring but was not the reason for the choice — this was judged by eye, which is
the only test that answers "does this feel life-sized".

### ✅ FOV forced to cover the eye

```
[fov] game renders 90.0 x 58.7 degrees (read from the matrix)
[fov] headset wants 100.0 deg vertical; targeting 129.5 x 100.0 at 1.78 aspect
```

**The game natively renders 90° horizontal** — not the 65° `DefaultCamera.ini` implies, and not
an estimate. Reading the frustum out of the matrix beat every guess available.

Forced by rescaling the x and y columns, which is exactly a clip-space x/y scale and therefore
exactly an FOV change, applied **after** the positional offset so the two compose. The forced
constant is also what gets submitted to the compositor: following the observed value is what
produced the reference's flicker, because a submitted frustum that tracks the engine inherits
every wobble its interpolation produces.

> **⛔ CORRECTION.** This was recorded for one round as *"the culling FOV write is not sticking —
> reads back 90.0 every time."* **Wrong.** A later run shows the engine rendering at **148.9°**,
> exactly the value asked for. The write lands; the reads simply happened to fall at moments the
> engine had already interpolated back. That drift is precisely what the reference documents, and
> it is harmless here **because the projection is forced in the matrix** rather than requested.
> Diagnosing a value that oscillates by sampling it occasionally is how the wrong conclusion got
> written down.

### ⚠️ `c0` carries more than one matrix — re-validate every upload

The derived FOV alternates between the scene view-projection and something at **160° × 160°** on
consecutive uploads to the same register — a shadow or light transform. Injecting into it
corrupts that pass, and does so *invisibly*: the symptom appears somewhere else entirely.

**The register says where to look. It is never permission to modify.** Every upload is gated by
the same test that found the matrix — a world→clip matrix maps the camera position to
`clip.w ≈ 0`, and nothing else arriving there does. Four multiply-adds against a camera position
cached once per frame; `c0` is written thousands of times a frame and a memory read per call
would be absurd.

Measured effect: **220,000 accepted against 91 rejected**. The foreign matrix is rare, which is
exactly why this would have been so hard to find from the symptom.

### A large first head-tracking step must be dropped, not applied

```
[head] primed at yaw -719 pitch 2103
[head] write #1  dYaw -749 dPitch +4246
```

~23° of pitch in one frame, which pointed the camera at the floor. The head moved between priming
and the first write — the player was putting the headset on — and the whole accumulated movement
arrived as one step. No real head turns that fast at 90 Hz, so a delta that large means *time
passed*, not that the head moved. Steps beyond ~11° are dropped and counted. **Dropped rather
than clamped**: clamping still injects a large bogus turn, only more slowly.

---

## ✅ Simultaneous stereo — draw duplication, 2026-08-06

Every scene draw is issued twice, into each half of the backbuffer with that eye's matrix, so
both eyes come from the same frame. Each eye gets 1280×1440.

**Reported: depth perception "way better than it ever did in alternate eye rendering, on any
setting", and no doubling at any separation.** That is the confirmation that the doubling under
alternate-eye was **temporal disparity**, not separation — each eye's image was one frame stale,
near objects sweep the view fastest, so the inter-eye difference carried a time offset. Stereo
strength therefore goes back to **100%**, the geometrically correct 6.3 cm, which is now also
the comfortable one.

Costs no extra frame grab — still one backbuffer per frame — so the D3D9Ex wrapper stays
deferred until per-eye resolution is restored by widening the backbuffer.

### Occlusion queries must be overridden while duplicating

UE3 wraps draws in occlusion queries and culls objects whose visible-pixel count returns near
zero. Duplication renders each object into two half-width viewports, so the count the engine
reads back is not the one it would have got, and objects are culled that should not be. That
presents as **objects flickering in and out**.

Overriding `IDirect3DQuery9::GetData` (slot 7) to report a large visible count stops it.

> **Override the answer, not the availability.** The reference records that *refusing to create*
> the queries crashed that build; patching `GetData` runs the same experiment with the engine's
> control flow untouched. Its mode 3 is deliberately not implemented here so it cannot be
> retried by accident.

> **⚠️ All of a device's queries share one vtable**, so `EVENT` queries (fences) arrive at the
> same hook. Check `GetType() == D3DQUERYTYPE_OCCLUSION` per call — a version that checked only
> the data size would overwrite fence results with a pixel count.

Mode **AUTO** (override only while duplication is running) is the default, adopted from the
reference: mono has no reason to pay for disabled culling, and the safe fallback keeps the
engine's own.

> ### ✅ CONFIRMED by A/B, not by inference
>
> ```
> 545: [occ] overriding to VISIBLE (mode 1, 380000 queries faked)
> 546: *** [occ] DELETE -> mode 2: NEVER override - engine culling live
>      <- no override lines at all here, and the flickering returned
> 548: *** [occ] DELETE -> mode 0: AUTO
> 549: [occ] overriding to VISIBLE (mode 0, 400000 queries faked)
> ```
>
> The override provably stopped, the flickering came back, and it resumed. Cause established.
>
> The first attempt to confirm this could not have worked: AUTO had already engaged the override
> when duplication started, and the toggle could only force it *further on*. A switch that
> cannot turn the suspected cause **back on** cannot demonstrate anything — the failing state
> has to be reachable, not just the working one.

### ✅ Head roll — working first try, 2026-08-06

Baked into the view matrix in clip space. Worked with no sign flip needed, unlike yaw and pitch
which were both inverted — which follows: roll goes through the matrix directly rather than
through the engine's `FRotator`, so it never passes the OpenXR→UE3 handedness conversion that
inverted the other two.

> **⛔ Roll CANNOT be left to the compositor.** The projection layer already carries the head's
> full orientation, roll included, so it looks like the runtime should rotate the image. It does
> not: a projection layer is reprojected by the **delta** between the pose claimed and the pose
> the display is at. Claim a rolled pose for an unrolled render while the head is at that roll,
> the delta is zero, and the image presents straight. Baking roll into the render is what makes
> the claimed pose true.

> **⚠️ Rotating the raw clip columns would SHEAR.** The frustum is not square — `tanX` and `tanY`
> differ, and under duplication `tanX` is the half-width per-eye value. Convert to the symmetric
> view-space direction, rotate there, convert back:
> ```
> x_v = clip.x * tanX,  y_v = clip.y * tanY     rotate     clip' = x_v'/tanX, y_v'/tanY
> ```
> Applied **after** the forced projection so the tangents are the frustum the eye actually sees.
> Commutes with the positional offset: an offset pre-multiplies in world space, a roll
> post-multiplies in clip space.

**The angle comes from vectors, not an Euler decomposition** — an Euler order has to be assumed
and the wrong one silently mixes roll into yaw near vertical. World-up is projected perpendicular
to the facing and the signed angle to the head's own up is measured. Degenerate looking straight
up or down, where the projection collapses; the previous value is held rather than snapped to
zero.

### Cost, measured with everything on

Frame grab **3.6–4.1 ms** with draw duplication, stereo, roll and the occlusion override all
running — no worse than the mono path, because the backbuffer is the same size and it is still
grabbed once.

### The shadow-map theory was wrong, and was nearly settled on

The render-target census showed four scene-sized surfaces, two taking heavy draw counts, and the
obvious reading was that a whole-scene dominant shadow map was being split — the reference's own
run 9 diagnosis. It was the wrong cause here. Occlusion was raised as a competing hypothesis and
turned out to be it.

INSERT still bisects which single target is duplicated, kept because the census genuinely cannot
separate four same-sized surfaces and the question may return.

---

## ⛔ Never issue D3D calls on a lost device

**2026-08-06: a test run hard-froze the machine.** Kernel-Power 41, no TDR recorded, reboot
required. The log ends at:

```
--- Reset requested ---
--- Reset returned hr=0x8876086C ---      D3DERR_INVALIDCALL
```

An alt-tab out of exclusive fullscreen lost the device, `Reset` was rejected, and the game ran
for roughly two more minutes before the system died.

Nothing in the mod checked `TestCooperativeLevel`. For those two minutes it kept calling
`Clear()`, `GetBackBuffer()`, `GetRenderTargetData()` and `LockRect()` on a **lost device**,
every frame, while an OpenXR compositor held shared surfaces.

**That this caused the freeze is not proven. That it is invalid is not in question** — every one
of those calls has undefined behaviour on a lost device.

Two changes, and the second is the one that matters:

- The mod now checks first and, when the device is not operational, forwards `Present` and
  touches nothing else. A failed `Reset` sets the same flag and says so loudly.
- **Windowed mode is forced at `CreateDevice`.** Exclusive fullscreen loses the device on every
  alt-tab; a windowed device never takes that path. This removes the hazard rather than
  surviving it, and it was required eventually anyway since the desktop window is only a mirror.

Verified in the next run: `Windowed=1` granted, no device loss, no failed reset.

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

---

## Head tracking, latency, and frame delivery â€” measured

Findings from the judder investigation. Everything here is from a log line, not inference.

### The head basis had a sign error, and it hid for four rounds

The head's forward vector is the **negated** third column of the pose quaternion's rotation
matrix, so its y component is `-2*(yz - wx)`. The code had `+2*(yz - wx)`.

It survived that long because of where it did and did not matter:

| Consumer | Effect | Why it hid |
|---|---|---|
| Pitch | Inverted | `g_pitchSign` had been flipped to `-1` to cancel it |
| Yaw | None | Reads only x and z |
| Roll | Fine when level, ~180Â° at steep pitch | The level-up reference is `worldUp` minus its forward component: at level pitch that is `(0,1,0)` either way |

A defect cancelled by a setting chosen to make its symptom disappear is the expensive kind: the
bug is paid for and kept. `g_pitchSign` is `+1` now; `g_yawSign` stays `-1`, which is the genuine
handedness between OpenXR's anticlockwise yaw and UE3's clockwise one.

The formula now lives in one place (`HeadBasis`). It had been pasted into three, and exactly one
copy was wrong.

### Two coordinate senses, and a diagnostic that could not see it

UE3 has X forward and Y right, so from above `atan2(fwd.y, fwd.x)` grows **clockwise**. OpenXR
has X right and -Z forward, so its yaw grows **anticlockwise**. One physical turn to the right
raises one and lowers the other.

Consequence: `matrixYaw + headYaw` is the invariant, not the difference. Its deviation from its
own slow mean **is** the view's lag behind the head.

A first attempt tracked `cam - head` and `cam + head`, intending the stable one to reveal
handedness. It could not: two mirror-image conventions only ever agree on the sum, whichever way
round the frames actually are.

### Where each rotation axis comes from

| Axis | Path | Corrected how |
|---|---|---|
| Pitch | `Controller.Rotation`, written absolutely from the head | Matrix rotation about the camera's right axis |
| Yaw | `Controller.Rotation`, written as deltas so mouse and stick still accumulate | Matrix rotation about **world** up, driven by the invariant above |
| Roll | Never reaches `Controller.Rotation` â€” `UpdateRotation` zeroes `ViewRotation.Roll` | Clip-space shear in the matrix only |

That asymmetry is diagnostic. Pitch has an absolute anchor and never lagged; yaw has none and
lagged 5-6Â°, which showed as background geometry bouncing while sweeping left and right, and as
nothing at all while looking up and down.

### The sampling site decides whether a correction works

`Present` runs **after** the frame is drawn. Anything sampled there is a frame old before the next
frame's draws use it â€” fine for a value that drifts, exactly wrong for one that steps.

Three separate faults were all this:

- the scene-acceptance test judged each matrix against **last frame's** camera position, with a
  flat 25 unit tolerance. On a zip line the camera covers more than that per frame, so the real
  scene matrix was rejected â€” and a rejected matrix silently leaves the previous one in
  `g_sceneMat`, which every duplicated draw then renders from.
- the animation contribution used by the pitch fix was a frame stale, worst exactly when the
  animation moved fastest.
- head roll was sampled in `Present`, so the image carried the previous frame's roll while the
  pose submitted alongside carried the current one, and the compositor reprojected the difference.

The one point that is both on the render thread and inside the frame is the vertex-constant hook.
The controller rotation, the camera position and the head roll are all read there now.

### Frame delivery

- The game ships capped at **62 fps**. `Engine.MaxSmoothedFrameRate`, found by class name at
  runtime; the ini is hash-checked but a float in memory has no hash.
- Uncapped it reaches **75-80 fps**, with a 4-5 ms frame grab in every frame.
- The headset asks for **120 Hz**.

âš ï¸ **62 into 120 is worse than 60 into 120.** 120/60 is exactly 2, so every frame is shown for
exactly two display periods. 120/62 is 1.935, so most are shown twice and roughly every fifteenth
once â€” a beat of about two per second. Measured as a cadence histogram: 60 gives a single bucket,
every other cap gives a near-even split between one and two periods.

So the default is 60. Fewer unique images than the game can produce, and the only rate it can
currently hold that divides 120.

At 80 fps the frame is 12.5 ms with roughly a third of it in our own frame grab. Taking the grab
off the critical path puts 8.3 ms â€” a true 120, one image per display period â€” inside what the
engine already demonstrates it can do. That is the measured case for the D3D9Ex wrapper.

### Settled by measurement, closed

- **Eye cant** 0.02Â° â€” the runtime is not asking for per-eye orientations a single-matrix render
  cannot give.
- **Roll applied twice** â€” no. The image roll and the submitted pose roll both derive from the
  same head orientation, so they are equal *by construction*; that equality was never evidence
  either way.
- **Multiple views per frame** â€” no. 127896 accepted uploads, none disagreeing, worst 0.03Â°.
- **The compositor inventing motion from a dishonest pose** â€” no, once the poses are sampled in
  step with the image.

### A note on guard thresholds

Two guards in the pitch correction had thresholds picked from an idea of what seemed reasonable
rather than from measurement, and both eventually fired on the feature instead of the fault:

- a 20Â° clamp, against a landing dip that measures 40Â°. It applied half the correction and left
  the other half on screen.
- a 45Â° plausibility bound, against a real 46Â° animation contribution â€” and its fallback set the
  contribution to zero, which during a steep animation applies the *whole animation* as a
  rotation. It yanked the view further than anything it was protecting against.

Both now **bail out** rather than clamp. Leaving the engine's own view alone is never
catastrophic; correcting on a value already judged untrustworthy always can be.

---

## Input: how the game reads a gamepad, and what it binds

### The import table answers the architecture question without a run

`MirrorsEdge.exe` imports, in order:

```
Direct3DCreate9  d3d9.dll  ...  DirectInput8Create  DINPUT8.dll  XINPUT1_3.dll
```

So a 360 pad is a first-class input path in this engine. Everything that path reaches â€” menus,
jump, crouch, interact, run, movement, look â€” is bound and tested by the developers, which makes
a synthesised XInput pad the cheapest complete controller support available.

âš ï¸ The exe names `XINPUT1_3.dll` but **no XInput function by name**, so the functions are imported
**by ordinal** and there is no import name to patch. The function body has to be detoured.

The alternative â€” writing `PlayerInput`'s axis properties directly, which the object model makes
possible â€” moves the player and nothing else. The axes are readable, but the BUTTONS are bound to
exec functions and reaching those means driving the script VM. Every button would be its own
reverse-engineering job, and menus would still need the keyboard.

### The active gamepad bindings

From `TdGame/Config/DefaultInput.ini`. The `.Bindings` lines are the live set; the `-Bindings`
lines above them are removals of the engine defaults.

| Pad control | Command | In game |
|---|---|---|
| LeftShoulder | `GBA_Jump` | **jump** |
| LeftTrigger | `GBA_Crouch` | crouch / slide |
| RightTrigger | `GBA_Fire` | fire |
| RightShoulder | `GBA_LookBehind` | look behind |
| A | `GBA_Use` | use / interact |
| B | `GBA_LookAt` | look at |
| X | `GBA_ReactionTime` | reaction time |
| Y | `GBA_SwitchWeapon` | switch weapon |
| Start | `GBA_Pause` | pause |
| Back | `GBA_InGameMenu` | in-game menu |
| RightThumbstick | `GBA_ZoomWeapon` | zoom |
| LeftX / LeftY | `GBA_Strafe_Gamepad` / `GBA_Move_Gamepad` | move |
| RightX / RightY | `GBA_Turn_Gamepad` / `GBA_Look_Gamepad` | look |

**Jump is on a shoulder, deliberately** â€” it keeps both thumbs on the sticks. That is the right
call for a pad and reads oddly on a controller held in the fist, where it lands on the grip. The
mod maps 1:1 and does not second-guess it.

### âš ï¸ The two controls the table above leaves out, and why they matter

The table lists what is bound to something useful. Reading it as the complete picture is what
sent one investigation the wrong way, so the remaining two entries are recorded here.

**D-pad â€” bound, but to engine debris. Do not map it.** The game removes the engine defaults
(`MoveForward` / `MoveBackward` / `TurnLeft` / `TurnRight`, lines 61â€“64) and replaces them under
a comment reading `; Hard bindings`:

| D-pad | Command |
|---|---|
| Up | `SwitchToItemInSlot 1 \| God` |
| Down | `SwitchToItemInSlot 2 \| Jesus \| TriggerEmoteMessage 1` |
| Left | `SwitchToItemInSlot 3 \| InvertMouseCheat \| TriggerEmoteMessage 2` |
| Right | `SwitchToItemInSlot 4 \| DropMe \| TriggerEmoteMessage 0` |

Vestigial UT3 CheatManager exec functions, not Mirror's Edge actions. Whether a shipping build
instantiates the CheatManager at all is **unverified** â€” but `InvertMouseCheat` or `God` firing
mid-run is a bad way to find out, and there is no upside to weigh against it.

In menus the d-pad *is* used heavily â€” `NavFocus*`, `MoveSelection*`, `ScrollUp`/`Down`, slider
increment, `NextPage`/`PreviousPage`. Every one of those aliases lists `Gamepad_LeftStick_*`
beside it, so the left stick already reaches all of it. Two go further and are stick-only:
`UIOptionListBase.MoveSelectionLeft/Right` names no d-pad key, and the chapter shelf uses
shoulders and triggers. **Nothing in the UI needs the d-pad.**

**Left thumbstick click â€” deliberately blanked, and therefore free.** Line 146:

```
.Bindings=(Name="XboxTypeS_LeftThumbstick",Command="")
```

An explicit override killing the engine default of `ToggleDebugCamera` (`BaseInput.ini` line
115). It is dead in menus too â€” every widget alias naming a stick click names the right one.

That made it the place to put **Back / `GBA_InGameMenu`**, which was otherwise unreachable: a
Touch pair has no spare button, and the right controller's system button belongs to the runtime.
The mod maps left stick click â†’ `MEVR_PAD_BACK`.

Menus bind **A to `Clicked`** and **B to `Consume`**, so any remap that moves those makes confirm
a different button in menus than in play.

âš ï¸ Hand-editing `DefaultInput.ini` to remap is expected to fail: the game hash-checks its config
and refuses to start when modified, measured earlier at a cost of two runs. Nothing in the input
config mentions rebinding, so whether the in-game controls menu exposes gamepad rebinding is
**unverified**. A remap inside the mod has no such problem â€” the pad is synthesised, so any
physical control can drive any pad button.

### Notes on synthesising the pad

- Only index 0 answers. Reporting a controller on all four makes the game believe four players
  are present.
- The **packet number must change when the state changes**, or a poller comparing packet numbers
  decides nothing happened and skips the read.
- `XInputGetCapabilities` needs hooking too, with every field at its maximum. That structure
  describes what the device is *capable* of, not its current state, and a zeroed one reads as a
  pad with no sticks.
- Actions are synced once per frame **beside the head sample**, so the sticks and the view
  describe the same instant.
- OpenXR action-set attachment is **permanent**: once attached to a session no further action can
  be created, so every action must exist before `xrAttachSessionActionSets`.
- Bindings suggested for `oculus/touch_controller` (what a Quest reports through Virtual Desktop)
  and `khr/simple_controller` as a fallback. Simple carries only select and menu â€” not playable,
  but an unrecognised controller reaches the menus instead of appearing dead.
