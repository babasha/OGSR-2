#!/bin/bash
# Per-file first errors of the last _wsl_build_xrgame.sh run.
# Usage: bash _wsl_errsummary.sh [g++|clang++] [N]      — first N errors per failing file
#        bash _wsl_errsummary.sh [g++|clang++] top      — most common first errors across files
CXX=${1:-g++}; N=${2:-4}
cd /tmp/xrgame_build
logname() { echo "${1%.cpp}" | sed 's#^\.\./##;s#/#_#g'; }
if [ "$N" = "top" ]; then
  for f in $(grep ^FAIL "result_$CXX.txt" | cut -d' ' -f2); do
    grep -m1 'error' "$(logname "$f")_$CXX.log" | sed 's/^[^ ]*: //'
  done | sort | uniq -c | sort -rn | head -40
  exit 0
fi
for f in $(grep ^FAIL "result_$CXX.txt" | cut -d' ' -f2 | sort); do
  b=$(logname "$f")
  echo "## $f ($(grep -c 'error' "${b}_$CXX.log"))"
  grep -m"$N" 'error' "${b}_$CXX.log"
done
