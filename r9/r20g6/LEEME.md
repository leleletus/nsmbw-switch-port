# r20g6: frente G6 — el generador de código, más cerca del AArch64 nativo (9-nov-2026)

Objetivo: que **todo** el código traducido (no funciones sueltas) cueste menos en la Switch. Base: m1
(`r9/r18m`, la versión instalada). Sin commits. No se tocó ninguna carpeta de otro frente (lo necesario se
copió aquí), ni la NAND, ni la consola. Todo lo compilado pasó por `scripts/lim` con `JOBS=2`.

> Estado: ver "Etapas" abajo (se actualiza sobre la marcha).

## 1. Medida: ¿en qué se va el código generado? (perfil real de la consola)

Herramienta nueva `tools/hostprof.py`: toma un `profile_cpu.txt` de la consola y el `.elf` de esa NRO,
desensambla las funciones generadas que tienen muestras y clasifica **cada instrucción AArch64 muestreada**
(acceso a `ctx.<campo>` —con los desplazamientos reales de `CPUContext` en AArch64—, pila, memoria del juego
`[x, w, uxtw]`, `rev` = cambio de endianness, llamadas, saltos, constantes, FP...). Además, con `--export`,
cuenta instrucciones AArch64 por instrucción PPC.

m1, 4 jugadores (`switch/real_logs/bench_m1_perfil4j_1108_2330`, 28 322 muestras en código generado =
50,8 % del hilo del juego; el 36 % es espera): `prof_m1_4j.txt`.

| clase (muestras del código generado) | % |
|---|---|
| lecturas de memoria del juego (fastmem) | 20,1 |
| ALU | 15,5 |
| constantes (`mov/movk`, direcciones del juego de 32 bits) | 11,0 |
| `ctx.gpr` (volcados/recargas de registros de la Wii) | 8,1 |
| pila (`sp`: registros que GCC no pudo mantener) | 7,5 |
| `ctx.pc` (protocolo de llamada/retorno, sondeos) | 4,9 |
| `ctx.inst_count` (reloj virtual: volcado/recarga en cada llamada) | 3,5 |
| `ctx.cr` (flags volcados) | 2,9 |
| sondeo de interrupciones (`ctx.msr`, `poll_dec_due`, ...) | ~1,5 |
| FPR en `ctx` (`fpr`/`ps1`) | 2,4 |
| `rev` (byteswap) | 1,2 |

- **10,7 instrucciones AArch64 por instrucción PPC** (estático, ponderado por muestras).
- Casi todo lo que no es "trabajo" (ctx.gpr/pc/inst_count/cr, pila, constantes) está **alrededor de las
  llamadas entre funciones del juego** y en las entradas/salidas de función.
- `rlwinm`/`extsh` & cía. ya salen como una instrucción de campo de bits (GCC lo hace solo); el byteswap es
  1,2 %; `inst_count` ya va por bloque, no por instrucción.

### Hallazgo principal: el 30 % del código generado tenía "cada instrucción es un punto de reanudación"

`perf/apply_fastdispatch.py` puso la etiqueta `nwii_entry:` delante del `switch` de entrada de toda función
que tuviera el texto `ctx.pc = ctx.ctr; return;` — **pero ese texto también está dentro de cada `bctrl`**
(llamada indirecta). Resultado: 10 774 funciones con la etiqueta, de las que **8 347 no tienen ningún salto
computado local**. `perf/apply_r8_codegen.py` (R8) deja **todos** los `case` (uno por instrucción) en las
funciones con esa etiqueta, así que en ellas cada instrucción es un punto de unión y GCC no puede dejar nada
en un registro; G5 tuvo que excluir muchas (crecían > 1,2×) o salían peor.

Muestras de la consola (m1 4J) por tipo de función:

| funciones | nº | % del código generado |
|---|---|---|
| sin etiqueta, con G5 | 24 060 | 66,2 |
| **etiqueta sin salto computado** (el fallo) | 8 347 | **19,6** |
| **etiqueta con `bctr` local real** | 2 427 | **10,2** |
| sin etiqueta, excluidas de G5 | 1 279 | 3,8 |

De los 2 563 `bctr` locales, sólo 428 son tablas de saltos (`switch` de C); los otros 2 135 son llamadas de
cola por `ctr` (métodos virtuales).

