# r16a2: menos audio en el hilo del juego (frente A2, 8-nov-2026)

Base: `r9/r13` (r13b). Sin commits. Se compila con `tools/variant_build.sh` (copia de la de r14r5, que además
compila **copias editadas de archivos generados** de `export_mod/` con las mismas opciones que la base y cambia su
objeto en el enlace). No toca ninguna cabecera del runtime.

## Qué cambia (3 cosas)

### 1. Prioridad del hilo AX (núcleo 2): 0x3B → 0x2C
`pthread_create` de libnx crea **todos** los hilos con prioridad 0x3B (la más baja). El hilo que mezcla el audio
(AX) estaba por debajo del worker de render (0x2D) en el mismo núcleo 2, así que cuando el worker tenía trabajo el
mezclado esperaba, y el hilo del juego se quedaba **bloqueado esperándolo** (`ax::finish()`, o el mutex de `submit()`
que el hilo AX tenía cogido cuando lo echaron). En la consola (r13b, demo del desierto) eran **55-69 ms de espera por
segundo** en las ventanas pesadas, más 55-72 mezclas por segundo hechas en el hilo del juego ("inline").
Ahora el hilo AX se pone a 0x2C al arrancar (por encima del worker, por debajo de la salida de audio 0x2B).
Es **el mismo código que R5 a12** (`ax_async.cpp` idéntico al de `r9/r14r5`), para que se junten sin conflicto.
Interruptor: `NWII_AX_PRIO` en `debug.toml` (`"0x3B"` = como antes). Log: `[NX] AX thread on core 2, priority 0x2C`.
No cambia el audio: la mezcla es el mismo cálculo, solo termina antes.

### 2. Reverb AXFX (`nwii_r9_axfx_fast`, en `output_24.cpp`): "kernel 2"
Era lo más caro del hilo del juego después de dormir: 3,84 % de todas las muestras (≈ 8 % del tiempo ocupado),
~100 µs por llamada, 333 llamadas/s. El perfil por instrucción (`tools/annot.py`) mostró que casi todo eran
**esperas a memoria**: cada línea de retardo (3 canales × 10 buffers) se lee una vez por periodo de retardo, así que
cada línea de caché es un fallo, y el prefetcher del A57 no sigue ~35 flujos a la vez. Además cada operación iba en
`double` con redondeo a `float` y vuelta (`fcvt` + `fcvt`), y cada escritura pasaba por `ppc_single_bits` con saltos.

El bucle ahora es una función aparte con dos versiones (`NWII_AXFX_KERNEL`):
- **1** = el bucle r9 de antes, sin tocar (para comparar).
- **2** (por defecto) = las **mismas operaciones, en el mismo orden, en `float`**. Todos los operandos son de
  precisión simple (vienen de `lfs` o de operaciones simples; se comprueba en cada llamada y si no, se usa el 1), y
  para + − × de dos floats `(float)a op b` es bit a bit igual a `(double)(float)((double)a op (double)b)` (el producto
  es exacto en double y el doble redondeo de la suma es inocuo: 53 ≥ 2·24+2). `ppc_single_bits` de un valor simple
  da sus bits de float. Mismo orden de lecturas y escrituras por canal. Más **prefetch por software** de todos los
  flujos (al empezar, 2 líneas; luego cada 16 muestras, 32 muestras por delante).
- **ab** = alterna 1 y 2 llamada a llamada (mismo resultado) para medir los dos en la misma partida.

Otros interruptores: `NWII_AXFX_PREFETCH=0` (kernel 2 sin prefetch), `NWII_AXFX_TIME=0` (sin cronómetro),
`NWII_AXFX_KCHECK=n` (prueba: cada n llamadas ejecuta el 2, guarda el resultado, restaura las regiones y ejecuta el 1,
y compara byte a byte; es barato porque las regiones que se escriben son disjuntas y conocidas).
Línea `[A2] axfx N calls: kernel1 X us/call (n), kernel2 Y us/call (n), kcheck ok=.. bad=..` cada 3000 llamadas.

### 3. Delay AXFX (`func_801A5C10`) nativo
Igual que se hizo con la reverb en r9: `tools/apply_a2_delay.py` copia el cuerpo (de `loc_801A5C24` a
`loc_801A5E70`) a `nwii_a2_delay_native` con los registros de la Wii en variables locales (solo enteros; `xer`,
`cr0`, `ctr`, `inst_count` incluidos) y los mismos accesos a memoria en el mismo orden; sin el sondeo del `bdnz`
(solo corre con MSR[EE] apagado). 0,26 % de las muestras en r13b. Interruptores: `NWII_AXFX_DELAY_NATIVE=0`,
`NWII_AXFX_DVERIFY=n` (prueba: compara con el código generado toda la RAM y los registros). Va en el binario
**a2d**; el binario **a2** no lo lleva.

