#!/usr/bin/env bash
# reapply.sh: puts back the pristine r18m copies of the files apply_x1.py touched and applies it again
set -euo pipefail
X=/home/djvemo/nsmbw-port/r9/r26x1; O=/home/djvemo/nsmbw-port/r9/r18m/export
for f in $(grep -l '// \[n1\]' $X/export/output_*.cpp || true); do cp -f $O/$(basename $f) $f; done
python3 $X/tools/apply_x1.py $X/export "$@" | tee $X/runs/apply_x1.txt
grep -l '// \[n1\]' $X/export/output_*.cpp | xargs -n1 basename | sed 's/\.cpp$//' | tr '\n' ' ' > $X/runs/mods.txt
# output_24 is always rebuilt (r18m's own edit lives there)
grep -qw output_24 $X/runs/mods.txt || echo -n "output_24 " >> $X/runs/mods.txt
echo; cat $X/runs/mods.txt; echo
