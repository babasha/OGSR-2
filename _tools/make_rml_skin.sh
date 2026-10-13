#!/bin/bash
# Own artwork for the unified RmlUi menu skin ("True Stalker" look, our drawing).
# Output: $OUT/*.png (preview) and $OUT/*.dds (uncompressed BGRA, no mips).
set -e
OUT=${1:-./out}
mkdir -p "$OUT"
cd "$OUT"

GOLD='#c2ad7e'      # line colour
GOLD_HI='#e0c995'   # highlighted segment
GOLD_DIM='#6f6449'

# ---------------------------------------------------------------- panel ------
# 512x1240: dark vertical slab with soft top/bottom fades, two thin side rails
# that step outwards over the middle third, small diamonds where the rails start.
W=512; H=1240
magick -size ${W}x${H} xc:'rgba(14,13,11,0.42)' slab.png
# vertical alpha profile: 0 -> 1 over 140px, 1 -> 0 over the last 140px
magick -size 1x140 gradient:black-white -resize ${W}x140! top.png
magick -size ${W}x$((H-280)) xc:white mid.png
magick -size 1x140 gradient:white-black -resize ${W}x140! bot.png
magick top.png mid.png bot.png -append vmask.png
magick slab.png vmask.png -alpha off -compose CopyOpacity -composite slab_a.png

# rails
L=16; R=$((W-1-16)); S=4            # rail x, step outwards
Y0=150; Y1=440; Y2=760; Y3=1110     # rail start, step out, step back, end
magick -size ${W}x${H} xc:none -fill none -stroke "$GOLD" -strokewidth 2 \
  -draw "polyline $L,$Y0 $L,$Y1 $((L-S)),$((Y1+S)) $((L-S)),$((Y2-S)) $L,$Y2 $L,$Y3" \
  -draw "polyline $R,$Y0 $R,$Y1 $((R+S)),$((Y1+S)) $((R+S)),$((Y2-S)) $R,$Y2 $R,$Y3" \
  -stroke "$GOLD_HI" -strokewidth 2 \
  -draw "line $((L-S)),$((Y1+S+40)) $((L-S)),$((Y2-S-40))" \
  -draw "line $((R+S)),$((Y1+S+40)) $((R+S)),$((Y2-S-40))" \
  -stroke none -fill "$GOLD" \
  -draw "polygon $L,$((Y0-9)) $((L+5)),$((Y0-4)) $L,$Y0 $((L-5)),$((Y0-4))" \
  -draw "polygon $R,$((Y0-9)) $((R+5)),$((Y0-4)) $R,$Y0 $((R-5)),$((Y0-4))" \
  rails_raw.png
# rails fade at both ends
magick -size 1x200 gradient:black-white -resize ${W}x200! rt.png
magick -size ${W}x$((H-400)) xc:white rm.png
magick -size 1x200 gradient:white-black -resize ${W}x200! rb.png
magick rt.png rm.png rb.png -append rmask.png
magick rails_raw.png \( rails_raw.png -alpha extract rmask.png -compose Multiply -composite \) -alpha off -compose CopyOpacity -composite rails.png
magick slab_a.png rails.png -compose Over -composite panel.png

# ---------------------------------------------------------------- hline ------
# 1024x8: 2px line, transparent at both ends.
magick -size 1024x8 xc:none -fill "$GOLD" -draw "rectangle 0,3 1023,4" hl_raw.png
magick -size 8x512 gradient:black-white -rotate -90 -resize 512x8! hl_l.png
magick hl_l.png -flop hl_r.png
magick hl_l.png hl_r.png +append hl_m.png
magick hl_raw.png \( hl_raw.png -alpha extract hl_m.png -compose Multiply -composite \) -alpha off -compose CopyOpacity -composite hline.png

# ---------------------------------------------------------------- tab mark ---
# 200x14: the active-tab marker, a shallow trapezoid hanging under the line.
magick -size 200x14 xc:none -fill none -stroke "$GOLD_HI" -strokewidth 2 \
  -draw "polyline 0,3 22,3 31,11 169,11 178,3 199,3" tabmark.png

# ---------------------------------------------------------------- selector ---
# 400x48: two brackets facing each other, tails fading outwards.
magick -size 400x48 xc:none -stroke "$GOLD_HI" -strokewidth 2 -fill none \
  -draw "line 34,8 34,40" -draw "line 34,8 42,8" -draw "line 34,40 42,40" \
  -draw "line 365,8 365,40" -draw "line 365,8 357,8" -draw "line 365,40 357,40" \
  -strokewidth 2 -draw "line 0,24 34,24" -draw "line 365,24 399,24" sel_raw.png
magick -size 48x40 gradient:black-white -rotate -90 -resize 40x48! s_l.png
magick -size 320x48 xc:white s_m.png
magick s_l.png -flop s_r.png
magick s_l.png s_m.png s_r.png +append selmask.png
magick sel_raw.png \( sel_raw.png -alpha extract selmask.png -compose Multiply -composite \) -alpha off -compose CopyOpacity -composite selector.png

# ---------------------------------------------------------------- chevrons ---
# 32x32 «  » in three states: lock / avail / focus
chev() { # name colour
  magick -size 32x32 xc:none -fill none -stroke "$2" -strokewidth 3.5 -draw "polyline 15,6 6,16 15,26" -draw "polyline 26,6 17,16 26,26" "$1_l.png"
  magick "$1_l.png" -flop "$1_r.png"
}
chev chev_lock '#4a4436'
chev chev_avail '#b3a381'
chev chev_focus '#e39f3c'
magick chev_lock_l.png chev_avail_l.png chev_focus_l.png chev_lock_r.png chev_avail_r.png chev_focus_r.png +append chevrons.png


# emblem: _tools/make_rml_emblem.py (Pillow -- exact pie slices)

# ---------------------------------------------------------------- export -----
for f in panel hline tabmark selector chevrons; do
  magick "$f.png" -define dds:compression=none -define dds:mipmaps=0 "$f.dds"
done
rm -f slab.png top.png mid.png bot.png vmask.png slab_a.png rails_raw.png rt.png rm.png rb.png rmask.png rails.png \
      hl_raw.png hl_l.png hl_r.png hl_m.png sel_raw.png s_l.png s_m.png s_r.png selmask.png chev_*.png
ls -la
