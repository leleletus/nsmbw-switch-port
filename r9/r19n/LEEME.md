# r19n: n1, n2 = m1 + funciones de biblioteca en nativo 1:1 (frente N, 8/9-nov-2026)

> **Resumen (9-nov 01:45).** Consola: **n1 no ganó nada medible** (las 33 funciones pesan lo mismo en el hilo del
> juego con y sin ellas): copiar el cuerpo generado con los FPR en locales no basta, el coste es la emulación de
> la coma flotante de Broadway en double. **n2** cambia de método para las 7 más pesadas (PSMTXConcat/Copy/Identity,
> SinFIdx/CosFIdx, GXLoadTex/PosMtxImm): cálculo en single sobre punteros del anfitrión, exacto por construcción y
> con salida al código generado en los casos raros (subnormales, NaN copiados, memoria que no es RAM). NRO para medir:
> `bin/nsmbw_n2.nro`; **n3** (`bin/nsmbw_n3.nro`) = n2 + PSMTXInverse, PSMTXMultVec, PSMTXTrans y `__cvt_fp2unsigned`
> con el mismo método (11 funciones "rápidas" en total). Lista de toda la biblioteca por coste y estado: §5
> (`runs/ranking_lib.md`). Plan por rondas: §6.

Base: copia de `r9/r18m` (m1 = R5 + deko3d + RES + W + A2 + G5 + orden L). Objetivo: bajar el tiempo del hilo
del juego (núcleo 1) en las escenas pesadas de 4 jugadores sustituyendo funciones hoja del SDK (y alguna del
juego) por versiones nativas **exactas**: mismos registros (también los volátiles y el ps1 de los FPR), la
misma memoria escrita en el mismo orden, los mismos sondeos de interrupción y el mismo `inst_count`.
Sin commits. No se tocó ninguna carpeta de otro frente, ni `nand/`, ni la consola.

## 1. Qué se eligió (perfil de la consola)

Ranking por muestras del hilo del juego en `switch/real_logs/bench_m1_perfil4j_1108_2330` (m1, ventanas lentas
`SLOW_MS=18`, 55 753 muestras, 39 % ocioso) agregando el PC invitado por función (incluye los ayudantes del
runtime que llama cada función) y mirando el propio perfil del anfitrión (`psq_load`/`psq_store`/`ps_madds0/1`
fuera de línea, llamados casi solo desde PSMTXConcat, SinFIdx, la animación de g3d y GXLoadTexMtxImm).
Se escogieron las funciones **hoja** (no llaman a otras funciones del juego) con trabajo de coma flotante o
con bucles con sondeo:

| Grupo | Funciones (dirección) | % muestras perfil4j |
|---|---|---|
| MTX | PSMTXConcat 801C0540, Copy 801C0500, Identity 801C04D0, ConcatArray 801C0610, Inverse 801C07A0, InvXpose 801C08A0, Trans 801C0BD0, MultVec 801C1160 | 2,5 |
| nw4r::math | SinFIdx 80237BD0, CosFIdx 80237C40, SinCosFIdx 80237CA0, 802382B0, 802381A0, 802390D0 | 2,3 |
| animación g3d | 802403C0, 80240100, 80240240 (búsqueda de clave + Hermite, psq cuantizado) | 1,0 |
| GX | GXLoadTexMtxImm 801C9A80, GXLoadPosMtxImm 801C9940, GXInitTexObjLOD 801C6FF0 | 0,8 |
| caché | DCInvalidateRange 801AC440, DCFlushRange 801AC470 | 0,8 |
| MSL / runtime | memcpy 80004364, __fill_mem 80004600, strcmp 802E1AB4, __cvt_fp2unsigned 802DCC4C | 0,7 |
| otras | searchNodeByID 80162C30, CXUncompressLZ 801D8190, nw4r::snd 802835B0/80283760/802837C0/802838A0, juego 8000D0D0 | 1,2 |

En total **~9,4 % de todas las muestras ≈ 15 % del hilo del juego ocupado**. No entraron (y por qué): sqrt de
fdlibm (802E861C es una función "fusionada" con otra entrada), `__AXSyncPBs` (lógica grande, poco FP),
`OSRestoreInterrupts` (su coste es la entrega de interrupciones), código del juego grande no hoja
(`800ECAE0`: función fusionada con `interpret_step` y saltos a `nwii_entry`), y lo que es del frente W (escritura
del juego al FIFO de GX: `GX_WGPIPE_Write8` desde 8000D000).

## 2. Cómo se reemplazan

`tools/apply_n1.py` (reproducible; comprueba el sha256 de cada función contra `r18m/export` y se niega a correr
dos veces; parche resultante: `n1_export_vs_r18m.patch`, sha256 en §6). Cada función generada recibe una línea
al principio:

```cpp
if (ctx.pc == 0x801C0540 && nwii::runtime::n1_hook(n1s_801C0540, ctx)) return;  // [n1]
```

