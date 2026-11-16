#!/usr/bin/env bash
# [r45p] runstat.sh <bin_input_test> <tag>: NWII_P_RUNSTAT=1 en mapa y selector (1.5789x, 90 s). -> test/runstat_<tag>.txt
bin=$(realpath "$1"); cd /home/djvemo/nsmbw-port/r9/r41m; tag=$2; O=/home/djvemo/nsmbw-port/r9/r45p/test/runstat_$tag.txt; : > $O
for p in mapa selector; do
  s1=; [[ $p == selector ]] && s1='300-306:1000:100:6'
  name=runstat_${tag}_$p
  NWII_INPUT_SCRIPT_1=$s1 flock pc/.runlock env NWII_NO_SAVE=1 NWII_INPUT_PHASES=3 NWII_P_RUNSTAT=1 EVERY=0 PROFILE=free RES=1.5789 \
    pc/run.sh "$bin" $name 90 mp '0-100000:100:20:4' > /dev/null 2>&1
  echo "== $p" >> $O; grep -a '^\[P45RUN\]' pc/runs/$name/run.log | tail -2 >> $O
done
