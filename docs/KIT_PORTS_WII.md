# wii-port-kit: diseño de un kit para portar juegos de Wii de forma nativa

Documento de diseño, 27-oct-2026. Lo escribí a partir de todo lo que se hizo para New Super
Mario Bros. Wii (USA, SMNE01). Es solo lectura y diseño: no se compiló ni se cambió nada.

**Respuesta corta a "¿se puede generalizar?":** sí, en buena parte. Casi todo lo que costó
semanas en NSMBW **no es propio de NSMBW**. Son errores y huecos del recompilador y del
runtime de NWiiRecomp que cualquier juego de Wii hecho con el SDK de Nintendo (RVL SDK +
NW4R) habría encontrado igual. Es el caso del acarreo de `addic`, las tablas de `switch`,
los registros de coma flotante compartidos entre hilos, `sc`, la caché L1 bloqueada, los
paired-singles, el límite de shaders por frame y la caché de display lists. Lo propio de
cada juego es poco: direcciones de HLE, bases de los REL, símbolos y la forma de controlarlo.
Calculo que **un 70-80 % del trabajo de NSMBW es reutilizable tal cual o con detección
automática**. El resto seguirá necesitando a una persona en cada juego, sobre todo los
controles por movimiento y puntero, el modo online, los juegos con varios DOL y los
efectos gráficos raros.

Un "programa que hace el port solo" no es realista. Sí lo es un **kit que convierte lo que
fueron semanas en días**: lo que se puede detectar se detecta solo, hay presets por versión
del SDK, el oráculo de Dolphin ya viene montado y hay una batería de pruebas que dice en
qué punto falla un juego nuevo.

---

## 1. Inventario: qué se construyó para NSMBW

Leyenda de la columna "Tipo":
- **G** = genérico: sirve para cualquier juego de Wii sin cambios.
- **S** = semigenérico: el mecanismo es genérico, pero cada juego o versión del SDK necesita
  datos propios (direcciones, tamaños de structs).
- **E** = específico de NSMBW.

### 1.1 Recompilador y analizador (`NWiiRecomp`, rama `nsmbw`, todo **sin commit** en el árbol de trabajo)

| Cambio | Qué arregla | Tipo | Dónde |
|---|---|---|---|
| `addic`/`addic.` con inmediato negativo | XER[CA] salía invertido. Rompía el idioma CodeWarrior `addic rX,rY,-1; subfe` (= `rY != 0`) | G | `nWiiRecomp/src/recompiler.cpp` (opcodes 12/13), `oracle/FINDINGS.md` D1 |
| Tablas de saltos de `switch` | `bctr`/`mtctr` mal decodificados (XO, cruce de mitades del SPR). Tamaño por `cmplwi`, base por `lis/addi/lwzx`. De 0/190 a 136/143 en el DOL y 47/47 en los REL | G | `nWiiAnalyzer/src/analyzer.cpp` |
| Raíces débiles desde datos de los REL | Punteros a código en `.data` de los REL que no caen en ninguna función conocida: 634 funciones nuevas | G | `analyzer.cpp`, `src/core/loader.cpp` |
| REL AOT (recompilar los módulos REL por adelantado) + importaciones | Los REL (`d_profileNP`, `d_basesNP`, `d_enemiesNP`, `d_en_bossNP`) se recompilan con bases fijas y sus secciones relocadas son visibles al analizador | S (las bases son por juego) | `[[rel_aot]]` en el TOML, `loader.cpp`, `main.cpp` del recompilador |
| Cambio de FPU por hilo | El SDK cambia los FPR de forma perezosa (MSR[FP] y la excepción "FP no disponible"), pero el código generado no comprueba MSR[FP]. Ahora `OSSaveContext`/`OSLoadContext` se reconocen **por su forma** y emiten `fpu_on_save_context`/`fpu_on_load_context`. Era la causa de los cuelgues A/B | G | `recompiler.cpp`, `cpu_context.h` |
| `sc` (syscall) | Reanudaba en el `bdnz` con CTR = 0, lo que daba 2^32 vueltas, y tomaba r0 por una llamada IOS | G | recompilador e `interpreter.cpp` |
| Caché L1 bloqueada + DMA (`DMA_U`/`DMA_L`, HID2, `dcbz_l`) | Sin esto NW4R no puede armar matrices en la caché bloqueada | G | `cpu_context.h` |
| FP y paired-single (`fres`, `frsqrte`, redondeo a single, `fctiw`, `psq_l/st` con GQR, FPSCR, `divw`) | Hacía que la animación esquelética saliera mal | G | `cpu_context.h`, recompilador |
| Despacho rápido O(1) + saltos indirectos nativos | Tabla plana dirección → función, y `bctrl`/`blrl` sin volver a `run_game` | G (hoy es un **post-proceso** en Python) | `perf/apply_fastdispatch.py` |
| Firmas de prólogo para HLE + lista `hle_skip` | Desvía funciones del SDK al código nativo. Hay falsos positivos: funciones de ES confundidas con `IOS_Ioctlv` | S | `analyzer.cpp`, `hle_skip` en el TOML |
| Base de datos de firmas (`totaldb.dsy`) | Solo da nombres a funciones del SDK. No cambia el código generado | G (datos GPL, ver §6) | `third_party/dolphin-sigdb/` |

