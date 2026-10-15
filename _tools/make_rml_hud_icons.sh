#!/bin/bash
# The RmlUi HUD's own icons: PNGs from make_rml_hud_icons.ps1 -> DDS.
#   bash _tools/make_rml_hud_icons.sh <dir with png/> <textures/ui/rml_hud out dir>
# Uncompressed BGRA WITH mips: the icons are drawn 24..60 px from 64..512 px
# masters, and a minified picture without mips sparkles at its edges.
set -e
IN=${1:?png dir parent}
OUT=${2:?output dir}
mkdir -p "$OUT"
for f in "$IN"/png/*.png; do
    n=$(basename "$f" .png)
    magick "$f" -define dds:compression=none -define dds:mipmaps=fromlist -define dds:mipmaps=8 "$OUT/$n.dds" 2>/dev/null \
      || magick "$f" -define dds:compression=none -define dds:mipmaps=8 "$OUT/$n.dds"
    echo "$n.dds"
done
