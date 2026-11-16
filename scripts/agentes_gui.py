#!/usr/bin/env python3
"""Ventana con el avance de los agentes de Claude del proyecto NSMBW (versión gráfica de scripts/agentes).

    python3 ~/nsmbw-port/scripts/agentes_gui.py &

Se actualiza sola cada 10 s. Incluye la otra laptop (scripts/remoto.sh estado, cada 15 s). Usa la misma lógica que scripts/agentes: los registros de cada agente y las fichas de
status/agentes_tareas.json (el % es la parte de los archivos entregables que ya existen, no una estimación de tiempo).
"""
import glob
import importlib.machinery
import json
import os
import subprocess
import sys
import time
from datetime import datetime

from PySide6 import QtCore, QtGui, QtWidgets

HERE = os.path.dirname(os.path.abspath(__file__))
ag = importlib.machinery.SourceFileLoader("agentes_mod", os.path.join(HERE, "agentes")).load_module()

COLORES = {"🟢": "#2e9d4f", "⏳": "#c98a00", "✅": "#3a6fd8", "⛔": "#9a9a9a"}


def datos():
    tareas = json.load(open(ag.TAREAS)) if os.path.exists(ag.TAREAS) else {}
    ahora = time.time()
    archivos = glob.glob(f"{ag.PROJ}/*/subagents/agent-*.jsonl") + \
        glob.glob(f"{ag.PROJ}-nsmbw-port/*/subagents/agent-*.jsonl")
    agentes = [ag.leer_agente(p) for p in archivos]
    agentes = [a for a in agentes if ahora - a["t1"] < 24 * 3600]
    # Primero los que trabajan, luego los que esperan, luego los terminados.
    def estado(a):
        quieto = ahora - a["t1"]
        if quieto < ag.ACTIVO_MIN * 60 and not a["entregado"]:
            return "🟢", "Trabajando"
        if a["entregado"]:
            return "✅", "Terminó (entregó su informe)"
        if quieto < 3 * 3600:
            return "⏳", f"Esperando su compilación o prueba ({ag.dur(quieto)} sin escribir)"
        return "⛔", f"Detenido (sin actividad hace {ag.dur(quieto)})"
    orden = {"🟢": 0, "⏳": 1, "✅": 2, "⛔": 3}
    filas = []
    for a in agentes:
        icono, texto = estado(a)
        desc = a["meta"].get("description", "(sin descripción)")
        info = tareas.get(desc, {})
        p, det = ag.progreso(info.get("entregables"))
        filas.append(dict(icono=icono, estado=texto, titulo=info.get("titulo", desc), info=info, pct=p, det=det,
                          t0=a["t0"], t1=a["t1"], pasos=a["pasos"]))
    filas.sort(key=lambda f: (orden[f["icono"]], -f["t1"]))
    return filas


def cabecera():
    partes = [datetime.now().strftime("%H:%M:%S")]
    try:
        perfil = subprocess.run(["powerprofilesctl", "get"], capture_output=True, text=True).stdout.strip()
        partes.append(f"energía: {perfil}")
    except OSError:
        pass
    try:
        partes.append("carga CPU: " + open("/proc/loadavg").read().split()[0])
        for line in open("/proc/meminfo"):
            if line.startswith("MemAvailable"):
                partes.append(f"RAM libre: {int(line.split()[1]) / 1048576:.1f} GB")
    except OSError:
        pass
    return "   ·   ".join(partes)


import re

NINJA_RX = re.compile(rb"\[(\d+)/(\d+)\]")


