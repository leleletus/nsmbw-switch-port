#!/usr/bin/env bash
# verify_m2.sh <name> [steps...]   Verificación en PC de bin/NSMBW_<name>_input_test de r24m2
# (copia de r22r7/tools/verify_m.sh; cada corrida del juego toma r9/render/pc/.runlock, NAND privada mp de render/pc).
#   checks : render/test/checks.sh a 1x (FRAMECHECK + ECACHE; title/map/walk/lvl11) + NWII_PINDEX_CHECK/NWII_DLMEMO_CHECK de R7
#   wcheck : NWII_WGP_CHECK (frente W), título 100 s
#   lista  : lista de demos por defecto de r21s (FORCE_DEMO + LOOP + NO_SAVE), 300 s (como runs/g6e1s_lista de r21s)
#   sanity long cb4 title off : verify_g5_copy.sh
# Resultados: r24m2/runs/<paso>_<name>.txt
set -u
M=/home/djvemo/nsmbw-port/r9/r24m2; R=/home/djvemo/nsmbw-port/r9/render
n=$1; shift
bin=$M/bin/NSMBW_${n}_input_test
lock() { flock "$R/pc/.runlock" "$@"; }
for st in "$@"; do
  case $st in
    checks)
      (cd $R && lock env RESS=1 NWII_PINDEX_CHECK=1 NWII_DLMEMO_CHECK=1 test/checks.sh "$bin" m2_$n) > $M/runs/checks_$n.txt 2>&1
      for sc in title map walk lvl11; do L=$R/pc/runs/chk_m2_${n}_${sc}_1x/run.log
        echo "$sc: $(grep '\[PINDEX_CHECK\]' $L | tail -1) | $(grep '\[DLMEMO_CHECK\]' $L | tail -1) | $(grep '\[FASTMEM\]' $L | tail -1) | $(tail -1 $L)"
      done >> $M/runs/checks_$n.txt ;;
    wcheck)
      rm -f $M/runs/wcheck_$n.*
      (cd $R && lock env NWII_INPUT_PHASES=6 NWII_WGP_CHECK=$M/runs/wcheck_$n EVERY=0 PROFILE=free RES=1 \
         pc/run.sh "$bin" m2_wcheck_$n 100 mp '0-100000:100:20:4') > /dev/null 2>&1
      { L=$R/pc/runs/m2_wcheck_$n/run.log
        head -3 $R/pc/runs/m2_wcheck_$n/summary.txt
        echo "scenes: $(grep -o 'scene=[0-9]* ([A-Z_]*)' "$L" | awk '{print $2}' | uniq | tr '\n' ' ')"
        grep -m1 '\[WGP\] write-gather' "$L"; grep -m1 'render worker:' "$L"; grep -m1 'GPUXF\] self-test' "$L"
        grep '\[FASTMEM\]' "$L" | tail -1; grep '\[IDXSNAP\]' "$L" | tail -1; grep '\[WGP\] check' "$L" | tail -3
        w=$M/runs/wcheck_$n.w f=$M/runs/wcheck_$n.f
        sw=$(stat -c %s $w) sf=$(stat -c %s $f)
        echo "stream: written $sw B, handed over $sf B, prefix equal: $(cmp -n $sf $w $f > /dev/null && echo yes || echo NO)"
      } > $M/runs/wcheck_$n.txt 2>&1
      rm -f $M/runs/wcheck_$n.w $M/runs/wcheck_$n.f ;;
    lista)
      rn=m2_lista_$n
      (cd $R && lock env NWII_FORCE_DEMO=desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo \
         NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 EVERY=300 PROFILE=free RES=1 pc/run.sh "$bin" $rn 300 mp '0-330:100:20:4') > /dev/null 2>&1
      out=$M/runs/lista_$n; rm -rf "$out"; mkdir -p "$out"; L=$R/pc/runs/$rn/run.log
      grep -E '^\[DEMO\]|\[SCENE\].*scene=' "$L" > "$out/demo.txt"
      cp $R/pc/runs/$rn/summary.txt "$out/"; grep -m3 -E 'FASTMEM\]|exit=' "$L" >> "$out/summary.txt"
      magick montage -label '%t' $(ls $R/pc/runs/$rn/t_*.png | sort -V) -tile 8x -geometry 240x180+2+2 "$out/hoja.jpg"
      { cat "$out/summary.txt"; grep '^\[DEMO\]' "$out/demo.txt"; } > $M/runs/lista_$n.txt ;;
    sanity|long|cb4|title|off)
      BIN=$bin OUT=$M/runs TAG=m2_$n $M/tools/verify_g5_copy.sh $st > /dev/null 2>&1
      mv -f $M/runs/$st.txt $M/runs/${st}_$n.txt
      rn=g5_m2_${n}_$st
      ls $R/pc/runs/$rn/t_*.png > /dev/null 2>&1 && \
        magick montage -label '%t' $(ls $R/pc/runs/$rn/t_*.png | sort -V) -tile 8x -geometry 240x180+2+2 $M/runs/hoja_${st}_$n.jpg ;;
  esac
  echo "$st done: $M/runs/${st}_$n.txt"
done
