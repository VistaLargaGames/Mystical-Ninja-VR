#!/bin/bash
# Configures the Quest (Android arm64) build in out/build/android-quest.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="C:/Users/QbAnYtO/AppData/Local/Android/Sdk"
NDK="$SDK/ndk/29.0.14206865"
CMAKE="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
NINJA="C:/PROGRA~1/MICROS~4/2022/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/Ninja/ninja.exe"
"$CMAKE" -S "$(cygpath -m "$ROOT")" -B "$(cygpath -m "$ROOT")/out/build/android-quest" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release "$@"
