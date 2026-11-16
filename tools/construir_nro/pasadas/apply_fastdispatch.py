#!/usr/bin/env python3
"""Prototype of two codegen changes, applied to an already generated export/.

1. O(1) dispatch: a flat table (guest address -> recompiled function) built from
   run_game's switch cases; run_game consults it before the 70k-case switch.
2. Native indirect branches: bctrl/blrl call the target through the table and
   continue in place when it returns (like a direct bl), and bctr jumps locally
   when the target is inside the same function, instead of unwinding the whole
   host stack back to run_game and re-entering every frame through the switch.

Usage: apply_fastdispatch.py <export_dir>     (idempotent: refuses to run twice)
"""
import glob
import os
import re
import sys

MARK = "// [fastdispatch]"


def main():
    d = sys.argv[1]
    main_cpp = os.path.join(d, "main_output.cpp")
    src = open(main_cpp).read()
    if MARK in src:
        sys.exit("already applied")

    # 1. table entries from run_game's switch
    start = src.index('extern "C" void run_game')
    sw = src.index("switch (target) {", start)
    end = src.index("default:", sw)
    cases = re.findall(r"case 0x([0-9A-F]+): (\w+)\(ctx\); break;", src[sw:end])
    entries = [(int(a, 16), f) for a, f in cases if int(a, 16) >= 0x80000000]
    # Every instruction address inside a recompiled function is a valid entry
    # (the function's own switch resumes there): interrupt/thread-switch
    # resumes land mid-function, and without these they missed the table and
    # fell to a scan of g_func_bounds.
    bsrc = src[src.index("g_func_bounds[] = {"):]
    bsrc = bsrc[:bsrc.index("};")]
    bounds = [(int(a, 16), int(b, 16), f) for a, b, f in
              re.findall(r"\{0x([0-9A-F]+), 0x([0-9A-F]+), (\w+)\}", bsrc)
              if int(a, 16) >= 0x80000000]
    lo = min([a for a, _ in entries] + [a for a, _, _ in bounds])
    hi = max([a for a, _ in entries] + [b - 4 for _, b, _ in bounds])
    print(f"table: {len(entries)} entries + {len(bounds)} function ranges, 0x{lo:08X}-0x{hi:08X}, "
          f"{((hi - lo) // 4 + 1) * 8 / 2**20:.1f} MiB")

    with open(os.path.join(d, "fastdispatch.cpp"), "w") as f:
        f.write(f'{MARK}\n#include "functions.h"\n#include <cstdlib>\n\n')
        f.write("namespace {\nstruct Entry { uint32_t addr; nwii_fn fn; };\n")
        f.write("const Entry kEntries[] = {\n")
        for a, fn in entries:
            f.write(f"  {{0x{a:08X}u, {fn}}},\n")
        f.write("};\nstruct Bound { uint32_t start, end; nwii_fn fn; };\n")
        f.write("const Bound kBounds[] = {\n")
        for a, b, fn in bounds:
            f.write(f"  {{0x{a:08X}u, 0x{b:08X}u, {fn}}},\n")
        f.write("};\n}  // namespace\n\n")
        f.write(f"const uint32_t g_disp_base = 0x{lo:08X}u;\n")
        f.write(f"const uint32_t g_disp_span = 0x{hi - lo + 4:X}u;\n")
        f.write("nwii_fn *g_disp_table = [] {\n"
                "  if (std::getenv(\"NWII_SLOW_DISPATCH\")) return (nwii_fn *)nullptr;\n"
                "  auto *t = new nwii_fn[g_disp_span / 4]();\n"
                "  for (const Bound &b : kBounds)\n"
                "    for (uint32_t a = b.start; a < b.end; a += 4)\n"
                "      if (!t[(a - g_disp_base) >> 2]) t[(a - g_disp_base) >> 2] = b.fn;  // first range wins, as the old linear scan\n"
                "  for (const Entry &e : kEntries) t[(e.addr - g_disp_base) >> 2] = e.fn;\n"
                "  return t;\n}();\n")

    hdr = os.path.join(d, "functions.h")
    h = open(hdr).read()
    h += f"""
{MARK}
typedef void (*nwii_fn)(nwii::runtime::CPUContext&);
extern const uint32_t g_disp_base, g_disp_span;
extern nwii_fn *g_disp_table;
// Calls the recompiled function holding ctx.pc. Unknown targets are left for
// run_game (ctx.pc unchanged), so the caller's "pc != return address" check
// unwinds exactly as before.
static inline void nwii_dispatch(nwii::runtime::CPUContext& ctx) {{
    uint32_t off = ctx.pc - g_disp_base;
    if (g_disp_table && !(off & 3) && off < g_disp_span)
        if (nwii_fn f = g_disp_table[off >> 2])
            if (nwii::runtime::native_stack_ok()) f(ctx);
}}
"""
    open(hdr, "w").write(h)

    src = src.replace("        switch (target) {", f"""        {MARK}
        {{
            uint32_t off = target - g_disp_base;
            if (g_disp_table && !(off & 3) && off < g_disp_span)
                if (nwii_fn f = g_disp_table[off >> 2]) {{ f(ctx); continue; }}
        }}
        switch (target) {{""", 1)
    open(main_cpp, "w").write(src)

    # 2. indirect branches in every generated unit
    re_func = re.compile(r"^void (\w+)\(CPUContext& ctx\) \{\n", re.M)
    re_bctrl = re.compile(r"ctx\.lr = 0x([0-9A-F]+); // save return address\n(\s*)if \(ctx\.ctr\) \{ ctx\.pc = ctx\.ctr; return; \}")
    re_blrl = re.compile(r"ctx\.lr = 0x([0-9A-F]+); // save return address\n(\s*)ctx\.pc = temp_lr; return;")
    stats = dict(bctrl=0, blrl=0, bctr_local=0, bctr_tail=0, funcs=0)
    for path in sorted(glob.glob(os.path.join(d, "output_*.cpp"))):
        s = open(path).read()
        heads = list(re_func.finditer(s))
        out = [s[:heads[0].start()] if heads else s]
        for i, m in enumerate(heads):
            body = s[m.start():heads[i + 1].start() if i + 1 < len(heads) else len(s)]
            labels = [int(x, 16) for x in re.findall(r"^loc_([0-9A-F]{8}): ;", body, re.M)]
            n = body.count("ctx.pc = ctx.ctr; return;")
            body, k = re_bctrl.subn(lambda mm: f"ctx.lr = 0x{mm.group(1)}; // save return address\n{mm.group(2)}"
                                    f"if (ctx.ctr) {{ ctx.pc = ctx.ctr; nwii_dispatch(ctx); if (ctx.pc != 0x{mm.group(1)}) return; }}", body)
            stats["bctrl"] += k
            body, k = re_blrl.subn(lambda mm: f"ctx.lr = 0x{mm.group(1)}; // save return address\n{mm.group(2)}"
                                   f"ctx.pc = temp_lr; nwii_dispatch(ctx); if (ctx.pc != 0x{mm.group(1)}) return;", body)
            stats["blrl"] += k
            entry_check = re.search(r"^    if \(ctx\.pc != 0x[0-9A-F]+\) \{\n        switch \(ctx\.pc\) \{", body, re.M)
            if n and labels and entry_check:
                lo, hi = min(labels), max(labels)
                body = body[:entry_check.start()] + "nwii_entry: ;\n" + body[entry_check.start():]
                body = body.replace("ctx.pc = ctx.ctr; return;",
                                    f"ctx.pc = ctx.ctr; if (ctx.pc >= 0x{lo:08X}u && ctx.pc <= 0x{hi:08X}u) goto nwii_entry; "
                                    f"nwii_dispatch(ctx); return;")
                stats["bctr_local"] += n
            elif n:
                body = body.replace("ctx.pc = ctx.ctr; return;", "ctx.pc = ctx.ctr; nwii_dispatch(ctx); return;")
                stats["bctr_tail"] += n
            stats["funcs"] += 1
            out.append(body)
        open(path, "w").write("".join(out))
    print(stats)

    cm = os.path.join(d, "CMakeLists.txt")
    c = open(cm).read()
    if "fastdispatch.cpp" not in c:
        c = c.replace("main_output.cpp)", "main_output.cpp fastdispatch.cpp)", 1)
        open(cm, "w").write(c)


if __name__ == "__main__":
    main()
