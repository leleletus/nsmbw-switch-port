#!/usr/bin/env python3
"""dx_tablas.py [r60=gcov60] [r30=gcov30u]: las tablas del LEEME en Markdown, a partir de los JSON de datos/."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dolphin_seg  # noqa: E402
import dxlib  # noqa: E402

V = "/home/djvemo/nsmbw-port/r9/r32dx"
r60 = sys.argv[1] if len(sys.argv) > 1 else "gcov60"
r30 = sys.argv[2] if len(sys.argv) > 2 else "gcov30u"
A = json.load(open(f"{V}/datos/{r60}/resumen_main.json"))
B = json.load(open(f"{V}/datos/{r30}/resumen_main.json")) if os.path.exists(f"{V}/datos/{r30}/resumen_main.json") else {}
D = dolphin_seg.dolphin_mean(f"{V}/dolphin/serie1.tsv")
IPC = json.load(open(f"{V}/datos/ipc.json")) if os.path.exists(f"{V}/datos/ipc.json") else {}
IDLE = "func_801B4EA0"
esc = list(A)
M = lambda x: f"{x / 1e6:.2f}".replace(".", ",")  # noqa: E731
N = lambda x, d=0: f"{x:,.{d}f}".replace(",", " ").replace(".", ",")  # noqa: E731


def idle(r, k="ppc_fot"):
    for x in r["top"]:
        if x["f"] == IDLE:
            return x[k]
    return 0.0


print("### T1. Instrucciones PPC por fotograma (PC, juego a 60 fot/s): port frente al original en Dolphin\n")
print("| Escena | port: PPC exactas (M) | de ellas bucle ocioso | port sin ocioso (M) | en ciclos de Dolphin (M) | **Dolphin: ciclos ocupados (M)** | % del Broadway ocupado | port / original | original − port (M) | `inst_count` del runtime (M) |")
print("|---|---|---|---|---|---|---|---|---|---|")
for e in esc:
    r = A[e]
    p = r["ppc_fot"]
    i = idle(r)
    c = r["ppc_dolcyc_fot"] * (p - i) / p
    d = D.get(e, 0)
    print(f"| {e} | {M(p)} | {M(i)} | {M(p - i)} | {M(c)} | **{M(d)}** | {100 * d / 12.162e6:.1f} | {c / d:.2f} | {M(d - c)} | {M(r['inst_count_fot'])} |")

if B:
    print("\n### T2. Parte fija y parte proporcional al tiempo (corrida frenada de forma uniforme)\n")
    print("| Escena | ms/fot 60 | ms/fot frenada | PPC/fot a 60 (M) | PPC/fot frenada (M) | fijo por fotograma (M) | por segundo (M/s) | a 30 fot/s (M) |")
    print("|---|---|---|---|---|---|---|---|")
    for e in esc:
        if e not in B:
            continue
        a, b = A[e], B[e]
        w6, w3 = 1000 / a["fps_pc"], 1000 / b["fps_pc"]
        p6, p3 = a["ppc_fot"] - idle(a), b["ppc_fot"] - idle(b)
        ps = (p3 - p6) / ((w3 - w6) / 1000) if abs(w3 - w6) > 2 else float("nan")
        fx = p6 - ps * w6 / 1000
        if ps != ps:
            print(f"| {e} | {w6:.1f} | {w3:.1f} | {M(p6)} | {M(p3)} | — | — | — |")
        else:
            print(f"| {e} | {w6:.1f} | {w3:.1f} | {M(p6)} | {M(p3)} | {M(fx)} | {ps / 1e6:.1f} | {M(fx + ps / 30)} |")

print("\n### T3. Instrucciones AArch64 del código generado de m2 por fotograma (a 60 fot/s, sin el bucle ocioso)\n")
print("| Escena | AArch64/fot (M) | cotas (M) | AArch64 por PPC | % trabajo | % protocolo (pc/lr, volcado, recarga, ic, entrada) | llamadas directas | indirectas | sondeos |")
print("|---|---|---|---|---|---|---|---|---|")
for e in esc:
    r = A[e]
    ia, ip = idle(r, "a64_fot"), idle(r)
    a = r["a64_fot"] - ia
    cf = r["clases_fuente"]
    tot = sum(cf.values())
    proto = sum(cf.get(k, 0) for k in ("pc_lr", "volcado", "recarga", "ic", "entrada_carga", "entrada_switch", "llamada", "return", "despacho"))
    print(f"| {e} | {M(a)} | {M(r['a64_lo_fot'] - ia)}–{M(r['a64_hi_fot'] - ia)} | {a / (r['ppc_fot'] - ip):.2f} | {r['trabajo_pct']:.0f} | {100 * proto / tot:.0f} | "
          f"{N(r['llamadas_directas_fot'])} | {N(r['despachos_fot'])} | {N(r.get('sondeos_fot', 0))} |")

print("\n### T4. Clases de sentencia y de instrucción (% de las AArch64 ejecutadas)\n")
keys = sorted({k for e in esc for k in A[e]["clases_fuente"]}, key=lambda k: -A["desierto4j"]["clases_fuente"].get(k, 0))
print("| Clase de sentencia del fuente | " + " | ".join(esc) + " |")
print("|---|" + "---|" * len(esc))
for k in keys:
    print(f"| {k} | " + " | ".join(f"{100 * A[e]['clases_fuente'].get(k, 0) / sum(A[e]['clases_fuente'].values()):.1f}" for e in esc) + " |")


def grp(k):
    for p, n in (("ctx.gpr", "ctx.gpr"), ("ctx.cr", "ctx.cr"), ("ctx.pc", "ctx.pc/lr"), ("ctx.lr", "ctx.pc/lr"), ("ctx.inst_count", "ctx.inst_count"),
                 ("ctx.fpr", "ctx.fpr/ps1"), ("ctx.ps1", "ctx.fpr/ps1"), ("ctx.", "ctx (msr, sondeo, mmu, ctr, xer...)"),
                 ("stack", "pila"), ("guest", "memoria del juego"), ("other-mem", "otra memoria")):
        if k.startswith(p):
            return n + (" (carga)" if k.endswith(" ld") else " (almacén)" if k.endswith(" st") else "")
    return k


G = {}
for e in esc:
    d = {}
    for k, v in A[e]["clases_a64"].items():
        d[grp(k)] = d.get(grp(k), 0) + v
    G[e] = d
keys = sorted({k for e in esc for k in G[e]}, key=lambda k: -G["desierto4j"].get(k, 0))
print("\n| Clase de instrucción AArch64 | " + " | ".join(esc) + " |")
print("|---|" + "---|" * len(esc))
for k in keys:
    print(f"| {k} | " + " | ".join(f"{100 * G[e].get(k, 0) / sum(G[e].values()):.1f}" for e in esc) + " |")

print("\n### T5. Huella de código AArch64 ejecutado\n")
print("| Escena | funciones ejecutadas | su tamaño total (KB) | KB ejecutados (ventana de ~40 s) | líneas de 64 B | páginas de 4 KB | 50 % del trabajo en (KB) | 90 % | 99 % |")
print("|---|---|---|---|---|---|---|---|---|")
for e in esc:
    r = A[e]
    print(f"| {e} | {r['funciones_ejecutadas']} | {N(r['kb_funciones_ejecutadas'])} | {N(r['huella_bytes'] / 1024)} | {N(r['huella_lineas64'])} | {N(r['huella_paginas4k'])} | "
          f"{N(r['bytes_50pct'] / 1024)} | {N(r['bytes_90pct'] / 1024)} | {N(r['bytes_99pct'] / 1024)} |")
hp = f"{V}/datos/gcovpf/huella.json"
if os.path.exists(hp):
    H = json.load(open(hp))
    print("\n| Escena (un periodo de VI, media de 8) | KB ejecutados | líneas de 64 B | KB en líneas | páginas de 4 KB | funciones | 90 % del trabajo en (KB) | se repite del anterior | unión de 8 (KB) |")
    print("|---|---|---|---|---|---|---|---|---|")
    for e, h in H.items():
        print(f"| {e} | {N(h['kb_ejec'])} | {N(h['lineas'])} | {N(h['lineas'] * 64 / 1024)} | {N(h['paginas'])} | {N(h['funciones'])} | {N(h['kb90'])} | {h['repite_pct']:.0f} % | {N(h['union_kb'])} |")

if IPC:
    print("\n### T6. Consola (perfil T01, 1020 MHz) frente a las cuentas llevadas a su ritmo\n")
    print("| Escena | ms de pared/fot | ms ocupado | ms código generado | ms runtime | PPC/fot (M) | AArch64/fot (M) | ciclos por PPC (todo) | **IPC del código generado** | cotas |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for e, d in IPC.items():
        cg = d["gen_ms"] * 1.02e6
        r = A[e]
        lo = (r["a64_lo_fot"]) / r["a64_fot"] * d["a64"]
        hi = (r["a64_hi_fot"]) / r["a64_fot"] * d["a64"]
        print(f"| {e} | {d['wall']:.1f} | {d['busy_ms']:.1f} | {d['gen_ms']:.1f} | {d['busy_ms'] - d['gen_ms']:.1f} | {M(d['ppc'])} | {M(d['a64'])} | "
              f"{d['busy_ms'] * 1.02e6 / d['ppc']:.1f} | **{d['a64'] / cg:.2f}** | {lo / cg:.2f}–{hi / cg:.2f} |")
    print("\n| Runtime en el hilo del juego (ms/fot) | " + " | ".join(IPC) + " |")
    print("|---|" + "---|" * len(IPC))
    names = sorted({k for d in IPC.values() for k in d["rt"]}, key=lambda k: -sum(d["rt"].get(k, 0) for d in IPC.values()))
    for k in names:
        print(f"| {k} | " + " | ".join(f"{d['rt'].get(k, 0):.2f}" for d in IPC.values()) + " |")
    print("| **total** | " + " | ".join(f"**{sum(d['rt'].values()):.2f}**" for d in IPC.values()) + " |")
    print("\n| Cuentas exactas por fotograma (ritmo de la consola) | " + " | ".join(IPC) + " |")
    print("|---|" + "---|" * len(IPC))
    for k, t in (("entradas_funcion_fot", "entradas a función"), ("llamadas_directas_fot", "llamadas directas"), ("despachos_fot", "llamadas indirectas (nwii_dispatch)"),
                 ("sondeos_fot", "sondeos de interrupción (en línea)"), ("sondeos_lentos_fot", "sondeos lentos (1 de cada 16)")):
        print(f"| {t} | " + " | ".join(N(d.get(k, 0)) for d in IPC.values()) + " |")
    for k in ("write8_io", "write16_io", "write32_io", "write_f32_io", "psq_l_fast", "psq_st_fast", "ppc_to_single"):
        print(f"| `{k}` | " + " | ".join(N(d["usos"].get(k, 0)) for d in IPC.values()) + " |")
    for e, d in IPC.items():
        print(f"\n#### {e}: las 25 funciones con más tiempo en la consola\n")
        print("| ms/fot | % del generado | función | subsistema | AArch64/fot | PPC/fot | AArch64/PPC | llamadas/fot | IPC | ns por llamada | % trabajo |")
        print("|---|---|---|---|---|---|---|---|---|---|---|")
        for msf, fa, fp, fc, tw, k in d["func"][:25]:
            try:
                lib = dxlib.lib_of(int(k[-8:], 16))
            except ValueError:
                lib = "?"
            print(f"| {msf:.3f} | {100 * msf / d['gen_ms']:.1f} | `{k[-40:]}` | {lib} | {N(fa)} | {N(fp)} | {fa / max(fp, 1):.1f} | {N(fc, 1)} | {fa / max(msf * 1.02e6, 1):.2f} | "
                  f"{N(1e6 * msf / max(fc, 1e-9))} | {tw:.0f} |")

print("\n### T7. Subsistemas por instrucciones AArch64 por fotograma (a 60 fot/s)\n")
libs = sorted({k for e in esc for k in A[e]["bibliotecas"]}, key=lambda k: -sum(A[e]["bibliotecas"].get(k, [0, 0])[0] for e in esc))
print("| Subsistema (AArch64 k/fot · PPC k/fot · ratio) | " + " | ".join(esc) + " |")
print("|---|" + "---|" * len(esc))
for k in libs[:22]:
    print(f"| {k} | " + " | ".join(
        (lambda v: f"{v[0] / 1e3:,.0f} · {v[1] / 1e3:,.0f} · {v[0] / max(v[1], 1):.1f}".replace(",", " "))(A[e]["bibliotecas"].get(k, [0, 0])) for e in esc) + " |")
