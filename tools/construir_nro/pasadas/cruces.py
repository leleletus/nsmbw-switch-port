#!/usr/bin/env python3
"""cruces.py <carpeta de objetos> > cruces.tsv
Saltos condicionales (b.cond/cbz/cbnz = R_AARCH64_CONDBR19, ±1 MiB; tbz/tbnz = R_AARCH64_TSTBR14, ±32 KiB) que van
de una sección a OTRA sección de código (con -freorder-blocks-and-partition: de .text[.hot].F a .text.unlikely.F, la
parte fría). GNU ld no pone veneros para esos saltos, así que las dos secciones tienen que quedar cerca: el orden de
enlace (orden_pares.py) las pone juntas. Salida: objeto<TAB>tipo<TAB>sección origen<TAB>sección destino."""
import os, re, subprocess, sys

R = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitA64/bin/aarch64-none-elf-readelf")  # [construir_nro]
root = sys.argv[1]
for dp, _, fs in os.walk(root):
    for f in sorted(fs):
        if not f.endswith(".o"):
            continue
        p = os.path.join(dp, f)
        out = subprocess.run([R, "-rW", p], capture_output=True, text=True).stdout
        src = None
        seen = set()
        for l in out.splitlines():
            m = re.match(r"Relocation section '\.rela(\.text[^']*)'", l)
            if m:
                src = m.group(1)
                continue
            if src is None or ("CONDBR19" not in l and "TSTBR14" not in l):
                continue
            t = l.split()
            # offset info type value name + addend
            name = t[4] if len(t) > 4 else ""
            if name.startswith(".text") and name != src:
                k = (f, t[2], src, name)
                if k not in seen:
                    seen.add(k)
                    print("\t".join(k))
