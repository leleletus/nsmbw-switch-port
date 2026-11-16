#!/usr/bin/env python3
"""perf_report.py <perf.data> [top=25]

Per-thread view of a `perf record -s -p <game>` profile of the PC build (r9/base/pc/prof.sh):
each thread's share of all samples, its role (game CPU thread = mostly generated func_* code,
host/render thread = gx/renderer, GL driver threads = Mesa), a bucket breakdown and its top
functions. Buckets follow r8/threads/verify/prof/bucket.py.
"""
import collections
import re
import subprocess
import sys


def bucket(sym, dso, kind):
    if kind == "k":
        return "kernel"
    if "ax::" in sym or "adma" in sym or "audio_push" in sym or "hw_dsp" in sym or "dsp_" in sym:
        return "AX/DSP HLE"
    if "WGPIPE" in sym or "wgp_" in sym or "GX_DL" in sym or "FifoLock" in sym:
        return "GX FIFO write (guest side)"
    if ("process_pending" in sym or "ppc_" in sym or "EventScheduler" in sym or
            "clock_gettime" in sym or "read_timebase" in sym or "governor" in sym):
        return "interrupt poll / timebase"
    if "idle_wait" in sym:
        return "idle_wait"
    if sym.startswith("func_") or re.search(r"_8[0-9A-F]{7}$", sym):
        return "guest code (generated)"
    if "hash_ram" in sym or "efb_copy" in sym or "TextureCache" in sym:
        return "GX / renderer (host)"  # texture hashing / EFB copies take a CPUContext* too
    if "MMU" in sym or "psq_" in sym or "CPUContext::" in sym or "MemView" in sym:
        return "runtime mem / paired-single helpers"
    if ("gx::" in sym or "GX" in sym or "Fifo" in sym or "RendererGL" in sym or "Texture" in sym or
            "DisplayList" in sym or "hash_ram" in sym or "efb_" in sym or "Draw" in sym):
        return "GX / renderer (host)"
    if "libc" in dso or "libstdc" in dso or "libm" in dso:
        return "libc / libstdc++"
    if "gallium" in dso or "mesa" in dso.lower() or "libGL" in dso or "iris" in dso or "dri" in dso:
        return "GL driver (Mesa)"
    if dso.startswith("lib") or ".so" in dso:
        return "other libs (" + dso + ")"
    if sym.startswith("0x"):
        return "unresolved"
    return "runtime other"


def main():
    data = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    out = subprocess.run(["perf", "report", "-i", data, "--stdio", "--no-children",
                          "-s", "pid,dso,sym", "--percent-limit", "0"],
                         capture_output=True, text=True).stdout
    th_b = collections.defaultdict(collections.Counter)
    th_s = collections.defaultdict(collections.Counter)
    tot = collections.Counter()
    for l in out.splitlines():
        if not l.strip() or l.startswith("#"):
            continue
        m = re.match(r"\s*([\d.]+)%\s+(\d+):(.+?)\s{2,}(\S+)\s+\[(.)\]\s+(.*)", l)
        if not m:
            continue
        p = float(m.group(1))
        tid = m.group(2) + ":" + m.group(3).strip()
        dso, kind, sym = m.group(4), m.group(5), m.group(6).strip()
        tot[tid] += p
        th_b[tid][bucket(sym, dso, kind)] += p
        th_s[tid][sym[:110]] += p
    print(f"# {data}: share of all samples per thread (100 % = all threads together)")
    for tid, t in tot.most_common():
        if t < 1:
            continue
        g = th_b[tid]["guest code (generated)"]
        role = ("game CPU thread" if g > 0.3 * t else
                "GL driver thread" if th_b[tid]["GL driver (Mesa)"] > 0.4 * t else
                "host / render thread" if th_b[tid]["GX / renderer (host)"] > 0.2 * t else "other")
        print(f"\n== {tid}  {t:.1f}% of samples  ({role})")
        for b, p in th_b[tid].most_common():
            if p / t >= 0.005:
                print(f"   {p / t * 100:5.1f}%  {b}")
        print("   top functions (% of this thread):")
        for s, p in th_s[tid].most_common(top):
            print(f"     {p / t * 100:5.1f}%  {s}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