y su cuerpo generado queda **intacto debajo** (respaldo y referencia del modo de verificación). Dos clases:

- **"fp" / "int" — traducción mecánica del cuerpo generado** (`n1v_<dir>`, en el mismo `output_N.cpp`):
  copia exacta del código de G5 con
  - los FPR del invitado en variables locales (`ctx.fpr[N]`/`ctx.ps1[N]` → `fN`/`pN`), que el compilador
    deja en registros en vez de leer/escribir `ctx` en cada instrucción;
  - los ayudantes de FP de `CPUContext` (`ps_madds0`, `fmuls`, `fill`...) → macros `N1_*`
    (`nWiiRuntime/include/runtime/native_n1_fp.h`) que son **el mismo cuerpo** con `fpr[X]`→`fX`: mismas
    funciones `ppc_force25`/`ppc_madd_s`/`ppc_single`, mismo orden de operandos. En línea (antes PSMTXConcat
    llamaba a `ps_madds0/1` fuera de línea 15 veces por llamada);
  - `psq_l`/`psq_st`: la ruta float igual que `psq_l_fast`/`psq_st_fast` (mismos accesos `MemView`, mismos
    sitios de fastmem); los tipos cuantizados de GQR (u16 de SinFIdx, s16/u8 de la animación) **en línea** con
    el cuerpo de `CPUContext::psq_load/psq_store` en vez de la llamada genérica;
  - **sondeos de interrupción agrupados** (`N1Poll`, `native_n1.h`): un sondeo que la parte en línea del sondeo
    generado (`poll_due`) daría por "nada que hacer" (poll_left > 1 y MSR[EE] apagado o sin banderas) solo
    descuenta una copia local de `poll_left`; los demás se hacen de verdad, con el mismo `ctx.pc`, los mismos
    volcados de registros (y de los FPR locales) y la misma ruta lenta. Con el reloj virtual (`poll_left`
    siempre 1) todos los sondeos se hacen de verdad, como en el código generado. Las banderas se vuelven a leer
    tras cada sondeo real (como mucho cada 16 sondeos con el reloj de pared): si otro hilo pone una bandera,
    se ve unos sondeos después (lo mismo que si hubiera llegado un poco más tarde; nunca se pierde).
  - Todo lo demás carácter por carácter (registros G5, CR, `inst_count`, saltos, accesos a memoria).
  - Se niega (error) si la función llama a otra (no es hoja), usa `ctx` de otra forma, toca MSR, o es una
    función fusionada (entrada ≠ dirección).
- **"hand" — escritas a mano** en `nWiiRuntime/src/hle/native_n1.cpp`:
  - `DCInvalidateRange`/`DCFlushRange`: forma cerrada (la operación de caché no hace nada aquí) + sondeos por
    lotes con el estado exacto de cada sondeo real (r3, ctr, inst_count, pc) para que, si se entrega una
    interrupción, el código generado reanude en su `case` igual que antes. `sc` de DCFlushRange incluido.
  - `fLiMgBa_c::searchNodeByID`: el recorrido de la lista con los mismos accesos `MemView` y sondeos por lotes.

Interruptores (entorno o `debug.toml`):

| Clave | Efecto |
|---|---|
| `NWII_NATIVE = "0"` | todo apagado: el mismo binario se comporta como m1 (para A/B en la consola) |
| `NWII_NATIVE_<NOMBRE> = "0"` | apaga una función (nombres de la tabla de `apply_n1.py`: PSMTXCONCAT, SINFIDX, DCINVALIDATERANGE, STRCMP, ...) |
| `NWII_NATIVE_TIME = "1"` | (implica STATS) tiempo por llamada con el contador del sistema: con la función activa mide el cuerpo nativo; con `NWII_NATIVE="0"` ejecuta el código generado desde el gancho (mismo código y estado, una llamada más honda) y lo mide igual → A/B por llamada con el mismo binario: `[N1] <NOMBRE> time stats: N calls, X ns/call (native\|generated code)`. Añade unos ns por llamada a los dos lados |
| `NWII_NATIVE_STATS = "1"` | contadores por función (`[N1] <nombre> stats`, y `final` al salir) |
| `NWII_NATIVE_VERIFY = "n"` | modo verificación: la 1.ª, 2.ª, 4.ª, 8.ª... y cada n-ésima llamada de cada función ejecuta el cuerpo nativo sobre una instantánea de registros + toda la RAM (MEM1/MEM2/LC), guarda su resultado, restaura, ejecuta el código generado y compara registros (GPR, FPR y ps1 en bits, CR, XER, CTR, LR, PC, inst_count, poll_left...) y hashes de toda la RAM (`[N1] ... verify MISMATCH` si difieren; se queda el resultado generado). Las funciones que escriben MMIO/el FIFO de GX no se verifican así (no se puede deshacer). **Solo PC** (copia 88 MB por comprobación) |

