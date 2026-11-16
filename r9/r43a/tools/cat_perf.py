#!/usr/bin/env python3
"""cat_perf.py <perf.data> <tid>: reparto por categorias del hilo de sonido (perf report --sort sym)."""
import re, subprocess, sys, collections
d, tid = sys.argv[1], sys.argv[2]
out = subprocess.run(["perf", "report", "-i", d, "--no-children", "--tid=" + tid, "--sort", "sym", "--stdio",
                      "--percentage", "relative"], capture_output=True, text=True).stdout
RANGES = [(0x80004000, 0x80006780, "invitado: MSL (memcpy...)"), (0x80195000, 0x801A0000, "invitado: Snd* del juego"),
          (0x801A0000, 0x801ABA00, "invitado: AX/AXFX/AI/DSP"), (0x801ABA00, 0x801D0000, "invitado: OS"),
          (0x801D0000, 0x801E0000, "invitado: DSP/DVD/SDK"), (0x80260000, 0x80285800, "invitado: nw4r::snd")]
cat = collections.Counter(); top = collections.defaultdict(list)
for l in out.splitlines():
    m = re.match(r"\s+([0-9.]+)%\s+\[(.)\]\s+(.*)", l)
    if not m: continue
    p, k, s = float(m.group(1)), m.group(2), m.group(3).strip()
    g = re.match(r"func_([0-9A-F]{8})", s)
    if k == "k": c = "kernel"
    elif g:
        a = int(g.group(1), 16); c = "invitado: otro"
        for lo, hi, n in RANGES:
            if lo <= a < hi: c = n
    elif "ax::" in s: c = "mezcla AX nativa (ax_hle)"
    elif "axfx" in s or "a2_delay" in s: c = "AXFX nativa (reverb/delay)"
    elif "clock" in s or "timebase" in s or "chrono" in s: c = "reloj"
    elif "sched_yield" in s or "nanosleep" in s or "futex" in s or "pthread_cond" in s: c = "ceder/dormir"
    elif "s2::" in s or "nwii_s2" in s: c = "S2 (sondeo, candado, peticiones)"
    elif "g7" in s or "psq" in s or "ps_" in s: c = "ayudantes g7 (bctrl, psq)"
    else: c = "otro: " + s[:60]
    cat[c] += p; top[c].append((p, s[:70]))
for c, p in sorted(cat.items(), key=lambda x: -x[1]):
    if p < 0.3: continue
    print("%6.1f %%  %s   [%s]" % (p, c, "; ".join("%s %.1f" % (s, q) for q, s in top[c][:3])))
