# r21s: escenas repetibles para medir (frente S, 9-nov-2026)

Copia de **m1** (`r9/r18m`) con un añadido solo de runtime, **apagado por defecto**: sin estas variables el
juego hace exactamente lo mismo que m1. Sin commits. No se tocó ninguna carpeta de otro frente.

## Para qué

La pantalla de título pone demos al azar (el juego baraja el orden con la hora al arrancar). Dos corridas
enseñaban escenas distintas y cazar un fallo de una demo concreta costaba 5-6 intentos. Además las pruebas
guionizadas pasan por la selección de archivo y podían escribir la partida del usuario.

## Opciones (debug.toml en la Switch; variables de entorno en PC)

| Variable | Qué hace |
|---|---|
| `NWII_FORCE_DEMO = "a,b,c"` | Lista **ordenada** de demos (nombres de la tabla, índices 0-15 o nivel `2-1`). Se ponen en ese orden; al acabar la lista vuelve a empezar. Una sola = siempre esa. |
| `NWII_FORCE_DEMO_REPEAT = "N"` | Cada elemento de la lista N veces seguidas (1 por defecto). |
| `NWII_FORCE_DEMO_LOOP = "1"` | Sin los ~16 s de la pantalla de título entre demos: al acabar una demo empieza la siguiente (~2 s de carga). La cinemática `castillo` sí pasa por el título (sale de ahí). |
| `NWII_DEMO_LOG = "1"` | Solo escribe las líneas `[DEMO]` (sin forzar nada). Con `NWII_FORCE_DEMO` ya van solas. `"2"`: además el estado crudo (para depurar esto). |
| `NWII_NO_SAVE = "1"` | **La partida no se escribe nunca.** Al arrancar se copia la carpeta de la NAND (`save/` en la Switch) a `save_prueba/` (se rehace en cada arranque) y el juego lee y escribe ahí: cree que guardó bien; la real no se toca. Nada más cambia (mismo contenido). |
| `NWII_FORCE_LEVEL = "1-1"` | Arranca directo en ese nivel (modo normal, jugador 1 = Mario con 5 vidas) en vez de la pantalla de título. Formatos: `1-1`…`8-7`, `3-T` (torre), `6-C` (castillo), `5-G` (casa fantasma). **Lleva siempre `NWII_NO_SAVE`.** Al morir o acabar, el juego sigue solo (al mapa). |

Líneas en `nwii.log` (para cortar fps, hilos y perfiles por escena; segundos desde que arranca el runtime):

```
[DEMO] start desierto4j at 119.5
[DEMO] end desierto4j at 164.4 (44.9 s)
[DEMO] lista completa; vuelve a empezar
```

`start/end` se escriben siempre que la opción esté activa, también para demos que el juego eligió solo.

## Tabla de demos (IDs estables)

El índice es el valor de la tabla del juego (`dScStage_c::m_titleRandomTable`) y de `c_stage` en
`dScRestartCrsin_c::startTitle`. Comprobado en PC con capturas (`runs/todas_loop/demos_por_id.jpg`, en el orden de la tabla).

| # | ID | Nivel | Qué se ve |
|---|---|---|---|
| 0 | `agua_pinguino` | 1-4 | submarina, traje de pingüino (bolas de hielo) — la de los polígonos estirados de deko3d |
| 1 | `pradera` | 1-6 | colinas verdes |
| 2 | `desierto4j` | 2-1 | desierto con pirámide y géiser de arena |
| 3 | `desierto_helices` | 2-4 | desierto con bloques y trajes hélice |
| 4 | `nieve` | 3-1 | bosque nevado |
| 5 | `nubes_plataformas` | 3-5 | cielo de nubes grises, setas rosas que giran |
| 6 | `torre3` | 3-T | torre del mundo 3 |
| 7 | `playa_hielo` | 4-1 | playa medio sumergida, enemigos en bloques de hielo |
| 8 | `playa_yoshi` | 4-5 | playa con Yoshi, Lakitu y setas rosas |
| 9 | `jungla` | 5-4 | jungla con pantano morado |
| 10 | `cielo_hoja` | 5-5 | cielo con hoja voladora |
| 11 | `casa_oscura` | 5-G | casa fantasma (oscura) |
| 12 | `castillo6` | 6-C | castillo del mundo 6 (lava) |
| 13 | `cielo_paratroopas` | 7-6 | cielo con Para-Beetles rojos (caparazones voladores) |
| 14 | `lava` | 8-7 | cueva de lava |
| 15 | `castillo` | — | cinemática del título (castillo de Peach, la tarta; escena MOVIE) |

Duraciones en PC: demos 43-50 s, cinemática ~57 s, pantalla de título ~16 s, carga entre demos ~2 s.

### Lista por defecto para comparar compilaciones

```toml
NWII_FORCE_DEMO = "desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo"
NWII_FORCE_DEMO_LOOP = "1"
NWII_NO_SAVE = "1"
```

