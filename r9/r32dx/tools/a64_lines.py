#!/usr/bin/env python3
"""a64_lines.py <elf con -g1> <salida.npz>: línea de fuente de cada instrucción AArch64 (tabla DWARF .debug_line).

Índice i = dirección / 4.  fil[i] = N si la instrucción viene de una línea de output_N.cpp; <= -10 si viene de una
cabecera (código integrado: MemView, fcmp, psq...): cabecera nº -(fil+10) de la lista 'cab'; -2 si no hay
información. lin[i] = número de línea.
Lee la salida de `objdump --dwarf=decodedline` en flujo.
"""
import json
import os
import re
import subprocess
import sys

import numpy as np

DKP = os.environ.get("DEVKITPRO", "/opt/devkitpro")
OD = os.path.join(DKP, "devkitA64", "bin", "aarch64-none-elf-objdump")
RE = os.path.join(DKP, "devkitA64", "bin", "aarch64-none-elf-readelf")


def main():
    elf, dst = sys.argv[1], sys.argv[2]
    tsize = 0
    for ln in subprocess.run([RE, "-S", "-W", elf], capture_output=True, text=True).stdout.splitlines():
        m = re.match(r"\s*\[\s*\d+\] \.text\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)", ln)
        if m:
            tsize = int(m.group(2), 16)
    n = tsize // 4
    A, F, L = [], [], []
    fcache = {}
    cab = []
    rx = re.compile(r"output_(\d+)\.cpp$")
    p = subprocess.Popen([OD, "--dwarf=decodedline", elf], stdout=subprocess.PIPE, text=True, bufsize=1 << 20)
    rows = 0
    for line in p.stdout:
        q = line.split()
        if len(q) < 3 or not q[2].startswith("0x") and q[2] != "0":
            continue
        try:
            addr = int(q[2], 16)
        except ValueError:
            continue
        if q[1] == "-":            # fin de secuencia
            f, l = -2, 0
        else:
            try:
                l = int(q[1])
            except ValueError:
                continue
            f = fcache.get(q[0])
            if f is None:
                m = rx.search(q[0])
                if m:
                    f = int(m.group(1))
                else:
                    cab.append(q[0])
                    f = -(10 + len(cab) - 1)
                fcache[q[0]] = f
        A.append(addr)
        F.append(f)
        L.append(l)
        rows += 1
    p.wait()
    A = np.array(A, dtype=np.int64)
    F = np.array(F, dtype=np.int16)
    L = np.array(L, dtype=np.int32)
    # orden estable por dirección: para la misma dirección gana la ÚLTIMA fila (estado final antes de emitir),
    # salvo los fines de secuencia, que no deben tapar el comienzo de la secuencia siguiente
    key = A * 2 + (F != -2)
    o = np.argsort(key, kind="stable")
    A, F, L = A[o], F[o], L[o]
    last = np.ones(len(A), dtype=bool)
    last[:-1] = A[1:] != A[:-1]
    A, F, L = A[last], F[last], L[last]
    slot = np.arange(n, dtype=np.int64) * 4
    idx = np.searchsorted(A, slot, side="right") - 1
    file = np.where(idx >= 0, F[np.maximum(idx, 0)], -2).astype(np.int16)
    line = np.where(idx >= 0, L[np.maximum(idx, 0)], 0).astype(np.int32)
    np.savez_compressed(dst, fil=file, lin=line, cab=json.dumps(cab))
    print(rows, "filas;", n, "instrucciones:", int((file >= 0).sum()), "en output_N.cpp,", int((file <= -10).sum()),
          "en cabeceras,", int((file == -2).sum()), "sin línea")


if __name__ == "__main__":
    main()
