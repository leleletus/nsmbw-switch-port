#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
construir_nro.py: construye NSMBWnx.nro (New Super Mario Bros. Wii para Nintendo Switch, port por
recompilación estática) y la carpeta de datos de la SD a partir de TU PROPIA copia del juego.

    python3 construir_nro.py --juego <carpeta|.iso|.wbfs> --trabajo <dir> [--jobs N] [--sin-pgo] [--docker]

Etapas (cada una se salta si ya está hecha y sus entradas no cambiaron; registro en <trabajo>/registros/):
  1 juego         valida el juego (preparar_sd.py), extrae la imagen si hace falta y descomprime los REL
  2 herramientas  copia/clona el recompilador NWiiRecomp, aplica los cambios del port y compila nwiirecomp + jtdump
  3 recompilar    DOL + REL -> C++ (export/), con las pasadas base (fastdispatch, R8) y la tabla de saltos
  4 pasadas       pasadas del port sobre el código generado (cpucode, fastmem, A2, G6, G6c, G7, S2) + A43 + agua W;
                  cada paso se compara con el SHA-256 de referencia (los mismos fuentes que el binario m6q)
  5 uamlib        compila uamlib (compilador de shaders de deko3d, devkitPro/uam + parches del port)
  6 vista         árbol de fuentes para compilar (export + runtime) con «#line» a las rutas originales (PGO)
  7 compilar      CMake + ninja con devkitA64/libnx; enlace con el agua nativa y (con PGO) pares + orden L
  8 sd            NSMBWnx.nro + carpeta de datos de la SD (preparar_sd.py)