### 1.2 Runtime (`nWiiRuntime`)

| Área | Qué se hizo | Tipo |
|---|---|---|
| **AX (audio) HLE** | Mezclador de voces AX nativo, frames de 3 ms a 32 kHz, altavoz del Wiimote (`src/hle/ax/ax_hle.cpp`). DSP e interfaz de audio ajustados (`hw_dsp.cpp`, `hw_ai.cpp`) | S: depende de la versión de AX (tamaño de la struct de voz) |
| **WPAD/KPAD HLE** | Sustituye toda la pila Bluetooth (WUD/BTE) por un Wiimote virtual de lado: botones, inclinación, sacudida y vibración. Se engancha por dirección (`[hle_hooks]`) y los manejadores se llaman `NWII_*` | S: direcciones por juego y diseño de `KPADStatus` por versión del SDK |
| **NAND / FS / ES / SYSCONF** | NAND emulada en una carpeta del host, ISFS, ES mínimo, SYSCONF en formato SCv0 con idioma (`IPL.LNG`) y 16:9 (`IPL.AR`) | G |
| **Bloqueo del planificador en interrupciones** | Los manejadores de interrupción y callbacks de IOS corren con el planificador bloqueado, como `__OSDispatchInterrupt`. Sin esto se perdían acks del IPC | S: el desplazamiento `r13-0x4F80` es de SMNE01 y hoy se pone a mano (`[hle] sched_lock_offset`) |
| **Renderer GX → OpenGL** | Programas deduplicados por su código fuente (FNV-64). Ya no se salta ningún dibujo por el límite de compilación (la causa del parpadeo). Caché de shaders en disco `shadercache.bin` con prewarm. Anillos de vértices e índices. `glDepthRange` sigue al viewport. Iluminación y normales. Instantánea de matrices por dibujo (skinning) | G |
| **Caché de display lists** | Se vaciaba en cada frame (650 listas contra un límite de 512) y nunca acertaba. Con el tamaño corregido, la fiesta de Peach pasó de 18-50 a 60 fps | G (el tamaño debería adaptarse solo) |
| **Ritmo en tiempo real / presentación** | Una presentación por frame, sin renders perdidos. Contadores reales (`g_stat_presented`, `repeats`, `unshown`) | G |
| **Capa de Switch** | `src/platform/switch/`: rutas `sdmc:`, `env.txt`, hilo de CPU en su propio núcleo, mandos por libnx (inclinación, sacudida, HD Rumble), comprobación SHA-256 de `main.dol`, salida limpia. Compilación con `-ffp-contract=off` para que la coma flotante dé lo mismo que en x86 | G (salvo el nombre y el SHA del juego) |
| **Variables de diagnóstico** | Unas 100 `NWII_*`: trazas de GX, IPC, DVD y VI, A/B de cada arreglo, volcado XFB, `NWII_FPS`, `NWII_REALTIME`… | G |

### 1.3 Herramientas alrededor

| Herramienta | Qué hace | Tipo |
|---|---|---|
| `scripts/lim` | Lanza cualquier cosa pesada dentro de un slice de systemd con límite de memoria (7 GB) | G |
| `scripts/sym_match.py` | Lleva los símbolos de una decomp (NSMBW-Decomp, PALv1) a otra versión (USA) con estas fuentes: patrón único, vecino en el mismo objeto, destino de `bl`, alias PAL→USA. Resultado: 2324 coincidencias en el DOL y 300 en los REL, más las de layout y callee | S: el método es genérico; las rutas y los nombres de módulos están fijos para NSMBW |
| `scripts/dolphin_rels.py` | Lee la lista de módulos REL de un Dolphin en marcha (0x800030C8) y escribe `[[rel_aot]]` listo para pegar | G |
| `scripts/dolcg.py` | Grafo de llamadas de un DOL (capstone) | G |
| `scripts/log_summary.py` | Resume un log: avisos, hardware e IOS sin manejar, archivos DVD, `OSReport`, PCs atascados con nombre | G (recibe el mapa de símbolos como argumento) |
| `oracle/` | **Oráculo diferencial con Dolphin**: el mismo estado de CPU (registros + memoria) en el mismo PC en los dos lados. Formato JSONL común, `compare.py`, *replay* entrada/salida y RAM completa del nativo cargada en Dolphin. Dolphin corre sin ventana en un KWin virtual | G, y es la pieza **más valiosa** del kit |
| `cpu/tests/` | Pasan instrucciones codificadas a mano por el **emisor real** del recompilador y por el intérprete, y comparan resultados (288 pruebas) | G |
| `cpu/tools/input_script.cpp` | Entrada guionizada (`NWII_INPUT_SCRIPT`) enlazada con `--wrap` en un binario de pruebas aparte | G (los bits de WPAD son estándar) |
| `cpu/tools/hang_summary.py` | Veredicto OK / STALL / HANG a partir de las líneas `[STAT]` | G |
| `render/tools/flicker.py` | Mide el parpadeo A-B-A entre volcados XFB consecutivos | G |
| `render/tools/rprof_summary.py`, `perf/*` | Perfiles del hilo host, A/B de rendimiento, medición de swaps con gdb | G |
| `verify/explore/run.sh` | Escenarios de juego sin ventana con plantilla de NAND, mosaico de capturas y resumen | G |
| `switch/` | `sync_sources.sh --checked` (instantánea de las fuentes), toolchain en CMake, `citron_test.sh` (emulador sin ventana), `dist/` con su guía | G |
| Ghidra + `rea` | Ingeniería inversa asistida | G |