def compilaciones():
    """Builds in progress: every *.log under r9/ written in the last 10 min that holds ninja progress
    lines ([n/total]); the last such line gives the progress. Plus how many compiler processes run."""
    ahora = time.time()
    res = []
    base = os.path.expanduser("~/nsmbw-port")
    for p in glob.glob(f"{base}/r9/**/*.log", recursive=True) + glob.glob(f"{base}/recomp/*.log"):
        try:
            mt = os.path.getmtime(p)
            if ahora - mt > 600:
                continue
            with open(p, "rb") as fh:
                fh.seek(max(0, os.path.getsize(p) - 65536))
                tail = fh.read()
        except OSError:
            continue
        m = list(NINJA_RX.finditer(tail))
        if not m:
            continue
        n, total = int(m[-1].group(1)), int(m[-1].group(2))
        if total <= 0:
            continue
        nombre = os.path.relpath(p, base)
        fin_txt = tail[-400:]
        # Ninja's last step (linking / packing the .nro) often prints no [total/total] line: the build
        # scripts end with "exit=0" / "rc=0" or list the produced binaries.
        terminado = (n >= total or b"exit=0" in fin_txt or b"rc=0" in fin_txt or b".nro\n" in fin_txt
                     or b"/NSMBW" in fin_txt.splitlines()[-1] if fin_txt.strip() else False)
        if terminado:
            n = total
        errs = [l for l in tail.decode(errors="replace").splitlines() if re.search(r"\berror\b|FAILED|fatal|Killed", l)]
        res.append(dict(nombre=nombre, n=n, total=total, pct=100 * n // total, quieto=ahora - mt, fin=terminado,
                        errs=errs[-2:], nerr=len(errs)))
    res.sort(key=lambda r: r["quieto"])
    try:
        out = subprocess.run(["pgrep", "-c", "-f", "cc1plus"], capture_output=True, text=True).stdout.strip()
        procesos = int(out or 0)
    except (OSError, ValueError):
        procesos = 0
    return res, procesos


class CajaCompilaciones(QtWidgets.QGroupBox):
    def __init__(self, builds, procesos):
        super().__init__("Compilaciones en ESTA laptop (últimos 10 min)")
        lay = QtWidgets.QVBoxLayout(self)
        lay.addWidget(QtWidgets.QLabel(f"Procesos del compilador trabajando ahora: <b>{procesos}</b>"))
        if not builds:
            lay.addWidget(QtWidgets.QLabel("<span style='color:#777'>Ninguna compilación activa.</span>"))
        for b in builds:
            fila = QtWidgets.QHBoxLayout()
            et = QtWidgets.QLabel(b["nombre"])
            et.setMinimumWidth(260)
            fila.addWidget(et)
            bar = QtWidgets.QProgressBar()
            bar.setRange(0, 100)
            bar.setValue(b["pct"])
            estado = "terminada" if b["fin"] else f"{b['n']} de {b['total']} archivos"
            bar.setFormat(f"{b['pct']} %  ·  {estado}")
            color = "#3a6fd8" if b["fin"] else "#2e9d4f"
            bar.setStyleSheet(f"QProgressBar::chunk {{ background: {color}; }}")
            fila.addWidget(bar, 1)
            lay.addLayout(fila)
            if b.get("nerr"):
                el = QtWidgets.QLabel(f"<span style='color:#c0392b'>{b['nerr']} líneas de error: " +
                                      " | ".join(x[:150].replace("<", "&lt;") for x in b["errs"]) + "</span>")
                el.setWordWrap(True)
                lay.addWidget(el)


COLA = os.path.expanduser("~/nsmbw-port/tools/switchctl")


def cola_consola():
    """Console benchmark queue (tools/switchctl/cola.sh): the job running now, the pending ones, the last results."""
    d = os.path.join(COLA, ".cola")
    try:
        nombres = sorted(os.listdir(d))
    except OSError:
        nombres = []
    def nombre(f):
        return f.split("_", 1)[1].split(".job")[0] if "_" in f else f
    corriendo = [nombre(f) for f in nombres if f.endswith(".job.running")]
    pendientes = [nombre(f) for f in nombres if f.endswith(".job")]
    try:
        hechos = open(os.path.join(COLA, "cola_hecho.txt")).read().splitlines()[-4:]
    except OSError:
        hechos = []
    try:
        activo = subprocess.run(["systemctl", "--user", "is-active", "nsmbw-cola"], capture_output=True,
                                text=True).stdout.strip() == "active"
    except OSError:
        activo = False
    return corriendo, pendientes, hechos, activo