## 2. Cambios (todos son pasadas opcionales sobre el código generado; cada una se puede quitar para A/B)

Por qué pasadas y no regenerar con NWiiRecomp: el `export/` de m1 sale de NWiiRecomp + 6 pasadas encadenadas
(fastdispatch, R8, r9 cpucode/psq/crdse/axfx, fastmem r12, A2, G5), cada una comprobando el sha256 de su
entrada. Regenerar obligaría a repetir toda la cadena y re-verificar todo; aplicar las mejoras como pasadas
sobre `r18m/export_a2/export` (= m1 antes de G5) y luego la pasada G5 (copia en `tools/apply_g5_regs.py`,
con 6 formas de sentencia nuevas, marcadas `[r20-g6]`) deja intacto todo lo demás y es compatible con N
(edita funciones concretas de `export/`) y F (reentrena sobre el `export/` final).
Del generador sólo se usa el analizador: `gen/jtdump` (= `main.cpp` de NWiiRecomp hasta el análisis, enlazado
con las librerías ya compiladas de `NWiiRecomp/build`, sin tocar ese árbol) vuelca, por función, sus límites y
los destinos de sus tablas de saltos (`gen/jt.txt`: 40 031 funciones, 428 tablas, 6 279 destinos; las mismas
428 tablas que dice `recomp/recomp.log`).

### Etapa 1 (`e1/`): sin funciones "todo es punto de reanudación" — `tools/apply_g6_entry.py`

En cada función con `nwii_entry`:
1. cada `bctr` local pasa a ser
   `switch (ctx.ctr) { case 0xT: ctx.pc = 0xT; goto loc_T; ... }` + `ctx.pc = ctx.ctr; nwii_dispatch(ctx); return;`
   con T = los destinos de tabla de esa función (o sólo la segunda línea si no tiene tabla: llamada de cola).
   Exacto: un destino de tabla salta a su etiqueta como antes; cualquier otro va a la función que contiene esa
   dirección (esta misma si está dentro) y se reanuda por su `switch` de entrada (o `resume_pruned`, que es lo
   que hacía `interpret_step` sin `case`), y al volver se retorna como tras cualquier llamada de cola. Son dos
   sentencias para que G5 sólo vuelque los registros en el camino de `nwii_dispatch`.
2. se quita la etiqueta y el `switch` de entrada se poda con la regla de R8 (sólo direcciones a las que algo
   puede volver: las que el código guarda en `ctx.pc/lr/srr0` o compara con `ctx.pc`, y los `case` de
   `run_game`, que incluyen todos los destinos de tabla). 630 410 `case` fuera, 71 848 se quedan.
3. G5 (mismas opciones que m1) con la lista de exclusión sin esas 10 774 funciones (`tools/g5_exclude_g6a.txt`,
   1 279 en vez de 2 463): 38 715 funciones transformadas (m1: ~37 500).

Orden: `cp -a r18m/export_a2/export e1/export_pre; apply_g6_entry.py e1/export_pre --jt gen/jt.txt --orig
../r18m/export_a2/export; cp -a e1/export_pre e1/export; apply_g5_regs.py e1/export --orig e1/export_pre
--eager --entry-fns --exclude tools/g5_exclude_g6a.txt --inline-leaves --leaves-from e1/export_pre`.

Medida estática (AArch64, mismas opciones que la Switch; `exp/`): instrucciones m1 → e1:
func_800ECAE0 14 030 → 10 383 (cargas de ctx 2 288 → 475), func_8029D300 29 076 → 21 999, func_80292C90
22 124 → 16 832, func_802821E0 10 723 → 8 225, func_809D2B60 13 695 → 9 901, func_80250E70 3 089 → 2 062,
func_8025DB30 2 091 → 1 578, GXLoadTexObjPreLoaded 576 → 448.

### Etapa 2 (`e2/`): sin prueba de traza + callees pequeños copiados en el llamador

- `tools/apply_g6_notrace.py`: quita `if (g_trace_calls && ctx.pc == F) trace_call(...)` de las 39 995
  funciones (carga global + prueba en cada llamada). Sólo imprimía con `NWII_TRACE_CALLS=1` (depuración); con
  esta pasada esa variable ya no hace nada.
