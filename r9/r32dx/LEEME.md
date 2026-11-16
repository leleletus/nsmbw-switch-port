# r32dx: frente DX — diagnóstico exacto del hilo del juego (12-nov-2026)

Solo diagnóstico: aquí no se optimiza nada. Sin commits. No se tocó la consola (ni se encoló nada), ni `nand/`,
ni carpetas de otros frentes (lo necesario se copió aquí; de `r31t` y `r17pgo` solo se leyó). Todo lo pesado
de esta laptop fue por `scripts/lim` con `JOBS≤2`; en la otra laptop un único trabajo (`dx_objs`, `JOBS=6`).

## PAUSA 12-nov ~05:25 — estado

**Parado a petición del usuario.** No queda nada corriendo de este frente, ni aquí ni en la otra laptop
(`dx_objs` terminó solo, rc 0). **No hay ningún trabajo de este frente en la cola de la consola** (nunca se
añadió ninguno).

### Qué quedó medido

| Dato | Estado |
|---|---|
| Instrucciones PPC exactas por fotograma y por función, 6 escenas, juego a 60 fot/s (§2 T1) | **firme** (±0,8 %) |
| Lo mismo al ritmo de la consola (~31 fot/s): parte fija + parte por segundo (§2 T2) | **firme** en agua y cielo; **provisional** en desierto y nubes (ver "Qué falta" 2) |
| Coste del original en Dolphin por escena (§2 T1) | **firme** como medida (se repite al 0,05 %); la tabla de ciclos por instrucción de Dolphin la puse **de memoria** (ver §1.3) |
| Instrucciones AArch64 del código generado de m2 por fotograma, clases, protocolo, huella (§2 T3-T5) | **estimación** con cotas: central ±10 % (no hay contador de instrucciones en la consola) |
| IPC efectivo del código generado en la consola (§2 T6) | **estimación**: 0,6-0,9; las cotas extremas son 0,56-1,24 |
| Reparto del runtime en el hilo del juego por escena (§2 T6) | **firme** (son muestras de la consola del frente T) |
| Control en x86 de los fallos de caché por símbolo (`tools/perf_x86.sh`) | **no hecho**: la primera pasada salió mal (periodos de muestreo) y la segunda se paró en la pausa |

### Qué falta (por orden) y cómo retomarlo

1. **Control en x86 con `perf`** (5 min, sin IA): qué parte de las instrucciones y de los fallos de L1I/último
   nivel del hilo de la CPU cae en código generado. Sirve para comprobar el orden de magnitud de la cuenta
   AArch64 y para atribuir los fallos de caché de instrucciones.
   ```
   cd ~/nsmbw-port/r9/r32dx
   tools/perf_x86.sh desierto4j 20 && python3 tools/perf_x86.py desierto4j 20 60
   tools/perf_x86.sh cielo_paratroopas 20 && python3 tools/perf_x86.py cielo_paratroopas 20 60
   ```
2. **Repetir la corrida frenada con más freno** para que desierto y nubes bajen de verdad a ~30 fot/s (con
   3000 ns se quedaron en 20,2 y 19,2 ms/fot; agua y cielo sí llegaron a 33 ms). Con 5000 ns debería bastar:
   ```
   tools/run_dx.sh bin/NSMBW_dxgcov2 gcov30v 700 NWII_DX_POLL_BURN_NS=5000
   JOBS=3 python3 tools/gcov_seg.py runs/gcov30v titulo_main desierto4j_main agua_pinguino_main cielo_paratroopas_main nubes_plataformas_main castillo_main
   python3 tools/dx_report.py gcov30v --top 30 > runs/gcov30v/report.txt
   python3 tools/dx_ipc.py --r30 gcov30v --top 25 > runs/ipc.txt
   python3 tools/dx_tablas.py gcov60 gcov30v > datos/tablas.md
   ```
