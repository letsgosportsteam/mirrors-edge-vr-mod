# OpenXR startup diagnostics, 2026-09-20

A reported v0.2.1-alpha run loaded the proxy and D3D9 hooks successfully, then
both `xrCreateInstance` attempts returned `-32` (`XR_ERROR_FILE_ACCESS_ERROR`).
The existing generic headset/streaming hint did not identify the failing file.
The reporter confirmed Virtual Desktop with VDXR; the log alone does not identify
the selected runtime, failing component, or GOG/EA storefront.

## Implementation

`src/xr_diagnostics.inl` runs from `InitXR` at first Present, outside DllMain.
Readable API versions and symbolic/numeric results remain enabled independently
of Detailed Logging. Unknown result values retain their numeric code. Session,
HMD, graphics-requirements and reference-space startup calls also report results.
Failure explicitly states that flat mode continues and VR needs a game restart.

Detailed Logging uses the same condition as the menu: any of MotionHandsDebug,
ArmSwingDebug or ParkourDebug enabled. It adds:

- Loaded OpenXR loader path and file version.
- Relevant OpenXR environment overrides and HKLM's **32-bit** ActiveRuntime.
- HKLM/HKCU 32-bit implicit/explicit layer registrations and registry enable state.
- Bounded manifest inspection, decoded relative library paths, readable-file
  checks, PE machine type, and environment gate presence. Registered layers are
  explicitly distinguished from components actually loaded. Bare DLL names using
  Windows search paths are reported without guessing their resolution.
- Startup-thread `OutputDebugStringA/W` messages, plus `LoadLibraryW/ExW` attempts
  and their actual Win32 error codes/text. The latter captures the failed DLL
  path even when a loader warning is sent only to stdout.

The four temporary MinHook hooks forward original calls and preserve DLL-load
results and last-error codes. They are disabled when InitXR returns, including
failure returns. Their tiny trampolines remain allocated to avoid freeing code
while another thread may still be returning through a forwarded call. Existing
hooks owned by another component are not replaced. No loader environment,
standard stream, runtime registration, or layer enable state is changed.

Messages and library calls are each capped at 128. Manifests are capped at 64 KiB,
JSON nesting at 16, and registry inventories at 128 entries per hive/category.
Control characters are escaped and long messages are split/bounded. Inspection
opens files for reading; it never loads a DLL just to diagnose it.

This is not a complete loader trace: stdout-only messages and other threads'
messages are not captured. Inventory checks cannot establish dependency health,
actual layer activation, or headset availability. Environment/elevation and
loader-version rules remain authoritative. A readable x86 DLL can still fail to
load because of a dependency. A Win32 126 error identifies the requested DLL but
does not necessarily identify which dependency is missing.

## Validation

Passed: `src/build.ps1` (x86 build and static-analysis checks),
`tools/test-xr-diagnostics.ps1`, the existing `tools/test-vr-menu.ps1` regression,
`tools/check-clean.ps1`, documentation link checks and whitespace checks. The
build retains existing SDK/engine static-analysis advisories; the diagnostic
module introduces no reported analysis warnings.

`tools/test-xr-diagnostics.ps1` compiles the production diagnostic module as x86.
It verifies structured/escaped/Unicode manifest paths, malformed and duplicate
keys, nesting/size limits, x86/x64 PE evidence, missing files, control-character
escaping, the logging-off gate, and startup-thread capture/cleanup. Native DLL
load failures preserve Win32 126 and 193. With a child-process-only nonexistent
runtime override, the actual supplied OpenXR loader fails before loading a
runtime or API layers, and its error messages reach the diagnostic log before
any OpenXR instance exists. No machine registration is edited.

This synthetic failure is `XR_ERROR_RUNTIME_UNAVAILABLE`, not a reproduction of
the reporter's `-32`. The reported machine still needs a run with the new build
to identify its failing component. Automated diagnostics do not establish game
startup or headset verification. No tested release files or approved ZIPs are
modified by this change.

References: [Khronos result codes](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrResult.html),
[loader diagnostics](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/loader/loader_logger.cpp),
[Windows DLL loading](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/loader/loader_platform.hpp).

## Independently rebuilt publication candidate

The user requested a rebuild and exact test/release parity, preferably updating
the existing v0.2.2-alpha release. The existing release is editable (GitHub reports
`immutable: false`); replacing assets is possible, but the original assets and
their approval remain distinct from this new candidate. No published asset or
tag has been changed during candidate preparation. A new release is not a
technical requirement; any in-place update must clearly identify its new source
commit and replace the matching PDB and verification records alongside the ZIP.

Before rebuilding, the installed development DLL, loader, and INI were preserved
under ignored `.analysis/openxr-release-refresh-20260920/previous-test/`, together
with the normal build's matching symbols. That installed DLL had SHA-256
`E9661680D395294830EB54880BAF27D635288AB3BF0EF0CDEBEF08C7947FC0A0`.
Its recorded source identity matches this candidate, but its normal build used
different compiler/debug/linker settings, so it is not byte-identical to the
reproducible DLL. No parity with that older binary is claimed.

`tools/build-reproducible-candidate.ps1` compiled all source inputs twice with
mandatory static analysis. Both builds produced identical install files:

| File | SHA-256 |
|---|---|
| d3d9.dll | 27600FE9C44DC3C30D84581F684C6D69052FEE99367736504FEF56F5B8C15321 |
| openxr_loader.dll | D60C68DCF66C8DAD31CA5F251DA6423C94631BBD7AD3B57F70A487E8C918C86A |
| mevr.ini | E5288480E69D395D88FEE622C7BBA7BF5842E64B58AE357E9A2EB162EA9C8A12 |

Source identity:
`28DBB7470987C8BAD59178C8DDCC244F8DB1F653ABC794DA2ED94D68CEBB22CA`.

Candidate directory: `dist/mevr-0.2.2-alpha-openxr-candidate/`.
ZIP: `package/mevr-0.2.2-alpha.zip`, containing independent build 2.
ZIP SHA-256:
`05186AF1BC2143310E159EB8C8704AEEA1FBB9C2DDFE2A666D35361BEB8AD1F6`.
Matching build-2 PDB SHA-256:
`1243791FC370F84D54F766F7F2B28CC3CC121AE76E53C724004B4DFCA7884DB8`.

Build 1 was installed into the development test game after confirming the game
was closed and the previous files still matched their backup. All three newly
installed files were then hashed and checked against the build-2 ZIP: exact
parity passed. Steam files were not installed or changed. The source snapshot,
42 source-input hashes, build logs/manifests, installed manifest, symbols and
candidate evidence are retained locally. The version remains 0.2.2-alpha in
accordance with the requested in-place update candidate.

The OpenXR diagnostic harness and VR-menu regression passed. ZIP validation
passed for exactly three top-level files, x86 DLLs, all 72 defaults and license
notices. The production settings loader confirmed all 72 effective settings
match the previous test INI. As in the original alpha, the generated INI spells
`SmoothTurnSpeed = 1.0` while the previously saved INI spells it `1`; the test
installation now has the exact generated candidate INI. Debug Overlay remains
off and Detailed Logging remains on. Static analysis retains the previously
documented advisories and reports none in the new diagnostic module.

**Awaiting headset approval of this new reproducible binary.** Automated checks
and installation parity are complete; neither establishes successful VR startup
with the new diagnostic hooks. The original published v0.2.2-alpha remains
unchanged until the new candidate is approved. Recheck the installed files and
downloaded replacement asset when publication resumes.
