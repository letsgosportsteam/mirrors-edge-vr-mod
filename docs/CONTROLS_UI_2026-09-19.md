# Native UI, turning, and standard controller support

Installed DLL SHA256: `52504EB1896DCA8F185184A3FC7E7801823B66315A5651527D3EA1001B0E6DA7`.
Previous DLL/example are in `.analysis/controls-20260919/installed-backup-20260919-074821`.
The live game `mevr.ini` was preserved and its hash checked after installation.

Before this change, the managed XInput hook always replaced player zero with the
Quest-generated packet. A connected standard controller could therefore be hidden.
The mod now polls the original XInput trampoline, maps the selected connected pad
to player zero, and exposes Auto / Quest / Gamepad in Controllers and turning.
Auto changes source on fresh activity; a held Quest grip cannot steal input back.
Gamepad mode suspends motion gestures and arm overrides without erasing preferences.
The same hold-Y panel and input quarantine apply to either source.

Smooth turning defaults to 100% of the live native PlayerInput.LookRightScale.
The multiplier is 25–200%, is never compounded, and restores the baseline outside
gameplay. The declaring owners and native use were checked against locally
decompiled PlayerInput and TdPlayerInput. Snap defaults to 45 degrees, allows
15–90 degrees, and requires a centered stick between turns. It is suppressed in
the mod menu, native pause, lost focus, and scripted camera states. Snap yaw changes
the world reference without entering the accumulated headset delta correction.

Recognized native Canvas UI now scales to 65%, centered within each eye, with
matching scissor transforms. Graphics and performance exposes size (40–100%) and
vertical position. This affects native HUD/menu/tutorial Canvas; it does not resize
the separate OpenXR settings panel. The existing shader and render-target guards
remain in place.

Validation: x86 release build with analysis succeeded; VR controls, VR menu,
screen sampling, and pipe exit tests passed. All nine production menu page images
were rendered; root, graphics, and controller pages were visually checked after
adding percentage glyph support. New settings passed active-INI roundtrips and
shipped-default restoration. The standard controller tests use a fake XInput pad
on slot 2 and exercise the actual production hook/helpers.

Still requires an actual headset/gamepad pass: training popup framing, native
LookRightScale resolution in a running game, perceived turning speed, and snap
behavior during gameplay. Automated checks do not establish hardware usability.