3. **Contadores de hardware en la propia consola** (lo que de verdad cerraría el IPC y los fallos de caché):
   ver recomendación 0 de §4. Necesita permiso del usuario (cambia un ajuste de Atmosphère en la SD).
4. Opcional: cruzar con los `.gcda` de la consola (`r17pgo/profiles/raw/261112_052404`, objetos
   `r17pgo/nx/m2_pgogen` en la otra laptop) para tener las cuentas PPC exactas de la corrida instrumentada de
   la consola (totales de 7 demos, no por escena). No se empezó.
5. Opcional: el castillo no entra en la tabla del IPC porque el perfil T01 del frente T acaba antes.

Los binarios `bin/NSMBW_dxgcov2` y `bin/NSMBW_dx2` son los que llevan `NWII_DX_POLL_BURN_NS`; `NSMBW_dxgcov`
y `NSMBW_dx` son los de antes (iguales en todo lo demás). La carpeta pesa 4,0 GB (lo grande: `src/`, `nx_g1/`,
`pc_gcov/`, `nx/dxg1/nsmbw.elf`, `runs/*/dx/`); se puede borrar `runs/gcovpf/dx` y `runs/gcov30/` sin perder nada.

---

## 0. Respuestas a las cinco preguntas

1. **El desierto NO ejecuta más instrucciones PPC: es la demo jugable que menos ejecuta.** A 60 fot/s, sin el
   bucle ocioso: desierto 2,00 M por fotograma · nubes 2,16 · agua 2,90 · cielo 3,49 · castillo 1,09 · título
   1,60. Al ritmo de la consola (~31 fot/s, el sonido pesa el doble por fotograma): 2,13 · 2,22 · 3,09 · 3,56.
   Sus instrucciones salen más caras: 9,7 ciclos de la consola por instrucción PPC frente a 6,4 en el cielo.
   De paso: el contador `ctx.inst_count` del runtime **exagera un 35-70 %** (cuenta bloques enteros aunque se
   salga a mitad, y además cuenta las vueltas del bucle ocioso): no sirve como cuenta de trabajo.
2. **El port ejecuta MENOS instrucciones invitadas que el original, no más.** En Dolphin el juego original
   ocupa 2,47 (título) · 2,88 (desierto) · 3,73 (agua) · 4,41 (cielo) · 3,10 (nubes) · 1,91 (castillo) millones
   de ciclos del Broadway por fotograma, es decir, **solo el 16-36 % de la CPU de la Wii**. El port, en la misma
   unidad, hace 0,60-0,83 de eso. La diferencia es casi constante, **0,74 ± 0,04 M de ciclos por retrazo en las
   6 escenas**: trabajo de sistema que va por tiempo y que el original hace en PPC y el port no. No lo
   identifiqué; lo probable es la reverb AXFX (aquí nativa) y la pila Bluetooth/WPAD e IOS (aquí HLE). No hay
   bucles de espera, sondeos ni hilos girando de más: lo único "extra" es el bucle ocioso del SO (0,05 M por
   fotograma, y cuenta como ocioso).
3. **Expansión: 4,4-5,1 instrucciones AArch64 por instrucción PPC** (dinámico, m2); 9,5-15,9 M de AArch64 por
   fotograma. **IPC efectivo del código generado en la consola: 0,61 (desierto) · 0,70 (nubes) · 0,75 (agua)
   · 0,86 (cielo)**. Es bajo: los bucles compactos del mismo binario van a 1,3-2,4 y las funciones pequeñas
   repartidas a 0,35-0,55. O sea, el problema son las dos cosas: muchas instrucciones **y** que cada una
   tarda. La causa más probable del IPC bajo es la caché de instrucciones (no está medido en la consola): cada
   fotograma ejecuta ~1,0-1,5 MB de código repartido en 20 000-33 000 líneas de caché y 750-1 200 páginas,
   contra una L1I de 48 KB (750 líneas) y una L2 de 2 MB compartida; el camino caliente va mezclado con ramas
   frías (ejemplo medido: `PSMTXCopy`, 13 instrucciones PPC, ocupa 3 256 bytes y ejecuta 118 instrucciones
   saltando por ellos; en la consola tarda 330 ciclos por llamada).
