#!/usr/bin/env python3
"""split_lines.py <export_in> <export_out>: copy a generated-code export putting ONE STATEMENT PER LINE.

Only whitespace changes (a newline after every ';', '{' or '}' that is outside parentheses, string/char
literals and comments), so the compiler sees exactly the same tokens: the object code must come out identical
(checked by tools/check_same_text.sh against the e2 objects). Why: gcov counts and DWARF line tables are per
source LINE; e2 puts a whole call sequence (flush; call; check; reload) or a poll with its slow path on one
line. One statement per line = exact execution counts per statement and a per-statement map to AArch64 code.
Lines that start with '#' or '//' and every non output_*.cpp file are copied unchanged.
"""
import os
import shutil
import sys
from concurrent.futures import ProcessPoolExecutor


def split_line(line):
    s = line.rstrip("\n")
    st = s.lstrip()
    if not st or st[0] == "#" or st.startswith("//"):
        return [s]
    indent = s[:len(s) - len(st)]
    out, cur, depth, i, n = [], [], 0, 0, len(s)
    while i < n:
        c = s[i]
        if c == "/" and i + 1 < n and s[i + 1] == "/":
            cur.append(s[i:])
            i = n
            break
        if c == "/" and i + 1 < n and s[i + 1] == "*":
            j = s.find("*/", i + 2)
            j = n if j < 0 else j + 2
            cur.append(s[i:j])
            i = j
            continue
        if c in "\"'":
            j = i + 1
            while j < n and s[j] != c:
                j += 2 if s[j] == "\\" else 1
            cur.append(s[i:j + 1])
            i = j + 1
            continue
        cur.append(c)
        i += 1
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif depth == 0 and c in ";{}":
            out.append("".join(cur))
            cur = []
    tail = "".join(cur)
    if tail.strip():
        # a trailing comment stays on the line of the last statement
        if tail.lstrip().startswith("//") and out:
            out[-1] += tail
        else:
            out.append(tail)
    res = []
    for k, piece in enumerate(out):
        p = piece.strip()
        if p:
            res.append((indent + p) if k else piece.rstrip())
    return res or [s]


def do_file(args):
    src, dst = args
    with open(src, errors="surrogateescape") as f, open(dst, "w", errors="surrogateescape") as g:
        for line in f:
            if "\\\n" in line:
                raise SystemExit(f"{src}: line continuation, not handled")
            for p in split_line(line):
                g.write(p + "\n")
    return dst


def main():
    a, b = sys.argv[1], sys.argv[2]
    os.makedirs(b, exist_ok=True)
    jobs = []
    for fn in sorted(os.listdir(a)):
        p = os.path.join(a, fn)
        if os.path.isdir(p):
            continue
        if fn.startswith("output_") and fn.endswith(".cpp"):
            jobs.append((p, os.path.join(b, fn)))
        else:
            shutil.copy2(p, os.path.join(b, fn))
    with ProcessPoolExecutor(max_workers=int(os.environ.get("JOBS", "2"))) as ex:
        for d in ex.map(do_file, jobs):
            pass
    print(len(jobs), "files split")


if __name__ == "__main__":
    main()
