# r26x1: frente X1 — funciones calientes de nw4r / RVL SDK / MTX / MSL en nativo ARM exacto (10-nov-2026)

> **Resumen.** Sobre **r7f** (`r9/r22r7`; m2 todavía no tenía LEEME verificado) se ponen en nativo **22 funciones
> calientes del hilo del juego = 12,6 % del hilo ocupado** (rank.tsv de r25dec, r7f a 1020 MHz):
> - las 16 nativas "a mano" del frente N (r9/r19n: N2-N5 + DC*), que **no estaban en la troncal** (N quedó
>   en r19n, sobre m1): 9,1 % del hilo;
> - **6 nuevas de X1** (2,2 %): `sqrt` de fdlibm (802E8868, 1,23 %), `PSMTXConcatArray` (801C0610), las curvas
>   de animación g3d `80240100` / `80240240`, y `802381A0` / `80238200` (MTX34 de nw4r::math);
> - **`802403C0`** (animación g3d, 1,24 %; N4 la tenía en double con la emulación de cada instrucción) reescrita
>   en **single del anfitrión** (FMA en una instrucción `fmadd s` en la Switch).
>
> Todo **exacto**: mismos registros (también volátiles y ps1), misma memoria, mismo `inst_count`, mismos sondeos
> de interrupción (con el estado exacto en los que van por la ruta lenta). Verificación: comparador por
> función (difftest) **0 diferencias** en la versión final (~20 000 casos dirigidos + aleatorios en total; dos
> fallos encontrados por el comparador en versiones intermedias y arreglados, §2); modo verificación con datos
> reales del juego en PC (x1e: 4 606 comparaciones, 0 malas); checks 1x idénticos, sanity y Coin Battle bien (§4). NRO: **`bin/nsmbw_x1.nro`**
> (+ `.elf`; = x1e, GL, orden L). Medida en PC x86: **no concluyente** (x86 no tiene FMA en single en línea en la
> compilación base y el PC estaba cargado por otros agentes; §5) → la A/B buena es la consola (mismo binario,
> `NWII_NATIVE="0"`, que ahora sí funciona en la Switch).

Sin commits. No se tocó `r24m2`, ni `nand/`, ni la consola. Todo lo compilado/enlazado/comparado pasó por
`scripts/lim` (JOBS=2).

## 1. Qué se reemplazó

`tools/apply_x1.py` (copia de `r19n/tools/apply_n1.py` con la tabla nueva y la clase `x1`): una línea al principio
de cada función generada, `if (ctx.pc == 0xADDR && (slot.mode == 1 ? x1h_ADDR(ctx) : n1_hook_slow(...))) return;`;
el cuerpo generado queda debajo intacto (respaldo y referencia del modo verificación). Cuerpos:
`nWiiRuntime/src/hle/native_n1.cpp`, `native_n2.cpp` (de r19n, sin cambios) y **`native_x1.cpp`** (nuevo).

| dir. | función | % hilo (r7f) | origen | método |
|---|---|---|---|---|
| 801C0540 | PSMTXConcat | 1,59 | N2 | single en el anfitrión |
| 802403C0 | curva g3d (claves 32 bits) | 1,24 | **X1** (antes N4 en double) | single del anfitrión |
| 802E8868 | **sqrt** (fdlibm e_sqrt) | 1,23 | **X1** | bucles bit a bit sin saltos dependientes de datos, sondeos por lotes |
| 80237BD0 / 80237C40 | SinFIdx / CosFIdx | 1,03 / 0,34 | N2 | single |
| 801C9A80 / 801C9940 | GXLoadTexMtxImm / PosMtxImm | 0,98 / 0,34 | N2 | single, escribe el FIFO igual |
| 801AC440 / 801AC470 | DCInvalidateRange / DCFlushRange | 0,87 / 0,66 | N1 a mano | forma cerrada + sondeos por lotes |
| 802E1AB4 | strcmp | 0,75 | N5 | bucles en el anfitrión |
| 801C0500 / 801C04D0 | PSMTXCopy / Identity | 0,69 / 0,29 | N2 | |
| 802382B0 | matriz de rotación (nw4r::math) | 0,62 | N5 | |
| 801C07A0 | PSMTXInverse | 0,36 | N3 | |
| 801C0610 | **PSMTXConcatArray** | 0,28 | **X1** | single; pipeline del SDK reproducido (§2) |
| 802DCC4C | __cvt_fp2unsigned | 0,27 | N3 | |
| 80240240 | **curva g3d (claves cuantizadas 6 B)** | 0,26 | **X1** | single |
| 802381A0 | **MTX34 escala** (nw4r::math) | 0,19 | **X1** | single |
| 801C0BD0 / 801C1160 | PSMTXTrans / MultVec | 0,17 / 0,16 | N3 | |
| 80240100 | **curva g3d (claves float 12 B)** | 0,14 | **X1** | single |
| 80238200 | **MTX34 × escala (traslación)** (nw4r::math) | 0,12 | **X1** | single |

