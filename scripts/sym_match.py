#!/usr/bin/env python3
"""Build a USA (SMNE01) symbol map from NSMBW-Decomp (PALv1) objects.

1. Every function in the compiled decomp objects becomes a byte pattern with
   relocated fields masked, searched in the USA DOL and RELs.
2. Ambiguous matches are settled by the layout of already-matched neighbours in
   the same object.
3. Call targets of matched functions take the callee name from the relocation.
4. syms.txt (PAL DOL) and alias_db.txt (PAL REL offsets) are translated with
   offsets measured from the matches, only where both neighbouring anchors
   agree.

Usage (run with the project venv, from ~/nsmbw-port):
  tools/venv/bin/python -I scripts/sym_match.py
Writes symbols/usa_symbols.csv, symbols/usa_symbols.txt, symbols/report.txt
"""
import bisect
import collections
import json
import os
import re
import struct
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

ROOT = os.path.expanduser("~/nsmbw-port")
DECOMP = os.path.join(ROOT, "symbols/decomp_build")
OBJDIR = os.path.join(DECOMP, "bin/compiled")
OUT = os.path.join(ROOT, "symbols")
BIN = os.path.join(ROOT, "bin_usa")

REL_IDS = {1: "d_profileNP", 2: "d_basesNP", 3: "d_enemiesNP", 4: "d_en_bossNP"}
UNIT_TO_MODULE = {"wiimj2d": 0, "d_profileNP": 1, "d_basesNP": 2,
                  "d_enemiesNP": 3, "d_en_bossNP": 4}

# PPC relocation types and the instruction bits they patch.
R_PPC_ADDR32 = 1
R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA = 4, 5, 6
R_PPC_REL24, R_PPC_REL14 = 10, 11
R_PPC_EMB_SDA21 = 109


def read_rel_bases():
    bases = {}
    text = open(os.path.join(ROOT, "recomp/rels_from_dolphin.toml")).read()
    for path, base in re.findall(r'path = "([^"]+)"\nmodule_base = 0x([0-9A-Fa-f]+)', text):
        bases[os.path.basename(path)[:-4]] = int(base, 16)
    return bases


