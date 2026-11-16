#!/usr/bin/env bash
# time_x1.sh <name> [secs]: per-call time of every native body on PC (NWII_NATIVE_TIME=1), same binary, native vs
# generated code (NWII_NATIVE=0), forced demos desierto4j + agua_pinguino (front S), each run under render/pc/.runlock
set -u
M=/home/djvemo/nsmbw-port/r9/r26x1; R=/home/djvemo/nsmbw-port/r9/render
n=$1; secs=${2:-150}; bin=$M/bin/NSMBW_${n}
D=(NWII_FORCE_DEMO=desierto4j,agua_pinguino NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 NWII_NATIVE_TIME=1 EVERY=0 PROFILE=free)
(cd $R && flock pc/.runlock env "${D[@]}" pc/run.sh "$bin" x1_time_on_$n $secs mp) > /dev/null 2>&1
(cd $R && flock pc/.runlock env "${D[@]}" NWII_NATIVE=0 pc/run.sh "$bin" x1_time_off_$n $secs mp) > /dev/null 2>&1
python3 -I - $R/pc/runs/x1_time_on_$n/run.log $R/pc/runs/x1_time_off_$n/run.log <<'PY' | tee $M/runs/pc_time_$n.txt
import re, sys
def last(p):
    d = {}
    for l in open(p, errors='replace'):
        m = re.match(r'\[N1\] (\S+) time \S+.*?: (\d+) calls, ([\d.]+) ns/call', l)
        if m: d[m.group(1)] = (int(m.group(2)), float(m.group(3)))
    return d
a, b = last(sys.argv[1]), last(sys.argv[2])
print('PC x86, desierto4j + agua_pinguino, per-call time (last report): native | generated code | generated/native')
for k in sorted(set(a) | set(b)):
    na, ta = a.get(k, (0, 0.0)); nb, tb = b.get(k, (0, 0.0))
    print('%-18s native %8.1f ns (%9d)  generated %8.1f ns (%9d)  %s' % (k, ta, na, tb, nb, 'x%.2f' % (tb / ta) if ta and tb else ''))
PY
grep -h "fps avg" $R/pc/runs/x1_time_on_$n/summary.txt $R/pc/runs/x1_time_off_$n/summary.txt