Con LOOP una vuelta dura ~4 min: título (16 s) → desierto4j → agua_pinguino → cielo_paratroopas →
nubes_plataformas → título (16 s) → castillo → título → desierto4j... (la primera pantalla de título del
arranque siempre se ve).

## Cómo funciona (y por qué no cambia nada más)

Del decomp (`d_s_restart_crsin_static.cpp`) y del código generado:

- `startTitle(isReplay)`: la demo es `c_stage[m_titleRandomTable[m_titleCount]]`; la tabla (15 entradas) se
  baraja **una vez** al arrancar (`setTitleReplayRandomTable`, desde `dScBoot_c::create`); `m_titleCount`
  sube al empezar cada demo (`dScStage` create, modo TITLE_REPLAY).
- En la pantalla de título, tras 900 frames: si `contador % 4 != 0` → demo (`startTitle(1)`), si no → la
  cinemática (escena MOVIE). El contador (0x8042A1F4, va de 0 a 19) sube en cada pantalla de título.
- Una demo acaba a los 2400 frames con `startTitle(0)` (vuelta al título).

El añadido (`nWiiRuntime/src/core/force_demo.cpp`) corre en el hilo de la CPU, en el evento de VI (60 veces
por segundo de tiempo del juego) y solo:

1. escribe la siguiente demo de la lista en **la entrada de la tabla que se va a leer** (`tabla[m_titleCount]`),
   y solo después de que el juego la haya barajado (no cambia cuántos números aleatorios se piden);
2. en la pantalla de título, ajusta el contador demo/cinemática para que salga lo que toca;
3. con LOOP, cuando la demo acaba por tiempo, cambia lo que escribió `startTitle(0)` por lo que habría escrito
   `startTitle(1)` (las dos ramas solo difieren en 6 campos: mundo, nivel, modo, `mIsReplay`; el fundido y el
   cambio de escena son los mismos). Si se sale de la demo con un botón, no se toca;
4. con `NWII_FORCE_LEVEL`, lo mismo con el primer `startTitle(0)` del arranque → nivel en modo normal, y
   apunta al jugador 1 (`daPyMng_c::mPlayerEntry[0]=1`, `mPlayerType[0]=Mario`, vidas 5 si eran 0), que es lo
   que deja la selección de archivo con 1 jugador.

`ios.cpp` solo añade `if (g_force_demo_on) force_demo_tick(c);` en el evento de VI (un bool, falso sin
variables). `NWII_NO_SAVE` está en `hle/devices/fs_device.cpp` (al iniciar la NAND).

Direcciones (USA, SMNE01; r13 = 0x8042F680): tabla 0x803741BC, `m_titleCount` 0x8042A1F8, contador de
título 0x8042A1F4, frames de la escena 0x8042A200, `m_gameMode` 0x8042A204, `startGameInfo` de
RESTART_CRSIN 0x80373D60, escena actual 0x80428732, `c_stage` 0x802F49D0, `mPlayerEntry` 0x80354E50,
`mPlayerType` 0x80354E60, `mRest` 0x80354E90.

## Parche para otras compilaciones (deko3d m1dk, etc.)

`forcedemo.patch` = diferencia de `r18m/nWiiRuntime` → `r21s/nWiiRuntime` (4 archivos: `CMakeLists.txt`,
`src/core/force_demo.cpp` nuevo, `src/hle/ios.cpp`, `src/hle/devices/fs_device.cpp`). Se aplica desde la
carpeta `nWiiRuntime` de cualquier runtime basado en m1:

```
cd <variante>/nWiiRuntime && patch -p2 < ~/nsmbw-port/r9/r21s/forcedemo.patch
```

Comprobado con `--dry-run` sobre `r15dk/m1dk/nWiiRuntime`, `r15dk/nWiiRuntime` y `r15dk/r5dk/nWiiRuntime`:
entra limpio. No toca cabeceras que incluya el código generado (se puede compilar solo el runtime).

## Compilación

```
cd ~/nsmbw-port/r9
env BASE=/home/djvemo/nsmbw-port/r9/r14g5 JOBS=2 ~/nsmbw-port/scripts/lim r21s/tools/build_m.sh r21s s1 pc   # bin/NSMBW_s1 (+_input_test)
cd r21s && JOBS=2 ~/nsmbw-port/scripts/lim tools/build_s.sh s1                                              # Switch
```

`build_s.sh` = `build_m.sh … nx` con libdeko3d de release (como m1) + re-enlace con **el mismo orden L de m1**
(`nx/hot/order/m1_dk3drel.txt`; las funciones calientes quedan en las mismas direcciones que en m1, comprobado
con `nm`). El código nuevo queda al final (frío).

## Binarios (`bin/`)

