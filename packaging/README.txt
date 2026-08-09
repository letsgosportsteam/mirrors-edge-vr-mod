================================================================================
  MIRROR'S EDGE VR  -  pre-alpha
================================================================================

A VR mod for Mirror's Edge (2008). It renders the game in stereo to an OpenXR
headset with 6-DOF head tracking.

No game files are modified. Installing is copying three files in; uninstalling
is deleting them.

  *** PRE-ALPHA. The HUD is broken in VR - see KNOWN ISSUES below - and this
  *** has been tested on very few machines. Expect crashes.


  WHAT WORKS TODAY

    - Native stereo rendering. Both eyes are rendered by the engine with their
      own projection, every frame. Not reprojection, and not a flat image on a
      floating screen.
    - 6-DOF head tracking, sampled inside the frame it is drawn for.
    - Touch controllers, synthesised as an Xbox pad, so the game's own gamepad
      layout applies 1:1 - or play with keyboard and mouse.


  >>> PAGE UP RECENTRES THE VIEW, AT ANY TIME <<<

    A headset put on even slightly crooked leaves the world tilted, and it is
    the first thing that will bother you. PAGE UP puts the view back where you
    are facing, instantly, as often as you like. It works during play, in
    menus, and whether or not anything else is switched on.


--------------------------------------------------------------------------------
  BEFORE YOU START