### 4. Desde las 21:40 (solo en `combo_r5`, ver abajo)
- **Prefetch de la llamada siguiente** (`[a2pfnext]`): al final del kernel 2 se piden a L2 (PSTL2KEEP) las 95 muestras
  siguientes de cada línea de retardo, que son justo las que leerá la llamada de dentro de 3 ms.
  `NWII_AXFX_PFNEXT` = `0` no (**por defecto**), `1` sí, `ab` = alterna en bloques de 1000 llamadas; la línea `[A2]`
  da `kernel2` (sin) y `kernel2+next` (con) por separado. El audio es el mismo. **En la consola es peor**: 30,1 µs sin,
  31,1 µs con (`bench_a2dr5_1108_2211`), así que va apagado; queda el interruptor para probar.
- **Caché de la comprobación de solapes** (`[a2ovcache]`): la prueba de ~630 pares (~0,1 % del hilo en la consola)
  solo se hace si la lista de regiones no es una de las 4 últimas que ya la pasaron. Contador "overlap check skipped".
  **Casi nunca acierta** (2 de 78 000 llamadas en PC: los buffers aux cambian de dirección): no estorba, pero no ahorra
  nada; separar las regiones fijas (líneas de retardo) de las que cambian ahorraría ~0,05 % como mucho.
- Al regenerar, `export_mod/output_24.cpp` es esta versión; la que se probó en la consola como a2d está guardada en
  `export_mod/output_24.cpp.a2d` (y la de a2 en `.a2`).

## a2d sobre R5 (`combo_r5/`)
Runtime de R5 tal cual (`r9/r14r5/nWiiRuntime`, a13: no cambió nada después de compilar a13) + el `output_24.cpp` de
este frente. El runtime de este frente frente a R5 **no tiene ninguna diferencia** (el cambio de prioridad ya es el de
R5), así que todo el parche es código generado:
- **`a2d_vs_r14r5.patch`**: solo `export/output_24.cpp` (contra `r9/r13/export`, que es la base que usa R5). Se aplica con
  `patch -p1` dentro de una copia de `r9/r13` (probado: reproduce el archivo byte a byte).
- `r16a2_vs_r13.patch`: lo mismo + `ax_async.cpp` (para bases que no son R5).
- Verificación en PC (AX en línea, 200 s): kernel 2 frente a 1 en cada llamada 51 000/0; reverb `VERIFY=50` 1051/0;
  delay `DVERIFY=20` 1301/0. Tiempos en PC (LP-E): kernel 2 10,3 µs, kernel 2 + prefetch de la siguiente 11,9 µs (en PC no
  ayuda: las cachés son grandes; lo decide la consola).
  `RESS=1 checks.sh` de `NSMBW_a2dr5_input_test`: title 220/220, map 220/220, walk 220/220, lvl11 200/200 idénticas,
  `diff_lines=0` (`combo_r5/runs/checks_1x.txt`).
- Binarios: `combo_r5/bin/nsmbw_a2dr5.nro` / `.elf`, `NSMBW_a2dr5` (PC). Se compila con
  `cd combo_r5 && BASE=/home/djvemo/nsmbw-port/r9/r13 JOBS=2 ~/nsmbw-port/scripts/lim ./tools/variant_build.sh . a2dr5 both`.

## Estado final (22:30)
- **Lo que vale** es `a2d_vs_r14r5.patch` (= `combo_r5/export_mod/output_24.cpp`, PFNEXT apagado) y `combo_r5/bin/nsmbw_a2dr5.nro`
  (recompilado 22:27 con PFNEXT apagado; el que corrió en la consola a las 22:11 era el mismo con PFNEXT en `ab`). Se lo pasé
  al frente de fusión (`r9/r18m`).
- Lo que se puede hacer después: el kernel 2 sigue esperando memoria (~30 µs/llamada en la consola, ~1 % del hilo del juego);
  `__AXSyncPBs` (`func_801A2B10`, 0,56 %) y las `DC*` no se tocaron.
- Ojo para quien toque el código: en la Switch **no se puede leer `cntvct_el0`** desde el programa (Horizon lo atrapa:
  así se cayó el primer a2); usar `cntpct_el0` como `armGetSystemTick`.

## Binarios (`bin/`)
| Binario | Qué lleva |
|---|---|
| `nsmbw_a2.nro` / `.elf` | 1 + 2 |
| `nsmbw_a2d.nro` / `.elf` | 1 + 2 + 3 |
| `NSMBW_a2`, `NSMBW_a2d` (+ `_input_test`, solo pruebas) | lo mismo en PC (la prioridad es solo de la Switch) |

## Verificación en PC (todo con `tools/audio_run.sh`: título + demos sin mandos, NAND privada, bajo `.runlock`)

