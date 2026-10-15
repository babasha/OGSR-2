#!/bin/bash
# Assembles the game data for the phone in D:\android_stage\files (pushed with push_data.ps1 to
# /sdcard/Android/data/com.ogsr.online/files). Layers, later wins:
#   1. the Kordon-only pack v4 (squashfs archives with the .db names, textures <= 512 px): /root/pack/out4
#      (_tools/switch_pack/pack_kordon.sh, see the switch-kordon-data-pack note)
#   2. loose config/scripts/spawns from the Switch stage, then the Gunslinger overrides from mod_overrides/
#      (local, not in the public repo; same files sync.ps1 puts into the PC install): the RmlUi screens,
#      their fonts and icons under textures\ui -- without them the menu has no text and the screen stays black
#   3. from the repo's Game/Resources_SoC_1.0006/gamedata only what the PC and Switch installs carry too:
#      the rspec_vk_* presets and actor_legs.ltx (+ config/ui_rml: the touch controls). The rest is stock OGSR (misc/items.ltx, ui_mm_*.xml,
#      _g.script...) and overrode Gunslinger's own copies in the archives: "Can't open section 'bio'".
#   4. the compiled SPIR-V from the repo (ogsr_engine/Layers/xrRenderVulkan/shaders/*.spv)
#
#   MSYS_NO_PATHCONV=1 wsl -d archlinux bash /mnt/d/OGSR-2/ogsr_engine/android/stage_data.sh
set -euo pipefail

PACK=${OGSR_ANDROID_PACK:-/root/pack/out4}
SWITCH_STAGE=/mnt/d/switch_stage/ogsr
REPO=/mnt/d/OGSR-2
OUT=${OGSR_ANDROID_DATA:-/mnt/d/android_stage/files}

mkdir -p "$OUT/gamedata/shaders"
rsync -a --size-only "$PACK/" "$OUT/"
for d in config scripts spawns; do
    [ -d "$SWITCH_STAGE/gamedata/$d" ] && rsync -a --checksum "$SWITCH_STAGE/gamedata/$d/" "$OUT/gamedata/$d/"
done
MOD=$REPO/mod_overrides/gunslinger/gamedata
if [ -d "$MOD" ]; then rsync -a --checksum "$MOD/" "$OUT/gamedata/"; else echo "! no $MOD: RmlUi screens and fonts missing"; fi
rsync -a --checksum "$REPO/Game/Resources_SoC_1.0006/gamedata/config/"{rspec_vk_*.ltx,actor_legs.ltx} "$OUT/gamedata/config/"
mkdir -p "$OUT/gamedata/config/ui_rml"
rsync -a --checksum "$REPO/Game/Resources_SoC_1.0006/gamedata/config/ui_rml/" "$OUT/gamedata/config/ui_rml/" # the touch controls
rsync -a --checksum "$REPO/ogsr_engine/Layers/xrRenderVulkan/shaders/"*.spv "$OUT/gamedata/shaders/"
# The Switch preset is the starting point for the phone (the user's own settings land in _appdata_/user.ltx).
[ -f "$OUT/args.txt" ] || echo "-nosound" > "$OUT/args.txt"
du -sh "$OUT"