4. **Dónde se va** (por fotograma del juego en la consola, 18,7-26,0 ms ocupados): código generado 15,5-20,7 ms
   y runtime 3,2-5,3 ms (escritura GX 0,7-1,7 · sondeo de interrupciones 0,8-1,2 · ayudantes psq/ps 0,6-0,9 ·
   sonido nativo 0,3-0,6). Dentro del código generado, de las instrucciones ejecutadas el 66-69 % es trabajo y
   el **25-29 % protocolo** (pc/lr 7 %, volcados 5-6 %, `inst_count` 5 %, cargas de entrada 3-4 %, recargas 3 %,
   `switch` de entrada 2,5 %). El perfil es plano: ninguna función pasa del 8 % y las 200 primeras suman el
   66-75 %. Las llamadas indirectas (5 900-10 400 por fotograma) cuestan ~9 instrucciones cada una: no son el
   problema.
5. **El desierto con PGO/G6c3:** lo de "empeora" era la métrica (lo midió el frente T: el render descartaba
   fotogramas; con fotogramas reales el PGO lo baja de 20,7 a 16,0-16,8 ms). Lo que añado: es la escena con
   peor coste por instrucción (IPC 0,61) porque su fotograma es el más disperso (90 % del trabajo en 544 KB de
   código frente a 330 en el cielo) y el que más sonido lleva (9,7 M de instrucciones PPC por segundo que van
   por tiempo, frente a 4,1 en el cielo).

Recomendaciones: §4.

## 1. Método

### 1.1 Cuentas exactas de instrucciones PPC (PC)

- `src/` = el código generado de m2 (G6 e2, `r20g6/e2/export`) con **una sentencia por línea**
  (`tools/split_lines.py`: solo cambia espacios en blanco). Comprobado: los objetos de Switch compilados de
  `src/` con la orden de m2 + `-g1` enlazados con la línea de m2 dan un `.text` **idéntico byte a byte** al de
  `r24m2/bin/nsmbw_m2.elf` (`tools/link_g1.sh`).
- `pc_gcov/` = esos fuentes compilados para PC con `--coverage -fprofile-update=single` (gcov cuenta cada arco
  del grafo de cada función: cuentas exactas, no muestreo). `bin/NSMBW_dxgcov` = línea de enlace de m2 de PC
  con esos objetos y el runtime de `nWiiRuntime/` (= r7f + `src/core/dx.cpp`).
- `dx.cpp` (`NWII_DX=<carpeta>`): en cada evento de VI mira la escena y el fotograma del nivel y parte la
  corrida en segmentos (`<escena>.carga`, `.main` = fotogramas 120-2300 del nivel, `.cola`, `entre`); al
  cerrar cada uno escribe una línea `[DX]` con los contadores del hilo y vuelca los `.gcda` de ese segmento.
- `tools/gcov_seg.py` → veces que se ejecutó cada línea; `tools/srcmap.py` → qué instrucción PPC y qué clase
  de sentencia es cada línea. Una instrucción PPC cuenta las veces de su primera sentencia. El 22 % no tiene
  sentencia propia (el compilador la eliminó antes de instrumentar): se le da la cuenta de la anterior de su
  bloque; con cota inferior y superior el total se mueve ±0,75 %.
- El juego instrumentado mantiene 60 fot/s en PC. "Fotograma" = copia XFB (fotograma del juego).

### 1.2 Parte que va por tiempo

