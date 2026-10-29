# AGENTS.md

This file provides guidance to coding agents when working with code in this repository.

## What this is

Eye-tracking calibration overlay for EyeTrackVR. Renders a follow-the-dot target (plus instruction text) in the headset and streams capture signals to the EyeTrackVR Python app over localhost UDP (big-endian int32 signals out on `127.0.0.1:2112`, interactive-mode commands in on `2113` — see the table at the top of `src/modes.cpp`, and `src/config.h` for the constants). The protocol, including per-point timing (appear 0.15 s → shrink 0.5 s → signal → hold 0.5 s), must stay byte- and timing-compatible: the Python side (`EyeTrackApp/data_collection.py`, `osc_calibrate_filter.py`) samples the instant a signal arrives and relies on the hold after it.

## Build & test

Requires CMake ≥ 3.21, C++20, vcpkg (Windows) or system packages (Linux, see `build-linux.sh`), and the OpenVR SDK submodule (`git submodule update --init` if `external/OpenVR` is empty).

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release -j --output-on-failure  # unit + protocol tests, no headset needed
```

`build/` (VS 2022) and `build-release/` exist and are configured. Core sources are globbed (`CONFIGURE_DEPENDS`) into the `etvr_core` static lib; the VR backends + `main.cpp` are listed explicitly in CMakeLists.txt. Run the binary from its output directory (`build/Release/EyeTrackVR-Overlay.exe`); post-build copies the font to `assets/` and the DLLs. Linux is verified in WSL `Ubuntu-22.04` (GCC 11, OpenXR 1.0.20 headers — stay within that API): `cmake -S . -B /root/etvr-build -G Ninja && cmake --build /root/etvr-build && (cd /root/etvr-build && ctest)`. Note `$?` does not survive the Git-Bash → wsl.exe hop; check exit codes inside Linux.

Running in a headset requires an active VR runtime. Without one, use the simulator: `--backend=sim --sim-hmd=beyond2 --sim-fast --sim-dump=<dir> --sim-debug` writes per-eye PNGs (debug draws the target ellipse and lens outline). The exe tees stdout (timestamped, flushed per line) to `<exe-dir>/eyetrackvr_overlay.log` (temp dir if not writable).

```
EyeTrackVR-Overlay [9point|center|ellipse|datacollect|finetune|interactive]
                   [--coverage=0.70] [--max-deg=30] [--max-deg-up=15] [--max-deg-down=40]
                   [--center-deg=0] [--backend=auto|openvr|openxr|sim] [--runtime=keep|<path.json>]
                   [--sim-hmd=..] [--sim-fast] [--sim-seconds=N] [--sim-dump=<dir>] [--sim-dump-every=N]
                   [--sim-eye-px=N] [--sim-debug] [--signal-port=N] [--cmd-port=N] [--no-parent-watch]
                   [--lang=en|de|es|ja|zh] [--list-langs]
