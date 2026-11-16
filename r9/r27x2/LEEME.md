# r27x2: frente X2 — escrituras del juego al FIFO de GX en línea (10-nov-2026)

Sin commits. No se tocó `r24m2`, `r20g6`, `r22r7` ni ningún otro frente (solo se leen), ni `~/nsmbw-port/nand`, ni la
consola. Todo lo pesado (compilar, enlazar, correr el juego) pasó por `~/nsmbw-port/scripts/lim` con `JOBS=2`.

> **Resumen.** Cada `stb/sth/stw/stfs/psq_st` del juego al write-gather pipe (0xCC008000) hacía dos llamadas fuera
> de línea (`MMU::writeN_slow` → `GX_WGPIPE_WriteN`, en otra unidad) con sus comprobaciones. Ahora, en los ~5 400 (r7f; 7 077 en G6 e2)
> sitios que el recompilador ya marcaba como MMIO (`mv_.writeN_io`, `psq_st_io`), el código generado añade los bytes
> **directamente al búfer write-gather** (una comparación: "página del pipe y cabe en el búfer mientras los caminos en
> línea están permitidos") y hace exactamente lo que hacía el camino en línea de R16w (misma máquina de estados
> BP/CP/LOAD_INDX, mismo contador). Todo lo demás cae a la llamada de siempre.
> **Resultado:** 94-99 % de las escrituras al pipe en línea en las demos forzadas; hilo del juego en PC (perfil `sw`,
> nivel 1-1) **−6,5 a −7 % de instrucciones por frame** frente a la base (m2: 11,61 → 10,81 M; r7f: 12,4 → 11,7 M),
> pero **en ciclos/tiempo la diferencia con la base queda dentro del ruido en PC** (x86 predice bien las dos
> llamadas viejas; 10 parejas x2m2/m2: 11,53 contra 11,51 Mcyc/frame de media). En la consola esos ayudantes eran
> **~4,5-5,6 % del hilo del juego ocupado** (perfiles r7f a 1020 MHz); cuánto de eso se recupera en el A57 (como
> mucho ≈ 1-1,4 ms/frame de ~26) **hay que medirlo allí**: este frente no habla con la consola.
> Salida idéntica: autoprueba diferencial (400 000 escrituras aleatorias: mismo flujo y mismo estado), FRAMECHECK
> 4/4 escenas idénticas, flujo FIFO grabado idéntico a la base hasta donde la base es idéntica a sí misma.

## 1. Qué costaba (perfiles de la consola, r7f, 1020 MHz, portátil)

`switch/real_logs/bench_E02_dtorre_on_1110_0119` y `bench_F01_r7f_lista1020_1110_0132` con `bin/nsmbw_r7f.elf`
(símbolos con `nm`, muestras atribuidas por PC/LR; guion en el scratchpad, mismo método que `r25dec/tools/rank.py`):

| Ayudante (hilo del juego, % de lo ocupado, E02) | % |
|---|---|
| `GX_WGPIPE_Write8` / `WriteF32` / `Write32` / `Write16` | 1,66 / 0,84 / 0,79 / 0,73 |
| `MMU::write8_slow` / `write_f32_slow` / `write16_slow` / `write32_slow` | 0,47 / 0,45 / 0,23 / 0,23 |
| `wg_flush_now` + `GX_WGPIPE_Flush` + `AgeFlush` | 0,21 |
| **total escritura al pipe** | **5,6** (media de los dos perfiles ≈ 4,5) |
| además: `psq_store` fuera de línea desde GXLoadTexMtxImm/PosMtxImm (filas de matriz al pipe) | 0,45 |
| `wgp_write32` camino completo (CALL_DL, LOAD_INDX, BP que entregan, valores CP) | 0,59 |

Quién escribe: `func_8000D000` (renderizador de bloques/fondo con GXPosition3f32 + GXColor4u8 + GXTexCoord, ~1,1 %),
`8010AA60`, las funciones GX del SDK (GXBegin, GXLoadTexObjPreLoaded, __GXSetDirtyState, GXSetTev*...) y
GXLoadTex/PosMtxImm. Por archivo generado: output_27 1,07 %, output_1 1,07, output_26 0,64, output_16 0,24,
output_35 0,18, el resto < 0,05.

## 2. Cambios

