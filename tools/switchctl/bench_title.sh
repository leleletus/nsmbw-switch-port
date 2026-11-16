#!/usr/bin/env bash
# bench_title.sh <file.nro> <name> [seconds_at_title=100]
#   env: SHOT_EVERY=10, BURST_FROM, BURST_SECS, NXDEST=<folder/file.nro under sdmc:/switch, folder must exist>, DEBUG_EXTRA,
#        PULL_PREFIX, NO_PRESS=1 (no button presses: deterministic runs)
#
# Automatic benchmark on the user's real Switch (Wi-Fi only, sys-botbase + nxlink + ftpd):
#   0. make sure the console is at the hbmenu (from the game, the HOME menu, an error dialog or a console app)
#   1. hbmenu: Y (NetLoader) and send the NRO with nxlink (default sdmc:/switch/NSMBW_bench/NSMBW_bench.nro; the folder
#      must exist, the netloader does not create folders: `curl -Q "MKD /switch/NSMBW_bench" ftp://IP:5000/`;
#      with no data/ there, the game uses sdmc:/switch/NSMBWnx: same save, config, debug.toml and logs/)
#   2. A on the strap screen, then idle at the title: the attract demos run (castle cutscene, then demo levels; the
#      desert with 4 players is the heaviest); screenshots every 10 s (SHOT_EVERY) and an optional 1/s burst
#   3. every screenshot is classified (estado.py): if the game is gone (crash dialog, black screen) the run stops,
#      the crash report is fetched and the console goes back to the hbmenu
#   4. close the game -> hbmenu -> ftpd classic -> download logs/ over FTP (+ restore debug.toml) -> + (hbmenu)
#   5. print the fps / thread-load table of the run
# Results: switch/real_logs/bench_<name>_<date>/ (nwii.log, profile_*.txt, shots, crash.log if any).
# Exit code: 0 ok, 3 the game crashed or did not start, 1 setup problem.
set -uo pipefail
NRO=$(realpath "$1"); NAME=$2; SECS=${3:-100}
IP=${SWITCH_IP:-<IP_SWITCH>}
HERE=$(cd "$(dirname "$0")" && pwd)
CTL="python3 $HERE/switchctl.py --ip $IP"
FTP=ftp://$IP:5000
OUT=/home/djvemo/nsmbw-port/switch/real_logs/bench_${NAME}_$(date +%m%d_%H%M)
mkdir -p "$OUT"
touch "$HERE/.ocupado"; trap 'rm -f "$HERE/.ocupado"' EXIT   # pauses keepalive.sh during the run
shot() { timeout 30 $CTL shot "$OUT/$1.jpg" > /dev/null 2>&1 || true; }
estado() {   # estado [name]: screenshot + classification (juego/hbmenu/home/oscuro/otro)
  local f=$OUT/${1:-.estado}.jpg
  timeout 30 $CTL shot "$f" > /dev/null 2>&1 && python3 "$HERE/estado.py" "$f" || echo "sin_conexion"
}
a_hbmenu() {  # bring the console back to the hbmenu by RECOGNIZING tiles (nav.py); never blind presses on the main menu
  NAV_TMP=$OUT/.nav.jpg python3 -W ignore "$HERE/nav.py" hbmenu
}
ftpd_open() { NAV_TMP=$OUT/.nav.jpg python3 -W ignore "$HERE/nav.py" ftpd || echo "[bench] WARNING: could not open ftpd safely"; }
ftpd_close() { $CTL click PLUS; sleep 4; }

echo "[bench] $NAME -> $OUT"
a_hbmenu || exit 1
shot 00_hbmenu
# DEBUG_EXTRA='NWII_A="1";NWII_B="1"': temporary top-level lines in sdmc:/switch/NSMBWnx/debug.toml for this
# run only (the original is saved in $OUT/debug.toml.orig and put back when the logs are downloaded).
DBG=$FTP/switch/NSMBWnx/debug.toml
MAESTRO=$HERE/debug.toml.maestro   # the user's real debug.toml (never contains bench lines)
restore_dbg() {   # put the master copy back on the SD and verify it (3 tries); return 1 if it can't
  local t
  for t in 1 2 3; do
    curl -s --max-time 60 -T "$MAESTRO" "$DBG"; sleep 2
    cmp -s <(curl -s --max-time 60 "$DBG") "$MAESTRO" && return 0
    sleep 3
  done
  return 1
}
# Every run starts by checking the SD: a debug.toml with leftover bench lines is replaced by the master copy,
# leftover XFB dumps are deleted. Then DEBUG_EXTRA='NWII_A="1";NWII_B="1"' adds temporary top-level lines on top of
# the MASTER copy (never on top of what is on the SD), and the master copy goes back when the logs are downloaded.
ftpd_open
curl -s --max-time 60 -o "$OUT/debug.toml.sd_inicio" "$DBG"
if ! cmp -s "$OUT/debug.toml.sd_inicio" "$MAESTRO"; then
  echo "[bench] debug.toml on the SD differs from the master copy: restoring it"
  restore_dbg || { echo "[bench] FATAL: could not restore debug.toml"; ftpd_close; exit 4; }