class Image:
    """One searchable code image (USA DOL text or one REL .text)."""

    def __init__(self, name, base, data):
        self.name, self.base, self.data = name, base, data
        self.words = struct.unpack(f">{len(data) // 4}I", data[: len(data) // 4 * 4])
        self.index = collections.defaultdict(list)
        for i, w in enumerate(self.words):
            self.index[w].append(i)

    def contains(self, addr):
        return self.base <= addr < self.base + len(self.data)

    def word_at(self, addr):
        return self.words[(addr - self.base) // 4]


def load_images():
    images = {}
    dol = open(os.path.join(BIN, "wiimj2d.dol"), "rb").read()
    offs = struct.unpack(">18I", dol[0:0x48])
    addrs = struct.unpack(">18I", dol[0x48:0x90])
    sizes = struct.unpack(">18I", dol[0x90:0xD8])
    # Text sections are contiguous enough to treat as one image per section.
    images[0] = []
    for i in range(7):
        if sizes[i]:
            images[0].append(Image(f"dol.text{i}", addrs[i], dol[offs[i]:offs[i] + sizes[i]]))
    bases = read_rel_bases()
    for mid, name in REL_IDS.items():
        d = open(os.path.join(BIN, name + ".rel"), "rb").read()
        nsec, secoff = struct.unpack(">2I", d[12:20])
        off, size = struct.unpack(">2I", d[secoff + 8:secoff + 16])  # section 1 = .text
        off &= ~1
        images[mid] = [Image(f"{name}.text", bases[name] + off, d[off:off + size])]
        images[mid][0].text_off = off
    return images


def object_functions(path):
    """Yield (name, value, size, words, masks, callrelocs) for .text functions."""
    with open(path, "rb") as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name(".symtab")
        text_idx = [i for i, s in enumerate(elf.iter_sections()) if s.name.startswith(".text") or s.name == ".init"]
        for ti in text_idx:
            sec = elf.get_section(ti)
            data = sec.data()
            mask = bytearray(b"\xff" * len(data))
            calls = {}
            for rs in elf.iter_sections():
                if not isinstance(rs, RelocationSection) or rs["sh_info"] != ti:
                    continue
                for r in rs.iter_relocations():
                    off, typ = r["r_offset"], r["r_info_type"]
                    sym = symtab.get_symbol(r["r_info_sym"])
                    if typ in (R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA):
                        mask[off:off + 2] = b"\0\0"
                    elif typ == R_PPC_REL24:
                        mask[off:off + 4] = bytes([0xFC, 0, 0, 0x03])
                        if sym.name:
                            calls[off] = sym.name
                    elif typ == R_PPC_REL14:
                        mask[off + 2:off + 4] = bytes([0, 0x03])
                    elif typ == R_PPC_EMB_SDA21:
                        mask[off + 1:off + 4] = bytes([0xE0, 0, 0])
                    elif typ == R_PPC_ADDR32:
                        mask[off:off + 4] = b"\0\0\0\0"
                    else:
                        mask[off:off + 4] = b"\0\0\0\0"
            for s in symtab.iter_symbols():
                if s["st_shndx"] != ti or s["st_info"]["type"] != "STT_FUNC" or not s["st_size"]:
                    continue
                v, n = s["st_value"], s["st_size"]
                n4 = n // 4
                words = struct.unpack(f">{n4}I", data[v:v + n4 * 4])
                masks = struct.unpack(f">{n4}I", bytes(mask[v:v + n4 * 4]))
                fcalls = {o - v: name for o, name in calls.items() if v <= o < v + n}
                yield s.name, s["st_value"], n, words, masks, fcalls, s["st_info"]["bind"]


def find_candidates(images, words, masks):
    # Anchor on the rarest fully-unmasked word.
    best = None
    for i, (w, m) in enumerate(zip(words, masks)):
        if m != 0xFFFFFFFF:
            continue
        cnt = sum(len(img.index.get(w, ())) for img in images)
        if best is None or cnt < best[0]:
            best = (cnt, i, w)
            if cnt <= 1:
                break
    if best is None:
        return []
    _, ai, aw = best
    out = []
    for img in images:
        for pos in img.index.get(aw, ()):
            start = pos - ai
            if start < 0 or start + len(words) > len(img.words):
                continue
            if all((img.words[start + k] & m) == (w & m) for k, (w, m) in enumerate(zip(words, masks))):
                out.append((img, img.base + start * 4))
    return out


def decode_bl(word, site):
    if (word >> 26) != 18:
        return None
    li = word & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return (li if (word & 2) else site + li) & 0xFFFFFFFF


def main():
    images = load_images()
    named = {}           # usa addr -> (name, size, source)
    by_name = collections.defaultdict(set)
    report = collections.Counter()
    anchors = collections.defaultdict(list)  # module -> [(pal_addr, usa_addr)]

    slices = {}
    for unit, mid in UNIT_TO_MODULE.items():
        j = json.load(open(os.path.join(DECOMP, "slices", ("wiimj2d.json" if mid == 0 else unit + ".json"))))
        slices[unit] = j

    def pal_text_base(unit):
        meta = slices[unit]["meta"]
        if UNIT_TO_MODULE[unit] == 0:
            return int(meta["sections"][".text"]["addr"], 16)
        return 0  # REL: offsets within .text

    call_names = []  # (usa_site_addr, callee_name)
    for unit in UNIT_TO_MODULE:
        mid = UNIT_TO_MODULE[unit]
        for sl in slices[unit]["slices"]:
            src = sl.get("source")
            if not src or ".text" not in sl.get("memoryRanges", {}):
                continue
            obj = os.path.join(OBJDIR, unit, os.path.splitext(src)[0] + ".o")
            if not os.path.exists(obj):
                report["missing_obj"] += 1
                continue
            pal_start = pal_text_base(unit) + int(sl["memoryRanges"][".text"].split("-")[0], 16)
            funcs = list(object_functions(obj))
            results = []
            for name, value, size, words, masks, fcalls, bind in funcs:
                cands = find_candidates(images[mid], words, masks)
                results.append((name, value, size, words, fcalls, cands, bind))
            # Unique matches anchor the object layout.
            uniq = [(value, c[0][1]) for name, value, size, words, fcalls, c, bind in results if len(c) == 1]
            # The linker keeps only one copy of each weak function, so the PAL
            # address is only predictable for functions before the first weak.
            first_weak = min((v for _, v, _, _, _, _, b in results if b == "STB_WEAK"), default=1 << 32)
            for name, value, size, words, fcalls, cands, bind in results:
                usa = None
                if len(cands) == 1:
                    usa, how = cands[0][1], "match"
                elif len(cands) > 1 and uniq and bind != "STB_WEAK":
                    # Weak copies may live in another object, so only strong
                    # functions are placed, and only inside this object's span.
                    prev = [u for u in uniq if u[0] <= value]
                    ref = max(prev) if prev else min(uniq)
                    want = ref[1] + (value - ref[0])
                    lo = min(u for _, u in uniq)
                    hi = max(u for _, u in uniq)
                    if lo - 0x100 <= want <= hi + 0x2000 and any(a == want for _, a in cands):
                        usa, how = want, "layout"
                if usa is None:
                    report["ambiguous" if cands else "nomatch"] += 1
                    continue
                report[f"{how}_{unit}"] += 1
                named[usa] = (name, size, f"{how}:{src}")
                by_name[name].add(usa)
                if value < first_weak and how == "match":
                    anchors[mid].append((pal_start + value, usa))
                for off, callee in fcalls.items():
                    call_names.append((mid, usa + off, callee))

    # Callee names from relocations of matched functions.
    callee_votes = collections.defaultdict(collections.Counter)
    for mid, site, callee in call_names:
        for img in images[0] + (images[mid] if mid else []):
            if img.contains(site):
                tgt = decode_bl(img.word_at(site), site)
                if tgt is not None:
                    callee_votes[tgt][callee] += 1
                break
    for tgt, votes in callee_votes.items():
        if len(votes) != 1:
            report["callee_conflict"] += 1
            continue
        name = next(iter(votes))
        if tgt in named:
            continue
        if any(not (img.contains(tgt)) for img in []):
            pass
        named[tgt] = (name, 0, "callee")
        by_name[name].add(tgt)
        report["callee"] += 1

    # Offset tables from anchors, per module, sorted by PAL address.
    def translator(mid):
        pts = sorted(set(anchors[mid]))
        keys = [p for p, _ in pts]

        def tr(pal):
            i = bisect.bisect_right(keys, pal)
            if i == 0 or i == len(keys):
                return None
            d0 = pts[i - 1][1] - pts[i - 1][0]
            d1 = pts[i][1] - pts[i][0]
            return pal + d0 if d0 == d1 else None
        return tr, pts

    tr_dol, dol_pts = translator(0)
    # SDK region shift measured earlier on 38 shared signatures; used where no
    # decomp anchors bracket the address.
    bl_targets = set()
    for img in images[0]:
        for i, w in enumerate(img.words):
            if (w >> 26) == 18 and (w & 1):
                bl_targets.add(decode_bl(w, img.base + i * 4))
    dol_keys = [p for p, _ in dol_pts]

    def tr_dol_ext(pal):
        u = tr_dol(pal)
        if u is not None:
            return u
        # No agreeing bracket: try the deltas of the neighbouring anchors and
        # accept a single candidate that is a real call target in the USA DOL.
        i = bisect.bisect_right(dol_keys, pal)
        deltas = {pts[1] - pts[0] for pts in dol_pts[max(0, i - 2):i + 2]}
        if 0x801A0000 <= pal < 0x80240000:
            deltas.add(-0x140)
        cands = {pal + d for d in deltas if (pal + d) in bl_targets}
        return cands.pop() if len(cands) == 1 else None

    for line in open(os.path.join(DECOMP, "syms.txt")):
        m = re.match(r"\s*([^=\s]+)\s*=\s*0x([0-9A-Fa-f]+)", line)
        if not m:
            continue
        name, pal = m.group(1), int(m.group(2), 16)
        usa = tr_dol_ext(pal)
        if usa is None:
            report["syms_untranslated"] += 1
            continue
        if usa in named:
            report["syms_already"] += 1
            continue
        named[usa] = (name, 0, "syms.txt")
        by_name[name].add(usa)
        report["syms_translated"] += 1

    rel_tr = {}
    for mid in REL_IDS:
        tr, pts = translator(mid)
        rel_tr[mid] = tr
        deltas = collections.Counter(u - p for p, u in pts)
        report[f"rel{mid}_deltas"] = len(deltas)
    for line in open(os.path.join(DECOMP, "alias_db.txt")):
        m = re.match(r"\s*R_(\d+)_(\d+)_([0-9a-fA-F]+)\s*=\s*(\S+)", line)
        if not m:
            continue
        mid, sec, off, name = int(m.group(1)), int(m.group(2)), int(m.group(3), 16), m.group(4)
        if sec != 1 or mid not in REL_IDS:
            continue
        usa = rel_tr[mid](off)
        if usa is None:
            report["alias_untranslated"] += 1
            continue
        if usa in named:
            report["alias_already"] += 1
            continue
        named[usa] = (name, 0, "alias_db")
        by_name[name].add(usa)
        report["alias_translated"] += 1

    # Names claimed by more than one address are dropped unless weak/local dups
    # come from distinct objects (keep all; the recompiler uniquifies), but the
    # same address with two names keeps the first (matches beat translations).
    dup_names = {n: a for n, a in by_name.items() if len(a) > 1}
    report["names_at_multiple_addrs"] = len(dup_names)

    with open(os.path.join(OUT, "usa_symbols.csv"), "w") as f:
        f.write("Name,Location,Size\n")
        for a in sorted(named):
            n, s, _ = named[a]
            f.write(f"\"{n}\",{a:08X},{s}\n")
    with open(os.path.join(OUT, "usa_symbols.txt"), "w") as f:
        for a in sorted(named):
            n, s, src = named[a]
            f.write(f"{a:08X} {s:6x} {n}  [{src}]\n")
    with open(os.path.join(OUT, "report.txt"), "w") as f:
        for k in sorted(report):
            f.write(f"{k}: {report[k]}\n")
        f.write(f"total_symbols: {len(named)}\n")
        f.write("dol_anchor_deltas: " + ", ".join(
            f"{d:#x}(x{c})" for d, c in collections.Counter(u - p for p, u in dol_pts).most_common(12)) + "\n")
    print(open(os.path.join(OUT, "report.txt")).read())


if __name__ == "__main__":
    main()
