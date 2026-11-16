#!/usr/bin/env bash
# run_prof.sh: scripted PC runs of the site-profile binary (r9/r11c/bin/NSMBW_siteprof_input_test),
# one at a time, private NAND templates (never ~/nsmbw-port/nand). Snapshots in pc/runs/<name>/siteprof.
cd /home/djvemo/nsmbw-port/r9/r11c/pc || exit 1
B=/home/djvemo/nsmbw-port/r9/r11c/bin/NSMBW_siteprof_input_test
S11='0-10000000:100:120:4;60-10000000:4:120:12'
CB4='0-610:100:120:4;700-900:1:50:4;920-1000:4:40:4;1040-1100:100:60:4;1500-1600:4:60:4;1650-1660:100:10:4;1950-1960:10:10:4;2500-2510:10:10:4;3100-100000:100:150:4;3800-100000:4:100:60;p2:on@2100;p2:2700-2900:100:100:4;p2:3800-100000:8:150:80;p3:on@2200;p3:2730-2900:100:100:4;p3:3800-100000:100:90:6;p4:on@2300;p4:2760-2900:100:100:4;p4:3800-100000:10000:120:10;p4:3800-100000:4:300:100'
for sc in ${SCEN:-idle lvl11 walk cb4}; do
  echo "== $sc $(date +%T)"
  unset NWII_INPUT_PHASES NWII_INPUT_SCRIPT_1 NWII_INPUT_SCRIPT_2
  case $sc in
    idle)  EVERY=900 ./run.sh $B idle 240 fresh ;;
    lvl11) EVERY=900 ./run.sh $B lvl11 300 mp "$S11" ;;
    walk)  NWII_INPUT_PHASES=3 NWII_INPUT_SCRIPT_1='60-100000:4:300:30;210-100000:8:300:30' \
             EVERY=900 ./run.sh $B walk 150 mp '0-100000:100:20:4' ;;
    cb4)   TOML=mp.toml EVERY=600 ./run.sh $B cb4 200 mp "$CB4" ;;
  esac
  grep -o 'scene=[0-9]* ([A-Z_]*)' runs/$sc/run.log | awk '{print $2}' | uniq | tr '\n' ' '; echo
done
echo "== done $(date +%T)"