- `tools/apply_g6_inline.py`: una llamada directa a un callee pequeño se sustituye por su cuerpo (sin su
  `switch` de entrada; etiquetas renombradas; su `blr`, la comprobación tras sus propias llamadas directas y sus
  llamadas de cola siguen en la comprobación del llamador; los `return` del sondeo de interrupciones y de sus
  llamadas indirectas salen del llamador, lo que deja `ctx` exacto y `run_game`
  reanuda en `ctx.pc` igual que cuando la comprobación del llamador fallaba). Ver el docstring para el razonamiento de exactitud. Guiado por perfiles de la
  consola (`tools/fnsamples.py` → `fn_weights.tsv`): callees con ≥ 3 muestras y ≤ 32 instrucciones PPC
  (`tools/inline_callees.txt`), sólo dentro de funciones con muestras (`tools/inline_callers.txt`): 4 930
  sitios en 1 191 llamadores, +68 924 instrucciones PPC copiadas (+4 % de código).

## 3. Verificación

### e1 (etapa 1)
- `cputest/`: 288/288, salida idéntica a la de G5.
- Comparador por función (`difftest/`, A = objetos PC de m1, B = e1; todas las 40 031 funciones):
  semilla 1: 40 030 iguales, 0 diferencias, 1 no concluyente; semilla 2: 40 029 iguales, 1 "diferencia" en
  `func_8021B738` que es del comparador: sin tabla de despacho, un `bctr` de cola a una dirección interna era
  `goto nwii_entry` → `interpret_step` en A y `nwii_dispatch` (que en el comparador no hacía nada) en B. Con
  la opción nueva `--dispatch ../gen/jt.txt` (despacho real a la función A_/B_ que contiene la dirección):
  **igual** (los dos acaban en `interpret_step`/`resume_pruned`, status 4). `difftest/runs/`.
  OJO: la primera versión de `--dispatch` reservaba 2×4 GB por proceso y, lanzada fuera de `lim` con 4
  procesos, provocó la caída de RAM del 9-nov 02:27. Arreglado (sólo funciones ≥ 0x80000000, ~24 MB, aborta
  si pasa de 64 MB); el comparador se corre siempre dentro de `scripts/lim`.
- `checks.sh` 1x: title/map/walk/lvl11 **220/220 idénticas, diff_lines=0** (`runs/checks_g6e1.txt`). El
  "pass1 differs" de title (18; m1 0) varía de una corrida a otra en otras versiones (r13 20, w4r5 0).
- Corridas con guion en PC (`tools/verify_g6.sh g6e1 sanity title cb4 long`, cada una con `.runlock`):
  las mismas escenas que m1, `[FASTMEM]` 0 fallos, sin cuelgues, `resume_pruned` 27-28 líneas por corrida y
  sólo en 0x801b119c / 0x801d0c10 (como m1). `runs/{sanity,title,cb4,long}_g6e1.txt`. **e1 verificada.**
- Tabla de fastmem: 0 entradas mal formadas; 69 sitios sin entrada (m1: 36): los 33 nuevos son código muerto
  que GCC ya puede probar tras la poda (revisados `func_808732E0`: `r0 ∈ {0,1}` y luego `cmplwi r0,2`;
  `func_802770D0`: `bne` y `beq` seguidos sobre el mismo cr0).

### e2 (etapa 2)
- Primer intento: el comparador encontró **25 funciones distintas** (semilla 1): 17 callees cuya dirección de
  entrada (la del nombre) no es su dirección más baja (p. ej. `func_8005E6E0`, que entra por un salto hacia
  atrás a 0x8005E570): la llamada directa entraba por su `switch` (`case 0x8005E6E0`) y la copia empezaba por
  el principio del cuerpo. Arreglado (esos callees no se copian: `entry not first`). Los binarios de ese
  intento están apartados en `bin/bad_e2_preinlinefix/` (no usar).

## 4. Etapa 3 (plan): cambiar el protocolo de llamada — lo que falta para "la mitad del coste"

Tras e1/e2 el coste que queda es casi todo el protocolo de llamada entre funciones del juego, que hoy es
"todo pasa por `ctx` en memoria": antes de cada `bl` el llamador escribe en `ctx` los registros sucios
(G5: 10-20 `str`), `ctx.pc`/`ctx.lr` (constantes de 32 bits), `inst_count`; el llamado comprueba
`ctx.pc != F`, monta `MemView`, carga de `ctx` lo que usa; al volver vuelca lo sucio, `ctx.pc = ctx.lr`; el
llamador compara `ctx.pc` con la dirección de retorno y recarga 10-20 registros. ~40-60 instrucciones por
llamada, más los volcados a pila que eso provoca (ctx.gpr 8 % + pila 7,5 % + ctx.pc 5 % + inst_count 3,5 % +
cr 2,9 % + buena parte del 11 % de constantes ≈ 30-35 % de las muestras del código generado).

