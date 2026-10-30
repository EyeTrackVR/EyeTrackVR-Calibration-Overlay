#!/usr/bin/env bash
# Checks out the pinned OpenVR SDK submodule with only the files the build
# uses: the headers and one platform's libraries (a few MB instead of the
# ~150 MB full checkout).
#
#   .github/scripts/fetch-openvr.sh <platform>    # linux64, linuxarm64 or win64
set -euo pipefail

platform=$1
path=external/OpenVR
commit=$(git rev-parse "HEAD:$path")
url=$(git config -f .gitmodules "submodule.$path.url")

git init -q "$path"
git -C "$path" sparse-checkout set headers "lib/$platform" "bin/$platform"
git -C "$path" fetch -q --depth 1 --filter=blob:none "$url" "$commit"
git -C "$path" checkout -q FETCH_HEAD