Al primer uso cada función imprime `[N1] <NOMBRE> (0x...): native`.

## 3. Compilación

Como r18m (`tools/build_m.sh`, objetos de G5 v3 en `r9/r14g5` sin tocarlos) con
`MODS="output_1 output_21 output_24 output_25 output_26 output_27 output_28 output_34 output_38 output_43 output_44"`
y el runtime de r19n (= el de r18m + `native_n1.cpp` + las dos cabeceras nuevas; `cpu_context.h` y las demás
cabeceras que incluye el código generado **no cambian**: parche `n1_runtime_vs_r18m.patch`).

```
cd ~/nsmbw-port/r9
python3 r19n/tools/apply_n1.py r19n/export        # sobre una copia limpia de r18m/export (ya hecho)
M="output_1 output_21 output_24 output_25 output_26 output_27 output_28 output_34 output_38 output_43 output_44"
env BASE=$PWD/r14g5 MODS="$M" JOBS=2 ~/nsmbw-port/scripts/lim r19n/tools/build_m.sh r19n n1 pc
env BASE=$PWD/r14g5 DK_LIB=deko3d MODS="$M" JOBS=2 ~/nsmbw-port/scripts/lim r19n/tools/build_m.sh r19n n1_dk3drel nx
cd r19n && ~/nsmbw-port/scripts/lim bash -c 'tools/build_hot.sh n1_dk3drel $(cat nx/hot/profiles_n1.txt)'
```

`tools/build_hot.sh` = el de r18m con M=r19n, perfiles de r18m + `bench_m1_perfil4j` (peso 2) y
`bench_m1pre_titulo` (con `r18m/bin/nsmbw_m1.elf`), y además coloca cada variante `n1v_*` justo después de su
función generada y `native_n1.o` delante del runtime. Comprobaciones del enlace: `.text` del enlace base idéntico
al de la NRO sin orden; `hot_order_eqcheck`: 0 funciones distintas (solo campos de reubicación).


## 4. Resultados

### n1 en la consola (coordinador, 9-nov 00:21/00:28, `switch/real_logs/bench_n1_1109_0021` y `bench_n1_off_1109_0028`)

Mismo binario, `NWII_NATIVE="0"` en la segunda. Peso de las 33 funciones en las muestras ocupadas del hilo del juego
(PC invitado agregado por función; `tools/lib_rank.py`/script de comparación):

| corrida | fps medio (demo del título) | ventanas < 45 fps | las 33 funciones (% del hilo ocupado) |
|---|---|---|---|
| m1 (`bench_m1_perfil4j`) | 51,2 | 14/62 | 14,8 % |
| n1 con `NWII_NATIVE=0` | 52,6 | 8/62 | 14,3 % |
| **n1** | 51,1 | 11/62 | **14,8 %** |

**Sin ganancia.** Por qué (visto en el código AArch64): en PSMTXConcat la variante n1 hace ~785 instrucciones en el
camino normal, casi todas de la emulación exacta de cada operación paired-single en double (`ppc_force25`, FMA en
double, comprobación del empate, dos conversiones double↔single por carril), igual que el código generado; quitar
los accesos a `ctx` y las llamadas fuera de línea ahorra poco frente a eso. En x86 (difftest `--bench`) algunas
salían más lentas (PSMTXConcat ×1,18). Conclusión: el reemplazo tiene que **hacer menos trabajo**, no el mismo
trabajo en registros → n2.


### n2 en la consola (coordinador, 9-nov 02:07, `switch/real_logs/bench_n2_1109_0207`; NWII_NATIVE_STATS=1, 720p portátil)

Peso en el hilo ocupado (ventanas lentas; la corrida incluye el desierto 4J; los contadores de STATS pasan cada
llamada por `n1_hook_slow`, así que la versión normal es algo mejor):

| función | m1 | n1 apagado | n1 | **n2** |
|---|---|---|---|---|
| PSMTXConcat | 1,58 % | 1,68 % | 1,68 % | **0,99 %** |
| GXLoadTexMtxImm | 0,87 % | 1,25 % | 1,09 % | **0,53 %** |
| PSMTXCopy | 0,69 % | 0,75 % | 0,79 % | **0,60 %** |
| las 33 con gancho | 14,8 % | 14,3 % | 14,8 % | **13,5 %** |

Llamadas/s de los contadores (media de toda la corrida): SinFIdx 29 700, strcmp 55 300, searchNodeByID 24 100,
PSMTXCopy 22 500, Concat 16 000, cvt_fp2unsigned 14 400, Identity 13 600, GXLoadTexMtxImm 11 200, CosFIdx 8 000,
anim 802403C0 6 400, 802382B0 5 600, memcpy 4 000. Coste aproximado por llamada (muestras de las ventanas /
llamadas, sobreestima porque las ventanas lentas tienen más llamadas que la media): 802403C0 ~1,4 µs, 802382B0
~0,76 µs, memcpy ~0,63 µs, Concat ~0,38 µs. La A/B buena (mismo binario con `NWII_NATIVE=0`) está en la cola.

