#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""aplicar_u0.py <parche.U0.patch> <archivo>

Aplica un parche unificado SIN contexto (diff -U0) a un único archivo, sin depender de
`patch` (vale en Windows). Comprueba que cada línea que se quita es exactamente la esperada;
si no, no escribe nada y sale con error. Los parches del port se guardan sin contexto a
propósito: así no llevan ni una línea del código generado del juego, solo lo escrito a mano.
"""
import re
import sys


def partir(texto):
    """Líneas con su '\n' (solo '\n' separa: splitlines() también cortaría en \x85, \x0c…)."""
    trozos = texto.split("\n")
    lineas = [t + "\n" for t in trozos[:-1]]
    if trozos[-1]:
        lineas.append(trozos[-1])
    return lineas

HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def leer_hunks(texto):
    hunks, actual = [], None
    for ln in partir(texto):
        if ln.startswith("---") or ln.startswith("+++"):
            if actual is None:
                continue
        m = HUNK.match(ln)
        if m:
            a, b = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
            actual = {"a": a, "b": b, "menos": [], "mas": []}
            hunks.append(actual)
        elif actual is not None and ln.startswith("-"):
            actual["menos"].append(ln[1:])
        elif actual is not None and ln.startswith("+"):
            actual["mas"].append(ln[1:])
        elif actual is not None and ln.startswith("\\"):
            # "\ No newline at end of file": quita el salto de la última línea añadida/quitada
            lista = actual["mas"] if actual["mas"] else actual["menos"]
            if lista and lista[-1].endswith("\n"):
                lista[-1] = lista[-1][:-1]
    return hunks


def aplicar(parche, archivo):
    with open(parche, encoding="latin-1", newline="") as f:
        hunks = leer_hunks(f.read())
    with open(archivo, encoding="latin-1", newline="") as f:
        lineas = partir(f.read())
    for h in sorted(hunks, key=lambda h: h["a"], reverse=True):
        # con -U0, «-a,0» significa «insertar DESPUÉS de la línea a»
        ini = h["a"] - 1 if h["b"] > 0 else h["a"]
        if len(h["menos"]) != h["b"]:
            raise SystemExit("aplicar_u0: trozo mal formado en %s (línea %d)" % (parche, h["a"]))
        if lineas[ini:ini + h["b"]] != h["menos"]:
            raise SystemExit("aplicar_u0: %s no casa con %s en la línea %d (¿otra versión del código generado?)"
                             % (parche, archivo, h["a"]))
        lineas[ini:ini + h["b"]] = h["mas"]
    with open(archivo, "w", encoding="latin-1", newline="") as f:
        f.write("".join(lineas))
    return len(hunks)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    n = aplicar(sys.argv[1], sys.argv[2])
    print("aplicar_u0: %d trozos aplicados a %s" % (n, sys.argv[2]))