Propuesta concreta (generador + pasada G6, mismo `CPUContext`, exacto):
1. **Firma nueva para las llamadas directas**: además de `void F(CPUContext&)` (la que usan `run_game`,
   `nwii_dispatch`, la reanudación), cada función tiene `F_d(CPUContext& ctx, Regs r)` con
   `struct Regs { uint32_t r1, r3..r10; uint64_t ic; }` pasados por valor (AAPCS64 los mete en x1..x7 +
   pila; con `__attribute__((pcs))` no hace falta: 8 argumentos enteros van en registros). Los argumentos PPC
   (r3-r10), la pila (r1) y `inst_count` viajan en registros del host; el resultado (`r3`, `r4`, `ic`) vuelve
   en x0/x1 (estructura de 16 bytes). `F` = "cargar Regs de ctx, llamar a `F_d`, guardar en ctx".
2. **ctx exacto sólo donde alguien lo mira**: dentro de `F_d` los registros recibidos son locales de G5
   desde el principio (sin cargarlos de `ctx`). Antes de todo escape que pueda mirar `ctx` (sondeo lento,
   MMIO, `nwii_dispatch`, HLE, retorno por desenrollado) se vuelcan, igual que hoy. El llamador, al llamar
   `F_d`, NO vuelca r3-r10/r1/ic (van como argumentos) y sólo vuelca los registros que el llamado puede leer
   de `ctx` en un escape (todos los sucios, para que el desenrollado reanude exacto) — pero el volcado pasa
   al camino lento: el llamado, en su primer escape, llama a un "volcado del llamador" (cadena de marcos con
   las locales vivas, como un unwinder). Esa es la parte cara de hacer; versión intermedia: volcar como hoy
   lo sucio que no sean r3-r10/r1, que es mucho menos (los callee-saved r14-r31 se tocan poco entre llamadas).
3. **`ctx.pc`/`ctx.lr` en el camino rápido**: con `F_d` la entrada no necesita `ctx.pc == F` (es directa) y el
   retorno normal devuelve un indicador en vez de escribir `ctx.pc = ctx.lr` y que el llamador lo compare:
   `F_d` devuelve 0 = "retorno normal a LR" (el llamador sigue, sin leer `ctx.pc`) o 1 = "se desenrolla"
   (ctx exacto, `ctx.pc` vale lo que toque; el llamador vuelca lo suyo y retorna 1). `ctx.lr` sólo se escribe
   si el llamado lo lee (`mflr`, casi siempre sólo en el prólogo para guardarlo en la pila: allí se escribe
   la constante directamente en memoria del juego, como hoy).
4. **`inst_count` fuera de `ctx` en el camino rápido**: viaja como argumento/retorno (punto 1) y sólo se
   escribe en `ctx` antes de un escape (sondeo, lectura del reloj, MMIO), que es exactamente lo que G5 ya
   hace con `G5ic` dentro de una función.
Exactitud: el estado del juego visible en cada punto observable (escapes, desenrollado, `run_game`) es el
mismo; lo que cambia es dónde viven los registros entre medias. Se verifica igual (comparador por función,
que compara el `ctx` final y todos los efectos, más las corridas).
Estimación: quita del orden de 25-40 instrucciones por llamada directa (de ~40-60) y casi todos los
`mov/movk` de direcciones de retorno; con ~1/3 de las muestras del código generado en ese protocolo, **−15 a
−25 % del código generado** (consola), además de lo de e1/e2. Junto con FPR en locales (ahora que los caminos
lentos de `psq_*` podrían ir por un stub que conserva registros) y el sondeo en un solo contador, es lo que
puede acercar el hilo del juego a la mitad. Coste: es un cambio del protocolo de **todas** las funciones
(generador + G5 + `run_game`/`nwii_dispatch`/HLE), varios días; se puede hacer por pasos (1 y 4 primero,
detrás de una opción, y A/B en la consola).