### n2: método "single en el anfitrión" (`nWiiRuntime/src/hle/native_n2.cpp`)

Para operandos que son singles (todo lo que carga `psq_l` de tipo float y todo resultado de una operación en
single), la emulación de `cpu_context.h` da exactamente lo mismo que la operación IEEE en single del anfitrión:
`ppc_force25(c) == c`, el producto de dos singles es exacto en double (así `ppc_single(a*c)` = `a*c` en float), y
`ppc_madd_s` + `ppc_single` es "FMA redondeada una vez" (lo que hace `std::fma` en float, una instrucción `fmadd s`)
en todo el rango normal. Solo pueden diferir con resultados subnormales en single (doble redondeo en esa posición):
cada valor calculado con 0 < |r| < 2^-125 hace que la función se rinda **antes de escribir nada** y corra el código
generado. Lo mismo con NaN que se copiarían sin operar (el camino en double los "silencia") y con subnormales que
`psq_st` pondría a cero. La memoria va por punteros del anfitrión solo si todo es RAM normal (MEM1/MEM2/LC), sin
`NWII_WATCH`, y sin solapes que hicieran importar el orden. Se dejan todos los registros como el código generado
(también los ps1 "pisados" de f14/f15/f31 en PSMTXConcat, que `lfd` no restaura), los bytes de la pila, CR,
`inst_count` y los sondeos (SinFIdx/CosFIdx tienen uno: solo se salta si su parte en línea no haría nada, si no
corre el código generado). GXLoad*MtxImm llama directamente a `GX_WGPIPE_Write8/32/F32` con los mismos valores en el
mismo orden (y `inst_count` puesto antes de la primera escritura, como el generado).

PSMTXConcat pasa de ~785 (n1) / ~1000 (generado) instrucciones a ~100-150 (36 operaciones en single + cargas y
guardados). Medida real: falta la consola (§7).


**Fallo encontrado y arreglado en N3 (prueba de que la verificación sirve):** la primera n3 leía las constantes de
`__cvt_fp2unsigned` de `0x8030D338` (cuenta mal hecha de `lis`/`addi`; lo correcto es `0x802FD338`). Lo vieron las
dos comprobaciones: el difftest dirigido (600 de 600 `DIFF` en esa función, ninguna en las demás) y el modo
verificación en PC con datos reales (`[N1] CVTFP2UNSIGNED verify MISMATCH`, r3/r4 distintos; extracto en
`runs/vtitle_n3_bug_cvt_caught.txt`). Esa NRO nunca salió; la de `bin/` es la arreglada.

## 5. Toda la biblioteca, por coste (lista viva)

`runs/ranking_lib.md` (150 filas; generar de nuevo con `tools/lib_rank.py <perfiles...>`, `TOP=n`). Media de
4 perfiles de ventanas lentas de la consola (m1 perfil4j, m1pre título, n1, n1 apagado). La biblioteca (RVL SDK,
nw4r, EGG, HBM, MSL/runtime de CodeWarrior por rango de direcciones) es **~70 % del hilo del juego ocupado**, muy
repartida: las 10 primeras suman 12 %, las 60 primeras 32 %. Primeras filas:

