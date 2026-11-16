#!/usr/bin/env bash
# verify_m.sh <name> [steps...]   Verificación en PC del binario bin/NSMBW_<name>_input_test de r22r7 (copia de r18m).
#   checks : render/test/checks.sh a 1x (FRAMECHECK + ECACHE; title/map/walk/lvl11)
#   wcheck : NWII_WGP_CHECK (frente W): lo escrito por el juego == lo entregado, título 100 s
#   axfx   : frente A2: AX en línea, NWII_AXFX_KCHECK=1 NWII_AXFX_VERIFY=50 NWII_AXFX_DVERIFY=20, título+demos 200 s
#   sanity long cb4 title off : corridas de r14g5/tools/verify_g5.sh (copia: verify_g5_copy.sh)
# Cada corrida del juego toma r9/render/pc/.runlock. Resultados: r18m/runs/<step>_<name>.txt
set -u
M=/home/djvemo/nsmbw-port/r9/r27x2; R=/home/djvemo/nsmbw-port/r9/render
n=$1; shift
bin=$M/bin/NSMBW_${n}_input_test
lock() { flock "$R/pc/.runlock" "$@"; }
for st in "$@"; do
  case $st in
    checks)
      (cd $R && lock env RESS=1 test/checks.sh "$bin" r7_$n) > $M/runs/checks_$n.txt 2>&1 ;;
    wcheck)
      rm -f $M/runs/wcheck_$n.*
      (cd $R && lock env NWII_INPUT_PHASES=6 NWII_WGP_CHECK=$M/runs/wcheck_$n EVERY=0 PROFILE=free RES=1 \
         pc/run.sh "$bin" r7_wcheck_$n 100 mp '0-100000:100:20:4') > /dev/null 2>&1
      { L=$R/pc/runs/r7_wcheck_$n/run.log
        head -3 $R/pc/runs/r7_wcheck_$n/summary.txt
        echo "scenes: $(grep -o 'scene=[0-9]* ([A-Z_]*)' "$L" | awk '{print $2}' | uniq | tr '\n' ' ')"
        grep -m1 '\[WGP\] write-gather' "$L"; grep -m1 'render worker:' "$L"; grep -m1 'GPUXF\] self-test' "$L"
        grep '\[FASTMEM\]' "$L" | tail -1; grep '\[IDXSNAP\]' "$L" | tail -1; grep '\[WGP\] check' "$L" | tail -3
        w=$M/runs/wcheck_$n.w f=$M/runs/wcheck_$n.f
        sw=$(stat -c %s $w) sf=$(stat -c %s $f)
        echo "stream: written $sw B, handed over $sf B, prefix equal: $(cmp -n $sf $w $f > /dev/null && echo yes || echo NO)"
      } > $M/runs/wcheck_$n.txt 2>&1
      rm -f $M/runs/wcheck_$n.w $M/runs/wcheck_$n.f ;;
    axfx)
      lock $M/tools/audio_run.sh "$bin" axfx_$n 200 NWII_AX_THREAD=0 NWII_AXFX_KCHECK=1 NWII_AXFX_VERIFY=50 NWII_AXFX_DVERIFY=20 > /dev/null 2>&1
      { cat $M/runs/axfx_$n/scenes.txt; L=$M/runs/axfx_$n/run.log
        grep -E 'VERIFY|verify|DVERIFY|kcheck|\[A2\]|a2delay' "$L" | tail -8
        grep '\[FASTMEM\]' "$L" | tail -1; tail -1 "$L"; } > $M/runs/axfx_$n.txt 2>&1 ;;
    sanity|long|cb4|title|off)
      BIN=$bin OUT=$M/runs TAG=r7_$n $M/tools/verify_g5_copy.sh $st > /dev/null 2>&1
      mv -f $M/runs/$st.txt $M/runs/${st}_$n.txt ;;
  esac
  echo "$st done: $M/runs/${st}_$n.txt"
done
