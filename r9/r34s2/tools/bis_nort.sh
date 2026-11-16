#!/usr/bin/env bash
# como bis.sh pero sin NWII_REALTIME (reloj virtual)
cd /home/djvemo/nsmbw-port/r9/r34s2; b=$1; n=$2; shift 2
out=runs/$n
sed 's/NWII_FPS=1/NWII_FPS=1/' tools/audio_run.sh > /dev/null
flock ../render/pc/.runlock ./tools/audio_run.sh $b $n 62 "$@" > /dev/null 2>&1
echo "$n: efb=$(grep -a '\[TCSTAT\]' runs/$n/run.log | grep -o 'efb=[0-9]*' | tr '\n' ' ') copias/fot=$(grep -a '\[TCSTAT\]' runs/$n/run.log | grep -o 'efb copies=[0-9.]*' | cut -d= -f2 | tr '\n' ' ') $(grep -a '\[S2\] ACTIVO' runs/$n/run.log | tail -1 | cut -c1-15)"