fi
for f in $(curl -s --max-time 30 "$FTP/switch/NSMBWnx/logs/" | awk '{print $NF}' | grep '^xfb'); do
  curl -s --max-time 30 -Q "DELE /switch/NSMBWnx/logs/$f" "$FTP/" -o /dev/null
done
if [[ -n ${DEBUG_EXTRA:-} ]]; then
  cp "$MAESTRO" "$OUT/debug.toml.orig"
  { echo "# bench_title.sh DEBUG_EXTRA (temporary)"; sed -E 's/;[[:space:]]*([A-Za-z_][A-Za-z0-9_]*[[:space:]]*=)/\n\1/g' <<< "$DEBUG_EXTRA"; if [[ -n ${DEBUG_DROP:-} ]]; then grep -v -E "$DEBUG_DROP" "$MAESTRO"; else cat "$MAESTRO"; fi | grep -v -E "^[[:space:]]*($(sed -E 's/;[[:space:]]*([A-Za-z_][A-Za-z0-9_]*[[:space:]]*=)/\n\1/g' <<< "$DEBUG_EXTRA" | sed -E 's/[[:space:]]*=.*//' | paste -sd'|'))[[:space:]]*="; } > "$OUT/debug.toml.run"   # 15-nov: DEBUG_DROP='<regex>' removes master lines for this run (e.g. '^profile')
  curl -s --max-time 60 -T "$OUT/debug.toml.run" "$DBG"
  cmp -s <(curl -s --max-time 60 "$DBG") "$OUT/debug.toml.run" || { echo "[bench] FATAL: could not write debug.toml"; restore_dbg; ftpd_close; exit 4; }
  echo "[bench] debug.toml + $DEBUG_EXTRA${DEBUG_DROP:+ | sin lineas: $DEBUG_DROP}"
