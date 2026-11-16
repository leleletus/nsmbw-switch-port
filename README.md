# NSMBW → Nintendo Switch: port por recompilación estática

Port de **New Super Mario Bros. Wii** (EE. UU., `SMNE01`) a Nintendo Switch (homebrew, formato NRO) hecho por
**recompilación estática**: el código PowerPC del juego se traduce a C++ nativo (aarch64) con un recompilador y se
enlaza con un runtime propio que reimplementa el hardware y el sistema de la Wii (GX, AX, IOS, mandos, disco virtual…).
No es un emulador: el código del juego corre directamente en la CPU de la Switch.

El recompilador es un proyecto de terceros (NWiiRecomp, ver [`recompilador/UPSTREAM.md`](recompilador/UPSTREAM.md));
todo lo demás (runtime para Switch, arreglos, optimizaciones, banco de medida, documentación) es trabajo propio.

> **Este repositorio no contiene nada del juego**: ni disco, ni ejecutable, ni recursos, ni el código C++ que sale de
> recompilarlo, ni partidas guardadas. Hace falta una copia propia legal del juego (ver [Requisitos legales](#requisitos-legales)).

## Estado actual (16-nov-2026): binario `m6q`

Objetivo: 60 fps en una Switch v1 sin overclock, en portátil, a los relojes reales de ese modo (**CPU 1020 MHz / GPU 460 MHz**).
Fotogramas **dibujados por segundo** en consola, mismo recorrido de cinco escenas forzadas (desierto · agua · cielo ·
nubes · castillo), imagen revisada y sin cuelgues:

| Escena | Antes (g7d + PGO, 15-nov) | Ahora (m6q) |
|---|---:|---:|
| Desierto | 55 | **59,5** |
| Agua (agua_pinguino) | 35 | **57,6** |
| Cielo | 41 | **59,5** |
| Nubes | 59 | **60,0** |
| Castillo | 47 | **58,2** |
| Selector de mundos («−» en el mapa) | 39,8 | **59,8** |

Tirada Q02 de `status/ESTADO.md`. El selector se midió aparte (tanda P47 → P43). El tiempo de render por fotograma
bajó de ~14 ms a 6–11 ms según la escena.

Pendientes conocidos:
- El **agua** aún no llega a 60 estables (ventanas sueltas por debajo y fotogramas >30 ms; el hilo del juego sigue en
  ~13,5 ms ahí). La única palanca que queda es **regenerar el código del juego y repetir el PGO** (~7–8 h de máquina).
- **Fondo negro ocasional** en el selector de mundos (visto también en PC; investigado en el frente P, sin causa cerrada).
- Falta instalar `m6q` como binario por defecto y retirar del `debug.toml` las líneas provisionales (ver `status/ESTADO.md`).
- `tools/preparar_sd/` (guion que prepara la SD) se añade aparte: ver [`tools/PREPARAR_SD_PENDIENTE.md`](tools/PREPARAR_SD_PENDIENTE.md).

Cronología completa: [`docs/HISTORIA.md`](docs/HISTORIA.md). Bitácora íntegra y mediciones: [`status/ESTADO.md`](status/ESTADO.md).

## Cómo está organizado

| Ruta | Contenido |
|---|---|
| `runtime/nWiiRuntime/` | Fuentes del runtime final (árbol de `m6q`: `src/`, `include/`, `assets/`, `CMakeLists.txt`). |
| `recompilador/` | Cambios locales sobre el recompilador/analizador de terceros + `UPSTREAM.md` + licencia original. |
| `r9/<frente>/` | Un directorio por frente de trabajo: su `LEEME.md`, parches (`*.patch`, `parches/`) y guiones (`tools/`). |
| `r9/tools_comunes/` | Herramientas compartidas (línea de resumen de corridas, verificación en la máquina de pruebas, plan de limpieza). |
| `scripts/` | Guiones del PC de desarrollo (límites de memoria, vigilante, cadenas de compilación/PGO, control remoto). |
| `tools/switchctl/` | Control de la Switch por red (Wi-Fi): banco de medida, cola de corridas, verificación de imagen, `debug.toml.maestro`. |
| `docs/` | Guías: compilar para PC y Switch, jugar, depurar, diseño (deko3d, fastmem), CPU/GPU de la Switch, `HISTORIA.md`. |
| `status/ESTADO.md` | Bitácora de estado, la más reciente primero (≈ 370 líneas). |

Los parches de `r9/` están **filtrados**: se han quitado los fragmentos que tocaban código generado del juego
(`output_*.cpp`, `*_gen.*`); solo quedan cambios sobre el runtime y las herramientas. Los parches enteros de algunos
frentes antiguos (los que solo cambiaban código generado) no se incluyen.

Frentes más recientes (los que producen `m6q`), cada uno con su LEEME:

| Frente | Carpeta | Tema |
|---|---|---|
| V | `r9/r37v` | sincronía juego → trabajador → render → retrazo |
| C | `r9/r38c` | reparto de núcleos y cuelgues de la CPU de sonido (S2) |
| R2 | `r9/r39r2` | hilo de render |
| D | `r9/r40d` | descartar fotogramas barato en el trabajador |
| M | `r9/r41m` | integración m6a…m6i |
| K | `r9/r42k` | código frío fuera del camino caliente + sondeo de interrupciones |
| A | `r9/r43a` | abaratar la CPU de sonido |
| W | `r9/r44w` | el fotograma del agua |
| P | `r9/r45p` | mapa del mundo y selector de mundos (origen de `m6q`) |

## Mejoras y sus interruptores

Todo se controla con variables `NWII_*`, que en la Switch se ponen en `debug.toml` (clave = valor entre comillas).
Las mejoras llevan un interruptor general de apagado (`NWII_<FRENTE>_OFF=1`) además de los propios. Nota: en la Switch
las variables con valor `"0"` se borran salvo una lista de excepciones, así que para apagar usa el `_OFF` o quita la línea.
La lista completa de cada frente está en su `LEEME.md`; aquí lo esencial:

| Mejora | Qué hace | Interruptores principales | Apagar |
|---|---|---|---|
| **V** sincronía | el bucle ocioso del invitado no duerme tras cada retrazo; retrazo VI elástico (el juego deja de ir «encadenado» a 35 fps) | `NWII_V_WAKE=1`, `NWII_V_LATE=8` | `NWII_V_OFF=1` |
| **C** sonido sin cuelgues | arregla un bloqueo por prioridades y la lista de llamadas AX sin protección | activas por defecto | `NWII_C_SPIN_OFF=1`, `NWII_C_VIGIA_OFF=1`, `NWII_C_AXLIST_OFF=1` |
| **S2** sonido fuera del hilo del juego | la CPU de sonido corre en su propio hilo (núcleo 2) | por defecto | `NWII_S2_OFF=1` |
| **R2** render | copias EFB, borrados, estado y hash de texturas más baratos (render −4 ms) | `NWII_R2_*` (LATEFREE, CLEARTEX, COPYFBO…) | `NWII_R2_OFF=1` |
| **D** descarte barato | el trabajador descarta fotogramas sin parsear todo; fusión de los ~1 100 dibujos de la superficie del agua | `NWII_D_DROPFAST`, `PARSESTOP`, `LIGHTPARSE`, `FASTDECODE`, `FASTPREP`, `NWII_D_MERGE` | `NWII_D_OFF=1` |
| **K** código frío | reordenación caliente/frío del código generado (compilación, no runtime) y sondeo de interrupciones más espaciado | `NWII_POLL_EVERY=128` | `NWII_K_OFF=1` |
| **A** audio nativo | mezclador AX integrado, ADPCM por lotes, esperas cortas que duermen | `NWII_A_MIX=2`, `NWII_A_NOSPIN=1`, `NWII_A_REQSPIN=200` | `NWII_A_OFF=1` |
| **W** agua | rutina nativa de la superficie de agua/lava (salida idéntica byte a byte) y escritura ligera al pipe de GX | `NWII_W_AGUA=1`, `NWII_W_WGP=1` (`NWII_W_SIM`: probada, **no** gana, apagada) | `NWII_W_OFF=1` |
| **M** registro | vaciado del registro a la SD en un hilo aparte | `NWII_M_LOGBG=1` | `NWII_M_LOGBG_OFF=1` |
| **P** mapa/selector | fusión de draws en la caché de display lists y descarte de dibujos fuera de pantalla; **encendidos por defecto en m6q** | `NWII_P_DLMERGE`, `NWII_P_CULLSKIP` (`TRIPLE`, `KEEPCOPY`, `COPYMRT`: probados, apagados) | `NWII_P_OFF=1` |

Además: PGO (optimización guiada por perfil real de la consola) sobre el código generado, cuyos perfiles no se
publican. Interruptores de comprobación (`*_CHECK`) comparan la ruta nueva con la antigua píxel a píxel en PC.

## Cómo se construye (visión general)

Hay una orden única: `tools/construir_nro/construir_nro.py` (ver `tools/construir_nro/LEEME.md`), que parte de tu copia del juego y produce el NRO y los datos de la SD.

1. **Recompilar el juego** con NWiiRecomp (commit y cambios en [`recompilador/`](recompilador/UPSTREAM.md)) sobre el
   ejecutable y los módulos extraídos de tu disco. Salida: `output_*.cpp`, `main_output.cpp` (no se incluyen).
2. **Compilar para PC (Linux)**: [`docs/COMPILAR_PC_LINUX.md`](docs/COMPILAR_PC_LINUX.md); runtime en
   `runtime/nWiiRuntime` (CMake), banco de comprobación en `r9/r41m/tools` y `r9/r45p/tools`.
3. **Compilar el NRO** (devkitA64/libnx): [`docs/COMPILAR_SWITCH.md`](docs/COMPILAR_SWITCH.md). Para m6q:
   `r9/r45p/tools/build_nro.sh` y `nro_completo.sh`; el flujo PGO/orden de funciones, en `r9/r17pgo`, `r9/r41m/tools/build_pgo_m6.sh`
   y `r9/r42k/tools`. Las rutas absolutas de los guiones apuntan al entorno original: ajústalas.
4. Los LEEME de `r9/r41m`, `r9/r45p` y `r9/r44w` explican paso a paso cómo se produjeron `m6i`/`m6q`.

## Instalación en la consola

Guía: [`docs/INSTALAR_README_ES.md`](docs/INSTALAR_README_ES.md) y [`docs/JUGAR_SWITCH.md`](docs/JUGAR_SWITCH.md).
En resumen: Switch con Atmosphère y homebrew; copiar a la microSD el `.nro` y la carpeta `nsmbw/` con los datos
extraídos **de tu propio disco** (`tools/preparar_sd`, pendiente de añadir, automatiza esto). Para medir o depurar se
usa `debug.toml` (`tools/switchctl/debug.toml.maestro`) y el control remoto por red de `tools/switchctl/`
(sys-botbase, nxlink, ftpd). La IP de la consola aparece como `<IP_SWITCH>`.

## Requisitos legales

- Necesitas **tu propia copia legal** del juego (NSMBW, región EE. UU.). Este repositorio no incluye, ni debe incluir,
  ejecutable, recursos, código generado del juego ni partidas guardadas (el `.gitignore` lo impide).
- El recompilador de terceros tiene licencia propia **de uso no comercial** y prohíbe el entrenamiento de IA
  ([`recompilador/LICENSE_NWiiRecomp`](recompilador/LICENSE_NWiiRecomp)); las obras derivadas deben mantener esas condiciones.
- «Nintendo», «Wii», «Switch» y «New Super Mario Bros.» son marcas de Nintendo; este proyecto no está afiliado a ella.
- Licencia de este repositorio: la misma del proyecto original del que deriva (ver `LICENSE`): uso no comercial y atribución. No incluye nada del juego; el NRO no se distribuye: cada quien lo construye con `tools/construir_nro` a partir de su propia copia.