El sonido (333 fotogramas AX por segundo) y las interrupciones no dependen de los fotogramas del juego, así
que a 30 fot/s pesan el doble por fotograma. Se mide con una segunda corrida frenada de forma uniforme
(`NWII_DX_POLL_BURN_NS=3000`: cada sondeo lento quema 3 µs) y se interpola en ms por fotograma hasta el ritmo
que tuvo la consola. Comprobado que así las interrupciones AX siguen llegando a 333/s (`__AXSyncPBs`: 5,6
llamadas por fotograma a 16,7 ms, 10,9 a 32,9 ms). **Ojo:** `runs/gcov30` se hizo con `NWII_DX_BURN_US`
(quema de golpe en el VI) y **no vale**: se pierden interrupciones de sonido.

### 1.3 Referencia: el original en Dolphin

`dolphin/dolphin_ref.py` arranca Dolphin 2609 sin ventana (copia privada del oráculo de `oracle/dolphin`,
puerto 55120), fuerza las mismas demos escribiendo la tabla del juego como hace `NWII_FORCE_DEMO`, y lee de
la memoria del proceso el reloj de ciclos emulados (`CoreTiming::Globals.global_timer`, 729 MHz) y los ciclos
que Dolphin salta en el bucle ocioso (`m_idled_cycles`). Ocupado por fotograma = (Δreloj − Δocioso) / Δretrazados.
Sale 12,162 M de ciclos por retrazo (lo esperado) y el título repetido 6 veces da 2,4744-2,4760 M.

Dolphin cuenta ciclos, no instrucciones: casi todas valen 1 (mul 3-5, div 40, fdiv 17-31, lmw/stmw 11...).
Para comparar pondero las cuentas del port con esa tabla, **que escribí de memoria** (`dxlib.dolphin_cycles`).
No la cotejé con el código de Dolphin. Lo que la apoya: la diferencia original − port es constante (0,68-0,77 M)
en escenas de 1,1 a 3,5 M; si la tabla estuviera mal de forma apreciable, la diferencia crecería con la carga.

### 1.4 Instrucciones AArch64 ejecutadas (estimación)

- `tools/a64_static.py`: clase, bloque básico y función de las 16,3 M de instrucciones de `.text` de m2.
- `tools/a64_lines.py`: línea de fuente de cada instrucción (tabla DWARF del ELF con `-g1`).
- `tools/dxlib.py`: cada bloque básico toma la mediana de las cuentas exactas de sus líneas de `output_N.cpp`.
  El 36-39 % de las instrucciones ejecutadas es código integrado de cabeceras (accesos a memoria, coma
  flotante, psq, sondeo) y hay bloques hechos solo de eso, sin línea propia: su cuenta se calibra por línea de
  cabecera para que el total cuadre con el que da gcov para esa línea (por archivo). Resultado de la
  calibración: esos bloques se ejecutan de media el 14 % de su cota alta (son casi todos ramas frías).
- Cotas: baja = esos bloques nunca; alta = siempre que se pasa por delante. La estimación central queda cerca
  de la baja. Control interno: para las líneas de cabecera que solo están en bloques con línea propia, lo
  predicho supera a gcov en un 6-13 % (o sea, si hay sesgo es hacia contar de más).
- **No es una medida directa**: en la consola no hay contador de instrucciones accesible. El control en x86
  quedó sin hacer.

### 1.5 Tiempo de la consola

Las muestras por escena y por función son las del perfil del frente T (`bench_T01_tp1_tprof_1112_0330`,
binario tp1 = mismo código generado y mismo orden que m2), leídas con `tools/consamp.py`. Los ms por fotograma
son por fotograma **del juego** (la corrección del frente T). IPC = AArch64 por fotograma / (ms en código
generado × 1020 MHz).

## 2. Resultados

Todas las tablas completas (incluidas las 25 funciones más caras de cada escena con su IPC y los subsistemas)
están en `datos/tablas.md`; los listados en `runs/gcov60/report.txt`, `runs/gcov30u/report.txt` y `runs/ipc.txt`.

### T1. Instrucciones PPC por fotograma a 60 fot/s: port frente al original (millones)