| Prueba | Resultado |
|---|---|
| Kernel 2 frente a kernel 1 en **cada** llamada (`NWII_AXFX_KCHECK=1`), tres corridas (con el AX en su hilo y en línea) | 48 000 + 33 000 + 45 000 llamadas, **0 diferencias** (regiones escritas y estado) |
| `NWII_AXFX_VERIFY=50` (cuerpo nativo de la reverb frente al código generado; toda la RAM y los registros), AX en línea (`NWII_AX_THREAD=0`) | 651/651 y 901/901 bien, 0 mal |
| (con el AX en su hilo salen algunos "mal" **también en r13 sin tocar**: 4 de 401; el hilo AX escribe RAM mientras se compara. No es de este frente) | |
| Delay nativo, `NWII_AXFX_DVERIFY=20`, AX en línea | **1101/1101 bien**, 0 mal (RAM, registros, `xer`, `inst_count`) |
| `RESS=1 r9/render/test/checks.sh` (a2d) | title 220/220, map 220/220, walk 200/200, lvl11 220/220 idénticas, `diff_lines=0` (los "pass1 differs" salen igual en r13) |
| WAV de r13 frente a a2 con reloj virtual (`DET=1`) | el sonido **empieza en otro momento** (29,5 s frente a 34,2 s: el modo DET no es determinista del todo, el VI sale del bucle del anfitrión); alineados en el primer sonido, **idénticos durante 123 s**; después se separan. **r13 contra r13** (dos corridas iguales) hace exactamente lo mismo: inicio 29,5/34,2 s y separación en el mismo +123,021 s; o sea, la diferencia es del modo DET, no del cambio. `tools/wavalign.py` |

## Medidas

**PC** (núcleos LP-E, `PROFILE=sw`, `NWII_AXFX_KERNEL=ab`): kernel 1 27,5 µs/llamada → kernel 2 **11,6 µs** (×2,4).

**Consola** (portátil, OC 1785, demo del título 150 s; `switch/real_logs/bench_r16a2b_1108_2121`, `bench_r16a2d_1108_2124`,
comparadas con `bench_r5a12_1108_1905` = R5 a12, que ya lleva la misma prioridad del hilo AX, y `bench_r13b_1108_1813`).
Muestras del hilo del juego (`tools/shares.py`, ms de CPU por segundo):

| | r13b | R5 a12 | a2 | a2d |
|---|---|---|---|---|
| reverb (`nwii_r9_axfx_fast` + kernels) | 35,0 ms/s (3,98 %) | 29,1 (3,49 %) | **10,2 (1,24 %)** | **8,9 (1,07 %)** |
| delay (`func_801A5C10` / nativo) | 2,3 (0,26 %) | 2,3 (0,28 %) | 0,7 | **0,3 (0,03 %)** |
| espera al hilo AX (`svcWaitProcessWideKeyAtomic`) + mutex (`svcArbitrateLock`) | 2,59 % + 4,05 % | 0 + ~0,3 % | 0 + 0,39 % | 0 + 0,30 % |
| `[A2]` kernel 2 | | | 29,6 µs/llamada | 29,4 µs/llamada |
| `ax thread ... wait` / `inline` | 55-69 ms/s en lo pesado | 0 / 0 | 0 / 0 | 0 / 0 |

- Reverb + delay: **31,4 → 9,2 ms/s ≈ −2,2 % del tiempo del núcleo 1 a 1785 MHz** (a 1020 MHz, ~−3,9 %: el coste es
  sobre todo esperas a memoria, que no bajan con el reloj, así que la cifra real a 1020 está entre las dos).
- La prioridad del hilo AX (igual que R5) quitó ~6,6 % de esperas del hilo del juego frente a r13b.
- **a2dr5** (R5 a13 + esto, `bench_a2dr5_1108_2211`, con PFNEXT en `ab`): sin cuelgues, media 50,3 / mín. 30,0; reverb
  11,3 ms/s (1,35 %; la mitad de las llamadas con el prefetch de la siguiente, que es peor), delay 0,5 ms/s, `ax wait 0`.
- Media/mín. de fps del banco: a2 47,6/30,0, a2d 44,5/30,0, a12 49,9/30,8. **No sirven para comparar**: el orden de las
  demos cambia de una corrida a otra (lo dijo el coordinador); hay que mirar por escena o por perfil.
- Lo que queda del kernel 2 en la consola sigue siendo esperar a memoria (perfil por instrucción: las cargas de las líneas
  de retardo y el final de la cadena); el prefetch del lambda tiene 0,24 % propio (los PRFM esperan sitio).

## Cómo repetir
```
cd ~/nsmbw-port/r9/r16a2
BASE=/home/djvemo/nsmbw-port/r9/r13 JOBS=2 ~/nsmbw-port/scripts/lim ./tools/variant_build.sh . a2d both
flock ../render/pc/.runlock ./tools/audio_run.sh bin/NSMBW_a2d prueba 150 NWII_AX_THREAD=0 NWII_AXFX_KCHECK=1 NWII_AXFX_VERIFY=50 NWII_AXFX_DVERIFY=20
DET=1 flock ../render/pc/.runlock ./tools/audio_run.sh bin/NSMBW_a2d det 200   # WAV en runs/det/audio.wav
python3 -I tools/annot.py <elf> <profile_cpu.txt> nwii_a2_axfx_k2   # muestras por instrucción
```
Para rehacer `export_mod/output_24.cpp` desde la base: copiar `r9/r13/export/output_24.cpp`, cambiar el bloque de
`nwii_r9_axfx_fast` por `export_mod/axfx_fast_a2.inc` y pasar `tools/apply_a2_delay.py`.
