# r17pgo: optimizar el código generado con el compilador (frente F)

8-nov-2026. Sin commits. Base: **r13b** (runtime `r9/r13/nWiiRuntime` con el arreglo del asignador + código
generado `r9/r13/export`, copiados aquí sin cambios). No se cambia lo que hace el juego: solo cómo compila GCC.

## Resumen (9-nov 03:50)

- **PGO = perfil real de la consola → GCC ordena ramas, integra y desenrolla según lo que de verdad se ejecuta.**
  Funciona en la Switch (libgcov por newlib a la SD, con dos arreglos propios: volcado explícito y TLS).
- **r13b**: hilo del juego −11 a −18 % (castillo 15,8 → 14,0 ms; título −17 %; perfil completo 9,65 → 7,92 ms).
  Con el orden L encima, castillo 13,6 ms (L suma ~3 %). `-O3` solo: −3 % (casi todo el mezclador AX); `-O3`
  encima de la PGO: nada. `-mcpu`, `-fno-semantic-interposition`: mismo código; `-fno-plt`: peor.
- **m1**: título −16 % en el hilo del juego (9,9 → 8,3 ms). **Solo el código generado debe llevar perfil**: con
  el runtime también, el worker de render salió 36 % más lento (el perfil se toma con el juego lento y aprende
  colas vacías). Con solo código generado + L: hilo del juego −10 a −15 % en el mapa con guion, fps igual que m1
  (castillo y mapa los limita el worker de render del núcleo 2, que con PGO trabaja algo más: 84 % vs 71-77 %).
- La PGO es el **último paso de cada release** (cada cambio del código generado invalida el perfil):
  `./pgo_release.sh prep <nombre> --export … --runtime …` → 2 entrenamientos en la consola →
  `./pgo_release.sh finish <nombre> <carpetas> --order …`.
- La PGO solo acelera lo entrenado: entrenar largo (900 s del título con varios demos jugables) + juego con guion.

## Qué es la PGO (en dos líneas)

PGO = "optimización guiada por perfil". 1) Se compila una NRO **instrumentada**: cada salto del código lleva un
contador. 2) Se juega con ella en la consola y se guardan los contadores (archivos `.gcda`, uno por cada `.cpp`).
3) Se compila otra vez con esos contadores: GCC sabe qué ramas se toman de verdad. Así ordena los bloques para que
el camino caliente vaya seguido, aparta lo frío (`.text.unlikely`), integra (inline) las llamadas calientes y
desenrolla solo los bucles calientes. El resultado hace exactamente lo mismo, solo que más rápido.

## Archivos

| Archivo | Qué es |
|---|---|
| **`pgo_release.sh`** | **la PGO de una release en dos órdenes** (`prep` / `finish`, ver "PGO de una release") |
| `pgo.sh` | los tres pasos sueltos: `gen`, `import`, `use` (con `CONF=<nombre>` toma `conf/<nombre>.env`) |
| `conf/*.env` | qué árbol compilar (`TREE`), librerías extra (deko3d), verificador de fastmem |
| `m1src/` | copia exacta de los fuentes de m1 (r18m) con la que se entrenó |
| `tools/order_relink.sh` | orden L encima de una compilación (re-enlace, sin recompilar) |
| `tools/compare.py` | ms del hilo del juego por frame, ventana a ventana, entre corridas de `bench_title.sh` |
| `tools/check_fm_tab_g5.py` | verificador de la tabla de fastmem (copia del de G5; vale también para r13) |
| `build_pgo.sh` | una compilación completa para la Switch con unos flags dados (`nx/<variante>` → `bin/<variante>.nro` + `.elf`) |
| `cmake/pgo.cmake` | se engancha al CMake compartido (`switch/nsmbw`) sin tocarlo (`CMAKE_PROJECT_nsmbw_switch_INCLUDE`): flags por objetivo/archivo |
| `cmake/nsmbw-switch.cmake` | copia sin cambios de la de r13 |
| `pgo/gcov_nx.cpp` | solo en la NRO instrumentada: el hilo que vuelca los `.gcda` a la SD y el arreglo del TLS de libgcov |
| `tools/pull_gcda.sh` | baja por FTP una carpeta de `.gcda` de la consola |
| `tools/tu_peak.sh` | mide la RAM máxima al compilar un solo `.cpp` |
| `tools/test_nro.sh`, `tools/citron_pgo.sh` | NRO de pruebas para citron (con entrada guionizada) y la escena de citron que conserva los `.gcda`. **Solo citron, nunca para la consola como juego** |
| `profiles/<nombre>/` | perfiles ya juntados, listos para `pgo.sh use` |
| `logs/build_<variante>.log` | registro de cada compilación |


