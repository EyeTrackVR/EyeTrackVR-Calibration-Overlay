# EyeTrackVR Calibration Overlay

Cross-platform **OpenXR + Vulkan** eye-tracking calibration overlay for EyeTrackVR.
Renders a follow-the-dot target (plus instruction text) in the headset and streams
capture signals to the EyeTrackVR Python app over localhost UDP.

Runs on any PC OpenXR runtime (SteamVR, WMR, Oculus, Monado) on **Windows and
Linux**, and uses a native **OpenVR overlay** on SteamVR so it never closes the
running game.

## Two backends (auto-selected)

On SteamVR a focused OpenXR session becomes *the* scene application, and SteamVR
allows only one — so launching an OpenXR calibration would terminate whatever game
is running (e.g. VRChat). To avoid that, the tool has two rendering backends and
picks the right one at startup:

- **OpenVR overlay** (`src/ovr_app.cpp`) — used whenever **SteamVR is already
  running**. The dot, instruction text and an opaque grey backdrop are drawn as
  `IVROverlay` overlays composited *on top of* the running game. The game keeps
  running underneath; quitting the calibration returns straight to it, with no
  relaunch. Attaches with `VRApplication_Background`, so it never launches SteamVR
  itself.
- **OpenXR + Vulkan** (`src/xr_app.cpp`) — used otherwise (native Virtual Desktop,
  standalone runtimes, Monado, etc.), where a focused session evicts nothing. The
  dot/text are head-locked `XrCompositionLayerQuad` layers over an opaque grey
  projection background.

Force a backend with `--backend=openvr` or `--backend=openxr` (the latter restores
the game-evicting focused-session behaviour on SteamVR).

Both backends place targets using the runtime's authoritative per-eye field of view
(`XrFovf`, or SteamVR's `GetProjectionRaw`): each eye's frustum is intersected with
the overlay plane and the two are overlapped to get the **binocular** region, centred
on the true optical axis. This guarantees every target is inside both eyes' visible
field and centred correctly.

## Modes

```
EyeTrackVR-Overlay [mode] [--coverage=0.85] [--max-deg=40] [--backend=openvr|openxr]
```

| mode          | behaviour                                                       |
|---------------|-----------------------------------------------------------------|
| *(default)*   | classic 9-point grid; signals point index 0–8, then 9           |
| `center`      | single centre point; signals 9                                  |
| `ellipse`     | orbit + inward spiral for dense ellipse-fit sampling            |
| `datacollect` | 9-point normal pass + 9-point squint pass                       |
| `interactive` | driven by the EyeTrackVR Python app over UDP                    |

- `--coverage=` (0–1): fraction of the binocular FOV the outermost targets reach.
- `--max-deg=` (0–90): hard cap on target eccentricity in degrees.

## UDP protocol

Unchanged from the OpenVR build — the Python side needs no changes. Big-endian
int32 signals are sent to `127.0.0.1:2112`; interactive mode receives commands on
`2113`. See the comments in `src/modes.cpp` for the full command/signal table.

## Building

Requires CMake ≥ 3.21, a C++20 compiler, and [vcpkg](https://github.com/microsoft/vcpkg).
Dependencies (`openxr-loader`, `vulkan-headers`, `vulkan-loader`, `stb`) are declared
in `vcpkg.json` and installed automatically at configure time. The OpenVR SDK used by
the overlay backend is vendored as a git submodule (`external/OpenVR`) — clone with
`--recurse-submodules` (or run `git submodule update --init`). On Windows the build
links it and copies `openvr_api.dll` next to the executable; on other platforms only
the OpenXR backend is built.

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

The build copies `assets/` (the dot image and bundled Open Sans font) and the
required runtime DLLs next to the executable. Launch with your OpenXR runtime
(e.g. SteamVR) active and the headset on.