### 1.4 Configuración por juego (`recomp/nsmbw.toml`)

Todo lo que es **E** cabe en unas 90 líneas:
- `hle_hooks`: 27 direcciones de WPAD/KPAD → manejadores `NWII_*`.
- `rel_aot`: 4 módulos con `module_base` y `bss_base` (leídos de Dolphin).
- `hle_skip`: 4 falsos positivos de firmas.
- `[hle] sched_lock_offset = 0x4F80`.
- `[system] language`, ventana y modo de entrada.

**Conclusión:** la parte propia del juego ya está casi toda en datos, no en código. Eso es
justo lo que permite hacer un kit.

---

## 2. Lecciones aprendidas: lista de errores típicos para cualquier juego nuevo

Esta es la parte que más se puede reutilizar: **qué se rompe y cómo se detecta**. El kit
debería traer una prueba automática para cada fila.

| Síntoma visto en NSMBW | Causa real | Cómo se encontró | Prueba que debería traer el kit |
|---|---|---|---|
| Cuelgue en el título; bucle infinito en `nw4r::math` | FPR compartidos entre hilos (sin cambio de FPU perezoso) | gdb + oráculo (CalcWorld idéntico tras el arreglo) | Prueba con dos hilos que usan FP y `OSSaveContext`/`OSLoadContext` reales |
| Congelaciones de 40-80 s en demos | `sc` reanudaba en el `bdnz` con CTR = 0 | Perfil del PC atascado | `sc` dentro de `DCStoreRange` (ya existe en `cpu/tests`) |
| Booleanos invertidos esporádicos | `addic` con inmediato negativo | Oráculo, paso a paso en Dolphin | Prueba de idioma `addic; subfe` (ya existe) |
| "WILD JUMP" al intérprete | Tablas de `switch` no resueltas | Contador de WILD JUMP en el log | Informe de cobertura de `switch` (resueltos/total) como métrica de compilación |
| Personajes rígidos o cabeza separada | Paired-singles/FP + matrices por dibujo en el renderer | Oráculo de matrices de huesos | Replay de PSMTX* y CalcWorld contra Dolphin |
| Parpadeo de capas enteras | Límite de shaders por frame + clave de shader inflada (3012 claves para 52 programas) | `flicker.py` + `[RPROF]` | `flicker.py` sobre un escenario fijo; alarma si hay frames > 5 % |
| Caídas de FPS en una escena | Caché de display lists más pequeña que una escena | Perfil del hilo host | Tasa de aciertos de la caché DL en `[RPROF]`; alarma si baja del 90 % |
| Tirones al empezar | Compilación de shaders en caliente | `[RPROF]` shaders nuevos/ms | Caché en disco + prewarm (ya genérico) |
| IPC que se pierde | Interrupciones sin bloquear el planificador | Trazas IPC | Detectar `sched_lock_offset` por patrón (ver §4.3) |
| Llamadas desviadas por error | Falsos positivos en las firmas de prólogo | Log de HLE, nombres de símbolos | Cruce de firmas con nombres de símbolos: si una función que el HLE desvía tiene en la decomp un nombre que no coincide, avisar |

---

## 3. Qué necesitaría un juego nuevo

### 3.1 Siempre (cualquier juego)
1. Disco propio extraído (`sys/main.dol`, `files/`, los `.rel` si los hay).
2. Versión del SDK de cada biblioteca. **Se lee sola del DOL**: cada biblioteca deja una
   cadena como `<< RVL_SDK - KPAD release build: Jun 22 2009 18:32:13 (0x4302_145) >>`. En
   SMNE01 son OS/AX/GX/... `0x4302_145` y NW4R G3D/SND/EF/LYT del mismo año.
3. Símbolos: firmas del SDK siempre. Una decomp si existe; si no, un mapa de Dolphin o
   Ghidra.
4. Bases de los REL: se capturan una vez en Dolphin.
5. Presets de HLE para su versión del SDK (WPAD/KPAD, AX, NAND) y el desplazamiento del
   bloqueo del planificador.
