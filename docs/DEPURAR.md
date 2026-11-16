# Depurar y probar

## Switch: `debug.toml` (solo desarrollo)

Si existe `SD:/switch/NSMBWnx/debug.toml`, el `.nro` lo lee. El `.nro` nunca lo crea y no va en la release.
Sin ese archivo, el jugador solo tiene `config.toml`.
`logs/` aparece únicamente si hay un error (`nwii_err.log`) o si `debug.toml` activa un registro.

```toml
fps_log = true          # fps, memoria e hilos cada 5 s en logs/nwii.log
cpu_boost = true        # CPU a 1785 MHz (la GPU baja a 76,8 MHz)
shader_prewarm = false  # no precompilar los shaders guardados al arrancar
joycon_rotate = false   # no rotar los ejes del sensor de un Joy-Con suelto
NWII_XFBDUMP = "sdmc:/switch/NSMBWnx/shots/t"   # cualquier variable NWII_* tal cual
NWII_XFBDUMP_EVERY = 60
NWII_LOG = "nxlink"     # registro por red: nxlink -s NSMBWnx.nro
NWII_SHADER_SSO = 0     # R8: volver a los programas enlazados de r7b (si los separables fallan)
NWII_FIFO_MAX_FRAMES = "0"   # R8: quitar el límite de la cola de GX (vuelve el pico de memoria)
```

`debug.toml` pasa tal cual cualquier clave `NWII_*` y también `NV50_*`, `MESA_*` y `GALLIUM_*` (solo pruebas).
Un `0` se respeta en `NWII_HOST_FPS`, `NWII_SHADER_PREWARM`, `NWII_SHADERCACHE`, `NWII_NX_SIX_ROTATE`,
`NWII_SHADER_SSO`, `NWII_SHADER_PREWARM_DRAW` y `NWII_FIFO_MAX_FRAMES`; en las demás, `0` = sin definir.

En el código, el orden es: valores por defecto → `config.toml [switch]` → `debug.toml` → `env.txt` (antiguo).
Implementado en `load_switch_options()` de `nWiiRuntime/src/platform/switch/switch_platform.cpp`.

## PC: variables de entorno `NWII_*`

| Variable | Efecto |
|---|---|
| `NWII_FPS=1` | fps en la consola cada segundo (también salen en el título de la ventana) |
| `NWII_XFBDUMP=<prefijo>` `NWII_XFBDUMP_EVERY=<n>` | guarda un frame cada n (PPM) |
| `NWII_CAPTURE_AT=<n>` / `NWII_SHOT_AT=<n>` | lo mismo que F12 / F11, pero en el frame n (sin teclado) |
| `NWII_CAPTURE_DIR`, `NWII_CAPTURE_FRAMES`, `NWII_CAPTURE_SCALE` | carpeta, frames del búfer (0 = apagado) y escala de F12 |
| `NWII_SHADERCACHE=<archivo>` / `=0` | ruta de la caché de shaders / desactivarla |
| `NWII_REALTIME=1` | reloj real (por defecto en PC y Switch; `NWII_REALTIME=0` = reloj virtual) |
| `NWII_GOV=0` | apaga el regulador de sobrecarga (ver abajo) |
| `NWII_GOV_STARVE_MS=<ms>` / `NWII_GOV_MIN=<n>` | regulador: ms sin reposo antes de frenar (1000) y freno máximo 1/n (16) |
| `NWII_INTERNAL_RES=auto\|1..4` | resolución interna (EFB de 640·N x 528·N); gana a `[graphics] internal_resolution` del toml |
| `NWII_INTERNAL_RES_CYCLE=<n>` | pruebas: cambia la escala 1x→2x→3x→2x… cada n frames (prueba el cambio en marcha, como al conectar el dock) |
| `NWII_DUMP_SCALED=1` | `NWII_XFBDUMP` y F11 guardan a resolución interna (sin esto se guardan a tamaño nativo, 640 de ancho, para comparar con corridas viejas) |
| `NWII_GPUTIME=1` | tiempo real de GPU por frame: `[GPUTIME] scale=Nx avg=… max=… ms` cada 300 frames (también en Switch vía `debug.toml`) |
| `NWII_SHADER_SSO=1` / `=0` | programas separables (R8): el vertex shader se compila una vez y cada programa es solo su fragment shader. Por defecto **sí en Switch, no en PC**. En `debug.toml`, `NWII_SHADER_SSO = 0` vuelve al modo enlazado de r7b |
| `NWII_SHADER_PREWARM=0` / `NWII_SHADER_PREWARM_DRAW=0` | no precalentar la caché al arrancar / precalentar sin el triángulo de prueba de cada programa |
| `NWII_SHADER_BENCH=<n>` (+ `NWII_SHADER_BENCH_ONLY=1`) | diagnóstico: mide el coste de compilar n programas por cada camino (enlazado, separable…) |
| `NWII_FIFO_MAX_FRAMES=<n>` / `=0` | cola de GX acotada (R8): el juego espera en su copia al XFB si el host tiene n frames sin recoger (8 por defecto); su reloj se para mientras espera. `0` = sin límite (como r7b). También en `debug.toml` |
| `NWII_MEMTEST_SHADER_MS=<ms>` | solo pruebas: el host se detiene esos ms en cada shader nuevo (reproduce en el PC el atasco de la Switch) |
| `SDL_VIDEODRIVER=offscreen` | sin ventana; también fuerza audio `dummy` (sin sonido) |