Nada del juego viene con esta herramienta: todo lo que sale del juego se genera en <trabajo>.
Ver LEEME.md.
"""

import argparse
import hashlib
import json
import lzma
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path

AQUI = Path(__file__).resolve().parent
PASADAS = AQUI / "pasadas"
DATOS = AQUI / "material"
REFERENCIA = AQUI / "referencia"
COMPILACION = AQUI / "compilacion"
TERCEROS = AQUI / "terceros"
PERFILES = AQUI / "perfiles"

VERSION_HERRAMIENTA = "1"

# Recompilador de terceros (ver recompilador/UPSTREAM.md del repositorio)
NWIIRECOMP_URL = "https://github.com/BlackLineInteractive/NWiiRecomp.git"
NWIIRECOMP_COMMIT = "595f176f1d24cc54ff2e8389feed12d7fb553cc2"
SIGDB_URL = "https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Data/Sys/totaldb.dsy"
SIGDB_SHA256 = "09fb4a167946dbdfee5890c0f027e40b3eacbb6cc5ee9184adeb9d1a81c2c0aa"
TOMLPP_URL = "https://github.com/marzer/tomlplusplus.git"
TOMLPP_TAG = "v3.4.0"
UAM_URL = "https://github.com/devkitPro/uam.git"
UAM_COMMIT = "5a5afc2bae8b55409ab36ba45be63fcb73f68993"
IMAGEN_DOCKER_BASE = "devkitpro/devkita64"
IMAGEN_DOCKER = "construir_nro_nsmbw:1"

# Rutas con las que se compiló el código generado de m5/m6q y con las que se tomaron los perfiles de PGO
# (r9/r17pgo/m4src). GCC mete la ruta del archivo en la suma de control de cada función del perfil, así que la
# «vista» pone «#line 1 "<ruta original>"» al principio de cada fuente y cabecera (el código no cambia).
RUTA_PGO = "/home/djvemo/nsmbw-port/r9/r17pgo/m4src"
GCC_PGO = "16.1.0"   # versión de aarch64-none-elf-gcc con la que se tomaron los perfiles (.gcda)
KFLAGS = "-fno-jump-tables -freorder-blocks-and-partition"   # frente K (r42k): código frío aparte

# SHA-256 de lo que tiene que salir (los mismos fuentes que m6q; comprobado el 16-nov-2026)
SHA_OUTPUT24_A43 = "17cf37b394df14ce25d67e09a69014056ec585239eab8dddaba5ecdcd9288540"
SHA_W_AGUA_GEN = "3d0e59abe3b245ea420c46b3328c2365ad342a1af74e5e588735361dc7767c11"
CABECERA_W_AGUA = ("// [r44w] GENERADO por r9/r44w/tools/gen_w_agua.py a partir de "
                   "/home/djvemo/nsmbw-port/r9/r35m4/export (NO editar a mano)\n")

RELS = ["d_profileNP.rel", "d_basesNP.rel", "d_enemiesNP.rel", "d_en_bossNP.rel"]

ETAPAS = ["juego", "herramientas", "recompilar", "pasadas", "uamlib", "vista", "compilar", "sd"]

# Pasadas sobre el código generado, en orden. (nombre, referencia, función que la aplica)
#   cada una trabaja sobre una copia del paso anterior y se compara con referencia/<ref>.sha256


class Fallo(Exception):
    pass


# ------------------------------------------------------------------------------------------- utilidades

def sha256_archivo(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def sha256_texto(t):
    return hashlib.sha256(t.encode("utf-8")).hexdigest()


def huella_arbol(raiz, patron=None, excluir=()):
    """SHA-256 de un árbol de archivos (rutas relativas + contenidos), para claves de etapa."""
    raiz = Path(raiz)
    h = hashlib.sha256()
    for dp, dns, fns in os.walk(raiz):
        dns[:] = sorted(d for d in dns if d not in excluir and not d.startswith("."))
        for fn in sorted(fns):
            p = Path(dp) / fn
            rel = p.relative_to(raiz).as_posix()
            if patron and not re.search(patron, rel):
                continue
            h.update(rel.encode() + b"\0" + sha256_archivo(p).encode() + b"\n")
    return h.hexdigest()


def manifiesto(carpeta):
    """{archivo: sha256} de los archivos de primer nivel (como referencia/*.sha256)."""
    carpeta = Path(carpeta)
    return {p.name: sha256_archivo(p) for p in sorted(carpeta.iterdir()) if p.is_file()}


def leer_referencia(nombre):
    ref = {}
    for ln in (REFERENCIA / (nombre + ".sha256")).read_text().splitlines():
        if ln.strip():
            sha, f = ln.split(None, 1)
            ref[f.strip()] = sha
    return ref


def comparar_manifiesto(carpeta, nombre, ignorar=()):
    m = manifiesto(carpeta)
    ref = leer_referencia(nombre)
    malos = []
    for f in sorted(set(m) | set(ref)):
        if f in ignorar:
            continue
        if m.get(f) != ref.get(f):
            malos.append("%s (%s vs referencia %s)" % (f, (m.get(f) or "falta")[:12], (ref.get(f) or "no esperado")[:12]))
    return malos


def copiar_arbol(src, dst, ignorar=None):
    dst = Path(dst)
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst, ignore=ignorar, symlinks=False)


def mb(n):
    return "%.1f MB" % (n / 1e6)


def tam_arbol(p):
    t = 0
    for dp, _, fns in os.walk(p):
        for fn in fns:
            try:
                t += os.path.getsize(os.path.join(dp, fn))
            except OSError:
                pass
    return t


def hora():
    return time.strftime("%H:%M:%S")


def buscar_primero(*cands):
    for c in cands:
        if c and Path(c).exists():
            return Path(c)
    return None


# ------------------------------------------------------------------------------------------- etapas y estado

class Obra:
    """Estado de una construcción en <trabajo>."""

    def __init__(self, a):
        self.a = a
        self.t = Path(a.trabajo).resolve()
        self.reg = self.t / "registros"
        self.est = self.t / "estado"
        for d in (self.t, self.reg, self.est):
            d.mkdir(parents=True, exist_ok=True)
        self.log = None
        self.etapa = None
        self.tiempos = {}

    # -- registro
    def abrir(self, etapa):
        self.etapa = etapa
        self.log = open(self.reg / ("%s.log" % etapa), "a", encoding="utf-8")
        self.escribe("==== %s %s (construir_nro %s) ====" % (time.strftime("%F %T"), etapa, VERSION_HERRAMIENTA))

    def cerrar(self):
        if self.log:
            self.log.close()
        self.log = None

    def escribe(self, txt):
        if self.log:
            self.log.write(txt + "\n")
            self.log.flush()

    def dice(self, txt):
        print("  " + txt)
        sys.stdout.flush()
        self.escribe(txt)

    def corre(self, orden, cwd=None, env=None, shell=False, nombre=None):
        """Ejecuta una orden con su salida al registro de la etapa; si falla, enseña el final del registro."""
        txt = orden if isinstance(orden, str) else " ".join(shlex.quote(str(x)) for x in orden)
        self.escribe("$ " + txt + ("   (en %s)" % cwd if cwd else ""))
        e = os.environ.copy()
        if env:
            e.update(env)
        t0 = time.time()
        if isinstance(orden, str) and not shell:
            raise Fallo("orden de texto sin shell: " + orden[:80])
        r = subprocess.run(orden, cwd=cwd, env=e, shell=shell, stdout=self.log, stderr=subprocess.STDOUT)
        self.escribe("# código %d, %.0f s" % (r.returncode, time.time() - t0))
        if r.returncode != 0:
            self.log.flush()
            cola = (self.reg / ("%s.log" % self.etapa)).read_text(encoding="utf-8", errors="replace").splitlines()[-25:]
            raise Fallo("falló %s (código %d). Últimas líneas de %s:\n    %s" % (
                nombre or txt[:120], r.returncode, self.reg / ("%s.log" % self.etapa), "\n    ".join(cola)))
        return r

    def salida_de(self, orden, cwd=None, env=None):
        e = os.environ.copy()
        if env:
            e.update(env)
        return subprocess.run(orden, cwd=cwd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True).stdout

    # -- estado
    def hecha(self, etapa, clave):
        p = self.est / (etapa + ".json")
        if not p.exists():
            return None
        try:
            d = json.loads(p.read_text())
        except ValueError:
            return None
        return d if d.get("clave") == clave else None

    def marca(self, etapa, clave, **datos):
        datos.update(clave=clave, fin=time.strftime("%F %T"), version=VERSION_HERRAMIENTA)
        (self.est / (etapa + ".json")).write_text(json.dumps(datos, indent=1, ensure_ascii=False))

    def info(self, etapa):
        p = self.est / (etapa + ".json")
        return json.loads(p.read_text()) if p.exists() else {}


# ------------------------------------------------------------------------------------------- 1 juego

def lz11_descomprimir(datos):
    """Descompresor LZ77 de Nintendo (tipos 0x10 y 0x11) para los .rel.LZ del disco."""
    tipo = datos[0]
    tam = datos[1] | (datos[2] << 8) | (datos[3] << 16)
    pos = 4
    if tam == 0:
        tam = int.from_bytes(datos[4:8], "little")
        pos = 8
    out = bytearray()
    while len(out) < tam:
        banderas = datos[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= tam:
                break
            if not (banderas & (0x80 >> bit)):
                out.append(datos[pos])
                pos += 1
                continue
            if tipo == 0x10:
                b0, b1 = datos[pos], datos[pos + 1]
                pos += 2
                n = (b0 >> 4) + 3
                disp = (((b0 & 0xF) << 8) | b1) + 1
            elif tipo == 0x11:
                b0 = datos[pos]
                ind = b0 >> 4
                if ind == 0:
                    b1, b2 = datos[pos + 1], datos[pos + 2]
                    pos += 3
                    n = (((b0 & 0xF) << 4) | (b1 >> 4)) + 0x11
                    disp = (((b1 & 0xF) << 8) | b2) + 1
                elif ind == 1:
                    b1, b2, b3 = datos[pos + 1], datos[pos + 2], datos[pos + 3]
                    pos += 4
                    n = (((b0 & 0xF) << 12) | (b1 << 4) | (b2 >> 4)) + 0x111
                    disp = (((b2 & 0xF) << 8) | b3) + 1
                else:
                    b1 = datos[pos + 1]
                    pos += 2
                    n = ind + 1
                    disp = (((b0 & 0xF) << 8) | b1) + 1
            else:
                raise Fallo("compresión LZ de tipo 0x%02X no conocida" % tipo)
            ini = len(out) - disp
            if ini < 0:
                raise Fallo("archivo LZ dañado")
            for k in range(n):
                out.append(out[ini + k])
    return bytes(out[:tam])


def cargar_preparar_sd():
    cand = [AQUI.parent / "preparar_sd" / "preparar_sd.py"]
    for c in cand:
        if c.exists():
            import importlib.util
            spec = importlib.util.spec_from_file_location("preparar_sd", str(c))
            m = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(m)
            return m, c
    raise Fallo("no encuentro tools/preparar_sd/preparar_sd.py (tiene que estar junto a tools/construir_nro/)")


def etapa_juego(o):
    a = o.a
    ps, ps_ruta = cargar_preparar_sd()
    entrada = Path(a.juego).expanduser().resolve()
    if not entrada.exists():
        raise Fallo("no existe %s" % entrada)
    huella_entrada = "%s|%d|%d" % (entrada, entrada.stat().st_size if entrada.is_file() else 0,
                                   int(entrada.stat().st_mtime))
    clave = sha256_texto(huella_entrada + VERSION_HERRAMIENTA)
    d = o.hecha("juego", clave)
    if d and Path(d["juego"]).exists() and all((o.t / "entrada" / r).exists() for r in RELS):
        o.dice("ya hecha: juego en %s" % d["juego"])
        return
    o.abrir("juego")
    try:
        if entrada.is_dir():
            juego = ps.buscar_juego(entrada)
            if not juego:
                raise Fallo("en %s no hay ninguna carpeta con sys/main.dol y files/ (la extracción del disco)" % entrada)
        else:
            dest = o.t / "juego_extraido"
            if dest.exists():
                shutil.rmtree(dest)
            dest.mkdir(parents=True)
            o.dice("extrayendo la imagen %s (wit o DolphinTool)…" % entrada.name)
            juego = ps.extraer_imagen(entrada, dest, a.herramienta_imagen)
        juego = Path(juego).resolve()
        o.dice("validando %s (SMNE01 rev 1, SHA-256 de main.dol, archivos según fst.bin)…" % juego)
        try:
            lista = ps.comprobar_juego(juego)
        except ps.ErrorUsuario as e:
            raise Fallo(str(e))
        ent = o.t / "entrada"
        ent.mkdir(exist_ok=True)
        for r in RELS:
            src = juego / "files" / "rels" / (r + ".LZ")
            if not src.exists():
                raise Fallo("falta files/rels/%s.LZ en el juego" % r)
            datos = lz11_descomprimir(src.read_bytes())
            (ent / r).write_bytes(datos)
            o.escribe("%s: %d bytes, sha256 %s" % (r, len(datos), hashlib.sha256(datos).hexdigest()))
        o.marca("juego", clave, juego=str(juego), archivos=len(lista),
                main_dol_sha256=sha256_archivo(juego / "sys" / "main.dol"))
        o.dice("juego válido: %d archivos; REL descomprimidos en %s" % (len(lista), ent))
    finally:
        o.cerrar()


# ------------------------------------------------------------------------------------------- 2 herramientas

def compilador_host():
    for c in (os.environ.get("CXX"), "g++", "clang++"):
        if c and shutil.which(c):
            return c
    raise Fallo("no hay compilador de C++ para el PC (g++ o clang++): instala build-essential (o usa --docker)")


def parche_recompilador(a):
    c = buscar_primero(a.parche_recompilador,
                       AQUI.parent.parent / "recompilador" / "cambios_locales_completos.patch",
                       AQUI.parent.parent / "_repo_github" / "recompilador" / "cambios_locales_completos.patch")
    if not c:
        raise Fallo("no encuentro recompilador/cambios_locales_completos.patch (usa --parche-recompilador)")
    return c


def obtener_git(o, url, ref, dst, nombre):
    if not shutil.which("git"):
        raise Fallo("hace falta git para descargar %s (o pasa una copia local con la opción correspondiente)" % nombre)
    o.dice("descargando %s (%s @ %s)…" % (nombre, url, ref[:12]))
    if dst.exists():
        shutil.rmtree(dst)
    o.corre(["git", "clone", "--quiet", url, str(dst)], nombre="git clone " + nombre)
    o.corre(["git", "-c", "advice.detachedHead=false", "checkout", "--quiet", ref], cwd=dst, nombre="git checkout " + nombre)


def descargar(o, url, dst, sha):
    import urllib.request
    o.dice("descargando %s…" % url)
    with urllib.request.urlopen(url, timeout=120) as r:
        datos = r.read()
    if hashlib.sha256(datos).hexdigest() != sha:
        raise Fallo("%s: SHA-256 inesperado (¿cambió el archivo en origen?)" % url)
    Path(dst).parent.mkdir(parents=True, exist_ok=True)
    Path(dst).write_bytes(datos)


def aplicar_parche_git(o, arbol, parche, nombre):
    """Aplica un parche con git apply; si ya estaba aplicado (se puede deshacer), no hace nada."""
    if not shutil.which("git"):
        raise Fallo("hace falta git para aplicar %s" % parche.name)
    r = subprocess.run(["git", "apply", "--check", "-R", str(parche)], cwd=arbol,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode == 0:
        o.dice("%s: los cambios del port ya estaban aplicados" % nombre)
        return "ya"
    o.corre(["git", "apply", "--whitespace=nowarn", str(parche)], cwd=arbol, nombre="git apply " + parche.name)
    o.dice("%s: aplicado %s" % (nombre, parche.name))
    return "aplicado"


def tomlpp_include(o, a):
    if a.tomlplusplus:
        p = Path(a.tomlplusplus).resolve()
        for c in (p / "include", p):
            if (c / "toml++" / "toml.hpp").exists():
                return c, p
        raise Fallo("--tomlplusplus %s: no tiene include/toml++/toml.hpp" % p)
    dst = o.t / "deps" / "tomlplusplus-src"
    if not (dst / "include" / "toml++" / "toml.hpp").exists():
        obtener_git(o, TOMLPP_URL, TOMLPP_TAG, dst, "toml++")
    return dst / "include", dst


def etapa_herramientas(o):
    a = o.a
    parche = parche_recompilador(a)
    origen = Path(a.recompilador).resolve() if a.recompilador else None
    if origen:
        huella_origen = huella_arbol(origen, patron=r"^(nWiiAnalyzer|nWiiRecomp|nWiiRuntime/include|nWiiRuntime/src/core)/",
                                     excluir=("build",))
    else:
        huella_origen = NWIIRECOMP_COMMIT
    clave = sha256_texto("|".join([VERSION_HERRAMIENTA, huella_origen, sha256_archivo(parche),
                                   sha256_archivo(PASADAS / "jtdump.cpp"), str(a.tomlplusplus)]))
    bin_dir = o.t / "herramientas" / "bin"
    d = o.hecha("herramientas", clave)
    if d and (bin_dir / "nwiirecomp").exists() and (bin_dir / "jtdump").exists():
        o.dice("ya hecha: nwiirecomp y jtdump en %s" % bin_dir)
        return
    o.abrir("herramientas")
    try:
        src = o.t / "herramientas" / "NWiiRecomp"
        if origen:
            o.dice("copiando el recompilador de %s (sin build/ ni .git/)…" % origen)
            copiar_arbol(origen, src, ignorar=shutil.ignore_patterns("build", ".git", "nWiiStudio", "image", "*.o"))
        else:
            obtener_git(o, NWIIRECOMP_URL, NWIIRECOMP_COMMIT, src, "NWiiRecomp")
        estado_parche = aplicar_parche_git(o, src, parche, "NWiiRecomp")
        sigdb = src / "third_party" / "dolphin-sigdb" / "totaldb.dsy"
        if not sigdb.exists():
            descargar(o, SIGDB_URL, sigdb, SIGDB_SHA256)
        elif sha256_archivo(sigdb) != SIGDB_SHA256:
            raise Fallo("%s no tiene el SHA-256 esperado (%s)" % (sigdb, SIGDB_SHA256))
        inc, _ = tomlpp_include(o, a)
        cxx = compilador_host()
        bin_dir.mkdir(parents=True, exist_ok=True)
        comunes = [cxx, "-std=c++20", "-O1", "-w", "-I" + str(src / "nWiiRecomp" / "include"),
                   "-I" + str(src / "nWiiAnalyzer" / "include"), "-I" + str(src / "nWiiRuntime" / "include"),
                   "-I" + str(inc)]
        analiz = sorted(str(p) for p in (src / "nWiiAnalyzer" / "src").glob("*.cpp"))
        recomp = [str(src / "nWiiRecomp" / "src" / f) for f in ("recompiler.cpp", "symbols.cpp")]
        loader = [str(src / "nWiiRuntime" / "src" / "core" / "loader.cpp")]
        o.dice("compilando nwiirecomp (solo recompilador + analizador + cargador; ~20 s)…")
        o.corre(comunes + [str(src / "nWiiRecomp" / "src" / "main.cpp")] + recomp + analiz + loader +
                ["-o", str(bin_dir / "nwiirecomp")], nombre="compilar nwiirecomp")
        o.dice("compilando jtdump (tabla de saltos para G6)…")
        o.corre(comunes + [str(PASADAS / "jtdump.cpp")] + recomp + analiz + loader + ["-o", str(bin_dir / "jtdump")],
                nombre="compilar jtdump")
        o.marca("herramientas", clave, recompilador=str(src), parche=str(parche), parche_estado=estado_parche,
                sigdb=str(sigdb))
    finally:
        o.cerrar()


# ------------------------------------------------------------------------------------------- 3 recompilar

def etapa_recompilar(o):
    juego = Path(o.info("juego")["juego"])
    h = o.info("herramientas")
    src = Path(h["recompilador"])
    clave = sha256_texto("|".join([VERSION_HERRAMIENTA, o.info("juego")["clave"], h["clave"],
                                   sha256_archivo(DATOS / "nsmbw.toml.plantilla"),
                                   sha256_archivo(PASADAS / "apply_fastdispatch.py"),
                                   sha256_archivo(PASADAS / "apply_r8_codegen.py")]))
    gen = o.t / "gen"
    out = gen / "00_recomp"
    d = o.hecha("recompilar", clave)
    if d and out.exists() and (gen / "jt.txt").exists():
        o.dice("ya hecha: %s" % out)
        return
    o.abrir("recompilar")
    try:
        gen.mkdir(exist_ok=True)
        if out.exists():
            shutil.rmtree(out)
        nand = o.t / "nand_vacia"
        nand.mkdir(exist_ok=True)
        toml = (DATOS / "nsmbw.toml.plantilla").read_text(encoding="utf-8")
        for k, v in (("@JUEGO@", juego), ("@SALIDA@", out), ("@RUNTIME@", src / "nWiiRuntime"),
                     ("@SIGDB@", Path(h["sigdb"])), ("@NAND@", nand), ("@RELS@", o.t / "entrada")):
            toml = toml.replace(k, Path(v).as_posix())
        (gen / "nsmbw.toml").write_text(toml, encoding="utf-8")
        binr = o.t / "herramientas" / "bin"
        o.dice("recompilando DOL + 4 REL → C++ (nwiirecomp)…")
        o.corre([str(binr / "nwiirecomp"), str(gen / "nsmbw.toml")], cwd=gen, nombre="nwiirecomp")
        # el recompilador copia también su runtime y un informe de símbolos; para el port solo cuenta el código
        if (out / "nWiiRuntime").exists():
            shutil.rmtree(out / "nWiiRuntime")
        o.dice("pasadas base: fastdispatch (tabla O(1)) y R8 (switch de entrada podado + MemView)…")
        py = sys.executable
        o.corre([py, "-I", str(PASADAS / "apply_fastdispatch.py"), str(out)], nombre="apply_fastdispatch.py")
        o.corre([py, "-I", str(PASADAS / "apply_r8_codegen.py"), str(out), "--prune-only", "--memview"],
                nombre="apply_r8_codegen.py")
        malos = comparar_manifiesto(out, "00_recomp")
        o.dice("tabla de saltos (jtdump)…")
        o.corre([str(binr / "jtdump"), str(gen / "nsmbw.toml"), str(gen / "jt.txt")], cwd=gen, nombre="jtdump")
        if malos:
            o.dice("AVISO: el código generado NO es el de referencia en %d archivos: %s" % (len(malos), "; ".join(malos[:5])))
        else:
            o.dice("código generado = referencia (92 archivos, SHA-256 iguales)")
        if malos and not o.a.permitir_distinto:
            raise Fallo("la recompilación no reproduce el código de referencia. Las pasadas del port dependen de él "
                        "byte a byte. Si sabes lo que haces: --permitir-distinto (sin PGO).")
        o.marca("recompilar", clave, igual_referencia=not malos, distintos=malos[:50], tam=tam_arbol(out))
    finally:
        o.cerrar()


# ------------------------------------------------------------------------------------------- 4 pasadas

def pasos_pasadas(o):
    """Lista de (nombre_ref, función(src, dst)) de las pasadas del port, en orden."""
    py = [sys.executable, "-I"]
    gen = o.t / "gen"
    jobs = str(max(1, min(o.a.jobs, 4)))

    def p(script):
        return str(PASADAS / script)

    def cpucode(s, d):
        for f in ("apply_r9_cpucode.py", "apply_r9_axfx.py", "apply_r9_psq.py", "apply_r9_axfx_fast.py"):
            o.corre(py + [p(f), str(d)], nombre=f)

    def fastmem(s, d):
        o.corre(py + [p("apply_r12_fastmem.py"), str(d), str(DATOS / "prof_mmio_list.txt"), str(gen / "sites.tsv"),
                      "--orig", str(s)], nombre="apply_r12_fastmem.py")

    def a2(s, d):
        o.corre(py + [p("aplicar_u0.py"), str(DATOS / "a2_output_24.U0.patch"), str(d / "output_24.cpp")],
                nombre="parche A2 (output_24)")

    def g6entry(s, d):
        o.corre(py + [p("apply_g6_entry.py"), str(d), "--jt", str(gen / "jt.txt"), "--orig", str(s)],
                nombre="apply_g6_entry.py")

    def g6notrace(s, d):
        o.corre(py + [p("apply_g6_notrace.py"), str(d), "--orig", str(s)], nombre="apply_g6_notrace.py")

    def g6inline(s, d):
        o.corre(py + [p("apply_g6_inline.py"), str(d), "--orig", str(s), "--max-ppc", "32",
                      "--only", str(DATOS / "inline_callees.txt"), "--callers", str(DATOS / "inline_callers.txt")],
                nombre="apply_g6_inline.py")

    def g6c(s, d):
        o.corre(py + [p("apply_g6c.py"), str(d), "--orig", str(s), "--eager", "--entry-fns", "--exclude",
                      str(DATOS / "g5_exclude_g6b.txt"), "--inline-leaves", "--leaves-from", str(s), "--proto", "3"],
                env={"G5_JOBS": jobs}, nombre="apply_g6c.py")

    def g7(s, d):
        o.corre(py + [p("apply_g7.py"), str(d), "--feat", "stfs,psq,fp,poll,icm,disp,cr", "--jobs", jobs],
                nombre="apply_g7.py")

    def s2(s, d):
        # apply_s2.py escribe en d solo los output_N.cpp que cambia; d ya es una copia de s
        o.corre(py + [p("apply_s2.py"), str(s), str(d)], nombre="apply_s2.py")

    return [("01_cpucode", cpucode), ("02_fastmem", fastmem), ("03_a2", a2), ("04_g6entry", g6entry),
            ("05_g6notrace", g6notrace), ("06_g6inline", g6inline), ("07_g6c", g6c), ("08_g7", g7), ("09_s2", s2)]


def etapa_pasadas(o):
    a = o.a
    rec = o.info("recompilar")
    clave = sha256_texto("|".join([VERSION_HERRAMIENTA, rec["clave"], huella_arbol(PASADAS), huella_arbol(DATOS),
                                   huella_arbol(REFERENCIA), str(a.permitir_distinto)]))
    gen = o.t / "gen"
    final = gen / "export"
    d = o.hecha("pasadas", clave)
    if d and final.exists() and (gen / "wagua" / "w_agua_gen.cpp").exists():
        o.dice("ya hecha: %s" % final)
        return
    o.abrir("pasadas")
    try:
        anterior = gen / "00_recomp"
        distintos = {}
        temporales = []
        for nombre, fn in pasos_pasadas(o):
            dst = gen / nombre
            t0 = time.time()
            copiar_arbol(anterior, dst, ignorar=shutil.ignore_patterns("sdk_symbols.csv", "nWiiRuntime", "build"))
            fn(anterior, dst)
            malos = comparar_manifiesto(dst, nombre)
            estado = "= referencia" if not malos else "DISTINTO en %d archivos" % len(malos)
            o.dice("%s %-13s %4.0f s  %s" % (hora(), nombre, time.time() - t0, estado))
            if malos:
                distintos[nombre] = malos[:20]
                o.escribe("distintos: " + "; ".join(malos))
                if not a.permitir_distinto:
                    raise Fallo("la pasada %s no da los fuentes de referencia (%s). Sin ellos el binario no es el "
                                "probado; --permitir-distinto para seguir igualmente (sin PGO)." % (nombre, malos[0]))
            if anterior.name != "00_recomp" and not a.conservar_intermedios:
                temporales.append(anterior)
            anterior = dst
        for t in temporales:
            shutil.rmtree(t, ignore_errors=True)
        # export final = S2 + kernel 4 de la reverb (A43, en output_24)
        copiar_arbol(anterior, final)
        o.corre([sys.executable, "-I", str(PASADAS / "aplicar_u0.py"), str(DATOS / "a43_output_24.U0.patch"),
                 str(final / "output_24.cpp")], nombre="parche A43 (output_24)")
        sha24 = sha256_archivo(final / "output_24.cpp")
        o.dice("output_24 con el kernel 4 de la reverb (A43): %s" % ("= referencia" if sha24 == SHA_OUTPUT24_A43
                                                                    else "DISTINTO (%s)" % sha24[:12]))
        # agua nativa (W, gancho w3): se genera del export S2 (antes de A43, como en m6i/m6q)
        wag = gen / "wagua"
        wag.mkdir(exist_ok=True)
        o.corre([sys.executable, "-I", str(PASADAS / "gen_w_agua.py"), str(anterior), str(wag / "w_agua_gen.cpp")],
                nombre="gen_w_agua.py")
        txt = (wag / "w_agua_gen.cpp").read_text(encoding="utf-8")
        lineas = txt.split("\n")
        # w3 = el generador de antes de la simulación de olas (W_SIM): sin el #include de w_sim_gen.inc
        lineas[0] = CABECERA_W_AGUA.rstrip("\n")
        lineas = [ln for ln in lineas if '#include "w_sim_gen.inc"' not in ln]
        (wag / "w_agua_gen.cpp").write_text("\n".join(lineas), encoding="utf-8")
        for f in ("w_agua_cola.h", "w_agua_hook.inc"):
            shutil.copy2(DATOS / f, wag / f)
        shaw = sha256_archivo(wag / "w_agua_gen.cpp")
        o.dice("agua nativa (W): w_agua_gen.cpp %s" % ("= referencia" if shaw == SHA_W_AGUA_GEN else "DISTINTO (%s)" % shaw[:12]))
        if anterior != final and not a.conservar_intermedios and anterior.name != "00_recomp":
            shutil.rmtree(anterior, ignore_errors=True)
        igual = not distintos and sha24 == SHA_OUTPUT24_A43 and shaw == SHA_W_AGUA_GEN
        if not igual and not a.permitir_distinto:
            raise Fallo("el código final no es el de referencia")
        o.marca("pasadas", clave, igual_referencia=igual, distintos=distintos, export=str(final),
                wagua=str(wag), tam=tam_arbol(final))
    finally:
        o.cerrar()


# ------------------------------------------------------------------------------------------- devkitPro

def devkitpro():
    dkp = os.environ.get("DEVKITPRO") or "/opt/devkitpro"
    gcc = Path(dkp) / "devkitA64" / "bin" / "aarch64-none-elf-gcc"
    if not gcc.exists():
        raise Fallo("no encuentro devkitA64 (%s). Instala devkitPro (paquetes switch-dev, switch-sdl2, switch-mesa, "
                    "switch-glm, deko3d) y exporta DEVKITPRO, o usa --docker." % gcc)
    return Path(dkp)


def version_gcc(dkp):
    out = subprocess.run([str(dkp / "devkitA64" / "bin" / "aarch64-none-elf-gcc"), "-dumpfullversion"],
                         stdout=subprocess.PIPE, universal_newlines=True).stdout.strip()
    return out


def herramienta_dkp(dkp, nombre):
    return str(dkp / "devkitA64" / "bin" / ("aarch64-none-elf-" + nombre))


def cmake_ninja():
    for p in ("cmake", "ninja"):
        if not shutil.which(p):
            raise Fallo("falta %s en el PATH (instálalo, o usa --docker)" % p)


# ------------------------------------------------------------------------------------------- 5 uamlib

def etapa_uamlib(o):
    a = o.a
    dkp = devkitpro()
    cmake_ninja()
    origen = Path(a.uam).resolve() if a.uam else None
    huella = huella_arbol(origen, patron=r"^(source|mesa-imported)/") if origen else UAM_COMMIT
    clave = sha256_texto("|".join([VERSION_HERRAMIENTA, huella, sha256_archivo(DATOS / "uam_r15dk.patch"),
                                   huella_arbol(COMPILACION / "uamlib"), huella_arbol(TERCEROS / "uam_gen"),
                                   version_gcc(dkp)]))
    lib = o.t / "uamlib" / "libuamlib_ns.a"
    d = o.hecha("uamlib", clave)
    if d and lib.exists():
        o.dice("ya hecha: %s" % lib)
        return
    o.abrir("uamlib")
    try:
        src = o.t / "uamlib" / "uam"
        if origen:
            o.dice("copiando uam de %s…" % origen)
            copiar_arbol(origen, src, ignorar=shutil.ignore_patterns(".git", "build"))
        else:
            obtener_git(o, UAM_URL, UAM_COMMIT, src, "devkitPro/uam")
        aplicar_parche_git(o, src, DATOS / "uam_r15dk.patch", "uam")
        b = o.t / "uamlib" / "build"
        if b.exists():
            shutil.rmtree(b)
        env = {"DEVKITPRO": str(dkp)}
        o.dice("compilando uamlib para la Switch (Mesa del compilador de shaders; ~3-6 min)…")
        o.corre(["cmake", "-S", str(COMPILACION / "uamlib"), "-B", str(b), "-G", "Ninja",
                 "-DCMAKE_TOOLCHAIN_FILE=%s/cmake/Switch.cmake" % dkp, "-DCMAKE_BUILD_TYPE=Release",
                 "-DUAM=%s" % src, "-DGEN=%s" % (TERCEROS / "uam_gen")], env=env, nombre="cmake uamlib")
        o.corre(["ninja", "-C", str(b), "-j", str(o.a.jobs), "uamlib"], env=env, nombre="ninja uamlib")
        # libuamlib_ns.a: un solo objeto sin grupos COMDAT y todo local menos uamlib::Compile
        # (si no, el enlazador tiraría las vtables del Mesa de libEGL.a: r9/r15dk/LEEME.md)
        o.corre([herramienta_dkp(dkp, "ld"), "-r", "--force-group-allocation", "--whole-archive", "libuamlib.a",
                 "--no-whole-archive", "-o", "uamlib_all.o"], cwd=b, nombre="ld -r uamlib")
        o.corre([herramienta_dkp(dkp, "objcopy"), "--keep-global-symbol=_ZN6uamlib7CompileENS_5StageEPKcRSt6vectorIhSaIhEE",
                 "uamlib_all.o", "uamlib_local.o"], cwd=b, nombre="objcopy uamlib")
        if lib.exists():
            lib.unlink()
        o.corre([herramienta_dkp(dkp, "ar"), "rcs", str(lib), "uamlib_local.o"], cwd=b, nombre="ar uamlib")
        o.marca("uamlib", clave, lib=str(lib), sha256=sha256_archivo(lib))
        o.dice("uamlib: %s (%s)" % (lib, mb(lib.stat().st_size)))
    finally:
        o.cerrar()


# ------------------------------------------------------------------------------------------- 6 vista

def buscar_runtime(a):
    c = buscar_primero(a.runtime, AQUI.parent.parent / "runtime" / "nWiiRuntime",
                       AQUI.parent.parent / "_repo_github" / "runtime" / "nWiiRuntime")
    if not c:
        raise Fallo("no encuentro el runtime (runtime/nWiiRuntime del repositorio); usa --runtime")
    return c.resolve()


def con_line(src, dst, ruta_presunta):
    datos = Path(src).read_bytes()
    Path(dst).parent.mkdir(parents=True, exist_ok=True)
    with open(dst, "wb") as f:
        f.write(('#line 1 "%s"\n' % ruta_presunta).encode("utf-8"))
        f.write(datos)


def sincronizar(src, dst):
    """Deja dst igual que src escribiendo solo lo que cambia (conserva la fecha de lo que no cambia)."""
    src, dst = Path(src), Path(dst)
    n = 0
    vistos = set()
    for p in src.rglob("*"):
        rel = p.relative_to(src)
        vistos.add(rel)
        q = dst / rel
        if p.is_dir():
            q.mkdir(parents=True, exist_ok=True)
            continue
        if q.exists() and q.stat().st_size == p.stat().st_size and q.read_bytes() == p.read_bytes():
            continue
        q.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, q)
        n += 1
    if dst.exists():
        for q in sorted(dst.rglob("*"), reverse=True):
            if q.relative_to(dst) not in vistos:
                if q.is_dir():
                    shutil.rmtree(q, ignore_errors=True)
                else:
                    q.unlink()
    return n


def etapa_vista(o):
    a = o.a
    pas = o.info("pasadas")
    rt = buscar_runtime(a)
    clave = sha256_texto("|".join([VERSION_HERRAMIENTA, pas["clave"], huella_arbol(rt), huella_arbol(TERCEROS / "glad")]))
    v = o.t / "vista"
    d = o.hecha("vista", clave)
    if d and (v / "export").exists():
        o.dice("ya hecha: %s" % v)
        return
    o.abrir("vista")
    try:
        destino = v
        v = o.t / "vista.nueva"   # se monta aparte y luego solo se escriben en vista/ los archivos que cambian
        if v.exists():
            shutil.rmtree(v)
        exp = Path(pas["export"])
        (v / "export").mkdir(parents=True)
        n = 0
        for p in sorted(exp.iterdir()):
            if p.is_file() and p.suffix in (".cpp", ".h", ".inc"):
                con_line(p, v / "export" / p.name, "%s/export/%s" % (RUTA_PGO, p.name))
                n += 1
            elif p.is_file():
                shutil.copy2(p, v / "export" / p.name)
        copiar_arbol(rt, v / "nWiiRuntime", ignorar=shutil.ignore_patterns(".git", "build"))
        for p in sorted((v / "nWiiRuntime" / "include").rglob("*")):
            if p.is_file() and p.suffix in (".h", ".hpp", ".inc"):
                rel = p.relative_to(v / "nWiiRuntime").as_posix()
                tmp = p.with_suffix(p.suffix + ".tmp")
                con_line(p, tmp, "%s/nWiiRuntime/%s" % (RUTA_PGO, rel))
                tmp.replace(p)
                n += 1
        # GLAD (cargador de GL 3.3 core generado por glad v0.1.36) como copia «vendored» del runtime
        g = v / "nWiiRuntime" / "src" / "platform" / "glad"
        if not (g / "src" / "glad.c").exists():
            copiar_arbol(TERCEROS / "glad", g)
        cambiados = sincronizar(v, destino)
        shutil.rmtree(v)
        v = destino
        o.dice("archivos nuevos o cambiados en la vista: %d (los demás conservan su fecha: ninja no los recompila)"
               % cambiados)
        o.marca("vista", clave, archivos_con_line=n, runtime=str(rt))
        o.dice("vista lista: %d fuentes/cabeceras con #line a %s; runtime de %s" % (n, RUTA_PGO, rt))
    finally:
        o.cerrar()


# ------------------------------------------------------------------------------------------- 7 compilar

def nombre_gcda(objeto_rel):
    """Nombre del .gcda que busca GCC para un objeto (ruta relativa a -fprofile-prefix-path, '/' -> '#')."""
    return objeto_rel[:-2].replace("/", "#") + ".gcda"


def preparar_perfil(o):
    """Los .gcda tal cual: los objetos del código generado se compilan con la misma ruta relativa que en m5/m6q."""
    dst = o.t / "perfil"
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir()
    n = 0
    with tarfile.open(str(PERFILES / "m4_all_codigo_generado.tar.xz"), "r:xz") as t:
        for m in t.getmembers():
            if m.isfile() and "/" not in m.name and m.name.endswith(".gcda"):
                (dst / m.name).write_bytes(t.extractfile(m).read())
                n += 1
    return dst, n


def compilar_en_paralelo(o, ordenes, cwd, env, jobs, al_terminar=None):
    """[(nombre, orden de shell)] con hasta `jobs` a la vez; la salida de cada una va al registro."""
    pend = list(ordenes)
    vivos = []
    e = os.environ.copy()
    e.update(env)
    hechos = 0
    while pend or vivos:
        while pend and len(vivos) < jobs:
            nombre, orden = pend.pop(0)
            sal = open(str(o.t / "registros" / ("_tmp_%s.txt" % nombre)), "w+")
            vivos.append((nombre, orden, sal, subprocess.Popen(orden, shell=True, cwd=str(cwd), env=e,
                                                                 stdout=sal, stderr=subprocess.STDOUT)))
        time.sleep(0.5)
        for v in list(vivos):
            nombre, orden, sal, pr = v
            if pr.poll() is None:
                continue
            vivos.remove(v)
            sal.seek(0)
            txt = sal.read()
            sal.close()
            os.unlink(sal.name)
            o.escribe("$ " + orden + "\n" + txt + "# %s: código %d" % (nombre, pr.returncode))
            if pr.returncode != 0:
                for _, _, s2, p2 in vivos:
                    p2.kill()
                raise Fallo("falló la compilación de %s:\n%s" % (nombre, txt[-2000:]))
            hechos += 1
            if al_terminar:
                al_terminar(nombre, orden)
            if hechos % 10 == 0:
                o.dice("%s   %d/%d" % (hora(), hechos, len(ordenes)))


def decidir_pgo(o, dkp):
    a = o.a
    if a.sin_pgo:
        return False, "pedido --sin-pgo"
    pas = o.info("pasadas")
    if not pas.get("igual_referencia"):
        return False, "el código generado no es el de referencia (los perfiles no casan)"
    v = version_gcc(dkp)
    if v != GCC_PGO:
        return False, "aarch64-none-elf-gcc %s (los perfiles son de %s: formato .gcda distinto)" % (v, GCC_PGO)
    if not (PERFILES / "m4_all_codigo_generado.tar.xz").exists():
        return False, "faltan los perfiles en %s" % PERFILES
    return True, "perfiles m4_all (88 .gcda) con GCC %s" % v


def ninja_comandos(o, b, objetivo):
    return o.salida_de(["ninja", "-C", str(b), "-t", "commands", objetivo]).splitlines()


def etapa_compilar(o):
    a = o.a
    dkp = devkitpro()
    cmake_ninja()
    pgo, porque = decidir_pgo(o, dkp)
    vista = o.t / "vista"
    vexp = vista / "export"
    vrt = vista / "nWiiRuntime"
    juego = Path(o.info("juego")["juego"])
    uam = o.info("uamlib")
    orden = pgo or a.orden
    clave = sha256_texto("|".join([VERSION_HERRAMIENTA, o.info("vista")["clave"], uam["clave"], str(pgo), str(orden),
                                   str(a.sin_comprobar_dol),
                                   huella_arbol(COMPILACION), version_gcc(dkp), str(dkp), o.info("juego")["main_dol_sha256"]]))
    sal = o.t / "salida"
    d = o.hecha("compilar", clave)
    if d and (sal / "NSMBWnx.nro").exists():
        o.dice("ya hecha: %s" % (sal / "NSMBWnx.nro"))
        return
    o.abrir("compilar")
    try:
        o.dice("PGO: %s (%s)" % ("SÍ" if pgo else "no", porque))
        b = o.t / "compilacion"   # se conserva entre intentos: ninja solo rehace lo que cambió
        env = {"DEVKITPRO": str(dkp)}
        gen_flags = "-DG7_NOIC" + (" " + KFLAGS if pgo else "")
        perfil = ""
        if pgo:
            perfil, n = preparar_perfil(o)
            o.dice("perfil: %d .gcda en %s" % (n, perfil))
            if n != 88:
                raise Fallo("esperaba 88 perfiles del código generado y hay %d" % n)
        tomlpp_dir = o.t / "deps" / "tomlplusplus-src"
        if a.tomlplusplus:
            tomlpp_dir = Path(a.tomlplusplus).resolve()
        cfg = ["cmake", "-S", str(COMPILACION / "nsmbw"), "-B", str(b), "-G", "Ninja",
               "-DCMAKE_TOOLCHAIN_FILE=%s" % (COMPILACION / "cmake" / "nsmbw-switch.cmake"),
               "-DCMAKE_BUILD_TYPE=Release", "-DNWII_RUNTIME_DIR=%s" % vrt, "-DNSMBW_EXPORT_DIR=%s" % vexp,
               "-DCMAKE_PROJECT_nsmbw_switch_INCLUDE=%s" % (COMPILACION / "cmake" / "pgo.cmake"),
               "-DNWII_PGO=%s" % ("use" if pgo else "off"), "-DNWII_PGO_DIR=%s" % perfil,
               "-DNWII_GEN_FLAGS=%s" % gen_flags,
               "-DNWII_EXTRA_LIBS=%s %s" % (uam["lib"], dkp / "libnx" / "lib" / "libdeko3d.a"),
               "-DNWII_UAMLIB_INCLUDE=%s" % (COMPILACION / "uamlib"),
               "-DNSMBW_GAME_DOL=%s" % ("" if a.sin_comprobar_dol else juego / "sys" / "main.dol")]
        if tomlpp_dir.exists():
            cfg.append("-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=%s" % tomlpp_dir)
        o.dice("configurando (CMake)…")
        o.corre(cfg, env=env, nombre="cmake")
        # main_output.cpp pide ~3,5 GB de RAM: sola primero; luego lo demás en paralelo
        # El código generado (88 fuentes + main_output) se compila con las órdenes de ninja, pero con el objeto en
        # la MISMA ruta relativa que en m5/m6q (CMakeFiles/nsmbw.dir/<RUTA_PGO>/export/X.o): GCC usa esa ruta
        # para el nombre del .gcda y para el identificador de perfil de las funciones static. El resto (runtime,
        # main.cpp, glad) lo compila ninja; el enlace lo hace esta herramienta (con PGO el de CMake fallaría por
        # saltos condicionales a las partes frías demasiado lejos, y además falta el agua nativa).
        comandos = ninja_comandos(o, b, "nsmbw.nro")
        pat = re.compile(r" -c (%s/(output_\d+|main_output|fastdispatch)\.cpp)$" % re.escape(str(vexp)))
        od_nuevo = "CMakeFiles/nsmbw.dir" + RUTA_PGO + "/export"
        gen, cambios = [], {}
        for c in comandos:
            m = pat.search(c)
            if not m:
                continue
            viejo = re.search(r" -o (\S+) ", c).group(1)
            nuevo = "%s/%s.o" % (od_nuevo, m.group(2))
            cambios[viejo] = nuevo
            c = c.replace(viejo, nuevo)
            if pgo and m.group(2) == "output_24":
                # A43 cambió 3 funciones de output_24 después de tomar el perfil (receta de r9/r43a): solo avisos.
                # Como en m6q, output_24 va SIN los flags K: la receta de A43 tomó la orden de k_cmds.txt, que no
                # los lleva (build_k.sh los añade al vuelo). Así sale el mismo código que el binario medido.
                c = c.replace(" " + KFLAGS, "")
                c = c.replace(" -c " + m.group(1), " -Wno-error=coverage-mismatch -c " + m.group(1))
            gen.append((m.group(2), c))
        if len(gen) != 89:
            raise Fallo("esperaba 89 fuentes generados en las órdenes de ninja y hay %d" % len(gen))
        (b / od_nuevo).mkdir(parents=True, exist_ok=True)
        objs = o.salida_de(["ninja", "-C", str(b), "-t", "query", "nsmbw.elf"])
        entradas = []
        dentro = False
        for ln in objs.splitlines():
            t = ln.strip()
            if t.startswith("input:"):
                dentro = True
                continue
            if t.startswith("outputs:"):
                dentro = False
            if not dentro or not t:
                continue
            ruta = t.split()[-1]    # «| ruta» = dependencia implícita (p. ej. libnwiiruntime.a): también cuenta
            if ruta in ("|", "||") or "LINKER" in ruta or ruta.startswith("/opt/") or ruta.startswith(str(dkp)) \
                    or ruta in cambios or ruta.endswith(".a") and os.path.isabs(ruta):
                continue
            entradas.append(ruta)
        t0 = time.time()
        o.dice("%s compilando el runtime (ninja -j%d)…" % (hora(), a.jobs))
        o.corre(["ninja", "-C", str(b), "-j", str(a.jobs)] + entradas, env=env, nombre="ninja (runtime)")
        # incremental: se rehace un objeto si falta, si su fuente es más nueva o si cambió su orden
        def hace_falta(nombre, orden):
            ob = b / od_nuevo / (nombre + ".o")
            marca = b / od_nuevo / (nombre + ".orden")
            return not (ob.exists() and marca.exists() and marca.read_text() == orden
                        and ob.stat().st_mtime >= (vexp / (nombre + ".cpp")).stat().st_mtime)
        pend = [(n, c) for n, c in gen if hace_falta(n, c)]
        mo = [x for x in pend if x[0] == "main_output"]
        resto = [x for x in pend if x[0] != "main_output"]
        def apunta(nombre, orden):
            (b / od_nuevo / (nombre + ".orden")).write_text(orden)
        for n, c in gen:
            if (n, c) in pend:
                m_ = b / od_nuevo / (n + ".orden")
                if m_.exists():
                    m_.unlink()
        if mo and a.jobs < 6:
            # main_output.cpp pide ~3,5 GB: con pocos trabajos (poca RAM), sola primero
            o.dice("%s compilando main_output.cpp (sola, ~3,5 GB de RAM)…" % hora())
            compilar_en_paralelo(o, mo, b, env, 1, apunta)
            mo = []
        o.dice("%s compilando %d fuentes generados con -j%d (%s)…" % (
            hora(), len(mo) + len(resto), a.jobs, "PGO + " + KFLAGS if pgo else "sin PGO"))
        compilar_en_paralelo(o, mo + resto, b, env, a.jobs, apunta)
        o.dice("%s compilado en %.1f min" % (hora(), (time.time() - t0) / 60))
        enl = [c for c in comandos if " -o nsmbw.elf " in c]
        nacp = [c for c in comandos if "nacptool" in c]
        e2n = [c for c in comandos if "elf2nro" in c]
        c1 = [c for n, c in gen if n == "output_1"]
        if not (enl and nacp and e2n and c1):
            raise Fallo("no encuentro en ninja las órdenes de enlace/nacp/elf2nro/output_1")
        # agua nativa: con la orden de output_1 sin perfil ni KFLAGS (como r41m/build_pgo_m6.sh)
        wag = Path(o.info("pasadas")["wagua"])
        wo = b / "w_agua_gen.o"
        cw = c1[0]
        cw = re.sub(r" -fprofile-[a-z-]+(=\S*)?", "", cw)
        cw = cw.replace(" -Wno-missing-profile", "").replace(" " + KFLAGS, "")
        cw = re.sub(r" -MD -MT \S+ -MF \S+", "", cw)
        cw = re.sub(r" -o \S+ -c \S+$", " -I%s -o %s -c %s" % (wag, wo, wag / "w_agua_gen.cpp"), cw)
        if "-fprofile" in cw:
            raise Fallo("quedan opciones de perfil en la orden del agua")
        o.dice("compilando el agua nativa (w_agua_gen.cpp)…")
        o.corre(cw, cwd=b, env=env, shell=True, nombre="w_agua_gen.cpp")
        lk = enl[0]
        lk = re.sub(r"^: && ", "", lk)
        lk = re.sub(r" && :$", "", lk)
        for viejo, nuevo in cambios.items():
            lk = lk.replace(" " + viejo + " ", " " + nuevo + " ")
        lk = lk.replace(" -o nsmbw.elf ", " %s -o nsmbw.elf " % wo, 1)
        if any(" " + v + " " in lk for v in cambios):
            raise Fallo("quedan objetos viejos en la orden de enlace")
        (b / "orden").mkdir(exist_ok=True)
        if orden:
            od = od_nuevo
            cru = o.salida_de([sys.executable, "-I", str(PASADAS / "cruces.py"), od], cwd=b, env=env)
            (b / "orden" / "cruces.tsv").write_text(cru)
            o.corre([sys.executable, "-I", str(PASADAS / "orden_pares.py"), "orden/cruces.tsv", "orden/order.txt",
                     str(DATOS / "orden_L_m6q.txt")], cwd=b, nombre="orden_pares.py")
            o.dice("orden de enlace: %d saltos condicionales entre partes caliente/fría + orden L de m6q"
                   % len(cru.splitlines()))
            lk = lk.replace(" -o nsmbw.elf ", " -Wl,--section-ordering-file=orden/order.txt -Wl,-Map=orden/nsmbw.map "
                            "-o nsmbw.elf ", 1)
        for f in ("nsmbw.elf", "nsmbw.nro"):
            if (b / f).exists():
                (b / f).unlink()
        o.dice("enlazando…")
        o.corre(lk, cwd=b, env=env, shell=True, nombre="enlace")
        o.corre(nacp[0], cwd=b, env=env, shell=True, nombre="nacptool")
        o.corre(e2n[0], cwd=b, env=env, shell=True, nombre="elf2nro")
        # comprobaciones
        fm = o.salida_de([sys.executable, "-I", str(PASADAS / "check_fm_tab_g5.py"), str(b / "nsmbw.elf")])
        o.escribe(fm)
        fm_linea = " ".join(fm.strip().splitlines()[-2:])
        o.dice("tabla fastmem: %s" % fm_linea)
        # check_fm_tab_g5.py: «N entries, …» y «bad: 0» (o un dict con las entradas mal formadas)
        m = re.search(r"^bad: (.*)$", fm, re.M)
        if not m or not re.search(r"^\d+ entries", fm, re.M):
            raise Fallo("no pude leer la comprobación de la tabla fastmem:\n" + fm[-500:])
        mal = 0 if m.group(1).strip() == "0" else m.group(1).strip()
        wagua = (b / "nsmbw.elf").read_bytes().count(b"[WAGUA]")
        o.dice("[WAGUA] en el ELF: %d" % wagua)
        if mal:
            raise Fallo("la tabla fastmem tiene entradas mal formadas: %s" % mal)
        if not wagua:
            raise Fallo("el ELF no tiene el agua nativa ([WAGUA])")
        sal.mkdir(exist_ok=True)
        shutil.copy2(b / "nsmbw.nro", sal / "NSMBWnx.nro")
        shutil.copy2(b / "nsmbw.elf", sal / "NSMBWnx.elf")
        size = o.salida_de([herramienta_dkp(dkp, "size"), str(b / "nsmbw.elf")])
        o.escribe(size)
        o.marca("compilar", clave, pgo=pgo, pgo_motivo=porque, orden=orden, nro=str(sal / "NSMBWnx.nro"),
                nro_sha256=sha256_archivo(sal / "NSMBWnx.nro"), elf_sha256=sha256_archivo(sal / "NSMBWnx.elf"),
                fastmem=fm_linea, gcc=version_gcc(dkp), minutos=round((time.time() - t0) / 60, 1))
    finally:
        o.cerrar()


# ------------------------------------------------------------------------------------------- 8 sd

def etapa_sd(o):
    comp = o.info("compilar")
    juego = o.info("juego")["juego"]
    sal = o.t / "salida"
    clave = sha256_texto("|".join([VERSION_HERRAMIENTA, comp["nro_sha256"], o.info("juego")["clave"]]))
    d = o.hecha("sd", clave)
    if d and (sal / "sd").exists():
        o.dice("ya hecha: %s" % (sal / "sd"))
        return
    o.abrir("sd")
    try:
        _, ps = cargar_preparar_sd()
        o.dice("preparando la carpeta de la SD (copia de los datos del juego + el NRO)…")
        o.corre([sys.executable, str(ps), juego, "--salida", str(sal / "sd"), "--nro", str(sal / "NSMBWnx.nro")],
                nombre="preparar_sd.py")
        o.marca("sd", clave, sd=str(sal / "sd"))
    finally:
        o.cerrar()


# ------------------------------------------------------------------------------------------- docker

def ruta_docker(p):
    return str(Path(p).resolve())


def con_docker(a, o):
    """Ejecuta las etapas 2-8 dentro de la imagen de devkitPro (la 1, extraer/validar el juego, va en el anfitrión)."""
    if not shutil.which("docker"):
        raise Fallo("no encuentro docker (instala Docker Desktop con WSL2 en Windows, o docker en Linux)")
    raiz_repo = AQUI.parent.parent
    print("[docker] imagen %s (de %s + python3, git, cmake, ninja, g++)…" % (IMAGEN_DOCKER, IMAGEN_DOCKER_BASE))
    r = subprocess.run(["docker", "build", "-t", IMAGEN_DOCKER, str(AQUI / "docker")])
    if r.returncode != 0:
        raise Fallo("docker build falló")
    juego = Path(o.info("juego")["juego"])
    vol = ["-v", "%s:/herramienta:ro" % ruta_docker(raiz_repo), "-v", "%s:/trabajo" % ruta_docker(o.t),
           "-v", "%s:/juego:ro" % ruta_docker(juego)]
    extra = []
    for opt, dst in (("recompilador", "/ext/recompilador"), ("uam", "/ext/uam"), ("tomlplusplus", "/ext/tomlplusplus"),
                     ("runtime", "/ext/runtime"), ("parche_recompilador", "/ext/parche.patch")):
        v = getattr(a, opt)
        if v:
            vol += ["-v", "%s:%s:ro" % (ruta_docker(v), dst)]
            extra += ["--" + opt.replace("_", "-"), dst]
    rel = AQUI.relative_to(raiz_repo).as_posix()
    orden = ["docker", "run", "--rm"] + vol + [IMAGEN_DOCKER, "python3", "/herramienta/%s/construir_nro.py" % rel,
                                              "--juego", "/juego", "--trabajo", "/trabajo", "--jobs", str(a.jobs),
                                              "--dentro-docker"] + extra
    for f in ("sin_pgo", "orden", "permitir_distinto", "conservar_intermedios", "sin_comprobar_dol"):
        if getattr(a, f):
            orden.append("--" + f.replace("_", "-"))
    if a.hasta:
        orden += ["--hasta", a.hasta]
    if a.rehacer:
        orden += ["--rehacer", a.rehacer]
    print("[docker] " + " ".join(orden))
    sys.stdout.flush()
    return subprocess.run(orden).returncode


# ------------------------------------------------------------------------------------------- principal

def jobs_por_defecto():
    n = os.cpu_count() or 2
    try:
        mem = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / 2 ** 30
        n = min(n, max(1, int((mem - 4) // 1.2)))
    except (ValueError, OSError, AttributeError):
        pass
    return max(1, min(n, 8))


def resumen(o):
    print("\n=== Resultado ===")
    comp = o.info("compilar")
    if comp:
        nro = Path(comp["nro"])
        print("  NRO:      %s" % nro)
        print("            sha256 %s  (%s)" % (comp["nro_sha256"], mb(nro.stat().st_size)))
        print("  ELF:      %s  (sha256 %s)" % (nro.with_suffix(".elf"), comp["elf_sha256"]))
        print("  PGO:      %s (%s); orden de enlace: %s" % ("sí" if comp["pgo"] else "no", comp["pgo_motivo"],
                                                            "sí" if comp["orden"] else "no"))
        print("  fastmem:  %s" % comp["fastmem"])
    sd = o.info("sd")
    if sd:
        print("  SD:       %s  (copia su contenido a la raíz de la tarjeta)" % sd["sd"])
    print("  Registros: %s" % o.reg)
    for e in ETAPAS:
        if e in o.tiempos:
            print("    %-13s %5.1f min" % (e, o.tiempos[e] / 60))


def principal(argv=None):
    ap = argparse.ArgumentParser(description="Construye NSMBWnx.nro y la carpeta de la SD desde tu copia del juego.")
    ap.add_argument("--juego", required=True, help="carpeta extraída del disco, o imagen .iso/.wbfs (necesita wit o DolphinTool)")
    ap.add_argument("--trabajo", required=True, help="carpeta de trabajo (~3 GB libres; se puede reanudar)")
    ap.add_argument("--jobs", type=int, default=jobs_por_defecto(), help="compilaciones en paralelo (~1 GB de RAM cada una)")
    ap.add_argument("--sin-pgo", action="store_true", help="no usar los perfiles de PGO incluidos")
    ap.add_argument("--orden", action="store_true", help="orden de enlace L también sin PGO (con PGO va siempre)")
    ap.add_argument("--docker", action="store_true", help="compilar dentro de la imagen devkitpro/devkita64 (Windows, o sin devkitPro)")
    ap.add_argument("--recompilador", help="copia local de NWiiRecomp (si no, se clona de GitHub)")
    ap.add_argument("--parche-recompilador", help="cambios_locales_completos.patch (por defecto, el del repositorio)")
    ap.add_argument("--uam", help="copia local de devkitPro/uam (si no, se clona de GitHub)")
    ap.add_argument("--tomlplusplus", help="copia local de toml++ v3.4.0 (si no, se clona de GitHub)")
    ap.add_argument("--runtime", help="carpeta nWiiRuntime (por defecto, runtime/nWiiRuntime del repositorio)")
    ap.add_argument("--herramienta-imagen", help="ruta de wit o DolphinTool para leer .iso/.wbfs")
    ap.add_argument("--hasta", choices=ETAPAS, help="parar después de esta etapa")
    ap.add_argument("--rehacer", choices=ETAPAS, help="rehacer desde esta etapa aunque esté hecha")
    ap.add_argument("--permitir-distinto", action="store_true",
                    help="seguir aunque el código generado no sea el de referencia (sin PGO; no probado)")
    ap.add_argument("--conservar-intermedios", action="store_true", help="no borrar las copias de cada pasada")
    ap.add_argument("--sin-comprobar-dol", action="store_true",
                    help="no meter en el NRO la comprobación del SHA-256 de main.dol (así sale idéntico a m6q, que no la "
                         "lleva por un descuido de su compilación)")
    ap.add_argument("--dentro-docker", action="store_true", help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    o = Obra(a)
    print("construir_nro %s — trabajo: %s, jobs %d, %s" % (VERSION_HERRAMIENTA, o.t, a.jobs, platform.platform()))
    if a.rehacer:
        for e in ETAPAS[ETAPAS.index(a.rehacer):]:
            p = o.est / (e + ".json")
            if p.exists():
                p.unlink()
    funcs = {"juego": etapa_juego, "herramientas": etapa_herramientas, "recompilar": etapa_recompilar,
             "pasadas": etapa_pasadas, "uamlib": etapa_uamlib, "vista": etapa_vista, "compilar": etapa_compilar,
             "sd": etapa_sd}
    try:
        etapas = ETAPAS
        if a.docker and not a.dentro_docker:
            print("\n[%s] etapa juego (en este equipo)" % hora())
            etapa_juego(o)
            if a.hasta == "juego":
                return 0
            rc = con_docker(a, o)
            if rc == 0:
                resumen(o)
            return rc
        for e in etapas:
            o.etapa = e
            print("\n[%s] etapa %s" % (hora(), e))
            t0 = time.time()
            funcs[e](o)
            o.tiempos[e] = time.time() - t0
            if a.hasta == e:
                break
        resumen(o)
        return 0
    except Fallo as e:
        print("\nERROR en la etapa %s: %s" % (o.etapa or "?", e))
        return 1
    except KeyboardInterrupt:
        print("\ninterrumpido (se puede reanudar con la misma orden)")
        return 130


if __name__ == "__main__":
    sys.exit(principal())
