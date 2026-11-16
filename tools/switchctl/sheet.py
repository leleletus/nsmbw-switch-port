#!/usr/bin/env python3
"""sheet.py <bench_dir>: contact sheet of a screenshot burst (burst_*.jpg from bench_title.sh).

One row per screenshot: seconds after the title, the game's fps overlay (top-left, with its frame-time graph)
and the status-monitor bar (bottom: per-core CPU %, GPU % and MHz, RAM, temperatures, power) next to a small
thumbnail of the scene. Output: <bench_dir>/burst_sheet.jpg.
"""
import glob
import os
import sys

from PIL import Image, ImageDraw

d = sys.argv[1]
shots = sorted(glob.glob(os.path.join(d, "burst_*.jpg")))
if not shots:
    sys.exit("no burst_*.jpg in " + d)
FPS_BOX = (0, 0, 200, 125)          # fps overlay of the game
BAR = (0, 683, 1280, 720)           # status monitor bar
THUMB_W = 160
row_h = 125
W = 60 + THUMB_W + 200 + 1280
sheet = Image.new("RGB", (W, row_h * len(shots)), (16, 16, 16))
dr = ImageDraw.Draw(sheet)
for i, p in enumerate(shots):
    im = Image.open(p).convert("RGB")
    y = i * row_h
    dr.text((6, y + 55), os.path.basename(p)[6:-4] + " s", fill=(230, 230, 230))
    th = im.resize((THUMB_W, THUMB_W * im.height // im.width))
    sheet.paste(th, (60, y + (row_h - th.height) // 2))
    sheet.paste(im.crop(FPS_BOX), (60 + THUMB_W, y))
    sheet.paste(im.crop(BAR), (60 + THUMB_W + 200, y + (row_h - (BAR[3] - BAR[1])) // 2))
out = os.path.join(d, "burst_sheet.jpg")
sheet.save(out, quality=88)
print(out)
