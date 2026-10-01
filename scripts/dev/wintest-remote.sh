#!/usr/bin/env bash
# Runs on build-win under MSYS2 UCRT64. Called by scripts/dev/wintest.sh.
set -euo pipefail
REF="$1"; REGEX="$2"; shift 2
cd /c/build/Vibepollo
git fetch -q origin "$REF"
git checkout -q -f FETCH_HEAD
git -c submodule.third-party/libwebrtc.update=none \
    -c submodule.third-party/depot_tools.update=none \
    -c submodule.third-party/build-deps.update=none \
    -c submodule.packaging/linux/flatpak/deps/flatpak-builder-tools.update=none \
    -c submodule.packaging/linux/flatpak/deps/shared-modules.update=none \
    submodule update -q --init --recursive
git submodule update -q --init third-party/build-deps
git -C third-party/build-deps submodule update -q --init --depth 1 third-party/FFmpeg/Vulkan-Headers
export PATH="/c/Program Files/nodejs:$PATH"
if [ ! -f build-tests/build.ninja ]; then
  cmake -B build-tests -G Ninja -S . -DBUILD_TESTS=ON -DBUILD_DOCS=OFF -DBUILD_WERROR=OFF \
    -DCMAKE_BUILD_TYPE=Release -DSUNSHINE_ENABLE_WEBRTC=OFF \
    -DBUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER=OFF -DBUILD_VIRTUALDISPLAY_PROBE=OFF \
    -DBUILD_VIRTUALDISPLAY_TOOLS=OFF -DBUILD_VIRTUALDISPLAY_VULKAN_LAYER=OFF \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DSUNSHINE_NPM_EXECUTABLE="$(cygpath -m '/c/Program Files/nodejs/npm.cmd')" > /dev/null
fi
cmake --build build-tests --parallel 8 --target "$@"
ctest --test-dir build-tests -R "$REGEX" --output-on-failure