--------------------------------------------------------------------------------

  ****************************************************************************
  *  YOU MUST PLAY THROUGH VIRTUAL DESKTOP. THERE IS NO ALTERNATIVE.         *
  ****************************************************************************

  Mirror's Edge is a 32-BIT process, and almost nothing ships a 32-bit OpenXR
  runtime any more. This was measured, not assumed:

      VirtualDesktopXR (VDXR)     works - the only one that does
      SteamVR                     ships NO 32-bit runtime at all. Index, Vive
                                  and other SteamVR-native headsets cannot run
                                  this yet.
      Meta / Oculus native        its 32-bit runtime crashes on startup
        (Link, Air Link)

  So: a Quest or Pico, Virtual Desktop (https://www.vrdesktop.net/), with VDXR
  set as your OpenXR runtime, and the stream ALREADY RUNNING before you launch
  the game.

  If it is not streaming when the game starts, the log says so in as many
  words: "no OpenXR instance. Is the headset connected and Virtual Desktop
  streaming?"


1. Mirror's Edge (2008), the 32-bit original. NOT Mirror's Edge Catalyst.

2. Virtual Desktop, streaming, with VDXR as your OpenXR runtime - see above.

3. The Microsoft Visual C++ 2015-2022 Redistributable, x86 version:

       https://aka.ms/vs/17/release/vc_redist.x86.exe

   READ THIS ONE TWICE. The x64 redistributable you almost certainly already
   have does NOT satisfy it - the game is a 32-bit process and needs the 32-bit
   runtime. If it is missing, Windows quietly declines to load the mod and the
   game starts flat with no error message of any kind. This is the single most
   common reason for "I installed it and nothing happened".


--------------------------------------------------------------------------------
  INSTALL
--------------------------------------------------------------------------------

1. Find the game's Binaries folder - the one containing MirrorsEdge.exe.

   On Steam: right-click the game, Manage, Browse local files, then open
   Binaries.

2. Copy these three files from this zip into that folder, beside the exe:

       d3d9.dll
       openxr_loader.dll
       mevr.ini

3. START THE VIRTUAL DESKTOP STREAM FIRST, then launch the game normally.

Stereo arms itself a few seconds into the first loaded level, not at the menu.
If it gives up it says so in the log and tells you to press F6.


  UNINSTALL

Delete those three files. Nothing else was touched.


  *** DO NOT PUT ANYTHING IN TdGame\Config ***

  Mirror's Edge hash-checks the files in that folder and refuses to start when
  they have been edited. Nothing this mod needs goes anywhere near it.


--------------------------------------------------------------------------------
  READ THIS FIRST - three to set now, and one to keep in your back pocket
--------------------------------------------------------------------------------

1. SET THE GAME'S RESOLUTION AS HIGH AS IT GOES, AND KEEP IT 16:9.

   The mod does not take its resolution from the headset yet. It splits the
   game's own frame down the middle, one half per eye - so the game's
   resolution IS your per-eye resolution. Keep the aspect at 16:9; anything
   else is letterboxed, and the eye crop then has to assume the black bars are
   centred.

   A RESOLUTION CHANGE ONLY TAKES EFFECT AFTER RESTARTING THE GAME. Set it
   before you get comfortable, not partway through a session.

2. TURN VERTICAL SYNC OFF.

   It paces the game to your flat monitor, which fights the frame cap the mod
   uses to pace the headset.

3. SET FrameCap IN mevr.ini TO A RATE THAT DIVIDES YOUR HEADSET'S REFRESH
   EXACTLY - 60 for a 120 Hz headset, 72 for 144 Hz. Usually that means half.

   What matters is not the number but the division. A frame held for two
   display periods EVERY time looks smoother than a faster rate held for two,
   then three. Uneven pacing shows up as judder, most obviously when you look
   up and down.

4. IF YOU FEEL MOTION SICK, TRY LockAnimPitch AND LockAnimRoll.

   Both ship OFF, so the game's own camera animations - wall-run roll, landing
   dips, vaults - play as it intended. Turning them on stops those animations
   moving your view on those axes, which may help a lot.

   Worth reaching for if the first session is uncomfortable, rather than
   something to change before you have played.

   Leave LockAnimYaw OFF either way. An animation that turns the player is
   carrying them somewhere, and cancelling it leaves your body facing one way
   and your view another.


--------------------------------------------------------------------------------
  CONTROLS
--------------------------------------------------------------------------------

Keyboard and mouse work exactly as they always did.

Motion controllers are synthesised as an Xbox pad, so the game's own default
gamepad layout applies 1:1 - the mod remaps nothing, and the in-game control
list is accurate.

Two are worth knowing before you start, because neither is where a VR player
would look for it:

    LEFT GRIP          Jump. The game puts jump on the left shoulder so both
                       thumbs stay on the sticks; on a controller held in the
                       fist, that lands on the grip.

    LEFT STICK CLICK   Back / in-game menu. Press the left thumbstick IN.

                       *** YOU NEED THIS FOR THE TUTORIAL. ***

                       It was the one live binding no physical control could
                       otherwise reach, so the mod puts it here.

Three keyboard keys stay live even though the mod is otherwise invisible:

    PAGE UP        Recentre the view. Use whenever the headset was put on at
                   an angle, which is most times.
    PAUSE (hold)   Quit cleanly, so the engine writes your save.
    F6             Rescan for the view matrix, if stereo never came on.


--------------------------------------------------------------------------------
  SETTINGS
--------------------------------------------------------------------------------

Open mevr.ini in any text editor. It documents itself; every line is echoed to
the log at startup, applied or rejected with a reason, so a typo is never
silent. Deleting the file is safe.

The settings that matter most for a first session - FrameCap and the animation
locks - are covered under READ THIS FIRST above.

Setting Debug = on restores a development overlay and about twenty diagnostic
hotkeys. Useful when reporting a bug, unpleasant to play with.


--------------------------------------------------------------------------------
  KNOWN ISSUES
--------------------------------------------------------------------------------

* THE HUD AND IN-GAME MENUS ARE NOT DISPLAYED CORRECTLY IN THE HEADSET. They
  arrive through the same 2D drawing path as the game's full-screen post
  passes, and separating them properly means giving the overlay its own render
  target - substantial work, not a tweak.

  WORKAROUND: glance at the flat monitor. The game is still rendering there
  normally, so the menus are perfectly readable on it.

* RESOLUTION IS NOT TAKEN FROM THE HEADSET. The game's own frame is split per
  eye, so your in-game resolution sets your per-eye resolution. See READ THIS
  FIRST above.

* No weapon or hand anchoring. This is a stereo-camera mod, not a full VR
  conversion.

* Camera animations still move the view unless locked per axis in mevr.ini.

* Deleting mevr.ini restores the compiled default of Debug = on, so the
  development overlay comes back. Edit the file rather than deleting it.


--------------------------------------------------------------------------------
  REPORTING A BUG
--------------------------------------------------------------------------------

    https://github.com/letsgosportsteam/mirrors-edge-vr-mod/issues

ATTACH THE LOG. It is at:

    %LOCALAPPDATA%\MirrorsEdgeVR\mevr.log

Paste that path into the address bar of a File Explorer window and it will take
you there.

That log is the entire diagnostic channel - it names the mod version, the
headset runtime, which mevr.ini it read, and what the startup sequence was
waiting for when things went wrong. A report without it usually cannot be acted
on at all.

Also say which headset and which OpenXR runtime you are using, and whether the
game reached a loaded level or failed earlier.


--------------------------------------------------------------------------------
  LICENCE
--------------------------------------------------------------------------------

MIT - see LICENSE.txt. Third-party components are listed in
THIRD-PARTY-NOTICES.txt.

Mirror's Edge is a trademark of Electronic Arts Inc. This project is not
affiliated with or endorsed by EA or DICE, contains no game code or assets, and
does nothing without a legally obtained copy of the game.