## Cómo se hace (el método, reutilizable)

### 1. NRO instrumentada: `./pgo.sh gen`

`build_pgo.sh pgogen` con `PGO=gen`. Flags (puestos por `cmake/pgo.cmake`):

- código generado (`output_*.cpp`, `fastdispatch.cpp`, `main.cpp` del runtime):
  `-fprofile-generate=/pgo -fprofile-prefix-path=<carpeta de compilación> -fprofile-update=single`.
  Solo lo ejecuta el hilo del juego (núcleo 1), así que los contadores normales bastan (rápidos).
- runtime (`libnwiiruntime.a`): lo mismo con `-fprofile-update=atomic`. Lo usan varios hilos (render, worker de
  render, AX, audio): contadores atómicos (LDXR/STXR en el A57, sin llamadas) para que no se pisen.
- **`main_output.cpp` (= `run_game`) NO se instrumenta** ni se compila con perfil (`NWII_PGO_SKIP`): es una sola
  función con un `switch` de 168 000 casos y un `setjmp`. Compilarla normal ya pide **3,5 GB** de RAM; instrumentada
  pasa de 5 GB (el 8-nov a las 19:50 eso, junto con otros 2 compiladores, tumbó el PC). Lo único caliente de
  `run_game` es el bucle de dispatch, así que perderla del perfil no importa.
- RAM medida por archivo (`tools/tu_peak.sh`, pico de RSS): `output_22.cpp` (el más grande) 0,86 GB normal y
  0,83 GB instrumentado. `build_pgo.sh` compila `main_output.o` primero y **solo**, luego el resto con `-j1` en la
  instrumentada (`-j3` en las normales), y antes de cada paso pesado espera a tener ≥ 7 GB libres.
- Sin ccache (el nombre del `.gcda` sale de la ruta del objeto y ccache compila en otra ruta).

Arreglos que necesita la Switch (en `pgo/gcov_nx.cpp`, que **solo** entra en la instrumentada):

- **Volcado**: al cerrar desde HOME el proceso muere sin `exit()`, así que libgcov nunca escribiría nada. Un hilo
  de baja prioridad en el núcleo 2 llama a `__gcov_dump()` + `__gcov_reset()` según `debug.toml`:
  `NWII_GCOV_DUMP_EVERY = "60"` (cada 60 s; cada volcado suma lo nuevo a los archivos) y/o
  `NWII_GCOV_DUMP_AT = "300"` (último volcado a los 300 s y para). Los segundos cuentan desde que arranca la NRO.
  Escribe en `sdmc:/switch/NSMBWnx/gcda/<AAMMDD_HHMMSS>/` (una carpeta por arranque, para que dos sesiones no se
  mezclen sin querer) y deja `dumps.txt` y una línea `[GCOV] dump …` en `logs/nwii.log` por cada volcado.
  Rutas: libgcov usa `GCOV_PREFIX`/`GCOV_PREFIX_STRIP` (puestas en un constructor, antes de que exista ningún hilo).
  libgcov de devkitA64 no crea carpetas: las crea el hilo. No hay bloqueo de archivos (no hace falta).
- **TLS**: el perfilador de llamadas indirectas de libgcov (`__gcov_indirect_call_profiler_v4`) guarda su estado
  en una variable `__thread` leída con `mrs tpidr_el0`, pero en libnx el TLS va por `tpidrro_el0`
  (`-mtp=soft`, `__aarch64_read_tp`). Mezclarlos = escribir en una dirección basura. `gcov_nx.cpp` define esa
  variable y las dos funciones (misma lógica que libgcov de GCC 16, comprobada desensamblando `libgcov.a`), así el
  enlazador nunca saca las de libgcov.

