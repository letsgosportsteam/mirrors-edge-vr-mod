# Sun flare dots and pause-menu material panel

The user confirmed the sun/glow alignment works. Remaining reports: dots extending from the sun, and the red pause-menu panel still spanning the eye boundary.

## Evidence

Preserved PID 34488 run and captures under `.analysis/sun-dots-menu-20260917/`:

- Marker 1: frame 11608, capture 11611, 373 draws / 9 pages, no truncation.
- Marker 2: frame 12336, capture 12339, 662 draws / 13 pages, no truncation.

The dot-chain draws use **E6318E88 / 6942F53D**, capture 11611 draws 293..311. They sample a 128x256 atlas, selected using half-width/quarter-height UV ranges and scalar atlas bounds. This is separate from the retained **E6318E88 / A1D87F1F** sun glow (draw 291, 512x512 starburst).

The red panel first appears in **DF014469 / 33DB2A77**, capture 12339 draw 645. The ordered before/after tiles directly show the red center strip being added by this draw. It runs through PrimitiveUP, depth disabled, standard alpha blending, on the full-size backbuffer. Its pixel shader samples four narrow UI mask textures (8x1024 and 32x1024), not a scene texture.

The panel uses the ordinary mesh vertex factory but uploads a Canvas projection to **c0..3**, with c3.w=2112. Menu text uses the recognized Canvas factory 5470E8CC and the corresponding projection at **c5..8**. The old UI path only recognized the text factory, so it left this material panel at full width.

## Changes

- `LensFlares=off` now also suppresses the exact E6318E88/6942F53D dot-atlas material pair. The working sun-glow draw remains enabled and retains its previous stereo correction. No global native-flare shutdown was enabled.
- The UI stereo helper recognizes DF014469/33DB2A77 specifically, validates its Canvas perspective projection at c0, and splits its viewport and scissor just like the existing text path.
- The exact shader pair, perspective shape, depth-disabled state, target dimensions, active stereo state and scene-texture safeguards exclude ordinary world meshes sharing DF014469.
- No user INI values or calibration data changed.

## Validation and deployment

Production-code tests pass for the actual captured panel projection/register, both eye viewports and clipping, restoration after a failed eye draw, rejection of shared sky/world shaders and depth-enabled meshes. Flare-filter tests confirm dots are suppressed with the setting off, restored when on, and the sun glow remains enabled. Existing sun/haze/shadow reconstruction tests passed. Mandatory static analysis and release build passed with existing advisory warnings.

Game was closed; previous DLL backed up to `.analysis/sun-dots-menu-20260917/d3d9-before.dll`. Installed build and hash verified:

`65F02E88FCA9570BDF3CAFDEA9D7799590ABA63118EF451435C689A6C3E0095C`

Headset verification of dot removal and red-panel alignment remains pending.
