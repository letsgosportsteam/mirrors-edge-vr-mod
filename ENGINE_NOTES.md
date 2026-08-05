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

**Why it is created twice is still not known, and rung 1 FAILED to settle it.** The plan was to
log `self` inside `CreateDevice` and see which instance it was. In the rung-1 run both
`Direct3DCreate9` calls returned `0278D960` and `CreateDevice` arrived on `0278D960` — so the
address identifies nothing. The aliasing described above is precisely what defeated the
instrument.

Settling it would need identity that survives reallocation: a counter assigned at creation
time, or hooking `AddRef`/`Release` to watch the first object die. **Not worth doing unless
something later depends on it.** Vtable patching means nothing currently does — which is the
second time that choice has paid for itself here.

Recorded as a method note: *an instrument that reads an address cannot distinguish objects that
share one.* The trap was written down before the test was built and the test was still built
around it.

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

## Script

`TdGame.u` is 29 MB, `PackageFlags = 0x02A80009` (`PKG_Compressed`), 20,592 names / 52,410
exports / 2,246 imports. The name table is inside compressed chunks — standard UE3 chunked LZO,
which the Singularity project's `uedecompress` does not handle (it assumes fully-compressed
packages and fails with `chunk 0 runs past end of file`).

`DefaultCamera.ini` confirms the camera class exists as UnrealScript, not native:

```ini
[TdGame.TdPlayerCamera]
FreeflightScale = 1.0
```

Reading `TdPlayerCamera` and `TdPlayerController` is the highest-information-per-hour task
outstanding. It decides whether the camera is steered by binary detour or by a custom
UnrealScript package, and that choice should not be guessed.
