# Development notes

Feature plans, investigations, and validation records live here. Start with the
topic below, then follow its dated evidence. These documents preserve earlier
failures and experiments; a historical limitation is not necessarily present in
the current build. Check newer notes and the source before acting on it.

For player instructions, see the [main README](../README.md). Coding agents should
read the root [AGENTS.md](../AGENTS.md). Engine measurements remain in
[ENGINE_NOTES.md](../ENGINE_NOTES.md), with the original assessment in
[FEASIBILITY.md](../FEASIBILITY.md).

## Settings, controllers, and UI

Release notes: [v0.2.0-alpha](RELEASE_v0.2.0-alpha.md).
Default-settings correction: [v0.2.1-alpha](RELEASE_v0.2.1-alpha.md).
Candidate VR approval and publication checks: [v0.2.2-alpha](RELEASE_v0.2.2-alpha.md).
Release/test comparison and pending fixes: [FOUR_RUNS_2026-09-19.md](FOUR_RUNS_2026-09-19.md).
Exact-file publication: [RELEASE_WORKFLOW.md](RELEASE_WORKFLOW.md).
Independent rebuild request and parity audit: [RELEASE_REBUILD_2026-09-19.md](RELEASE_REBUILD_2026-09-19.md).
Second four-run capture analysis: [RELEASE_PARITY_2026-09-19.md](RELEASE_PARITY_2026-09-19.md).
Missing-smoke investigation (tabled; also reported in flat mode, GOG/Steam difference unresolved): [SMOKE_RELEASE_2026-09-19.md](SMOKE_RELEASE_2026-09-19.md).
Steam PhysX freeze reports and optional local capture tools: [STEAM_PHYSX_2026-09-19.md](STEAM_PHYSX_2026-09-19.md).
OpenXR startup diagnostics and failure-capture validation: [OPENXR_DIAGNOSTICS_2026-09-20.md](OPENXR_DIAGNOSTICS_2026-09-20.md).

| Note | Scope |
|---|---|
| [VR_MENU.md](VR_MENU.md) | Settings pages, dependencies, persistence, pause ownership, and menu tests. |
| [CONTROLS_UI_2026-09-19.md](CONTROLS_UI_2026-09-19.md) | Standard gamepad support, Auto input, smooth/snap turning, and native UI scaling. |
| [TUTORIAL_POPUP_2026-09-19.md](TUTORIAL_POPUP_2026-09-19.md) | Tutorial grouping/placement and reticle control; includes the latest follow-up. |

## Hands, combat, and parkour

| Note | Scope |
|---|---|
| [PHASE1_MOTION_CONTROLS.md](PHASE1_MOTION_CONTROLS.md) | Original motion-hand implementation plan; historical, not a current feature checklist. |
| [ARM_SWING_LOCOMOTION.md](ARM_SWING_LOCOMOTION.md) | Locomotion measurements, gesture design, and validation plan. |
| [WEAPON_AND_MELEE.md](WEAPON_AND_MELEE.md) | Pistol aiming, melee, pickup, throwing, and calibration history. Current settings persistence is covered in VR_MENU.md. |
| [PICKUP_CAMERA_EFFECTS_2026-09-16.md](PICKUP_CAMERA_EFFECTS_2026-09-16.md) | Pickup discovery/catches, camera changes, and effect investigations. |
| [PICKUP_2026-09-17.md](PICKUP_2026-09-17.md) | Follow-up pickup targeting and bounds work. |
| [PICKUP_DEBUG_2026-09-17.md](PICKUP_DEBUG_2026-09-17.md) | Pickup diagnostics and visualization. |
| [PIPE_EXIT_2026-09-18.md](PIPE_EXIT_2026-09-18.md) | Authored pipe-top exits, animation handover, and regression coverage. |

## Rendering, camera, and performance

| Note | Scope |
|---|---|
| [STEREO_STARTUP_FIX.md](STEREO_STARTUP_FIX.md) | Stale render-target metadata and false mono fallback. |
| [PERFORMANCE_2026-09-16.md](PERFORMANCE_2026-09-16.md) | Measured stalls, background metadata discovery, and logging overhead. |
| [SMOKE_STEREO_2026-09-16.md](SMOKE_STEREO_2026-09-16.md) | Smoke/bird stereo and remaining validation limits. |
| [CULLING_SUN_2026-09-17.md](CULLING_SUN_2026-09-17.md) | Culling and sun investigation. |
| [FIVE_MARKERS_2026-09-17.md](FIVE_MARKERS_2026-09-17.md) | Five captured problem scenes and their rendering evidence. |
| [GLARE_CAMERA_2026-09-17.md](GLARE_CAMERA_2026-09-17.md) | Camera glare and view correction. |
| [SHADOW_LEDGE_2026-09-17.md](SHADOW_LEDGE_2026-09-17.md) | Shadow and ledge-camera follow-up. |
| [SUN_DOTS_MENU_2026-09-17.md](SUN_DOTS_MENU_2026-09-17.md) | Sun/dot artifacts and menu rendering. |
| [SUN_MENU_2026-09-17.md](SUN_MENU_2026-09-17.md) | Sun and stereo menu work. |
| [SUN_TRACE_2026-09-17.md](SUN_TRACE_2026-09-17.md) | Sun diagnostic tracing. |
| [TEXTURES_CAMERA_2026-09-17.md](TEXTURES_CAMERA_2026-09-17.md) | Texture effects and camera behavior. |
| [GLASS_REFLECTION_2026-09-18.md](GLASS_REFLECTION_2026-09-18.md) | Glass projection correction and menu follow-up; reflections remain shared between eyes. |

## Reading and adding evidence

Commands and paths such as `src/`, `tools/`, and `.analysis/` in these notes are
relative to the repository root, unless stated otherwise. Markdown links resolve
relative to the containing document. Captures in `.analysis/` are local, ignored
artifacts and are not distributed with this repository.

Keep new investigations here and add them to this index. Record what was observed,
what changed, which checks ran, and whether the result was verified in a headset.
Use dates and explicit follow-ups so older plans are not mistaken for current behavior.
