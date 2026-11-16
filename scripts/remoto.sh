#!/usr/bin/env bash
# remoto.sh: use the second laptop (ssh alias nsmbw-build = <IP_SWITCH>, 12 threads, 31 GB) as a build machine.
# Same paths on both machines (/home/djvemo/nsmbw-port). No AI needed: plain commands.
#
#   remoto.sh push <path> [...]      copy paths (relative to ~/nsmbw-port) there      (log: remoto/copias/)
#   remoto.sh pull <path> [...]      bring paths back from there
#   remoto.sh run <name> <command>   start <command> there, detached, inside ~/nsmbw-port, with DEVKITPRO set.
#                                    There: ~/nsmbw-port/remoto/<name>/{log,rc,cmd,t0,t1}
#   remoto.sh estado                 JSON with the remote machine state and every job (used by the GUI)
#   remoto.sh log <name> [n]         last n lines (default 40) of a job's log
#   remoto.sh parar <name>           stop a job
# The window scripts/agentes_gui.py shows all of this ("Otra laptop" box).
set -euo pipefail
H=nsmbw-build
P=$HOME/nsmbw-port
cmd=${1:-}; shift || true
case $cmd in
  push|pull)
    mkdir -p "$P/remoto/copias"
    lg=$P/remoto/copias/$(date +%m%d_%H%M%S)_$cmd.log
    echo "$cmd $*" > "$lg"
    cd "$P"
    for x in "$@"; do
      x=${x%/}
      if [[ $cmd == push ]]; then
        ssh $H "mkdir -p ~/nsmbw-port/$(dirname "$x")"
        rsync -a --info=progress2,stats1 --no-inc-recursive "$x" "$H:nsmbw-port/$(dirname "$x")/" 2>&1 | tr '\r' '\n' >> "$lg"
      else
        mkdir -p "$(dirname "$x")"
        rsync -a --info=progress2,stats1 --no-inc-recursive "$H:nsmbw-port/$x" "$(dirname "$x")/" 2>&1 | tr '\r' '\n' >> "$lg"
      fi
    done
    echo "rc=0" >> "$lg"; tail -3 "$lg" ;;
  run)
    name=${1:?name}; shift
    c64=$(printf '%s\n' "$*" | base64 -w0)
    ssh $H bash -s <<EOF2
d=\$HOME/nsmbw-port/remoto/$name
if [ -f \$d/t0 ] && [ ! -f \$d/rc ]; then echo "ya corre: $name"; exit 1; fi
rm -rf \$d; mkdir -p \$d; echo $c64 | base64 -d > \$d/cmd; date +%s > \$d/t0
cd \$HOME/nsmbw-port
export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64
setsid nohup bash -c 'echo \$\$ > "\$0/pid"; bash "\$0/cmd"; rc=\$?; date +%s > "\$0/t1"; echo \$rc > "\$0/rc"' \$d > \$d/log 2>&1 < /dev/null &
echo "lanzado: $name"
EOF2
    ;;
  log) ssh $H bash -s <<< "tail -n ${2:-40} \$HOME/nsmbw-port/remoto/${1:?name}/log" ;;
  parar) ssh $H bash -s <<< "p=\$(cat \$HOME/nsmbw-port/remoto/${1:?name}/pid); s=\$(ps -o sid= -p \$p | tr -d ' '); pkill -TERM -s \${s:-\$p}; echo 143 > \$HOME/nsmbw-port/remoto/${1:?name}/rc; echo parado" ;;
  estado)
    ssh -o BatchMode=yes -o ConnectTimeout=4 $H python3 - <<'PY'
import glob, json, os, re, subprocess, time
R = os.path.expanduser("~/nsmbw-port/remoto")
st = {"ok": True, "host": os.uname().nodename, "carga": open("/proc/loadavg").read().split()[0], "nucleos": os.cpu_count()}
for l in open("/proc/meminfo"):
    if l.startswith(("MemAvailable", "MemTotal")):
        st[l.split(":")[0]] = round(int(l.split()[1]) / 1048576, 1)
st["compiladores"] = int(subprocess.run(["pgrep", "-c", "-f", "cc1plus"], capture_output=True, text=True).stdout.strip() or 0)
jobs = []
for d in sorted(glob.glob(R + "/*/"), key=os.path.getmtime, reverse=True)[:8]:
    rd = lambda f: open(d + f).read().strip() if os.path.exists(d + f) else ""
    t0 = int(rd("t0") or 0); t1 = int(rd("t1") or 0); rc = rd("rc")
    try:
        with open(d + "log", "rb") as fh:
            fh.seek(max(0, os.path.getsize(d + "log") - 200000)); tail = fh.read().decode(errors="replace")
    except OSError:
        tail = ""
    lines = [l for l in tail.splitlines() if l.strip()]
    prog = re.findall(r"\[(\d+)/(\d+)\]", tail)
    errs = [l for l in lines if re.search(r"\berror\b|Error |FAILED|fatal|Killed|No space", l)]
    vivo = not rc and t0 and os.path.exists(d + "pid") and subprocess.run(["kill", "-0", rd("pid")]).returncode == 0
    jobs.append(dict(nombre=os.path.basename(d.rstrip("/")), cmd=rd("cmd"), t0=t0, t1=t1, rc=rc, vivo=bool(vivo),
                     n=int(prog[-1][0]) if prog else None, total=int(prog[-1][1]) if prog else None,
                     ultimas=lines[-4:], errores=errs[-3:], nerr=len(errs), quieto=int(time.time() - os.path.getmtime(d + "log")) if os.path.exists(d + "log") else 0))
st["trabajos"] = jobs
print(json.dumps(st))
PY
    ;;
  *) sed -n 2,13p "$0"; exit 2 ;;
esac
