#!/usr/bin/env python3
"""hostprof.py: what the game thread's samples in GENERATED code are spent on, per AArch64 instruction class.

  python3 -I tools/hostprof.py --elf <nro.elf> [--export <export dir>] profile_cpu.txt [...]

Maps every sample pc (pc - base = .elf offset) of a generated function (func_*, *_8XXXXXXX, run_game) to
the instruction it hit (objdump of the sampled functions only) and classifies it:
  ctx.<field> ld/st  (the function's ctx register = the base register with the most [xN,#off] accesses at
                      CPUContext offsets; offsets from tools/ctx_offsets.txt of the AArch64 build)
  stack ld/st        ([sp, ...]: spills / saved registers)
  guest ld/st        ([xB, wI, uxtw]: fastmem guest memory)
  bswap              (rev/rev16)
  call / ret / branch / cmp / fcvt (single rounding) / fp / alu / const (mov/movk) / other-mem
Also prints the same classes over the static code of the sampled functions, weighted by samples, and,
with --export, the AArch64-instructions-per-PPC-instruction ratio of the hottest functions.
Samples skid a little on AArch64 (often the instruction after a slow one), so read classes as a guide.
"""
import argparse
import bisect
import collections
import os
import re
import subprocess

DKP = os.environ.get("DEVKITPRO", "/opt/devkitpro")
NM = os.path.join(DKP, "devkitA64", "bin", "aarch64-none-elf-nm")
OD = os.path.join(DKP, "devkitA64", "bin", "aarch64-none-elf-objdump")

# CPUContext offsets (AArch64, r18m cpu_context.h; tools/ctx_offsets.sh recomputes them)
FIELDS = [(0, 128, "gpr"), (128, 384, "fpr"), (384, 640, "ps1"), (640, 672, "cr"), (672, 676, "pc"),
          (676, 680, "lr"), (680, 684, "ctr"), (684, 688, "xer"), (688, 692, "msr"), (692, 696, "fpscr"),
          (696, 704, "srr"), (704, 736, "gqr"), (856, 912, "mmu"), (912, 920, "inst_count"),
          (1244, 1248, "poll_left"), (1248, 1249, "poll_dec_due"), (1016, 1020, "pending_cb")]


def field(off):
    for lo, hi, n in FIELDS:
        if lo <= off < hi:
            return n
    return "other" if off < 1320 else None


GEN = re.compile(r"(^func_|^run_game$|_8[0-9A-Fa-f]{7}(_g6)?$)")
MEM = re.compile(r"\[(\w+)(?:, ?([^\]]+))?\](!?)")


def parse_profile(path):
    base, pairs = None, []
    for line in open(path):
        p = line.split()
        if not p or p[0].startswith("#"):
            continue
        if p[0] == "base":
            base = int(p[1], 16)
        elif len(p) == 3 and p[0].isdigit():
            pairs.append((int(p[0]), int(p[1], 16)))
    return [(c, pc - base) for c, pc in pairs]


