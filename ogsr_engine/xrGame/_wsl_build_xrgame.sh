#!/bin/bash
# WSL (Arch) compile harness for xrGame.vcxproj — the file list is read from the vcxproj
# itself, so it always matches what the Windows build compiles.
#
# Sources are first mirrored (rsync, sources only) into the WSL-native ext4 filesystem and compiled
# THERE: (1) reading hundreds of headers per TU over /mnt/d (9P) made the build IO-bound ~5x slower,
# (2) ext4 is case-sensitive, so wrong-case #includes fail exactly like on a real Linux box
# (DrvFs under /mnt/d is case-insensitive and hides them). Set NOSYNC=1 to skip the rsync.
#
# Usage: bash _wsl_build_xrgame.sh [g++|clang++ ...] [-- file.cpp ... | --failing | --every K | --regex RE]
#   --failing : only files marked FAIL for any of the chosen compilers in _wsl_xrgame_status_<cxx>.txt
#   --every K : every Kth vcxproj file (quick survey of shared-header breakage)
#   --regex RE: only vcxproj files matching extended regex RE
# Logs: /tmp/xrgame_build/<path_with_underscores>_<compiler>.log
set -u
SRC=/mnt/d/OGSR-2
MIRROR=${MIRROR:-$HOME/ogsr_mirror}
OUT=/tmp/xrgame_build
mkdir -p "$OUT" "$MIRROR"

if [ "${NOSYNC:-0}" != 1 ]; then
  RS=(rsync -a --delete --prune-empty-dirs --include='*/' --include='*.h' --include='*.hpp' --include='*.inl'
      --include='*.cpp' --include='*.c' --include='*.cc' --include='*.cxx' --include='*.vcxproj' --include='*.txt' --include='*.sh' --exclude='*')
  "${RS[@]}" --exclude='_TEMP/' --exclude='_LIB/' "$SRC/ogsr_engine/" "$MIRROR/ogsr_engine/"
  for d in ode eax OpenAL_Soft/include tracy/public imgui RmlUi/Include zstd/zstd/lib; do
    mkdir -p "$MIRROR/3rd_party/Src/$d"; "${RS[@]}" "$SRC/3rd_party/Src/$d/" "$MIRROR/3rd_party/Src/$d/"
  done
  mkdir -p "$MIRROR/3rd_party/GameNetworkingSockets/include"
  "${RS[@]}" "$SRC/3rd_party/GameNetworkingSockets/include/" "$MIRROR/3rd_party/GameNetworkingSockets/include/"
fi

cd "$MIRROR/ogsr_engine/xrGame"
ROOT=$MIRROR/ogsr_engine
TP=$MIRROR/3rd_party/Src

INCLUDES=(
  -I .
  -I ../COMMON_AI -I ../COMMON_AI/PATH -I ../COMMON_AI/GRAPH -I ../COMMON_AI/ALGORITHMS
  -I "$ROOT/xrCore"
  -I "$ROOT/xr_3da"
  -I "$ROOT/xrCDB"
  -I "$ROOT/xrSound"
  -I "$ROOT/xrParticles"
  -I "$ROOT/Luabind"
  -I "$ROOT/LuaJIT/src"
  -I "$ROOT/xrOnlineNet"
  -I "$ROOT/Include"
  -I "$TP/ode"
  -I "$TP/tracy/public"
  -I "$TP/imgui"
  -I "$TP/RmlUi/Include"
  -I "$TP/zstd/zstd/lib"
  -I "$TP/OpenAL_Soft/include/AL" # eax.h's OPENAL branch includes <al.h> (the embedded sound-env editor)
  -I "$MIRROR/3rd_party/GameNetworkingSockets/include"
)
# MSVC is NOT defined here on purpose: the Windows project defines it for ODE's config.
DEFS=(-DXRGAME_STATIC -DXRENGINE_STATIC -DXRCORE_STATIC -DXRCDB_STATIC -DXRSOUND_STATIC -DXR_PARTICLES_STATIC
      -DLUABIND_STATIC -DRMLUI_STATIC_LIB -DNDEBUG -D_WINDOWS -D_USRDLL -DXRGAME_EXPORTS -DdSINGLE -D_MT)
FLAGS=(-std=c++20 -c -w)