| Qué | Dónde |
|---|---|
| Cabecera nueva **`x2_wgpipe.h`**: estado del write-gather compartido (`g_wgi`: búfer de 4 KB, fill, `lim`, estado del sniffer BP/CP/LOAD_INDX, bases/strides de los arreglos CP, contador) y `x2_w8/w16/w32/wf32/psq_st` en línea. `lim` = capacidad del búfer solo mientras R16w permitiría su camino en línea (write-gather, sin `NWII_WGP_CHECK`/`NWII_SAMPLE`, sin redirección a una DL en RAM, `NWII_X2_INLINE` ≠ 0); si no, 0 y todo va por la llamada vieja. `g_wgi` es `hidden` (adrp+add, sin GOT) | `nWiiRuntime/include/runtime/x2_wgpipe.h` |
| `w32` en línea: dato sin comando pendiente, BP que no entrega (≠ 0x45/47/48/52) y valor de registro CP (actualiza bases/strides), igual que R16w w4. LOAD_INDX, CALL_DL y BP que entregan → camino completo | ídem |
| `psq_st` al pipe con GQR float: ps0 (y ps1 si W=0) como `wf32` de `ConvertToSingleFTZ`, lo mismo que hacía `psq_store` → `write_f32` → `WriteF32` | ídem |
| `gx.cpp`: `s_wg`, `s_wg_n`, `s_bp_armed`, `s_sn_st/a`, `s_sn_base/stride`, `s_wg_flush_all_bp` y el contador viven en `g_wgi` (macros: el código de R16w queda igual); `idx_sniff8` usa la misma `x2_sniff8` que los sitios (una sola copia de la máquina); `GX_WgInlineUpdate()` recalcula `lim`; contador de escrituras que entran por las funciones (`x2 inline N%` en la línea `[FPS]`/`wgp`); grabador `NWII_X2_FIFOREC`; autoprueba `NWII_X2_SELFTEST` | `src/hle/gx.cpp` |
| `wgp_redirect_update()` llama a `GX_WgInlineUpdate()` (la redirección a DL en RAM apaga los sitios en línea al instante) | `src/hw/hw_pi.cpp` |
| `NWII_X2_INLINE` (y `NWII_X2_NATIVE`, reservado) en `keeps_zero` → en la Switch `NWII_X2_INLINE = "0"` sí apaga | `src/platform/switch/switch_platform.cpp` |
| Código generado: **`tools/apply_x2.py`** cambia textualmente `mv_.write8_io(` → `nwii::runtime::x2_w8(mv_, ` (y 16/32/f32, `psq_st_io(` → `x2_psq_st(`) e incluye la cabecera. Nada más cambia. r7f: 19 archivos (incl. el `output_24` de r18m), 5 423 sitios; G6 e2: 21 archivos, 7 077 sitios. Re-aplicable sobre cualquier `export/` (se usó también sobre G6 e2) | `export/`, `export/x2_sha256.txt` (sha256 de entrada y salida) |

Parche del runtime: **`x2_runtime_vs_r7f.patch`** (`diff -ruN r22r7/nWiiRuntime r27x2/nWiiRuntime`). Las cabeceras
que incluye el código base (cpu_context.h, config.h, nwii_env.h, event_scheduler.h, hw/hw.h) **no cambian**.
Coordinación con X1 (funciones de matemáticas): `COORD_X1.md` — X2 no sustituye ninguna función; en
GXLoadTex/PosMtxImm solo cambian sus `psq_st_io`.

### Interruptores

- `NWII_X2_INLINE=0` → `lim` = 0: todo por el camino de r7f (mismo binario, para A/B). En la consola funciona (keeps_zero).
- `NWII_X2_SELFTEST=1` → en la primera escritura al pipe corre la autoprueba (ver §4) y la imprime (`[X2] selftest`).
- `NWII_X2_FIFOREC=<archivo>` → graba el flujo que recibe el anfitrión (cada entrega del búfer) sin cambiar el modo.

### Evaluado y no hecho

- **(b) GX del SDK en nativo** (GXBegin, __GXSetDirtyState, GXLoadTexObj*, GXSetTev*...): todo el rango GX del SDK
  es ~5,9 % propio del hilo ocupado repartido en 121 funciones (la mayor 0,34 %); cada una exige reproducir
  exactamente `__GXData`, registros, CR e `inst_count` (método N de r19n). Con X2 sus escrituras ya no pagan las
  llamadas; lo que queda es lógica del invitado por función: mucho riesgo por ~0,1-0,3 % cada una. No se hizo.
