#!/bin/bash
# Build the ROF Vita port. Run from WSL: bash build.sh [Release|Debug]
# Uses VitaSDK-softfp (the game's .so uses the softfp float ABI).
set -e
export VITASDK=${VITASDK_SOFTFP:-$HOME/vitasdk-softfp}
export PATH=$VITASDK/bin:$PATH
TYPE=${1:-Release}
cd "$(dirname "$0")"
# Options are passed explicitly: CMake keeps old values in its cache otherwise.
cmake -S . -B build -DCMAKE_BUILD_TYPE=$TYPE -DDUMP_COMPILED_SHADERS=OFF -DUSE_SCELIBC_IO=OFF -DROF_LOG=ON > /dev/null
cmake --build build -j"$(nproc)"
ls -la build/*.vpk
