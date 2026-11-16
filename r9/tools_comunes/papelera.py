#!/usr/bin/env python3
"""Mueve (no borra) las entradas del NIVEL 1 de LIMPIEZA_plan.md a .papelera_1115/ conservando la ruta.
uso: papelera.py [--hacer]   (sin --hacer solo lista)"""
import os, re, shlex, subprocess, sys
P = "/home/djvemo/nsmbw-port"
T = os.path.join(P, ".papelera_1115")
hacer = "--hacer" in sys.argv
txt = open(os.path.join(P, "r9/tools_comunes/LIMPIEZA_plan.md")).read()
n1 = txt.split("## NIVEL 1", 1)[1].split("## NIVEL 2", 1)[0]
PROHIBIDO = re.compile(r"(^|/)(nand|game|status/ESTADO|tools/switchctl|profiles)(/|$)|save|respaldo|\.gcda|r9/(r41m|r40d|r42k|r43a|r44w|r35m4|r17pgo|r33g7|r39r2|r37v|r38c|r36r|r31t|r32dx|tools_comunes|r24m2|r30g6c3|r34s2|r14g5|base|threads|render)(/|$)|r20g6/e2|uamlib_nx")
VIVOS_EXT = (".md", ".patch", ".py", ".sh")
filas = [l for l in n1.splitlines() if l.startswith("|") and "`" in l]
tot = 0; nmov = 0; saltadas = []
def mover(rel):
    global tot, nmov
    src = os.path.join(P, rel)
    if not os.path.lexists(src): return
    if PROHIBIDO.search(rel) or rel.endswith(VIVOS_EXT): saltadas.append(rel); return
    dst = os.path.join(T, rel)
    if hacer:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.rename(src, dst)
    nmov += 1
for l in filas:
    cmd = l.split("`")[1]
    gb = float(l.split("|")[1])
    if cmd.startswith("rm -rf "):
        rel = cmd[7:].strip()
        if " " in rel or rel.startswith("/") or ".." in rel: saltadas.append(cmd); continue
        if PROHIBIDO.search(rel): saltadas.append(rel); continue
        # no mover carpetas que contengan documentos o guiones: mover solo si no hay ninguno
        docs = subprocess.run(["find", os.path.join(P, rel), "-type", "f", "(", "-name", "*.md", "-o", "-name", "*.patch", "-o", "-name", "*.py", "-o", "-name", "*.sh", ")", "-print", "-quit"], capture_output=True, text=True).stdout.strip()
        if docs: saltadas.append(rel + "  (contiene docs/guiones: " + docs[len(P)+1:] + ")"); continue
        mover(rel); tot += gb
    elif cmd.startswith("find "):
        a = shlex.split(cmd)
        if a[-1] != "-delete" or a[1].startswith("/") or ".." in a[1]: saltadas.append(cmd); continue
        if PROHIBIDO.search(a[1]): saltadas.append(a[1]); continue
        out = subprocess.run(["find"] + a[1:-1] + ["-print0"], cwd=P, capture_output=True).stdout.split(b"\0")
        for f in out:
            if f: mover(f.decode("utf-8", "replace"))
        tot += gb
    else:
        saltadas.append(cmd)
print(("MOVIDO" if hacer else "SE MOVERIA") + f": {nmov} entradas/ficheros, ~{tot:.1f} GB segun el plan -> {T}")
print("saltadas:", len(saltadas))
for s in saltadas[:40]: print("  ", s[:160])