6. Mapa de controles: qué espera el juego (Wiimote de lado, Wiimote + Nunchuk, puntero IR,
   Classic, mando de GameCube).

### 3.2 Juegos de ejemplo: qué cambia respecto a NSMBW

| Juego | Lo que ya sirve | Lo nuevo o difícil |
|---|---|---|
| **NSMBW PAL/JPN (SMNP01/SMNJ01)** | Todo. Es el **primer objetivo de validación** del kit: mismo juego, otras direcciones | Solo símbolos (la decomp es PAL), bases de los REL y direcciones de HLE. Si el kit no lo hace en una tarde, el kit no sirve |
| **Mario Kart Wii** | CPU, FP, REL AOT (casi todo el juego está en `StaticR.rel`), NW4R, AX | Pantalla dividida para 2-4 jugadores (varios viewports y copias EFB), volante (inclinación precisa), Nunchuk, Classic, **mando de GameCube** (SI), modo online (DWC: hay que desactivarlo), streaming de música. Tiene una decomp avanzada, así que habrá buenos símbolos |
| **Super Mario Galaxy 1/2** | CPU, FP, NW4R parcial | **Puntero IR obligatorio** (DPD: hay que simularlo con ratón, giroscopio o stick), Nunchuk, JSystem (J3D, JAudio2 sobre AX), mucho GX avanzado (texturas indirectas, copias EFB para bloom, niebla). Hay decomp comunitaria (SMG1) en progreso |
| **Kirby's Return to Dream Land** | CPU, FP, AX, WPAD de lado (como NSMBW) | Motor propio de HAL; no conozco una decomp pública madura, así que solo habrá firmas del SDK. 4 jugadores. Buen candidato para probar el kit **sin decomp** |
| **Wii Sports / Wii Play** | CPU, AX, NAND | Puntero IR, movimiento fino (acelerómetro real, no solo inclinación), Miis (RFL, lee la base de datos de Miis de la NAND) |
| Juegos con varios DOL (p. ej. recopilaciones) | — | El kit tendría que recompilar varios DOL y emular el salto entre ellos (`OSExec` o recarga de IOS) |

### 3.3 Lo que el kit **no** resolverá solo
- Controles por movimiento y puntero bien hechos: hay que diseñarlos para cada juego.
- Modo online (WiiConnect24, DWC): se apaga con HLE.
- Código cargado de formas raras (overlays propios que no son REL, código autogenerado).
- Efectos GX que el renderer todavía no soporta: aparecen como errores de imagen que hay
  que investigar.
- Temporización: juegos que dependen del ritmo exacto de interrupciones de VI, DSP o DVD.

---

## 4. Diseño del `wii-port-kit` (`wpk`)

### 4.1 Principios
1. **Todo lo propio del juego va en datos** (`wpk.toml`). No hay `#ifdef NSMBW` en el código.
2. **Primero el oráculo.** Cada parte que el kit detecta sola (bases, símbolos, offsets) se
   puede comprobar contra Dolphin. Cuando falla algo, la primera pregunta es "¿en qué
   instrucción diverge de Dolphin?".
3. **Reproducible:** `wpk rebuild` regenera un juego desde el disco con un solo comando. Su
   prueba de aceptación es reconstruir NSMBW y que salga byte a byte igual a
   `NSMBW_r5_final` (o con las mismas métricas).
4. **Seguro con la máquina del usuario:** todo lo pesado pasa por `lim` (o su equivalente).
   Las pruebas usan copias de la NAND y borran los volcados que ya no hacen falta.
5. **El kit no distribuye nada del juego** (ver §6).

### 4.2 Arquitectura

```
                 disco del usuario (ISO/WBFS/carpeta)
                               |
                         wpk extract
                               |
          sys/main.dol, files/*.rel, assets   (nunca salen de la máquina)
                               |
   +---------------------------+----------------------------+
   |                           |                            |
wpk detect               wpk symbols                   wpk rels
 cadenas << RVL_SDK >>    firmas SDK (sigdb)            Dolphin oráculo:
 NW4R/JSystem/EGG         + decomp (sym_match)          lista de módulos
 -> preset SDK            + mapa Dolphin/Ghidra         -> [[rel]] bases
   |                           |                            |
   +-------------> wpk.toml (config del juego) <------------+
                               |
                    wpk hle  (aplica presets, detecta offsets por patrón)
                               |
                    wpk recomp (NWiiRecomp + despacho rápido integrado)
                               |
               +---------------+----------------+
               |                                |
        wpk build --pc                   wpk build --switch
               |                                |
         wpk run / wpk test               wpk package (.nro + guía)
               |
         wpk oracle (diferencial con Dolphin cuando una prueba falla)
```

El kit es un **orquestador** (Python, una sola CLI) que llama a NWiiRecomp, Dolphin, CMake y
las herramientas que ya existen. No reimplementa el recompilador. Así su licencia queda
separada de la de NWiiRecomp (§6).