### 2. Entrenar en la consola

Con la cola de la sesión principal (`tools/switchctl/cola.sh`), la NRO instrumentada como en una medida del título
pero **larga**:

```
DEBUG_EXTRA='NWII_GCOV_DUMP_EVERY="60";NWII_GCOV_DUMP_AT="880"' tools/switchctl/cola.sh add bin/<conf>pgogen.nro <nombre> 900
```

- **Tiene que durar**: con los contadores el juego va a 10-20 fps y el demo del título es por frames. El primer
  entrenamiento (r13b, 380 s) se gastó casi entero en la cinemática del castillo y el demo jugable solo empezó a
  los 360 s (`switch/real_logs/bench_pgogen_1108_2310/capturas.jpg`): el desierto de 4 jugadores **no** entró en
  el perfil y la NRO optimizada no mejoró ahí (ver medidas). Con ~900 s entran todos los demos.
- Cada volcado tarda ~20 s en la consola (escribe ~6,6 MB en 136 archivos) y va en un hilo de baja prioridad en
  el núcleo 2: no para el juego.
- Para entrenar también el mapa / un nivel: `INPUT_TEST=1 ./pgo.sh gen` deja además
  `bin/<conf>pgogen_input_test.nro` (entrada por guion de `debug.toml`, `NWII_INPUT_SCRIPT="0-100000:100:20:4"`,
  `NWII_INPUT_PHASES="3"` = hasta el mapa del Mundo 1). Solo para entrenar, **nunca como juego**.
- Bajar los archivos: `tools/pull_gcda.sh profiles/raw [<carpeta>]` (con ftpd abierto; sin carpeta toma la más
  nueva de `sdmc:/switch/NSMBWnx/gcda/`). Luego se puede borrar esa carpeta de la SD.

### 3. Juntar: `./pgo.sh import <nombre> <carpeta>[:peso] …`

Copia (y si hay varias carpetas, suma con `aarch64-none-elf-gcov-tool merge`, con pesos opcionales) en
`profiles/<nombre>/`, y comprueba que cada `.gcda` corresponde a un objeto de `nx/pgogen`.

### 4. Optimizada: `./pgo.sh use <nombre> [variante]`

Mismos fuentes y flags + `-fprofile-use=profiles/<nombre> -fprofile-prefix-path=<carpeta de compilación>
-fprofile-partial-training -fprofile-correction -Wno-missing-profile`:

- `-fprofile-partial-training`: lo que no se ejecutó en el entrenamiento se optimiza como siempre (`-O2`), no
  para tamaño. Imprescindible: el demo del título no toca casi nada del juego.
- `-fprofile-correction`: suaviza los contadores del runtime que se pisaran entre hilos.
- Un `.gcda` de otro fuente da error `coverage mismatch` (se deja como error a propósito: así se nota un perfil viejo).
- **Sin separar bloques fríos a otra sección** (`-freorder-blocks-and-partition`): en este GCC 16 de AArch64 viene
  apagado, y si se enciende el ensamblador falla (`invalid operands (.text.unlikely.X and .text.hot.X sections)`):
  las tablas de saltos compactas de los `switch` (entradas de 2 bytes, `(.Lcaso - .Lbase) / 4`) no pueden apuntar a
  un bloque que se fue a otra sección. Solo se podría con `-fno-jump-tables`, que empeora los `switch`. Así que la
  PGO aquí ordena los bloques **dentro** de cada función (el camino caliente seguido, lo frío al final), pero no
  saca lo frío fuera de la función.
- `-O3` encima de un perfil entrenado con `-O2` da error `coverage mismatch` / "number of counters … does not
  match" (con `-O3` el inliner temprano mete más, `early-inlining-insns` 6 → 14, y cambia el grafo que se contó).
  Solución sin re-entrenar: `GEN_FLAGS="-O3 --param=early-inlining-insns=6"` (comprobado: 0 errores). Regla
  general: cualquier flag que cambie el código **antes** de instrumentar exige o re-entrenar o fijar ese parámetro.