Para ver todas: `grep -rho 'getenv("NWII_[A-Z0-9_]*' NWiiRecomp/nWiiRuntime/src | sort -u`.

## Regulador de sobrecarga (reloj real)

Con el reloj real, el audio (AX: ~1000 interrupciones por segundo), el VI y las alarmas van a tiempo real
aunque la CPU no dé abasto. Si el anfitrión es más lento que una Wii, ese trabajo puede ocupar todo el hilo
de la CPU: cada interrupción empalma con la siguiente y los hilos del juego no avanzan (0 fps para siempre).
El regulador (`governor_step` en `src/core/os_hle_stubs.cpp`) mide cuánto tiempo pasa el juego sin reposar
(bucle de reposo del SO o espera activa). Si pasa 1 s sin reposo, frena el reloj del juego (×3/4 cada
50 ms, hasta 1/16): el juego va más lento y el audio se corta, pero no se cuelga. En cuanto vuelve a reposar,
el reloj recupera velocidad: rápido hasta 3/4 de la velocidad a la que se atascó y luego despacio, para
quedarse justo por debajo de su límite; ese tope se olvida tras 20 s sin atascos. En un PC que va a 60 fps
el juego reposa cada frame (y menos de 0,5 s seguidos al arrancar), así que el reloj nunca se frena. Un
anfitrión lento que todavía reposa entre frames (título de Switch a 7-8 fps) tampoco se toca.

Las líneas `[FPS]` (PC, `NWII_FPS=1`) y `[NX] game … fps` (Switch, `fps_log`) terminan con
`busy max <ms>` (tramo más largo sin reposo en los últimos 5 s), `clock x<r>` (velocidad mínima del reloj),
`slip <ms>` (retraso acumulado) e `interp <n>/s` (llamadas al intérprete). Cada frenada deja una línea `[Gov]`.

## Resolución interna

`[graphics] internal_resolution = "auto" | 1..4` (como la "resolución interna" de Dolphin): el EFB de GL mide
640·N x 528·N y todo rectángulo GX se multiplica por N. Las texturas del juego siguen a su tamaño; se afilan
bordes, geometría, texto y efectos de copia EFB. A 1x el render es idéntico al de antes.

- PC, `auto`: la escala más pequeña cuyo EFB cubre la altura de la imagen 16:9 en la ventana
  (720p → 2x, 1080p/1440p → 3x, 4K → 4x). Sigue a la ventana: un cambio que dura 30 frames re-crea el EFB.
- Switch, `auto`: 1x en portátil y 2x en el dock (R8: 1x en portátil hasta medir 2x en la consola; la GPU
  portátil va a 307-384 MHz). `internal_resolution_handheld` fija la del modo portátil; cambia en marcha al
  conectar o quitar el dock.
- La línea `[GL] internal resolution Nx (EFB …)` del arranque dice la escala inicial.
- Para medir el coste en la consola: `debug.toml` con `fps_log = true` y `NWII_GPUTIME = "1"`.

## R8: shaders, memoria y código generado

- **Caché de shaders que se lleva.** `switch/NSMBWnx/cache/shadercache.bin` (de `r8/shaders/cache_switch/`):
  201 programas de título, menús, mapa, 1-1, historia 2J y Caza monedas, sin binarios (id de driver 0). Al
  arrancar el NRO compila el vertex shader compartido y precalienta todos (en citron ~1 s + ~7-8 s de pantalla
  negra, una vez por arranque). En `logs/nwii.log`: `separable programs: on`,
  `[SHADERCACHE] prewarm i/n (ms)`, `prewarm: … 0 failed in … ms` y, jugando, `shaders: new=0` en `[RPROF]`.
  `[RPROF] render_max_us=` es el frame más lento de la ventana. Unir cachés: `r8/shaders/tools/nsc2.py merge`.
