# Steam chapter-load freezes — September 19, 2026

The user reported chapter-load freezes in Steam, forcibly closed the game, and
subsequently reproduced the problem without the mod and after reinstalling the
game. They reported that disabling the game's PhysX setting avoids the freeze,
then approved the v0.2.2-alpha candidate after Steam VR testing with PhysX off.
This narrows the reproduction conditions but does not identify a blocked
function or explain why PhysX previously worked. No PhysX fix was made to the mod.
The missing-smoke investigation remains [tabled](SMOKE_RELEASE_2026-09-19.md).

Earlier candidate logs showed the correct headset-sized backbuffer and chapter
loading, but did not establish the cause of the freeze. A separate development
installation crash report occurred during shutdown in the VR runtime; it must
not be treated as evidence of the Steam chapter-load hang's cause.

## Optional local capture tools

`tools/capture-hang.cpp` is an external x86 minidump helper, not injected code.
It accepts a game PID and a new output filename and limits targets to
`MirrorsEdge.exe` or its own self-test process. Build it in an x86 MSVC developer
shell with `cl /EHsc /W4 /std:c++17 tools\capture-hang.cpp dbghelp.lib`, directing
the EXE and object into ignored `.analysis/hang-capture-tools/`. The helper was
compiled and successfully tested against its own child process; no Steam hang
dump was captured during this investigation.

`tools/watch-game-hang.ps1 -GameDirectory '<game>\Binaries' -OutputDirectory
'.analysis/new-hang-capture'` can be started manually before reproducing a hang.
It watches only the specified executable, records mod hashes, and attempts two
dumps five seconds apart after an unresponsive window or stalled current log.
These are capture heuristics, not proof of a hang. The watcher does not launch or
terminate the game or alter installed files. Its default watch window is twenty
minutes. Keep dumps and logs local; they may contain game-derived memory and
machine-specific information. The script passed syntax validation but has not
been exercised against an actual game hang. No watcher was started for this
release, and further PhysX investigation is paused.
