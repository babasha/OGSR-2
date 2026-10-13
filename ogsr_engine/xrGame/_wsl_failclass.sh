#!/bin/bash
# One line per file currently FAILING on g++ or clang++ according to the cumulative status files
# (_wsl_xrgame_status_<cxx>.txt): "path|first clang error (or g++ one)". Needs the logs of the run that
# produced that status to still be in /tmp/xrgame_build (re-run the build after a WSL restart).
# Usage: bash _wsl_failclass.sh > out.txt
S=$(cd "$(dirname "$0")" && pwd)
cd /tmp/xrgame_build
cat "$S/_wsl_xrgame_status_g++.txt" "$S/_wsl_xrgame_status_clang++.txt" 2>/dev/null | grep ^FAIL | cut -d' ' -f2 | sort -u | while read -r f; do
  b=$(echo "${f%.cpp}" | sed 's#^\.\./##;s#/#_#g')
  e=""
  grep -q "^FAIL $f\$" "$S/_wsl_xrgame_status_clang++.txt" && e=$(grep -m1 'error' "${b}_clang++.log" 2>/dev/null | sed 's/^[^ ]*: //' | cut -c1-110)
  [ -z "$e" ] && e="(g++) $(grep -m1 'error' "${b}_g++.log" 2>/dev/null | sed 's/^[^ ]*: //' | cut -c1-104)"
  echo "$f|$e"
done
