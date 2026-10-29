# EyeTrackVR Calibration Overlay

Cross-platform **OpenXR + Vulkan** eye-tracking calibration overlay for EyeTrackVR.
Renders a follow-the-dot target (plus instruction text) in the headset and streams
capture signals to the EyeTrackVR Python app over localhost UDP.

Runs on any PC OpenXR runtime (SteamVR, WMR, Oculus, Virtual Desktop, Monado) on
**Windows and Linux**, and uses a native **OpenVR overlay** on SteamVR so it never
closes the running game.

## Backends (auto-selected)

On SteamVR a focused OpenXR session becomes *the* scene application, and SteamVR
allows only one — so launching an OpenXR calibration would terminate whatever game
is running (e.g. VRChat). To avoid that, the tool picks a rendering backend at
startup:

- **OpenVR overlay** (`src/ovr_app.cpp`) — used whenever **SteamVR is already
  running**. The target, instruction panel and an opaque grey backdrop are drawn as
  `IVROverlay` overlays composited *on top of* the running game. The game keeps
  running underneath; quitting the calibration fades back to it, with no relaunch.
  Attaches with `VRApplication_Background`, so it never launches SteamVR itself.
- **OpenXR + Vulkan** (`src/xr_app.cpp`) — used otherwise (native Virtual Desktop,
  standalone runtimes, Monado, etc.), where a focused session evicts nothing. The
  target/panel are head-locked `XrCompositionLayerQuad` layers over an opaque grey
  projection background. With `XR_EXTX_overlay` (e.g. Monado) it runs as an
  overlay session on top of the running app, and on Linux it is tried before OpenVR.
- **Simulator** (`src/sim_app.cpp`, `--backend=sim`) — headless software renderer
  with a synthetic headset. Runs the full session and UDP protocol without VR
  hardware and can dump what each eye sees as PNGs. Used by the tests and for
  previewing layouts.

Force a backend with `--backend=openvr|openxr|sim` (`openxr` restores the
game-evicting focused-session behaviour on SteamVR).

## Target placement and FOV

All backends describe the headset as two eyes (pose + visible frustum + optional
lens-visibility polygon) and share one policy (`src/fov.cpp`):

1. Each eye's frustum is intersected with the overlay plane (3 m) and the two are
   overlapped to get the **binocular** region; `--coverage` and the per-direction
   eccentricity caps (`--max-deg*`) bound it, anchored on straight-ahead.
2. The whole normalised target ellipse is then **checked against what each eye can
   actually see** — the OpenVR hidden-area mesh or the OpenXR `XR_KHR_visibility_mask`
   when the runtime provides one, the frustum otherwise. Only directions that would
   clip are trimmed, so on headsets where the caps bind (the normal case) target
   labels are unchanged; on narrow or round-lens headsets no target lands outside
   the lens any more. Implausible masks are rejected rather than trusted.

SteamVR specifics: `GetProjectionRaw` is decoded by sign (robust to drivers that
label the vertical pair either way), the render frustum is shrunk to the
lens-visible region through `ComputeDistortion` (with sanity limits), and canted
eye poses are honoured. Every run logs per-eye FOV, the resolved range and any
trimming to `eyetrackvr_overlay.log`.

## Look and flow

- Procedurally drawn, anti-aliased, flat target in the app's violet: a dot with a
  dark centre ring and white fixation pip, and a ring that converges onto it while
  the eye settles (the "shrinking dot" of the original build). The dot **glides**
  from one point to the next instead of popping, so it is never lost at the edge
  of the field, and leaves a short fading **trail** while it moves (gone before
  any capture of a stationary target).
- Instruction text in the EyeTrackVR app's language (English, German, Spanish,
  Japanese, Chinese); the app passes `--lang=<code>` when it launches the overlay.
- A compact instruction card (bold title, lighter subtitle, progress pips/bar,
  success state) that is laid out per headset: centred when no target is shown,
  otherwise below (or above) the target field — always fully visible to both eyes,
  and it **moves out of the way whenever a target would overlap it**. It never
  appears or moves while a point is being captured.
- Backdrop fades in at start and out at the end; everything is depth-ordered
  (backdrop → card → target) so no compositor can draw the card over the dot.

Timings and signals are identical to the previous build — only presentation changed.

## Modes

```
EyeTrackVR-Overlay [9point|center|ellipse|datacollect|finetune|interactive]
                   [--coverage=0.70] [--max-deg=30] [--max-deg-up=15] [--max-deg-down=40]
                   [--center-deg=0] [--backend=auto|openvr|openxr|sim] [--runtime=keep|<path.json>]
```