def load_syms(elf):
    syms = []
    out = subprocess.run([NM, "-n", "-S", "--defined-only", elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        q = line.split()
        if len(q) == 4 and q[2] in "tTwW":
            syms.append((int(q[0], 16), int(q[1], 16), q[3]))
    return syms


def disasm(elf, lo, hi):
    """{addr: (mnemonic, operands)} for [lo, hi)."""
    out = subprocess.run([OD, "-d", "--no-show-raw-insn", f"--start-address={lo:#x}", f"--stop-address={hi:#x}", elf],
                         capture_output=True, text=True).stdout
    res = {}
    for line in out.splitlines():
        if not line.startswith("  ") or ":\t" not in line:
            continue
        a, rest = line.split(":\t", 1)
        try:
            addr = int(a.strip(), 16)
        except ValueError:
            continue
        parts = rest.split("\t", 1)
        res[addr] = (parts[0].strip(), parts[1].split("//")[0].split("<")[0].strip() if len(parts) > 1 else "")
    return res


def ctx_reg(insts):
    cnt = collections.Counter()
    for mn, ops in insts.values():
        if mn.startswith(("ld", "st")):
            m = MEM.search(ops)
            if m and m.group(1) != "sp" and m.group(1).startswith("x"):
                off = m.group(2)
                if off is None:
                    cnt[m.group(1)] += 0.2
                elif off.startswith("#"):
                    try:
                        v = int(off[1:], 0)
                    except ValueError:
                        continue
                    if field(v) in ("gpr", "fpr", "ps1", "cr", "pc", "lr", "ctr", "inst_count", "poll_left"):
                        cnt[m.group(1)] += 1
    return cnt.most_common(1)[0][0] if cnt else None


def classify(mn, ops, creg):
    if mn.startswith(("ld", "st")) and "[" in ops:
        kind = "ld" if mn.startswith("ld") else "st"
        m = MEM.search(ops)
        b, off = m.group(1), m.group(2)
        if b == "sp":
            return "stack " + kind
        if off and "uxtw" in off:
            return "guest " + kind
        if b == creg:
            v = 0
            if off and off.startswith("#"):
                try:
                    v = int(off[1:], 0)
                except ValueError:
                    v = 0
            f = field(v) or "other"
            if f == "mmu":
                f = "mmu(MemView)"
            return f"ctx.{f} {kind}"
        return "other-mem " + kind
    if mn in ("rev", "rev16", "rev32"):
        return "bswap"
    if mn in ("bl", "blr"):
        return "call"
    if mn == "ret":
        return "ret"
    if mn in ("b", "br") or mn.startswith(("b.", "cb", "tb")):
        return "branch"
    if mn in ("cmp", "cmn", "tst", "ccmp", "ccmn", "fcmp", "fcmpe", "cset", "csel", "csinc", "csinv", "csneg"):
        return "cmp/cset"
    if mn == "fcvt":
        return "fcvt (round)"
    if mn.startswith("f") or mn in ("scvtf", "ucvtf"):
        return "fp"
    if mn in ("mov", "movk", "movz", "movn", "adrp", "adr"):
        return "mov/const"
    return "alu"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True)
    ap.add_argument("--export", help="export dir (C++ of that build) for the PPC instruction counts")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("profiles", nargs="+")
    a = ap.parse_args()
    syms = load_syms(a.elf)
    starts = [s[0] for s in syms]
    fsamp = collections.Counter()
    isamp = collections.Counter()
    for p in a.profiles:
        for c, off in parse_profile(p):
            i = bisect.bisect_right(starts, off) - 1
            if i < 0 or off >= syms[i][0] + max(syms[i][1], 4):
                continue
            if GEN.search(syms[i][2]):
                fsamp[i] += c
                isamp[off] += c
    total = sum(fsamp.values())
    print(f"generated-code samples: {total} in {len(fsamp)} functions")
    cls = collections.Counter()
    cls_static = collections.Counter()
    fn_info = {}
    # one objdump over the hot-ordered head of .text (L order puts the hot functions first), one call per
    # function after it (only functions with >= 3 samples there; the rest is counted as "not analysed")
    hot = sorted(i for i in fsamp)
    head_end = 0
    acc = 0
    for i in hot:
        acc += fsamp[i]
        head_end = syms[i][0] + syms[i][1]
        if acc >= 0.97 * total:
            break
    big = disasm(a.elf, syms[hot[0]][0], head_end)
    skipped = 0
    for i, n in fsamp.most_common():
        a0, sz, name = syms[i]
        if a0 + sz <= head_end:
            ins = {k: v for k, v in ((ad, big.get(ad)) for ad in range(a0, a0 + sz, 4)) if v}
        elif n >= 3:
            ins = disasm(a.elf, a0, a0 + sz)
        else:
            skipped += n
            continue
        creg = ctx_reg(ins)
        st = collections.Counter()
        for addr, (mn, ops) in ins.items():
            st[classify(mn, ops, creg)] += 1
        for k, v in st.items():
            cls_static[k] += n * v / max(1, len(ins))
        for addr in ins:
            if isamp.get(addr):
                cls[classify(*ins[addr], creg)] += isamp[addr]
        fn_info[name] = (n, len(ins), st)
    print(f"(functions after the hot head with < 3 samples not analysed: {skipped} samples)")
    print("\nsampled instruction classes (share of generated-code samples):")
    for k, v in cls.most_common():
        print(f"  {100.0 * v / total:6.2f} %  {v:7d}  {k}")
    print("\nstatic instruction mix of the sampled functions, weighted by their samples:")
    for k, v in cls_static.most_common():
        print(f"  {100.0 * v / total:6.2f} %  {k}")
    ppc = {}
    if a.export:
        cur = None
        fre = re.compile(r"^void (\w+)\(CPUContext& ctx\) \{")
        for fn in sorted(os.listdir(a.export)):
            if not (fn.startswith("output_") and fn.endswith(".cpp")):
                continue
            for line in open(os.path.join(a.export, fn), errors="replace"):
                m = fre.match(line)
                if m:
                    cur = m.group(1)
                    ppc[cur] = 0
                elif cur and line.startswith("    // 0x"):
                    ppc[cur] += 1
        num = den = 0
        for name, (n, nins, st) in fn_info.items():
            if ppc.get(name):
                num += n * nins / ppc[name]
                den += n
        if den:
            print(f"\nAArch64 instructions per PPC instruction (static, weighted by samples): {num / den:.2f}")
    print(f"\ntop {a.top} functions: samples, AArch64 ins, PPC ins, ratio, ctx ld/st, stack ld/st, calls")
    for name, (n, nins, st) in sorted(fn_info.items(), key=lambda kv: -kv[1][0])[:a.top]:
        cl = sum(v for k, v in st.items() if k.startswith("ctx.") and k.endswith("ld"))
        cs = sum(v for k, v in st.items() if k.startswith("ctx.") and k.endswith("st"))
        p = ppc.get(name, 0)
        print(f"  {n:6d} {100.0 * n / total:5.2f}%  {name:48s} {nins:6d} {p:5d} {nins / p if p else 0:5.2f} "
              f"ctx {cl}/{cs} stk {st['stack ld']}/{st['stack st']} call {st['call']}")


if __name__ == "__main__":
    main()