### 4.3 Flujo de comandos

| Comando | Qué hace | De dónde sale hoy |
|---|---|---|
| `wpk init <disco> --id SMNE01` | Crea la carpeta del juego y un `wpk.toml` mínimo. Comprueba el SHA-1 del disco o el SHA-256 de `main.dol` contra una lista conocida (solo hashes, nunca datos) | nuevo |
| `wpk extract` | Extrae `sys/` y `files/` (con `dolphin-tool extract` o `wit`) | manual |
| `wpk detect` | Lee las cadenas `<< RVL_SDK ... (0xNNNN_NNN) >>` y `<< NW4R ... >>` de cada biblioteca. Detecta EGG y JSystem por símbolos o firmas. Elige el preset HLE y avisa si una versión no tiene preset | nuevo (las cadenas ya están en el DOL) |
| `wpk symbols [--decomp RUTA] [--dolphin-map ARCHIVO]` | Junta tres fuentes en `symbols.csv`: firmas del SDK, decomp (generalizar `sym_match.py`: mapa de módulos y región como parámetros) y mapa de Dolphin o Ghidra. Informa cuántas funciones tienen nombre y cuántas son ambiguas | `sym_match.py`, `totaldb.dsy` |
| `wpk rels` | Arranca Dolphin sin ventana (receta de `oracle/dolphin`), espera a que cargue cada REL (pone un punto de ruptura en `OSLink` o sondea 0x800030C8) y escribe `[[rel]]` con `module_base`/`bss_base`. Opción `--at-frame N` para juegos que cargan los REL tarde | `dolphin_rels.py`, `oracle.py` |
| `wpk hle` | Aplica el preset de la versión del SDK: busca por firma las funciones de WPAD/KPAD/AX/NAND y llena `hle_hooks`. Calcula `sched_lock_offset` leyendo el `lwz rX,-N(r13)` de `OSDisableScheduler`. Cruza nombres para encontrar falsos positivos (`hle_skip` automático) | nuevo; hoy es `nsmbw.toml` escrito a mano |
| `wpk recomp` | NWiiRecomp + despacho rápido. Informe: funciones, cobertura de `switch`, funciones de REL encontradas, sitios que irán al intérprete | `recomp/build.sh regen` + `apply_fastdispatch.py` |
| `wpk build --target pc\|switch` | CMake/ninja con `-j` según la RAM libre, siempre bajo `lim`. En Switch hace la instantánea de fuentes (`sync_sources.sh --checked`) | `build.sh`, `switch/build.sh` |
| `wpk run [--offscreen] [--script GUION]` | Arranca el juego con la receta sin ventana o con ventana; NAND privada por defecto | `verify/*/run.sh` |
| `wpk test [--suite smoke\|full]` | Ver §4.5 | scripts de `verify/`, `cpu/tests` |
| `wpk oracle bp\|replay\|ram\|frames ...` | Fachada sobre `oracle/` | `oracle.py`, `compare.py`, `replay_cmp.py` |
| `wpk package --switch` | `.nro` + carpeta `sdmc:/<juego>/` + guía en el idioma del usuario. Nunca incluye el DOL | `switch/dist` |
| `wpk doctor` | Comprueba dependencias (devkitA64, Dolphin, ccache, mold, venv de Python, slice de memoria) | `rea_doctor.json` es la misma idea |

### 4.4 Esquema de configuración (`wpk.toml`)

```toml
[game]
id        = "SMNE01"
title     = "New Super Mario Bros. Wii"
region    = "USA"
dol_sha256 = "..."                 # solo el hash: el kit nunca guarda el DOL
disc_dir  = "game/SMNE01"          # sys/ y files/ extraídos por el usuario

[sdk]                              # lo rellena `wpk detect`; se puede forzar a mano
rvl       = "0x4302_145"           # versión común; por biblioteca si difieren
kpad      = "0x4302_145"
ax        = "0x4302_145"
nw4r      = "2009"
engines   = ["nw4r", "egg"]        # nw4r | egg | jsystem | propio

[symbols]
sigdb     = "auto"                 # firmas del SDK (descarga aparte, ver §6)
decomp    = { path = "../NSMBW-Decomp", region = "PALv1", map = "decomp_map.toml" }
dolphin_map = ""                   # opcional
out       = "build/symbols.csv"

[[rel]]                            # `wpk rels` lo genera
path        = "files/rels/d_profileNP.rel"
module_base = 0x807684C0
bss_base    = 0x8076D460
verify_base = true                 # en ejecución: avisar si OSLink la carga en otra base

[hle]
preset            = "rvl-0x4302"   # elige los ganchos por versión
sched_lock_offset = "auto"         # o 0x4F80
skip              = "auto"         # + lista manual si hace falta
[hle.overrides]                    # ganchos extra o que no detectó el preset
# "801ee0b0" = "NWII_HLE_Return0"

[input]
controller_style = "wiimote-sideways"   # sideways | nunchuk | pointer | classic | gc
max_players      = 4
[[input.player]]                        # mapa por jugador (ver 4.6)
slot = 1
source = "auto"                         # teclado, mando SDL n, joycon-pair, joycon-L…

[render]
internal_scale  = 1
dl_cache        = "auto"                # tamaño adaptado a la escena
shader_cache    = true

[platform.switch]
title       = "NSMBW Recomp"
cpu_boost   = false
vertex_ring = false                     # hoy apagado en Switch por un fallo visto en citron

[tests]
nand_template = "tests/nand_user_tpl"
[[tests.scenario]]
name   = "boot-idle"
secs   = 180
expect = { verdict = "OK", min_fps = 50, flicker_frames_over_5pct = 0 }
[[tests.scenario]]
name   = "enter-1-1"
script = "0-10000000:100:120:4;60-10000000:4:120:12"
secs   = 180
expect = { verdict = "OK", min_fps = 50 }
```

