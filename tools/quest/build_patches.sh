#!/bin/bash
# Builds the MIPS patches with Zig's clang (LLVM 23 here has no Mips target) and
# recompiles them with N64Recomp.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
Z="C:/Users/QbAnYtO/tools/zig/ziglang/zig.exe"
export PATH="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:$PATH"
if ! make -C "$ROOT/patches" CC="$Z clang" LD="$Z ld.lld" -j8 > "$ROOT/out/build/patches_build.log" 2>&1; then
    grep -iE "error" "$ROOT/out/build/patches_build.log" | head
    exit 1
fi
cd "$ROOT" && ./N64Recomp.exe patches.toml | tail -1