**Total 12,6 %** del hilo del juego ocupado (2,2 % nuevas de X1 + 1,24 % reescrita + 9,1 % de N traídas a la
troncal). No se incluyó `searchNodeByID` (lo lleva el frente D1, `r9/r28d1`), ni las traducciones "fp/int" de
N1 (no ganaban nada).

## 2. Las nuevas (`native_x1.cpp`)

Contrato de N (`native_n1.h`): el cuerpo devuelve `true` solo tras dejar `ctx` y la memoria exactamente como el
código generado en su `blr`; si algo no se cumple devuelve `false` **sin tocar nada** y corre el código generado.

- **Coma flotante en single del anfitrión** (curvas, MTX34, ConcatArray, y 802403C0): todos los operandos son
  singles (cargas `lfs`/`psq_l`, resultados de instrucciones en single), y para singles la emulación de
  `cpu_context.h` da lo mismo que la operación IEEE en single: `fmuls` (producto exacto en double), `fadds/fsubs`
  (exacta o tan cerca del operando mayor que el segundo redondeo no cruza un punto medio), `fmadds/fmsubs/
  ps_nmsub` = `ppc_madd_s` + `ppc_single` = **una** FMA en single (`std::fma` → `fmadd s` en AArch64, comprobado
  en el ELF: sin llamadas a `fmaf`), `fres` = la propia `ppc_fres`. Se rinden ante resultados en el rango
  subnormal de single, infinitos, NaN (de entrada o calculados) y GQR que no sean de los tipos válidos.
  En las curvas el fotograma `f1` puede ser cualquier double finito: se compara en double y su `fsubs` se
  calcula como la emulación (`ppc_single(f1 - x)`).
- **Curvas g3d** (80240100, 80240240, 802403C0): primera/última clave, índice estimado por la pila (GQR3),
  búsqueda lineal con un sondeo por iteración (si todos serían "callados" solo se descuenta `poll_left`; si no, el
  código generado), clave exacta o Hermite. Marco de pila en una copia local que se escribe al final; todo lo que
  se lee antes de escribir nada. `ps1` = 1.0 en los registros que deja un `psq_l` W=1.
- **sqrt** (fdlibm, raíz bit a bit, 22 + 32 iteraciones, **57 sondeos por llamada**): con el reloj de pared
  `poll_left` va de 1 a 16, así que **siempre** hay sondeos por la ruta lenta: se hacen de verdad con el estado
  exacto de ese sondeo (registros, CR0/CR1, CA, `inst_count`, `ctx.pc`), y si entregan una interrupción el cuerpo
  vuelve ahí (el código generado reanuda por su `case`). Las iteraciones sin saltos dependientes de los datos
  (selecciones), `inst_count` con las sumas por bloque del propio código generado (que no siempre son las
  instrucciones ejecutadas: G5 suma por "superbloque"). x > 0 normal y ±0; negativos, subnormales, inf y NaN →
  código generado (escriben `errno` o normalizan).
- **PSMTXConcatArray**: el SDK la tiene en *software pipelining* (el prólogo empieza el elemento 0, cada vuelta
  termina el k y empieza el k+1, el epílogo termina el último) y guarda f28-f31 con `stfd` + `psq_st` (al
  restaurar, `ps1` vuelve como el single FTZ de lo que era). Primera pasada: todos los elementos y las
  comprobaciones (nada escrito); segunda: las escrituras en el orden del programa entre sondeos, y en un sondeo
  lento el estado intermedio exacto del pipeline (B del elemento k+1 en f6-f10, columnas a medias en f12/f31/f29).
  D == B (en sitio) vale; solape parcial → código generado.

**Dos fallos encontrados por el comparador y arreglados** (ninguno llegó a la NRO final):
- ConcatArray con D == B: la primera versión releía B de la memoria ya sobrescrita para los registros f6-f10
  finales (`difftest/runs/x1c_directed_s40000-40299.txt`, 1 DIFF solo en f6-f10). Ahora se guardan los B leídos (x1d).
