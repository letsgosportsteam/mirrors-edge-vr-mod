# v0.2.0-alpha

A new alpha with motion hands, parkour, pistol interaction, and in-headset
settings. The first public build was closer to a pre-alpha; this remains an
experimental release with limited hardware coverage.

## Install

Download **mevr-0.2.0-alpha.zip** from **Assets** below. Extract its three files
directly into the game's `Binaries` folder beside `MirrorsEdge.exe`:

```text
d3d9.dll
openxr_loader.dll
mevr.ini
```

No INI edits or extra mod files are needed. Start Virtual Desktop with **VDXR**
selected, connect the headset, then launch the game. After the first connected
run, quit and relaunch to apply the automatically recorded headset resolution.
Stereo gameplay starts after a level loads.

Requires the original *Mirror's Edge* (2008) for Windows, Virtual Desktop/VDXR,
and the [Visual C++ x86 runtime](https://aka.ms/vs/17/release/vc_redist.x86.exe).
SteamVR and Meta Link/Air Link are not supported by this build.

Upgrading: back up your old `mevr.ini`. Copying all three new files applies this
release's defaults. Keeping your old INI preserves its explicit settings.

## Changes since v0.1.0-alpha

- **VR settings panel:** hold Y for one second to open or close. Changes save
  automatically, with recentering, gun alignment, comfort settings, and defaults.
- **Motion hands and locomotion:** controller-driven hands, arm-swing running,
  hands-up jumping, physical crouching, and grip-based ledge/pipe/bar interactions.
- **Pistols and melee:** Colt1911 and Glock18c follow either hand, with controller
  aiming, grip pickup/drop/throw, punch and disarm gestures, and per-hand calibration.
- **Controllers and turning:** standard XInput gamepads, automatic input switching,
  smooth turning speed, and snap turning.
- **Stereo UI and rendering:** fixes for observed HUD/menu/tutorial paths, effects,
  glass projection, camera behavior, and stereo startup; adjustable game UI size/height.
- **Defaults:** motion hands and arm swing On, resolution Auto, Debug Off.

## Controls to know

| Input | Default action |
|---|---|
| Hold Y for one second | Open / close VR settings |
| Left stick | Move / strafe |
| Left stick click | Back / in-game tutorial menu |
| Right stick left/right | Turn |
| Right stick up/down | Jump / quick-turn with Hand tracking On |
| Grips | Close hands / grip / interact with supported pistols and parkour holds |
| Holding hand's trigger | Fire a tracked pistol |
| Page Up | Recenter; also available in VR settings |

In VR settings, use either stick to navigate/adjust, A or right trigger to
confirm, and B to go back. Release controls and center sticks to resume gameplay.
Full controls and setup details are in the
[README](https://github.com/letsgosportsteam/mirrors-edge-vr-mod/blob/v0.2.0-alpha/README.md).

## Known limitations

Motion interactions remain experimental; the game owns many contextual
animations. Tracked weapons currently support the two pistols listed above.
Rendering fixes target observed scenes and materials, and glass still uses a
shared reflection image for both eyes. Recent tutorial/reticle and pipe-exit
fixes need broader headset testing. Frame rate depends on scene and resolution.

The release is checked with the x86 build/static analysis, regression harnesses,
and archive validation. Those checks do not replace an in-headset playthrough.

Report problems through
[Issues](https://github.com/letsgosportsteam/mirrors-edge-vr-mod/issues), with
`%LOCALAPPDATA%\MirrorsEdgeVR\mevr.log`, your headset/runtime, input device,
settings, and reproduction steps.

The separate **d3d9-0.2.0-alpha.pdb** asset contains debugging symbols; it is not
needed to play. GitHub's automatic source-code downloads are for developers;
players should use **mevr-0.2.0-alpha.zip**. License notices are included as
comments in the packaged `mevr.ini`.
