#!/usr/bin/env bash
# keepalive.sh [start|stop|status] [seconds=540]
#
# Keeps the user's Switch awake (no auto-sleep / screen dim) while it waits for automated tests:
# every 9 min (default) sys-botbase sends a short right-stick nudge UP at 37 % (12000/32767, 0.2 s).
#   - hbmenu ignores it (tested 8-nov: moves the selection only above ~50 %; 16000 still ignored),
#   - the game ignores it (switch_input.cpp reads only the X axis of the right stick, for tilt),
#   - the system sees it as controller input, which restarts the auto-sleep / dim timer.
# Skipped while bench_title.sh runs (it creates tools/switchctl/.ocupado) so it never touches a measurement.
# The loop runs detached (setsid), survives this terminal, logs to tools/switchctl/keepalive.log.
HERE=$(cd "$(dirname "$0")" && pwd)
PIDF=$HERE/.keepalive.pid
LOG=$HERE/keepalive.log
IP=${SWITCH_IP:-<IP_SWITCH>}
CMD=${1:-start}
SECS=${2:-540}

running() { [[ -f $PIDF ]] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }

case $CMD in
  start)
    if running; then echo "ya está en marcha (pid $(cat "$PIDF"))"; exit 0; fi
    setsid bash "$0" loop "$SECS" >> "$LOG" 2>&1 < /dev/null &
    echo $! > "$PIDF"
    echo "keepalive en marcha (pid $!), toque cada $SECS s; registro: $LOG"
    ;;
  stop)
    if running; then kill "$(cat "$PIDF")"; echo "keepalive detenido"; else echo "no estaba en marcha"; fi
    rm -f "$PIDF"
    ;;
  status)
    if running; then echo "en marcha (pid $(cat "$PIDF"))"; tail -3 "$LOG"; else echo "detenido"; fi
    ;;
  loop)
    while true; do
      # 10-nov: the user wants the nudge always, also during measurements (it is harmless for the game)
      if timeout 15 python3 "$HERE/switchctl.py" --ip "$IP" stick RIGHT 0 12000 200 2>/dev/null; then
        echo "$(date '+%F %T') toque enviado"
      else
        echo "$(date '+%F %T') la consola no responde (¿apagada o sin Wi-Fi?)"
      fi
      sleep "$SECS"
    done
    ;;
  *) echo "uso: $0 [start|stop|status] [segundos]"; exit 1 ;;
esac