## Medidas en la consola (OC 1785 MHz, portátil, demo del título 150 s, `tools/compare.py`)

Métrica: tiempo del hilo del juego por frame = `cpu%` / fps de cada ventana de 5 s de `nwii.log`
(tiempo ocupado, sin esperas). Las ventanas se alinean en el título; el orden del demo puede variar un poco.
Ruido entre dos corridas iguales de r13b: ~3 % en las ventanas pesadas; el desierto varía ±10 %.

| NRO | castillo (ventanas 5-12) | perfil completo: ms ocupados del juego / frame | notas |
|---|---|---|---|
| r13b (`bench_r13b_ref_1108_2346`) | 15,89 ms | 9,65 | referencia, misma tanda |
| `o3` (`bench_o3_1108_2342`) | 15,43 ms (−2,9 %) | 9,34 (−3,2 %) | generado −1,3 %; `nwii_r9_axfx_fast` (mezclador AX, en `output_24.cpp`) −15 % (vectoriza) |
| r13b otra vez (`bench_r13b_ref2_1109_0007`) | 15,81 ms | | coincide con la primera ±0,5 % |
| **`pgo_titulo1`** (`bench_pgo1_1109_0003`) | **14,03 ms (−11 %)** | **7,92 (−18 %)** | generado −21 %, runtime −12 %, axfx −14 %. Pantalla del título (ventanas 2-4) 12,3 → 10,3 ms (−16 %). Media de la corrida 45,3 → 51,1 fps; ventanas pesadas de la referencia 33,8 → 43,7 fps |
| `pgo_titulo1_ord` (PGO + orden L de su propio perfil; `bench_pgo1_ord_1109_0016`) | 13,6 ms (−13 %) | | L suma ~3 % encima de la PGO en el castillo |
| `pgo_titulo1` 200 s (`bench_pgo1_200_1109_0039`) | 14,0 ms | | título −17 % (12,4 → 10,2 ms) |
| `pgo_titulo1_o3` (PGO + `-O3`; `bench_pgo1_o3_1109_0044`) | ~14,0 ms (= PGO) | | título 10,2-10,3 ms (= PGO): **`-O3` no suma nada encima de la PGO** (lo que daba `-O3` solo, el mezclador AX vectorizado, la PGO ya lo saca: axfx −14 %) |

**Ojo con los demos jugables**: cada corrida pone un demo distinto a partir de los ~110 s (al azar): conchas de
Koopa (ref, ref2), lava (ref3), castillo oscuro (pgo1), **desierto 4J** (pgo1_200), playa de Yoshi (pgo1_ord y
r13b_rafaga 18:44). Solo el título y la cinemática del castillo se pueden comparar ventana a ventana; un demo,
solo contra otra corrida con **el mismo** demo (mirar las capturas `02_title_*.jpg`). Mismo demo, playa de
Yoshi: r13b 19-27 ms vs pgo1_ord 20-23 ms, a 30 fps: **sin ganancia**, porque el entrenamiento de r13b no llegó a
los demos jugables (ver "Entrenar"). Conclusión: la PGO acelera mucho lo entrenado y deja igual lo demás (no se
vio nada peor). Lo que más importa es que el entrenamiento cubra lo que se juega.
Para medir juego de forma repetible: los trabajos con guion (mapa / 1-1) con la `_input_test` de cada NRO.

## PGO y el orden de enlace de L (pregunta 4)

**Se combinan; la PGO no sustituye a L.** En este GCC/AArch64 la PGO no puede separar lo frío de cada función a
otra sección (ver arriba), y `-freorder-functions` solo reparte funciones enteras en `.text.hot`/`.text.unlikely`
dentro de cada objeto, sin orden entre objetos. L ordena **entre** funciones y objetos. Medido: castillo
PGO 14,0 ms → PGO+L 13,6 ms. Hay que sacar el orden de un perfil de la **propia** NRO con PGO (las funciones
cambian de tamaño y forma): `tools/order_relink.sh <variante> <perfil_cpu>@<elf> <perfil_main>@<elf>` (re-enlaza
sin recompilar, ~2 min). Nota: `hot_order_eqcheck.py` marca "distintas" ~40 funciones locales clonadas
(`psq_store…isra.0`, `fcmp…constprop.0`) que existen con el mismo nombre en varios objetos: con PGO cada copia es
distinta y la herramienta las empareja por nombre; `order_relink.sh` comprueba que todas sean nombres repetidos.