| mode          | behaviour                                                       |
|---------------|-----------------------------------------------------------------|
| *(default)*   | classic 9-point grid; signals point index 0–8, then 9           |
| `center`      | single centre point; signals 9                                  |
| `ellipse`     | orbit + inward spiral for dense ellipse-fit sampling; 0 … 9     |
| `datacollect` | 9-point normal pass + 9-point squint pass                       |
| `finetune`    | 11-point two-ring fine-tune; signals 0–10, then 100             |
| `interactive` | driven by the EyeTrackVR Python app over UDP                    |

- `--coverage=` (0–1): fraction of the binocular FOV the outermost targets reach.
- `--max-deg=`, `--max-deg-up=`, `--max-deg-down=`: eccentricity caps in degrees.
- `--center-deg=`: elevation of the centre target.
- Simulator: `--sim-hmd=beyond2|index|quest3|pimax|vive|canted|narrow|generic`,
  `--sim-fast`, `--sim-seconds=N`, `--sim-dump=<dir>`, `--sim-dump-every=N`,
  `--sim-eye-px=N`, `--sim-debug` (draws the target ellipse and lens outline).
- Misc: `--lang=en|de|es|ja|zh` (unknown codes fall back to English), `--list-langs`,
  `--signal-port=`, `--cmd-port=` (tests only), `--no-parent-watch`, `--version`.

## UDP protocol

Unchanged — the Python side needs no changes. Big-endian int32 signals are sent to
`127.0.0.1:2112`; interactive mode receives commands on `2113`. The full
command/signal table is at the top of `src/modes.cpp`.

Robustness in interactive mode: commands are queued in order instead of dropped;
`200` (quit) works at any time; if the app starts a new pass while an old one is
still running (e.g. after an error on its side) the stale pass is abandoned instead
of the app waiting forever; non-finite coordinates are ignored. The command port is
bound exclusively, and a leftover overlay instance holding it is asked to quit. If
the launching app exits or crashes, an interactive overlay closes itself rather than
leaving the user behind a grey backdrop. Ctrl+C / SIGTERM fade out and tear down
cleanly.

## Building

Requires CMake ≥ 3.21, a C++20 compiler, and [vcpkg](https://github.com/microsoft/vcpkg)
on Windows. Dependencies (`openxr-loader`, `vulkan-headers`, `vulkan-loader`, `stb`)
are declared in `vcpkg.json` and installed automatically at configure time. The
OpenVR SDK used by the overlay backend is vendored as a git submodule
(`external/OpenVR`) — clone with `--recurse-submodules` (or run
`git submodule update --init`). On Windows and Linux x86_64 both the OpenVR overlay
and the OpenXR backend are built.

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

The build copies `assets/` (the bundled Open Sans font) and the required runtime
DLLs next to the executable. The target is drawn procedurally, so no image assets
are needed. Text uses Open Sans, falling back to system fonts for glyphs it lacks
(CJK etc.).

### Tests

```sh
ctest --test-dir build -C Release -j --output-on-failure
```

- `unit.*` (`tests/tests.cpp`, one test per suite plus shards of the layout
  sweep): geometry/visibility across synthetic headsets and
  cap configurations, exact signal order and timing of every mode, the
  interactive command protocol (including recovery paths), presenter layout
  invariants (the card never covers a target, never appears during a capture, is
  always fully visible), text/UTF-8, and the session lifecycle.
- `protocol` (`tests/protocol_test.py`): runs the real executable on the simulator
  and talks to it over UDP like the EyeTrackVR app does, plus stale-instance
  takeover, parent-exit watchdog and SIGTERM shutdown.

Preview what a headset would show:

```sh
EyeTrackVR-Overlay finetune --backend=sim --sim-fast --sim-hmd=beyond2 --sim-dump=frames --sim-debug
```

### Linux runtime folder

On Ubuntu 22.04 or newer:

```sh
sudo apt-get install build-essential cmake ninja-build libvulkan-dev libopenxr-dev libstb-dev patchelf
bash build-linux.sh
```

Copy the contents of `build-linux/package/` into the app repository's
`EyeTrackApp/Tools/linux/`. This folder includes the executable, `.so` loaders,
assets and license notices. Build on Ubuntu 22.04 for a glibc 2.35 baseline.
The headset's SteamVR/OpenXR runtime and Vulkan graphics driver are required.
