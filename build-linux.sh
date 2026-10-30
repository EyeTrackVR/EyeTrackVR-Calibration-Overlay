#!/usr/bin/env bash
# Ubuntu 22.04+: apt install build-essential cmake ninja-build libvulkan-dev libopenxr-dev libstb-dev patchelf
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build-linux"
PACKAGE="$BUILD/package"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DETVR_BUILD_TESTS=OFF
cmake --build "$BUILD" --parallel 2
rm -rf "$PACKAGE"
mkdir -p "$PACKAGE/assets" "$PACKAGE/licenses"
install -m 0755 "$BUILD/EyeTrackVR-Overlay" "$PACKAGE/"
install -m 0644 "$ROOT/external/font/OpenSans-Regular.ttf" "$PACKAGE/assets/"
# Keep loaders next to the executable; graphics drivers and glibc stay system-provided.
while IFS= read -r lib; do
    install -m 0644 "$lib" "$PACKAGE/$(basename "$lib")"
done < <(ldd "$BUILD/EyeTrackVR-Overlay" | awk '/lib(openvr_api|openxr_loader|jsoncpp|vulkan).* =>/ {print $3}')
for lib in "$PACKAGE"/*.so*; do
    # shellcheck disable=SC2016 # $ORIGIN is expanded by the dynamic linker, not the shell.
    patchelf --set-rpath '$ORIGIN' "$lib"
done
install -m 0644 "$ROOT/LICENSE" "$PACKAGE/licenses/overlay-MIT.txt"
install -m 0644 "$ROOT/external/OpenVR/LICENSE" "$PACKAGE/licenses/OpenVR-LICENSE.txt"
install -m 0644 "$ROOT/external/font/OFL.txt" "$PACKAGE/licenses/OpenSans-OFL.txt"
for name in libopenxr-loader1 libjsoncpp25 libvulkan1 libstb-dev; do
    notice="/usr/share/doc/$name/copyright"
    if [ -f "$notice" ]; then
        install -m 0644 "$notice" "$PACKAGE/licenses/$name-copyright.txt"
    fi
done
if ldd "$PACKAGE/EyeTrackVR-Overlay" | grep -q 'not found'; then
    echo 'ERROR: unresolved overlay runtime library' >&2
    exit 1
fi
echo "Linux runtime folder: $PACKAGE"
