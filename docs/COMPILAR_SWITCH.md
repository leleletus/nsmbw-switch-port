# Compilar el .nro de Switch

El `.nro` usa el **mismo código generado** que la versión de PC (`recomp/export`) más el runtime de NWiiRecomp.
Todo lo propio de Switch está dentro de `#ifdef __SWITCH__` o en `nWiiRuntime/src/platform/switch/`.
Compila la versión de PC primero: ver [COMPILAR_PC_LINUX.md](COMPILAR_PC_LINUX.md).

## 1. devkitPro

Instala devkitPro (pacman de devkitPro en Arch) y luego:

```fish
sudo pacman -S --needed switch-dev switch-sdl2 switch-mesa switch-libdrm_nouveau
```

Queda en `/opt/devkitpro`: devkitA64 (GCC para ARM64), libnx, SDL2, Mesa (OpenGL/EGL) y libdrm_nouveau.

## 2. Compilar

```fish
cd ~/nsmbw-port/switch
./build.sh        # → nsmbw/build/nsmbw.nro (~158 MB, de ellos ~110 MB de código)
```

`build.sh` hace tres cosas:
1. `sync_sources.sh --checked`: toma una copia de `NWiiRecomp/nWiiRuntime` → `runtime-snap/` y de
   `recomp/export` → `export-snap/`. Así, si alguien edita el runtime a mitad de una compilación, no la rompe.
   Antes comprueba que el runtime compila en el PC.
2. La primera vez configura con CMake usando `cmake/nsmbw-switch.cmake`:
   - devkitPro `Switch.cmake`;
   - `-O2 -ffp-contract=off`: sin FMA implícitas, para que la coma flotante dé lo mismo que en PC;
   - `NWII_PLATFORM_SWITCH=1`.
3. `ninja -j4`.

Tiempos: si no cambian `cpu_context.h`, `config.h`, `nwii_env.h` ni `functions.h`, recompila en ~1 min.
Si cambian (o tras `recomp/build.sh regen`), son ~20 min con `-j4`. Si otros procesos usan el slice de memoria,
compilar a `-j3` o `-j2` (`ninja -C nsmbw/build -j3` tras `./sync_sources.sh --checked`): a -j3 junto a otras
compilaciones lo mató una vez el tope de 6,5 GB de `scripts/lim`.

El SHA-256 de `game/SMNE01/sys/main.dol` se calcula al configurar. El `.nro` lo comprueba al arrancar y
avisa si el disco no es la revisión correcta.

## 3. Armar la carpeta de la SD

```
SD:/switch/NSMBWnx/
├── NSMBWnx.nro      ← nsmbw/build/nsmbw.nro renombrado
├── cache/shadercache.bin  ← (R8, recomendado) caché de shaders que se lleva: switch/NSMBWnx/cache/
└── data/            ← sys/ y files/ del disco extraído
```

`config.toml`, `save/`, `cache/` y `logs/` los crea el `.nro`. La caché de `cache/` es la de
`r8/shaders/cache_switch/shadercache.bin` (201 programas, solo fuentes): sin ella cada shader nuevo se compila
mientras se juega (~20-50 ms con los programas separables de R8; ~2 s con los enlazados de r7b). Si la consola ya
tiene una caché propia, unirlas con `python3 -I r8/shaders/tools/nsc2.py merge <salida> <una> <otra>`. Para probar con registros, agrega `debug.toml`
(ver [DEPURAR.md](DEPURAR.md)).

La carpeta de trabajo local es `switch/NSMBWnx/`.

Copia por MTP (DBI → "MTP Responder") desde Linux con KDE:

```fish
kioclient copy --noninteractive switch/NSMBWnx "mtp:/DBI MTP Responder/1: SD Card/switch/"
# solo el .nro después de recompilar:
kioclient remove "mtp:/DBI MTP Responder/1: SD Card/switch/NSMBWnx/NSMBWnx.nro"
kioclient copy --noninteractive switch/NSMBWnx/NSMBWnx.nro "mtp:/DBI MTP Responder/1: SD Card/switch/NSMBWnx/"
```

### Dónde busca sus archivos

`choose_root()` en `switch_platform.cpp` busca en este orden y usa la primera carpeta que tenga el juego:
1. la carpeta del `.nro`, que hbmenu pasa en `argv[0]`;
2. `sdmc:/switch/NSMBWnx/`;
3. `sdmc:/nsmbw/` (instalaciones antiguas).

Los nombres viejos siguen funcionando: `SMNE01/`, `nsmbw.toml`, `nand/` y `env.txt`.

## 4. Probar en citron (emulador)

```fish
cd ~/nsmbw-port/switch
./citron_test.sh <ruta.nro> 600 citron-test/miprueba
```

Abre citron dentro de un KWin sin pantalla, sin ventana y sin sonido. La SD de citron es
`~/.local/share/citron/sdmc`. Pon ahí `switch/NSMBWnx/` con `data/` y un `debug.toml` con `NWII_XFBDUMP`
para tener capturas.

El script pone el PC en **"rendimiento"** antes de la prueba: siempre se trabaja a máxima potencia.
Con `PERFIL=ahorro` corre en ahorro de energía, solo si se pide.

citron en el PC es **más lento que la Switch real**. En los avisos del arranque, la Switch real iba a 60 fps y
citron a 26-49 fps. Por eso es una prueba más estricta: lo que llega a 60 fps estables en citron debería ir a
60 fps en la consola. Se prueba en la Switch real solo cuando citron ya va a 60 estables.

## 5. Memoria

Hay que abrirlo con *title takeover*: mantener R al abrir un juego. Así hay ~3,2 GB disponibles.
En modo applet hay ~400 MB y el `.nro` necesita más de 430 MB.
