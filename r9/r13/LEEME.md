# r13 — todo junto: fastmem (r12fm) + render en 2 núcleos (A, r12a) + vértices en la GPU (B, r11b)

8-nov-2026. Sin commits. Base: `r9/r12fm` (= r10c + fastmem, con su `export/` ya pasado por la pasada fastmem).

## Qué hay dentro

| Parte | De dónde | Interruptor |
|---|---|---|
| fastmem (cabecera `cpu_context.h` + `memory.cpp` + `fastmem_impl.inc` + `a64_decode.h`; código generado con la pasada) | `r9/r12fm` | de compilación (`-DNWII_FASTMEM=1` en `cmake/nsmbw-switch.cmake` y `build_nx.sh`; en PC por defecto). No hay interruptor en tiempo de ejecución |
| A: worker de render (núcleo 2 en la Switch) sobre el runtime de la consola (r10c: arreglo del iris + mandos) | `r9/r12a` (`r12a_vs_r10c.patch`) | `NWII_RENDER_WORKER=0` |
| B: transformación de vértices y texgen en la GPU (+ autoprueba `[GPUXF]`) | `r9/r11b` (`renderer_gl.cpp`, `tev_shader_gen.cpp`, `tev_shader_gen.h`) | `NWII_GPU_XF=0` |

Binarios (`bin/`): **`nsmbw_r13.nro`** + `nsmbw_r13.elf` (Switch), `nsmbw_r13_input_test.nro` (solo pruebas),
`NSMBW_r13`, `NSMBW_r13_input_test` (PC). Las dos compilaciones son **completas** (FULL=1, JOBS=3; Switch ~13 min,
PC ~13 min; registros `build_nx_full.log`, `build_pc_full.log`, 0 errores).
Tabla de excepciones de fastmem (`tools/check_fm_tab.py`): Switch 681 661 entradas, PC 682 614, **0 mal formadas**
(lo mismo que r12fm).

En la consola: `SD:/switch/NSMBW_prueba_r13/NSMBW_prueba_r13.nro` ("NSMBW PRUEBA r13 (todo junto)"); primero se instaló
esta compilación (bit a bit) y luego otro agente la cambió por **r13b** (ver la sección r13b). Copia congelada con el `.elf` para simbolizar perfiles:
`switch/real_builds/r13_1108/` (`SHA256SUMS`: nro `f2f99681…`, elf `dd2a4660…`).

## Cómo se juntó (notas de la fusión)

- Copia de r12fm (`cp -a --reflink=auto`): `nWiiRuntime`, `export/` (sin `build/` ni `export/nWiiRuntime`),
  `cmake/`, `build_*.sh`, `tools/`, `sites.tsv`, `test/{build_unit.sh,fm_unit.cpp}`. Carpetas de compilación nuevas
  (`nx/build`, `export/build`).
- Comprobado antes: r11base = r10c en los tres archivos de B; fastmem solo cambia `cpu_context.h` y `memory.cpp`
  (+ 2 nuevos), que **ni A ni B tocan**; los 8 archivos de A en r12fm = los de r10c.
- A: el parche no se aplica con `patch` (rutas de un directorio temporal), así que se copiaron los 7 archivos de
  `r9/r12a/nWiiRuntime` que solo cambia A (`fifo_parser.{h,cpp}`, `gx.cpp`, `main.cpp`, `switch_platform.{h,cpp}`,
  `switch_profiler.cpp`): equivale exactamente al parche. B: copiados `tev_shader_gen.{cpp,h}` de r11b.
- `renderer_gl.cpp`: fusión de 3 vías (`git merge-file`, base r10c, A = r12a, B = r11b). Dos conflictos, los dos
  donde se cruzan:
  1. **`ecache::Entry::~Entry`**: A cambió `vbuf::ArenaFree` por `vbuf::ArenaFreeAny` (la entrada puede morir en el
     worker al destruirse sus DecodedVerts, y entonces el hueco se devuelve en el hilo de render en el siguiente
     `Render()`); B añadió `xf::ArenaFree(x_slot)` (hueco de la arena de vértices de GPU XF). **El hueco de B tiene
     el mismo problema** (las listas de `xf` no son de varios hilos), así que se generalizó lo de A:
     `ArenaFreeAny(off, cls, gpu_xf)` y la cola diferida guarda de qué arena es; `TakeDeferred()` llama a
     `ArenaFree` o a `xf::ArenaFree`. El destructor llama `ArenaFreeAny` para los dos huecos.
  2. **Inicio de `Render()`**: A llamaba `vbuf::TakeDeferred()` solo dentro de `if (vbuf::s_on)`; con GPU XF el
     vbuf no se crea, así que ahora `TakeDeferred()` va **siempre**, antes de `xf::BeginFrame()` / `vbuf::Poll()`
     (así lo diferido se retira con el `s_frame` actual y se recicla cuando su valla termina: nunca antes de tiempo).
- Nada más se cruzaba: B no copia ni destruye `HostPtr` (regla de A: solo el worker); la entrada de la caché
  (`emit_cache`) la crea el hilo de render como antes y `xf::Emit`/`Analyze` corren en el hilo de render.
- `tools/` es copia de r12fm (algunos scripts siguen apuntando a `r12fm`). Nuevo: `tools/verify_r13.sh`.

## Verificación en PC (Intel Arc, Mesa; arnés compartido `r9/render/pc/run.sh` bajo `flock .runlock`)

