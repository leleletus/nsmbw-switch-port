#!/usr/bin/env bash
# [r41m] pc_m6h.sh <n>: verificación de PC de m6h (bin/NSMBW_<n>):
#  P1 por defecto: FC 5 demos con las comprobaciones de D y R2 + checks 1x.
#  P2 «todo» (+ NWII_D_MERGE NWII_W_AGUA NWII_W_WGP): FC con MERGE_CHECK + checks 1x + estrés (c) 5×150 s.
#  P3 «audio» (todo + NWII_A_MIX=1 NWII_A_NOSPIN=1 NWII_A_REQSPIN=200 + NWII_AX_VERIFY=1): FC + checks 1x + estrés 5×150 s.
#  P4 agua: NWII_W_AGUA_CHECK (con NWII_S2_OFF=1, como W) 200 s real + 200 s control nulo; NWII_W_WGP_SELFTEST.
cd /home/djvemo/nsmbw-port/r9/r41m; N=${1:?n}
CK=(NWII_D_DROPFAST_CHECK=1 NWII_D_LIGHTPARSE_CHECK=1 NWII_D_FASTDECODE_CHECK=1 NWII_D_FASTPREP_CHECK=1
    NWII_R2_CLEARTEX_CHECK=1 NWII_GPU_XF_BASEINST=0 NWII_SHADER_SSO=1)
SL=(NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000)
TODO=(NWII_D_MERGE=1 NWII_W_AGUA=1 NWII_W_WGP=1)
AUD=(NWII_A_MIX=1 NWII_A_NOSPIN=1 NWII_A_REQSPIN=200)
dchk() { for k in '\[D40CHECK\]' '\[D40DEC_CHECK\]' '\[D40PREP_CHECK\]' '\[D40L\] parses'; do grep -a "^$k" $1 | tail -1 | grep -oE '(distintos|mismatches)=[0-9]+|prediccion fallida [0-9]+' | tr '\n' ' '; done; }
fc() {  # fc <tag> [env...]
  local t=$1; shift
  { echo "env: $* ${CK[*]} ${SL[*]}"
    test/demos_fc.sh bin/NSMBW_${N}_input_test $t 100 "$@" "${CK[@]}" "${SL[@]}"
    for d in desierto4j agua_pinguino cielo_paratroopas nubes_plataformas castillo; do L=pc/runs/fc_${t}_$d/run.log
      echo "$d: D: $(dchk $L)| MERGE $(grep -a '^\[D40MERGE_CHECK\]' $L | tail -1 | cut -c1-90) | $(grep -a '^\[WAGUA\]' $L | tail -1 | cut -c1-110) | $(grep -a '^\[WGP\] r44w' $L | head -1) | AXV $(grep -a '^\[AXV\]' $L | grep -v MISMATCH | tail -1 | cut -c1-90) MISMATCH $(grep -a -c 'AXV\] MISMATCH' $L) | VIGIA $(grep -a -c '^\[VIGIA\] #' $L) | $(grep -a -m1 '^\[M6\]' $L | grep -o 'apagados.*' | cut -c1-60)"; done
    echo FIN; } > test/fc_$t.txt 2>&1
}
fc ${N}_def
tools/pc_checks_m6.sh $N ${N}_def NWII_D_DROPFAST_CHECK=1
fc ${N}_todo "${TODO[@]}" NWII_D_MERGE_CHECK=10
tools/pc_checks_m6.sh $N ${N}_todo "${TODO[@]}" NWII_D_MERGE_CHECK=10 NWII_D_DROPFAST_CHECK=1
tools/estres_m6e.sh bin/NSMBW_$N ${N}_todo 5 "${TODO[@]}"
fc ${N}_audio "${TODO[@]}" "${AUD[@]}" NWII_D_MERGE_CHECK=10 NWII_AX_VERIFY=1
tools/pc_checks_m6.sh $N ${N}_audio "${TODO[@]}" "${AUD[@]}" NWII_AX_VERIFY=1 NWII_D_DROPFAST_CHECK=1
tools/estres_m6e.sh bin/NSMBW_$N ${N}_audio 5 "${TODO[@]}" "${AUD[@]}"
O=test/agua_$N.txt; : > $O
for m in real null; do
  ex=(); [[ $m == null ]] && ex=(NWII_W_AGUA_CHECK_NULL=1)
  flock pc/.runlock tools/pcrun.sh bin/NSMBW_$N chkagua_${N}_$m 200 NWII_NO_SAVE=1 NWII_FORCE_DEMO=agua_pinguino NWII_FORCE_DEMO_LOOP=1 \
    NWII_W_AGUA_CHECK=1 NWII_S2_OFF=1 NWII_W_WGP=1 NWII_W_WGP_SELFTEST=1 "${ex[@]}"
  L=pc/runs/chkagua_${N}_$m/run.log
  { echo "== $m: $(grep -a 'WAGUA\] check:' $L | tail -1)"; grep -a 'MEMORIA\|REGISTROS\|check: PIPE\|selftest\|SELFTEST' $L | head -10; grep -a -m2 '^\[WAGUA\]' $L | cut -c1-200; tail -1 $L; } >> $O
done
echo FIN >> $O