## m1 (la compilación unificada `r9/r18m`): `CONF=m1`

- `m1src/` = copia exacta de `r18m/nWiiRuntime` + `r18m/export` del 9-nov 00:20 (hash de todos los archivos
  `a85dd7ea…`, igual a r18m en ese momento): código generado G5 v3 + A2, runtime R5 + RES + W + deko3d.
  El perfil va atado a esta copia: si r18m cambia, se copia otra vez y se re-entrena.
- `conf/m1.env`: `TREE=m1src`, `EXTRA_LIBS` = `m1src/lib/libuamlib_ns.a` (copiada de r15dk) + `libdeko3d.a`
  de release (como `r18m/tools/build_m.sh` con `DK_LIB=deko3d`), y el verificador de fastmem de G5.
- Instrumentada: `CONF=m1 INPUT_TEST=1 JOBS=2 ./pgo.sh gen` → `bin/m1_pgogen.nro` (+ `_input_test`, solo para
  entrenar con guion). Pico por TU instrumentada de m1: 0,84 GB (la más grande); `-j2` autorizado por la sesión
  principal (~1,7 GB); `main_output` sola y sin instrumentar. ~60 min.
- Flags solo para algunos archivos: `SRC_RE='output_24\.cpp$' SRC_FLAGS='-O3 --param=early-inlining-insns=6'`
  (p. ej. `-O3` solo para el mezclador AX).

## Reaplicar a mano sobre otro código (lo mismo que hace `pgo_release.sh`, paso a paso)

1. Copiar `nWiiRuntime/` + `export/` del árbol final a una carpeta nueva aquí (p. ej. `n1src/`) y crear
   `conf/n1.env` copiando `conf/m1.env` (cambiar `TREE`; `EXTRA_LIBS` si enlaza deko3d).
2. `CONF=n1 INPUT_TEST=1 JOBS=2 ./pgo.sh gen` (~1 h) → entrenar en la consola (título 900 s + mapa/1-1, ver
   "Entrenar") → `tools/pull_gcda.sh profiles/raw` tras cada corrida.
3. `CONF=n1 ./pgo.sh import n1_todo profiles/raw/<título> profiles/raw/<mapa>` →
   `CONF=n1 ./pgo.sh use n1_todo` (~25 min) → medir → con el perfil de esa NRO,
   `tools/order_relink.sh n1_pgo_n1_todo <perfil_cpu>@<elf> <perfil_main>@<elf>`.
Si el código cambia aunque sea un poco, `use` falla con `coverage mismatch` (a propósito): hay que repetir 2-3.

### Entrenamientos de m1 (9-nov)

| Carpeta (`profiles/raw/`) | Corrida | Qué cubre |
|---|---|---|
| `261109_063036` | `bench_m1gen_titulo_1109_0130`, título, `DUMP_EVERY=60` | castillo, demo del cielo (peces voladores), demo de las conchas, desierto 4J (~570-600 s). La consola se durmió a mitad del volcado 10: hay datos hasta ~600 s (lo dormido no cuenta, solo es menos dato) |
| `261109_071407` | `bench_m1gen_juego2_1109_0214`, `_input_test`, guion viejo (`PHASES="3>5"`) | mapa del Mundo 1 + 1-1 varias veces (muertes, vuelta al mapa) |
| `261109_072615` (**descartada**) | `bench_m1gen_speedrun_1109_0226` | se quedó 900 s en el mapa: inversión de prioridad (abajo); 106 de 139 archivos, sin `dumps.txt` |
| (pendiente) | `m1gen_speedrun` otra vez, con la NRO arreglada | 1-1 → 1-2 → … mientras pase |

