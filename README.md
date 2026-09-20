# Mirror's Edge VR

A VR mod for *Mirror's Edge* (2008), with native stereo rendering, 6-DOF head
tracking, motion controllers, and optional tracked hands, pistols, and parkour.

> **Alpha.** Hardware coverage is limited, and motion interactions are still experimental.

**[Download v0.2.2-alpha](https://github.com/letsgosportsteam/mirrors-edge-vr-mod/releases/tag/v0.2.2-alpha)**

## Installation

1. Download `mevr-0.2.2-alpha.zip` from the release's **Assets** section and extract it.
2. Copy these three files into the game's `Binaries` folder, beside `MirrorsEdge.exe`:

   ```text
   d3d9.dll
   openxr_loader.dll
   mevr.ini
   ```

3. Connect your headset through Virtual Desktop, with **VDXR** selected as the
   OpenXR runtime, then launch the game normally.

The ZIP contains exactly those three files, ready to copy with no INI editing.
The separate `.pdb` asset is for debugging and is not needed to play. License
notices are included as comments in `mevr.ini`.

On Steam, use **Manage > Browse local files** to find the game folder. Stereo
starts after a level loads; the startup screen is not a stereo gameplay view.

The release was tested in VR on Steam with the game's **PhysX disabled**. If
loading a chapter freezes, turn PhysX off in the game's settings. This was also
reported without the mod; the underlying cause remains unresolved.

**Hold Y for one second to open VR settings.** Changes save automatically.
Resolution defaults to **Auto**: launch once with the headset connected, then
quit and relaunch so the recorded headset size takes effect.
If gameplay stays flat after that, quit and relaunch once. The mod can load
successfully while the game selects a resolution that prevents stereo; this is
separate from injection failing. Keep the log if it repeats.
When updating, back up your existing `mevr.ini` if you want to keep your settings.

To uninstall, remove the three mod files. The mod does not replace game assets;
automatic resolution can update the game's user settings. Do not edit the
installation's `TdGame\Config` files: the game checks their integrity.

## Requirements

| | |
|---|---|
| Game | *Mirror's Edge* (2008) for Windows, the original game rather than *Catalyst*. |
| Headset | A headset supported by [Virtual Desktop](https://www.vrdesktop.net/), connected through **VDXR**. Motion input uses the Touch controller layout. |
| Runtime | [Microsoft Visual C++ 2015–2022 Redistributable **x86**](https://aka.ms/vs/17/release/vc_redist.x86.exe), required to run the mod. The x64 package alone is insufficient. |
| OS | Windows 10 or 11. |
| Input | Touch motion controllers, a standard XInput gamepad, or keyboard and mouse. |

**This build supports Virtual Desktop with VDXR.** SteamVR and Meta Link/Air Link
are not supported by this mod's current 32-bit OpenXR path.

If the game starts flat with no mod log, check that the x86 redistributable is
installed. If OpenXR cannot start, check that Virtual Desktop is already streaming.

## Features

| Feature | Current support |
|---|---|
| Stereo and head tracking | Separate eye views with positional and rotational tracking. |
| Controllers and turning | Touch input, automatic switching to an XInput gamepad, smooth or snap turning. |
| VR settings | In-headset panel, automatic saving, recentering, and restore defaults. |
| Game UI | Recognized HUD, menus, and tutorial prompts rendered in both eyes; adjustable size and height. |
| Motion hands | Optional controller-driven hands, with the game taking over during supported animations. |
| Pistols | Colt1911 and Glock18c follow either hand, with controller aiming, grip pickup/drop/throw, and per-hand calibration. |
| Melee and movement | Optional punch/disarm gestures, arm-swing running, hands-up jumping, and physical crouching. |
| Motion parkour | Grip interactions on ledges, pipes, and horizontal bars. |
| Graphics and comfort | Headset-based resolution option, frame cap, camera animation locks, lens-flare and reticle controls. |

**Motion hands and arm-swing locomotion default to On.** You can turn either off
under **Hands and movement**; Guns, Melee, and
Motion parkour have their own switches. Here, “hand tracking” means tracking the
controllers, not playing with bare hands.

## Controls

These are the Touch controller bindings. With the default Hand tracking setting,
the grips control your hands and right-stick up/down performs jump/quick-turn.
The tables also show the bindings with Hand tracking off. Keyboard/mouse
and standard gamepads retain the game's normal bindings, with the gamepad's Y
hold reserved for VR settings.

### Left controller

| Input | Action |
|---|---|
| Thumbstick | Move / strafe. |
| Thumbstick click | Back / in-game menu, including the tutorial menu. |
| Trigger | Crouch / slide. With a tracked pistol in the left hand, fires that pistol instead. |
| Grip | Jump with Hand tracking off; close the hand / grip with Hand tracking on. |
| X | Reaction Time. |
| Y | Weapon action on a short press; **hold one second to open or close VR settings**. |
| Menu | Native pause menu. |

### Right controller

| Input | Action |
|---|---|
| Thumbstick left/right | Turn; choose Smooth or Snap in VR settings. |
| Thumbstick up/down | With Hand tracking on: **up to jump, down to quick-turn**. Otherwise, the game's look axis. |
| Thumbstick click | Weapon zoom, where supported by the game. |
| Trigger | Attack / fire; a tracked pistol uses the trigger on its holding hand. |
| Grip | Quick-turn / look behind with Hand tracking off; close the hand / grip with Hand tracking on. |
| A | Use / interact. |
| B | Look at the game's point of interest. |

Center the right stick between snap turns or repeated jump/quick-turn inputs.
With a tracked pistol in the left hand, the right trigger no longer fires it.

### Motion interactions

These require Hand tracking and the relevant feature enabled in **Hands and movement**.

| Action | Gesture |
|---|---|
| Pick up a pistol | Look at an eligible pistol within 2 m, then squeeze either grip. A blue box marks the selected pistol. |
| Drop / throw | Release the holding grip; move the controller as you release to throw. A pistol acquired with the weapon button needs an initial grip squeeze to arm release-to-drop. |
| Punch | While unarmed, hold a grip and thrust that fist forward. Retract before the next punch. |
| Disarm | Extend both hands forward and squeeze both grips together while the game's normal disarm conditions are met. |
| Run | Swing your arms with Arm swing locomotion enabled. The movement stick remains available. |
| Jump | Raise both hands above your head with Arm swing locomotion enabled. |
| Crouch / slide | Lower your head with Physical crouching enabled under Comfort; this also requires arm swing. Recenter while standing. |
| Climb / shimmy | Grip a ledge or pipe and move hand over hand. Push down while gripping to pull up from a ledge. |
| Swing / release | Grip a horizontal bar with both hands and pump forward/back. Release both grips to leave the bar. |

The game still owns contextual animations and decides whether attacks, pickups,
and disarms are allowed. Other weapon types retain their native handling.

### VR settings and shortcuts

| Input | Action |
|---|---|
| Hold Y for one second | Open or close VR settings, on Touch or an XInput gamepad. |
| Either stick up/down | Select a settings row. |
| Either stick left/right | Change its value. |
| A or right trigger | Confirm. |
| B | Back; closes the panel from its main page. |
| Page Up | Recenter the view. Also available under Calibration and menu. |
| Hold Pause on the keyboard | Exit the game cleanly. |
| F6 | Rescan if stereo fails to start after loading a level. |

Release controls and center the sticks when leaving VR settings to resume gameplay.
**Controllers and turning > Auto** selects the active controller from fresh input.
Gamepad mode suspends motion features while keeping their preferences and headset tracking.

## Settings

Start with the in-headset panel. The main options are:

- **Hands and movement:** controller hand tracking, arm swing, guns, melee, and parkour.
- **Comfort:** physical crouching, head-tilt balance, animation locks, and reticle visibility.
- **Graphics and performance:** frame cap, resolution, lens flares, FPS display, and game UI size/height.
- **Controllers and turning:** input source, turning mode, smooth speed, and snap angle.
- **Calibration and menu:** recentering, left/right gun alignment, panel size/distance, and restore defaults.

Turn off the game's vertical sync. The default frame cap is **72 FPS**; choose a
cap your PC can hold that matches the headset refresh or divides it evenly.
For example, 60 FPS fits a 120 Hz headset. Unlimited is also available.

Resolution defaults to **Auto**, using a 16:9 render size based on the headset's
requested eye width. The first run records that size; quit and relaunch to apply
it. The headset size is cached during a run and used on the next launch;
changing headsets or Virtual Desktop render scale also needs a restart. Higher
resolution costs performance. Resolution, lens flares, and D3D9Ex are restart settings.
Choose **Off** to use the game's chosen resolution instead. Existing saved Off
or custom resolution settings are preserved when updating.

Auto offers the cached headset resolution as the game's only display mode,
preventing selection of a different aspect ratio. This correction is included
in v0.2.2-alpha, which was approved after Steam VR testing with PhysX disabled.

The game retains its camera animations by default. If landing dips or wall-run
roll are uncomfortable, try the pitch and roll locks under **Comfort**.

The v0.2.2-alpha release keeps the tuned pistol alignment and bar-swing settings,
with **Debug Overlay off** and **Detailed Logging on**;
the VR menu can still change your preferences and calibrate either pistol hand.

Settings live in `mevr.ini` beside the DLL. An existing
`%LOCALAPPDATA%\MirrorsEdgeVR\mevr.ini` is also supported; the file beside the DLL
takes priority. See [mevr.ini.example](mevr.ini.example) for advanced options.
The release, example and compiled fallback use **Debug = off**. Detailed Logging
is separate: hand, arm-swing and parkour logging stay enabled.

## Known limitations

- Motion hands, gestures, and parkour are experimental. The game takes control of
  the arms during many animations; tracked gun support is limited to the two pistols above.
- Stereo fixes for effects, UI, and glass target the rendering paths observed so
  far. Unseen materials or scenes may still need work. Glass reflections still
  use one game-rendered reflection image shared between the eyes.
- Recent tutorial-placement, reticle, and pipe-exit fixes have automated coverage
  but still need broader headset testing. See the [development notes](docs/README.md)
  for what was tested and what remains unverified.
- Performance varies by scene and render resolution; there is no guaranteed frame rate.

## Reporting a bug

[Open an issue](https://github.com/letsgosportsteam/mirrors-edge-vr-mod/issues) and attach
`%LOCALAPPDATA%\MirrorsEdgeVR\mevr.log`. Include the mod version, headset, runtime,
controller type, enabled motion features, and the level or action that reproduces it.

With Detailed Logging enabled, startup diagnostics also record the OpenXR loader,
32-bit runtime/layer registrations, manifest and DLL checks, and captured startup
errors in `mevr.log`. Keep the complete log when VR fails to start. OpenXR errors
include their names as well as numbers; a failed initialization requires a game
restart before VR can be tried again.

## Building

Requires Windows, Visual Studio with the C++ x86 toolset, and the
[OpenXR.Loader NuGet package](https://www.nuget.org/packages/OpenXR.Loader)
with `include\` and `native\Win32\release\{lib,bin}` available.

Copy `src\paths.local.ps1.example` to `src\paths.local.ps1` and set your local SDK
and game paths, or set `MEVR_OPENXR_SDK` and `MEVR_GAME_BIN` in the environment.
Use a separate development copy of the game for installation tests.

```powershell
.\src\build.ps1              # build x86 d3d9.dll and run static analysis
.\src\build.ps1 -Install     # also install to the configured game folder
.\tools\check-clean.ps1      # check for personal or machine-local information
```

For independently rebuilt candidates, use
[`tools/build-reproducible-candidate.ps1`](tools/build-reproducible-candidate.ps1).
It compiles twice and verifies all three install files match before packaging
build 2. Test and approve the candidate before publishing; preserve the approved
archive and settings afterward. See the [release workflow](docs/RELEASE_WORKFLOW.md).

## Development notes

| Location | Contents |
|---|---|
| [AGENTS.md](AGENTS.md) | Starting point for coding agents and documentation routing. |
| [ENGINE_NOTES.md](ENGINE_NOTES.md) | Measured engine internals, offsets, input bindings, and rendering behavior. |
| [FEASIBILITY.md](FEASIBILITY.md) | Original assessment, preserved as historical context. |
| [docs/README.md](docs/README.md) | Topic index for feature plans, investigations, fixes, and validation notes. |
| `src/` | The D3D9/OpenXR shim and its feature modules. |
| `tools/` | Regression harnesses, diagnostics, and publication checks. |
| `reference/` | Reference material from the Singularity shim; not compiled. |

Development notes record earlier experiments as well as current behavior. Check
the dates and newer follow-ups before treating an old result as a current limitation.

## Licence

MIT — see [LICENSE](LICENSE) and [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

*Mirror's Edge* is a trademark of Electronic Arts Inc. This project is not affiliated
with or endorsed by EA or DICE. It contains no game code or assets and requires a
legally obtained copy of the game.