- **(c) Agrupar entregas**: `wg_flush_now` es 0,13-0,21 %; las entregas por BP (0x45/47/48/52, ~1-3 mil/s) hacen
  falta para la sincronización. Ganancia ≤ 0,1 %: no se tocó.
- Lo que queda fuera de línea (1-6 % de las escrituras): las de las costuras HLE del runtime y algún sitio no listado.

## 3. Compilar

```sh
cd ~/nsmbw-port/r9
# r7f + X2 (x2b): objetos del juego de r14g5, runtime y MODS de r27x2
python3 -I r27x2/tools/apply_x2.py r22r7/export r27x2/export $(cat r27x2/export/MODS.txt)   # ya hecho
export MODS="$(cat r27x2/export/MODS.txt)" BASE=/home/djvemo/nsmbw-port/r9/r14g5
JOBS=2 ~/nsmbw-port/scripts/lim r27x2/tools/build_m.sh r27x2 x2b pc
DK_LIB=deko3d JOBS=2 ~/nsmbw-port/scripts/lim r27x2/tools/build_m.sh r27x2 x2b_dk3drel nx
cd r27x2 && NACP_NAME="NSMBW PRUEBA x2" ~/nsmbw-port/scripts/lim tools/build_hot.sh x2b_dk3drel $(cat nx/hot/profiles.txt)
# m2 + X2 (x2m2): compila los 19 output_N de G6 e2 con sitios y enlaza la línea de r24m2
JOBS=2 ~/nsmbw-port/scripts/lim tools/build_x2m2.sh
```

(`export/functions.h` es un enlace duro a `r14g5/export/functions.h`: los MODS lo incluyen desde su carpeta.)

## 4. Verificación (PC)

| Prueba | Resultado |
|---|---|
| **Autoprueba diferencial** `NWII_X2_SELFTEST` (x2a, x2b, x2m2): 400 000 escrituras aleatorias (bytes 0x61/0x08/0x20-0x38/0x40/0x78/0xAC/0xBC... sesgados, BP no-entrega, valores CP, 1/16 con `lim`=0 para probar la caída, el búfer se llena y entrega muchas veces) por las funciones en línea y por las de R16w | **PASS**: 1 099 567 B idénticos, mismo estado (sniffer, bases/strides, BP, contador) |
| Flujo FIFO grabado, reloj virtual (`test/fiforec.sh`, `tools/fifocmp.py`), título 100 s | r7e contra r7e: idénticos los primeros 2 551 644 B (**348 copias XFB**) y luego divergen (límite conocido del arnés, también con `NWII_AX_THREAD=0`); **x2b contra r7e y x2b encendido contra apagado: idénticos exactamente los mismos 348 frames** y divergen en el mismo punto, por la misma no-determinación (un float de vértice de la animación) |
| `checks.sh` 1x (FRAMECHECK cada 30 frames) | x2a, x2b y x2m2: title/map/walk/lvl11 **todas idénticas** (200-220 cada una), `diff_lines=0` |
| Demos forzadas en tiempo real 330 s (desierto4j, agua_pinguino, cielo_paratroopas, nubes_plataformas, castillo) | x2b: las 5 demos, sin parones, imágenes bien (`runs/hoja_x2b_demos.jpg`); `x2 inline` 94-99 % |
| Orden L de la NRO | mismas 59 294 secciones, 0 funciones distintas (`build_hot_x2b.log`) |

## 5. Medidas (PC, perfil `sw` = CPU débil tipo Switch; `test/prof.sh ... lvl11 25 6 stat`, hilo del juego)

| | Mcyc/frame (media / mediana) | Minstr/frame | ms/frame (mediana) | % CPU del hilo |
|---|---|---|---|---|
| x2b con `NWII_X2_INLINE=0`, 8 corridas | 12,46 / 12,87 | 12,46 | 7,08 | 37,4 |
| **x2b**, 8 corridas | 12,31 / 12,49 | **11,70** | 6,85 | 35,4 |
| r7e (= r7f en PC), 3 corridas | 11,92 / 12,33 | 12,48 | — | 36,4 |
| **m2** (r24m2), 10 corridas | 11,51 / 11,62 | 11,61 | 6,46 | 34,0 |
| **x2m2** (m2 + X2), 10 corridas alternadas con m2 | 11,53 / 11,75 | **10,81 (−6,9 %)** | 6,53 | 33,9 |
| x2m2 con `NWII_X2_INLINE=0`, 4 corridas | 12,45 / 12,29 | 11,74 | 6,83 | 34,0 |

