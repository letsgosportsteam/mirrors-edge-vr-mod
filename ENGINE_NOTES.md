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

**Why it is created twice is not yet known.** The plausible reading is a throwaway instance for
adapter and display-mode enumeration to populate the video options, followed by the real one —
but that is a hypothesis, not a measurement. Rung 1 settles it by logging which instance
receives `CreateDevice`; until then, do not build on the assumption that call #2 is the one.

### Not yet measured

Deliberately left to rung 1 rather than guessed, because a wrong backbuffer format cost the
Singularity project a `D3DERR_INVALIDCALL` that was traced back to a note saying `X8R8G8B8`
where the truth was `A8R8G8B8`:

- backbuffer format, dimensions, and whether MSAA is in use
- `CreateDevice` behaviour flags
- windowed vs fullscreen at device creation (the ini says fullscreen; the mod will need windowed)
- `D3DPOOL_MANAGED` vs `DEFAULT` allocation counts, which drive the texture wrapper design

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