| # | dir. | biblioteca | función | % ocupado | acum. | estado |
|---|---|---|---|---|---|---|
| 1 | `80225330` | nw4r | IOS_SeekAsync | 1.770 | 1.77 | E/S del disco (HLE del DI en el acto): no es CPU de biblioteca |
| 2 | `801C0540` | RVL SDK | PSMTXConcat | 1.661 | 3.43 | **N2** nativa rápida (float, host) |
| 3 | `802403C0` | nw4r | func_802403C0 | 1.440 | 4.87 | **N4** nativa (cálculo exacto en double, GQR decodificados una vez, pila local) |
| 4 | `80237BD0` | nw4r | SinFIdx | 1.406 | 6.28 | **N2** nativa rápida (float, host) |
| 5 | `801A2B10` | RVL SDK | func_801A2B10 | 1.065 | 7.34 | __AXSyncPBs: N4 |
| 6 | `801A3880` | RVL SDK | func_801A3880 | 1.063 | 8.40 | reverb AXFX: ya nativa (A2) |
| 7 | `801C9A80` | RVL SDK | GXLoadTexMtxImm | 1.022 | 9.43 | **N2** nativa rápida (float, host) |
| 8 | `802E861C` | MSL/runtime | func_802E861C | 1.005 | 10.43 | sqrt de fdlibm: N4 (función fusionada y no hoja) |
| 9 | `80250E70` | nw4r | func_80250E70 | 0.977 | 11.41 | pendiente |
| 10 | `802824B4` | nw4r | func_802824B4 | 0.819 | 12.23 | pendiente |
| 11 | `80267F40` | nw4r | func_80267F40 | 0.816 | 13.04 | pendiente |
| 12 | `801C0500` | RVL SDK | PSMTXCopy | 0.742 | 13.79 | **N2** nativa rápida (float, host) |
| 13 | `80273560` | nw4r | func_80273560 | 0.688 | 14.47 | pendiente |
| 14 | `80248330` | nw4r | func_80248330 | 0.669 | 15.14 | pendiente |
| 15 | `802382B0` | nw4r | func_802382B0 | 0.669 | 15.81 | **N5** nativa (exacta, macros N1 sobre locales, pila local) |
| 16 | `8029D300` | nw4r | func_8029D300 | 0.634 | 16.45 | pendiente |
| 17 | `80004364` | MSL/runtime | memcpy | 0.597 | 17.04 | N1 traducida (sin ganancia medida) → rehacer (N4+) |
| 18 | `801AC440` | RVL SDK | DCInvalidateRange_801AC440 | 0.551 | 17.59 | N1 a mano |
| 19 | `801C52F0` | RVL SDK |  | 0.542 | 18.14 | __GXSetDirtyState: N4 (GX, escribe __GXData y el FIFO) |
| 20 | `801C7350` | RVL SDK | GXLoadTexObjPreLoaded | 0.529 | 18.66 | GXLoadTexObjPreLoaded: N4 (GX) |
| 21 | `801B1180` | RVL SDK | OSRestoreInterrupts_801B1180 | 0.526 | 19.19 | su coste es la entrega de interrupciones: no |
| 22 | `80237C40` | nw4r | CosFIdx | 0.477 | 19.67 | **N2** nativa rápida (float, host) |
| 23 | `802405B0` | nw4r | func_802405B0 | 0.469 | 20.14 | pendiente |
| 24 | `802DCC4C` | MSL/runtime | __cvt_fp2unsigned | 0.461 | 20.60 | **N3** nativa rápida (float, host) |
| 25 | `802821E0` | nw4r | func_802821E0 | 0.445 | 21.04 | pendiente |
| 26 | `8025DB30` | nw4r | func_8025DB30 | 0.432 | 21.47 | pendiente |
| 27 | `8024C960` | nw4r | func_8024C960 | 0.425 | 21.90 | pendiente |
| 28 | `801C5570` | RVL SDK | GXBegin | 0.413 | 22.31 | pendiente |
| 29 | `8024B720` | nw4r | func_8024B720 | 0.394 | 22.71 | pendiente |
| 30 | `801AC470` | RVL SDK | DCFlushRange_801AC470 | 0.393 | 23.10 | N1 a mano |
| 31 | `801C07A0` | RVL SDK | PSMTXInverse | 0.393 | 23.49 | **N3** nativa rápida (float, host) |
| 32 | `802CC730` | EGG | func_802CC730 | 0.390 | 23.88 | pendiente |
| 33 | `8024C2B0` | nw4r | func_8024C2B0 | 0.373 | 24.25 | pendiente |
| 34 | `80280910` | nw4r | func_80280910 | 0.366 | 24.62 | pendiente |
| 35 | `8024E180` | nw4r | func_8024E180 | 0.364 | 24.98 | pendiente |
| 36 | `802E1AB4` | MSL/runtime | strcmp | 0.363 | 25.35 | **N5** nativa (los tres bucles en el anfitrión, sondeos agrupados) |
| 37 | `801C78D0` | RVL SDK | func_801C78D0 | 0.358 | 25.70 | pendiente |
| 38 | `802CC050` | EGG | func_802CC050 | 0.353 | 26.06 | pendiente |
| 39 | `8029C820` | nw4r | func_8029C820 | 0.340 | 26.40 | pendiente |
| 40 | `802631F0` | nw4r | func_802631F0 | 0.338 | 26.74 | pendiente |

(sigue en `runs/ranking_lib.md`.)

## 6. Plan por rondas (N3, N4, ...)

Cada ronda: las siguientes funciones más caras de la lista (§5) con el método de n2 (trabajo real en single o en
enteros sobre punteros del anfitrión, salida exacta al código generado en los casos raros), y la misma
verificación: difftest normal + `--fp` + `--n2` (estados dirigidos que hacen correr el camino rápido; añadir los de
cada función nueva en `n2_regs`), `NWII_NATIVE_VERIFY` en PC con `NWII_AX_THREAD=0`, `checks.sh` 1x, Coin Battle,
y una NRO propia (`nsmbw_n3.nro`...). En la consola, A/B del mismo binario con `NWII_NATIVE="0"` y, mejor,
`NWII_NATIVE_STATS="1"` en las dos (con la función apagada se cuentan igual las llamadas): coste por llamada =
muestras / llamadas, que no depende de qué escenas caigan en las ventanas lentas.