```
The EyeTrackVR app launches `interactive --coverage=0.80 --max-deg=36 --max-deg-up=20 --max-deg-down=40`; other modes with defaults. Every launch appends `--lang=<app UI language>` (`EyeTrackApp/utils/overlay_launch.py`).

## Architecture

Pipeline per frame: **mode → `Scene` → Presenter → `Frame` → backend**, orchestrated by `Session` (`src/session.cpp`: warm-up until headset geometry is valid, then `on_start` (interactive sends 255 "ready"), run, fade-out outro).

- **Modes** (`src/modes.cpp`, `IMode`): state machines that own *where* the target is and *when* signals fire. They emit a `Scene` (`src/scene.h`): dot position/size, attention-ring size/centre (destination), `capture_hold`, instruction text, tone, progress. New modes: `make_mode()` + `is_mode_name()`. `InteractiveMode` queues commands; pass-start commands abandon a stale pass; `200` works anywhere.
- **Presenter** (`src/presenter.cpp`): everything cosmetic and deterministic — backdrop fade, dot pop-in, ring opacity, the motion trail (a rotated comet quad from where the dot was 0.28 s ago to the dot; reset on `Scene::dot_cut` so jumps never streak), and **instruction-panel layout**: slots (centre / lower / upper) verified visible to both eyes; the panel fades out whenever the dot, its destination or (for moving targets) its predicted position would overlap, and never starts appearing during `capture_hold`. Output is `Frame` (`src/frame.h`): backdrop grey+alpha and four quads (panel, ring, trail, dot; `Quad::rot` spins about the view axis) in final head-space metres. Depth order backdrop (3.03 m) > panel (3.015) > ring > trail > dot (3.0) matches composite order.
- **Backends** (`IOverlayBackend`, `src/backend.h`: `init → {poll_events, frame}* → shutdown`):
  - `OvrApp` (`src/ovr_app.cpp`) — OpenVR overlays, used whenever SteamVR is running (a focused OpenXR session would become the scene app and kill the running game). `VRApplication_Background`; 5-face backdrop cube (any FOV) + panel/ring/trail/dot overlays with PID-unique keys (stretched quads via `SetOverlayTexelAspect`); paced by `WaitFrameSync`; waits for/re-resolves HMD geometry on connect/IPD events. Windows + Linux x86_64 (`HAVE_OPENVR`), inert stubs elsewhere.
  - `XrApp` (`src/xr_app.cpp`, `vk_base.*`, `xr_vk.h`) — focused OpenXR + Vulkan session (Virtual Desktop native, Monado, …), or an overlay session on top of the running app with `XR_EXTX_overlay` (Monado, tried before OpenVR on Linux). API 1.0 (1.1 breaks some runtimes). Everything in VIEW space; one batched command buffer per frame; quad opacity applied on the CPU; optional `XR_KHR_visibility_mask`; graceful `xrRequestExitSession` on shutdown.
  - `SimApp` (`src/sim_app.cpp`, profiles in `src/sim_hmd.cpp`) — CPU renderer + synthetic headsets for tests/previews.
- **Geometry** (`src/fov.cpp/h`): backends describe each eye as `EyeGeom` (pose incl. cant, visible half-angle tangents, optional lens polygon in tangent space) → `compute_view_geometry()`: binocular box at `cfg::OVERLAY_DIST` (3 m), coverage + per-direction caps anchored at straight-ahead (normalised coords are training labels: ±1 = edge of range, `GazeRange::x()/y()` → metres), then the whole unit target ellipse is verified visible to both eyes (cardinal trims first, then a uniform scale) — on headsets where caps bind nothing changes. Implausible polygons are discarded. OpenVR traps in `OvrApp::compute_geometry`: `GetProjectionRaw`'s vertical pair is in image order (**GL-top = `pfBottom`**; decoded by sign so either labelling works — decoding `up = -pfTop` mirrors the FOV, ~20° on a Beyond 2); `GetEyeToHeadTransform` can carry real rotation; raw values are the *render* frustum, shrunk to the lens-visible region via `ComputeDistortion` on the panel's centre cross (sanity-limited); hidden-area line loop is render-target UV with v=0 at the top. Backends log per-eye FOV and the resolved range — check the log first when placement looks wrong.
- **Text/sprites** (`src/text.cpp`, `src/sprites.cpp`, `src/raster.h`): stb_truetype panel (UTF-8, kerning, lazy system fallback fonts, title/subtitle, pips/bar, success check) re-rasterised only when content changes; the dot, ring and trail are flat procedural AA sprites in the panel palette (no image assets).
- **Localisation** (`src/i18n.cpp`): built-in instruction text for every language in the app's `EyeTrackApp/lang/*.toml` (en, de, es, ja, zh), chosen by `--lang`. In interactive mode the app sends its own translated card text (50) *before* each voice prompt; pass/centre commands keep text that is already up instead of replacing it. Adding an app language: add a catalog here too (the app's `tests/test_overlay_localization.py` checks `--list-langs`).
- **Platform** (`src/platform.cpp`): exe dir, Ctrl+C/SIGTERM → graceful outro (second signal exits immediately), parent-process watchdog (interactive mode closes if the app dies), fallback fonts. `src/net.cpp`: exclusive bind of the command port, asks a leftover instance to quit (200) and retries.

Tests: `tests/tests.cpp` (unit; layout invariants run every mode × every sim headset × several cap configs) and `tests/protocol_test.py` (real exe over UDP). CTest registers each unit suite and layout shard separately (`etvr_tests <suite> [shard/shards]`; no arguments runs everything).
