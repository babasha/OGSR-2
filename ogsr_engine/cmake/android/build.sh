#!/bin/bash
# Android build of the engine (libmain.so, arm64-v8a) -- runs in WSL Arch.
#
#   MSYS_NO_PATHCONV=1 wsl -d archlinux bash /mnt/d/OGSR-2/ogsr_engine/cmake/android/build.sh [ninja target]
#
# The source of truth is the git tree on D:. It is copied into an ext4 mirror first (DrvFs is
# case-insensitive and hides include-case bugs), then built there with the NDK's own toolchain file.
# Do not edit sources on D: while this runs: rsync keeps mtimes, an edit made during the build looks
# older than its object file next time (touch the edited file then).
set -euo pipefail

SRC=/mnt/d/OGSR-2
MIRROR=${OGSR_ANDROID_MIRROR:-/root/ogsr_android}
DEPS=${OGSR_ANDROID_DEPS:-/root/android_deps}
SDK=${ANDROID_HOME:-/opt/android-sdk}
NDK=${ANDROID_NDK_HOME:-$SDK/ndk/27.3.13750724}
API=${OGSR_ANDROID_API:-29}
TARGET=${1:-xr_3da}

mkdir -p "$MIRROR/3rd_party/Src"
cd "$SRC"
git ls-files --cached --others --exclude-standard ogsr_engine > "$MIRROR/.files" # new, not yet added files too
rsync -a --checksum --files-from="$MIRROR/.files" "$SRC/" "$MIRROR/"
# 3rd_party/Src: half of it is submodules / untracked trees, copy from disk (no Windows binaries).
rsync -a --checksum --delete-excluded \
    --exclude='.git' --exclude='build/' --exclude='build_*/' --exclude='*.lib' --exclude='*.dll' \
    --exclude='*.pdb' --exclude='*.exe' --exclude='*.obj' --exclude='/reshade/' \
    "$SRC/3rd_party/Src/" "$MIRROR/3rd_party/Src/"

BUILD=$MIRROR/build_android
if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$MIRROR/ogsr_engine" -B "$BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=Release \
        -DOGSR_SDL2_DIR="$DEPS/SDL2-2.30.11" -DOGSR_OPENAL_DIR="$DEPS/openal-soft-1.24.3"
fi
ninja -C "$BUILD" -k 0 -j "${OGSR_ANDROID_JOBS:-4}" "$TARGET" # -j4: a full -j ran the machine out of memory (10-10)
