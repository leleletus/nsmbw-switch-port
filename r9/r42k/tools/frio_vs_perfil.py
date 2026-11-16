#!/usr/bin/env python3
"""frio_vs_perfil.py <lista_frias.txt> <perfil>@<elf> ...

¿Qué parte del tiempo del hilo del juego cae en funciones que una variante sin -fprofile-partial-training (k2n) manda
enteras a .text.unlikely (= no ejecutadas en el entrenamiento: optimizadas para tamaño y apartadas)?
lista_frias.txt: un nombre de función por línea (sale de los .o de k2n: secciones .text.unlikely.<fn> que no son .cold).
perfil: profile_cpu.txt de switch_profiler ("count pc lr", pc - base = dirección del .elf).
Imprime: muestras totales, en código generado (func_*), en funciones frías de k2n, y las 15 funciones frías con más muestras.
"""
import bisect, collections, subprocess, sys

frias = set(l.strip() for l in open(sys.argv[1]) if l.strip())
# los perfiles de otra compilación (m2) usan a veces el nombre sin _g6; en m4/m6a la copia usada es <nombre>_g6
import os
_symf = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), "k2n_syms.txt")
todos = set(l.strip() for l in open(_symf)) if os.path.exists(_symf) else set()
def norm(n):
    return n + "_g6" if (n + "_g6") in todos else n
NM = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"
for arg in sys.argv[2:]:
    prof, elf = arg.split("@")
    syms = []
    for l in subprocess.run([NM, "-S", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 4 and p[2] in "tTwW":
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    syms.sort()
    addrs = [s[0] for s in syms]
    base = None
    cnt = collections.Counter()
    tot = 0
    for l in open(prof):
        if l.startswith("base "):
            base = int(l.split()[1], 16)
            continue
        p = l.split()
        if len(p) < 2 or not p[0].isdigit():
            continue
        c, pc = int(p[0]), int(p[1], 16) - base
        tot += c
        i = bisect.bisect_right(addrs, pc) - 1
        name = "?"
        if i >= 0 and pc < syms[i][0] + max(syms[i][1], 1):
            name = syms[i][2]
        cnt[norm(name)] += c
    gen = sum(v for k, v in cnt.items() if k.startswith("func_"))
    fr = {k: v for k, v in cnt.items() if k in frias}
    nfr = sum(fr.values())
    print(f"{prof}\n  muestras {tot}; código generado (func_*) {gen} ({100*gen/max(tot,1):.1f} %); "
          f"en funciones frías de k2n {nfr} = {100*nfr/max(tot,1):.2f} % del hilo, {100*nfr/max(gen,1):.2f} % del generado")
    for k, v in sorted(fr.items(), key=lambda x: -x[1])[:15]:
        print(f"    {v:7d} {100*v/max(tot,1):5.2f} %  {k}")
