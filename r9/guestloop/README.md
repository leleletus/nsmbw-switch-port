# r9/base: copia congelada de r8 (31-oct-2026)

Punto de partida común de los frentes de la ronda R9 (ver `status/PLAN_R9.md`). **No editar esta carpeta**: cada
frente trabaja en su propia copia. Las medidas de partida (citron y PC) están en **`BASELINE.md`**.

r8 = r7b + `r8/shaders` (programas separables + caché de 201 programas) + `r8/memory` (cola de GX acotada) +
`r8/codegen` (poda del `switch` de entrada, `MemView`, `gqr_pow2`). Detalle en `status/r8_final.md`.

| Qué | Contenido |
|---|---|
| `nWiiRuntime/` | copia de `NWiiRecomp/nWiiRuntime` (lo que edita un frente del runtime) |
| `export/` | copia de `recomp/export` sin `build/` (código generado de r8, ya pasado por `perf/apply_r8_codegen.py`), con `export/nWiiRuntime/` (= `nWiiRuntime/`; `build_pc.sh` lo vuelve a sincronizar) |
| `gen/` | objetos de PC del código generado de r8 (`CMakeFiles/NSMBW.dir/*.cpp.o`), `libSDL2.a`, `libglad.a` y la línea de enlace (`link_cmd.txt`); `input_script_r8.o` es el objeto viejo de `cpu/tools/input_script.cpp` (solo para comprobar que se rehace `NSMBW_r8_input_test` igual) |
| `switch_gen/` | objetos de Switch (devkitA64) del código generado de r8 y la línea de enlace de `switch/nsmbw/build` |
| `build_pc.sh`, `build_nx.sh` | compilan un binario de PC / un NRO desde la carpeta donde están (la copia), **y además** su variante `_input_test` (entrada guionizada, solo pruebas) |
| `tools/input_script.cpp` | entrada guionizada de pruebas para PC **y Switch** (ver abajo): lo de siempre + registro `[SCENE]` + fases por escena |
| `citron_scene.sh` | llega en citron al título, al mapa del Mundo 1 o al 1-1 en 30-100 s y lo mide (ver abajo) |
| `citron/` | `save_tpl/` (NAND con una partida que ya tiene el Mundo 1 abierto; copia de la plantilla `mp` de `verify/integration_r8`, **no** la NAND del usuario) y `shadercache.bin` (la de r8, 201 programas) |
| `pc/` | pruebas en PC sin ventana ni sonido: `run.sh` (copia de `verify/integration_r8/run.sh` que usa su propia carpeta), `prof.sh` (perfil `perf` de una escena con el perfil de CPU `sw`), `limc`, plantillas de NAND y de caché de shaders |
| `tools/citron_scene_summary.py`, `tools/perf_report.py` | resúmenes de una corrida de citron y de un perfil `perf` |
| `bin/` | binarios de referencia de r8 (copias reflink): `NSMBW_r8`, `NSMBW_r8_input_test` (con la entrada nueva), `nsmbw_r8.nro` (= `switch/bin/nsmbw_r8_20261029.nro`), `nsmbw_r8_input_test.nro` |
| `runs/`, `pc/runs/`, `pc/prof/` | las corridas de `BASELINE.md` (logs, resúmenes, capturas pequeñas, `perf.data`) |

## Hacer una copia para un frente

```bash
cp -a --reflink=always ~/nsmbw-port/r9/base ~/nsmbw-port/r9/<frente>   # btrfs: casi no ocupa disco
rm -rf ~/nsmbw-port/r9/<frente>/{SNAPSHOT_OK,runs,pc/runs,pc/prof}
```

Se edita `r9/<frente>/nWiiRuntime/` (y `export/` solo si el frente regenera o toca el código generado). Los
scripts usan la carpeta en la que están, así que funcionan tal cual en la copia (también `pc/run.sh`, que reescribe
`nand_dir` a su propia carpeta).

## Compilar

