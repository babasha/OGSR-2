#!/usr/bin/env python3
"""Menu emblem: a thin ring around a radiation trefoil, our own drawing.
usage: make_rml_emblem.py out_dir   (writes emblem.png + emblem.dds; needs Pillow + ImageMagick)
"""
import subprocess
import sys

from PIL import Image, ImageDraw

out = sys.argv[1]
S = 4                      # supersampling
N = 256 * S
c = N // 2
gold = (194, 173, 126, 255)

img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
# outer ring
r = 118 * S
d.ellipse([c - r, c - r, c + r, c + r], outline=gold, width=4 * S)
# three blades, 60 degrees each, centred on 12, 4 and 8 o'clock (PIL: clockwise from 3 o'clock)
rb = 88 * S
for a0 in (-120, 0, 120):
    d.pieslice([c - rb, c - rb, c + rb, c + rb], a0, a0 + 60, fill=gold)
# clear ring around the hub, then the hub itself
rc = 26 * S
d.ellipse([c - rc, c - rc, c + rc, c + rc], fill=(0, 0, 0, 0))
rh = 15 * S
d.ellipse([c - rh, c - rh, c + rh, c + rh], fill=gold)

img = img.resize((256, 256), Image.LANCZOS)
img.save(f"{out}/emblem.png")
subprocess.check_call(["magick", f"{out}/emblem.png", "-define", "dds:compression=none", f"{out}/emblem.dds"])
print("emblem ok")
