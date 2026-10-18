#!/bin/bash
# Builds libmain.so, strips it, gathers what Gradle packages and builds the APK.
#   tools/quest/package.sh            # ninja + strip + gradle
#   tools/quest/package.sh --no-ninja # repackage only
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/out/build/android-quest"
SDK="C:/Users/QbAnYtO/AppData/Local/Android/Sdk"
NDK="$SDK/ndk/29.0.14206865"
NINJA="C:/PROGRA~1/MICROS~4/2022/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/Ninja/ninja.exe"

if [ "$1" != "--no-ninja" ]; then
    (cd "$BUILD" && "$NINJA" -j 6 Goemon64Recompiled)
fi

mkdir -p "$BUILD/jniLibs/arm64-v8a" "$BUILD/apk-assets" "$BUILD/sdl-java"
"$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe" --strip-unneeded \
    -o "$BUILD/jniLibs/arm64-v8a/libmain.so" "$BUILD/libmain.so"
cp -f "$BUILD/libSDL2.so" "$BUILD/jniLibs/arm64-v8a/libSDL2.so"
"$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe" --strip-unneeded \
    -o "$BUILD/jniLibs/arm64-v8a/libSDL2.so" "$BUILD/libSDL2.so"
cp -f "$NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$BUILD/jniLibs/arm64-v8a/"

# Game UI assets (fonts, menus) and the controller database, extracted on first start.
rm -rf "$BUILD/apk-assets/assets"
cp -r "$ROOT/assets" "$BUILD/apk-assets/assets"
rm -rf "$BUILD/apk-assets/assets/scss"
cp -f "$ROOT/recompcontrollerdb.txt" "$BUILD/apk-assets/"

rm -rf "$BUILD/sdl-java/org"
cp -r "$BUILD/_deps/sdl2-src/android-project/app/src/main/java/org" "$BUILD/sdl-java/"

cd "$ROOT/android"
JAVA_HOME="C:/Program Files/Android/Android Studio/jbr" ANDROID_HOME="$SDK" \
    "$HOME/.grok/gradle/gradle-8.9/bin/gradle" assembleRelease --no-daemon --console=plain -q
ls -la "$ROOT/android/app/build/outputs/apk/release/app-release.apk"
