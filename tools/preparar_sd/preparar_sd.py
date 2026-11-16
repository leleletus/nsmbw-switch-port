#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
preparar_sd.py: prepara la carpeta de datos del port NSMBWnx (New Super Mario
Bros. Wii para Nintendo Switch) a partir de TU PROPIA copia del juego.

    python preparar_sd.py <juego.iso | juego.wbfs | carpeta_extraída> [--salida DIR] [--sd LETRA_O_RUTA]
    python preparar_sd.py --autotest

Resultado (lo que el NRO busca en la SD, junto a sí mismo):

    switch/NSMBWnx/NSMBWnx.nro        el port (no lo genera esta herramienta)
    switch/NSMBWnx/data/sys/          main.dol, boot.bin, bi2.bin, fst.bin, apploader.img
    switch/NSMBWnx/data/files/        los archivos del disco (los que lista fst.bin)
    switch/NSMBWnx/config.toml        ajustes (solo si no existe; el NRO también lo crea)
    switch/NSMBWnx/save/              partidas guardadas (solo si no existe save/ ni nand/)

Solo usa la biblioteca estándar de Python 3 (3.7 o más nuevo). No contiene
claves ni datos del juego: para leer una imagen .iso/.wbfs usa una herramienta
externa (wit o DolphinTool) si está instalada; con una carpeta ya extraída no
necesita nada más.
"""

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# ---------------------------------------------------------------- constantes

ID_JUEGO = "SMNE01"
REVISION = 1
# SHA-256 del main.dol de NSMBW EE. UU. (SMNE01) revisión 1: el código nativo
# del port se generó de este archivo exacto y el NRO lo comprueba al arrancar
# (switch/runtime/CMakeLists.txt -> NWII_NX_EXPECTED_DOL_SHA256).
SHA256_MAIN_DOL = "42014a070cdecdcfb9a0cde45b3a4f07eed6036b1fb09ec940567f8953c6b5b0"
TAM_MAIN_DOL = 3491808

CARPETA_SD = ("switch", "NSMBWnx")  # sdmc:/switch/NSMBWnx/
NOMBRE_NRO = "NSMBWnx.nro"
# Archivos de sys/ que lee el port (loader.cpp y virtual_disc.cpp).
ARCHIVOS_SYS = ("main.dol", "boot.bin", "bi2.bin", "fst.bin", "apploader.img")
ARCHIVOS_SYS_OBLIGATORIOS = ("main.dol", "boot.bin", "fst.bin")

REGIONES = {
    "SMNE01": "EE. UU.",
    "SMNP01": "Europa",
    "SMNJ01": "Japón",
    "SMNK01": "Corea",
    "SMNW01": "Taiwán",
}

# Igual que kDefaultToml de switch_platform.cpp (lo que el NRO escribe si falta).
CONFIG_TOML = """\
# NSMBWnx: configuración (si borras este archivo, se vuelve a crear así)
#
# Carpeta del juego (todo junto al .nro):
#   data/   tu disco NSMBW USA (SMNE01) extraído: data/sys y data/files
#   save/   partidas guardadas (cópiala para tener respaldo)
#   cache/  gráficos ya preparados (se puede borrar)
#   logs/   registros para reportar fallos

[system]
language = "auto"        # auto = idioma de la consola, o "es" / "en" / "fr"

[input]
players = 4               # cuántos jugadores pueden conectarse (1-4)
joycon_mode = "auto"      # auto = J1 con par de Joy-Con y J2-4 con uno suelto;
                          # "single" = cada Joy-Con un jugador; "pair" = L+R juntos
nx_controller_applet = true  # abrir el applet de mandos en "Añadir control"
mode = 6                  # no tocar

[switch]
motion = true             # inclinar/sacudir girando el mando (false = solo botones)
tilt_invert = false       # true = inclinación al revés

[graphics]
# Resolución interna: "auto" = "720p" en portátil y "1080p" en el dock (la imagen del
# juego sale 1:1 en la pantalla). 1 = la de la Wii (640x528, lo más ligero); también vale
# cualquier número entre 1 y 4 ("1.3636", "1.5", 2...) o "720p"/"1080p". Más alto = más
# nítido y más trabajo de GPU. Un valor aquí vale para los dos modos.
internal_resolution = "auto"
# Solo en modo portátil (quítale el # para usarla): por ejemplo 1 si la GPU no llega.
#internal_resolution_handheld = 1
backend = "opengl"        # no tocar
window_width = 1280       # no tocar
window_height = 720       # no tocar
vsync = true              # no tocar

# ---- no tocar ----
[game]
id = "SMNE01"

