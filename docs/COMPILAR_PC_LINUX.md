# Compilar la versión de PC (Linux)

Probado en CachyOS / Arch con GCC 15+, CMake 4 y Mesa (Intel Arc).
Los scripts asumen que el proyecto está en `~/nsmbw-port`.

## 1. Dependencias

```fish
sudo pacman -S --needed base-devel cmake ninja ccache mold git python rsync wit
# opcionales, para depurar y medir: perf valgrind heaptrack gdb
```

SDL2, glad y toml++ no hace falta instalarlos: CMake los descarga y los enlaza de forma estática.

## 2. Extraer tu disco

```fish
cd ~/nsmbw-port
wit extract --psel DATA "/ruta/a/SMNE01.wbfs" game/SMNE01
# debe existir game/SMNE01/sys/main.dol y game/SMNE01/files/
```

Los RELs descomprimidos que usa el recompilador van en `bin_usa/`:
`d_profileNP.rel`, `d_basesNP.rel`, `d_enemiesNP.rel` y `d_en_bossNP.rel`.
Salen de `game/SMNE01/files/rels/*.rel.LZ`.

## 3. Compilar el recompilador (NWiiRecomp)

```fish
cd ~/nsmbw-port/NWiiRecomp      # rama nsmbw
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSDL_PIPEWIRE=OFF
ninja -C build nwiirecomp       # solo el recompilador (nWiiStudio no hace falta)
```

## 4. Traducir el juego y compilarlo

```fish
cd ~/nsmbw-port/recomp
JOBS=4 ./build.sh regen    # traduce DOL + 4 RELs a C++ (export/), aplica fast dispatch y compila
```

El resultado es `recomp/export/build/NSMBW`.

- `./build.sh regen` hace la traducción completa. Hace falta solo si cambias el recompilador o `nsmbw.toml`.
  Ejecuta `nwiirecomp`, luego `perf/apply_fastdispatch.py`, que crea la tabla de saltos O(1), y después (desde R8)
  `perf/apply_r8_codegen.py --prune-only --memview`, que poda el `switch` de entrada de cada función y le da una
  `MemView`. Los dos pasos son obligatorios: el código podado necesita el runtime de R8 (`resume_pruned` y
  `MemView` en `cpu_context.h`). La pasada marca los archivos con `// [reglocals]` y no se aplica dos veces.
- `./build.sh sync` copia los cambios del runtime (`NWiiRecomp/nWiiRuntime`) a `export/` y recompila.
- `./build.sh` recompila `export/` tal cual.
- `JOBS` controla los trabajos en paralelo. Con 16 GB de RAM usa 4 como máximo; el código generado son archivos C++ enormes.
- Opciones fijas del script: `-O3`, ccache, enlazador mold, `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (CMake 4)
  y `-DSDL_PIPEWIRE=OFF`.

`recomp/nsmbw.toml` es la configuración del recompilador:
- rutas;
- `[hle_hooks]`: funciones del SDK que se reemplazan, como los Wiimotes;
- `[[rel_aot]]`: RELs compilados por adelantado, con direcciones sacadas de Dolphin con `scripts/dolphin_rels.py`;
- `hle_skip`.

El runtime solo lee `[system]`, `[graphics]`, `[input]` y `[hle]`.

## 5. Ejecutar

```fish
cd ~/nsmbw-port/recomp
./export/build/NSMBW ~/nsmbw-port/game/SMNE01 nsmbw.toml
```

Argumentos: carpeta del juego extraído y archivo de configuración.
Las partidas se guardan en `nand/`, junto al `.toml`, salvo que `[system] nand_dir` diga otra cosa.
La caché de shaders va en `shadercache.bin`, junto a la NAND.

Si compilas en la misma máquina donde trabajan otros procesos pesados, usa `scripts/lim` (límite de memoria por systemd):
`~/nsmbw-port/scripts/lim ./export/build/NSMBW ...`

## 6. Empaquetar para otros (paquete "doble clic")

El paquete probado está en `dist/NSMBW-PC-r6/`:
- `NSMBW`: el ejecutable;
- `nsmbw.toml`: solo las secciones del runtime, sin rutas de tu máquina;
- `Jugar`: lanzador ELF que ejecuta `jugar.sh`;
- `jugar.sh`: la primera vez pide el `.wbfs`, comprueba que sea SMNE01 y lo extrae con `wit`;
- `crear_reporte.sh`;
- `LEEME.txt`.

Para una versión nueva:

```fish
cp recomp/export/build/NSMBW dist/NSMBW-PC-r6/NSMBW
tar -I 'zstd -19 -T4' -cf dist/NSMBW-PC-rX.tar.zst -C dist NSMBW-PC-r6
```

El ejecutable solo depende de `libc`, `libstdc++` y `libm`; SDL2 va estático y OpenGL se carga en ejecución.
Funciona en cualquier x86-64 moderno: compila sin `-march=native`.
