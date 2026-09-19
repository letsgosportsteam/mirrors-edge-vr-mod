# In-headset settings

Hold **Y for one second** to open the settings panel. A short Y press retains the
game's weapon action, sent on release. The original Menu button is unchanged.

Use either stick up/down to select, left/right to adjust, A or the right trigger
to confirm, and B to go back. **Hold Y again for one second** from any page to
close. **Done**, or B on the main page, also closes the panel.
Release the buttons/triggers/grips and center both sticks before gameplay input
resumes. Headset tracking remains active throughout.

The mod requests the game's ordinary pause menu through a short Start pulse and
checks `WorldInfo.Pauser` to confirm the transition. It only resumes a pause it
created; an already-paused game stays paused. If the game refuses to pause (for
example, during a transition), the panel reports that instead of claiming success.
Controller gameplay input and gestures remain blocked while the panel is open.

## Pages

- **Hands and movement:** Hand tracking; Arm swing locomotion (includes hands-up
  jump); Guns; Melee; Motion parkour. The child selections are remembered when
  Hand tracking is off. The master also controls grip-to-fist and right-stick
  up/down jump/quick-turn bindings. Guns currently supports the tuned pistols.
- **Comfort:** Physical crouching (requires arm swing), head-tilt beam balance,
  animation pitch/roll/yaw locks, and Reticle On/Off (default Off). The reticle
  setting applies with either controller type and with hand tracking off.
  Parkour's own camera behavior stays with parkour.
- **Graphics and performance:** 30, 36, 60, **72 default**, 90, 120, 144, Unlimited;
  resolution; lens flares; FPS display; native game UI size and vertical position.
  Native UI defaults to 65% size, centered in each eye. Numeric INI caps 20–1000 remain supported.
- **Controllers and turning:** Auto, Quest, or Gamepad input; Smooth or Snap turning;
  smooth speed from 25–200% (100% preserves the existing game speed); snap angle
  from 15–90 degrees (45 default). Center the turning stick between snaps.
- **Calibration and menu:** Recenter; separate left/right gun alignment; panel
  width and distance; confirmed restore of the shipped defaults.
- **Troubleshooting:** Debug overlay, detailed logging, FastCapture and D3D9Ex.

Resolution, lens flares and D3D9Ex are saved for the next launch. Unlimited clears
the reflected engine `bSmoothFrameRate` bit; selecting a numeric cap restores that
bit and the existing `MaxSmoothedFrameRate` write. Unlimited is unavailable when
the property/mask cannot be resolved. OpenXR still paces submissions.

Standard XInput controllers work through the same settings panel, including hold
Y to open/close. Auto switches on fresh button or stick activity. Choose Gamepad
to keep Quest input from switching it back. While Gamepad is active, motion hand
features are suspended; their preferences are remembered for switching back to
Quest. Headset tracking remains active. A disconnected gamepad falls back to Quest.
Snap turning and the smooth-speed multiplier only apply during active gameplay.

## Persistence and presentation

Changes update the active `mevr.ini`: beside the DLL first, then the existing
LocalAppData fallback. Saves preserve comments, unknown options and duplicate-key
consistency. A same-directory temporary file is flushed and atomically replaces
the INI; failures are visible in the panel. Runtime gun calibration now uses the
same writer. Legacy `mevr-gun.ini` values load first, and explicit `GunLeft...` and
`GunRight...` keys in `mevr.ini` take precedence.

The restore image is generated from `mevr.ini.example` by `build.ps1`, with Debug
off just as in release packaging. Restore replaces the complete file, applies the
menu's live defaults, and requests a restart for remaining settings.

The green bitmap font is rendered on black into a dedicated 1200×1000 OpenXR quad
visible to both eyes, independent of game resolution. The starting width is
1.1 m, distance 1.5 m, and center 10 cm below eye level. The quad follows the head.
Width is adjustable from 0.7–1.6 m and distance from 0.8–2.5 m.

## Validation

Run `src/build.ps1`, then `tools/test-vr-menu.ps1`. The latter compiles the actual
mod implementation into a standalone test executable without launching the game
or an XR session. It exercises hold timing, focus loss, input neutralization,
pause ownership, cooked-object metadata resolution, preferences and dependencies, cap/INI roundtrips, legacy gun
values, camera-lock preservation, atomic-save failure, and restoring defaults.
It also renders all nine menu pages into `.analysis/vr-menu-tests` for inspection.
Run `tools/test-vr-controls.ps1` for hardware polling, source selection, native
controller input, turning and settings persistence, and `tools/test-screen-sampling.ps1`
for per-eye native UI viewport and clipping checks.

An in-headset pass is still required for perceived text size, the native pause
transition, controller bindings, and measured Unlimited behavior in this game.