### 4.5 Batería de pruebas (`wpk test`)

Tres niveles, de más rápido a más lento:

1. **Unidad (segundos, sin juego):** las 288 pruebas de instrucciones de `cpu/tests`,
   generadas con el **emisor real** del recompilador y con el intérprete. Cada arreglo
   nuevo de la CPU añade la suya. Es genérico: se ejecuta una vez por versión del kit, no
   por juego.
2. **Humo (minutos, sin ventana):** escenarios del `wpk.toml` con entrada guionizada.
   Métricas automáticas:
   - `hang_summary.py` → OK / STALL / HANG.
   - FPS medio y mínimo (`[FPS]`, `[STAT]`).
   - `flicker.py` → frames con parpadeo > 5 %, > 1 %, > 0,25 %.
   - `[RPROF]`: dibujos saltados, shaders fallidos, aciertos de la caché DL.
   - WILD JUMP, llamadas al intérprete, IOS o hardware sin manejar (`log_summary.py`).
   - **Destellos de un frame sobre toda la pantalla:** es lo que el usuario vio el 27-oct
     al empezar un nivel o tomar monedas. Habría que añadir a `flicker.py` un detector de
     "frame aislado muy distinto de sus vecinos con mucho negro" para que el kit lo detecte
     sin que un humano lo vea.
3. **Oráculo (decenas de minutos):**
   - *Frames de referencia:* los mismos escenarios en Dolphin (volcado de frames) y en
     nativo, comparados por similitud (SSIM) por escena, no por frame exacto.
   - *Replay* de funciones puras (PSMTX*, matemáticas, conversión de ángulos) y RAM completa
     para funciones impuras (como CalcWorld o un frame de `mainLoop`).
   - Se lanza solo cuando falla el nivel 2 o antes de publicar una versión.

Cada ejecución guarda un `results.json`. `wpk test --compare <anterior>` dice qué empeoró.
Así las rondas nocturnas comparan siempre contra una referencia.

### 4.6 Entrada multijugador como parte del kit

Lo que pide el usuario para NSMBW (2-4 jugadores con mandos en PC y Joy-Con en Switch) es
**genérico**. Hoy `wpad_hle.cpp` tiene un solo Wiimote virtual en el canal 0. El diseño del
kit sería:
- N canales WPAD virtuales (0-3) con estado propio. `WPADProbe` y `WPADGetStatus` responden
  "conectado" solo en los canales que tienen un mando asignado.
- En PC: cada mando SDL que se conecta ocupa el primer canal libre. El teclado es el
  jugador 1 por defecto. Mapa por jugador en `[[input.player]]`.
- En Switch: un Joy-Con suelto de lado equivale a un Wiimote de lado (encaja perfecto con
  NSMBW). Un par de Joy-Con o un Pro Controller es un jugador. Se usa el applet de
  `hidLaPlayerSelect` (libnx) para que cada jugador elija mando.
- El estilo de mando (de lado, Nunchuk, puntero) es una propiedad del juego
  (`controller_style`). El mapa físico es una propiedad del usuario.

### 4.7 Rendimiento como parte del kit

Lo que vale para todos los juegos:
- Transformación de vértices y generación de coordenadas de textura (texgen) en el vertex
  shader de la GPU. Hoy se hacen en la CPU: en la fiesta de Peach el hilo host va al 40-60 %.
  Es la siguiente gran mejora y es genérica.
- Caché DL adaptable y caché de shaders en disco (ya están).
- Despacho rápido dentro del recompilador, no como post-proceso.
- Perfil de "modo económico": el kit debería medir cada versión con la CPU limitada (por
  ejemplo, `cpupower` o un cgroup con `cpu.max`) para imitar la Switch. Así se valida en PC,
  de forma repetible, lo que el usuario probó a mano con el modo ahorro de energía.

---

## 5. Qué subir a NWiiRecomp como PRs

