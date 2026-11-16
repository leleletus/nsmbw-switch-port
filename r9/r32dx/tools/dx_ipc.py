#!/usr/bin/env python3
"""dx_ipc.py [--r60 gcov60] [--r30 gcov30] [--top N]: junta las cuentas exactas (PC) con el tiempo de la consola.

Entradas: datos/<r60>/resumen_main.json y datos/<r30>/resumen_main.json (tools/dx_report.py: misma lista de escenas,
el juego a 60 fot/s y frenado a ~30), datos/consamp_T01.json (tools/consamp.py: muestras de la consola por escena
y función, perfil del frente T) y dolphin/serie1.tsv (tools/dolphin_seg.py).

El trabajo por fotograma tiene una parte fija (lógica + dibujo) y otra proporcional al tiempo (sonido: 333
fotogramas AX por segundo, interrupciones). Con las dos corridas de PC se saca cada una y se lleva la cuenta al
tiempo de pared por fotograma que tuvo la consola en esa escena (interpolación lineal en ms/fotograma).
IPC efectivo = instrucciones AArch64 del código generado por fotograma / ciclos de la consola en código generado
por fotograma (muestras × 1020 MHz). El bucle ocioso del SO (func_801B4EA0) no cuenta en ningún lado.
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dolphin_seg  # noqa: E402

V = "/home/djvemo/nsmbw-port/r9/r32dx"
GEN = re.compile(r"(^func_|_8[0-9A-Fa-f]{7}$)")
IDLE_FN = "func_801B4EA0"
MHZ = 1020.0


RT_CATS = [
    ("escritura GX (FIFO, write-gather)", ("GX_WGPIPE", "wgp_", "wg_flush", "GX_")),
    ("sondeo de interrupciones y reloj", ("process_pending_callbacks", "poll_tb_refresh", "tick_timebase", "wall_timebase_now",
                                          "ppc_full", "ppc_tail", "EventScheduler", "read_timebase", "poll_", "armGetSystemTick")),
    ("ayudantes psq/ps/fp del runtime", ("psq_load", "psq_store", "ps_m", "ps_n", "ps_s", "ps_a", "ppc_single", "ppc_to_single", "ps_")),
    ("memoria lenta / MMIO", ("MMU::", "MMIODispatcher", "register_dsp", "hw::", "mmio")),
    ("sonido nativo (reverb A2/AXFX, AX)", ("axfx", "nwii_a2", "nwii_r9", "ax_", "AX")),
    ("cambio de contexto / interrupciones HLE", ("hle_", "fpu_", "context", "Dispatch")),
    ("núcleo (svc*) y E/S", ("svc", "DIDevice", "ioctl", "fs", "File")),
    ("libc (memcpy, memset...)", ("memcpy", "memset", "memmove", "strlen")),
]


def rt_cat(sym):
    for name, pats in RT_CATS:
        if any(p_ in sym for p_ in pats):
            return name
    return "otros"


def is_idle(s):
    return "svcSleepThread" in s or "idle_wait" in s or "nanosleep" in s or s == IDLE_FN


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--r60", default="gcov60")
    ap.add_argument("--r30", default="gcov30")
    ap.add_argument("--top", type=int, default=30)
    a = ap.parse_args()
    R60 = json.load(open(f"{V}/datos/{a.r60}/resumen_main.json"))
    R30 = json.load(open(f"{V}/datos/{a.r30}/resumen_main.json")) if os.path.exists(f"{V}/datos/{a.r30}/resumen_main.json") else {}
    CS = json.load(open(f"{V}/datos/consamp_T01.json"))
    DOL = dolphin_seg.dolphin_mean(f"{V}/dolphin/serie1.tsv")

    def noidle(r, k):
        t = {x["f"]: x for x in r["top"]}
        i = t.get(IDLE_FN, {"a64_fot": 0, "ppc_fot": 0})
        if k == "ppc_dolcyc_fot":
            return r[k] - i["ppc_fot"] * r["ppc_dolcyc_fot"] / r["ppc_fot"]
        return r[k] - i[{"a64_fot": "a64_fot", "ppc_fot": "ppc_fot", "a64_lo_fot": "a64_fot", "a64_hi_fot": "a64_fot"}[k]]

    print("== 1. Instrucciones PPC exactas por fotograma (port, sin el bucle ocioso) frente al original en Dolphin")
    print(f"{'escena':18s} {'PPC/fot 60':>11s} {'PPC/fot ~30':>12s} {'fijo/fot':>10s} {'por seg':>11s} {'ciclosD 60':>11s} {'Dolphin':>10s} {'port/Dolphin':>12s}")
    fix = {}
    for esc, r in R60.items():
        p60 = noidle(r, "ppc_fot")
        w60 = 1000.0 / r["fps_pc"]
        row = f"{esc:18s} {p60:11,.0f}"
        if esc in R30:
            p30 = noidle(R30[esc], "ppc_fot")
            w30 = 1000.0 / R30[esc]["fps_pc"]
            persec = (p30 - p60) / ((w30 - w60) / 1000.0)
            fixed = p60 - persec * w60 / 1000.0
            fix[esc] = (fixed, persec)
            row += f" {p30:12,.0f} {fixed:10,.0f} {persec:11,.0f}"
        else:
            row += f" {'-':>12s} {'-':>10s} {'-':>11s}"
        c60 = noidle(r, "ppc_dolcyc_fot")
        d = DOL.get(esc)
        row += f" {c60:11,.0f} {d:10,.0f} {c60 / d:12.2f}" if d else f" {c60:11,.0f}"
        print(row)

    print("\n== 2. Consola (m2/tp1, 1020 MHz, perfil T01) frente a las cuentas: ciclos por instrucción e IPC")
    print(f"{'escena':18s} {'ms pared':>8s} {'ms ocup':>8s} {'ms gen':>7s} {'ms rt':>6s} {'PPC/fot':>10s} {'A64/fot':>11s} {'A64/PPC':>7s} "
          f"{'cic/PPC':>7s} {'IPC gen':>7s} {'[cotas]':>13s}")
    per_scene = {}
    for esc, c in CS.items():
        if esc not in R60:
            continue
        fr = c["fotogramas"]
        wall = c["pared_ms"] / fr
        tot = c["muestras"]
        idle = sum(v for k, v in c["por_simbolo"].items() if is_idle(k))
        gen = sum(v for k, v in c["por_simbolo"].items() if GEN.search(k) and k != IDLE_FN)
        ms = lambda n: wall * n / tot  # noqa: E731
        busy_ms, gen_ms = ms(tot - idle), ms(gen)
        r6 = R60[esc]
        w60 = 1000.0 / r6["fps_pc"]
        if esc in R30:
            r3 = R30[esc]
            w30 = 1000.0 / r3["fps_pc"]
            al = (wall - w60) / (w30 - w60)
        else:
            r3, al = r6, 0.0
        ip = lambda k: noidle(r6, k) + al * (noidle(r3, k) - noidle(r6, k))  # noqa: E731
        ppc, a64, lo, hi = ip("ppc_fot"), ip("a64_fot"), ip("a64_lo_fot"), ip("a64_hi_fot")
        lo = r6["a64_lo_fot"] / r6["a64_fot"] * a64
        hi = r6["a64_hi_fot"] / r6["a64_fot"] * a64
        cyc_gen = gen_ms * MHZ * 1000.0
        print(f"{esc:18s} {wall:8.2f} {busy_ms:8.2f} {gen_ms:7.2f} {busy_ms - gen_ms:6.2f} {ppc:10,.0f} {a64:11,.0f} {a64 / ppc:7.2f} "
              f"{busy_ms * MHZ * 1000.0 / ppc:7.2f} {a64 / cyc_gen:7.2f} [{lo / cyc_gen:.2f}-{hi / cyc_gen:.2f}]")
        per_scene[esc] = dict(wall=wall, busy_ms=busy_ms, gen_ms=gen_ms, ppc=ppc, a64=a64, alpha=al, ipc=a64 / cyc_gen)
        # por función
        t6 = {x["f"]: x for x in r6["top"]}
        t3 = {x["f"]: x for x in r3["top"]}
        rows = []
        for k, v in c["por_simbolo"].items():
            if not GEN.search(k) or k == IDLE_FN:
                continue
            x6, x3 = t6.get(k), t3.get(k)
            if not x6:
                continue
            fa = x6["a64_fot"] + al * ((x3 or x6)["a64_fot"] - x6["a64_fot"])
            fp = x6["ppc_fot"] + al * ((x3 or x6)["ppc_fot"] - x6["ppc_fot"])
            fc = x6["llamadas_fot"] + al * ((x3 or x6)["llamadas_fot"] - x6["llamadas_fot"])
            rows.append((ms(v), fa, fp, fc, x6["trabajo_pct"], k))
        rows.sort(reverse=True)
        per_scene[esc]["func"] = rows[:200]
        cats = {}
        for k, v in c["por_simbolo"].items():
            if GEN.search(k) or is_idle(k):
                continue
            ct = "run_game (despacho central)" if k == "run_game" else rt_cat(k)
            cats[ct] = cats.get(ct, 0.0) + ms(v)
        per_scene[esc]["rt"] = cats
        rt6 = r6.get("runtime_fot", {})
        rt3 = r3.get("runtime_fot", {})
        per_scene[esc]["usos"] = {k: rt6[k] + al * (rt3.get(k, rt6[k]) - rt6[k]) for k in rt6}
        for k in ("llamadas_directas_fot", "despachos_fot", "entradas_funcion_fot", "sondeos_fot", "sondeos_lentos_fot"):
            if k in r6:
                per_scene[esc][k] = r6[k] + al * (r3.get(k, r6[k]) - r6[k])
    json.dump(per_scene, open(f"{V}/datos/ipc.json", "w"), indent=1)
    print("\n== 4. Runtime (código no generado) en el hilo del juego de la consola, ms por fotograma")
    names = sorted({k for d in per_scene.values() for k in d["rt"]}, key=lambda k: -sum(d["rt"].get(k, 0) for d in per_scene.values()))
    print(f"{'categoría':42s} " + " ".join(f"{e[:11]:>11s}" for e in per_scene))
    for k in names:
        print(f"{k:42s} " + " ".join(f"{d['rt'].get(k, 0):11.2f}" for d in per_scene.values()))
    print(f"{'TOTAL runtime':42s} " + " ".join(f"{sum(d['rt'].values()):11.2f}" for d in per_scene.values()))
    print("  cuentas exactas por fotograma (llevadas al ritmo de la consola):")
    for k in ("llamadas_directas_fot", "despachos_fot", "entradas_funcion_fot", "sondeos_fot", "sondeos_lentos_fot"):
        print(f"{k:42s} " + " ".join(f"{d.get(k, 0):11,.0f}" for d in per_scene.values()))
    for k in ("write8_io", "write16_io", "write32_io", "write_f32_io", "psq_l_fast", "psq_st_fast", "ppc_to_single"):
        print(f"{k:42s} " + " ".join(f"{d['usos'].get(k, 0):11,.0f}" for d in per_scene.values()))
    for esc, d in per_scene.items():
        print(f"\n== 3. {esc}: funciones por tiempo en la consola: ms/fot, % del generado, A64/fot, PPC/fot, A64/PPC, llamadas/fot, "
              f"IPC, ns por llamada, % trabajo")
        for msf, fa, fp, fc, tw, k in d["func"][:a.top]:
            cyc = msf * MHZ * 1000.0
            print(f"  {msf:6.3f} {100 * msf / d['gen_ms']:5.2f}% {fa:10,.0f} {fp:9,.0f} {fa / max(fp, 1):6.2f} {fc:9.1f} "
                  f"{fa / max(cyc, 1):5.2f} {1e6 * msf / max(fc, 1e-9):8.0f} {tw:5.1f}  {k}")
        # IPC agregado por tramos de tamaño de función
        cov = sum(x[0] for x in d["func"])
        print(f"  (las {len(d['func'])} primeras cubren {100 * cov / d['gen_ms']:.0f} % del tiempo en código generado)")


if __name__ == "__main__":
    main()