- `x1_ram` (copia de `n2_ram` de N): con un tamaño variable mayor que la caché bloqueada (256 KB), `kLCSize - n`
  daba la vuelta y un puntero cualquiera "cabía" en la LC → lectura fuera de rango (SIGSEGV en
  `difftest/runs/x1e_all_fp_s500-559.txt`, curva 80240240 con un `count` aleatorio de ~65 000 claves; en el juego
  las tablas están en MEM1/MEM2 y no pasaba). Ahora se acota `n` antes (también en `n2_ram`, donde todos los
  tamaños son fijos y pequeños) (x1e).

Interruptores (entorno o debug.toml; **por defecto encendidos**): `NWII_NATIVE="0"` apaga todo,
`NWII_NATIVE_<NOMBRE>="0"` una (nombres: tabla de `tools/apply_x1.py`: SQRT, PSMTXCONCATARRAY, ANM80240100,
ANM80240240, ANM802403C0, MTX34SCALE, MATH80238200, PSMTXCONCAT, ...), `NWII_NATIVE_STATS=1`,
`NWII_NATIVE_TIME=1` (tiempo por llamada; con `NWII_NATIVE=0` mide el código generado: A/B con el mismo binario),
`NWII_NATIVE_VERIFY=n` (solo PC). **Cambio respecto a N:** `NWII_NATIVE` y todas las `NWII_NATIVE_*` están en
`keeps_zero` (`switch_platform.cpp`), así que **"0" sí apaga en la Switch** (en r19n se borraba).

## 3. Compilar

```
cd ~/nsmbw-port/r9/r26x1
tools/reapply.sh                       # export/ = copia de r18m/export (= la de r7f) + ganchos; escribe runs/mods.txt
cd ..; M="$(cat r26x1/runs/mods.txt)"  # output_25 26 27 34 43 44 (+ 24, la edición propia de r18m)
env BASE=$PWD/r14g5 MODS="$M" JOBS=2 ~/nsmbw-port/scripts/lim r26x1/tools/build_m.sh r26x1 x1e pc
env BASE=$PWD/r14g5 DK_LIB=deko3d MODS="$M" JOBS=2 ~/nsmbw-port/scripts/lim r26x1/tools/build_m.sh r26x1 x1e_dk3drel nx
cd r26x1 && env M=$PWD NACP_NAME="NSMBW PRUEBA x1 (r7f + N + X1)" ~/nsmbw-port/scripts/lim bash -c \
  "tools/build_hot.sh x1e_dk3drel $(cat ../r22r7/nx/hot/profiles.txt)"     # orden L (perfiles de r7f)
# bin/nsmbw_x1.nro/.elf/_input_test.nro = copias de bin/nsmbw_x1e_dk3drel_hot* (sin "dk3d" en el nombre: GL)
```

Parches para reaplicar: **`x1_runtime_vs_r7f.patch`** (`diff -ruN r22r7/nWiiRuntime r26x1/nWiiRuntime`:
CMakeLists + `native_n1.h`, `native_n1_fp.h`, `native_n1.cpp`, `native_n2.cpp`, `native_x1.cpp` + `keeps_zero`)
y **`x1_export_vs_r18m.patch`** (los ganchos; mejor regenerarlos con `tools/apply_x1.py`). Ninguna cabecera que
incluya el código generado cambia (`cpu_context.h` etc. idénticas: `build_m.sh` lo comprueba).

### Sobre m2 (r7f + G6 e2)

- `python3 tools/apply_x1.py <copia de r20g6/e2/export> --orig ""` **aplica limpio** (probado: los mismos 22
  ganchos en output_25/26/27/34/43/44), y **los cuerpos generados de las 22 funciones son idénticos** en e2 y en
  r18m (salvo los números de sitio de fastmem y la línea de traza que G6 quita): las nativas valen tal cual.
- G6 e2 copia callees pequeños en sus llamadores: de estas solo `__cvt_fp2unsigned` (81 sitios) y DC* (15/10)
  quedan copiadas; esas copias no pasan por el gancho (siguen siendo correctas, solo no aceleradas). Para
  acelerarlas, quitar esas tres de `r20g6/tools/inline_callees.txt` al regenerar e2.