[hle]
sched_lock_offset = 0x4F80   # necesario: sin esto el juego se cuelga
"""

ES_WINDOWS = os.name == "nt"
BLOQUE = 1 << 20


class ErrorUsuario(Exception):
    """Error con un mensaje para el usuario (se muestra sin traza)."""


# ---------------------------------------------------------------- utilidades

def L(p):
    """Ruta como str; en Windows con el prefijo \\\\?\\ para pasar de 260 caracteres."""
    s = str(Path(p).absolute())
    if ES_WINDOWS and not s.startswith("\\\\?\\"):
        s = os.path.normpath(s)
        if s.startswith("\\\\"):
            s = "\\\\?\\UNC\\" + s[2:]
        else:
            s = "\\\\?\\" + s
    return s


def mb(n):
    return ("%.1f MB" % (n / 1e6)).replace(".", ",")


def existe(p):
    return os.path.exists(L(p))


def es_archivo(p):
    return os.path.isfile(L(p))


def es_carpeta(p):
    return os.path.isdir(L(p))


def tamano(p):
    return os.stat(L(p)).st_size


def leer(p, n=-1):
    with open(L(p), "rb") as f:
        return f.read(n)


def crear_carpeta(p):
    os.makedirs(L(p), exist_ok=True)


def sha256_de(p, progreso=None):
    h = hashlib.sha256()
    with open(L(p), "rb") as f:
        while True:
            b = f.read(BLOQUE)
            if not b:
                break
            h.update(b)
            if progreso:
                progreso(len(b))
    return h.hexdigest()


class Progreso:
    def __init__(self, total, titulo, silencioso=False):
        self.total = max(total, 1)
        self.hecho = 0
        self.titulo = titulo
        self.silencioso = silencioso
        self.t = 0.0
        self.nombre = ""

    def avanza(self, n):
        self.hecho += n
        ahora = time.monotonic()
        if ahora - self.t >= 0.2:
            self.t = ahora
            self.dibuja()

    def dibuja(self):
        if self.silencioso:
            return
        pc = min(100, self.hecho * 100 // self.total)
        nombre = self.nombre if len(self.nombre) <= 40 else "…" + self.nombre[-39:]
        linea = "\r  %s [%3d %%] %s de %s  %s" % (self.titulo, pc, mb(self.hecho), mb(self.total), nombre)
        sys.stdout.write(linea.ljust(100)[:110])
        sys.stdout.flush()

    def fin(self):
        if not self.silencioso:
            self.hecho = self.total
            self.nombre = ""
            self.dibuja()
            sys.stdout.write("\n")
            sys.stdout.flush()


# ---------------------------------------------------------------- disco

def leer_fst(datos):
    """Lista (ruta relativa en files/, tamaño) de un fst.bin de Wii; las carpetas van con tamaño None."""
    if len(datos) < 12 or datos[0] != 1:
        raise ErrorUsuario("fst.bin no es válido (la extracción parece incompleta o dañada).")
    total = struct.unpack(">I", datos[8:12])[0]
    tabla = total * 12
    if total < 1 or tabla > len(datos):
        raise ErrorUsuario("fst.bin no es válido (tamaño incoherente).")

    def nombre(off):
        fin = datos.index(b"\0", tabla + off)
        return datos[tabla + off:fin].decode("shift_jis", errors="replace")

    salida = []
    pila = []  # (índice donde termina la carpeta, ruta)
    for i in range(1, total):
        while pila and pila[-1][0] <= i:
            pila.pop()
        e = datos[i * 12:i * 12 + 12]
        es_dir = e[0] == 1
        n = nombre(int.from_bytes(e[1:4], "big"))
        base = pila[-1][1] if pila else ""
        ruta = base + "/" + n if base else n
        if es_dir:
            pila.append((struct.unpack(">I", e[8:12])[0], ruta))
            salida.append((ruta, None))
        else:
            salida.append((ruta, struct.unpack(">I", e[8:12])[0]))
    return salida


def id_de_imagen(img):
    """(ID6, revisión) leídos de la cabecera sin cifrar de una .iso o .wbfs; None si no se sabe."""
    try:
        with open(L(img), "rb") as f:
            cab = f.read(16)
            if cab[:4] == b"WBFS":
                sector = 1 << cab[8]
                f.seek(12)
                tabla = f.read(sector - 12) if sector > 12 else b""
                ranura = next((i for i, b in enumerate(tabla) if b), 0)
                f.seek((ranura + 1) * sector)
                cab = f.read(16)
            if len(cab) >= 8 and cab[:6].isalnum():
                return cab[:6].decode("ascii"), cab[7]
    except (OSError, ValueError, IndexError):
        pass
    return None


def describe_id(id6):
    if id6 in REGIONES:
        return "New Super Mario Bros. Wii de %s (%s)" % (REGIONES[id6], id6)
    return "otro juego (ID %s)" % id6


def comprobar_juego(carpeta, sha_esperado=None):
    """Valida que carpeta/sys y carpeta/files sean NSMBW SMNE01 rev 1 completo.
    Devuelve la lista de (ruta relativa, tamaño) que hay que copiar."""
    sha_esperado = sha_esperado or SHA256_MAIN_DOL
    sys_dir = carpeta / "sys"
    for n in ARCHIVOS_SYS_OBLIGATORIOS:
        if not es_archivo(sys_dir / n):
            raise ErrorUsuario("Falta %s en %s.\nLa extracción está incompleta: vuelve a extraer el disco." % (n, sys_dir))
    if not es_carpeta(carpeta / "files"):
        raise ErrorUsuario("Falta la carpeta files/ en %s.\nLa extracción está incompleta." % carpeta)

    boot = leer(sys_dir / "boot.bin", 8)
    id6 = boot[:6].decode("ascii", errors="replace")
    rev = boot[7] if len(boot) >= 8 else -1
    print("  Disco: %s, revisión %d" % (describe_id(id6), rev))
    if id6 != ID_JUEGO:
        raise ErrorUsuario(
            "Este disco es %s.\n"
            "El port solo funciona con New Super Mario Bros. Wii de EE. UU. (%s), revisión %d:\n"
            "su código se generó de ese main.dol exacto y con otra versión fallaría." % (describe_id(id6), ID_JUEGO, REVISION))

    print("  Comprobando main.dol (SHA-256)…")
    sha = sha256_de(sys_dir / "main.dol")
    if sha != sha_esperado:
        raise ErrorUsuario(
            "El main.dol no es el de %s revisión %d (tu disco es la revisión %d).\n"
            "  SHA-256 encontrado: %s\n  SHA-256 esperado:   %s\n"
            "El port se generó de ese archivo exacto; con otro el juego fallaría.\n"
            "Si el disco es el correcto, la extracción está dañada: vuelve a extraerlo."
            % (ID_JUEGO, REVISION, rev, sha, sha_esperado))

    lista = []
    for n in ARCHIVOS_SYS:
        if es_archivo(sys_dir / n):
            lista.append(("sys/" + n, tamano(sys_dir / n)))
    faltan, distintos = [], []
    for ruta, tam in leer_fst(leer(sys_dir / "fst.bin")):
        if tam is None:  # carpeta (algunas están vacías en el disco: se crean igual)
            lista.append(("files/" + ruta, None))
            continue
        p = carpeta / "files" / ruta
        if not es_archivo(p):
            faltan.append(ruta)
        elif tamano(p) != tam:
            distintos.append("%s (%d bytes, deberían ser %d)" % (ruta, tamano(p), tam))
        else:
            lista.append(("files/" + ruta, tam))
    if faltan or distintos:
        txt = ["La copia del juego está incompleta o dañada (comparada con su fst.bin):"]
        txt += ["  falta: " + r for r in faltan[:10]]
        txt += ["  tamaño distinto: " + r for r in distintos[:10]]
        if len(faltan) + len(distintos) > 20:
            txt.append("  … y %d más" % (len(faltan) + len(distintos) - 20))
        txt.append("Vuelve a extraer el disco entero.")
        raise ErrorUsuario("\n".join(txt))
    return lista


def buscar_juego(carpeta, profundidad=3):
    """Busca la carpeta con sys/main.dol y files/ (la propia, o dentro de DATA/, P-…/, etc.)."""
    carpeta = Path(carpeta)
    if carpeta.name.lower() == "sys" and es_archivo(carpeta / "main.dol"):
        carpeta = carpeta.parent
    candidatas = []
    nivel = [carpeta]
    for _ in range(profundidad + 1):
        siguiente = []
        for c in nivel:
            if es_archivo(c / "sys" / "main.dol") and es_carpeta(c / "files"):
                candidatas.append(c)
                continue
            try:
                with os.scandir(L(c)) as it:
                    siguiente += [c / e.name for e in it if e.is_dir() and e.name not in ("files", "sys")]
            except OSError:
                pass
        if candidatas:
            break
        nivel = siguiente
    if not candidatas:
        return None
    for c in candidatas:  # con varias particiones, la del juego (boot.bin con el ID buscado)
        if leer(c / "sys" / "boot.bin", 6) == ID_JUEGO.encode():
            return c
    return candidatas[0]


# ---------------------------------------------------------------- motores para imágenes

def _candidatos_windows(rel):
    bases = [os.environ.get(v) for v in ("ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "LOCALAPPDATA")]
    bases = [Path(b) for b in bases if b]
    bases += [Path(os.environ["LOCALAPPDATA"]) / "Programs"] if os.environ.get("LOCALAPPDATA") else []
    casa = Path.home()
    bases += [casa / "Desktop", casa / "Escritorio", casa / "Downloads", casa / "Descargas", Path("C:/")]
    return [b / r for b in bases for r in rel]


def buscar_wit():
    for n in ("wit", "wit.exe"):
        w = shutil.which(n)
        if w:
            return Path(w)
    cand = [Path(__file__).resolve().parent / ("wit.exe" if ES_WINDOWS else "wit")]
    if ES_WINDOWS:
        cand += _candidatos_windows(["Wiimm/WIT/wit.exe", "Wiimm/wit/wit.exe", "WIT/wit.exe", "wit/wit.exe",
                                     "Wiimms ISO Tools/wit.exe", "wit/bin/wit.exe"])
    else:
        cand += [Path(p) for p in ("/usr/local/bin/wit", "/opt/homebrew/bin/wit", "/usr/bin/wit",
                                   str(Path.home() / "bin/wit"), str(Path.home() / ".local/bin/wit"))]
    return next((c for c in cand if es_archivo(c)), None)


def buscar_dolphin_tool():
    """Devuelve la orden (lista) para ejecutar DolphinTool, o None."""
    for n in ("dolphin-tool", "DolphinTool", "DolphinTool.exe", "dolphin-emu-tool"):
        w = shutil.which(n)
        if w:
            return [w]
    cand = [Path(__file__).resolve().parent / ("DolphinTool.exe" if ES_WINDOWS else "dolphin-tool")]
    if ES_WINDOWS:
        cand += _candidatos_windows(["Dolphin/DolphinTool.exe", "Dolphin-x64/DolphinTool.exe",
                                     "Dolphin Emulator/DolphinTool.exe", "Dolphin/Dolphin-x64/DolphinTool.exe"])
    elif sys.platform == "darwin":
        cand += [Path("/Applications/Dolphin.app/Contents/MacOS/dolphin-tool"),
                 Path("/Applications/Dolphin.app/Contents/MacOS/DolphinTool"),
                 Path.home() / "Applications/Dolphin.app/Contents/MacOS/dolphin-tool"]
    c = next((c for c in cand if es_archivo(c)), None)
    if c:
        return [str(c)]
    if not ES_WINDOWS and shutil.which("flatpak"):
        r = subprocess.run(["flatpak", "info", "org.DolphinEmu.dolphin-emu"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode == 0:
            return ["flatpak", "run", "--filesystem=host", "--command=dolphin-tool", "org.DolphinEmu.dolphin-emu"]
    return None


AYUDA_SIN_MOTOR = """
No encontré ninguna herramienta para leer imágenes de disco de Wii
(ni «wit» de Wiimms ISO Tools ni «DolphinTool» de Dolphin).

