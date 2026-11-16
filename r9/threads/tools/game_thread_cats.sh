#!/usr/bin/env bash
# Reparto del hilo del juego en un perfil perf de pc/prof.sh: % del hilo en escritura al FIFO de GX,
# AX/DSP HLE, sondeo de interrupciones/reloj, código del juego. Uso: game_thread_cats.sh <x.data> <x.txt>
d=$1; t=$2
tid=$(grep -m1 'game CPU thread' "$t" | sed -E 's/^== ([0-9]+):.*/\1/')
perf report -i "$d" --no-children --tid "$tid" --sort sym --stdio -q 2>/dev/null | python3 -I -c '
import re,sys
c={"wgp":0,"ax":0,"poll":0,"guest":0,"other":0}; tot=0
for l in sys.stdin:
    m=re.match(r"\s*([\d.]+)%\s+\[.\]\s+(.*)",l)
    if not m: continue
    p=float(m.group(1)); s=m.group(2); tot+=p
    if re.search(r"wgp_|GX_WGPIPE|wg_flush|wg_",s): c["wgp"]+=p
    elif re.search(r"runtime::ax::|ax_hle|dsp_|hw::dsp|ax_submit|ax_sync|ax_deliver",s): c["ax"]+=p
    elif re.search(r"process_pending_callbacks|clock_gettime|EventScheduler|ppc_tail|ppc_full|read_timebase",s): c["poll"]+=p
    elif re.search(r"(func_|_)8[0-9A-F]{7}\b",s) or s.startswith("func_"): c["guest"]+=p
    else: c["other"]+=p
print("game thread %.1f%% of samples: "%tot + "  ".join("%s %.1f%%"%(k,100*v/tot) for k,v in c.items()))
'