fi
# 14-nov: the netloader does not create folders ("open: err=2" when the user deleted /switch/NSMBW_bench): make sure
# the destination folder exists while ftpd is still open, and treat a failed send as a SETUP problem (exit 1 = the
# queue stops) instead of waiting and then running the next jobs into the same wall.
NXD=${NXDEST:-NSMBW_bench/NSMBW_bench.nro}
curl -s --max-time 30 -Q "MKD /switch/$(dirname "$NXD")" "$FTP/" -o /dev/null 2>&1 || true
curl -s --max-time 30 "$FTP/switch/" | awk '{print $NF}' | grep -qx "$(dirname "$NXD" | cut -d/ -f1)" || echo "[bench] WARNING: /switch/$(dirname "$NXD") not found on the SD"
ftpd_close
$CTL click Y; sleep 2
timeout 300 /opt/devkitpro/tools/bin/nxlink -a "$IP" -p "$NXD" "$NRO" > "$OUT/nxlink.log" 2>&1
NXRC=$?
echo "[bench] sent (nxlink rc $NXRC); waiting for boot"
sleep 25
CRASH=""; SETUP_FAIL=""
s=$(estado 01_strap)
if [[ $s != juego ]]; then sleep 15; s=$(estado 01_strap_b); fi
[[ $s == juego ]] || CRASH="did not start (screen: $s)"
if [[ -n $CRASH && $NXRC != 0 ]]; then SETUP_FAIL=1; CRASH="the NRO was not sent (nxlink rc $NXRC; screen: $s)"; fi
T0=$(date +%s)
el() { echo $(($(date +%s) - T0)); }
if [[ -z $CRASH ]]; then
  [[ -z ${NO_PRESS:-} ]] && { $CTL click A; sleep 3; $CTL click A; }   # strap screen, then the next (NO_PRESS=1: no input at all)
  T0=$(date +%s)
  EVERY=${SHOT_EVERY:-10}   # seconds between screenshots (the status-monitor bar at the bottom shows per-core CPU, GPU, RAM)
  # Burst: BURST_FROM=<s after the title> BURST_SECS=<n>: one screenshot per second (as fast as pixelPeek allows)
  # for that stretch, then sheet.py builds burst_sheet.jpg (fps counter + status bar of each).
  BF=${BURST_FROM:-0}; BS=${BURST_SECS:-0}
  bad=0
  for ((t = EVERY; t <= SECS; t += EVERY)); do
    if ((BS > 0 && t > BF && $(el) <= BF + BS)); then
      while (($(el) < BF)); do sleep 0.5; done
      while (($(el) < BF + BS)); do
        s0=$(date +%s%3N); shot "$(printf 'burst_%03d' "$(el)")"
        d=$((1000 - ($(date +%s%3N) - s0))); ((d > 0)) && sleep "0.$(printf '%03d' "$d")"
      done
      python3 "$HERE/sheet.py" "$OUT" > /dev/null || true
    fi
    now=$(el); ((now < t)) && sleep $((t - now))
    # keepalive.sh pauses during a bench, so the bench itself keeps the console awake on long runs
    # (auto-sleep is 10 min): the same harmless right-stick nudge every ~4 min
    if (( $(date +%s) - ${LAST_NUDGE:=$(date +%s)} >= 240 )); then timeout 15 $CTL stick RIGHT 0 12000 200 > /dev/null 2>&1; LAST_NUDGE=$(date +%s); fi
    s=$(estado "$(printf '02_title_%03d' $t)")
    if [[ $s == juego ]]; then bad=0; else ((bad++)); fi
    if ((bad >= 2)); then CRASH="game gone at ~$(el) s after the title (screen: $s)"; break; fi
  done
  if [[ -z $CRASH ]]; then
    rest=$((SECS - $(el)))
    while ((rest > 0)); do s_=$((rest < 200 ? rest : 200)); sleep "$s_"; rest=$((rest - s_))
      ((rest > 0)) && timeout 15 $CTL stick RIGHT 0 12000 200 > /dev/null 2>&1; done
  fi
fi
[[ -n $CRASH ]] && echo "[bench] CRASH: $CRASH" | tee "$OUT/CRASH.txt"

echo "[bench] back to the hbmenu"
a_hbmenu
shot 03_hbmenu
ftpd_open
for f in nwii.log profile_main.txt profile_cpu.txt profile_gxw.txt; do
  curl -s --max-time 180 -o "$OUT/$f" "$FTP/switch/NSMBWnx/logs/$f" || echo "[bench] FTP failed for $f"
done
# PULL_PREFIX=xfbgl_ : also download logs/<prefix>* (e.g. XFB dumps) and delete them from the SD afterwards
if [[ -n ${PULL_PREFIX:-} ]]; then
  n=0
  for f in $(curl -s --max-time 30 "$FTP/switch/NSMBWnx/logs/" | awk '{print $NF}' | grep "^$PULL_PREFIX"); do
    curl -s --max-time 120 -o "$OUT/$f" "$FTP/switch/NSMBWnx/logs/$f" && [[ -s $OUT/$f ]] &&
      curl -s --max-time 30 -Q "DELE /switch/NSMBWnx/logs/$f" "$FTP/" -o /dev/null && ((n++))
  done
  echo "[bench] pulled and deleted $n files logs/$PULL_PREFIX*"
fi
# PULL_CMD='<command>': run it while ftpd is open (e.g. a tool that downloads another SD folder); output to pull_cmd.log
if [[ -n ${PULL_CMD:-} ]]; then bash -c "$PULL_CMD" > "$OUT/pull_cmd.log" 2>&1; echo "[bench] PULL_CMD rc $? (pull_cmd.log)"; tail -3 "$OUT/pull_cmd.log"; fi
if [[ -n $CRASH ]]; then
  c=$(curl -s --max-time 20 "$FTP/atmosphere/crash_reports/" | awk '{print $NF}' | grep '\.log$' | sort | tail -1)
  [[ -n $c ]] && curl -s --max-time 30 -o "$OUT/crash.log" "$FTP/atmosphere/crash_reports/$c" && echo "[bench] crash report $c (check its date)"