| Escena | port, exactas | bucle ocioso | port sin ocioso | en ciclos de Dolphin | **original (Dolphin)** | % del Broadway | port / original | original − port | `inst_count` |
|---|---|---|---|---|---|---|---|---|---|
| título | 1,66 | 0,06 | 1,60 | 1,74 | **2,47** | 20,3 | 0,70 | 0,74 | 2,60 |
| desierto4j | 2,05 | 0,05 | 2,00 | 2,13 | **2,88** | 23,7 | 0,74 | 0,75 | 3,08 |
| agua_pinguino | 2,95 | 0,05 | 2,90 | 3,06 | **3,73** | 30,7 | 0,82 | 0,68 | 4,29 |
| cielo_paratroopas | 3,54 | 0,05 | 3,49 | 3,66 | **4,41** | 36,2 | 0,83 | 0,74 | 4,78 |
| nubes_plataformas | 2,22 | 0,06 | 2,16 | 2,32 | **3,10** | 25,5 | 0,75 | 0,77 | 3,26 |
| castillo | 1,14 | 0,05 | 1,09 | 1,14 | **1,91** | 15,7 | 0,60 | 0,77 | 1,98 |

### T2. Parte fija y parte por segundo (corrida frenada `gcov30u`)

| Escena | ms/fot frenada | PPC/fot a 60 (M) | PPC/fot frenada (M) | fijo por fotograma (M) | por segundo (M/s) | a 30 fot/s (M) |
|---|---|---|---|---|---|---|
| desierto4j | 20,2 | 2,00 | 2,04 | 1,84 | 9,7 | 2,16 |
| agua_pinguino | 32,9 | 2,90 | 3,09 | 2,71 | 11,4 | 3,09 |
| cielo_paratroopas | 33,4 | 3,49 | 3,56 | 3,43 | 4,1 | 3,56 |
| nubes_plataformas | 19,2 | 2,16 | 2,17 | 2,09 | 4,2 | 2,23 |

Desierto y nubes son provisionales: la corrida frenada solo se alejó 2,5-3,5 ms de la de 60 fot/s, así que la
pendiente sale de una diferencia pequeña y luego se extrapola hasta 30 ms. Título y castillo no se frenaron.

### T3. AArch64 del código generado por fotograma a 60 fot/s (sin el bucle ocioso)

| Escena | AArch64/fot (M) | cotas (M) | AArch64 por PPC | % trabajo | % protocolo | llamadas directas | indirectas | sondeos |
|---|---|---|---|---|---|---|---|---|
| título | 7,59 | 6,87–11,85 | 4,74 | 65 | 29 | 19 788 | 4 940 | 45 947 |
| desierto4j | 9,54 | 8,66–14,89 | 4,77 | 66 | 28 | 23 747 | 5 807 | 53 489 |
| agua_pinguino | 14,73 | 13,64–22,62 | 5,07 | 69 | 25 | 33 609 | 6 373 | 85 889 |
| cielo_paratroopas | 15,39 | 13,88–22,65 | 4,40 | 68 | 26 | 33 569 | 10 390 | 114 067 |
| nubes_plataformas | 10,54 | 9,56–16,74 | 4,87 | 66 | 28 | 29 083 | 8 026 | 58 286 |
| castillo | 5,41 | 4,73–9,22 | 4,97 | 66 | 27 | 11 571 | 2 236 | 41 307 |

Por subsistema la expansión va de 3,1 (MSL) y 4,0-4,7 (juego, g3d, EGG, snd) a 7-9 (MTX, `nw4r::math`, AX, OS:
coma flotante y pares). Por clase de instrucción (desierto): ALU 23,5 % · constantes 13,3 · saltos 8,9 ·
comparaciones 6,9 · byteswap 5,3 · memoria del juego 6,8 · `ctx.*` ~19 (gpr 7,4, cr 3,3, pc/lr 2,8,
`inst_count` 1,7) · pila 5,1 · coma flotante 3,9.

### T5. Huella de código ejecutado

