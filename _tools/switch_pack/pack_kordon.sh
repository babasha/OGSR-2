#!/bin/bash
# Builds the Kordon-only Switch data package (WSL). The original install is only READ.
# Each original archive is replaced by a slimmed squashfs of the SAME NAME (the engine detects squashfs by
# magic, and archive order / .xdb-after-loose precedence therefore stays exactly as it was).
#   kept: everything except the other levels; big textures lose their top mips (cap 1024)
#   out : $OUT (default /root/pack/out) mirrors sdmc:/switch/ogsr/
set -e -o pipefail
SRC="/mnt/d/OGSR Gunslinger Addon"
W=/root/pack
OUT=${OUT:-$W/out}
DBT=${DBT:-/root/dbtool/dbtool}
DDSMAX=${DDSMAX:-1024}
BCMIN=${BCMIN:-512}   # uncompressed DDS with a side >= this are re-encoded to DXT1/DXT5 (see ddsenc.h); 0 = off
KEEP_LEVEL=${KEEP_LEVEL:-l01_escape}
mkdir -p $W/tree $OUT/mods $OUT/gamedata
LOG=$W/pack.log; : > $LOG
say() { echo "[$(date +%T)] $*" | tee -a $LOG; }

# 1. every level directory present anywhere (archives + loose), minus the one we keep
say "collecting level names"
{
  for a in "$SRC"/gamedata.db* "$SRC"/mods/*.xdb*; do
    # an archive with no levels\ entries makes grep exit 1: that is not an error here
    [ -s "$a" ] && { $DBT list "$a" 2>/dev/null | awk -F'\t' '{print $3}' | grep -i '^levels\\' | awk -F'\\' 'NF>=2{print tolower($2)}' || true; }
  done
  ls "$SRC/gamedata/levels" 2>/dev/null | tr 'A-Z' 'a-z' || true
} | { grep -v '^$' || true; } | sort -u > $W/levels_all.txt
SKIPS=()
SKIPS+=(--skip "_backup_nomip_textures\\")
if [ "$KEEP_LEVEL" = all ]; then
  # the whole game: every level, loading screen and movie; only the stand-alone Pripyat test level
  # (run_pripyat.bat, not reachable from the story) stays out
  SKIPS+=(--skip "levels\\pripyat_full\\" --skip "spawns\\pripyat_full")
else
  while read -r l; do [ "$l" = "$KEEP_LEVEL" ] || SKIPS+=(--skip "levels\\$l\\"); done < $W/levels_all.txt
  # per-level loading-screen slideshows (textures\intro\intro_<level>_N.dds, ~1.6 GB for all levels) and the intro/ending movies
  while read -r l; do [ "$l" = "$KEEP_LEVEL" ] || SKIPS+=(--skip "textures\\intro\\intro_${l}_"); done < $W/levels_all.txt
  SKIPS+=(--skipext "textures\\intro\\" ".ogm")
fi
say "levels found: $(tr '\n' ' ' < $W/levels_all.txt)"
say "keeping level: $KEEP_LEVEL; ${#SKIPS[@]} skip args"

pack_archive() {  # <input archive> <output squashfs>
  local in="$1" out="$2" name; name=$(basename "$in")
  [ -s "$in" ] || { say "skip empty: $name"; return; }
  rm -rf $W/tree/cur; mkdir -p $W/tree/cur
  say "extract $name ($(du -h "$in" | cut -f1))"
  $DBT extract "$in" $W/tree/cur "${SKIPS[@]}" --ddsmax $DDSMAX --bcmin $BCMIN 2>>$LOG | tee -a $LOG
  say "mksquashfs -> $(basename "$out")"
  rm -f "$out"
  mksquashfs $W/tree/cur "$out" -comp zstd -Xcompression-level 10 -b 131072 -noappend -no-xattrs -all-root -quiet -processors 8 >>$LOG 2>&1
  say "done $name -> $(du -h "$out" | cut -f1)"
  rm -rf $W/tree/cur
}

for a in "$SRC"/gamedata.db*; do pack_archive "$a" "$OUT/$(basename "$a")"; done
for a in "$SRC"/mods/*.xdb*;   do pack_archive "$a" "$OUT/mods/$(basename "$a")"; done

# 2. loose gamedata (precedence over archives must stay): same rules, written as loose files
say "loose gamedata"
rm -rf "$OUT/gamedata"; mkdir -p "$OUT/gamedata"
$DBT dir "$SRC/gamedata" "$OUT/gamedata" "${SKIPS[@]}" --ddsmax $DDSMAX --bcmin $BCMIN 2>&1 | tee -a $LOG

# 3. small config files
cp "$SRC/fsgame.ltx" "$OUT/fsgame.ltx"
cp "$SRC/launch.ltx" "$OUT/launch.ltx" 2>/dev/null || true
[ -d "$SRC/localization" ] && cp -r "$SRC/localization" "$OUT/localization"
say "PACKAGE DONE"; du -sh "$OUT"; du -h --max-depth=1 "$OUT" | sort -rh | head -20 | tee -a $LOG
