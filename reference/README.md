# reference/

`singularity_d3d9.cpp` is the **complete, working** VR shim from the Singularity mod, kept
here verbatim as a guide. It is not compiled and is not part of the build.

## Why it is here rather than being the starting point

It is ~14,800 lines that reached a full VR ladder over roughly 180 runs. Most of it is
engine-agnostic and worth lifting; a substantial part encodes decisions about how
*Singularity* specifically behaves:

| | lines mentioning it |
|---|---|
| weapon / gun / hand anchoring | ~265 |
| aim, fire, trace | ~189 |
| HUD stereo, crosshair | ~138 |
| mesh hide / latch | ~89 |
| object model (`GObjects`, `UProperty`, offsets) | ~84 |
| rotation detour | ~48 |
| cutscene handling | ~32 |

Starting from a stripped-down copy would produce something that compiles and looks finished
while carrying engine assumptions that are wrong here — Mirror's Edge is UE3 **536** against
Singularity's **584**. Every misbehaviour would then be ambiguous between a port error, a wrong
offset, and a genuine engine difference, with no known-good state to fall back to.

That failure mode has a name in the Singularity notes: *"almost every hard problem here was a
stale premise, not a hard problem."* An adapted-but-untested 14,000-line file is that, at scale.

## How to use it

`src/d3d9.cpp` is grown one rung at a time, and this file is the guide:

- **Lift verbatim, in whole blocks** — the D3D9 proxy and device/texture wrapper, pool
  translation, the D3D9Ex upgrade and shared-surface path, OpenXR session and swapchain setup,
  logging, ini handling, MinHook plumbing. This code contains fixes that cost real runs
  (`AddDirtyRect(NULL)` for dirty regions, `A8R8G8B8` vs `X8R8G8B8`, `MANAGED`→`DEFAULT`).
  Retyping it would reintroduce bugs that are already solved.
- **Do not port at all** — aim modes, weapon anchoring, mesh latching, HUD stereo, cutscene
  handling. Re-add each as its own rung, once there is something to verify it against.
- **Re-derive, never copy** — every engine address, offset and register.

The comments are as valuable as the code. Several record approaches that did *not* work, and
those are cheaper to read than to rediscover.
