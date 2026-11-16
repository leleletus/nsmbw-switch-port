#!/usr/bin/env python3
"""Summarise an NWiiRecomp run log: warnings, errors, unhandled hardware/IOS,
DVD files, OSReport text, stuck PCs (named with the USA symbol map), last stats.

Usage: log_summary.py <run.log> [usa_symbols.txt] > summary.txt
"""
import bisect
import collections
import re
import sys

PROBLEM = re.compile(
    r"unhandled|unknown|unimplemented|not implemented|error|fail|warn|miss|"
    r"invalid|abort|panic|segfault|bad_alloc|exception|assert|spinning|stuck",
    re.I)
HEX = re.compile(r"0x[0-9a-fA-F]+|\b[0-9a-fA-F]{6,}\b|\b\d+\b")
PC = re.compile(r"(?:PC|pc)[:=] ?(?:0x)?([0-9a-fA-F]{8})")


def load_symbols(path):
    addrs, names = [], []
    try:
        for line in open(path, errors="replace"):
            parts = line.split()
            if len(parts) >= 3:
                addrs.append(int(parts[0], 16))
                names.append(parts[2])
    except OSError:
        pass
    order = sorted(range(len(addrs)), key=addrs.__getitem__)
    return [addrs[i] for i in order], [names[i] for i in order]


def name_of(sym, addr):
    addrs, names = sym
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0 or addr - addrs[i] > 0x4000:
        return "?"
    return f"{names[i]}+0x{addr - addrs[i]:x}"


def main():
    log = sys.argv[1]
    sym = load_symbols(sys.argv[2]) if len(sys.argv) > 2 else ([], [])
    problems = collections.Counter()
    examples = {}
    dvd = []
    osreport = collections.Counter()
    stuck = collections.Counter()
    stats = []
    lines = 0
    for line in open(log, errors="replace"):
        lines += 1
        line = line.rstrip("\n")
        if "[OSReport]" in line:
            osreport[line.split("]", 2)[-1].strip()[:160]] += 1
        if "[DVD" in line or "DVDLOG" in line or ("open" in line.lower() and "/" in line and "[VDisc]" in line):
            if len(dvd) < 400:
                dvd.append(line[:200])
        if line.startswith("[STAT]") or "FPS" in line[:40]:
            stats.append(line[:300])
        if "Spinning" in line or "Heartbeat" in line:
            m = PC.search(line)
            if m:
                stuck[int(m.group(1), 16)] += 1
        if "Context restore" in line or "[GXPROF]" in line:
            continue
        if PROBLEM.search(line):
            key = HEX.sub("#", line)[:180]
            problems[key] += 1
            examples.setdefault(key, line[:240])

    out = sys.stdout
    out.write(f"Log: {log}  ({lines} lines)\n\n")
    out.write("== Problemas (warnings/errores/no implementados), agrupados ==\n")
    for key, n in problems.most_common(60):
        out.write(f"{n:9d}  {examples[key]}\n")
    out.write("\n== Donde se queda girando la CPU (PC -> función) ==\n")
    for pc, n in stuck.most_common(15):
        out.write(f"{n:9d}  {pc:08x}  {name_of(sym, pc)}\n")
    out.write("\n== Mensajes del juego (OSReport) ==\n")
    for msg, n in osreport.most_common(40):
        out.write(f"{n:6d}  {msg}\n")
    out.write("\n== Archivos del disco leídos (primeros) ==\n")
    for d in dvd[:120]:
        out.write(f"  {d}\n")
    out.write("\n== Últimas estadísticas ==\n")
    for s in stats[-6:]:
        out.write(f"  {s}\n")


if __name__ == "__main__":
    main()
