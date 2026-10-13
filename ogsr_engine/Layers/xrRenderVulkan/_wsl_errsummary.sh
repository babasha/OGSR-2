#!/bin/bash
# Per-file first errors of the last _wsl_build_xrrendervulkan.sh run. Usage: bash _wsl_errsummary.sh [g++|clang++] [N]
CXX=${1:-g++}; N=${2:-4}
cd /tmp/xrrvk_build
for f in $(grep ^FAIL "result_$CXX.txt" | cut -d' ' -f2 | sort); do
  b=$(basename "$f" .cpp)
  echo "## $f ($(grep -c 'error' "${b}_$CXX.log"))"
  grep -m"$N" 'error' "${b}_$CXX.log"
done
