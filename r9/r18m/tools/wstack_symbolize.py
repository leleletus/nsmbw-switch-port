#!/usr/bin/env python3
# wstack_symbolize.py --elf <nsmbw.elf> <profile_cpu.txt> [--top 40]
#   Reads the "wstack count pc lr ret..." lines the r16w profiler writes for samples taken in a
#   kernel wait (svcArbitrateLock / svcWaitProcessWideKeyAtomic / svcSleepThread /
#   svcWaitSynchronization) and prints, per wait svc, the most frequent return-address chains
#   (stack scan: the first code addresses found above sp; a stale slot can show up as well).
#   Run with python3 -I.
import argparse, bisect, collections, os, subprocess

NM = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"

ap = argparse.ArgumentParser()
ap.add_argument("--elf", required=True)
ap.add_argument("profile")
ap.add_argument("--top", type=int, default=40)
a = ap.parse_args()

addrs, names = [], []
out = subprocess.run([NM, "-n", "-C", "--defined-only", a.elf], capture_output=True, text=True).stdout
for line in out.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        addrs.append(int(p[0], 16))
        names.append(p[2])


def sym(x):
    i = bisect.bisect_right(addrs, x) - 1
    if i < 0:
        return hex(x)
    n = names[i]
    n = n if len(n) < 70 else n[:67] + "..."
    return f"{n}+0x{x - addrs[i]:x}"


base, rows = 0, []
for line in open(a.profile):
    p = line.split()
    if not p:
        continue
    if p[0] == "base":
        base = int(p[1], 16)
    elif p[0] == "wstack":
        rows.append((int(p[1]), [int(v, 16) for v in p[2:]]))
if not rows:
    print("no wstack lines (profile from a build without the r16w profiler?)")
    raise SystemExit(0)

by_svc = collections.defaultdict(list)
for n, w in rows:
    by_svc[sym(w[0] - base).split("+")[0]].append((n, w))
total = sum(n for n, _ in rows)
for svc, lst in sorted(by_svc.items(), key=lambda kv: -sum(n for n, _ in kv[1])):
    s = sum(n for n, _ in lst)
    print(f"\n== {svc}: {s} samples ({100.0 * s / total:.1f} % of the wait samples)")
    # merge by the chain without the svc pc
    agg = collections.Counter()
    for n, w in lst:
        agg[tuple(w[1:])] += n
    for chain, n in agg.most_common(a.top):
        print(f"  {n:6d} {100.0 * n / s:5.1f} %  " + "  <-  ".join(sym(x - base) for x in chain if x))
