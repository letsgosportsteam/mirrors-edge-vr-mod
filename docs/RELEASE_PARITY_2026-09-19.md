# Second release/test comparison — 2026-09-19

Follow-up to [the first four runs](FOUR_RUNS_2026-09-19.md). The new logs and
1,026 capture files are preserved under ignored `.analysis/release-parity-20260919/`.
Both installed DLLs are unchanged from those first runs. The previous local
startup candidate was not installed, so these runs did not test its corrections.

| Artifact | Test installation | Steam release installation |
|---|---|---|
| d3d9.dll SHA-256 | 70939A64654F74692D123B8B9C48469ABCA1C11766030D9984A8B228BAC09D41 | 5DEBDFFD5D1D2D10EEA33D7858B074FAAB35FFBEC4EA4677079E4BEA30627A13 |
| openxr_loader.dll SHA-256 | D60C68DCF66C8DAD31CA5F251DA6423C94631BBD7AD3B57F70A487E8C918C86A | Identical |
| Initial Debug / ArmSwingDebug | off / on | off / off |

The previous release process rebuilt the DLL and generated mevr.ini from the
example. Effective-settings checks allowed disabling diagnostic options, which
explains Detailed Logging showing Off. These checks did not guarantee artifact
identity. The new instruction requires every file and setting to remain unchanged.
See [the replacement publication workflow](RELEASE_WORKFLOW.md).

## Repeated initialization failures

Runs 1 (13:05:24) and 3 (13:12:12) again created a 1600x1200 backbuffer with a
1600x900 world target, causing intentional mono fallback after successful DLL
loading. Their combat initialization also failed (missing aim function in run 1,
missing mesh/socket-name fields in run 3). Both builds can suffer startup failures.

Run 2 (Steam, 13:06:42, PID 24092) resolved zero mesh/socket-name fields during
its background pass. The one-shot combat setup failed permanently. This also
prevented CameraVisibilityValidate and the shared interpreter hook from enabling
the pre-culling camera correction. The fix's source exists in the release but
never became active in this run. Markers 2 and 3 correspond to the reported
scripted-camera culling failure. Marker 4 was accidental and is not evidence of
another reported issue.

Run 4 (test, 13:13 onward, PID 16104) resolved both fields, passed the live
dispatch check, logged early scripted camera=ready, and applied head look before
engine culling (frames 5997, 6297, 6597, 6897). This directly explains the
different culling behavior without requiring a missing source fix.

## Smoke capture

Steam marker 1: frame 8550, captured 8553. Test marker 1: frame 5254, captured
5257. The final images visibly confirm missing rooftop smoke in Steam and
visible smoke in the test installation.

The smoke-producing shader pair is VS 23C40ECF / PS FABE3964. The matching draw
has 244 primitives in both captures: Steam draw 1593, test draw 1644. Both are
classified as scene geometry, duplicated, unsuppressed, and return successful
before/after readbacks. The test draw visibly adds the smoke; the Steam draw
does not. This narrows the problem beyond aggregate UP duplication counters.

The captures contain different billboard basis constants (notably VS c13/c14)
and lighting values. They do not contain enough matching geometry/state to
attribute the difference to a specific DLL instruction, game setting, or
initialization dependency. No speculative smoke shader patch is justified yet.
The rendering modules are unchanged between source commit a9442b4 and v0.2.1.
The game executables themselves have different hashes, so these are not fully
identical environments even if the mod files are made identical.

Follow-up: [deeper smoke analysis](SMOKE_RELEASE_2026-09-19.md) rules out the
c13/c14 difference as sufficient evidence, verifies populated scene-depth inputs
and matching shared effect assets, and adds a focused input/state diagnostic.
The smoke root cause remains unconfirmed.

## Artifact verification

The new packaging path locks and snapshots the three files and verifies the
uncompressed ZIP entries against a frozen SHA-256/size manifest. Regression
checks cover preserved diagnostic settings, immutable output, changed INI,
changed DLL, extra ZIP entries, and rejection of rebuild-for-release.

The current installed test INI has Debug=on following capture and ArmSwingDebug=on.
An exact snapshot preserves both. It is labelled as a snapshot, not silently
approved/published as final release defaults. No game installation or release
asset was replaced in this investigation.