Hoy todo está **sin commit** en la rama `nsmbw` (43 archivos cambiados, unas 6000 líneas
nuevas y 1600 borradas, más archivos nuevos sin seguimiento: `src/hle/ax/`,
`src/hle/wpad_hle.cpp`, `src/platform/switch/`, `nwii_env.h`). Hay copias en
`status/nwiirecomp_wip_*.patch` y `status/nwiirecomp_untracked_*.tgz`. Propongo dividirlo en
PRs pequeños, cada uno con su prueba, en este orden (primero lo que menos se discute):

| # | PR | Prueba que lo acompaña |
|---|---|---|
| 1 | `addic`/`addic.` con inmediato negativo (CA) | `addic_neg_subfe` |
| 2 | `sc`: reanudar en la instrucción siguiente, sin tratar r0 como llamada IOS | `sc` en `DCStoreRange` |
| 3 | Decodificación de `bctr`/`mtctr` + tablas de `switch` en el analizador | Cobertura de `switch` en el log |
| 4 | Semántica FP y paired-single (fres, frsqrte, redondeo a single, fctiw, psq/GQR, FPSCR, divw) | Vectores `rsqrte_vectors.inc` y pruebas generadas |
| 5 | Caché L1 bloqueada + DMA + HID2 + `dcbz_l` | Prueba de LCEnable + DMA |
| 6 | Cambio de FPU por hilo en OSSaveContext/OSLoadContext (reconocidos por forma) | Prueba con dos contextos |
| 7 | Arnés de pruebas `cpu/tests` (emisor real + intérprete) | Él mismo |
| 8 | REL AOT: secciones relocadas visibles al analizador, raíces débiles, importaciones | Contar funciones REL antes y después |
| 9 | Despacho O(1) + saltos indirectos nativos **en el emisor** (no como post-proceso) | A/B de FPS |
| 10 | Renderer: dedup de programas, sin saltarse dibujos, caché de shaders en disco, anillos, depth range | `flicker.py` |
| 11 | Caché de display lists con tamaño adaptable | Aciertos de la caché |
| 12 | Presentación por frame y ritmo en tiempo real | Medición con gdb de los swaps |
| 13 | NAND/FS/ES/SYSCONF (idioma, 16:9) | Guardado y carga |
| 14 | AX HLE | Volcado de audio (`NWII_AUDIODUMP`) |
| 15 | WPAD/KPAD HLE con ganchos por config (y multijugador cuando exista) | Entrada guionizada |
| 16 | Capa de Switch (`src/platform/switch/`) | Compilación en CI con devkitA64 |
| 17 | Registro de variables `NWII_*` (`nwii_env.h`) documentado | — |

El orquestador `wpk`, el oráculo de Dolphin y los scripts de verificación pueden quedarse
**fuera** de NWiiRecomp, en un repositorio propio (ver la licencia abajo). Antes de abrir PRs
hay que leer `CONTRIBUTING` (si existe) y consultar al autor en su Discord: el proyecto
insiste en que "no usa código de Dolphin", y eso afecta al punto 6.2.

---

## 6. Licencias (notas; no es asesoría legal)

### 6.1 NWiiRecomp
- Su licencia es **propia: "MIT modificada con restricciones comerciales"**. Permite usar,
  modificar y redistribuir **solo sin fines comerciales**. No se puede vender, poner tras un
  Patreon ni mostrar anuncios sin un contrato con el autor. Hay que dar **crédito visible**
  (Volodymyr Vovchok / BlackLine Interactive), también en la sección "Acerca de" de los
  binarios. Las obras derivadas no pueden distribuirse con condiciones más permisivas.
- **Cláusula 6:** prohíbe usar su código para *entrenar* modelos de IA. Leer y modificar el
  código para hacer un port no es entrenar. Aun así conviene saberlo si se publica algo.
- Consecuencia para el kit: los parches al runtime y al recompilador son obra derivada y
  heredan esa licencia. Un orquestador separado que solo **ejecuta** NWiiRecomp como
  programa externo puede tener su propia licencia, pero un paquete que incluya NWiiRecomp
  sigue sus reglas.

### 6.2 Dolphin (GPL-2.0-or-later)
- **Usar Dolphin como herramienta externa** no obliga a que nuestro código sea GPL:
  ejecutarlo, conectarse a su stub GDB o leer su memoria desde `/proc` y `/dev/shm`. Así
  funciona el oráculo hoy: programas separados que se comunican por sockets o por archivos.
- **Copiar código de Dolphin** (o traducirlo línea a línea) al runtime **sí** lo convertiría
  en obra derivada GPL. Eso es incompatible con la licencia no comercial de NWiiRecomp: dos
  licencias que no se pueden combinar. El runtime tiene comentarios que citan archivos de
  Dolphin como referencia de comportamiento (por ejemplo `MMU::ClearDCacheLine` e
  `Interpreter_SystemRegisters.cpp` en `cpu_context.h`). Describir el comportamiento del
  hardware está bien. Antes de subir PRs habría que **auditar** que no haya fragmentos
  copiados y reescribir lo que se parezca demasiado.
