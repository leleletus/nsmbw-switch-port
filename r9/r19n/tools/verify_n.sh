#!/usr/bin/env bash
# verify_n.sh <name> [steps...]   Verificación en PC de r19n (bin/NSMBW_<name>_input_test).
#   checks    : render/test/checks.sh a 1x (FRAMECHECK + ECACHE; title/map/walk/lvl11)
#   det_<sc>  : render/test/det.sh (reloj virtual NWII_REALTIME=0, sin saltar cuadros, todos los XFB) con m1 (r18m)
#               y con <name>; match.py compara cuadro a cuadro (sc = title | map | lvl11). Lo más fuerte: mismo
#               juego bit a bit => mismas imágenes en los mismos cuadros.
#   vtitle    : demos del título 300 s con NWII_NATIVE_VERIFY=$VERIFY (2000 por defecto): cada función nativa
#               contra el código generado sobre datos reales ([N1] ... verify)
#   sanity long cb4 title off : corridas de r14g5/tools/verify_g5.sh (copia: verify_g5_copy.sh)
# Cada corrida toma r9/render/pc/.runlock. Resultados: r19n/runs/<step>_<name>.txt
set -u
M=/home/djvemo/nsmbw-port/r9/r19n; R=/home/djvemo/nsmbw-port/r9/render
n=$1; shift
bin=$M/bin/NSMBW_${n}_input_test
ref=/home/djvemo/nsmbw-port/r9/r18m/bin/NSMBW_m1_input_test
lock() { flock "$R/pc/.runlock" "$@"; }
for st in "$@"; do
  case $st in
    checks)
      (cd $R && lock env RESS=1 test/checks.sh "$bin" n19_$n) > $M/runs/checks_$n.txt 2>&1 ;;
    det_*)
      sc=${st#det_}; secs=${DET_SECS:-150}
      [[ -d $R/test/runs/n19det_m1_$sc ]] || (cd $R && lock test/det.sh "$ref" n19det_m1_$sc $sc $secs) > /dev/null 2>&1
      (cd $R && lock test/det.sh "$bin" n19det_${n}_$sc $sc $secs) > /dev/null 2>&1
      { python3 -I $R/test/match.py $R/test/runs/n19det_m1_$sc $R/test/runs/n19det_${n}_$sc
        grep -h 'scene=' $R/test/runs/n19det_${n}_$sc/run.log | grep -o 'scene=[0-9]* ([A-Z_]*)' | awk '{print $2}' | uniq | tr '\n' ' '; echo
        grep -h '^\[N1\] native SDK' $R/test/runs/n19det_${n}_$sc/run.log | head -1
        tail -1 $R/test/runs/n19det_${n}_$sc/run.log; } > $M/runs/${st}_$n.txt 2>&1 ;;
    vtitle)
      (cd $R && lock env NWII_NATIVE_VERIFY=${VERIFY:-2000} NWII_INPUT_PHASES=6 EVERY=0 PROFILE=free RES=1 \
         pc/run.sh "$bin" n19_vtitle_$n ${T_VT:-300} mp '0-330:100:20:4') > /dev/null 2>&1
      { L=$R/pc/runs/n19_vtitle_$n/run.log
        head -3 $R/pc/runs/n19_vtitle_$n/summary.txt
        echo "scenes: $(grep -o 'scene=[0-9]* ([A-Z_]*)' "$L" | awk '{print $2}' | uniq | tr '\n' ' ')"
        grep '^\[N1\].*\(final\|total\|MISMATCH\)' "$L" | head -80
        grep '\[FASTMEM\]' "$L" | tail -1; tail -1 "$L"; } > $M/runs/vtitle_$n.txt 2>&1 ;;
    sanity|long|cb4|title|off)
      BIN=$bin OUT=$M/runs TAG=n19_$n $M/tools/verify_g5_copy.sh $st > /dev/null 2>&1
      mv -f $M/runs/$st.txt $M/runs/${st}_$n.txt ;;
  esac
  echo "$st done: $M/runs/${st}_$n.txt"
done
