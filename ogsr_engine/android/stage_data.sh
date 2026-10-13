#!/bin/bash
# Assembles the game data for the phone in D:\android_stage\files (pushed with push_data.ps1 to
# /sdcard/Android/data/com.ogsr.online/files). Layers, later wins:
#   1. the Kordon-only pack v4 (squashfs archives with the .db names, textures <= 512 px): /root/pack/out4
#      (_tools/switch_pack/pack_kordon.sh, see the switch-kordon-data-pack note)
#   2. loose config/scripts/spawns from the Switch stage (kept current for the console)
#   3. the engine resources from the repo (Game/Resources_SoC_1.0006/gamedata: shaders.xr, rspec presets, ui...)
#   4. the compiled SPIR-V from the repo (ogsr_engine/Layers/xrRenderVulkan/shaders/*.spv)
#
#   MSYS_NO_PATHCONV=1 wsl -d archlinux bash /mnt/d/OGSR-ONLINE/ogsr_engine/android/stage_data.sh
set -euo pipefail

PACK=${OGSR_ANDROID_PACK:-/root/pack/out4}
SWITCH_STAGE=/mnt/d/switch_stage/ogsr
REPO=/mnt/d/OGSR-ONLINE
OUT=${OGSR_ANDROID_DATA:-/mnt/d/android_stage/files}

mkdir -p "$OUT/gamedata/shaders"
rsync -a --size-only "$PACK/" "$OUT/"
for d in config scripts spawns; do
    [ -d "$SWITCH_STAGE/gamedata/$d" ] && rsync -a --checksum "$SWITCH_STAGE/gamedata/$d/" "$OUT/gamedata/$d/"
done
rsync -a --checksum "$REPO/Game/Resources_SoC_1.0006/gamedata/" "$OUT/gamedata/"
rsync -a --checksum "$REPO/ogsr_engine/Layers/xrRenderVulkan/shaders/"*.spv "$OUT/gamedata/shaders/"
# The Switch preset is the starting point for the phone (the user's own settings land in _appdata_/user.ltx).
[ -f "$OUT/args.txt" ] || echo "-nosound" > "$OUT/args.txt"
du -sh "$OUT"
