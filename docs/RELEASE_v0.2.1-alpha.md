# v0.2.1-alpha — tested default settings

This corrects the default settings shipped in v0.2.0-alpha to match the installed
test build. Diagnostic logging is disabled. Gameplay code changes are limited to
default calibration, migration fallbacks, and accepting the existing bar-wrap value.

## Install / update

Download **mevr-0.2.1-alpha.zip** from **Assets**. Copy its three files into the
game's `Binaries` folder beside `MirrorsEdge.exe`:

```text
d3d9.dll
openxr_loader.dll
mevr.ini
```

Back up an existing INI before replacing it. Copy the included **mevr.ini** to
receive these corrected defaults; retaining an older file retains its values.
No manual INI editing is needed. Start Virtual Desktop with **VDXR** and launch
the game. Auto resolution needs one relaunch after the first connected run.

Requires the original *Mirror's Edge* (2008), Windows, Virtual Desktop/VDXR,
and the [Visual C++ x86 runtime](https://aka.ms/vs/17/release/vc_redist.x86.exe).
The separate PDB asset is for debugging and is not needed to play.

## Corrections

| Setting | v0.2.0-alpha | Tested defaults in v0.2.1-alpha |
|---|---|---|
| Left/right pistol wrist down | 0 degrees | 40 degrees |
| Left/right pistol wrist right | 0 degrees | -10 degrees |
| Left pistol right offset | 0 mm | 40 mm |
| Bar exit velocity multiplier | 1.0 | 1.5 |
| Bar hand travel for full swing input | 0.30 m | 0.15 m |

The bar-wrap fallback remains 63, as in the test build; the INI validator now
accepts that value rather than ignoring it. Restore Defaults restores the tuned
gun alignment too. Motion hands and arm swing remain On, resolution Auto,
frame cap 72, smooth turning, and Debug Off.

The installed INI and its startup log were checked. The test INI omits some keys,
so comparison uses the production settings loader in separate test processes to
include compiled fallbacks. Every example setting is covered, and the only
allowed differences disable diagnostics. The x86 build, settings/menu/controller
tests, gun-calibration tests, and archive checks validate this correction.
This is not a new in-headset playthrough.

See the [README](https://github.com/letsgosportsteam/mirrors-edge-vr-mod/blob/v0.2.1-alpha/README.md)
for controls and remaining alpha limitations, or the
[v0.2.0-alpha notes](RELEASE_v0.2.0-alpha.md) for the feature update.