- **N3 (hecha, `nsmbw_n3.nro`)**: PSMTXInverse (cofactores + `fres` con `ppc_fres` + un paso de Newton, todo en
  single; salida exacta si el inverso de `fres` no es un single), PSMTXMultVec, PSMTXTrans (`stfs` de dobles con
  `ppc_single_bits`), `__cvt_fp2unsigned` (en double con los mismos ayudantes, punteros del anfitrión).
- **N4 (hecha, `nsmbw_n4.nro`)**: animación g3d `802403C0` (la más cara por llamada, ~1,4 µs): extremos,
  acierto exacto y Hermite; búsqueda lineal con sondeos (solo si todos serían "callados", si no el código generado);
  los `psq` cuantizados con GQR2/3/6 decodificados una vez; aritmética con las macros `N1_*` (= los ayudantes de
  `cpu_context.h`, en double: la entrada f1 puede ser cualquier double); el marco de 32 B de la pila en una copia local
  que se escribe al final; todo lo que se lee del juego antes de escribir nada. Además MultVec acepta dst == src.
  Fallo encontrado por el difftest y arreglado: el límite de la tabla de claves se desbordaba (puntero
  fuera de rango cuando la búsqueda pasa por delante de la primera clave) → `SIGSEGV` en 6 de 250 semillas; ahora
  ese caso cae al código generado.
  También en n4: las funciones en single (Concat, MultVec, Inverse) ceden al código generado si alguna entrada es
  NaN: con dos NaN en una operación, cuál sale depende de la instrucción (FMA en double o en single; en x86 además
  `fma` frente a `fmaf` de la libm), y el difftest con NaN mostró esas diferencias ("NaN distinto", no solo el
  signo). Con la guarda: 4 800/4 800 iguales en estados con NaN.
- **N5 (hecha, `nsmbw_n5.nro`)**: `802382B0` (matriz de rotación desde tres ángulos de nw4r::math, 0,7 %,
  ~0,76 µs/llamada: tres bucles de reducción con sondeo, índices u16 por la pila, tabla SinCos; mismo método que
  la animación) y `strcmp` (0,96 % en n2, 55 000 llamadas/s: los tres bucles —alineación, palabras con la prueba
  de cero, bytes— en el anfitrión con los registros, CR (incluido cuándo se escribe SO), CTR, CA e `inst_count`
  de cada salida; sondeos agrupados).
- **N6 (siguiente)**: `80240100`/`80240240` (las otras dos de la animación), `802382B0`/`802381A0`/`802390D0` (nw4r::math), PSMTXConcatArray
  (bucle de Concat), `memcpy`/`__fill_mem` (memmove del anfitrión + registros en forma cerrada), sqrt de fdlibm
  (`802E861C`: función fusionada y no hoja, hay que enganchar su entrada secundaria y llamar a `scalbn` igual),
  `__AXSyncPBs`.
- **N7**: GX con estado (`__GXSetDirtyState`, `GXLoadTexObjPreLoaded`, `GXBegin`, `GXSetFog`, `801C8430`): hay
  que escribir los mismos bytes en el FIFO y los mismos campos de `__GXData`; verificación con el difftest (registra
  el FIFO) y `NWII_WGP_CHECK` de W.
- **N8+**: nw4r g3d (`80250E70` CalcWorld, `8024C960`, `8024B720`, `8025DB30`...), nw4r snd (`802824B4`,
  `80267F40`, `80273560`, `802821E0`, `802631F0`...), EGG (`802CC730`, `802CC050`), lyt/ef: funciones grandes y no
  hoja → reescritura por bloques con las llamadas al juego tal cual (más trabajo por función).
- Lo que **no** es biblioteca-CPU: `IOS_SeekAsync`/DI (E/S en el acto), `OSRestoreInterrupts` (entrega de
  interrupciones), el bucle ocioso.

**Compatibilidad con G6 (r9/r20g6, export regenerado):** `tools/apply_n1.py` busca cada función por su
**dirección** (el nombre generado termina en `_<DIR>`), no por el texto. Sobre un export nuevo:
`python3 tools/apply_n1.py <export_g6> --orig "" --skip-untranslatable` (sin comprobar sha256 contra r18m; las
funciones "fp"/"int" que el traductor no entienda en la forma nueva se quedan sin gancho con un aviso). Las de N2 y
"hand" solo necesitan el gancho y viven en el runtime (`native_n1.cpp`, `native_n2.cpp`), así que siguen valiendo
mientras el contrato de `CPUContext` (registros, `inst_count`, sondeo) no cambie. Las variantes traducidas de n1 no
dieron ganancia: en un export G6 se pueden quitar de la tabla sin perder nada.



### Coste por llamada medido (PC x86, `NWII_NATIVE_TIME`, n5s, desierto4j + agua_pinguino, 150 s cada lado)

`runs/pc_time_on_off.txt` (en x86 la FMA en single es de software y hay 16 registros: en la Switch los números
absolutos y las proporciones serán otros; la A/B de la consola con n5s es la que cuenta):

