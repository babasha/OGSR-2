#!/bin/bash
# WSL (Arch) compile harness for xrRender_Vulkan.vcxproj — the file list is read from
# the vcxproj itself, so it always matches what the Windows build compiles.
# Usage: bash _wsl_build_xrrendervulkan.sh [g++|clang++ ...] [-- file.cpp ...]
# Logs: /tmp/xrrvk_build/<file>_<compiler>.log
set -u
cd "$(dirname "$0")"
ROOT=/mnt/d/OGSR-2/ogsr_engine
TP=/mnt/d/OGSR-2/3rd_party/Src
VKSDK=${VKSDK:-/mnt/c/VulkanSDK/1.4.350.0/Include}
OUT=/tmp/xrrvk_build
mkdir -p "$OUT"

INCLUDES=(
  -I .
  -I "$ROOT/xrCore"
  -I "$ROOT/Luabind"
  -I "$ROOT/LuaJIT/src"
  -I "$TP/tracy/public"
  -I "$TP/NVIDIA_DLSS/DLSS/include"
  -I "$TP/AMD_FidelityFX/sdk/include"
  -I "$TP/VulkanMemoryAllocator/VulkanMemoryAllocator/include"
  -I "$TP/NVIDIA_Streamline/include"
  -I "$VKSDK"
)
DEFS=(-DNDEBUG -D_WINDOWS -D_USRDLL -DXRENGINE_STATIC -DXRCORE_STATIC -DXRCDB_STATIC -DXR_PARTICLES_STATIC -DLUABIND_STATIC -DXRRENDER_STATIC -D_MT)
FLAGS=(-std=c++20 -c -w)

COMPILERS=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do COMPILERS+=("$1"); shift; done
[ $# -gt 0 ] && shift
[ ${#COMPILERS[@]} -eq 0 ] && COMPILERS=(g++ clang++)

if [ $# -gt 0 ]; then
  FILES=("$@")
else
  mapfile -t FILES < <(tr -d '\r' < xrRender_Vulkan.vcxproj | grep -o '<ClCompile Include="[^"]*"' | sed 's/<ClCompile Include="//;s/"$//;s#\\#/#g')
fi

compile_one() {
  local CXX=$1 f=$2 base
  base=$(basename "$f" .cpp)
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
  grep ^FAIL "$OUT/result_$CXX.txt" | sort
done