- **Memoria del host.** Con `fps_log`, cada 5 s: `[NX] heap detail: arena …, in use …, free … (top …, holes …)`
  (mallinfo) y `[NX] mem: … backlog waits N (ms), trims N (MB) …` (cuánto guarda cada caché del host, cuántas
  veces esperó el juego a la cola acotada y cuánto se devolvió). En PC, la línea `[MEM]` tras cada `[FPS]`.
- **Código generado podado.** `recomp/build.sh regen` aplica, después de `apply_fastdispatch.py`,
  `perf/apply_r8_codegen.py --prune-only --memview`: deja en el `switch` de entrada de cada función solo las
  direcciones en las que algo puede reanudar y lee las bases de memoria una vez por función (`MemView`). Si el
  juego entra en una dirección podada, se interpreta igual que antes y sale `[R8] resume_pruned #n pc=…` en el
  log (las 16 primeras y luego solo cuando el contador es potencia de 2, así que el `#n` da el total). Conviene
  buscar esas líneas en partidas largas o de otros mundos: en las pruebas de R8 (PC y citron) solo salen
  `0x801b119c`, `0x801b116c` y `0x801d0c10`, direcciones que r7b ya interpretaba igual. Una dirección nueva no
  rompe nada (se interpreta), pero es un punto de reanudación que la pasada no vio.

## Capturas F11 / F12 (PC)

- **F12**: el juego guarda siempre en memoria los últimos ~120 frames (~2 s). Al pulsar F12 **después** de ver
  un fallo, los escribe en `capturas/<fecha_hora>/` como PNG (a mitad de tamaño). Incluye `log.txt` con lo que
  pasó en cada frame: draws, copias EFB, subidas de texturas, shaders nuevos e ids de programas GL.
- **F11**: una captura a tamaño completo más su registro.
- `capturas/` se crea junto a la carpeta de partidas (la NAND).

## Pruebas sin ventana ni sonido

```fish
cd <carpeta con un nsmbw.toml que use una NAND propia>
SDL_VIDEODRIVER=offscreen NWII_FPS=1 NWII_XFBDUMP=out/t NWII_XFBDUMP_EVERY=600 \
  ~/nsmbw-port/scripts/lim timeout 300 ./NSMBW ~/nsmbw-port/game/SMNE01 nsmbw.toml > run.log 2>&1
```

- Con entrada guionizada: los binarios `*_input_test` leen `NWII_INPUT_SCRIPT`, código en `cpu/tools/input_script.cpp`.
  Ejemplo para entrar al 1-1 desde una partida nueva: `'0-10000000:100:120:4;60-10000000:4:120:12'`.
- Desde R9 (`r9/base/tools/input_script.cpp`, mismo formato): también hay NRO `*_input_test.nro` (las claves van en
  `debug.toml`), una línea `[SCENE]` cada vez que cambia la escena del juego (título, mapa, nivel...) y fases por
  escena (`NWII_INPUT_PHASES = "3>5"` + `NWII_INPUT_SCRIPT_1`, `_2`...). `r9/base/citron_scene.sh <nro>
  <title|map|lvl11> <segs> <salida>` llega en citron al título, al mapa del Mundo 1 o al 1-1 en 30-100 s y mide fps,
  % de CPU por hilo de citron, `[RPROF]` y `[Gov]`. Detalles en `r9/base/README.md`.
- `scripts/agentes` muestra los agentes de IA que trabajan en el proyecto: qué hace cada uno, tiempo, último paso y % de entregables. Las tareas se describen en `status/agentes_tareas.json`.
- `scripts/estado.sh` muestra qué está corriendo. Para verlo en vivo: `watch -n 5 scripts/estado.sh`.
- `scripts/lim` ejecuta cualquier comando dentro de `nsmbw.slice` (systemd), con un tope de 6,5 GB de RAM (frena desde 5,75 GB; 2 GB de swap). Las compilaciones y pruebas de citron (el 2.º turno solo con ≥5 GB libres; `build*.sh`, `citron*.sh`, `ninja`, `make`, `cmake`) van de una en una (`LIM_BUILDS=2` permite dos si hay ≥5 GB libres); las demás esperan turno. `switch/citron_test.sh` también pide uno de esos turnos (citron ocupa ~8 GB).

## Dolphin como referencia 1:1

`oracle/` usa Dolphin como referencia: Dolphin sin ventana con stub GDB y una carpeta de usuario privada, más
herramientas para comparar registros, memoria y frames contra el port. Ver `oracle/README.md` y
`oracle/FINDINGS.md`.