- **Inversión de prioridad (arreglada 9-nov 02:49)**: el hilo del volcado iba a prioridad 0x3B (baja) en el núcleo 2,
  que está ocupado (AX + worker). Mientras libgcov escribe tiene tomados los candados de stdio/fs de newlib, y el
  juego lee sus archivos (disco virtual) por los mismos: cargar el 1-1 esperaba a un volcado que casi no recibía
  CPU (un volcado de 337 s en el título, otro que no terminó en 14 min en el speedrun, y juego2 solo entró al 1-1
  al acabar su primer volcado). Ahora el hilo va a 0x2C (como los del juego). Solo cambió `gcov_nx.cpp` (no
  instrumentado): los objetos instrumentados son los mismos y los perfiles ya tomados siguen valiendo.
- El primer volcado de una corrida tarda más (~160 s: crea los 139 archivos en la SD); los demás 20-40 s.
- `profiles/m1_titulo` = solo título; `profiles/m1_tj` = título + juego2 (`gcov-tool merge`, `runs=2`, sumas
  comprobadas con `gcov-dump`).
- `bin/m1_pgo_m1_titulo.nro` (+ `_input_test`): PGO m1 con el perfil del título (02:32, 0 mismatches,
  fastmem 0 mal formadas).

Guion "speedrun" para entrenar juego (`_input_test`, mando de lado: derecha de la pantalla = `DOWN 0x4`,
1 = `0x200` correr, 2 = `0x100` saltar; segmentos `desde-hasta:máscara:periodo:largo` sumados con `;`):

```
NWII_INPUT_SCRIPT="0-100000:100:20:4"
NWII_INPUT_PHASES="3>5>3>5>3>5>3>5"
NWII_INPUT_SCRIPT_1="0-100000:100:20:4"
NWII_INPUT_SCRIPT_2="0-10000000:204:1:1;0-10000000:100:45:30;17-10000000:100:97:6"   (y igual _4, _6, _8)
NWII_INPUT_SCRIPT_3="0-100000:4:120:8;60-100000:100:120:6"                           (y igual _5, _7)
```

Nivel: derecha + correr siempre; salto mantenido 30 de cada 45 frames (saltos altos) + un toque corto cada 97.
Mapa: derecha 8 frames cada 120 (al siguiente nivel) y 2 a los 60 frames (entrar). El giro (agitar) no se
puede en la Switch con `input_script.cpp` (el bit de agitar solo existe para los jugadores 2-4 en PC).

### m1: primera medida y el fallo del runtime con PGO (9-nov 02:43-02:58)

`m1_pgo_m1_titulo` **con el runtime también optimizado con el perfil** (renombrada `*_rtpgo`):
- Título (manda el juego): 9,8 → 8,4 ms/frame (−14 %).
- Mapa con guion (`bench_pgom1_mapa_1109_0253`): **45 fps contra 59-60 de m1**. Mismo tráfico GX (2,04 M
  escrituras/s), pero el worker de render (núcleo 2) al 91 % contra 68-71 %: 0,45 contra 0,33 µs por escritura,
  **36 % más lento**. En el castillo pasa lo mismo (28 fps contra 37-54; el juego sigue a 60 frames/s y se
  descartan más al presentar).
- Por qué: el worker, la entrega de frames al hilo de render y el write-gather dependen del tiempo. En la NRO
  instrumentada el juego va a 5-30 fps, así que el worker está casi siempre ocioso y con las colas vacías; la PGO
  aprende ese estado y aparta como frío lo que a 60 fps es caliente (colas llenas, contrapresión).
- Arreglo: **PGO solo en el código generado** (el juego, que por frame hace lo mismo vaya rápido o lento); el
  runtime y `main.cpp` se compilan como siempre (`NWII_PGO_RUNTIME=OFF` por defecto en `cmake/pgo.cmake`,
  `PGO_RUNTIME=ON` para el comportamiento viejo). La instrumentada no cambia (cuenta también el runtime, pero
  esos datos no se usan).

## PGO de una release (lo acordado el 9-nov: la PGO es el ÚLTIMO paso)

G6 y N siguen cambiando el código generado, y cada cambio invalida el perfil. Así que la PGO no es la base de
nada: se hace **una vez, cuando el tronco esté congelado para una release** (p. ej. m2 = m1 + G6 + N), y
encima el orden L. Dos órdenes, con el entrenamiento de la consola en medio:

