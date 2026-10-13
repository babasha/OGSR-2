#!/bin/bash
# Show context of the first occurrence of an error pattern among the files that FAILED in the last run
# (older logs in /tmp/xrgame_build are ignored). Usage: bash _wsl_errctx.sh [cxx] PATTERN [lines]
CXX=$1; P=$2; N=${3:-6}
cd /tmp/xrgame_build
mapfile -t LOGS < <(grep ^FAIL "result_$CXX.txt" | cut -d' ' -f2 | sed 's#\.cpp$##;s#^\.\./##;s#/#_#g;s#$#_'"$CXX"'.log#')
[ ${#LOGS[@]} -eq 0 ] && { echo "no failures"; exit 0; }
mapfile -t HITS < <(grep -l -- "$P" "${LOGS[@]}" 2>/dev/null)
echo "### '$P' in ${HITS[0]:-<none>} (${#HITS[@]} files)"
[ ${#HITS[@]} -eq 0 ] && exit 0
grep -m1 -B"$N" -A3 -- "$P" "${HITS[0]}" | grep -v '^\s*|\s*$' | grep -v '^\s*[0-9]* |' | cut -c1-300
