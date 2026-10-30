#!/usr/bin/env bash
# Build the Linux overlay runtime for this machine's architecture into dist/:
#   x86_64  -> dist/linux/        (build on Ubuntu 22.04: glibc 2.35)
#   aarch64 -> dist/linux-arm64/  (build on Ubuntu 24.04: Steam Frame)
#
#   bash build-dist.sh [repo-root]
#
# Needs: build-essential cmake ninja-build libvulkan-dev libopenxr-dev
#        libstb-dev patchelf git curl
# build-dist.bat runs this in WSL and in an arm64 Docker container.
#
# Builds the committed HEAD from a clean export on the native filesystem, so a
# Windows checkout's CRLF line endings and /mnt/c speed don't matter, and
# writes dist/<target>/SOURCE_TREE (see build-dist.bat).
set -euo pipefail

ROOT="$(cd "${1:-$(dirname "${BASH_SOURCE[0]}")}" && pwd)"
STAGE="$ROOT/build-dist"

case "$(uname -m)" in
    x86_64) TARGET="linux" ;;
    aarch64 | arm64) TARGET="linux-arm64" ;;
    *) echo "ERROR: unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
DEST="$ROOT/dist/$TARGET"

# OpenVR SDK matching EyeTrackVR's pyopenvr 2.12 bindings. external/OpenVR's
# older aarch64 build looks for its path registry in /data/work and so can't
# find SteamVR on a Steam Frame (VRInitError_Init_PathRegistryNotFound).
OPENVR_TAG="v2.12.14"
OPENVR_ARM64_SHA256="7fcdc649e5371c60530080c7ed3daef978daefd97304e1ff45b312aa344b3e76"

# build-dist.bat prepares the export with Windows git (a submodule checkout's
# .git points outside this folder, so git can't run in the container).
if [ "${ETVR_DIST_PREPARED:-}" != "1" ]; then
    GIT=(git -C "$ROOT" -c safe.directory='*' -c core.autocrlf=false -c core.eol=lf)
    if [ ! -f "$ROOT/external/OpenVR/headers/openvr.h" ]; then
        "${GIT[@]}" submodule update --init --depth 1 external/OpenVR
    fi
    mkdir -p "$STAGE"
    "${GIT[@]}" archive -o "$STAGE/src.tar" HEAD
    git -C "$ROOT/external/OpenVR" -c safe.directory='*' archive -o "$STAGE/openvr.tar" HEAD \
        headers lib/linux64 lib/linuxarm64 LICENSE
    INPUTS="$(tr -d '\r' < "$ROOT/dist/SOURCE_INPUTS")"
    # shellcheck disable=SC2086
    TREE="$("${GIT[@]}" ls-tree -r HEAD -- $INPUTS | git hash-object --stdin)"
    printf '%s\n' "$TREE" > "$STAGE/SOURCE_TREE"
fi

BUILD="${ETVR_OVERLAY_BUILD:-$HOME/etvr-overlay-build}"
echo "[dist] exporting sources to $BUILD..."
rm -rf "$BUILD"
mkdir -p "$BUILD/external/OpenVR"
tar -xf "$STAGE/src.tar" -C "$BUILD"
tar -xf "$STAGE/openvr.tar" -C "$BUILD/external/OpenVR"

echo "[dist] building for $TARGET..."
bash "$BUILD/build-linux.sh"
PACKAGE="$BUILD/build-linux/package"

if [ "$TARGET" = "linux-arm64" ]; then
    echo "[dist] replacing libopenvr_api.so with OpenVR $OPENVR_TAG..."
    curl -fsSL -o "$PACKAGE/libopenvr_api.so" \
        "https://raw.githubusercontent.com/ValveSoftware/openvr/$OPENVR_TAG/bin/linuxarm64/libopenvr_api.so"
    echo "$OPENVR_ARM64_SHA256  $PACKAGE/libopenvr_api.so" | sha256sum -c -
fi

echo "[dist] installing into dist/$TARGET..."
rm -rf "$DEST"
mkdir -p "$DEST"
cp -a "$PACKAGE/." "$DEST/"
cp "$STAGE/SOURCE_TREE" "$DEST/SOURCE_TREE"
echo "[dist] done: dist/$TARGET"