class CajaConsola(QtWidgets.QGroupBox):
    def __init__(self):
        super().__init__("Switch: cola de mediciones")
        lay = QtWidgets.QVBoxLayout(self)
        corriendo, pendientes, hechos, activo = cola_consola()
        estado = "<span style='color:#2e9d4f'>● procesando</span>" if activo else "<span style='color:#9a9a9a'>● parada</span>"
        lay.addWidget(QtWidgets.QLabel(f"Cola {estado}   ·   ahora: <b>{', '.join(corriendo) or '—'}</b>"))
        if pendientes:
            el = QtWidgets.QLabel("En espera: " + ", ".join(pendientes))
            el.setWordWrap(True)
            lay.addWidget(el)
        for h in hechos:
            el = QtWidgets.QLabel(f"<span style='color:#666;font-family:monospace'>{h[11:150].replace('<', '&lt;')}</span>")
            el.setWordWrap(True)
            lay.addWidget(el)


SERVICIOS_FIJOS = {"nsmbw-gui", "nsmbw-vigia", "nsmbw-keepalive", "nsmbw-cola"}


def tareas_largas():
    """Long jobs started as user services (systemd-run --unit=nsmbw-*): chains of builds/checks, PGO, etc.
    For each: what it runs, how long, and the last line of the newest *.log/*.out in its working dir."""
    try:
        out = subprocess.run(["systemctl", "--user", "list-units", "nsmbw-*", "--state=active", "--no-legend",
                              "--plain"], capture_output=True, text=True).stdout
    except OSError:
        return []
    res = []
    for linea in out.splitlines():
        unidad = linea.split()[0].removesuffix(".service")
        if unidad in SERVICIOS_FIJOS:
            continue
        props = dict(l.split("=", 1) for l in subprocess.run(
            ["systemctl", "--user", "show", unidad, "-p", "Description,ActiveEnterTimestampMonotonic,WorkingDirectory"],
            capture_output=True, text=True).stdout.splitlines() if "=" in l)
        cmd = props.get("Description", "").replace("[systemd-run] ", "").replace(os.path.expanduser("~/nsmbw-port/"), "")
        try:
            desde = time.monotonic() - int(props.get("ActiveEnterTimestampMonotonic", "0")) / 1e6
        except ValueError:
            desde = 0
        ult = ""; errs = []; logs = []
        wd = props.get("WorkingDirectory", "")
        if wd and os.path.isdir(wd):
            logs = [x for pat in ("*.log", "*/*.log", "*/*/*.log", "*.out", "*/*.out") for x in glob.glob(os.path.join(wd, pat))]
            if logs:
                lg = max(logs, key=os.path.getmtime)
                try:
                    with open(lg, "rb") as fh:
                        fh.seek(max(0, os.path.getsize(lg) - 4096))
                        lineas = [l for l in fh.read().decode(errors="replace").splitlines() if l.strip()]
                    ini = max([i for i, l in enumerate(lineas) if l.startswith("=== inicio")] or [0])
                    errs = [l for l in lineas[ini:][-15:] if re.search(r"\berror\b|FAILED|FALLÓ|fatal|Killed", l)]
                    ult = os.path.relpath(lg, wd) + ":<br>" + "<br>".join(l[:160].replace("<", "&lt;") for l in lineas[-3:])
                    if errs:
                        ult += "<br><span style='color:#c0392b'>error: " + errs[-1][:160].replace("<", "&lt;") + "</span>"
                except OSError:
                    pass
        res.append(dict(unidad=unidad, cmd=cmd, desde=desde, ult=ult, err=bool(errs) if wd and os.path.isdir(wd) and logs else False))
    return res


