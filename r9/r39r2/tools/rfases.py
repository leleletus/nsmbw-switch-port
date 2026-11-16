#!/usr/bin/env python3
"""rfases.py <bench_dir>...: por escena (lista fija), media de las lineas [RPHASE] (us por fotograma dibujado y
contadores por fotograma) y de [RPROF] (gl_draws, gx_draws, tex_uploads...). Se salta las 2 primeras lineas de cada
demo (carga). Frente R2 (r9/r39r2)."""
import re, sys
from collections import defaultdict
KV = re.compile(r"([a-zA-Z_/]+)=([0-9.]+)")
def per_scene(path):
    sc = defaultdict(lambda: dict(rp=defaultdict(list), pr=defaultdict(list)))
    cur, sk = None, {}
    for l in open(path, errors="replace"):
        if l.startswith("[DEMO] start "):
            cur = l.split()[2]; sk = {"RPHASE": 2, "RPROF": 2}
        elif l.startswith("[DEMO] end "):
            cur = None
        elif cur and (l.startswith("[RPHASE]") or l.startswith("[RPROF]")):
            t = l[1:l.index("]")]
            if sk[t]: sk[t] -= 1; continue
            d = sc[cur]["rp" if t == "RPHASE" else "pr"]
            for k, v in KV.findall(l):
                d[k].append(float(v))
    return sc
RPK = ["total", "regs", "state", "tex", "emit", "upload", "gldraw", "copy", "frame"]
CNT = ["gx", "dirty", "keychg", "flush", "verts", "idx", "upKB", "emit_hits"]
PRK = ["render_us/frame", "gx_draws/frame", "gl_draws/frame", "tex_uploads", "dropped_frames", "retracts", "shaders"]
for p in sys.argv[1:]:
    name = p.rstrip("/").split("/")[-1]
    sc = per_scene(p.rstrip("/") + "/nwii.log")
    print("==", name)
    for sid, d in sc.items():
        rp, pr = d["rp"], d["pr"]
        m = lambda dd, k: (sum(dd[k]) / len(dd[k])) if dd.get(k) else float("nan")
        if rp:
            print("  %-18s RPHASE ms/fot: " % sid + " ".join("%s=%.2f" % (k, m(rp, k) / 1000) for k in RPK)
                  + " | por fot: " + " ".join("%s=%.0f" % (k, m(rp, k)) for k in CNT if k in rp))
        if pr:
            print("  %-18s RPROF: " % sid + " ".join("%s=%.1f" % (k.replace("/frame", ""), m(pr, k)) for k in PRK if k in pr))
