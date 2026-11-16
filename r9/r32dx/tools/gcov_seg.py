#!/usr/bin/env python3
"""gcov_seg.py <runs/NOMBRE> [patrón de segmento ...]: .gcda de cada segmento -> cuentas exactas por línea.

Para cada carpeta runs/NOMBRE/dx/<nn>_<escena>_<seg>/ (volcada por dx.cpp con el binario NSMBW_dxgcov) corre
`gcov --json-format --stdout` sobre cada output_N.gcda (con los .gcno de pc_gcov/) y guarda en
datos/NOMBRE/<nn>_<escena>_<seg>.npz, por archivo N: líneas ejecutadas (L_N) y cuántas veces (C_N), más las
líneas instrumentadas con cuenta 0 (Z_N) y las entradas por función (JSON en 'funcs': nombre -> veces).
Las líneas de cabeceras (código integrado) van aparte en 'cab': por archivo N y cabecera, línea -> veces
(gcov las da sumadas sobre todas las funciones del archivo). Patrones: subcadenas del nombre de
la carpeta (por defecto todos los segmentos). JOBS (3 por defecto) procesos.
"""
import json
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

V = "/home/djvemo/nsmbw-port/r9/r32dx"


def one(args):
    seg, n = args
    gcda = os.path.join(seg, f"output_{n}.gcda")
    if not os.path.exists(gcda):
        return n, None
    inst_p = os.path.join(V, "datos", "srcmap", f"inst_{n}.npy")
    if os.path.getsize(gcda) < 128 and os.path.exists(inst_p):
        # .gcda sin contadores (ese archivo no se ejecutó en el segmento): todas sus líneas a cero
        return n, (np.zeros(0, dtype=np.int32), np.zeros(0, dtype=np.int64), np.load(inst_p), {}, {})
    gcno = os.path.join(seg, f"output_{n}.gcno")
    if not os.path.exists(gcno):
        os.symlink(os.path.join(V, "pc_gcov", f"output_{n}.gcno"), gcno)
    out = subprocess.run(["gcov", "--json-format", "--stdout", f"output_{n}.gcda"], cwd=seg, capture_output=True).stdout
    j = json.loads(out)
    L, C, Z, funcs = [], [], [], {}
    hdr = {}
    want = f"output_{n}.cpp"
    for f in j["files"]:
        if os.path.basename(f["file"]) != want:
            # cabeceras: cuenta por línea, sumada sobre todas las funciones de este archivo (así lo da gcov)
            h = hdr.setdefault(os.path.basename(f["file"]), {})
            for ln in f["lines"]:
                h[ln["line_number"]] = h.get(ln["line_number"], 0) + ln["count"]
            continue
        for fn in f["functions"]:
            if fn["execution_count"]:
                funcs[fn["name"]] = fn["execution_count"]
        for ln in f["lines"]:
            c = ln["count"]
            if c:
                L.append(ln["line_number"])
                C.append(c)
            else:
                Z.append(ln["line_number"])
    os.unlink(gcno)
    if not os.path.exists(inst_p):
        tmp = inst_p + f".{os.getpid()}.npy"
        np.save(tmp, np.array(sorted(L + Z), dtype=np.int32))
        os.replace(tmp, inst_p)
    return n, (np.array(L, dtype=np.int32), np.array(C, dtype=np.int64), np.array(Z, dtype=np.int32), funcs, hdr)


def main():
    run = sys.argv[1].rstrip("/")
    pats = sys.argv[2:]
    name = os.path.basename(run)
    os.makedirs(os.path.join(V, "datos", name), exist_ok=True)
    segs = sorted(d for d in os.listdir(os.path.join(run, "dx")) if not pats or any(p in d for p in pats))
    with ProcessPoolExecutor(max_workers=int(os.environ.get("JOBS", "3"))) as ex:
        for s in segs:
            dst = os.path.join(V, "datos", name, s + ".npz")
            if os.path.exists(dst):
                continue
            segd = os.path.join(run, "dx", s)
            arrs, funcs, tot, hdrs = {}, {}, 0, {}
            for n, r in ex.map(one, [(segd, n) for n in range(87)]):
                if r is None:
                    continue
                arrs[f"L_{n}"], arrs[f"C_{n}"], arrs[f"Z_{n}"] = r[0], r[1], r[2]
                funcs.update(r[3])
                hdrs[n] = r[4]
                tot += int(r[1].sum())
            np.savez_compressed(dst, funcs=json.dumps(funcs), cab=json.dumps(hdrs), **arrs)
            print(f"{s}: {len(funcs)} funciones ejecutadas, suma de cuentas de línea {tot}", flush=True)


if __name__ == "__main__":
    main()
