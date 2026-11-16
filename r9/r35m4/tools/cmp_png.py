#!/usr/bin/env python3
# cmp_png.py <dir_ref> <dir_prueba>: compara las capturas t_*.png de dos corridas de checks.sh
# (mismo fotograma). Por captura: % de pixeles distintos (>24 en algun canal) y % de pixeles negros.
import sys, os
from PIL import Image, ImageChops
a, b = sys.argv[1], sys.argv[2]
peor = 0.0
for f in sorted(os.listdir(a)):
    if not (f.startswith('t_') and f.endswith('.png')) or not os.path.exists(os.path.join(b, f)): continue
    ia = Image.open(os.path.join(a, f)).convert('RGB'); ib = Image.open(os.path.join(b, f)).convert('RGB')
    if ia.size != ib.size: print(f, 'tamano distinto'); continue
    d = ImageChops.difference(ia, ib).getdata(); n = len(d)
    dif = sum(1 for p in d if max(p) > 24) * 100.0 / n
    neg = lambda im: sum(1 for p in im.getdata() if max(p) < 12) * 100.0 / n
    print(f"{f}: distintos {dif:5.1f} %  negro ref {neg(ia):5.1f} % prueba {neg(ib):5.1f} %")
    peor = max(peor, dif)
print(f"PEOR {peor:.1f} %")
