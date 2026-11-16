#!/usr/bin/env python3
"""[r44w] stats_escena.py <bench> [<bench>...]: medias por escena de las líneas periódicas de nwii.log
([NX] game, [WPROF], [RPROF], [GXPROF], [PIPE], [S2]) para comparar agua con las demás escenas.
Se descartan los primeros 5 s de cada demo (carga), como por_escena_real.py."""
import collections
import re
import sys

ESC = ["desierto4j", "agua_pinguino", "cielo_paratroopas", "nubes_plataformas", "castillo"]
CAMPOS = [
    ("NX", r"^\[NX\] game ([\d.]+) fps", "fps_nx"),
    ("NX", r"wgp (\d+)k writes/s ([\d.]+)k flushes/s \((\d+) B; bp ([\d.]+)k/s, age ([\d.]+)k/s\)", "wgp_k wflush_k wflush_B wbp_k wage_k"),
    ("NX", r"slow polls (\d+)k/s \(full (\d+)/s, tail (\d+)/s, clock (\d+)k/s\)", "polls_k pfull ptail pclock_k"),
    ("NX", r"threads main (\d+)%, cpu (\d+)%, aud (\d+)%, gxw (\d+)%, snd (\d+)%", "th_main th_cpu th_aud th_gxw th_snd"),
    ("NX", r"interp (\d+)/s", "interp"),
    ("WPROF", r"busy_us/frame=(\d+) lock=(\d+) parse=(\d+) scan=(\d+) clear=(\d+) split=(\d+) decode=(\d+)", "w_busy w_lock w_parse w_scan w_clear w_split w_decode"),
    ("WPROF", r"cmds/frame=(\d+) draws/frame=(\d+) dl_hit_us/frame=(\d+) dl_hit_cmds/frame=(\d+) dl_miss_us/frame=(\d+)", "w_cmds w_draws w_dlhit_us w_dlhit_cmds w_dlmiss_us"),
    ("WPROF", r"render_freed=(\d+) \((\d+) us each\)", "w_freed w_free_us"),
    ("RPROF", r"frames=(\d+) render_us/frame=(\d+) render_max_us=(\d+)", "r_frames r_us r_max"),
    ("RPROF", r"gx_draws/frame=(\d+) gl_draws/frame=(\d+) stitches/frame=(\d+)", "r_gxdraws r_gldraws r_stitch"),
    ("RPROF", r"dropped_frames=(\d+) tex_uploads=(\d+) tex_bind_fixes=(\d+)", "r_dropped r_texup r_texfix"),
    ("GXPROF", r"parse\+decode_ms=(\d+) \(decode_ms=(\d+)\) draws_snap=(\d+) draw_bytes=(\d+) dl_calls=(\d+) dl_bytes=(\d+)", "g_pd_ms g_dec_ms g_draws g_drawB g_dlcalls g_dlB"),
    ("PIPE", r"game frames=(\d+) interval avg=([\d.]+)", "p_frames p_int"),
    ("PIPE", r"worker frame-end->ready avg=([\d.]+)", "p_wready"),
    ("PIPE", r"draw avg=([\d.]+)", "p_draw"),
    ("S2", r"sonido: irq ([\d.]+) ms/s, hilo ([\d.]+) \+ ([\d.]+) ms/s, espera S ([\d.]+) ms/s", "s_irq s_hilo s_hilo2 s_esperaS"),
    ("S2", r"mutex (\d+)/s \(fallo (\d+)/s, espera (\d+)/s, sin juego (\d+)/s, ([\d.]+) ms/s\)", "s_mtx s_mtxf s_mtxw s_mtxsj s_mtxms"),
    ("S2", r"juego: S secc (\d+)/s irq (\d+)/s, espera ([\d.]+) ms/s en (\d+)/s", "s_jsecc s_jirq s_jespera s_jesperan"),
]


def main():
    for b in sys.argv[1:]:
        acc = collections.defaultdict(lambda: collections.defaultdict(list))
        sc = None
        t0 = 0
        nline = 0
        for l in open(b + "/nwii.log", errors="replace"):
            m = re.match(r"\[DEMO\] start (\S+) at ([\d.]+)", l)
            if m:
                sc, t0, nline = m.group(1), float(m.group(2)), 0
                continue
            if l.startswith("[DEMO] end"):
                sc = None
                continue
            if sc is None:
                continue
            if l.startswith("[NX] game"):
                nline += 1
            if nline <= 5:   # [NX] cada segundo: saltar ~5 s de carga
                continue
            for tag, rx, names in CAMPOS:
                if not l.startswith("[" + tag + "]"):
                    continue
                m = re.search(rx, l)
                if m:
                    for n, v in zip(names.split(), m.groups()):
                        acc[sc][n].append(float(v))
        print("== " + b)
        keys = []
        for _, _, names in CAMPOS:
            keys += names.split()
        print("%-14s" % "campo" + "".join("%11s" % e[:10] for e in ESC))
        for k in keys:
            vals = []
            for e in ESC:
                v = acc[e].get(k)
                vals.append("%11.1f" % (sum(v) / len(v)) if v else "%11s" % "-")
            print("%-14s" % k + "".join(vals))


if __name__ == "__main__":
    main()