Haz esto (solo una vez) con Dolphin:
  1. Abre Dolphin y añade la carpeta donde está tu juego (si no aparece en la lista).
  2. Clic derecho en «New Super Mario Bros. Wii» → Propiedades → pestaña «Sistema de archivos».
  3. Clic derecho en el disco (la primera línea) → «Extraer disco entero…»
     (o en «Partición de datos» → «Extraer partición completa…»).
  4. Elige una carpeta vacía, por ejemplo  C:\\NSMBW_extraido
  5. Vuelve a ejecutar esta herramienta pasándole esa carpeta:
       python preparar_sd.py "C:\\NSMBW_extraido"

Otra opción: instala Wiimms ISO Tools (https://wit.wiimm.de) o una versión reciente
de Dolphin (trae DolphinTool.exe junto a Dolphin.exe) y vuelve a ejecutar con la imagen.
También puedes indicar dónde está con  --herramienta "RUTA\\wit.exe"  o  "RUTA\\DolphinTool.exe".
"""


def extraer_imagen(img, destino, herramienta=None):
    """Extrae la partición de datos de img en destino con wit o DolphinTool."""
    motores = []
    if herramienta:
        h = Path(herramienta)
        if not es_archivo(h):
            raise ErrorUsuario("No existe la herramienta indicada: %s" % h)
        motores.append(("dolphin" if "dolphin" in h.name.lower() else "wit", [str(h)]))
    else:
        w = buscar_wit()
        if w:
            motores.append(("wit", [str(w)]))
        d = buscar_dolphin_tool()
        if d:
            motores.append(("dolphin", d))
    if not motores:
        raise ErrorUsuario(AYUDA_SIN_MOTOR.strip("\n"))

    for tipo, orden in motores:
        if tipo == "wit":
            intentos = [orden + ["extract", str(img), "--dest", str(destino / "wit"),
                                 "--psel", "DATA", "--overwrite", "--progress"]]
        else:
            intentos = [orden + ["extract", "-i", str(img), "-o", str(destino / "dolphin"), "-p", "DATA"],
                        orden + ["extract", "-i", str(img), "-o", str(destino / "dolphin2")]]
        for o in intentos:
            print("  Extrayendo con %s (puede tardar unos minutos)…" % ("wit" if tipo == "wit" else "DolphinTool"))
            print("    " + " ".join('"%s"' % a if " " in a else a for a in o))
            sys.stdout.flush()
            try:
                r = subprocess.run(o)
            except OSError as e:
                print("    No se pudo ejecutar: %s" % e)
                break
            encontrado = buscar_juego(destino)
            if r.returncode == 0 and encontrado:
                return encontrado
            print("    La extracción no funcionó (código %d)." % r.returncode)
    raise ErrorUsuario("No pude extraer la imagen con las herramientas disponibles.\n" + AYUDA_SIN_MOTOR.strip("\n"))


# ---------------------------------------------------------------- copia

def mismo_archivo(src, dst, tam, comprobar):
    try:
        sd, ss = os.stat(L(dst)), os.stat(L(src))
    except OSError:
        return False
    if sd.st_size != tam:
        return False
    if comprobar:
        return sha256_de(src) == sha256_de(dst)
    return abs(sd.st_mtime - ss.st_mtime) <= 2.0  # FAT guarda la hora con 2 s de precisión


def copiar_archivo(src, dst, prog):
    crear_carpeta(dst.parent)
    tmp = dst.with_name(dst.name + ".parcial")
    h = hashlib.sha256()
    with open(L(src), "rb") as fi, open(L(tmp), "wb") as fo:
        while True:
            b = fi.read(BLOQUE)
            if not b:
                break
            h.update(b)
            fo.write(b)
            prog.avanza(len(b))
        fo.flush()
        os.fsync(fo.fileno())
    os.replace(L(tmp), L(dst))
    st = os.stat(L(src))
    os.utime(L(dst), (st.st_atime, st.st_mtime))
    return h.hexdigest()


def espacio_libre(p):
    p = Path(p).absolute()
    while not existe(p) and p.parent != p:
        p = p.parent
    try:
        return shutil.disk_usage(L(p)).free
    except OSError:
        return None


def preparar(origen_juego, raiz_nsmbwnx, lista, comprobar=False, nro=None, silencioso=False):
    """Copia el juego a raiz_nsmbwnx/data y crea lo que falte. Devuelve un resumen (dict)."""
    data = raiz_nsmbwnx / "data"
    carpetas = [r for r, t in lista if t is None]
    lista = [(r, t) for r, t in lista if t is not None]
    pendientes, ya = [], 0
    for rel, tam in lista:
        if mismo_archivo(origen_juego / rel, data / rel, tam, comprobar):
            ya += 1
        else:
            pendientes.append((rel, tam))
    total = sum(t for _, t in pendientes)
    nro_copiar = False
    if nro:
        total_nro = tamano(nro)
        if not mismo_archivo(nro, raiz_nsmbwnx / NOMBRE_NRO, total_nro, comprobar):
            nro_copiar = True
            total += total_nro
    libre = espacio_libre(raiz_nsmbwnx)
    if libre is not None and total > libre:
        raise ErrorUsuario("No hay espacio: hacen falta %s y quedan %s libres en el destino." % (mb(total), mb(libre)))

    print("  %d archivos del juego (%s); ya estaban bien %d, se copian %d (%s)."
          % (len(lista), mb(sum(t for _, t in lista)), ya, len(pendientes), mb(total)))
    crear_carpeta(data)
    prog = Progreso(total, "Copiando", silencioso)
    errores = []
    copiados = []
    for rel, tam in pendientes:
        prog.nombre = rel
        sha = copiar_archivo(origen_juego / rel, data / rel, prog)
        copiados.append((rel, tam, sha))
    if nro and nro_copiar:
        prog.nombre = NOMBRE_NRO
        sha_nro = copiar_archivo(nro, raiz_nsmbwnx / NOMBRE_NRO, prog)
    prog.fin()
    if nro and nro_copiar and sha256_de(raiz_nsmbwnx / NOMBRE_NRO) != sha_nro:
        raise ErrorUsuario("El NRO copiado no coincide con el original (¿tarjeta llena o dañada?).")

    # Verificación: tamaño de todo y SHA-256 de lo copiado (releído del destino).
    vprog = Progreso(sum(t for _, t, _ in copiados), "Verificando", silencioso)
    for rel, tam, sha in copiados:
        vprog.nombre = rel
        d = data / rel
        if tamano(d) != tam or sha256_de(d, vprog.avanza) != sha:
            errores.append(rel)
    vprog.fin()
    for rel, tam in lista:
        d = data / rel
        if not es_archivo(d) or tamano(d) != tam:
            if rel not in errores:
                errores.append(rel)
    for rel in carpetas:
        crear_carpeta(data / rel)
    if errores:
        raise ErrorUsuario("Tras copiar, no coinciden %d archivos (¿tarjeta llena o dañada?):\n  %s"
                           % (len(errores), "\n  ".join(errores[:10])))

    creado = []
    # config.toml: nunca se pisa; si hay un nsmbw.toml antiguo, el NRO usa ese.
    if not existe(raiz_nsmbwnx / "config.toml") and not existe(raiz_nsmbwnx / "nsmbw.toml"):
        with open(L(raiz_nsmbwnx / "config.toml"), "w", encoding="utf-8", newline="\n") as f:
            f.write(CONFIG_TOML)
        creado.append("config.toml")
    # save/: si ya hay save/ o nand/ (partidas antiguas) no se toca nada.
    if not existe(raiz_nsmbwnx / "save") and not existe(raiz_nsmbwnx / "nand"):
        crear_carpeta(raiz_nsmbwnx / "save")
        creado.append("save/")
    return {"total": len(lista), "ya": ya, "copiados": len(copiados), "bytes": total, "creado": creado}


# ---------------------------------------------------------------- programa

def raiz_sd(valor):
    v = valor.strip().strip('"')
    if ES_WINDOWS and len(v) <= 2 and v[:1].isalpha() and v[1:] in ("", ":"):
        return Path(v[0].upper() + ":\\")
    return Path(v)


def buscar_nro(arg):
    if arg:
        p = Path(arg)
        if not es_archivo(p):
            raise ErrorUsuario("No existe el NRO indicado: %s" % p)
        return p
    for c in (Path(__file__).resolve().parent / NOMBRE_NRO, Path.cwd() / NOMBRE_NRO):
        if es_archivo(c):
            return c
    return None


def principal(argv=None):
    ap = argparse.ArgumentParser(
        prog="preparar_sd.py",
        description="Prepara la carpeta switch/NSMBWnx/ de la SD a partir de tu copia de "
                    "New Super Mario Bros. Wii (EE. UU., SMNE01, revisión 1).")
    ap.add_argument("juego", nargs="?", help="imagen .iso / .wbfs (u otra que entienda wit o DolphinTool) "
                                             "o carpeta extraída con Dolphin/wit")
    ap.add_argument("--salida", help="carpeta donde dejar el resultado (por defecto: NSMBWnx_SD en la carpeta actual)")
    ap.add_argument("--sd", help="copiar directamente a la SD (letra como E: o ruta de la tarjeta montada)")
    ap.add_argument("--nro", help="ruta de NSMBWnx.nro para copiarlo también (si no, se busca junto a esta herramienta)")
    ap.add_argument("--herramienta", help="ruta de wit(.exe) o DolphinTool(.exe) si no se encuentra sola")
    ap.add_argument("--comprobar", action="store_true",
                    help="comparar por SHA-256 también los archivos que ya estaban (más lento)")
    ap.add_argument("--autotest", action="store_true", help="ejecutar las pruebas internas y salir")
    a = ap.parse_args(argv)

    if a.autotest:
        return autotest()
    if not a.juego:
        ap.print_help()
        return 2
    if a.salida and a.sd:
        raise ErrorUsuario("Usa --salida o --sd, no los dos a la vez.")

    entrada = Path(a.juego.strip().strip('"')).expanduser()
    if not existe(entrada):
        raise ErrorUsuario("No existe: %s" % entrada)

    if a.sd:
        sd = raiz_sd(a.sd)
        if not es_carpeta(sd):
            raise ErrorUsuario("No encuentro la tarjeta SD en %s. ¿Está conectada? Usa la letra (E:) o la ruta." % sd)
        base = sd
    else:
        base = Path(a.salida).expanduser() if a.salida else Path.cwd() / "NSMBWnx_SD"
    raiz = base.joinpath(*CARPETA_SD)
    nro = buscar_nro(a.nro)

    print("preparar_sd: New Super Mario Bros. Wii (%s, revisión %d) → %s" % (ID_JUEGO, REVISION, raiz))
    temporal = None
    try:
        if es_carpeta(entrada):
            print("1/3 Buscando el juego extraído en %s" % entrada)
            juego = buscar_juego(entrada)
            if not juego:
                raise ErrorUsuario(
                    "No encuentro sys/main.dol y files/ dentro de %s.\n"
                    "Pasa la carpeta donde extrajiste el disco con Dolphin («Extraer disco entero») "
                    "o con wit; debe contener sys/ y files/ (directamente o en una subcarpeta como DATA/)." % entrada)
        else:
            print("1/3 Leyendo la imagen %s" % entrada)
            info = id_de_imagen(entrada)
            if info:
                print("  Cabecera: %s, revisión %d" % (describe_id(info[0]), info[1]))
                if info[0] != ID_JUEGO:
                    raise ErrorUsuario("Esta imagen es %s.\nEl port solo funciona con New Super Mario Bros. Wii "
                                       "de EE. UU. (%s), revisión %d." % (describe_id(info[0]), ID_JUEGO, REVISION))
            temporal = Path(tempfile.mkdtemp(prefix="preparar_sd_"))
            juego = extraer_imagen(entrada, temporal, a.herramienta)
        print("  Juego encontrado en %s" % juego)

        print("2/3 Comprobando que es el juego correcto y que está completo")
        lista = comprobar_juego(juego)
        print("  Correcto: %s revisión %d, %d archivos." % (ID_JUEGO, REVISION, sum(1 for _, t in lista if t is not None)))

        print("3/3 Copiando a %s" % raiz)
        crear_carpeta(raiz)
        r = preparar(juego, raiz, lista, a.comprobar, nro)
    finally:
        if temporal:
            shutil.rmtree(L(temporal), ignore_errors=True)

    print()
    print("Listo. %d archivos del juego verificados (%d copiados ahora)." % (r["total"], r["copiados"]))
    if r["creado"]:
        print("Creado: " + ", ".join(r["creado"]))
    if existe(raiz / "SMNE01"):
        print("Aviso: hay una carpeta antigua %s; el port usa ahora data/ y puedes borrarla tú." % (raiz / "SMNE01"))
    if a.sd and existe(sd / "nsmbw" / "nand") and not existe(raiz / "nand"):
        print("Aviso: tienes partidas de una versión anterior en %s.\n"
              "  Si quieres seguir con ellas, copia el CONTENIDO de esa carpeta dentro de %s antes de jugar."
              % (sd / "nsmbw" / "nand", raiz / "save"))
    tiene_nro = existe(raiz / NOMBRE_NRO)
    print()
    if a.sd:
        print("La SD ya está lista en %s" % raiz)
    else:
        print("Copia el CONTENIDO de esta carpeta a la raíz de tu tarjeta SD (se fusiona con la carpeta switch/ que ya tengas):")
        print("    %s" % base)
        print("  En la SD debe quedar  switch/NSMBWnx/data/sys/main.dol")
    if not tiene_nro:
        print("Falta el port: copia %s dentro de switch/NSMBWnx/ en la SD (o usa --nro RUTA)." % NOMBRE_NRO)
    print("En la Switch: abre el Homebrew Menu manteniendo R al abrir un juego (no desde el Álbum) y elige NSMBWnx.")
    return 0


# ---------------------------------------------------------------- pruebas

def _fst_falso(arbol):
    """Construye un fst.bin a partir de {carpeta: {...}, archivo: bytes}."""
    entradas, nombres = [], bytearray()

    def nombre(n):
        off = len(nombres)
        nombres.extend(n.encode("shift_jis", errors="replace") + b"\0")
        return off

    def visita(nodo, padre):
        for n in sorted(nodo):
            v = nodo[n]
            if isinstance(v, dict):
                i = len(entradas)
                entradas.append([1, nombre(n), padre, 0])
                visita(v, i)
                entradas[i][3] = len(entradas)
            else:
                entradas.append([0, nombre(n), 0, len(v)])

    entradas.append([1, 0, 0, 0])
    nombre("")
    visita(arbol, 0)
    entradas[0][3] = len(entradas)
    out = bytearray()
    for t, no, a, b in entradas:
        out += bytes([t]) + no.to_bytes(3, "big") + struct.pack(">II", a, b)
    return bytes(out + nombres)


def _escribe_arbol(base, arbol):
    crear_carpeta(base)
    for n, v in arbol.items():
        if isinstance(v, dict):
            crear_carpeta(base / n)
            _escribe_arbol(base / n, v)
        else:
            with open(L(base / n), "wb") as f:
                f.write(v)


def autotest():
    global SHA256_MAIN_DOL
    fallos = []

    def caso(nombre, cond):
        print("  [%s] %s" % ("ok" if cond else "FALLO", nombre))
        if not cond:
            fallos.append(nombre)

    def espera_error(nombre, fn, texto):
        try:
            fn()
        except ErrorUsuario as e:
            caso(nombre, texto in str(e))
            return
        caso(nombre + " (no dio error)", False)

    archivos = {
        "Stage": {"01-01.arc": b"nivel" * 100, "Texture": {"t.arc": b"x" * 3000}},
        "Sound": {"stream": {"musica nandu 1.brstm": b"m" * 777}},
        "rels": {"d_profileNP.rel.LZ": b"r" * 50},
        "Layout": {"vacia": {}},
        "opening.bnr": b"b" * 20,
    }
    dol = b"DOL falso para pruebas" * 64
    with tempfile.TemporaryDirectory(prefix="preparar_sd_test_") as t:
        t = Path(t)
        print("Autotest en %s" % t)
        # Imagen extraída como la deja Dolphin («Extraer disco entero»): DATA/sys y DATA/files.
        juego = t / "extraído con espacios" / "DATA"
        _escribe_arbol(juego, {
            "sys": {"main.dol": dol, "boot.bin": b"SMNE01\x00\x01" + bytes(0x438), "bi2.bin": bytes(0x2000),
                    "apploader.img": b"a" * 100, "fst.bin": _fst_falso(archivos)},
            "files": dict(archivos, **{"extra_que_no_esta_en_fst.txt": b"no se copia"}),
        })
        viejo = SHA256_MAIN_DOL
        SHA256_MAIN_DOL = hashlib.sha256(dol).hexdigest()
        try:
            caso("leer_fst", sorted(r for r, x in leer_fst(_fst_falso(archivos)) if x is not None) ==
                 ["Sound/stream/musica nandu 1.brstm", "Stage/01-01.arc", "Stage/Texture/t.arc",
                  "opening.bnr", "rels/d_profileNP.rel.LZ"])
            caso("buscar_juego encuentra DATA/", buscar_juego(t / "extraído con espacios") == juego)
            caso("buscar_juego acepta la carpeta sys", buscar_juego(juego / "sys") == juego)

            # 1) salida a carpeta
            salida = t / "salida ñ"
            caso("preparación a --salida", principal([str(t / "extraído con espacios"), "--salida", str(salida)]) == 0)
            raiz = salida / "switch" / "NSMBWnx"
            caso("data/sys/main.dol copiado", leer(raiz / "data/sys/main.dol") == dol)
            caso("archivo con espacios copiado (rutas con acentos)", es_archivo(raiz / "data/files/Sound/stream/musica nandu 1.brstm"))
            caso("no copia lo que no está en fst.bin", not existe(raiz / "data/files/extra_que_no_esta_en_fst.txt"))
            caso("config.toml creado", "sched_lock_offset" in leer(raiz / "config.toml").decode())
            caso("save/ creado", es_carpeta(raiz / "save"))
            caso("carpeta vacía del fst.bin creada", es_carpeta(raiz / "data/files/Layout/vacia"))

            # 2) idempotente: segunda vez no copia nada
            lista = comprobar_juego(juego, SHA256_MAIN_DOL)
            r = preparar(juego, raiz, lista, silencioso=True)
            caso("segunda vez no copia nada", r["copiados"] == 0 and r["ya"] == len([1 for _, x in lista if x is not None]))
            # archivo dañado con el mismo tamaño: --comprobar lo repara
            with open(L(raiz / "data/files/Stage/01-01.arc"), "r+b") as f:
                f.write(b"X")
            r = preparar(juego, raiz, lista, comprobar=True, silencioso=True)
            caso("--comprobar repara un archivo dañado", r["copiados"] == 1 and
                 leer(raiz / "data/files/Stage/01-01.arc") == archivos["Stage"]["01-01.arc"])

            # 3) --sd con partidas y config del usuario: no se tocan
            sd = t / "SD"
            nsm = sd / "switch" / "NSMBWnx"
            _escribe_arbol(nsm, {"config.toml": b"# mio", "save": {"partida.sav": b"PARTIDA"}})
            nro = t / NOMBRE_NRO
            _escribe_arbol(t, {NOMBRE_NRO: b"NRO0" * 10})
            caso("--sd con --nro", principal([str(juego), "--sd", str(sd), "--nro", str(nro)]) == 0)
            caso("config.toml del usuario intacto", leer(nsm / "config.toml") == b"# mio")
            caso("partida intacta", leer(nsm / "save/partida.sav") == b"PARTIDA")
            caso("NRO copiado", leer(nsm / NOMBRE_NRO) == b"NRO0" * 10)
            # instalación antigua con nsmbw.toml y nand/: no se crea config.toml ni save/
            sd2 = t / "SD2"
            _escribe_arbol(sd2 / "switch" / "NSMBWnx", {"nsmbw.toml": b"#", "nand": {}})
            principal([str(juego), "--sd", str(sd2)])
            caso("con nsmbw.toml/nand antiguos no crea config.toml ni save/",
                 not existe(sd2 / "switch/NSMBWnx/config.toml") and not existe(sd2 / "switch/NSMBWnx/save"))

            # 4) errores claros
            otro = t / "europa"
            shutil.copytree(L(juego), L(otro))
            with open(L(otro / "sys/boot.bin"), "r+b") as f:
                f.write(b"SMNP01")
            espera_error("rechaza la versión europea", lambda: comprobar_juego(otro, SHA256_MAIN_DOL), "Europa")
            espera_error("rechaza un main.dol distinto", lambda: comprobar_juego(juego, "0" * 64), "SHA-256")
            roto = t / "roto"
            shutil.copytree(L(juego), L(roto))
            os.remove(L(roto / "files/rels/d_profileNP.rel.LZ"))
            espera_error("detecta un archivo que falta", lambda: comprobar_juego(roto, SHA256_MAIN_DOL),
                         "d_profileNP.rel.LZ")
            crear_carpeta(t / "vacía")
            espera_error("carpeta sin juego", lambda: principal([str(t / "vacía"), "--salida", str(t / "x")]), "No encuentro")

            # 5) cabeceras de imagen (sin datos del juego)
            iso = t / "falso.iso"
            _escribe_arbol(t, {"falso.iso": b"SMNE01\x00\x01" + bytes(100)})
            caso("ID de una .iso", id_de_imagen(iso) == ("SMNE01", 1))
            wbfs = bytearray(b"WBFS" + bytes(4) + bytes([9, 21]) + bytes(2) + b"\x01") + bytes(512)
            wbfs[512:520] = b"SMNP01\x00\x00"
            _escribe_arbol(t, {"falso.wbfs": bytes(wbfs)})
            caso("ID de una .wbfs", id_de_imagen(t / "falso.wbfs") == ("SMNP01", 0))
            espera_error("rechaza una imagen de otra región sin extraer", lambda: principal([str(t / "falso.wbfs"), "--salida", str(t / "x")]),
                         "Europa")
        finally:
            SHA256_MAIN_DOL = viejo
    print("Autotest: %s" % ("todo bien" if not fallos else "%d fallos" % len(fallos)))
    return 0 if not fallos else 1


def main():
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    try:
        return principal()
    except ErrorUsuario as e:
        print("\nERROR: %s" % e, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nCancelado. Puedes volver a ejecutarlo: seguirá donde lo dejó.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
