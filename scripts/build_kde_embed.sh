#!/usr/bin/env bash
# Build the KDE wallpaper kpackage zip with the embedded display module.
# The embed URI selects wrapper types that import the bundled private module.
# The resulting zip lands in ${BUILD_DIR}/waywallen-kde-*.zip.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${BUILD_DIR:-build}

cd "$REPO_ROOT"

cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DWAYWALLEN_DISPLAY_PLUGIN_QML=ON \
    -DWAYWALLEN_DISPLAY_QML_URI=Waywallen.DisplayEmbed

cmake --build "$BUILD_DIR" --target package