| Escena | funciones | su tamaño total (KB) | KB ejecutados en ~40 s | líneas de 64 B | páginas 4 KB | 90 % del trabajo en (KB) | por periodo de VI: KB / líneas / páginas |
|---|---|---|---|---|---|---|---|
| título | 1 272 | 3 219 | 1 502 | 31 740 | 1 049 | 364 | 1 179 / 25 624 / 886 |
| desierto4j | 3 216 | 6 642 | 3 533 | 72 650 | 2 305 | 544 | 1 033 / 22 409 / 812 |
| agua_pinguino | 4 028 | 7 779 | 4 259 | 86 952 | 2 726 | 448 | 909 / 19 757 / 740 |
| cielo_paratroopas | 2 959 | 6 331 | 3 288 | 68 022 | 2 217 | 330 | 1 251 / 27 076 / 948 |
| nubes_plataformas | 3 261 | 6 748 | 3 535 | 73 193 | 2 361 | 449 | 1 534 / 33 500 / 1 216 |
| castillo | 1 556 | 3 496 | 1 734 | 36 242 | 1 345 | 235 | 603 / 13 243 / 517 |

La columna "por periodo de VI" (media de 8, corrida `gcovpf`) se queda corta en desierto y agua: el propio
volcado de contadores frenó el juego y cada periodo recogió menos de un fotograma. Tómese como mínimo.
La mitad de los bytes de las funciones que se ejecutan no se ejecuta nunca en 40 s.

### T6. Consola (1020 MHz, perfil T01) frente a las cuentas llevadas a su ritmo

| Escena | ms pared/fot | ms ocupado | ms generado | ms runtime | PPC/fot (M) | AArch64/fot (M) | ciclos por PPC | **IPC generado** | cotas |
|---|---|---|---|---|---|---|---|---|---|
| desierto4j | 30,2 | 20,2 | 16,5 | 3,7 | 2,13 | 10,32 | 9,7 | **0,61** | 0,56–0,92 |
| agua_pinguino | 32,9 | 26,0 | 20,7 | 5,3 | 3,09 | 15,80 | 8,6 | **0,75** | 0,70–1,13 |
| cielo_paratroopas | 32,3 | 22,4 | 18,1 | 4,2 | 3,56 | 15,86 | 6,4 | **0,86** | 0,78–1,24 |
| nubes_plataformas | 30,7 | 18,7 | 15,5 | 3,2 | 2,22 | 10,98 | 8,6 | **0,70** | 0,64–1,07 |

| Runtime en el hilo del juego (ms/fot) | desierto | agua | cielo | nubes |
|---|---|---|---|---|
| escritura GX (FIFO) | 0,81 | 1,71 | 0,92 | 0,71 |
| sondeo de interrupciones y reloj | 0,86 | 1,12 | 1,22 | 0,81 |
| ayudantes psq/ps/fp | 0,60 | 0,90 | 0,65 | 0,69 |
| sonido nativo (reverb, AX) | 0,34 | 0,29 | 0,59 | 0,34 |
| núcleo y E/S | 0,32 | 0,13 | 0,09 | 0,02 |
| resto | 0,75 | 1,15 | 0,78 | 0,64 |
| **total** | **3,68** | **5,30** | **4,25** | **3,21** |

IPC por función (desierto; lista completa en `datos/tablas.md`): `func_80162D00` 1,53 · `func_802E8868` 1,30 ·
`func_80119C50` 1,19 · `__AXSyncPBs` 1,06 · `PSMTXConcat` 0,72 · `func_802821E0` (snd) 0,55 · `func_802403C0`
0,45 · `PSMTXIdentity` 0,45 · `PSMTXCopy` 0,36 · `func_8027E080` (snd) 0,27. En el cielo `checkCollision`
(`func_8008C6E0`) va a 2,39. Estos IPC por función son orientativos: en funciones cortas la muestra se
desliza y el fallo de caché de entrar en la función se le carga a ella.

