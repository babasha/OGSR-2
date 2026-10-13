#!/bin/bash
# Packs the Android build into an installable APK -- no Gradle: javac -> d8 -> aapt2 -> zip -> zipalign -> apksigner.
# Runs in WSL Arch after cmake/android/build.sh:
#
#   MSYS_NO_PATHCONV=1 wsl -d archlinux bash /mnt/d/OGSR-ONLINE/ogsr_engine/android/package_apk.sh
#
# Output: D:\android_stage\ogsr.apk (adb install -r it). Game data is NOT in the APK: it goes to
# /sdcard/Android/data/com.ogsr.online/files with adb push (see push_data.sh).
set -euo pipefail

MIRROR=${OGSR_ANDROID_MIRROR:-/root/ogsr_android}
DEPS=${OGSR_ANDROID_DEPS:-/root/android_deps}
SDK=${ANDROID_HOME:-/opt/android-sdk}
NDK=${ANDROID_NDK_HOME:-$SDK/ndk/27.3.13750724}
BT=$SDK/build-tools/36.0.0
PLATFORM_JAR=$SDK/platforms/android-34/android.jar
SDL2=$DEPS/SDL2-2.30.11
BUILD=$MIRROR/build_android
SRC=$MIRROR/ogsr_engine/android
STAGE=${OGSR_ANDROID_STAGE:-/mnt/d/android_stage}
KS=$HOME/.android/ogsr-debug.keystore

# The mirror copy of this folder is refreshed by build.sh; pick up manifest/Java edits made since.
rsync -a --checksum /mnt/d/OGSR-ONLINE/ogsr_engine/android/ "$SRC/"

T=$BUILD/apk_tmp
rm -rf "$T" && mkdir -p "$T/classes" "$T/dex" "$T/pkg/lib/arm64-v8a"

javac -encoding UTF-8 -nowarn -Xlint:-options -source 8 -target 8 -bootclasspath "$PLATFORM_JAR:$BT/core-lambda-stubs.jar" -classpath "$PLATFORM_JAR" -d "$T/classes" \
    $(find "$SDL2/android-project/app/src/main/java" "$SRC/java" -name '*.java')
"$BT/d8" --release --min-api 29 --lib "$PLATFORM_JAR" --output "$T/dex" $(find "$T/classes" -name '*.class')

"$BT/aapt2" link -o "$T/base.apk" --manifest "$SRC/AndroidManifest.xml" -I "$PLATFORM_JAR" \
    --min-sdk-version 29 --target-sdk-version 34

STRIP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
cp "$BUILD/xr_3da/libmain.so" "$BUILD/SDL2/libSDL2.so" \
   "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$T/pkg/lib/arm64-v8a/"
"$STRIP" --strip-unneeded "$T"/pkg/lib/arm64-v8a/*.so   # the unstripped libmain.so stays in the build dir (symbolize crashes with it)
cp "$T/dex/classes.dex" "$T/pkg/"
(cd "$T/pkg" && zip -q -r "$T/base.apk" classes.dex lib)

"$BT/zipalign" -f -P 16 4 "$T/base.apk" "$T/aligned.apk"
if [ ! -f "$KS" ]; then
    mkdir -p "$(dirname "$KS")"
    keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias ogsr -keyalg RSA -keysize 2048 \
        -validity 10000 -dname "CN=OGSR Debug" >/dev/null
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$T/ogsr.apk" "$T/aligned.apk"

mkdir -p "$STAGE"
cp "$T/ogsr.apk" "$STAGE/ogsr.apk"
ls -la "$STAGE/ogsr.apk"