```bash
JOBS=3 ~/nsmbw-port/scripts/lim ~/nsmbw-port/r9/<frente>/build_pc.sh ~/nsmbw-port/r9/<frente>/bin/NSMBW_x
JOBS=3 ~/nsmbw-port/scripts/lim ~/nsmbw-port/r9/<frente>/build_nx.sh ~/nsmbw-port/r9/<frente>/bin/nsmbw_x.nro
```

- PC: saca `NSMBW_x` y `NSMBW_x_input_test`. Switch: saca `nsmbw_x.nro` y `nsmbw_x_input_test.nro`.
- Rápido (por defecto): compila solo `libnwiiruntime.a`, `main` (y `glad` en Switch) más `tools/input_script.cpp`, y
  enlaza con `gen/` / `switch_gen/`. Con ccache caliente, ~40 s (PC) y ~20 s (Switch).
- `FULL=1`: recompila también el código generado (PC 20-40 min a -j3; Switch 15-20 min). **Obligatorio** si se
  regeneró `export/` o si se cambió una cabecera del runtime que incluye el código generado
  (`include/runtime/*.h`: `cpu_context.h`, `config.h`, `memory.h`...). Si no, los objetos de `gen/` no casan con el
  runtime nuevo (mismo nombre, otra estructura) y el binario falla de formas raras.
- El NRO rápido se enlaza ahora como `nx/build/fast/nsmbw.elf` (antes `nsmbw_fast.elf`): el nombre del .elf va dentro
  del NRO (`.nx-module-name`) y, si cambia de largo, se mueve todo el binario. Así el NRO rápido es igual al completo.
- No usa ni toca `switch/runtime-snap`, `switch/export-snap` ni `switch/nsmbw/build` (de ahí solo lee las fuentes de
  toml++ y glad).

## Entrada guionizada con escenas (`tools/input_script.cpp`, solo pruebas)

Va enlazada con `--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx` en los binarios `_input_test`; el runtime que se
entrega no cambia. En PC se lee del entorno; en Switch, de `debug.toml` (las claves `NWII_*` pasan tal cual).

- `NWII_INPUT_SCRIPT` igual que antes: `[pN:]desde-hasta:máscara:periodo:duración;...` (frames = lecturas de KPAD
  del canal 0; bits WPAD: 2 = `100`, A = `800`, derecha en pantalla = `4`...).