### x86 (PC, hilo de la CPU entero, corrida `base60` sin instrumentar, 2 núcleos P)

| Escena | ms/fot | instrucciones x86 | IPC x86 | fallos de salto | fallos L1I | fallos último nivel |
|---|---|---|---|---|---|---|
| título | 3,3 | 18,9 M | 1,50 | 32 k | 252 k | 85 k |
| desierto4j | 3,9 | 19,9 M | 1,34 | 44 k | 415 k | 115 k |
| agua_pinguino | 4,7 | 28,4 M | 1,64 | 48 k | 428 k | 121 k |
| cielo_paratroopas | 4,4 | 28,1 M | 1,67 | 69 k | 345 k | 105 k |
| nubes_plataformas | 3,3 | 22,7 M | 1,72 | 39 k | 367 k | 63 k |
| castillo | 2,7 | 21,2 M | 1,95 | 22 k | 168 k | 42 k |

Es el hilo entero (incluye runtime y las vueltas del bucle ocioso), así que no se compara con las cuentas
AArch64. Sí enseña que en PC el desierto también es el de peor IPC y más fallos de L1I por instrucción PPC.

## 3. El desierto

- La subida "con PGO" era la métrica: lo demostró el frente T (`r31t/LEEME.md` §0): el hilo de render
  descartaba fotogramas y `cpu% × 1000/fps` repartía el trabajo entre menos fotogramas de los reales.
- Con cuentas: ejecuta menos que ninguna otra demo (T1) y aun así cuesta 20,2 ms: IPC 0,61, el peor.
- Dos rasgos medidos que van con eso: es la escena más dispersa (90 % del trabajo en 544 KB; cielo 330) y la
  que más trabajo por tiempo lleva (9,7 M PPC/s; cielo 4,1). El frente T mide 4,8 ms de sonido de sus 20,1.
- Que la dispersión sea **la causa** del IPC bajo es una inferencia, no una medida: lo confirmaría el punto 0 de §4.

## 4. Recomendaciones (ordenadas por ms por fotograma estimados)

0. **Antes de optimizar más a ciegas: contadores de hardware en la consola** (0 ms, pero decide el resto).
   Atmosphère tiene `enable_user_pmu_access` en `exosphere.ini`; con eso el runtime puede leer en cada
   fotograma instrucciones retiradas, ciclos, fallos de L1I, de L2 y de salto del núcleo del juego. Cambia
   un ajuste de la SD y pide reinicio: **necesita el visto bueno del usuario**. Coste: ~1 día. Convertiría en
   medida lo que aquí es estimación (IPC, y si el tiempo se va en caché de instrucciones, de datos o saltos).
1. **Instalar el PGO de m2 con el arreglo de texturas EFB del frente T**: −3,5 a −4 ms medidos en la consola
   (desierto 20,7 → 16,0-16,8; agua 26,4 → 22,4-23,0; cielo 23,2 → 19,2-20,1; nubes 19,5 → 15,8). Encaja
   con lo de aquí: el PGO pone seguido el camino caliente, que es justo lo que m2 tiene roto.
2. **Sonido en otro núcleo** (diseño del frente T, §4.1 de su LEEME): 3,6-5,7 ms por fotograma a ~31 fot/s,
   1,9-3,0 a 60. Mi dato: 4-11 M de instrucciones PPC por segundo van por tiempo.
3. **Separar el código frío del caliente encima del PGO** (`-freorder-blocks-and-partition`, que hoy choca con
   las tablas de saltos; probar con `-fno-jump-tables` solo en el código generado, y orden L propio). Estimación
   **gruesa**: −1 a −2,5 ms. Sale de: ~0,35-0,43 M de fallos de L1I por fotograma medidos en x86 (hilo entero,
   caché de 64 KB) × ~12 ciclos, y de que la mitad de los bytes de las funciones calientes no se ejecuta nunca.
   Es una prueba de flags barata; el número no está respaldado por una medida en la consola.
