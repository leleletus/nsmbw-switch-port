# r12a: worker de render (r11a) sobre el runtime de la consola (r10c = r11base + arreglo del iris de r11d)

**Qué es:** `r9/r12a/nWiiRuntime` = runtime de r10c (lo que está en la Switch: r11base + arreglo del
iris del título del frente D + cambios de mandos) + el parche del worker de render de r11a
(`r9/r11a/r11a_vs_r11base.patch`; diseño, números y riesgos en **`r9/r11a/LEEME.md`**).
Copia intacta del runtime de partida: `/tmp/claude-1000/-home-djvemo-nsmbw-port/1b06eac6-a23b-489b-924a-77167831057b/scratchpad/r12a_orig`
(temporal; la original sigue en `r9/r10c/nWiiRuntime`).

- Binarios: **`r9/r12a/bin/nsmbw_r12a.nro`** (Switch), `nsmbw_r12a.elf`, `nsmbw_r12a_input_test.nro`
  (solo pruebas), `NSMBW_r12a`, `NSMBW_r12a_input_test` (PC).
- Compilar: `cd ~/nsmbw-port/r9 && JOBS=3 ~/nsmbw-port/scripts/lim tools/variant_build.sh r12a r12a both`
  (las cabeceras prohibidas son iguales a las de r10c: comprobado).
- `NWII_RENDER_WORKER=0` vuelve al drenaje en el hilo de render (mismo código que r10c en ese camino).

## Fusión

`patch -p1 < ../r11a/r11a_vs_r11base.patch` falló 2 trozos de `fifo_parser.cpp` (las dos lecturas
LOAD_INDX: en aciertos de la caché de display lists y en el flujo principal), porque el frente D las
cambió para usar sus copias (`GX_IdxReadDL` / `GX_IdxTakeMain`). Resuelto a mano: se queda el código de
D y solo cambian `g_state.arrayBase/arrayStride` → `s_ps.arrayBase/arrayStride` (estado privado del
parser de r11a). Resto del parche aplicado tal cual (con desplazamientos).

## Sincronización del arreglo de D con el worker (revisado)

- Los registros de D (`IdxRecMain`, `IdxRecDL`) los produce el hilo del juego al escribir el comando
  y los consume el parser; las dos colas (`s_idx_main`, `s_idx_dl`) y `s_idx_need` van siempre bajo
  `s_idx_mtx`. La poda por antigüedad compara sellos de los propios registros (bajo el mismo mutex).
- El registro se encola **antes** de que sus bytes lleguen a `g_hw_fifo` (se encola en la escritura;
  los bytes pasan al FIFO al vaciar el búfer de escritura, bajo `g_fifo_mutex`). El worker toma los
  bytes con `g_fifo_mutex` y luego busca el registro con `s_idx_mtx`: la cadena guest (push → unlock
  s_idx_mtx → … → unlock g_fifo_mutex) → worker (lock g_fifo_mutex → lock s_idx_mtx) garantiza que lo
  ve. Es la misma relación que había con el hilo de render, solo que ahora el consumidor es el worker.
- El estado del lado del parser de D (`s_idx_ctx`, `s_idx_cur`, `s_idx_seen`, contadores `s_idx_n`)
  lo usa un solo hilo: el que parsea (el worker; o el de render con `NWII_RENDER_WORKER=0`; el modo se
  fija una vez al principio).
- D no usa `HostPtr`: la regla "solo el worker copia/destruye HostPtr" sigue igual.
- Con la retracción de r11a (frames sustituidos cuando el render va lento) el parse sigue consumiendo
  todos los registros en orden del flujo (se parsea todo; solo se descartan draws).

## Verificación

Todas las corridas bajo `flock r9/render/pc/.runlock` con el arnés privado `r9/r11a/rt/`
(`verify_r12a.sh`; carpeta de corrida propia `r9/r11a/rt/pc/run`).

| Prueba | Resultado |
|---|---|
| Iris del título (como `r9/r11d/LEEME.md`: `NWII_INPUT_PHASES=6`, guion `0-330:100:20:4`, volcado cada 2 frames desde el 330), worker on | **correcto**: punto (frame ~354) → agujero que crece con el cielo y el escenario (366-378) → el negro se vuelve translúcido y desaparece (384+). Montaje: `r9/r12a/wipe_worker_on.png`; capturas en `r9/r11a/rt/pc/runs/r12a_wipe_w1/`. |
| Iris, worker off | igual de correcto, misma secuencia en los mismos frames (`r9/r12a/wipe_worker_off.png`). Píxel a píxel, 40 de 50 frames (330-429) difieren entre las dos corridas: es la variación normal entre corridas (tiempo real; ver r11a §4: ni el mismo binario repite frames), no un fallo; a la vista son iguales. |
| `checks.sh` 1x title/map/walk/lvl11 (frame check + caché de emisión) | 4 × `checks=220 identical=220 diff_lines=0`, `mismatches=0` (title, map, walk, lvl11; 13:31-13:38) |

No probado todavía en la Switch ni en citron (el resto de pruebas de r11a, como TSan, render lento,
parón y corrida de 7 min, no se repitió sobre r12a por falta de tiempo; el código del worker es el mismo).

## Estado (8-nov 13:40)

**Listo para probar en la Switch: `r9/r12a/bin/nsmbw_r12a.nro`** (sha256 `706dcb631478e8c8…`). Parche contra el
runtime de r10c: `r9/r12a/r12a_vs_r10c.patch`. A/B en la consola: el mismo NRO con
`NWII_RENDER_WORKER = "0"` en `debug.toml`; mirar en `fps_log` "threads … gxw" y `[RPROF] retracts=/held=`.
