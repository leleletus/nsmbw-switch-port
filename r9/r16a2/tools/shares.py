#!/usr/bin/env python3
# shares.py <elf> <profile.txt>: game-thread samples (ms per second of profile) of the audio functions + totals
import subprocess, sys, bisect, re
elf, prof = sys.argv[1:3]
NM = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"
syms = []
for l in subprocess.run([NM, "-S", "-C", elf], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r"([0-9a-f]+) ([0-9a-f]+) [tTwW] (.*)", l)
    if m: syms.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
syms.sort(); starts = [s[0] for s in syms]
base = None; el = None; tot = 0; per = {}
for l in open(prof):
    if l.startswith("base "): base = int(l.split()[1], 16)
    if l.startswith("elapsed_s "): el = float(l.split()[1])
    p = l.split()
    if len(p) == 3 and p[1].startswith("0x"):
        c = int(p[0]); pc = int(p[1], 16) - base; tot += c
        i = bisect.bisect_right(starts, pc) - 1
        name = syms[i][2] if i >= 0 and pc < syms[i][0] + max(syms[i][1], 1) else "?"
        per[name] = per.get(name, 0) + c
groups = {
  "reverb (axfx_fast+k1/k2+native)": lambda n: "axfx_fast" in n or "a2_axfx_k" in n or "axfx_native" in n,
  "delay (func_801A5C10 + native)": lambda n: n.startswith("func_801A5C10") or "a2_delay_native" in n,
  "__AXSyncPBs func_801A2B10": lambda n: n.startswith("func_801A2B10"),
  "ax mix on game thread (run_loaded_list)": lambda n: "run_loaded_list" in n,
  "wait cond (svcWaitProcessWideKeyAtomic)": lambda n: n == "svcWaitProcessWideKeyAtomic",
  "mutex (svcArbitrateLock)": lambda n: n == "svcArbitrateLock",
  "sleep (svcSleepThread)": lambda n: n == "svcSleepThread",
}
sleep = per.get("svcSleepThread", 0)
print(f"{prof.split('/')[-2]}: {tot} samples / {el:.0f} s; busy (no svcSleepThread) {100*(tot-sleep)/tot:.1f} %")
for g, f in groups.items():
    s = sum(c for n, c in per.items() if f(n))
    print(f"  {g:42s} {s:6d}  {100*s/tot:5.2f} % of all  {100*s/max(tot-sleep,1):5.2f} % of busy  {s/el:6.1f} ms/s")