- **Nuevo:** registro `[SCENE] frame=F t=S scene=N (NOMBRE) prev=M phase=P` (en stdout = `nwii.log` en Switch) cada
  vez que cambia `dScene_c::m_nowScene` (u16 en `0x80428732` de SMNE01; `NWII_INPUT_SCENE_ADDR` lo cambia).
  Escenas: 0 BOOT, 3 WORLD_MAP, 5 STAGE (el título también es un STAGE), 6 RESTART_CRSIN, 7 CRSIN (cartel "Mundo
  1-1"), 8 MOVIE (la fiesta de Peach del modo demo), 10 GAME_SETUP (elegir archivo y jugadores).
- **Nuevo:** `NWII_INPUT_PHASES = "3>5"`: la fase k empieza la primera vez que la escena pasa a ser el k-ésimo número
  (después de la fase k-1); desde ahí manda `NWII_INPUT_SCRIPT_<k>` (frames contados desde el inicio de la fase;
  vacío = sin botones). `NWII_INPUT_PHASE_DELAY = n` la retrasa n frames.
- En la Switch los jugadores 2-4 se inyectan directamente (no hay mandos virtuales de SDL).

## Llegar rápido a una escena en citron: `citron_scene.sh`

```bash
~/nsmbw-port/r9/<frente>/citron_scene.sh <nro> <title|map|lvl11> <segundos> <carpeta_salida>
# p. ej.: ~/nsmbw-port/r9/base/citron_scene.sh ~/nsmbw-port/r9/base/bin/nsmbw_r8.nro map 300 /tmp/x
```

- Usa `<nro sin .nro>_input_test.nro` si existe (para `map` y `lvl11` es obligatorio).
- Toma solo el candado de citron de toda la máquina (`flock switch/citron-test/.lock`), corre citron dentro de
  `nsmbw.slice` con `scripts/lim` (que también pide uno de los 2 turnos pesados), pone el perfil `performance` y lo
  vigila durante la corrida (si algo lo cambia, lo vuelve a poner y avisa: la corrida no vale).
- Respalda `sdmc:/switch/NSMBWnx` (todo menos `data/`) en `<salida>/.sd_backup` y lo deja como estaba al terminar,
  también si falla. Instala `save/` (partida con el Mundo 1), `cache/shadercache.bin` y un `debug.toml` con
  `fps_log`, capturas cada 600 frames y el guion de la escena.
- Espera (máx. 3 min) a que el procesador esté por debajo de 65 °C y **fija los hilos de citron** (`PIN=1`):
  `CPUCore_1` (hilo del juego) en la CPU 0, `CPUCore_0` (hilo principal: render y GL) en la 1, sin nada de citron en
  sus hermanas de HT (5 y 2), `CPUCore_2/3` en 3-4, GPU/Vulkan en 6-7 y el resto (emulación de audio...) en los
  E-cores 8-15. Sin fijar, la misma prueba daba de 9 a 17 fps (ver `BASELINE.md`).
- Guiones: `title` = 2 hasta pasar la pantalla de la correa y luego nada (título a los ~35 s; a los 1000 frames
  sin tocar nada el juego pasa solo a la fiesta de Peach, MOVIE). `map` = 2 cada 20 frames hasta el mapa del Mundo 1
  y luego nada (mapa a los ~60-75 s). `lvl11` = lo mismo, 2 en el mapa (el cursor está en el 1-1) y dentro del nivel
  el guion de humo del PC (2 cada 120 frames, derecha 12 de cada 120): 1-1 a los ~90-100 s.
- Salida: `summary.txt` (fps por ventana de 5 s, % de CPU de cada hilo de citron y dónde corrió, ms de CPU del hilo
  del juego por frame, `[RPROF]` render_us/frame y draws, `[Gov]`, tabla por escena), `timeline.tsv`, `nwii.log`,
  `threads_raw.txt`, `shots/*.png`. Opciones: `EVERY` (capturas), `EXTRA_DEBUG` (líneas extra de `debug.toml`, p. ej.
  claves `NWII_*` de un frente), `CACHE`, `SAVE`, `COOL`, `PIN=0`.

## PC: perfil de una escena (`pc/prof.sh`)

```bash
~/nsmbw-port/r9/<frente>/pc/prof.sh <binario_input_test> <nombre> <map|lvl11> <segundos> [seg_perf=60] [espera=10]
```

Corre el binario sin ventana ni sonido (NAND privada `mp`, caché de shaders caliente, resolución 1x, perfil de CPU
`sw` = 2 núcleos LP-E con cuota del 150 %, como en `verify/integration_r8`) con el mismo guion de escena; cuando el
`[SCENE]` dice que llegó, espera y engancha `perf record` al juego. Deja `pc/prof/<nombre>.txt` (por hilo: reparto en
cubos y funciones más caras) y `pc/runs/<nombre>/` (`run.log`, `summary.txt`).

## Comprobado (31-oct)

Desde una copia `cp -a --reflink=always` de esta carpeta (borrada después):

- `build_pc.sh` (rápido) dio un `NSMBW` **idéntico byte a byte** a `recomp/bin/NSMBW_r8` (sha256 `a85cb185…5b7f18`), y
  con el objeto viejo de entrada (`gen/input_script_r8.o`) un `_input_test` idéntico a `recomp/bin/NSMBW_r8_input_test`
  (`c10e30c2…519d49`).
- `build_nx.sh` (rápido) dio un NRO igual a `switch/bin/nsmbw_r8_20261029.nro` salvo 44 bytes: la hora de compilación
  (`built … __TIME__`) y el `build-id` que depende de ella (en el ELF y en la cabecera del NRO).
- Los dos `_input_test` (PC y NRO) llevan `__wrap_NWII_KPADRead*` y llegan a las escenas (ver `BASELINE.md`).
