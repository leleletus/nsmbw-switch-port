#!/usr/bin/env python3
"""orden_pares.py <cruces.tsv> <salida> [orden_L.txt]
Archivo para -Wl,--section-ordering-file que mantiene juntas las secciones unidas por saltos condicionales (cruces.py).
- Sin orden L: al principio de .text (tras .crt0) van, por pares, la sección origen y sus destinos; el resto lo coloca
  switch.ld como siempre (.text.unlikely*, .text.hot*, .text*).
- Con orden L (hot_order.py): se respeta ese orden y detrás de cada sección origen se ponen sus destinos; los pares
  cuyo origen no está en el orden L van al principio."""
import collections, re, sys

cr = collections.OrderedDict()
for l in open(sys.argv[1]):
    o, _, s, d = l.rstrip("\n").split("\t")
    cr.setdefault((o, s), [])
    if d not in cr[(o, s)]:
        cr[(o, s)].append(d)

def ent(o, s):
    return f"  */{o}({s})"

base = []
if len(sys.argv) > 3:
    for l in open(sys.argv[3]):
        m = re.match(r"\s*\*/([^(:]+\.o)\((\.text[^)]*)\)\s*$", l)
        base.append((l.rstrip("\n"), (m.group(1), m.group(2)) if m else None))

placed = set()
out = ["/* generado por r9/r42k/tools/orden_pares.py */", ".text : {", "  KEEP(*(.crt0))"]
in_base = {k for _, k in base if k}
# pares cuyo origen no está en el orden L: al principio
for (o, s), ds in cr.items():
    if (o, s) in in_base:
        continue
    for x in [s] + ds:
        if (o, x) not in placed:
            out.append(ent(o, x)); placed.add((o, x))
for line, k in base:
    if line.strip() in ("", ".text : {", "}", "KEEP(*(.crt0))") or line.startswith("/*"):
        continue
    if k is None:
        out.append(line); continue
    if k in placed:
        continue
    out.append(line); placed.add(k)
    for d in cr.get(k, []):
        if (k[0], d) not in placed:
            out.append(ent(k[0], d)); placed.add((k[0], d))
out.append("}")
open(sys.argv[2], "w").write("\n".join(out) + "\n")
print(f"[pares] {len(cr)} secciones origen con saltos condicionales a otra sección; {len(placed)} secciones en el orden")