| Prueba | Resultado |
|---|---|
| Humo título → menús → mapa → 1-1, 165 s (`verify_r13.sh sanity`) | llega al 1-1 (STAGE), 59,6 fps, sin cuelgues; `[GX] render worker: on`; `[GPUXF] self-test … mismatches 0 -> ok`, `cpu=0`; **`[FASTMEM] 0 faults, 0 sites, 0 unhandled`** |
| `RESS=1 test/checks.sh` (FRAMECHECK: cada 30 frames el frame con el camino r13 y con el r8 de CPU, píxel a píxel) | title 220/220, map 100/100, walk 60/60, lvl11 140/140: **diff_lines=0, mismatches=0** en las 4; worker on, GPUXF ok, 0 fallos fastmem en las 4 (`runs/checks_1x.txt`). *Menos comparaciones que en r12a (220 c/u) porque a mitad de la tanda el portátil pasó a batería y a `power-saver` (mapa 26 fps, paseo 18 fps con FRAMECHECK); con `performance` las demás corridas van a 59-60 fps.* **Repetición con `performance`** (`runs/checks_1x_repeat.txt`; corridas `chk_r13b_*` del arnés = el mismo binario PC de r13): map **220/220** y walk **220/220**, 59,2 fps, diff_lines=0, 0 fallos fastmem. |
| Iris del título (como r11d: fases 6, volcado cada 2 frames desde el 330), worker on y off | **correcto en los dos**: punto → agujero que crece con cielo y escenario → el negro translúcido desaparece. `wipe_r13_worker_on.png`, `wipe_r13_worker_off.png` (capturas en `runs/iris_w1`, `runs/iris_w0`) |
| Larga: mapa + 1-1, 300 s (`long`) | 59,8 fps (mín. 52), sin parones (`longest_no_frame=0`), 0 fallos fastmem, GPUXF arena 620 404/1 572 864, `full 0` |
| Coin Battle 4 jugadores, 200 s (`cb4`) | llega al nivel (MULTI_PLAY_COURSE_SELECT → STAGE), 59,6 fps, 0 fallos fastmem, GPUXF `cpu=0` |
| Los dos interruptores apagados (`NWII_RENDER_WORKER=0 NWII_GPU_XF=0`, `off`, 120 s) | llega al 1-1, 59,4 fps, `worker: off`, `[GPUXF] off`, 0 fallos fastmem |

Resultados de texto: `runs/*.txt`. En PC con 60 fps no se ve la ganancia (va como CPU libre: hilo del juego 17-19 %,
worker 5-6 %); la medida que importa es la de la consola.

## r13b (lo hizo OTRO agente, 17:48; no esta verificación)

La NRO de r13 **se cerró en la consola tras 37 min** (`operator new` → `bad_alloc` → abort): con el worker un hilo
crea comandos GX y otro los libera, y el asignador rápido de la Switch (`switch_alloc.cpp`) los metía en la lista del
hilo que libera, que nunca los reusa → fuga sin límite. Otro agente lo arregló en esta misma carpeta
(`nWiiRuntime/src/platform/switch/switch_alloc.cpp`: lista compartida por clase para liberaciones de otro hilo;
también `switch_input.cpp`: sin selector al volver a acoplar los Joy-Con), recompiló `nx/build` (`build_r13b.log`) e
instaló **r13b** en `switch/NSMBW_prueba_r13/` (copia en `switch/real_builds/r13b_1108/`). `bin/nsmbw_r13.nro` y
`switch/real_builds/r13_1108/` son el r13 **original, con la fuga: no usar**. Las pruebas de PC de arriba no pueden
ver esa fuga (en PC no se usa ese asignador; los dos archivos solo se compilan para la Switch), así que valen igual
para r13b en todo lo demás. **Ojo:** r12a (A solo) usa el mismo asignador con el worker: debería tener la misma fuga.

## Cómo probar en la consola (A/B)

Abrir "NSMBW PRUEBA r13 (todo junto)" desde el hbmenu (usa partida, config y logs de `switch/NSMBWnx/`; `logs/nwii.log`
se reescribe en cada partida). Para comparar, en `SD:/switch/NSMBWnx/debug.toml`:

```toml
NWII_RENDER_WORKER = "0"   # apaga A (render en un solo núcleo)
NWII_GPU_XF = "0"          # apaga B (vértices en la CPU como antes)
```

(quitar las líneas o comentarlas para volver). fastmem no se puede apagar: para comparar sin él, usar
`NSMBWnx_r12a.nro` / `NSMBW_prueba_r12a`. Qué mirar en `nwii.log`: `[GPUXF] self-test …` (si sale `off`, la GPU no
pasó la autoprueba y va por la CPU), `[GX] render worker: on`, en `fps_log` "gxw x%" y `[RPROF] retracts=/held=`.
`[FASTMEM] dump` solo sale al salir del juego. Simbolizar perfiles con `switch/real_builds/r13_1108/NSMBW_prueba_r13.elf`.

## Compilar de nuevo

`cd ~/nsmbw-port/r9/r13 && FULL=1 JOBS=3 ~/nsmbw-port/scripts/lim ./build_nx.sh bin/nsmbw_r13.nro` y
`FULL=1 JOBS=3 ~/nsmbw-port/scripts/lim ./build_pc.sh bin/NSMBW_r13` (incremental si solo cambia el runtime: el
mismo comando, ninja recompila lo justo). Pruebas: `tools/verify_r13.sh sanity|long|cb4|iris|off` y
`cd ../render && flock pc/.runlock env RESS=1 test/checks.sh ../r13/bin/NSMBW_r13_input_test r13`.