class CajaTareas(QtWidgets.QGroupBox):
    def __init__(self):
        super().__init__("Tareas largas en marcha en ESTA laptop (Acer): cadenas de compilación y pruebas")
        lay = QtWidgets.QVBoxLayout(self)
        ts = tareas_largas()
        if not ts:
            lay.addWidget(QtWidgets.QLabel("<span style='color:#777'>Ninguna.</span>"))
        for t in ts:
            el = QtWidgets.QLabel(f"<span style='color:#2e9d4f'>●</span> <b>{t['unidad']}</b> · {ag.dur(t['desde'])}"
                                  f"<br><span style='color:#555'>{t['cmd'].replace('<', '&lt;')}</span>"
                                  f"<br><span style='color:#666;font-family:monospace'>{t['ult']}</span>")
            el.setWordWrap(True)
            lay.addWidget(el)


import threading

REMOTO = {"datos": None, "error": "consultando…", "t": 0}


def _consultar_remoto():
    while True:
        try:
            r = subprocess.run([os.path.join(HERE, "remoto.sh"), "estado"], capture_output=True, text=True, timeout=20)
            if r.returncode == 0 and r.stdout.strip():
                REMOTO.update(datos=json.loads(r.stdout), error="", t=time.time())
            else:
                REMOTO.update(error=(r.stderr.strip().splitlines() or ["sin respuesta"])[-1][:160])
        except Exception as e:  # noqa: BLE001
            REMOTO.update(error=str(e)[:160])
        # local copies (remoto.sh push/pull) are read directly in CajaRemota
        time.sleep(15)


def _esc(x):
    return str(x).replace("&", "&amp;").replace("<", "&lt;")


class CajaRemota(QtWidgets.QGroupBox):
    """Second laptop (ssh nsmbw-build): its load/RAM, the jobs started with scripts/remoto.sh run, and the copies."""
    def __init__(self):
        super().__init__("OTRA laptop (Omen, nsmbw-build <IP_SWITCH>): compilaciones remotas")
        lay = QtWidgets.QVBoxLayout(self)
        d = REMOTO["datos"]
        if REMOTO["error"] or not d:
            lay.addWidget(QtWidgets.QLabel(f"<span style='color:#c0392b'>● sin conexión:</span> {_esc(REMOTO['error'])}"))
        if d:
            hace = int(time.time() - REMOTO["t"])
            lay.addWidget(QtWidgets.QLabel(
                f"<span style='color:#2e9d4f'>●</span> {d['host']} · carga {d['carga']} de {d['nucleos']} hilos · "
                f"RAM libre {d.get('MemAvailable', '?')} de {d.get('MemTotal', '?')} GB · "
                f"compiladores trabajando: <b>{d['compiladores']}</b> · datos de hace {hace} s"))
            if not d["trabajos"]:
                lay.addWidget(QtWidgets.QLabel("<span style='color:#777'>Ningún trabajo remoto todavía.</span>"))
            for j in d["trabajos"]:
                if j["vivo"]:
                    col, est = "#2e9d4f", f"corriendo {ag.dur(time.time() - j['t0'])} (última salida hace {ag.dur(j['quieto'])})"
                elif j["rc"] == "0":
                    col, est = "#3a6fd8", f"terminó bien en {ag.dur(j['t1'] - j['t0'])}"
                elif j["rc"] == "143":
                    col, est = "#7f8c8d", f"detenido a propósito tras {ag.dur(j['t1'] - j['t0']) if j['t1'] else '?'}"
                elif j["rc"]:
                    col, est = "#c0392b", f"FALLÓ (código {j['rc']}) tras {ag.dur(j['t1'] - j['t0'])}"
                else:
                    col, est = "#9a9a9a", "cortado (sin código de salida)"
                caja = QtWidgets.QFrame()
                caja.setStyleSheet(f"QFrame {{ border-left: 5px solid {col}; }}")
                cl = QtWidgets.QVBoxLayout(caja)
                cl.addWidget(QtWidgets.QLabel(f"<b>{_esc(j['nombre'])}</b> · <span style='color:{col}'>{est}</span>"
                                              f"<br><span style='color:#555'>{_esc(j['cmd'][:200])}</span>"))
                if j["total"]:
                    bar = QtWidgets.QProgressBar()
                    bar.setRange(0, j["total"]); bar.setValue(j["n"])
                    bar.setFormat(f"{100 * j['n'] // j['total']} %  ·  {j['n']} de {j['total']} pasos")
                    bar.setStyleSheet(f"QProgressBar::chunk {{ background: {col}; }}")
                    cl.addWidget(bar)
                if j["nerr"]:
                    el = QtWidgets.QLabel(f"<span style='color:#c0392b'><b>{j['nerr']} líneas de error</b>, últimas:<br>"
                                          + "<br>".join(_esc(x[:180]) for x in j["errores"]) + "</span>")
                    el.setWordWrap(True); cl.addWidget(el)
                el = QtWidgets.QLabel("<span style='color:#666;font-family:monospace'>" +
                                      "<br>".join(_esc(x[:180]) for x in j["ultimas"]) + "</span>")
                el.setWordWrap(True); cl.addWidget(el)
                lay.addWidget(caja)
        # copies in progress or recent (local logs of remoto.sh push/pull)
        copias = sorted(glob.glob(os.path.expanduser("~/nsmbw-port/remoto/copias/*.log")), key=os.path.getmtime)[-2:]
        for c in copias:
            try:
                ls = [l for l in open(c, errors="replace").read().splitlines() if l.strip()]
            except OSError:
                continue
            fin = ls and ls[-1].startswith("rc=")
            prog = [l for l in ls if "%" in l and "xfr" in l or "to-chk" in l]
            ult = (prog[-1] if prog else ls[-1] if ls else "").strip()
            lay.addWidget(QtWidgets.QLabel(
                f"{'✔' if fin else '⇅'} copia <b>{_esc(ls[0] if ls else os.path.basename(c))}</b> · "
                f"{'terminada' if fin else 'en curso'} · <span style='font-family:monospace;color:#555'>{_esc(ult[:120])}</span>"))


