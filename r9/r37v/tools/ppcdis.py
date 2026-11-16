#!/usr/bin/env python3
"""ppcdis.py <dir_ini hex> <dir_fin hex>: desensamblado minimo (instrucciones comunes) del main.dol de SMNE01."""
import sys, struct
sys.path.insert(0, "/home/djvemo/nsmbw-port/r9/r35m4/tools")
from tprof_m4 import Dol
import re
syms = {}
for l in open("/home/djvemo/nsmbw-port/symbols/usa_symbols.txt", errors="replace"):
    m = re.match(r"([0-9A-F]{8})\s+\S+\s+(\S+)", l)
    if m: syms[int(m.group(1), 16)] = m.group(2)[:60]
d = Dol("/home/djvemo/nsmbw-port/game/SMNE01/sys/main.dol")
def s16(v): return v - 0x10000 if v & 0x8000 else v
def dis(a, w):
    op = w >> 26; rD = (w >> 21) & 31; rA = (w >> 16) & 31; rB = (w >> 11) & 31; imm = w & 0xffff
    D = {14: "addi", 15: "addis", 32: "lwz", 33: "lwzu", 34: "lbz", 36: "stw", 37: "stwu", 38: "stb", 40: "lhz", 42: "lha", 44: "sth",
         48: "lfs", 50: "lfd", 52: "stfs", 54: "stfd", 46: "lmw", 47: "stmw", 7: "mulli", 8: "subfic"}
    if op in D:
        n = D[op]
        if n in ("addi", "addis", "mulli", "subfic"):
            if n == "addi" and rA == 0: return "li r%d, %d" % (rD, s16(imm))
            if n == "addis" and rA == 0: return "lis r%d, 0x%x" % (rD, imm)
            return "%s r%d, r%d, %d" % (n, rD, rA, s16(imm))
        f = "f" if n[1] == "f" or n.startswith("stf") or n.startswith("lf") else "r"
        return "%s %s%d, %d(r%d)" % (n, f, rD, s16(imm), rA)
    if op == 24: return "ori r%d, r%d, 0x%x" % (rA, rD, imm)
    if op == 25: return "oris r%d, r%d, 0x%x" % (rA, rD, imm)
    if op == 28: return "andi. r%d, r%d, 0x%x" % (rA, rD, imm)
    if op == 11: return "cmpwi cr%d, r%d, %d" % (rD >> 2, rA, s16(imm))
    if op == 10: return "cmplwi cr%d, r%d, %d" % (rD >> 2, rA, imm)
    if op == 18:
        li = w & 0x3fffffc
        if li & 0x2000000: li -= 0x4000000
        t = (a + li) & 0xffffffff if not w & 2 else li
        return ("bl" if w & 1 else "b") + " %08X %s" % (t, syms.get(t, ""))
    if op == 16:
        bd = w & 0xfffc
        if bd & 0x8000: bd -= 0x10000
        return "bc%s %d,%d, %08X" % ("l" if w & 1 else "", rD, rA, (a + bd) & 0xffffffff)
    if op == 21:
        return "rlwinm r%d, r%d, %d, %d, %d" % (rA, rD, rB, (w >> 6) & 31, (w >> 1) & 31)
    if op == 19:
        x = (w >> 1) & 1023
        if x == 16: return "bclr %d,%d" % (rD, rA) if rD != 20 else "blr"
        if x == 528: return "bcctr%s" % ("l" if w & 1 else "")
        return "op19/%d" % x
    if op == 31:
        x = (w >> 1) & 1023
        X = {444: "or", 266: "add", 40: "subf", 8: "subfc", 138: "adde", 136: "subfe", 0: "cmpw", 32: "cmplw", 235: "mullw",
             459: "divwu", 491: "divw", 28: "and", 23: "lwzx", 151: "stwx", 75: "mulhw", 11: "mulhwu", 24: "slw", 536: "srw", 824: "srawi", 792: "sraw", 104: "neg"}
        if x == 339: return "mfspr r%d, %d" % (rD, ((w >> 16) & 31) | (((w >> 11) & 31) << 5))
        if x == 467: return "mtspr %d, r%d" % (((w >> 16) & 31) | (((w >> 11) & 31) << 5), rD)
        if x == 371: return "mftb r%d, %d" % (rD, ((w >> 16) & 31) | (((w >> 11) & 31) << 5))
        if x in (0, 32): return "%s cr%d, r%d, r%d" % (X[x], rD >> 2, rA, rB)
        if x in X: return "%s r%d, r%d, r%d" % (X[x], rD if x not in (444, 28, 24, 536, 824, 792) else rA, rA if x not in (444, 28, 24, 536, 824, 792) else rD, rB)
        return "op31/%d" % x
    if op == 59 or op == 63:
        x = (w >> 1) & 31
        nm = {18: "fdiv", 20: "fsub", 21: "fadd", 25: "fmul"}.get(x)
        if nm: return "%s%s f%d, f%d, f%d" % (nm, "s" if op == 59 else "", rD, rA, rB if x != 25 else (w >> 6) & 31)
        x10 = (w >> 1) & 1023
        return {0: "fcmpu", 72: "fmr", 12: "frsp", 15: "fctiwz", 32: "fcmpo"}.get(x10, "fop%d/%d" % (op, x10)) + " f%d,f%d,f%d" % (rD, rA, rB)
    return "op%d" % op
a0, a1 = int(sys.argv[1], 16), int(sys.argv[2], 16)
for a in range(a0, a1, 4):
    w = d.ins(a)
    if a in syms: print("%08X <%s>" % (a, syms[a]))
    print("  %08X  %08X  %s" % (a, w, dis(a, w)))
