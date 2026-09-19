# Independent release rebuild audit — 2026-09-19

The user requests a new release compiled from source, with all three ZIP entries
verified against the tested installation. Merely copying the installed files is
explicitly disallowed for this request. This overrides the snapshot-only recipe
in [RELEASE_WORKFLOW.md](RELEASE_WORKFLOW.md) for this release attempt; snapshot
hashes remain useful as the comparison reference.

The user also specifies **Debug Overlay off, Detailed Logging on**. These are
separate menu settings: `Debug` controls the overlay/hotkeys; Detailed Logging
sets `MotionHandsDebug`, `ArmSwingDebug`, and `ParkourDebug` together. The currently
installed test INI has Debug on, hand/parkour logging off, and arm-swing logging
on, which makes the combined Detailed Logging menu item display On. A candidate
with the requested switches will intentionally differ from that existing INI.
Do not describe it as a byte-for-byte match to the unchanged INI.

## Original reference

Read-only hashes were recorded under ignored
`.analysis/release-rebuild-20260919/tested-files.json` before rebuilding:

| File | SHA-256 |
|---|---|
| d3d9.dll | 70939A64654F74692D123B8B9C48469ABCA1C11766030D9984A8B228BAC09D41 |
| openxr_loader.dll | D60C68DCF66C8DAD31CA5F251DA6423C94631BBD7AD3B57F70A487E8C918C86A |
| mevr.ini | DFB8532AA25CC48D495AEEBE52EBD8ADE76012FFD132E0A205A336EF1DE9BFA2 |

The test DLL logs an 08:42:04 build on September 19. Commit a9442b4 was created at
08:56:08 and is the nearest retained source candidate; this timing alone does
not prove source identity. Subsequent source changes include version strings,
compiled defaults, embedded INI/license content, and later uninstalled startup
and diagnostic work. Building the current tree cannot silently substitute for
the old tested DLL.

## Fresh historical-source build

An isolated `git archive a9442b4` was built with its own build.ps1, the configured
x86 toolchain and SDK, and Windows line endings restored for the embedded INI
template. The required static analysis and DLL compilation passed, with existing
advisory warnings. No test-installation file was used as a build output or
installed/replaced. The fresh DLL hash is:

`DBF436559E562512E07D8E7F63F5006551D40359C4C69ADB0CBE6D23B92C9E00`

It **does not match** the test DLL. The old/new file sizes are 1,313,280 and
1,320,960 bytes. PE inspection records differences in code/data section sizes,
addresses and contents, timestamps, and the embedded PDB path. These are not
proof of gameplay differences, but they are not an exact-match verification
either. Both contain incremental linker jump tables. The build embeds
`__DATE__`/`__TIME__`, uses `/Zi`, and does not disable incremental linking or
provide reproducible debug identities. The exact original source/build/link
state has not been recovered, and metadata differences have not been shown to
account for every differing byte.

Evidence, build log, fresh source/DLL/PDB, PE section comparison, and disassembly
are retained in the ignored audit directory. Do not patch binary bytes or copy
the original DLL merely to make the comparison pass.

The release remains unverified pending either reconstruction of the old build,
or an independently reproducible candidate accepted as a new tested reference.
No release ZIP or GitHub release was published by this audit.

## Accepted new candidate: v0.2.2-alpha

The user chose **Prepare a reproducible candidate for a new test**. This resolves
the earlier blocker by establishing a new reference; it does not retroactively
make the old DLL reproducible. The current source includes the pending Auto-mode,
initialization-retry and motion-sampling corrections, plus the focused diagnostic
capture (inactive with Debug off). The smoke investigation remains tabled.

The reproducible build replaces compile-time timestamps with a source hash,
uses embedded object debug information and deterministic compiler/linker mode,
disables incremental linking, and embeds a stable PDB filename. Build 1 and build
2 independently compile all sources. Each generates mevr.ini from the source
template and obtains openxr_loader.dll from the SDK. No installed test file is
used as a candidate or package input. The script rejects source drift and any
file mismatch before creating a ZIP.

Both independent builds produced these identical install files:

| File | SHA-256 |
|---|---|
| d3d9.dll | A851D578AC0C45ABB41C4A3BA0F3CF88715AF39C13C113F5D3CF9DCF43C47A2D |
| openxr_loader.dll | D60C68DCF66C8DAD31CA5F251DA6423C94631BBD7AD3B57F70A487E8C918C86A |
| mevr.ini | E5288480E69D395D88FEE622C7BBA7BF5842E64B58AE357E9A2EB162EA9C8A12 |

Source identity:
`7426D62783ADA34E46774E8BEE3387486047C1A1E6D3ED0C4DFD15265B2A832C`.

The ZIP contains **build 2**, not a copy of the test installation or build 1:
`dist/mevr-0.2.2-alpha-candidate/package/mevr-0.2.2-alpha.zip`.
ZIP SHA-256:
`900F408ED39487246522B228A421FA9243EB5D4639F90EA175163CAB6A5C789A`.
Its three entries were verified against build 1 and then against a fresh manifest
of the installed test files. Build 1 is installed in the GOG test installation;
all three old files were backed up under
`.analysis/release-rebuild-20260919/previous-test/`. The Steam installation was
not replaced. Source snapshots, both build manifests/logs, installed manifest,
the matching build-2 PDB, and LICENSES.txt are retained with the candidate.

Validation: both required static-analysis/build passes, exact archive parity,
x86/three-file/defaults/license checks, production VR-menu/Auto-mode/motion-sampling
tests, and bounded combat-initialization regression scenarios passed. All 72
effective settings were compared to the preceding test INI: the only differences
are Debug on to off, MotionHandsDebug off to on, and ParkourDebug off to on.
ArmSwingDebug was already on. No gameplay/calibration setting changed in that
comparison. The shipped template, compiled defaults and Restore Defaults agree.

This is a **new candidate awaiting headset verification**, not a published release
or a claim of parity with the older installed DLL. No GitHub release has been
published. Verify the three installed files remain unchanged after user testing
before publishing the existing ZIP; do not silently incorporate saved settings
or rebuild a different version afterward. PDB bytes are not included in the
three-file parity claim; the packaged PDB is retained directly from build 2.

## Follow-up: headset approval

The user subsequently approved this candidate after Steam VR tests with PhysX
disabled. This supersedes the awaiting-headset-verification status above.
The post-test comparison found identical DLLs and one formatting-only INI
difference. The user explicitly accepted the original ZIP with that exception.
See [the v0.2.2-alpha record](RELEASE_v0.2.2-alpha.md) for the approval scope,
PhysX limitation, and current publication status.
