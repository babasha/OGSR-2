#!/bin/bash
# Unique file:line sites of an error pattern across all logs of one compiler (last run).
# Usage: bash _wsl_errsites.sh <cxx> PATTERN [PATTERN...]
CXX=$1; shift
cd /tmp/xrgame_build
for p in "$@"; do echo "== $p"; grep -h -- "$p" *_"$CXX".log | grep -o '^[^ ]*:[0-9]*' | sort | uniq -c; done
