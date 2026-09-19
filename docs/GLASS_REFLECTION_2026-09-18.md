# Glass projection and menu follow-up

The two Backspace markers in run 26084 (frames 57664 and 58892) captured the
same building-glass material: VS `8A7B075B`, PS `4577ECC3`. Draws 1070 and 1145
sample a separate 1280x720 reflection render target through sampler 4.

The original VS copies its clip position to both POSITION and TEXCOORD5. The
pixel shader divides TEXCOORD5 by W, applies ScreenPositionScaleBias, and samples
the reflection. Replacing the geometry's camera matrix for VR therefore also
changed the lookup into a reflection still captured with the native camera.

For that exact shader pair, the VS now computes TEXCOORD5 using the original
scene matrix in reserved constants c252..255. POSITION still uses the per-eye
matrix, including head movement and FOV. Other material outputs are unchanged.
The runtime guard requires a separate render-target texture in sampler 4 and
rejects packed scene targets. The shader and reserved constants are restored
after both eye draws. Cached shaders are released on device reset.

This corrects the projective mapping mismatch; the game still supplies one
reflection image shared by both eyes. It does not add per-eye reflection captures.
Perceived alignment while moving the headset needs an in-game check at this building.

`tools/test-glass-reflection.ps1 -CaptureDirectory <capture-folder>` executes the
captured original and rewritten bytecode for 100 input/matrix combinations,
checks that only TEXCOORD5 changes, and validates the production guard on a real
D3D9 device (shader creation, material and target checks, full state restoration).
It expects the captured `mevr-effect-8A7B075B.vs.bin` and
`mevr-effect-4577ECC3.ps.bin` files. The device portion needs desktop GPU access.

Hold Y for one second now closes the VR menu from any page. Release and neutral
gating prevents that press from reopening it or becoming a gameplay Y press.
The same run exposed two metadata failures: Actor's cooked Children list was
unavailable, and bSmoothFrameRate belongs to GameEngine, not Engine. Pause and
Unlimited now resolve their fields in one owner/type-validated GObjects walk on
the discovery thread. The menu tests cover those missing-chain and wrong-owner
cases, invalid bitmasks, pause ownership, and both Y hold directions.