```
PC x86, desierto4j + agua_pinguino 150 s, n5s: per-call time (last report of each function)
ANM80240100        native   191.7 ns (  244375)  generated   184.0 ns (  244546)  x0.96
ANM80240240        native   330.2 ns (  213748)  generated   303.2 ns (  213922)  x0.92
ANM802403C0        native   150.1 ns ( 1195701)  generated   181.5 ns ( 1312275)  x1.21
COSFIDX            native    35.5 ns ( 1753143)  generated    54.7 ns ( 1869364)  x1.54
CVTFP2UNSIGNED     native    26.7 ns ( 4147731)  generated    28.1 ns ( 4150485)  x1.05
CXUNCOMPRESSLZ     native 1735662.0 ns (       2)  generated     0.0 ns (       0)  
DCFLUSHRANGE       native   669.6 ns (  206487)  generated   988.8 ns (  206530)  x1.48
DCINVALIDATERANGE  native   326.9 ns (  631999)  generated   503.4 ns (  632225)  x1.54
FILLMEM            native    45.0 ns ( 1064378)  generated    46.7 ns ( 1064814)  x1.04
GAME8000D0D0       native    34.4 ns ( 3097600)  generated    36.6 ns ( 3097600)  x1.06
GXINITTEXOBJLOD    native    52.7 ns (  987737)  generated    56.4 ns (  988151)  x1.07
GXLOADPOSMTXIMM    native    81.0 ns (  334785)  generated   113.7 ns (  334911)  x1.40
GXLOADTEXMTXIMM    native    70.6 ns ( 1586371)  generated    97.4 ns ( 1586932)  x1.38
MATH802381A0       native    71.7 ns (  498001)  generated    75.9 ns (  498286)  x1.06
MATH802382B0       native   150.8 ns (  949396)  generated   142.8 ns (  949935)  x0.95
MATH802390D0       native   213.9 ns (  197394)  generated   249.5 ns (  197460)  x1.17
MEMCPY             native   118.7 ns ( 1071185)  generated   114.3 ns ( 1071847)  x0.96
PSMTXCONCAT        native   133.7 ns ( 2748211)  generated   144.8 ns ( 2749349)  x1.08
PSMTXCONCATARRAY   native  1167.4 ns (   90101)  generated  1231.9 ns (   90136)  x1.06
PSMTXCOPY          native    43.3 ns ( 3434183)  generated    50.1 ns ( 3435577)  x1.16
PSMTXIDENTITY      native    26.5 ns ( 2282236)  generated    41.7 ns ( 2283070)  x1.57
PSMTXINVERSE       native   165.6 ns (  516775)  generated   193.7 ns (  516944)  x1.17
PSMTXMULTVEC       native    67.1 ns (  942416)  generated    66.9 ns (  942824)  x1.00
PSMTXTRANS         native    43.1 ns (  594589)  generated    51.7 ns (  594841)  x1.20
SEARCHNODEBYID     native    50.3 ns ( 4509573)  generated    56.5 ns ( 4511661)  x1.12
SINCOSFIDX         native   255.5 ns (   37932)  generated   193.1 ns (   37956)  x0.76
SINFIDX            native    32.4 ns ( 7896375)  generated    49.9 ns ( 8422195)  x1.54
SND802835B0        native    80.4 ns (  346612)  generated    72.5 ns (  346884)  x0.90
SND80283760        native    43.7 ns (  992446)  generated    44.5 ns (  993135)  x1.02
SND802837C0        native    57.6 ns (  593926)  generated    61.8 ns (  594244)  x1.07
SND802838A0        native    43.7 ns (  593926)  generated    46.4 ns (  594244)  x1.06
STRCMP             native    35.0 ns ( 7717879)  generated    35.9 ns ( 8720150)  x1.03
```

Lectura: por llamada se gana entre nada y ×1,5; como las llamadas cuestan decenas de ns, el total por segundo es
pequeño (p. ej. SinFIdx: 53 000 llamadas/s × 17 ns = 0,9 ms/s ≈ 0,1 % de un núcleo). Las funciones del SDK son
muchas y baratas; el grueso del hilo del juego está en el código generado de nw4r/EGG/juego. Esto confirma lo que
ve la consola (~1 % por ronda) y sugiere dejar las rondas de funciones hoja y atacar el generador (G6) o
funciones grandes concretas medidas por escena.

### Balance honesto y cómo seguir (9-nov 04:45)

- Lo medido en la consola hasta ahora (n2 con STATS): ~1 % del hilo del juego por ronda (PSMTXConcat, GXLoadTexMtxImm
  y PSMTXCopy bajan claramente; el resto se pierde en el ruido de las ventanas lentas aleatorias). n1 (copiar el
  código con FPR en locales) no ganó nada. Con la lista fija de demos del frente S y `NWII_NATIVE_TIME` la A/B por
  llamada y por escena ya no depende del azar: eso debe decidir qué funciones valen la pena.
