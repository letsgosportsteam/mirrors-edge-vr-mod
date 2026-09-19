# Repository guide for coding agents

## Start here

- Read `README.md` for the player-facing feature set and controls.
- Read `docs/README.md` to find the relevant feature and investigation notes.
  Search `docs/` before changing a subsystem; the working notes were moved there
  from the repository root. Do not assume they are missing because a root-level
  search no longer finds them.
- Consult `ENGINE_NOTES.md` for measured engine behavior and native input bindings.
  `FEASIBILITY.md` is the original assessment, not the current implementation status.

## Using and maintaining the notes

Notes are dated evidence, not a single up-to-date specification. Read newer
follow-ups for the same topic and check the current source and `mevr.ini.example`.
Distinguish an implemented fix, an automated test, a desktop startup, and actual
headset verification; one does not establish the others.

Put new feature plans, investigations, and validation write-ups in `docs/` and
add them to `docs/README.md`. Keep this routing file, the public README, engine
notes, and feasibility assessment at the root. Preserve older findings and clearly
mark superseded conclusions instead of silently rewriting historical evidence.
Commands and code paths in investigation notes are relative to the repository
root unless a note says otherwise; Markdown links are relative to their file.

## Code, checks, and publication

The implementation is in `src/d3d9.cpp` and the adjacent `.inl`/`.h` modules.
`src/build.ps1` builds x86 and runs static analysis. Use the relevant existing
regression harnesses in `tools/` for code changes; documentation-only changes
need link/path and whitespace checks rather than a rebuild.

Run `tools/check-clean.ps1` before pushing. Keep machine-local configuration,
game-derived data, logs, and build products out of commits. Respect the existing
Git identity hooks. Local diagnostic captures live under ignored `.analysis/`;
notes may reference them, but they are not included in a fresh clone.

When changing player-facing behavior, update `README.md` and the release guide
`packaging/README.txt` together. Check shipped defaults against `mevr.ini.example`
and `src/build.ps1`; release packaging turns Debug off. Do not copy private live
configuration values into public documentation.

The install ZIP contains exactly `d3d9.dll`, `openxr_loader.dll`, and `mevr.ini`.
`src/build.ps1 -Package` embeds license notices as INI comments and runs
`tools/check-package.ps1`. Upload the ZIP and PDB as actual GitHub release assets;
links in release prose alone are not attachments.

Before a release, compare its INI against the active installed test-build INI
with `tools/check-release-settings.ps1` (after building the menu harness).
Compare effective compiled fallbacks as well as explicit keys. Gameplay defaults
must match the tested configuration; diagnostic options may be disabled.
