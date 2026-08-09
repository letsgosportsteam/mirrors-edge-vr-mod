# Mirror's Edge VR

A VR mod for *Mirror's Edge* (2008, Unreal Engine 3.536). It is a `d3d9.dll` proxy: it
forwards every Direct3D 9 export to the real system library, and along the way renders the
game in stereo to an OpenXR headset with 6-DOF head tracking.

No game files are modified. Installing is copying three files into the game's `Binaries`
folder; uninstalling is deleting them.

> **Pre-alpha.** This is an early build shared to gather reports, not a finished mod. The
> HUD is broken in VR (see [Known issues](#known-issues)), and it has been tested on very
> few machines. Expect crashes.

---

## What works today

- **Native stereo rendering.** Both eyes are rendered by the engine with their own projection,
  per frame. This is not reprojection or a flat image on a floating screen.
- **6-DOF head tracking**, sampled inside the frame it is drawn for.
- **Touch controllers**, synthesised as an Xbox pad, so the game's own gamepad layout applies
  1:1 — or play with keyboard and mouse.

> ### 🎯 PAGE UP recentres the view — at any time
>
> A headset put on even slightly crooked leaves the world tilted, and it is the first thing
> that will bother you. **PAGE UP** puts the view back where you are facing, instantly, as
> often as you like. It works during play, in menus, and whether or not anything else is
> switched on.

## Requirements

> ### ⚠️ You must play through Virtual Desktop. There is no alternative.
>
> Mirror's Edge is a **32-bit** process, and almost nothing ships a 32-bit OpenXR runtime any
> more. This was measured, not assumed:
>
> - **VirtualDesktopXR (VDXR)** — works. The only one that does.
> - **SteamVR** — ships no 32-bit runtime at all. Index, Vive and every SteamVR-native headset
>   are out of reach for now.
> - **Meta / Oculus native (Link, Air Link)** — its 32-bit runtime crashes in `xrCreateSession`.
>
> So: a Quest or Pico, **[Virtual Desktop](https://www.vrdesktop.net/)**, with **VDXR set as
> your OpenXR runtime**, and the stream **already running before you launch the game**.

| | |
|---|---|
| Game | *Mirror's Edge* (2008), any store. The 32-bit original — **not** *Catalyst*. |
| Headset | A standalone headset streamed over Virtual Desktop, as above. Touch-style controllers; the mod suggests the `oculus/touch_controller` interaction profile, which is what a Quest reports through Virtual Desktop. |
| Runtime | [Microsoft Visual C++ 2015–2022 Redistributable — **x86**](https://aka.ms/vs/17/release/vc_redist.x86.exe). The x64 one you probably already have **does not** satisfy it; the game is a 32-bit process. |
| OS | Windows 10 or 11. |

If the x86 redistributable is missing, Windows silently declines to load the mod and the game
starts up flat with no error at all. It is the single most likely reason for "nothing happened".

If Virtual Desktop is not streaming when the game starts, the log says so in as many words:
`no OpenXR instance. Is the headset connected and Virtual Desktop streaming?`

## Set these before you play

Three to change before you start, and one to keep in your back pocket.

**Set the game's resolution as high as it goes, and keep it 16:9.** The mod does not take its
resolution from the headset yet — it splits the game's own frame down the middle, one half per
eye, so the game's resolution *is* your per-eye resolution. Keep the aspect at 16:9: anything
else is letterboxed, and the eye crop then has to assume the black bars are centred.

**A resolution change only takes effect after restarting the game**, so set it before you get
comfortable rather than partway through a session.

**Turn vertical sync off.** It paces the game to your flat monitor, which fights the frame cap
the mod uses to pace the headset.

**Set `FrameCap` in `mevr.ini` to a rate that divides your headset's refresh exactly** — 60 for
a 120 Hz headset, 72 for 144 Hz. Usually that means half. What matters is not the number but
the division: a frame held for two display periods *every* time looks smoother than a faster
rate held for two, then three. Uneven pacing shows up as judder, most obviously when you look
up and down.

**If you feel motion sick, try `LockAnimPitch` and `LockAnimRoll`.** Both ship off, so the
game's own camera animations — wall-run roll, landing dips, vaults — play as it intended.
Turning them on stops those animations moving your view on those axes, which may help a lot.
Worth reaching for if the first session is uncomfortable, rather than something to change
before you have played.

Leave `LockAnimYaw` off either way: an animation that turns the player is carrying them
somewhere, and cancelling it leaves your body facing one way and your view another.

## Controls

Keyboard and mouse work exactly as they always did. Motion controllers are synthesised as an
Xbox pad, so **the game's own default gamepad layout applies 1:1** — the mod remaps nothing, and
the in-game control list is accurate.

Two are worth knowing before you start, because neither is where a VR player would look:

| | |
|---|---|
| **Left grip** | **Jump.** The game puts jump on the left shoulder so both thumbs stay on the sticks; on a controller held in the fist, that lands on the grip. |
| **Left stick click** | **Back / in-game menu — you need this for the tutorial.** Press the left thumbstick in. It was the one live binding no physical control could otherwise reach, so the mod puts it here. |

Three keyboard keys stay live even though the mod is otherwise invisible:

| Key | |
|---|---|
| **PAGE UP** | Recentre the view. Use it whenever the headset was put on at an angle. |
| **PAUSE** (hold) | Quit cleanly, so the engine writes your save. |
| **F6** | Rescan for the view matrix, if stereo never came on. |

The in-game menus do not display correctly in the headset yet. Until that is fixed, glance at
the flat monitor to read them — the game is still rendering there normally.

## Install

1. Download `mevr-<version>.zip` from [Releases](https://github.com/letsgosportsteam/mirrors-edge-vr-mod/releases).
2. Find the game's `Binaries` folder. In Steam: right-click the game → **Manage** → **Browse
   local files**, then open `Binaries`. It is the folder containing `MirrorsEdge.exe`.
3. Copy `d3d9.dll`, `openxr_loader.dll` and `mevr.ini` into it, beside the exe.
4. **Start the Virtual Desktop stream first**, then launch the game normally.

Stereo arms itself a few seconds into the first loaded level — not at the menu. If it gives
up, it says so in the log and tells you to press **F6**.

### Uninstall

Delete those three files. Nothing else was touched, and no game file was modified.

> ⚠️ **Do not put anything in `TdGame\Config`.** Mirror's Edge hash-checks the files in that
> folder and refuses to start when they are edited. Nothing this mod needs goes anywhere
> near it.

## Configuration

`mevr.ini` sits beside `d3d9.dll`. It is read from `%LOCALAPPDATA%\MirrorsEdgeVR\` too, if
you would rather not put files in the game folder; beside the DLL wins if both exist.

Every line is echoed to the log at startup — applied, or rejected with the reason — so a
misspelt key is never silent. The file can only move settings the hotkeys could already
move; deleting it is always safe. See the comments in the file itself for each setting.

The settings that matter most for a first session — `FrameCap` and the animation locks — are
covered under [Set these before you play](#set-these-before-you-play).

Setting `Debug = on` restores the development overlay and roughly twenty diagnostic hotkeys.
Useful when investigating a bug, unpleasant to play with.

## Known issues

- **The HUD and in-game menus are split across the eyes and unreadable in the headset.** They
  arrive through the same 2D path as the game's full-screen post passes, and every filter
  tried so far separates the examples rather than the categories. The fix is to give the
  overlay its own render target and submit it as a second composition layer — substantial
  work, not a tweak. **Workaround: glance at the flat monitor**, which still shows the game
  normally.
- **Resolution is not taken from the headset.** The game's own frame is split per eye, so
  your in-game resolution sets your per-eye resolution. See
  [Set these before you play](#set-these-before-you-play).
- No weapon or hand anchoring; this is a stereo-camera mod, not a full VR conversion.
- Camera animations still move the view unless locked per axis in `mevr.ini`.
- Deleting `mevr.ini` entirely restores the compiled default of `Debug = on`, i.e. the
  development overlay comes back. Edit the file rather than deleting it.

## Planned

Roughly in the order they matter. Nothing here is a promise of a date.

**Toward alpha**

- A proper **VR settings menu**, instead of editing an ini by hand
- A **real fix for the in-game pop-up menus** — the second composition layer described above
- **Reposition the in-game UI** so it is readable at a comfortable distance in the headset
- **Full headset resolution**, rather than splitting the game's own frame per eye
- **Effects that render in one eye only** — currently they break the stereo
- **Misaligned sprites** (birds and similar) sitting at the wrong depth
- **Misaligned reflections**
- **Correct head-tracking yaw during in-game cinematics**
- **Stutter during the flat video cutscenes**
- **Snap turn**
- Possibly **basic hand tracking**, if it lands in time

**Toward beta**

- **Hand tracking**, in stages:

| Stage | |
|---|---|
| **Basic** | Hands are tracked, but the game takes control back during animations and gun use |
| **Guns** | Basic, plus hand tracking for weapons |
| **Advanced** | Parkour mechanics actually performed with the hands |

## Reporting a bug

Open an [issue](https://github.com/letsgosportsteam/mirrors-edge-vr-mod/issues) and attach
`%LOCALAPPDATA%\MirrorsEdgeVR\mevr.log`. That log is the entire diagnostic channel: it names
the version, the headset runtime, which `mevr.ini` it read, and what the startup sequence
was waiting for. A report without it is usually unactionable.

Say which headset and which OpenXR runtime, and whether the game reached a loaded level.

---

## Building

Windows, Visual Studio 2019 or later with the C++ x86 toolset, and the
[OpenXR.Loader NuGet package](https://www.nuget.org/packages/OpenXR.Loader) unpacked
somewhere (it must contain `include\` and `native\Win32\release\{lib,bin}`).

Nothing in this repository hardcodes a path. Point the build at your SDK and a **disposable**
copy of the game:

```powershell
Copy-Item src\paths.local.ps1.example src\paths.local.ps1   # then edit it
```

Or set `MEVR_OPENXR_SDK` and `MEVR_GAME_BIN` in the environment, which is what CI does.

```powershell
.\src\build.ps1              # build d3d9.dll
.\src\build.ps1 -Install     # build, then copy it to the configured game folder
.\src\build.ps1 -Package     # build, then stage a release zip in dist\
```

If PowerShell refuses with *"is not digitally signed"*, the machine's execution policy is
`AllSigned`. These scripts are locally created and carry no zone mark, so `RemoteSigned` is
enough — `Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned` (no admin
needed, and it still blocks unsigned scripts you download). For a one-off,
`powershell -ExecutionPolicy Bypass -File .\src\build.ps1` changes nothing persistently.

`-Install` writes to `$GameBin`. Point that at a copy of the game you don't mind breaking;
testing this means crashing it repeatedly.

The build is x86 only — the game is a 32-bit process — and runs `/analyze` over `d3d9.cpp`
before the real compile, failing on format-string defects and on any static function that is
defined but never called. Both have cost real debugging time on this project and neither is
caught by `/W4`.

### Repository layout

| | |
|---|---|
| `src/d3d9.cpp` | The whole mod, one file. Heavily commented with *why*, including the wrong turns. |
| `ENGINE_NOTES.md` | Everything measured about this build of UE3 — offsets, the object model, the view matrix, frame delivery. The most valuable file here for anyone modding this engine. |
| `FEASIBILITY.md` | The original assessment. |
| `third_party/minhook/` | Vendored [MinHook](https://github.com/TsudaKageyu/minhook), BSD-2. |
| `reference/` | The working VR shim from the Singularity mod, kept for study. Not compiled. |
| `tools/check-clean.ps1` | Refuses to push anything containing a machine-local or personal path. |

Development proceeds in "rungs" — each one a single question the game has to answer in a
headset before the next is attempted. The commit history is the ladder, and the notes record
what each rung actually measured rather than what was expected.

## Licence

MIT — see [LICENSE](LICENSE). Third-party components and their notices are listed in
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt); that file ships inside the release zip
too, because both licences involved require their notice to accompany binary redistributions.

*Mirror's Edge* is a trademark of Electronic Arts Inc. This project is not affiliated with or
endorsed by EA or DICE, contains no game code or assets, and does nothing without a legally
obtained copy of the game.
