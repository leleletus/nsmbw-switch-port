#!/usr/bin/env bash
# chain.sh <c1|c2|...> : todo lo que va después de generar <c>/export, en orden y de uno en uno
# (cada compilación/enlace/comparador por ~/nsmbw-port/scripts/lim, JOBS=2; el nombre build_* toma el
# único turno de compilación). Pasos (se salta lo ya hecho):
#   nx objs -> medida estática -> pc objs -> enlace s1 (Switch + PC) -> tabla fastmem -> comparador
#   (2 semillas, --dispatch) -> corridas en PC (checks sanity title cb4 long; cada una con .runlock)
# Log: runs/chain_<c>.log
set -u
C=/home/djvemo/nsmbw-port/r9/r23g6c; L=~/nsmbw-port/scripts/lim; E=$1; N=g6${E}s
cd $C
log() { echo "[$(date +%T)] $*"; }
exec >> runs/chain_$E.log 2>&1
log "chain $E start"
if [[ ! -f $E/nx_obj/.ok ]]; then
  JOBS=2 $L tools/build_g6.sh nx $E/export $E/nx_obj > $E/build_nx.log 2>&1 || { log "nx build FAILED"; exit 1; }
  touch $E/nx_obj/.ok
fi
log "nx objs ok"
python3 tools/static_cmp.py e2=../r20g6/e2/nx_obj $E=$E/nx_obj --top 30 > runs/static_$E.txt 2>&1; head -4 runs/static_$E.txt
if [[ ! -f $E/pc_obj/.ok ]]; then
  JOBS=2 $L tools/build_g6.sh pc $E/export $E/pc_obj > $E/build_pc.log 2>&1 || { log "pc build FAILED"; exit 1; }
  touch $E/pc_obj/.ok
fi
log "pc objs ok"
$L tools/build_g6cs.sh $E > runs/link_$E.log 2>&1 || { log "link FAILED"; exit 1; }
log "linked: $(tail -4 runs/link_$E.log | tr '\n' ' ')"
python3 ../r14g5/tools/check_fm_tab.py bin/nsmbw_$N.elf ../r14g5/sites.tsv > runs/fmtab_$E.txt 2>&1; log "fastmem: $(tail -2 runs/fmtab_$E.txt | tr '\n' ' ')"
# comparador: A = objetos de PC de G6 e2 (r20g6/e2/pc_obj; e2 = m1 comprobado sin --dispatch), B = los de
# este paso. [10-nov] Antes A era m1 (r14g5): con --dispatch eso da ~200-350 "diferencias" que no son
# de este paso (m1 tiene el switch de entrada completo y sigue nativo en un destino de bctr a media
# función; e1/e2 lo podaron y van a resume_pruned = status 4). Ver LEEME.
mkdir -p difftest/objB_$E
for f in $E/pc_obj/output_*.o; do b=$(basename $f .o); ln -sf $C/$f difftest/objB_$E/$b.cpp.o; done
A=${DT_A:-$C/../r20g6/difftest/objB_e2}
$L bash difftest/build.sh $A difftest/objB_$E difftest/w_$E > difftest/build_$E.log 2>&1 || { log "difftest build FAILED"; exit 1; }
log "difftest built: $(grep '_g6 bodies' difftest/build_$E.log)"
# corridas de PC en paralelo con el comparador (el comparador no compila; las corridas toman .runlock)
( tools/verify_g6.sh $N checks sanity title cb4 long > runs/verify_$E.log 2>&1; log "pc runs done" ) &
vp=$!
mkdir -p difftest/runs; (cd difftest && for s in 1 2; do $L ./run_all.sh w_$E runs/$E $s 2 --dispatch ../gen/jt.txt; done) > runs/difftest_$E.txt 2>&1
log "difftest: $(cat runs/difftest_$E.txt | tr '\n' ' ')"
wait $vp
log "chain $E done"
