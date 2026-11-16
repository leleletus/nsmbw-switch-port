#!/usr/bin/env python3
"""hoja_capturas.py <bench_dir>: all 02_title_* and burst_* screenshots of a run as one contact sheet
(capturas.jpg, 6 per row, with the file name), to review the IMAGE of every run by eye (glitches, vertex
explosions, missing textures), not only fps."""
import glob, os, sys
from PIL import Image, ImageDraw
d = sys.argv[1]
fs = sorted(glob.glob(os.path.join(d, "02_title_*.jpg"))) + sorted(glob.glob(os.path.join(d, "burst_[0-9]*.jpg")))
if not fs:
    sys.exit(0)
W, H, cols = 320, 180, 6
rows = (len(fs) + cols - 1) // cols
sheet = Image.new("RGB", (W * cols, H * rows), (0, 0, 0))
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(fs):
    x, y = (i % cols) * W, (i // cols) * H
    sheet.paste(Image.open(f).convert("RGB").resize((W, H)), (x, y))
    dr.text((x + 4, y + H - 14), os.path.basename(f)[:-4], fill=(255, 255, 0))
sheet.save(os.path.join(d, "capturas.jpg"), quality=80)
print(os.path.join(d, "capturas.jpg"))
