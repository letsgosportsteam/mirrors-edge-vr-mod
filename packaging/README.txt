MIRROR'S EDGE VR - ALPHA

Native stereo rendering and 6-DOF head tracking for Mirror's Edge (2008), with
optional controller-driven hands, pistols, melee, locomotion, and parkour.
Hardware coverage is limited; motion interactions remain experimental.

INSTALLATION

1. Copy d3d9.dll, openxr_loader.dll, and mevr.ini into the game's Binaries
   folder, beside MirrorsEdge.exe.
2. Connect the headset through Virtual Desktop with VDXR selected as the
   OpenXR runtime, then launch the game normally.
3. Load a level for stereo gameplay. Hold Y for one second for VR settings.

Back up your mevr.ini before an update if you want to keep your settings.
To uninstall, remove the three mod files. The mod does not replace game assets;
automatic resolution can update the game's user settings. Do not edit the
installation's TdGame\Config files: the game checks their integrity.

REQUIREMENTS

Game:     Mirror's Edge (2008) for Windows, not Catalyst.
Headset:  Virtual Desktop with VDXR. Motion input uses the Touch layout.
Runtime:  Microsoft Visual C++ 2015-2022 Redistributable, x86:
          https://aka.ms/vs/17/release/vc_redist.x86.exe
          The x64 package alone is insufficient.
OS:       Windows 10 or 11.
Input:    Touch controllers, an XInput gamepad, or keyboard and mouse.

This build supports VDXR. SteamVR and Meta Link/Air Link are not supported by
this mod's current 32-bit OpenXR path. Start the Virtual Desktop stream before
the game. If the mod does not load, check the x86 redistributable first.

CONTROLS - TOUCH CONTROLLERS

Left controller:
  Thumbstick          Move / strafe
  Thumbstick click    Back / in-game menu, including the tutorial menu
  Trigger             Crouch / slide; fires a tracked left-hand pistol instead
  Grip                Jump with Hand tracking off; grip with it on
  X                   Reaction Time
  Y                   Short press: weapon action; hold 1 second: VR settings
  Menu                Native pause menu

Right controller:
  Thumbstick L/R      Turn; Smooth or Snap in VR settings
  Thumbstick up/down  With Hand tracking on: jump / quick-turn
                      With Hand tracking off: native look axis
  Thumbstick click    Weapon zoom, where supported
  Trigger             Attack / fire; tracked pistols use the holding hand's trigger
  Grip                Quick-turn / look behind with Hand tracking off;
                      grip with it on
  A                   Use / interact
  B                   Look at the game's point of interest

Center the stick between snaps or repeated jump/quick-turn inputs.
With a tracked left-hand pistol, the right trigger no longer fires it.

Motion features:
  Enable Hand tracking under Hands and movement. This tracks controllers,
  not bare hands. Hand tracking and arm swing both default to Off.
  Guns, Melee, and Motion parkour have separate switches.

  Pickup:  Look at an eligible pistol within 2 m and squeeze either grip.
           A blue box marks the selected pistol.
  Throw:   Release the holding grip while moving the controller. A pistol
           acquired with Y needs an initial squeeze to arm release-to-drop.
  Punch:   Hold a grip and thrust the unarmed fist; retract before repeating.
  Disarm:  Extend both hands and squeeze both grips together. The game's
           normal disarm conditions must be met.
  Run:     Swing your arms with Arm swing locomotion enabled.
  Jump:    Raise both hands overhead with Arm swing locomotion enabled.
  Crouch:  Enable Physical crouching in Comfort; also requires arm swing.
           Recenter while standing, then lower your head to crouch/slide.
  Parkour: Grip ledges/pipes and move hand over hand. Push down while gripping
           to pull up from a ledge. Grip a bar with both hands and pump
           forward/back to swing; release both grips to leave it.

Tracked pistols currently support Colt1911 and Glock18c. Other weapons retain
native handling. Contextual animations and eligibility checks remain game-owned.

Standard gamepads retain native bindings, with hold-Y for VR settings.
Auto switches input source on fresh activity; Gamepad mode suspends motion
features while keeping their preferences and headset tracking.

VR SETTINGS

Hold Y for one second to open or close. Either stick up/down selects a row;
left/right adjusts it. A or right trigger confirms; B goes back. B on the main
page or Done closes the panel. Release inputs and center sticks to resume play.
Settings save automatically to the active mevr.ini.

Hands and movement:       Hand tracking, arm swing, guns, melee, parkour
Comfort:                  Physical crouching, balance, camera locks, reticle
Graphics and performance: Frame cap, resolution, flares, FPS, UI size/height
Controllers and turning:  Auto/Quest/Gamepad, Smooth/Snap, speed and angle
Calibration and menu:     Recenter, gun alignment, panel size, restore defaults

Turn off the game's vertical sync. The default cap is 72 FPS. Pick a stable
cap matching or evenly dividing headset refresh, such as 60 FPS at 120 Hz.

Resolution defaults to Off, using the game's chosen resolution. Auto sizes a
16:9 frame from the headset's requested eye width and needs a restart. The
headset size is cached during a run for the next launch; changing the headset
or Virtual Desktop render scale also needs a restart. Higher resolution costs
performance. Resolution, lens flares, and D3D9Ex are restart settings.

Try Comfort's animation pitch/roll locks if camera motion is uncomfortable.
Release mevr.ini has Debug off. Deleting it can restore the development overlay.

Keyboard shortcuts:
  Page Up       Recenter (also available in Calibration and menu)
  Hold Pause    Exit the game cleanly
  F6            Rescan if stereo fails after a level loads

KNOWN LIMITATIONS

Motion interactions are experimental, and many animations take back control of
the arms. Stereo UI/effect fixes cover observed rendering paths; other scenes
may still need work. Glass reflections use one shared reflection image for both
eyes. Recent tutorial, reticle, and pipe-exit fixes need broader headset testing.
Performance depends on the scene and resolution.

REPORTING A BUG

https://github.com/letsgosportsteam/mirrors-edge-vr-mod/issues

Attach %LOCALAPPDATA%\MirrorsEdgeVR\mevr.log and include the mod version,
headset, runtime, controller type, motion settings, and reproduction steps.

Full instructions and development notes:
https://github.com/letsgosportsteam/mirrors-edge-vr-mod

LICENCE

MIT - see LICENSE.txt and THIRD-PARTY-NOTICES.txt.
Mirror's Edge is a trademark of Electronic Arts Inc. This project is not
affiliated with or endorsed by EA or DICE, contains no game code or assets,
and requires a legally obtained copy of the game.