```
cd ~/nsmbw-port/r9/r17pgo
./pgo_release.sh prep m2 --export <carpeta export/ de la release> --runtime <su nWiiRuntime/>
#   copia los fuentes a m2src/ (+ SHA256SUMS), conf/m2.env (librerías de deko3d si el runtime las usa),
#   compila la instrumentada (bin/m2_pgogen.nro + _input_test, ~1 h a -j2) e imprime los 2 trabajos de la
#   cola (título 900 s + juego "speedrun" 900 s) y cómo bajar las carpetas.
./pgo_release.sh finish m2 profiles/raw/<título> profiles/raw/<juego> --order <perfil_cpu.txt@elf> <perfil_main.txt@elf>
#   comprueba que m2src/ no cambió, junta los perfiles, compila la PGO (solo código generado, ~30-40 min a
#   -j2), su _input_test y, con --order, el orden L (perfiles traducidos por nombre desde el .elf con el que se
#   tomaron; lo mejor después es un perfil de la propia NRO con PGO y `tools/order_relink.sh`).
```

Ejemplo con G6: `--export ~/nsmbw-port/r9/r20g6/e2/export --runtime ~/nsmbw-port/r9/r20g6/nWiiRuntime`
(o lo que se fusione en m2). El `export/` y el `nWiiRuntime/` tienen que ser los que van juntos (las cabeceras de
`nWiiRuntime/include/runtime/` con las que se generó ese código).

Reglas que salieron de las pruebas:
- Solo el código generado con perfil (el runtime depende del tiempo y la instrumentada va lenta: ver arriba).
- Entrenar largo (900 s) y mirar `capturas.jpg`: que salgan varios demos jugables; y el juego con guion.
- `-O3` no suma encima de la PGO; `main_output.cpp` nunca se instrumenta (RAM).
- Medir: título + castillo (deterministas) y el mapa con guion (`_input_test`), contra la release sin PGO en la
  misma tanda. Los demos jugables salen al azar: no comparar ventanas de demos distintos.

### m1: validación de "PGO solo en código generado" (9-nov 03:25-04:00)

| Corrida | Qué | Resultado |
|---|---|---|
| `bench_pgofix_t_1109_0325` vs `bench_m1ref_t3_1109_0330` | título 200 s, PGO+L (`m1_pgo_m1_titulo_ord1`) vs m1 | título: juego **9,9 → 8,3 ms/frame (−16 %)**; castillo 35-45 fps vs 37-52 (m1 contra m1 varía igual; worker ~90 % en los dos) |
| `bench_pgom1ord_t_1109_0257` | la vieja con runtime PGO + L | castillo 28-32 fps: confirma que el fallo era el runtime con perfil |
| mapa con guion: `pgofix` (sin L) / `pgoL` (L del título) / `m1ref_mapa3`, `m1L` (m1) / `m1sinL` | | 54,0 / **55,5** / 58,2 y 55,2 / 57,1 fps. Hilo del juego ocupado: PGO 67-68 % vs m1 72-79 % (**−10 a −15 %**). Worker (núcleo 2): PGO 84-87 % vs m1 71-77 % |

Conclusión: en m1 la PGO baja bastante el tiempo del hilo del juego, pero en el mapa y el castillo **manda el worker
de render** (núcleo 2), así que los fps no cambian (están dentro de lo que varía m1 entre corridas). Queda una
duda: con el código PGO el worker trabaja algo más por escritura GX (84 % vs 71-77 % con el mismo tráfico); el
orden L lo mejora un poco (sin L 87 %). Hipótesis: el código del juego (más grande, +2 % de `.text`) compite con
el worker por la L2 de 2 MB. Preparada sin medir: `bin/m1_pgo_m1_titulo_ord2.nro` (+ `_input_test`) con el orden
L sacado de los perfiles del título **y del mapa, incluido el del worker** (`profile_gxw.txt`). En la release
conviene sacar el orden L de perfiles del título + mapa + worker de la propia NRO con PGO.