- Pasos: runtime de m2 (`r24m2/nWiiRuntime_r7f_src` = r7f) + `x1_runtime_vs_r7f.patch`; compilar los 6
  `output_N.cpp` de e2 con gancho con las mismas opciones que `r20g6/e2/{nx_obj,pc_obj}` y cambiarlos en la línea
  de `r24m2/tools/build_m2.sh`; difftest A = objetos e2, B = e2 + ganchos (como `difftest/objdirs.sh`).

## 4. Verificación

### Comparador por función (`difftest/`, PC, objetos de r7f contra r7f + ganchos)

`difftest/objdirs.sh` (A = objetos de r14g5 + output_24 de r22r7; B = los mismos con los `mod_output_N.o` de
r26x1), `build.sh` / `build_relink.sh`. Opciones nuevas: estados dirigidos de X1 (`x1_regs`: curvas reales con
GQR variados, fotogramas que no son single, sqrt de todas las magnitudes y ±0, matrices limpias, ConcatArray de
2-21 elementos con D == B, "interrupción" en uno de los primeros sondeos lentos), las escrituras de memoria de
esos estados se reponen tras `restore_memory()` (antes se perdían: la primera tanda probaba las curvas sobre
memoria aleatoria), y `--clean` para `--bench`.

| Tanda | Casos | Resultado |
|---|---|---|
| X1 dirigida, 6 funciones (x1a), semillas 1000-1199 | 1 200 | 0 dif. |
| X1 dirigida (x1a), 20000-20399 | 2 800 (91 con interrupción en un sondeo) | 0 dif. |
| N dirigida, 15 funciones, 3000-3099 | 1 500 | 0 dif. (7 NANSIGN de N, como en r19n) |
| todas, aleatoria, 1-100 | 2 100 | 0 dif. |
| X1 + ConcatArray (x1c), 40000-40299 | 2 100 | **1 DIF** (ConcatArray en sitio, f6-f10) → arreglado (x1d) |
| ConcatArray (x1d), 40000-40299 | 300 | 0 dif. |
| X1 dirigida con estados buenos (final), 70000-70399 | 2 800 (135 con interrupción) | 0 dif. |
| X1 dirigida sin NaN, 80000-80199 | 1 400 | 0 dif. |
| todas aleatoria 200-299 / N dirigida 9000-9059 | 2 200 / 900 | 0 dif. (1 NANSIGN de N) |
| todas `--fp` 500-559 (x1d) | 1 320 | **1 SIGSEGV en 80240240** (puntero aleatorio, ver §2) → arreglado (x1e) |
| **x1e (final)**: todas `--fp` 500-579 / X1 dirigida 90000-90299 / todas aleatoria 300-359 | 1 760 / 2 100 / 1 260 | 0 dif. |

Cobertura (con `NWII_NATIVE_STATS=1`, estados buenos): sqrt 29/30 nativas, ConcatArray 27/30, curvas 21-26/30,
MTX34 29/30 (el resto: entradas que deben ir al código generado).

### Juego en PC

| Prueba | Binario | Resultado |
|---|---|---|
| `NWII_NATIVE_VERIFY=2000` + `NWII_AX_THREAD=0`, demos del título 300 s | x1a | 4 701 comparaciones, 0 malas |
| idem | x1d | **5 979 comparaciones, 0 malas**; las 22 nativas se usaron (`runs/vtitle_x1d.txt`: PSMTXConcat 1 195 180 nativas / 0 rechazos, sqrt 217 899 / 0, ConcatArray 42 404 / 0, curvas 802403C0 90 %, 80240240 55 %, 80240100 50 %) |
| idem | x1e (final) | **4 606 comparaciones, 0 malas**; sqrt 158 908 nativas / 0 rechazos, ConcatArray 33 011 / 0 (`runs/vtitle_x1e.txt`) |
| `checks.sh` 1x (title/map/walk/lvl11) | x1d | **220/220 idénticas en las 4, diff_lines=0** |
| `checks.sh` 1x | x1e (final) | **title/map/walk 200/200, lvl11 220/220 idénticas, diff_lines=0** |
| det título (reloj virtual, cuadro a cuadro contra r7f) | x1d | idénticos hasta la copia 350, luego divergen: **no concluyente**, es el límite conocido del arnés (m1 contra m1 en r19n: igual desde la 350; r16w: "r13 contra r13" igual) |
| sanity (título → menús → mapa → 1-1, 165 s) / Coin Battle 4J (200 s) | x1e (final) | llegan a sus escenas (STAGE / MULTI_PLAY_COURSE_SELECT → STAGE), 54,9 / 56,8 fps, sin cuelgues, FASTMEM 0 fallos (`runs/sanity_x1e.txt`, `runs/cb4_x1e.txt`) |

