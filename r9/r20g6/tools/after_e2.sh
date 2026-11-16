#!/usr/bin/env bash
# e2 tail after the object builds: Switch link + order L, PC link, difftest (2 seeds), PC checks + runs.
# Every step that compiles/links takes the single lim slot (build_* names); difftest runs inside lim.
cd /home/djvemo/nsmbw-port/r9/r20g6
L=~/nsmbw-port/scripts/lim
until grep -q '^exit' e2/build_nx2.log; do sleep 20; done
grep -q '^exit 0' e2/build_nx2.log || { echo "nx build failed"; exit 1; }
$L tools/build_link_g6.sh nx e2/nx_obj g6e2 && echo "nx link ok"
python3 ../r14g5/tools/check_fm_tab.py bin/nsmbw_g6e2.elf ../r14g5/sites.tsv | tail -2
$L tools/build_hot_g6.sh g6e2 $(cat tools/hot_profiles.txt) > runs/build_hot_g6e2.log 2>&1 && echo "hot ok: $(grep '\[eq\]' runs/build_hot_g6e2.log)"
$L tools/build_link_g6.sh pc e2/pc_obj g6e2 && echo "pc link ok"
$L bash difftest/build.sh difftest/objA difftest/objB_e2 difftest/w_e2 > difftest/build_e2.log 2>&1 && echo "difftest built"
(cd difftest && for s in 1 2; do $L ./run_all.sh w_e2 runs/e2b $s 2; done)
tools/verify_g6.sh g6e2 checks sanity title cb4 long
echo ALL DONE