fi
DBG_FAIL=""
if [[ -s $OUT/debug.toml.orig ]]; then
  if restore_dbg; then echo "[bench] debug.toml restored"; else DBG_FAIL=1; echo "[bench] FATAL: debug.toml NOT restored"; fi
fi
ftpd_close
a_hbmenu
shot 04_back_hbmenu

python3 "$HERE/hoja_capturas.py" "$OUT" > /dev/null 2>&1 || true   # capturas.jpg: review the image by eye
echo "[bench] fps per 5 s window (fps / render / game / core2):"
grep "\[NX\] game" "$OUT/nwii.log" 2>/dev/null | sed -E 's/.*game ([0-9.]+) fps.*cpu ([0-9]+) MHz.*threads main ([0-9]+)%, cpu ([0-9]+)%.*gxw ([0-9]+)%.*/\1 \3 \4 \5 \2/' |
  awk 'NR > 4 && $1 > 5 { n++; s += $1; if (min == "" || $1 < min) min = $1; printf "  %5.1f fps  render %3d%%  juego %3d%%  nucleo2 %3d%%  (%d MHz)\n", $1, $2, $3, $4, $5 }
       END { if (n) printf "[bench] %d windows, mean %.1f fps, min %.1f fps\n", n, s / n, min }' | tee "$OUT/resumen.txt"
# 14-nov: automatic image check against clean reference runs (tools/switchctl/verif_refs.txt), only for runs with the
# fixed demo list. A suspicious run is written to resumen.txt (with the word CRASH so cola.sh copies it to
# cola_hecho.txt) and raises a desktop alert; the numbers of such a run must not be trusted.
if [[ ${DEBUG_EXTRA:-} == *'NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo"'* ]]; then
  VI=$(python3 -W ignore "$HERE/verif_imagen.py" "$OUT" 2>&1 | tail -1)
  echo "[bench] $VI"
  if [[ $VI == *SOSPECHOSA* ]]; then
    echo "[bench] CRASH-GRAFICO? $VI (ver capturas.jpg y verif_imagen.txt)" >> "$OUT/resumen.txt"
    notify-send -u critical "⚠ Switch: imagen sospechosa en $NAME" "$VI" 2>/dev/null || true
  else
    echo "[bench] $VI" >> "$OUT/resumen.txt"
  fi
fi
# 15-nov: clock check. The user's 1020/460 MHz limit in sys-clk is TEMPORARY: a console reboot brings the overclock
# back (Z15 ran at 1785/1152 and its numbers were useless). Any run whose log shows other clocks is flagged, the
# user is notified and the queue stops (exit 4). EXPECT_CLK="cpu gpu" overrides; EXPECT_CLK=any disables.
CLK_FAIL=""
if [[ ${EXPECT_CLK:-1020 460} != any ]]; then
  read -r ECPU EGPU <<< "${EXPECT_CLK:-1020 460}"
  CLK=$(cat "$OUT"/nwii*.log 2>/dev/null | grep -ao "cpu [0-9]* MHz, gpu [0-9]* MHz" | sort | uniq -c | sort -rn | grep -v "cpu $ECPU MHz, gpu $EGPU MHz" | head -1 | sed 's/^ *//')
  if [[ -n $CLK ]]; then
    CLK_FAIL=1
    echo "[bench] RELOJ: $CLK (esperado $ECPU/$EGPU): corrida NO comparable; hay que volver a limitar sys-clk" >> "$OUT/resumen.txt"
    notify-send -u critical "⚠ Switch con overclock en $NAME" "$CLK (esperado $ECPU/$EGPU). ¿Se reinició? Hay que volver a limitar sys-clk." 2>/dev/null || true
  fi
fi
[[ -n $CRASH || -n $DBG_FAIL ]] && notify-send -u critical "⚠ Switch: fallo en $NAME" "${CRASH:-debug.toml sin restaurar}. Si se reinició, el overclock ha vuelto: limitar sys-clk a 1020/460." 2>/dev/null || true
[[ -n $DBG_FAIL ]] && { echo "[bench] FATAL: debug.toml NOT restored" >> "$OUT/resumen.txt"; exit 4; }
[[ -n ${SETUP_FAIL:-} ]] && { echo "[bench] SETUP: $CRASH" >> "$OUT/resumen.txt"; exit 1; }
[[ -n $CRASH ]] && { echo "[bench] CRASH: $CRASH" >> "$OUT/resumen.txt"; exit 3; }
[[ -n $CLK_FAIL ]] && exit 4
exit 0