- Prioridad para N6+: el coste medido **por escena** en el desierto 4J y en la demo submarina (las peores), no la media.
- Lo que más pesa y queda: `__AXSyncPBs` (audio), `80250E70` (g3d CalcWorld), snd (`802824B4`, `80267F40`,
  `80273560`...), `__GXSetDirtyState`/`GXLoadTexObjPreLoaded` (GX con estado), `searchNodeByID` (sin ganancia:
  pescar punteros por la lista cuesta lo que cuesta), `memcpy`, las otras dos de la animación.

## 7. Binarios y verificación

| Archivo | Qué es |
|---|---|
| `bin/nsmbw_n1.nro` (+ `.elf`) | n1 (33 funciones, ya medida: sin ganancia) |
| `bin/nsmbw_n2.nro` (+ `.elf`) | n2 = n1 + las 7 de N2 en single; orden L; arranca con GL como m1 |
| `bin/nsmbw_n3.nro` (+ `.elf`) | n3 = n2 + las 4 de N3; orden L; GL por defecto |
| `bin/nsmbw_n4.nro` (+ `.elf`) | n4 = n3 + la animación `802403C0` + MultVec en sitio + guarda de NaN; orden L; GL |
| `bin/nsmbw_n5.nro` (+ `.elf`) | n5 = n4 + `802382B0` + `strcmp`; orden L; GL |
| **`bin/nsmbw_n5s.nro`** (+ `.elf`) | **n5s** = n5 + demos forzadas y partida protegida del frente S (`r9/r21s/forcedemo.patch`, sha256 604be1e9…, aplicado con `patch -p2` en `nWiiRuntime/`) + `NWII_NATIVE_TIME` (el árbol actual); orden L; GL. Para las A/B con lista fija de demos |
| `bin/*_input_test.nro` | solo pruebas con guion. **Nunca como juego** |
| `bin/sin_orden/` | las mismas sin el orden L |
| `bin/NSMBW_n1` ... `NSMBW_n5` (+ `_input_test`) | PC |

sha256: `runs/bin_sha256.txt`. Parches del árbol actual (n3) contra r18m: `n_export_vs_r18m.patch`,
`n_runtime_vs_r18m.patch` (se regeneran con `diff -ruN r18m/export r19n/export` y `.../nWiiRuntime`). n1 y n2 se
compilaron con estados anteriores de la tabla de `apply_n1.py` (n1: todo "fp"/"hand"; n2: las 7 de N2 en "n2");
el árbol de ahora es el de n5s. Las declaraciones de `n2h_*` las escribe `apply_n1.py` en cada archivo generado
(desde n3), así una función nueva solo recompila su `output_N.cpp`.

Verificación (PC; consola: la sesión principal):

| Prueba | n1 | n2 | n3 | n4 | n5 |
|---|---|---|---|---|---|
| difftest aleatorio (todas las enganchadas) | 33 × 200 + 12 × 300: 0 dif. | — | 3 300: 0 dif. | 1 979: 0 dif. | 1 318: 0 dif. salvo 1 "NaN distinto" en 802390D0 (traducida de n1; ambos lados NaN con otra carga, orden de operandos en x86) |
| difftest dirigido (`--n2`; `--no-nan`) | — | 7 × 1000: 0 dif. | 4 399 sin NaN: 0 dif. | anim 595 semillas, 3 592 + 4 800 (con NaN, tras la guarda): 0 dif. | strcmp 1 500/1 500; rotación 579 sin NaN (21 NANSIGN de entradas NaN/inf); 15 rápidas 2 790: 0 dif. |
| `NWII_NATIVE_VERIFY` + `NWII_AX_THREAD=0`, título 300 s | 6 844 / 0 malas | 5 990 / 0 | 6 663 / 0 | 5 916 / 0 | 7 021 / 0 (802382B0 nativa 100 %, strcmp 82 %) |
| `checks.sh` 1x (4 escenas) | 200-220 idénticas, diff_lines=0 | ídem | ídem | ídem | 200-220 idénticas, diff_lines=0 |
| Coin Battle 4J / sanity | bien / bien | 57,7 fps / — | 59,5 fps / — | 59,4 fps / bien (59,1 fps) | bien, 59,7 fps / — |
| tabla fastmem; orden L (eqcheck) | 0; 0 distintas | 0; 0 | 0; 0 | 0; solo `__nx_mod0` | 0; 0 |

NANSIGN = solo el signo/carga de un NaN, por el orden de operandos que elige GCC en x86 (convención de r9).
Nota del modo verificación: la primera versión comparaba hashes de toda la RAM y daba falsos "MISMATCH" (otros
hilos —AX, render— escriben RAM mientras tanto; strcmp y PSMTXCopy LC→LC "cambiaban MEM1"); ahora compara bloques
de 32 B y restaura solo los que tocó el lado nativo.
