# Performance investigation — 2026-09-16

Source: the 19:12:46 run, preserved locally in the ignored
`.analysis/performance-2026-09-16/latest.log`. The log ends through the PAUSE quit
shortcut around 523 seconds of its internal timer. Its DLL was built September 9
at 21:30:38. Headset refresh and the engine cap are both 72 Hz/FPS.

## Observations

| Window | Recorded delivery | Context |
| --- | --- | --- |
| Frames 1200 and 2400 | 39.6 / 40.1 FPS | Flat menu/loading submission, not sustained stereo gameplay |
| Frames 3000–10200 | 52.2–68.7 FPS across 13 reports | Stereo gameplay; some pacing-window maxima of several hundred milliseconds, up to 914.51 ms |
| Frames 11400–14400 | 71.8–72.0 FPS across six reports | Much steadier gameplay |
| Later gameplay | Frequently near 72 FPS, with further dips | Startup is worse, but not the only affected period |

The frame-capture mean was about 2.12 ms in the early gameplay windows and 2.27 ms
in the steadier six-window interval. Its averages do not explain the improvement;
they also do not rule out capture spikes, GPU stalls, streaming, or runtime pacing.
The render size was 4224x2376. No quality/resolution settings were reduced.

There were 20,670 muzzle-trace log entries and 38 barrel-aim entries. The trace
getter is called during ordinary updates, often twice per frame. Each Log call
opens the file, writes a line and CRLF, and closes it synchronously under a lock.
This is avoidable game-thread I/O; the log does not time its individual cost.
There were also 1,409 FOV diagnostic lines, many triggered by a count of shader
uploads rather than a count of rendered frames.

Pistol initialization also ran five full object-table function searches and a
property search on the game thread. The property search alone measured 1209.7 ms
over 87,493 objects. This is a definite blocking setup cost. The existing general
layout derivation took 15,869.4 ms on a background thread; that wall time is not
evidence of a 15.9-second game-thread freeze.

## Changes

- Run pistol function/property discovery in a background worker. Publish all
  metadata with an Interlocked state transition; the arm hook returns immediately
  while discovery is pending. Parameter/ABI checks, detour installation and the
  live getter test still execute on the game thread before enabling pistol control.
- Keep the first three muzzle-trace samples, then at most one per 600 frames.
  Preserve exact trace/aim counters and actual bullet processing on every call.
- Report FOV counts once per 600 frames, accumulating all uploads between reports.
- Log separate metadata-discovery and hook-installation durations. Add a summary
  every 600 arm updates of mod mean/max time, original game mean time, and total
  maximum. These measure that arm hook, not all mod or engine work.

The saved gun calibration and gameplay settings are preserved. This removes
identified overhead; the size of the FPS improvement requires a new headset run.

## Validation

The x86 build and required static analysis pass with existing advisory warnings.
`tools/test-combat-performance.ps1` compiles the production initialization and
logging functions with mocked engine access and a real worker thread. It checks
that arm updates return while metadata is blocked, only one worker/setup occurs,
hooks and getter calls stay on the main test thread, invalid metadata does not
enable hooks, and worker-creation failure does not cause repeated retries. Its
two-getters-per-frame scenario reduces 20,670 trace writes to 20 while retaining
all getter calls. The shipped-package property-layout check also passes.