AVISADOS = set()


def avisar(clave, titulo, texto):
    """Desktop notification once per problem (notify-send, critical urgency so it stays on screen)."""
    if clave in AVISADOS:
        return
    AVISADOS.add(clave)
    try:
        subprocess.Popen(["notify-send", "-u", "critical", "-a", "NSMBW", titulo, texto[:300]])
    except OSError:
        pass


def revisar_alertas():
    d = REMOTO["datos"]
    if d:
        for j in d["trabajos"]:
            if j["rc"] and j["rc"] not in ("0", "143") and time.time() - (j["t1"] or 0) < 3600:
                avisar(("r", j["nombre"], j["t0"]), f"❌ Otra laptop: {j['nombre']} FALLÓ (código {j['rc']})",
                       "\n".join(j["errores"][-2:] or j["ultimas"][-2:]))
            elif j["vivo"] and j["nerr"]:
                avisar(("re", j["nombre"], j["t0"], j["errores"][-1]), f"⚠ Otra laptop: errores en {j['nombre']}", j["errores"][-1])
            elif j["rc"] == "0" and time.time() - (j["t1"] or 0) < 3600:
                avisar(("ok", j["nombre"], j["t0"]), f"✅ Otra laptop: {j['nombre']} terminó bien", j["cmd"][:200])
    for t in tareas_largas():
        if t.get("err"):
            avisar(("t", t["unidad"], t["ult"][-200:]), f"❌ Esta laptop: error en {t['unidad']}", re.sub("<[^>]+>", " ", t["ult"])[-250:])
    try:
        for h in open(os.path.join(COLA, "cola_hecho.txt")).read().splitlines()[-3:]:
            if "(rc 0)" not in h:
                avisar(("c", h), "❌ Switch: un trabajo de la cola falló", h[:250])
    except OSError:
        pass