4. **Sondeo de interrupciones**: 0,8-1,2 ms de camino lento por fotograma medidos, más ~5 % de las
   instrucciones en la comprobación en línea (53 000-114 000 sondeos por fotograma). `NWII_POLL_EVERY` del
   frente T ya ahorra ~0,6 ms.
5. **Protocolo de llamada: prioridad baja.** Es el 25-29 % de las instrucciones, pero c3 quitó la parte de
   pc/lr/`inst_count` (~12 % de las instrucciones) y la consola solo bajó 0,7-1,2 ms: son instrucciones baratas.
   Quitar todo lo que queda (volcados, recargas, entrada: ~15 %) daría como mucho ~1 ms.

Cuenta de la meta: a 60 fot/s hay 17,0 M de ciclos por fotograma. Hoy el hilo gasta 6,4-9,7 ciclos por
instrucción PPC con 2,0-3,5 M de instrucciones: desierto y nubes rozan, agua y cielo no caben sin 1 + 2.

## 5. Cómo repetir (sin IA)

```
cd ~/nsmbw-port/r9/r32dx
# 1) fuentes partidos, mapa del fuente, objetos (otra laptop: ~25 min cada tanda con JOBS=6)
JOBS=2 python3 tools/split_lines.py ../r20g6/e2/export src && JOBS=3 python3 tools/srcmap.py
~/nsmbw-port/scripts/remoto.sh push r9/r32dx/tools r9/r32dx/src
~/nsmbw-port/scripts/remoto.sh run dx_objs 'JOBS=6 r9/r32dx/tools/robjs.sh nxg1 && JOBS=6 r9/r32dx/tools/robjs.sh pcgcov'
~/nsmbw-port/scripts/remoto.sh pull r9/r32dx/nx_g1 r9/r32dx/pc_gcov     # (pcgcov también vale aquí: JOBS=2, ~15 min)
# 2) ELF con líneas (comprueba .text idéntico a m2) y tablas estáticas
~/nsmbw-port/scripts/lim tools/link_g1.sh
python3 -I tools/a64_static.py ../r24m2/bin/nsmbw_m2.elf datos/a64/m2_static.npz
python3 -I tools/a64_lines.py nx/dxg1/nsmbw.elf datos/a64/m2_lines.npz
# 3) runtime y binarios de PC
JOBS=2 ~/nsmbw-port/scripts/lim tools/build_dx.sh rt
~/nsmbw-port/scripts/lim tools/build_dx.sh link && ~/nsmbw-port/scripts/lim tools/build_dx.sh gcovlink
# 4) corridas (toman el .runlock común) y análisis
tools/run_dx.sh bin/NSMBW_dxgcov gcov60 330
tools/run_dx.sh bin/NSMBW_dxgcov gcov30u 620 NWII_DX_POLL_BURN_NS=3000
PROFILE=p2 tools/run_dx.sh bin/NSMBW_dx base60 300
for r in gcov60 gcov30u; do JOBS=3 python3 tools/gcov_seg.py runs/$r _main; python3 tools/dx_report.py $r > runs/$r/report.txt; done
python3 -I tools/consamp.py && python3 tools/dx_ipc.py --r30 gcov30u > runs/ipc.txt
python3 tools/dx_tablas.py gcov60 gcov30u > datos/tablas.md
# 5) Dolphin (~5 min, sin ventana)
cd dolphin && python3 -I dolphin_ref.py --secs 900 --out $PWD/serie1.tsv && python3 ../tools/dolphin_seg.py serie1.tsv
```

## 6. Avisos

- `bin/NSMBW_dx*` llevan entrada por guion: **solo PC**. No hay NRO de este frente.
- El perfil T01 es de tp1 (m2 + muestreador del frente T, +0,5-0,7 %).
- Las direcciones son de la versión USA (SMNE01).
- `tools/dxlib.lib_of` usa la tabla de subsistemas del frente T (`r31t/tools/fases.py`), copiada.
