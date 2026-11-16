#!/usr/bin/env bash
# robjs.sh <pcgcov|nxg1|pcg1>    objetos de r32dx/src (e2 con una sentencia por línea), pensado para la otra laptop:
#   scripts/remoto.sh push r9/r32dx/tools r9/r32dx/src
#   scripts/remoto.sh run dx_objs 'JOBS=6 r9/r32dx/tools/robjs.sh pcgcov && JOBS=6 r9/r32dx/tools/robjs.sh nxg1'
#   scripts/remoto.sh pull r9/r32dx/pc_gcov r9/r32dx/nx_g1
# pcgcov: PC, -O2 --coverage -fprofile-update=single  -> pc_gcov/output_N.o + .gcno (cuentas exactas por línea)
# nxg1:   Switch, EXACTAMENTE la orden de m2 + -g1    -> nx_g1/output_N.o (mismo código que e2/nx_obj + tabla de líneas)
# pcg1:   PC, la orden de m2 (-O3) + -g1               -> pc_g1/output_N.o (para validar el método contra perf en x86)
# Plantillas de compilación = las de r14g5 (tools/templates, copiadas de r30g6c3).
set -euo pipefail
W=$1; V=/home/djvemo/nsmbw-port/r9/r32dx; G5=/home/djvemo/nsmbw-port/r9/r14g5; E=$V/src
case $W in
  pcgcov) O=$V/pc_gcov; T=pc; RUNDIR=$G5/export/build; SUB='-O3/-O2 --coverage -fprofile-update=single' ;;
  nxg1)   O=$V/nx_g1;   T=nx; RUNDIR=$G5/nx/build;     SUB='' ; EXTRA='-g1' ;;
  pcg1)   O=$V/pc_g1;   T=pc; RUNDIR=$G5/export/build; SUB='' ; EXTRA='-g1' ;;
  *) exit 2 ;;
esac
mkdir -p "$O" "$RUNDIR"
tmpl=$(cat $V/tools/templates/$T.cmd); tmpl=${tmpl//@EXPORT@/$E}
if [[ $W == pcgcov ]]; then tmpl=${tmpl/ -O3 / -O2 --coverage -fprofile-update=single }; else tmpl=${tmpl/ -o \$out/ ${EXTRA} -o \$out}; fi
{
  echo "rule cc"
  echo "  command = cd $RUNDIR && nice -n 5 $tmpl"
  echo "  description = $W \$in"
  for f in "$E"/output_*.cpp; do b=$(basename "$f" .cpp); echo "build $O/$b.o: cc $f"; done
} > "$O/build.ninja"
echo "$tmpl" > "$O/compile.cmd"
echo "[$(date +%T)] $W start"
ninja -f "$O/build.ninja" -j"${JOBS:-2}"
echo "[$(date +%T)] $W ok: $(ls "$O"/output_*.o | wc -l) objetos"
