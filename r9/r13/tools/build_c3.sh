#!/usr/bin/env bash
# c3 = c2 + apply_r9_psq.py + apply_r9_axfx_fast.py: full PC build, then full Switch build (each queued by lim)
C=$HOME/nsmbw-port/r9/cpucode
cd $C
FULL=1 JOBS=3 ~/nsmbw-port/scripts/lim $C/build_pc.sh $C/bin/NSMBW_c3 > $C/build_pc_c3.log 2>&1; echo "pc rc=$?" >> $C/build_pc_c3.log
FULL=1 JOBS=3 ~/nsmbw-port/scripts/lim $C/build_nx.sh $C/bin/nsmbw_c3.nro > $C/build_nx_c3.log 2>&1; echo "nx rc=$?" >> $C/build_nx_c3.log
