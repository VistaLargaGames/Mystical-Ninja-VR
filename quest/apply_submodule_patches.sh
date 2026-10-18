#!/bin/bash
# Applies the Quest port's changes to the submodules (checked out at their upstream
# commits by `git clone --recursive` or `git submodule update --init --recursive`).
set -e
cd "$(dirname "$0")/.."
P="$(pwd)/quest/submodule-patches"
git submodule update --init --recursive
apply() { # dir patch
    if git -C "$1" apply --check "$P/$2" 2>/dev/null; then
        git -C "$1" apply "$P/$2"
        echo "applied $2"
    else
        echo "skipped $2 (already applied or the submodule moved)"
    fi
}
apply lib/rt64 rt64.patch
apply lib/rt64/src/contrib/plume rt64-plume.patch
apply lib/rt64/src/contrib/zstd rt64-zstd.patch
apply lib/rt64/src/contrib/nativefiledialog-extended rt64-nativefiledialog-extended.patch
apply lib/rt64/src/contrib/mupen64plus-win32-deps rt64-mupen64plus-win32-deps.patch
apply lib/N64ModernRuntime N64ModernRuntime.patch