COMPILERS=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do COMPILERS+=("$1"); shift; done
[ $# -gt 0 ] && shift
[ ${#COMPILERS[@]} -eq 0 ] && COMPILERS=(g++ clang++)

# vcxproj paths are spelled with Windows-insensitive case (AI\, Ik\, tri-colliderKNoOPC\...);
# respell each against the real (case-sensitive) tree so the harness doesn't fail on the project file itself.
respell() {
  local p=$1 cur=. out="" c hit
  IFS=/ read -ra parts <<< "$p"
  for c in "${parts[@]}"; do
    if [ "$c" = ".." ] || [ "$c" = "." ]; then hit=$c
    else hit=$(ls -1 "$cur" 2>/dev/null | grep -ixF -- "$c" | head -1); [ -z "$hit" ] && hit=$c; fi
    cur="$cur/$hit"; out="${out:+$out/}$hit"
  done
  echo "$out"
}
# Entries with <ExcludedFromBuild>true</ExcludedFromBuild> are skipped, as MSBuild does
# (dcTriListCollider.cpp is #included by dTriList.cpp and never compiled on its own).
mapfile -t RAW < <(tr -d '\r' < xrGame.vcxproj | awk '
  /<ClCompile Include="/ { if (f != "") print f; match($0, /Include="[^"]*"/); f = substr($0, RSTART + 9, RLENGTH - 10)
                           if ($0 ~ /\/>/) { print f; f = "" }; next }
  f != "" && /ExcludedFromBuild>true/ { f = ""; next }
  f != "" && /<\/ClCompile>/ { print f; f = "" }
  END { if (f != "") print f }' | tr '\\' '/')
ALL=(); for f in "${RAW[@]}"; do if [ -e "$f" ]; then ALL+=("$f"); else ALL+=("$(respell "$f")"); fi; done

if [ $# -ge 1 ] && [ "$1" = "--failing" ]; then
  mapfile -t FILES < <(for c in "${COMPILERS[@]}"; do grep ^FAIL "$SRC/ogsr_engine/xrGame/_wsl_xrgame_status_$c.txt" 2>/dev/null; done | cut -d' ' -f2 | sort -u)
elif [ $# -ge 2 ] && [ "$1" = "--every" ]; then   # quick survey: every Kth file
  mapfile -t FILES < <(printf '%s\n' "${ALL[@]}" | awk -v k="$2" 'NR%k==1')
elif [ $# -ge 2 ] && [ "$1" = "--regex" ]; then
  mapfile -t FILES < <(printf '%s\n' "${ALL[@]}" | grep -E "$2")
elif [ $# -gt 0 ]; then
  FILES=("$@")
else
  FILES=("${ALL[@]}")
fi

compile_one() {
  local CXX=$1 f=$2 base
  base=$(echo "${f%.cpp}" | sed 's#^\.\./##;s#/#_#g')
  if "$CXX" "${FLAGS[@]}" "${DEFS[@]}" "${INCLUDES[@]}" "$f" -o "$OUT/${base}_$CXX.o" 2> "$OUT/${base}_$CXX.log"; then
    echo "PASS $f"
  else
    echo "FAIL $f"
  fi
}

for CXX in "${COMPILERS[@]}"; do
  echo "=== $CXX (${#FILES[@]} files) ==="
  : > "$OUT/result_$CXX.txt"
  running=0
  for f in "${FILES[@]}"; do
    compile_one "$CXX" "$f" >> "$OUT/result_$CXX.txt" &
    running=$((running+1))
    if [ $running -ge "$(nproc)" ]; then wait -n; running=$((running-1)); fi
  done
  wait
  echo "$CXX: $(grep -c ^PASS "$OUT/result_$CXX.txt") pass, $(grep -c ^FAIL "$OUT/result_$CXX.txt") fail"
  # Cumulative per-file status, kept in the repo so it survives WSL restarts and partial runs:
  # a partial run only replaces its own files' lines. This is the multi-session to-do list.
  ST="$SRC/ogsr_engine/xrGame/_wsl_xrgame_status_$CXX.txt"
  touch "$ST"
  awk 'NR==FNR {res[$2]=$1; next} ($2 in res) {next} {print} END {for (f in res) print res[f], f}' \
      "$OUT/result_$CXX.txt" "$ST" | sort -k2 > "$ST.tmp" && mv "$ST.tmp" "$ST"
  echo "$CXX cumulative: $(grep -c ^PASS "$ST") pass, $(grep -c ^FAIL "$ST") fail (of ${#ALL[@]})"
done
