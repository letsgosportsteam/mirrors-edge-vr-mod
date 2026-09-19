# Release the approved files

## Current independent-build workflow

The user has explicitly requested independent source compilation and accepted a
new reproducible candidate for testing. This supersedes the copy-only recipe
below for v0.2.2-alpha. The older installed test DLL could not be reproduced
exactly; see [the audit](RELEASE_REBUILD_2026-09-19.md).

```powershell
.\tools\build-reproducible-candidate.ps1 -OutputDirectory 'dist\new-candidate' -ArchiveName 'mevr-version'
```

This snapshots the source inputs, compiles every source twice with the same
configured toolchain and SDK, disables incremental linking, and packages **build
2** only after its three files match **build 1** byte for byte. Build 1 is the new
test candidate. The INI is generated from the source template each time; the
OpenXR loader comes from the configured SDK. Neither build takes its files from
the game installation. Source changes during compilation fail the process.

`src/build.ps1 -Reproducible` uses `/Z7 /Brepro` and
`/DEBUG:FULL /INCREMENTAL:NO /Brepro /PDBALTPATH:d3d9.pdb`. The log identifies a
stable source hash instead of compile time. Reproducibility has been verified
with two builds in this checkout and configured toolchain; it is not a claim
that arbitrary compiler/SDK versions or checkout locations give identical files.

The v0.2.2-alpha candidate has Debug Overlay off and Detailed Logging on. Test
build 1 with the generated INI. Verify the installed files against
`build-1-files.json` and the build-2 ZIP before and after testing. Do not edit the
INI, rebuild a new version string, or replace files after approval. Changed test
settings require a deliberately updated source candidate and a fresh two-build
comparison; do not copy a live INI into the release behind the user's back.

Only after headset approval, upload the existing verified ZIP and matching PDB
as release assets with LICENSES.txt and the manifest. Download the uploaded ZIP
and verify its hash and entries against build 1 again. Preserve candidate-evidence.json
and the source snapshot, including uncommitted changes, with the release evidence.

## Earlier snapshot-only recipe

The release is a copy of the three installed files the user approved, not a new
build of approximately the same source/configuration. This supersedes the old
build-and-package path and permission to disable diagnostics during packaging.

1. Choose the version and final defaults before building/testing. Build once,
   retain that DLL and its matching PDB, and record the source commit plus any
   uncommitted source changes. Use a clean committed source for new candidates.
2. Test those files in the headset. Detailed Logging, gameplay settings, and
   other options must be the ones intended for release. Turn off a temporary
   capture overlay before final settings approval if the user wants it off.
3. Freeze the approved Binaries directory into a **new** output directory:

   ```powershell
   .\tools\package-tested-build.ps1 -BuildDirectory '<tested-game>\Binaries' -OutputDirectory 'dist\approved-alpha' -ArchiveName 'mevr-approved-alpha'
   ```

   This copies only d3d9.dll, openxr_loader.dll, and mevr.ini, unchanged. Source
   files are held read-only during the snapshot. The ZIP has exactly those three
   top-level entries. The frozen files and tested-files.json remain beside it.
   Output directories cannot be reused. Snapshotting does not itself establish
   user approval; record what was actually approved before publication.
4. Check private paths/content and accompanying notices. Upload the ZIP and
   LICENSES.txt as actual release assets. Notices may already be in the tested
   INI; never add them to an approved file afterward. Publish the frozen manifest
   with the release evidence. If symbols are available, attach only the PDB from
   this exact DLL build. Do not rebuild to obtain a missing PDB or version string.
5. Download the uploaded ZIP to a new path, compare its SHA-256 with the frozen
   archive, and verify every decompressed file:

   ```powershell
   .\tools\check-tested-package.ps1 -ZipPath '<downloaded-asset.zip>' -ManifestPath 'dist\approved-alpha\tested-files.json'
   ```

`src/build.ps1 -Package` now fails before compiling. The older
`check-package.ps1` remains useful for checking a newly prepared default template;
its comparison to current source defaults is not proof of parity with an approved
test build. `check-release-settings.ps1` is a diagnostic comparison, not an
alternative to byte-for-byte checks.

Identical files prevent release drift. Game executables/assets, engine user
settings, runtime, and initialization timing can still cause different behavior
between installations. Log the artifact hashes in the investigation and keep
those failures distinct from packaging errors. Never claim that hash equality
proves a fix works in the headset.