| Archivo | Qué es |
|---|---|
| **`nsmbw_s1.nro`** (+ `.elf`) | el juego = m1 + estas opciones; arranca con GL (`NWII_BACKEND = "deko3d"` en debug.toml lo pasa a deko3d). Sin variables se comporta como m1 |
| `nsmbw_s1_input_test.nro` | solo pruebas con entrada por guion. **Nunca como juego** |
| `sin_orden/` | lo mismo sin el orden L |
| `NSMBW_s1`, `NSMBW_s1_input_test` | PC |

sha256: `runs/bin_sha256_s1.txt`.

## g6e1s: G6 etapa 1 con el runtime de s1

Pedido por el coordinador (la sesión de G6 está cerrada). `tools/build_g6e1s.sh` (solo enlaza):
la línea de enlace de G6 (`r20g6/nx_link_g6e1.cmd` = la de m1 con los 87 objetos de `r20g6/e1/nx_obj`) con
las 3 piezas del runtime de m1 (main.o, libnwiiruntime.a, libglad.a) cambiadas por las de s1 (sin los
output_*.o queda idéntica a la de s1), y el orden L de G6 (`r20g6/nx/hot/order/g6e1.txt`, copiado). Mismo
orden de símbolos que `r20g6/bin/nsmbw_g6e1_hot.elf` salvo las funciones nuevas de force_demo. En PC lo mismo con
`pc_link_g6e1.cmd`. Nada se escribió en r20g6.

- `bin/nsmbw_g6e1s.nro` (+ `.elf`): el juego (GL); `bin/nsmbw_g6e1s_input_test.nro` solo pruebas;
  `bin/NSMBW_g6e1s(_input_test)` PC. sha256: `runs/bin_sha256_g6e1s.txt`.
- **g6e2s** (G6 etapa 2, verificada por G6/coordinador) igual: `tools/build_g6s.sh e2` (el mismo guion sirve para
  e1) → `bin/nsmbw_g6e2s.nro` (+ `.elf`, `_input_test.nro`, `NSMBW_g6e2s`), orden `r20g6/nx/hot/order/g6e2.txt`.
  Frente a `nsmbw_g6e2_hot.elf` el orden de símbolos solo cambia en lo nuevo del runtime (force_demo y la copia
  de NO_SAVE). sha256: `runs/bin_sha256_g6e2s.txt`.

## Verificación en PC

Herramientas: `tools/demo_run.sh` (corrida del título con variables, deja `runs/<nombre>/demo.txt` y
`hoja.jpg`), `tools/nosave_test.sh` (comprueba `NWII_NO_SAVE` dentro del mismo `.runlock`).

| Prueba | Resultado |
|---|---|
| Título natural con `NWII_DEMO_LOG=2` (240 s, `runs/natural/`) | el juego eligió solo (movie, 1-6, 2-1, 4-5); de ahí salen el contador demo/cinemática y los tiempos |
| Las 16 escenas en orden con LOOP (800 s, `runs/todas_loop/`) | las 16 en el orden pedido, 0 avisos, "lista completa" al final; 59,4 fps, FASTMEM 0 fallos. `demos_por_id.jpg` = una captura por ID |
| `NWII_FORCE_LEVEL=1-1` (90 s, `runs/nivel11/`) | arranca en 1-1 con Mario (5 vidas); al morir sigue al mapa del Mundo 1 |
| `NWII_NO_SAVE` arranque, NAND sin partida (`runs/nosave_fresh/`) | NAND real idéntica a su plantilla; lo que escribe el arranque (play_rec.dat, dvderror.dat) va a la copia |
| `NWII_NO_SAVE` + selección de archivo → mapa → 1-1, NAND con partida (`runs/nosave_mp/`) | el juego **guardó de verdad** (crea /tmp/sys/wiimj2d.sav, escribe, renombra encima de la partida, y banner.bin): todo en la copia; la NAND real idéntica a su plantilla |
| `checks.sh` 1x con las opciones apagadas (FRAMECHECK + ECACHE, `runs/checks_s1.txt`) | title, map, walk, lvl11: **220/220 idénticas cada una, `diff_lines=0`**, mismas escenas que m1 (el "pass1 differs" varía de una corrida a otra también en m1/G6/N) |
| g6e1s en PC con la lista por defecto + LOOP + NO_SAVE (300 s, `runs/g6e1s_lista/`) | desierto4j → agua_pinguino → cielo_paratroopas → nubes_plataformas → título → castillo, "lista completa"; 59,7 fps, FASTMEM 0 fallos; imagen bien (`hoja.jpg`) |

Sin consola: la consola la mide la sesión principal.

## Avisos

- Las direcciones son de la versión USA (SMNE01); con otra versión del juego las opciones no funcionarían
  (y escribirían en sitios equivocados): no usarlas con PAL/JPN.
- `NWII_FORCE_LEVEL` no pasa por la selección de archivo: el mapa al que se vuelve usa la partida cargada al
  arrancar (por eso va siempre con `NWII_NO_SAVE`).
- La semilla del juego sale de la hora: lo que hacen los enemigos al azar puede variar algo entre corridas aunque
  la demo (entradas grabadas) sea la misma.