class Tarjeta(QtWidgets.QFrame):
    def __init__(self, f):
        super().__init__()
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        color = COLORES.get(f["icono"], "#888")
        self.setStyleSheet(f"Tarjeta {{ border-left: 6px solid {color}; border-radius: 6px; }}")
        lay = QtWidgets.QVBoxLayout(self)
        lay.setSpacing(4)
        titulo = QtWidgets.QLabel(f"<b style='font-size:15px'>{f['titulo']}</b>")
        titulo.setWordWrap(True)
        lay.addWidget(titulo)
        tiempo = ag.dur((f["t1"] - f["t0"]) if f["t0"] else 0)
        est = QtWidgets.QLabel(f"<span style='color:{color}'>● {f['estado']}</span>   ·   {tiempo}   ·   "
                               f"{len(f['pasos'])} pasos")
        lay.addWidget(est)
        info = f["info"]
        if info:
            txt = QtWidgets.QLabel(f"<b>Para qué:</b> {info['para_que']}<br><b>Busca:</b> {info['busca']}")
            txt.setWordWrap(True)
            lay.addWidget(txt)
        if f["pct"] is not None:
            bar = QtWidgets.QProgressBar()
            bar.setRange(0, 100)
            bar.setValue(f["pct"])
            bar.setFormat(f"{f['pct']} % de entregables")
            bar.setStyleSheet(f"QProgressBar::chunk {{ background: {color}; }}")
            lay.addWidget(bar)
            ent = "  ".join(("✔ " if ok else "○ ") + os.path.basename(e.rstrip("/*")) for e, ok in f["det"])
            el = QtWidgets.QLabel(f"<span style='color:#777'>{ent}</span>")
            el.setWordWrap(True)
            lay.addWidget(el)
        if f["pasos"]:
            ult = []
            for t, texto in f["pasos"][-3:]:
                hora = datetime.fromtimestamp(ag.ts(t)).strftime("%H:%M") if t else "?"
                ult.append(f"{hora}  {texto[:110]}")
            pl = QtWidgets.QLabel("<span style='color:#666;font-family:monospace'>" +
                                  "<br>".join(x.replace("<", "&lt;") for x in ult) + "</span>")
            pl.setWordWrap(True)
            lay.addWidget(pl)


class Ventana(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Agentes NSMBW")
        self.resize(760, 900)
        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        v = QtWidgets.QVBoxLayout(central)
        self.cab = QtWidgets.QLabel()
        self.cab.setStyleSheet("font-size:13px; padding:4px")
        v.addWidget(self.cab)
        self.ver_terminados = QtWidgets.QCheckBox("Mostrar también los terminados y detenidos")
        self.ver_terminados.setChecked(False)
        self.ver_terminados.stateChanged.connect(self.refrescar)
        v.addWidget(self.ver_terminados)
        self.scroll = QtWidgets.QScrollArea()
        self.scroll.setWidgetResizable(True)
        v.addWidget(self.scroll)
        self.refrescar()
        t = QtCore.QTimer(self)
        t.timeout.connect(self.refrescar)
        t.start(10000)

    def refrescar(self):
        filas = datos()
        n = {k: sum(f["icono"] == k for f in filas) for k in COLORES}
        self.cab.setText(f"<b>{cabecera()}</b><br>trabajando {n['🟢']} · esperando {n['⏳']} · "
                         f"terminados {n['✅']} · detenidos {n['⛔']}   (se actualiza cada 10 s)")
        cont = QtWidgets.QWidget()
        lay = QtWidgets.QVBoxLayout(cont)
        lay.addWidget(CajaConsola())
        lay.addWidget(CajaTareas())
        lay.addWidget(CajaRemota())
        revisar_alertas()
        builds, procesos = compilaciones()
        lay.addWidget(CajaCompilaciones(builds, procesos))
        ver_todo = self.ver_terminados.isChecked()
        for f in filas:
            if ver_todo or f["icono"] in ("🟢", "⏳"):
                lay.addWidget(Tarjeta(f))
        lay.addStretch(1)
        pos = self.scroll.verticalScrollBar().value()
        self.scroll.setWidget(cont)
        self.scroll.verticalScrollBar().setValue(pos)


if __name__ == "__main__":
    app = QtWidgets.QApplication(sys.argv)
    threading.Thread(target=_consultar_remoto, daemon=True).start()
    w = Ventana()
    w.show()
    sys.exit(app.exec())
