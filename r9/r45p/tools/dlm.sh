#!/usr/bin/env bash
# [r45p] dlm.sh <bin_input_test> <tag> <modo: check|perf> <escena...>: NWII_P_DLMERGE en PC a 1.5789x.
#  check: NWII_P_DLMERGE=1 + NWII_P_DLMERGE_CHECK=5 (fundido contra partido, píxel a píxel: [D40MERGE_CHECK]) + FRAMECHECK=10
#  perf : NWII_RPHASE=1 sin y con NWII_P_DLMERGE=1 (fases del render por fotograma)
# escenas: selector, mapa o una demo (castillo, agua_pinguino, desierto4j, ...). -> test/dlm_<tag>_<modo>.txt
bin=$(realpath "$1"); cd /home/djvemo/nsmbw-port/r9/r41m; tag=$2; modo=$3; shift 3
O=/home/djvemo/nsmbw-port/r9/r45p/test/dlm_${tag}_$modo.txt; : > $O
for sc in "$@"; do
  case $sc in
    selector) ex=(NWII_INPUT_PHASES=3 NWII_INPUT_SCRIPT_1=300-306:1000:100:6); scr='0-100000:100:20:4' ;;
    mapa) ex=(NWII_INPUT_PHASES=3); scr='0-100000:100:20:4' ;;
    *) ex=(NWII_FORCE_DEMO=$sc NWII_FORCE_DEMO_LOOP=1); scr='0-130:100:20:4' ;;
  esac
  if [[ $modo == check ]]; then vs=(chk); else vs=(ctl dlm); fi
  for v in "${vs[@]}"; do
    vv=(); [[ $v != ctl ]] && vv=(NWII_P_DLMERGE=1)
    [[ $v == chk ]] && vv+=(NWII_P_DLMERGE_CHECK=5 NWII_R9_FRAMECHECK=10)
    [[ $modo == perf ]] && vv+=(NWII_RPHASE=1)
    name=dlm_${tag}_${sc}_$v
    flock pc/.runlock env NWII_NO_SAVE=1 "${ex[@]}" "${vv[@]}" EVERY=0 PROFILE=free RES=1.5789 pc/run.sh "$bin" $name 130 mp "$scr" > /dev/null 2>&1
    L=pc/runs/$name/run.log
    { echo "== $sc $v | $(tail -1 $L)"
      grep -a '^\[P45DL\]' $L | tail -1
      grep -a '^\[D40MERGE_CHECK\]' $L | tail -1; grep -a '^\[D40MERGE_CHECK\]' $L | grep -v ' 0 pixels differ' | head -3
      grep -a '^\[FRAMECHECK\]' $L | tail -1 | cut -c1-120
      grep -a '^\[RPHASE\]' $L | tail -1 | cut -c1-260
      grep -a '^\[RPROF\]' $L | tail -1 | grep -o 'render_us/frame=[0-9]*\|gx_draws/frame=[0-9]*\|gl_draws/frame=[0-9]*' | tr '\n' ' '; echo; } >> $O
  done
done
echo FIN >> $O
