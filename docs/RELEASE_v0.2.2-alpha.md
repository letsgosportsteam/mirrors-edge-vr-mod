# v0.2.2-alpha

## Changes

- Auto resolution exposes the cached headset size as the only display mode,
  avoiding startup at an incompatible resolution. The first connected run still
  records the headset size for the next launch.
- Combat metadata discovery retries transient initialization failures within a
  bounded budget. These failures could prevent melee and the shared camera
  visibility correction from initializing.
- Motion punches keep sampling hand movement when arm swing and its logging
  are both disabled.
- Debug Overlay defaults to off; Detailed Logging defaults to on. Motion hands,
  arm swing, tuned pistol alignment, and bar-swing settings are retained.
- Independently compiled candidate builds are compared byte for byte before
  packaging, with source snapshots, manifests, and matching symbols retained.

## Installation and compatibility

Download `mevr-0.2.2-alpha.zip` from the release's **Assets** and copy its three
files (`d3d9.dll`, `openxr_loader.dll`, `mevr.ini`) into the game's `Binaries`
folder beside `MirrorsEdge.exe`. Connect through Virtual Desktop with VDXR.
The PDB, license notices, and verification manifest are separate assets and do
not need to be installed.

The user approved the candidate after VR testing on Steam with **PhysX disabled**
on September 19, 2026. Chapter-load freezes with PhysX enabled were also reported
without the mod, including after reinstalling the game. Turn PhysX off in the
game's settings if chapter loading freezes. The underlying cause is unresolved;
this release does not claim a PhysX or missing-smoke fix.

## Verification and publication status

The two original builds and existing ZIP still match the hashes recorded in
[the independent-build audit](RELEASE_REBUILD_2026-09-19.md). Candidate source
inputs are unchanged. Both installed DLLs match those builds exactly.

Post-test inspection found the three Steam files renamed with `.off` extensions.
The Steam run at 18:56:08 identifies the candidate source build and records VR
gameplay. A later run used the development installation. Both installations now
have an INI differing from the original candidate only in the spelling
`SmoothTurnSpeed = 1` versus `SmoothTurnSpeed = 1.0`; exact text comparison after
that single substitution succeeds. The production loader confirms all 72
effective settings are identical, including Debug off and Detailed Logging on.

This is a two-byte difference, so the post-test INI is **not byte-identical** to
the original ZIP. No INI, DLL, or ZIP has been changed to hide that discrepancy.
The user explicitly accepted publication of the unchanged original candidate ZIP
with this formatting-only exception to the exact-file requirement. All other
file contents remain subject to exact-byte verification. The release uses the
existing independently compiled build-2 ZIP and its retained PDB; no rebuild or
post-approval INI edit is authorized or necessary.

Original ZIP SHA-256:
`900F408ED39487246522B228A421FA9243EB5D4639F90EA175163CAB6A5C789A`.

Post-test INI SHA-256:
`9BBE55A49A3CB2006FE9A90BA8EDC3EAB551F34D7A8AC99A5DFE643D8EC05F51`.

The retained build-2 PDB remains unchanged. Local approval evidence and the
successful Steam log are under ignored `.analysis/release-approval-20260919/`.
The original build/static-analysis and regression results remain recorded in
the audit; documentation updates do not rebuild the approved DLL.

Before tagging, all 41 recorded source inputs were checked against the current
workspace and matched their candidate snapshot hashes. Those previously
uncommitted inputs are committed and pushed as the release source. The build's
recorded base commit remains historical evidence, not a claim that the base
commit alone contained the candidate changes.
