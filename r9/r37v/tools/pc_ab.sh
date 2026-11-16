#!/usr/bin/env bash
# pc_ab.sh <bin_input_test> <tag> <demo> <secs> [VAR=val ...]: una corrida de PC con la linea de tiempo; resumen de 45 s al final
set -u
W=/home/djvemo/nsmbw-port/r9/r37v; bin=$1; tag=$2; demo=$3; secs=$4; shift 4
( cd $W && flock pc/.runlock env NWII_FORCE_DEMO=$demo NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 NWII_V_TL=$W/pc/runs/vtl_$tag.bin "$@" \
    EVERY=${EVERY:-0} PROFILE=${PROFILE:-free} pc/run.sh "$bin" "$tag" "$secs" mp "0-$secs:100:20:4" > /dev/null 2>&1 )
echo "== $tag ($*)"
python3 $W/tools/vtl.py $W/pc/runs/vtl_$tag.bin resumen --desde $((secs - 45 > 10 ? secs - 45 : 10))