- `totaldb.dsy` (firmas) es **GPL**. Hoy está aislado en `third_party/dolphin-sigdb/` con su
  `COPYING`, solo da nombres y no cambia el código generado. Para el kit: que se **descargue
  aparte** (no empaquetado), o generar una base de firmas propia a partir de objetos de
  decomps con licencia compatible.

### 6.3 Decomps y símbolos
- Cada proyecto de decomp tiene su licencia (o ninguna; en `NSMBW-Decomp` no encontré
  archivo `LICENSE`). El kit debe **importar símbolos en la máquina del usuario** a partir
  de la decomp que el usuario clone, sin redistribuir sus archivos.

### 6.4 El juego
- El DOL, los REL, los recursos y **el código C++ generado** (que es una traducción del
  código de Nintendo) son obra protegida de Nintendo. **El kit nunca los distribuye**: ni el
  `export/`, ni el binario de PC, ni el `.nro` con el juego dentro. El usuario pone su
  propio disco y compila en su máquina. La comprobación SHA-256 de `main.dol` que ya hace la
  versión de Switch es el patrón correcto.
- Las implementaciones HLE del SDK deben escribirse a partir del comportamiento observado,
  de las cabeceras de las decomps y del oráculo. **Nunca** a partir de SDK filtrados.

---

## 7. Hoja de ruta por fases

| Fase | Objetivo | Entregable | Criterio de "hecho" | Esfuerzo aproximado |
|---|---|---|---|---|
| **F0. Consolidar** | Dejar NWiiRecomp en commits limpios | Rama con commits por tema (lista de §5) + `cpu/tests` dentro del repo | `git status` limpio; reconstruir NSMBW desde los commits da el mismo binario | 1-2 días |
| **F1. Orquestador mínimo** | `wpk` que reproduce NSMBW | CLI con `init/extract/recomp/build/run/test`; `nsmbw.toml` pasa a `wpk.toml` | `wpk rebuild` desde el disco reproduce `NSMBW_r5_final` y `wpk test --suite smoke` da OK | 3-5 días |
| **F2. Detección automática** | `detect`, `symbols` (sym_match generalizado), `rels` (Dolphin), `hle` (presets + `sched_lock_offset` + `hle_skip` automáticos) | Presets `rvl-0x4302` | Para NSMBW, los valores automáticos son iguales a los escritos a mano | 1 semana |
| **F3. Segundo juego: NSMBW PAL o JPN** | Validar la detección con otras direcciones | `wpk.toml` de SMNP01 casi sin tocar a mano | Llega al 1-1 jugable en menos de un día de trabajo | 2-4 días |
| **F4. Oráculo integrado** | `wpk oracle` + frames de referencia de Dolphin + `results.json` comparable | Suite completa | Una regresión del renderer se detecta sin humanos | 1 semana |
| **F5. Juego distinto** | Un juego de otro estudio o motor: Kirby RtDL (sin decomp, de lado como NSMBW) o MKW (REL grande, pantalla dividida, mando de GameCube) | Lista de lo que faltó en el kit | Arranca hasta el gameplay; las carencias quedan documentadas como tareas del kit | 2-4 semanas |
| **F6. Entrada y rendimiento genéricos** | Multijugador N canales + Joy-Con; transformación de vértices en GPU; modo "CPU limitada" en las pruebas | — | NSMBW a 4 jugadores en Switch a 60 fps | 2-3 semanas |
| **F7. Interfaz** | Integrar el flujo en `nWiiStudio` (la GUI de NWiiRecomp) o hacer una TUI | — | Un usuario sin experiencia completa F1-F3 siguiendo la guía | abierto |

**Orden recomendado para el proyecto actual:** primero terminar NSMBW (multijugador,
destellos de pantalla, rendimiento en Switch). En paralelo, la F0, que es barata y protege
todo lo hecho. Luego F1-F3. Antes de F5, NSMBW debería estar "terminado".

---

## 8. Referencias dentro del proyecto

- Estado general: `status/ESTADO.md`. CPU: `status/handoff_cpu.md`. Gráficos:
  `status/r4_gfx.md`. Integración y rendimiento: `status/r5_final.json`, `perf/PROPOSALS.md`.
  Exploración: `status/exploracion_madrugada.md`. Pruebas del usuario:
  `status/feedback/USER_FEEDBACK.md`.
- Oráculo: `oracle/README.md`, `oracle/FINDINGS.md`.
- Switch: `switch/RUNTIME_PATCHES.md`, `switch/dist/`.
- Símbolos: `symbols/LEEME.md`, `symbols/report.txt`.
- Configuración del juego: `recomp/nsmbw.toml`. Licencias: `NWiiRecomp/LICENSE`,
  `NWiiRecomp/third_party/dolphin-sigdb/README.md`.
