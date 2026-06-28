# EyeTrackVR Calibration Overlay

Cross-platform **OpenXR + Vulkan** eye-tracking calibration overlay for EyeTrackVR.
Renders a follow-the-dot target (plus instruction text) in the headset and streams
capture signals to the EyeTrackVR Python app over localhost UDP.

Previously a Windows-only OpenVR/SteamVR overlay; now a standalone OpenXR
application that runs on any PC OpenXR runtime (SteamVR, WMR, Oculus, Monado) on
**Windows and Linux**.

## Why a standalone app (not an "overlay")

OpenXR has no portable overlay extension (`XR_EXTX_overlay` is Monado-only), so the
tool runs as a focused OpenXR session that owns the headset for the duration of a
calibration. Targets are placed using the runtime's authoritative per-eye field of
view (`xrLocateViews` → `XrFovf`): each eye's frustum is intersected with the
overlay plane and the two are overlapped to get the **binocular** region, centred on
the true optical axis. This guarantees every target is inside both eyes' visible
field and centred correctly — fixing the out-of-FOV / too-high/low targets some
users hit with the old build.

The dot and text are drawn as head-locked `XrCompositionLayerQuad` layers; Vulkan is
used only to upload two textures and blit them into the runtime's swapchains.

## Modes

```
EyeTrackVR-Overlay [mode] [--coverage=0.85] [--max-deg=40]
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
in `vcpkg.json` and installed automatically at configure time.

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

The build copies `assets/` (the dot image and bundled Open Sans font) and the
required runtime DLLs next to the executable. Launch with your OpenXR runtime
(e.g. SteamVR) active and the headset on.