Tablas: `prof/ab_l11_x2b_tabla.txt`, `prof/ab_l11_x2m2_vs_m2.txt`, `prof/ab_l11_x2m2_tabla.txt`.
Lectura honesta: las instrucciones bajan de forma estable (±1 % entre corridas); los ciclos varían ±8 % entre
corridas (núcleos E a 1,1-1,9 GHz) y **frente a la base real no hay diferencia medible en PC**. El modo
`NWII_X2_INLINE=0` no es la base: paga además la comprobación fallida de cada sitio (por eso "on contra off" sale
−3 % y no sirve como cifra de ganancia).
Castillo (x2a): instrucciones −7,4 % (15,60 → 14,44 M), ciclos sin cambio medible (escena limitada por el render).
En x86 las dos llamadas viejas se predicen bien; en el A57 de la Switch pesaban 4,5-5,6 % del hilo ocupado
(§1): allí la ganancia esperable es mayor que en PC.

### m2 + X2 (x2m2)

`tools/build_x2m2.sh` aplica `apply_x2.py` a `r20g6/e2/export` (21 archivos, 7 077 sitios), los compila con las
órdenes de e2 y enlaza la línea de `r24m2` con el runtime de x2b y el orden L de m2. Verificado como x2b:
autoprueba PASS, `checks.sh` 1x 4/4 idénticas (`runs/checks_x2m2.txt`), demos forzadas 330 s con las 5 demos e
imágenes bien (`runs/hoja_x2m2_demos.jpg`), `x2 inline` 97-99 %.

## 6. Binarios (`bin/`, sha256 en `runs/bin_sha256_x2.txt`, `runs/bin_sha256_x2m2.txt`)

| Archivo | Qué es |
|---|---|
| **`nsmbw_x2.nro`** (+ `nsmbw_x2.elf`) | **r7f + X2** (= `nsmbw_x2b_dk3drel_hot`), GL, orden L; NACP "NSMBW PRUEBA x2" |
| `nsmbw_x2m2.nro` (+ `.elf`) | **m2 (r24m2: r7f + G6 e2) + X2**, orden L de m2; NACP "NSMBW PRUEBA x2m2 (m2 + X2)" |
| `nsmbw_x2_input_test.nro`, `nsmbw_x2m2_input_test.nro`, `*_input_test.nro` | **solo pruebas** con entrada por guion: **nunca como juego** |
| `NSMBW_x2b(_input_test)`, `NSMBW_x2m2(_input_test)` | PC |
| `nsmbw_x2a*`, `NSMBW_x2a*` | primera versión (6 archivos, `g_wgi` por GOT); superada |

## 7. Para medir en la consola (no se hizo: este frente no habla con la Switch)

Lista fija de escenas de S, 1020 MHz, con `nsmbw_x2.nro` contra `nsmbw_r7f.nro` (o `nsmbw_x2m2.nro` contra
`nsmbw_m2.nro`), y A/B en el mismo binario con `NWII_X2_INLINE = "0"`. Mirar en `nwii.log` `x2 inline NN%` y en
`profile_cpu.txt` que `GX_WGPIPE_Write*` y `MMU::write*_slow` casi desaparecen del hilo del juego.

## 8. Riesgos

- Los sitios en línea saltan el `watch_hit`/`custom_watch` de `MMU::write*_slow` (solo depuración, `NWII_WATCH`):
  con un watch en la página del pipe, esas escrituras no avisarían (nadie lo usa).
- Un sitio listado como MMIO que alguna vez escribe en RAM sigue igual (cae a `write*_slow`, que escribe en RAM).
- Las cabeceras base no cambian, pero el estado del write-gather ahora es global (`g_wgi`): cualquier variante
  futura que toque `gx.cpp` debe mantener esos campos y `GX_WgInlineUpdate()` al cambiar modo o redirección.
