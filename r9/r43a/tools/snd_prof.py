#!/usr/bin/env python3
"""snd_prof.py <bench_dir|profile_snd.txt> <elf> [--top N]: reparto del hilo de sonido S2 en la consola
(profile_snd.txt de NWII_A_PROF_SND=1) por categorias y funciones (muestras propias), en % del tiempo de pared
del hilo (todas las muestras) y en % de lo ocupado (sin las esperas en el nucleo)."""
import bisect, collections, os, re, subprocess, sys
NM = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"
p, elf = sys.argv[1], sys.argv[2]
top = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 30
if os.path.isdir(p): p = os.path.join(p, "profile_snd.txt")
base = 0; pairs = []; guest = collections.Counter(); hdr = {}
for l in open(p):
    if l.startswith("base "): base = int(l.split()[1], 16)
    m = re.match(r"(\w+) (.*)", l)
    if m and m.group(1) in ("elapsed_s", "samples", "sampled_s", "interval_us"): hdr[m.group(1)] = m.group(2)
    m = re.match(r"(\d+) 0x([0-9a-f]+) 0x([0-9a-f]+)$", l.strip())
    if m: pairs.append((int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16)))
    m = re.match(r"guest (\d+) 0x([0-9a-f]+)", l)
    if m: guest[int(m.group(2), 16)] += int(m.group(1))
addrs, names = [], []
for line in subprocess.run([NM, "-n", "-S", "--defined-only", "-C", elf], capture_output=True, text=True).stdout.splitlines():
    q = line.split(None, 3)
    if len(q) >= 3 and len(q[1]) == 1: q = [q[0], "0", q[1], " ".join(q[2:])]  # sin tamano (asm)
    if len(q) == 4 and q[2] in "tTwW":
        addrs.append(int(q[0], 16)); names.append(q[3])
def sym(a):
    i = bisect.bisect_right(addrs, a) - 1
    return names[i] if i >= 0 else "?"
RANGES = [(0x80004000, 0x80006780, "invitado: MSL (memcpy...)"), (0x80195000, 0x801A0000, "invitado: Snd* del juego"),
          (0x801A0000, 0x801ABA00, "invitado: AX/AXFX/AI/DSP"), (0x801ABA00, 0x801D0000, "invitado: OS"),
          (0x801D0000, 0x801E0000, "invitado: DSP/DVD/SDK"), (0x80260000, 0x80285800, "invitado: nw4r::snd")]
WAIT = ("svcWaitSynchronization", "svcSleepThread", "svcArbitrateLock", "svcWaitProcessWideKeyAtomic", "svcWaitForAddress",
        "svcSendSyncRequest")
def cat(s):
    g = re.match(r"func_([0-9A-F]{8})", s)
    if any(w in s for w in WAIT): return "ESPERA (nucleo: dormido)"
    if g:
        a = int(g.group(1), 16)
        for lo, hi, n in RANGES:
            if lo <= a < hi: return n
        return "invitado: otro"
    if "ax::" in s: return "mezcla AX nativa (ax_hle)"
    if "axfx" in s or "a2_delay" in s: return "AXFX nativa (reverb/delay)"
    if "s2::" in s or "nwii_s2" in s: return "S2 (sondeo, candado, peticiones)"
    if "g7" in s or "psq" in s or "ps_" in s: return "ayudantes g7"
    if "svc" in s or "armGetSystemTick" in s: return "otras svc/reloj"
    if "mutex" in s.lower() or "condvar" in s.lower() or "pthread" in s or "__libnx" in s: return "libnx/pthread (candados)"
    if "hw::" in s or "dsp" in s.lower() or "MMIO" in s: return "hw DSP/AI/MMIO"
    return "otro runtime"
tot = sum(c for c, _, _ in pairs)
cats = collections.Counter(); fns = collections.Counter(); ex = collections.defaultdict(collections.Counter)
for c, pc, lr in pairs:
    s = sym(pc - base); k = cat(s); cats[k] += c; fns[s] += c; ex[k][s] += c
idle = cats["ESPERA (nucleo: dormido)"]; busy = tot - idle
print("%s: %s muestras (intervalo %s us, %s s muestreados); ocupado %.1f %% del tiempo de pared" %
      (p, tot, hdr.get("interval_us"), hdr.get("sampled_s"), 100.0 * busy / max(tot, 1)))
print("%-38s %7s %7s  principales" % ("categoria", "%pared", "%ocup"))
for k, c in cats.most_common():
    print("%-38s %6.1f %6.1f  %s" % (k, 100.0 * c / tot, 100.0 * c / max(busy, 1) if k[0] != "E" else 0,
          "; ".join("%s %.1f" % (s[:50], 100.0 * v / max(busy, 1)) for s, v in ex[k].most_common(3))))
print("\nfunciones (muestras propias, %% de lo ocupado):")
for s, c in fns.most_common(top + 3):
    if cat(s)[0] == "E": continue
    print("  %5.1f  %s" % (100.0 * c / max(busy, 1), s[:110]))
gt = sum(guest.values())
if gt:
    print("\npc invitado (ultimo destino de llamada/retorno; %d muestras):" % gt)
    for a, c in guest.most_common(15): print("  %5.1f  0x%08x" % (100.0 * c / gt, a))
