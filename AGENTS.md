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
and `src/build.ps1` before testing. Do not copy private live
configuration values into public documentation.

The install ZIP contains exactly `d3d9.dll`, `openxr_loader.dll`, and `mevr.ini`.
`tools/package-tested-build.ps1` freezes the approved installed files without
compilation or INI edits. `src/build.ps1 -Package` is blocked. Upload the ZIP and
its accompanying LICENSES.txt as actual GitHub release assets;
links in release prose alone are not attachments.

For the user's independently rebuilt candidate workflow, use
`tools/build-reproducible-candidate.ps1`: build 1 is the new test candidate;
the ZIP contains independently compiled build 2, checked against build 1.
Do not call this parity with an older installed DLL. Preserve the source
snapshot, both build manifests, matching PDB and candidate evidence; obtain
headset approval of the new candidate before publishing. Debug Overlay defaults
to off and Detailed Logging to on for the v0.2.2-alpha candidate. See the current
independent-build section in docs/RELEASE_WORKFLOW.md.

The user's approval applies to the exact tested d3d9.dll, openxr_loader.dll,
and mevr.ini. Preserve all three byte for byte, including Detailed Logging and
other diagnostics. Do not rebuild for a version string, regenerate the INI from
the example, normalize settings, or append comments after approval. Prepare any
desired changes before testing. Diagnostic capture-time settings must be clearly
identified in the frozen snapshot, not silently treated as release approval.

Use `tools/check-tested-package.ps1` with the frozen tested-files.json to verify
both the local ZIP and the downloaded GitHub asset. Preserve the manifest and ZIP
hash as release evidence. A settings comparison alone is insufficient. Only
attach the PDB retained from the exact tested DLL build; a freshly rebuilt PDB
does not match an older DLL. Read docs/RELEASE_WORKFLOW.md before publication.