El modo verificación nunca compara sqrt (todas sus llamadas toman sondeos lentos, que no se pueden deshacer):
sqrt la cubren el comparador (con interrupciones en sus sondeos) y det/checks.

## 5. Tiempos (PC) — no concluyentes

- Juego, `tools/time_x1.sh` (desierto4j + agua_pinguino, 150 s cada lado, mismo binario x1a, `NWII_NATIVE_TIME=1`
  con y sin `NWII_NATIVE=0`): `runs/pc_time_x1a.txt`. Los ns por llamada salen **2-3× los de r19n** para las
  mismas funciones (el PC estaba cargado por las compilaciones y corridas de otros agentes) y la mayoría de las
  nativas en single salen más lentas en x86 (PSMTXConcat 361 frente a 285 ns, sqrt 897 frente a 692...).
- Aislado (`difftest --n2 [--clean] --bench`, mínimo de 200 llamadas por estado): `difftest/runs/bench_*.txt`.
  Ruido de ±20-30 % entre corridas; con `--fp` las nativas en single caen en productos subnormales, que en x86
  cuestan un microcódigo de ~100 ciclos cada uno (PSMTXConcat ×4,8 más lenta); con `--clean` quedan
  entre ×0,6 y ×1,3 del código generado.
- Por qué x86 no sirve aquí: la compilación de PC es `-march=x86-64` (sin FMA): cada `std::fma` en single es una
  llamada a `fmaf` de la libc, y los subnormales van por microcódigo; en la Switch `std::fma` es una instrucción
  `fmadd s` (comprobado en `nsmbw_x1.elf`: 16-32 `fmadd/fmsub` en línea, sin llamadas) y los subnormales no
  penalizan. En la consola, N2 (mismo método) bajó PSMTXConcat de 1,58 % a 0,99 % del hilo y GXLoadTexMtxImm de
  0,87 % a 0,53 % (r19n §4).

**Pedido a la consola** (coordinador): A/B con el mismo `bin/nsmbw_x1.nro`, lista fija de S, `NWII_NATIVE_TIME="1"`
en las dos y `NWII_NATIVE="0"` en una (ahora sí se respeta en la Switch): ns por llamada nativa/generada de las
22 y el hilo del juego. Si alguna sale más lenta, se apaga sola con `NWII_NATIVE_<NOMBRE>="0"`.

## 6. Binarios (`bin/`)

| Archivo | Qué es |
|---|---|
| **`nsmbw_x1.nro`** (+ `nsmbw_x1.elf`) | **el juego**: r7f + N + X1 (x1e), GL, orden L de r7f. NACP "NSMBW PRUEBA x1 (r7f + N + X1)" |
| `nsmbw_x1_input_test.nro` | solo pruebas con guion. **Nunca como juego** |
| `nsmbw_x1e_dk3drel*.nro/.elf` | las mismas con su nombre de compilación (con "dk3d" en el nombre arrancan con deko3d) |
| `NSMBW_x1e`, `NSMBW_x1e_input_test` | PC |
| `*_x1a*` ... `*_x1d*` | intermedias (x1c: fallo de ConcatArray en sitio; x1a-x1d: el de `x1_ram`; no usar) |

sha256: `runs/bin_sha256_x1.txt`.

## 7. Siguiente

1. Medir en la consola (§5).
   Curvas 80240100 / 80240240: la mitad de las llamadas del juego se rinden (`runs/vtitle_x1d.txt`): la búsqueda
   lineal da más vueltas que `poll_left` (≤ 16) y entonces se deja al código generado. Arreglo: calcular primero
   todo (búsqueda + Hermite, sin escribir), y luego reproducir los sondeos de la búsqueda con su estado exacto
   (como sqrt/ConcatArray). Si sqrt / curvas / ConcatArray ganan, lo mismo para `PSVECNormalize` (801C1470,
   necesita `ppc_frsqrte` y `fnmsubs` sobre un no-single: mezcla), `memcpy` (0,37 %: 6 caminos
   adelante/atrás, con sondeos), y las funciones grandes de g3d (`80250E70` CalcWorld 0,89 %: un intérprete de
   bytecode de nodos; `80248330` mezcla de animaciones 0,66 %; `802405B0` 0,47 %: despachador de las curvas, sus
   hojas ya son nativas).
2. Reaplicar sobre m2 cuando esté verificada (§3).
