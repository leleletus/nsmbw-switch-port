# xfb_compare.py <dir_gl> <dir_dk> [salida]: compara los volcados NWII_XFBDUMP (<prefijo>_<n>.ppm) de dos corridas
# (backend GL y deko3d) por número de copia XFB. Escribe por par: píxeles distintos, diferencia máxima, y una
# imagen de diferencias (<salida>/diff_<n>.ppm, en rojo lo distinto) si hay diferencias. Sin dependencias.
import os, re, sys

def load(p):
    d = open(p, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:  # P6 w h max
        while d[i:i+1].isspace(): i += 1
        if d[i:i+1] == b'#':
            while d[i:i+1] != b'\n': i += 1
            continue
        j = i
        while not d[j:j+1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i:i + w * h * 3]

def index(dirn):
    out = {}
    for f in os.listdir(dirn):
        m = re.search(r'_(\d+)\.ppm$', f)
        if m: out[int(m.group(1))] = os.path.join(dirn, f)
    return out

a, b = index(sys.argv[1]), index(sys.argv[2])
outd = sys.argv[3] if len(sys.argv) > 3 else None
if outd: os.makedirs(outd, exist_ok=True)
common = sorted(set(a) & set(b))
same = 0
for n in common:
    wa, ha, pa = load(a[n]); wb, hb, pb = load(b[n])
    if (wa, ha) != (wb, hb):
        print(f"{n}: tamaño distinto {wa}x{ha} / {wb}x{hb}"); continue
    diff, mx = 0, 0
    dimg = bytearray(len(pa)) if outd else None
    for i in range(0, len(pa), 3):
        d = max(abs(pa[i] - pb[i]), abs(pa[i+1] - pb[i+1]), abs(pa[i+2] - pb[i+2]))
        if d:
            diff += 1; mx = max(mx, d)
            if dimg is not None: dimg[i] = 255
        elif dimg is not None:
            g = pa[i+1] // 3; dimg[i] = dimg[i+1] = dimg[i+2] = g
    if not diff: same += 1
    print(f"{n}: {diff} píxeles distintos (máx {mx})")
    if diff and outd:
        with open(os.path.join(outd, f"diff_{n:06d}.ppm"), 'wb') as f:
            f.write(b"P6\n%d %d\n255\n" % (wa, ha)); f.write(bytes(dimg))
print(f"pares {len(common)}, idénticos {same}; solo GL {len(set(a)-set(b))}, solo deko3d {len(set(b)-set(a))}")
