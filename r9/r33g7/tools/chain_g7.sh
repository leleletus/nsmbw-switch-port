#!/usr/bin/env bash
# chain_g7.sh <nombre> <variante nx> <variante pc> [pasos de verify...]     RT=t|g7 (como build_g7.sh)
#   Con los objetos ya traidos (r33g7/<v>/nx_obj, <vpc>/pc_obj): enlaza NRO/ELF de Switch (objetos <variante nx>) y los
#   binarios de PC (objetos <variante pc>: la misma fuente sin -DG7_NOIC), comprueba la tabla fastmem y corre la
#   verificacion de PC (por defecto: checks sanity lista). Registro: runs/chain_<nombre>.log
set -u
V=/home/djvemo/nsmbw-port/r9/r33g7; L=~/nsmbw-port/scripts/lim; N=$1; EN=$2; EP=$3; shift 3
steps=${*:-checks sanity lista}
cd $V; exec >> runs/chain_$N.log 2>&1
log() { echo "[$(date +%T)] $*"; }
log "chain $N (nx $EN, pc $EP, RT=${RT:-t}) start"
[[ $(ls $EN/nx_obj/output_*.o 2>/dev/null | wc -l) == 87 ]] || { log "faltan objetos nx de $EN"; exit 1; }
RT=${RT:-t} JOBS=${JOBS:-3} $L tools/build_g7.sh $N $EN nx > runs/link_nx_$N.log 2>&1 || { log "enlace nx FALLO"; tail -3 runs/link_nx_$N.log; exit 1; }
python3 ../r14g5/tools/check_fm_tab.py bin/nsmbw_$N.elf ../r14g5/sites.tsv > runs/fmtab_$N.txt 2>&1; log "fastmem: $(tail -2 runs/fmtab_$N.txt | tr '\n' ' ')"
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-size bin/nsmbw_$N.elf | tail -1
if [[ $(ls $EP/pc_obj/output_*.o 2>/dev/null | wc -l) == 87 ]]; then
  RT=${RT:-t} JOBS=${JOBS:-3} $L tools/build_g7.sh $N $EP pc > runs/link_pc_$N.log 2>&1 || { log "enlace pc FALLO"; tail -3 runs/link_pc_$N.log; exit 1; }
  tools/verify_g7.sh $N $steps > runs/verify_$N.log 2>&1; log "corridas de PC hechas: $(tail -1 runs/verify_$N.log)"
  for sc in title map walk lvl11; do grep "^chk_g7_${N}_${sc}_1x" runs/checks_$N.txt 2>/dev/null | cut -c1-110; done
else
  log "sin objetos de PC de $EP: solo enlace de Switch"
fi
log "chain $N done"
