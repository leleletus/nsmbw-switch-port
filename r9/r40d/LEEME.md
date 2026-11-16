# r40d: frente D — descartar fotogramas barato en el trabajador «gxw» (15-nov-2026)

Base: fuentes de v37a (= m5 + `NWII_V_*`): `base_v37a/nWiiRuntime` = r37v/nWiiRuntime con gx.cpp de
`r37v/remote_src` (r37v/nWiiRuntime/src/hle/gx.cpp ya lleva cambios posteriores de V: su propio `NWII_V_DROPFAST`,
sin publicar en su LEEME a las 13:05). Runtime propio en `nWiiRuntime/` (solo cambian gx.cpp y renderer_gl.cpp).
No se edita r37v, r38c, r39r2 ni r35m4. Sin commits.

## Herramientas
- `tools/ventanas.py <bench|pc_run> [demo] [--sum]`: por ventana de 2 s: fot. juego, dibujados, descartados
  (`dropped`) = retirados (`retracts`) + descartes en lotes con varios finales (`drop2`), render ms/fot,
  espera listo→tomado, latencia fin→listo, trabajador (fot. preparados, llamadas, busy/parse/split/decode ms por
  fotograma preparado; `busy` es tiempo de PARED entre marcas: incluye lo que la CPU de sonido le quita), y la
  línea `[D40]` si la hay.
- `tools/build_d.sh <n>`: binario de PC (runtime r40d + objetos m4 de r35m4), con `scripts/lim`.
- `tools/build_pgo_d.sh <tag>`: NRO de Switch en la otra laptop = objetos PGO de m5 (`r17pgo/nx/m5_pgo`) + las 5
  fuentes que difieren de m5src (gx, ios, os_hle_stubs, main, renderer_gl) con las órdenes exactas de ninja.
- `test/demos_fc.sh`, `test/checks.sh`: copias de r39r2/r37v con el arnés propio `pc/`.

## Interruptores (apagados por defecto; valor distinto de "0")
| Variable | Qué hace |
|---|---|
| `NWII_D_DROPFAST=1` | lote con dos o más finales de fotograma: la parte descartada `[0, prev_frame_end]` va a `pkt.pre` como un paquete retirado (sin draws, sin escrituras muertas de rcoal, copias EFB stale) y el fotograma bueno toma el camino rápido (conserva las entradas del parser); `Render()` empieza en `FramePacket::start` |
| `NWII_D_SLOWWORKER=<f>[,<us>]` | SOLO PC (no existe en el NRO): el trabajador duerme (f−1)× lo que tardó cada llamada → trabajador f veces más lento, como el de la Switch |

Línea nueva `[D40]` (trabajador, cada ~2 s, solo si hubo lotes con varios finales, con el interruptor encendido o
apagado): lotes por el camino barato / viejo, fotogramas tirados, órdenes, conservadas y µs por lote.

## Bitácora
- 13:05 inicio. Leídos r37v, r38c (§1), r36r (interruptores R), r39r2. **Aviso**: r37v/nWiiRuntime/src/hle/gx.cpp
  (13:02) ya trae un `NWII_V_DROPFAST` del frente V (misma idea, con `frame.erase` del tramo descartado: mueve el
  fotograma bueno y destruye los draws descartados en el trabajador) y un `NWII_V_SLOWWORKER_US` de PC. No lo uso; mi
  base es v37a (`r37v/remote_src`, = el NRO de V01-V04). Hay que elegir uno de los dos al unir.
- 13:10 **Medido en consola** (`tools/ventanas.py`, ventanas de 2 s; V03 = `bench_V03_v37a_late8_1115_1251`):
  * Hay DOS caminos de descarte: (a) **retirada** (el paquete listo no lo tomó el render y llega otro final:
    `RetractToPrefix`, con PREFIX+COALESCE de m5 ya barato) y (b) **lote con varios finales** (`prev_frame_end >= 0`
    → camino viejo de PrepFrame). En agua/castillo el (b) es casi todo: V03 agua retracts 0,2/s frente a drop2
    23,9/s; castillo 0,2 frente a 13,3/s; desierto/cielo/nubes < 1,6/s de cada tipo.
  * V03 agua: ventanas 1-6 (parte ligera, 4 000 draws/fot) régimen rápido: 60 dib, trabajador 11 ms/fot (parse 4,8,
    split 0,3, decode 6). Desde la ventana 7 (parte pesada, 7 200 draws/fot) cae al régimen lento y no sale:
    17-26 dib/s, 1 llamada = 1 fotograma preparado, **37-56 ms por fotograma preparado: parse 15-26 (2,5-3,5
    fotogramas del juego por lote), split 13-19, decode 8-9**; `[RCOAL]` por lote: ~35-40 k órdenes descartadas
    (≈2,5 fotogramas), 4,5-5 k conservadas. Render 17-18 ms y casi sin espera: el render espera al trabajador.
  * Mismo patrón en la fiesta (V01-V03: trabajador 28-31 ms/fot, split 12-13, dib 30-33) y en V02 agua ventanas 7-9
    y 19-20 (lento) alternando con tramos de juego a ~40 sin descartes (trabajador 16-18 ms/fot: parse 7,3, decode
    7,5: con el juego a 40 fot/s cabe).
  * Modelo (coste de pared por llamada = parse·k + c·(k−1) + decode, k = fotogramas por lote; ajusta V03): con el
    camino viejo (c ≈ 5,5 ms + decode completo) el punto fijo es k ≈ 2,7 → ~21 fps dib en agua pesada (medido 17-26);
    con c ≈ 1 ms y entradas conservadas el punto fijo es k = 1 (sin descartes) si parse+decode < 16,7-17,5 ms/fot.
    En agua pesada parse+decode ≈ 14,8 ms/fot de pared (V02) → al límite: mejora segura, 50+ no garantizado.
- 13:15 Implementado `NWII_D_DROPFAST` (gx.cpp `dfast::`, PrepFrame, RetractToPrefix, RenderPacket; renderer_gl.cpp
  `g_render_start`), línea `[D40]`, `NWII_D_SLOWWORKER` (PC). `NWII_D_DROPFAST_CHECK=1` (pruebas): compara orden a
  orden (tipo, registro, valor, carga, objeto del draw, stale) la lista que recibe el render con la que habría hecho el
  camino viejo. Fallo encontrado al revisar: un paquete con tramo descartado que luego se RETIRA volvía a compactar el
  tramo descartado (escrituras muertas ya superadas reaparecían DESPUÉS de las buenas) → `RetractToPrefix` empieza en
  `pkt.start` (corregido en d2, antes de cualquier prueba de imagen).
- 13:20 PC (`NSMBW_d1`, agua 70-95 s, `NWII_D_SLOWWORKER`): coste por lote con varios finales (`[D40]`): camino viejo
  1,6-3,2 ms (PC) frente a 0,26-1,2 ms el barato. Con f=6 (trabajador al límite, como la consola): agua apagado
  54,2 dib (mín. 13,5) → encendido 57,2 (mín. 44,5); castillo (PC no entra en régimen lento con f=6) 58,6 → 59,6.
  Con f=7 (trabajador por encima del presupuesto incluso sin descartes) ambos caen al régimen lento en la parte pesada:
  el descarte barato no puede compensar un trabajador que no da abasto ni para un fotograma por fotograma.
- 13:25 `tools/build_pgo_d.sh` con las fuentes base reproduce `v37a_pgo.nro`/`.elf` **byte a byte** (sha256
  36945895…0903a / 7f782552…e882). NRO **d2** (`bin/d2_pgo.nro`, sha256 f3373698…45bcd; fastmem 0 mal formadas).
- 13:35 **PC, imagen/exactitud con d2** (`NWII_D_DROPFAST=1 NWII_D_DROPFAST_CHECK=1 NWII_D_SLOWWORKER=6
  NWII_R11_SLOW_RENDER_US=12000`, + `NWII_R_COALESCE_CHECK=1`):
  `test/fc_d2_on.txt` (5 demos a 1,5789x, FRAMECHECK cada 10): **5/5 idénticas** (desierto 540/540, agua 500/500,
  cielo 540/540, nubes 540/540, castillo 520/520), coalesce 0 discrepancias, **D40CHECK 299 lotes / 7,4 M órdenes,
  0 distintos** (retiradas también ejercitadas: 447 en agua, 414 en castillo). `test/checks_d2_on.txt`
  (title/map/walk/lvl11 1x): 220/220 idénticas en las cuatro, diff_lines=0, D40CHECK 120 lotes / 3,4 M órdenes 0
  distintos, coalesce 0, salida normal (timeout). («pass1 differs» 22 y 116: el efecto conocido de copias EFB leídas
  antes de rehacerse, igual que en r36r/r39r2.)
- 13:36 Consola: D01_d2_off / D02_d2_on (mismo NRO d2, contiguas, entorno de V03, por delante de C06).
- 13:54 **Consola D01/D02** (mismo NRO d2, contiguas, 1020/460 MHz en todas las líneas, 80 ventanas, 5 escenas,
  IMAGEN OK 3/36 en ambas, hojas `capturas.jpg` miradas: limpias, la fiesta se ve bien; negras = fundidos/iris).
  `bench_D01_d2_off_1115_1335` / `bench_D02_d2_on_1115_1344`. Ver tabla en «Resultado».
- 14:05 Perfil de muestras del trabajador (D02, `profile_gxw.txt` + `bin/d2_pgo.elf`): `DropPart` 0,45 % del hilo,
  `_M_release` (liberar DrawRaw de los draws tirados) 0,42 %; el resto es parse (ReplayMemo 5,1, ParseStream 4,6,
  copias de GXCommand/vector/new ~7) y decode (DecodeDraw 4,0).

## Resultado (consola, V_WAKE=1 + V_LATE=8, S2 por defecto; `tools/ventanas.py --sum` salvo «dib (por escena)»)
Celdas: fps dibujados (por_escena_real) · fps juego · render ms/fot · trabajador ms/fot preparado (pared).
| escena | V03 (v37a) | D01 d2 apagado | D02 d2 `NWII_D_DROPFAST=1` |
|---|---|---|---|
| desierto4j | 58,4 · 59,8 · 14,2 · 7,4 | 58,2 · 59,8 · 14,6 · 7,4 | **58,6** · 59,8 · 14,3 · 7,1 |
| agua_pinguino | 31,7 · 56,8 · 15,2 · 34,2 | 34,1 · 55,1 · 15,9 · 29,9 | **43,4** · 54,5 · 16,2 · 20,4 (mín. ventana 17,5 → **30,0**) |
| cielo_paratroopas | 58,2 · 59,9 · 14,5 · 7,7 | 57,9 · 59,8 · 15,5 · 8,1 | **58,6** · 59,9 · 14,7 · 7,5 |
| nubes_plataformas | 59,9 · 59,9 · 10,3 · 6,1 | 59,9 · 59,9 · 10,7 · 6,3 | **59,9** · 59,9 · 10,4 · 6,3 |
| castillo (entero) | 46,0 · 59,4 · 13,3 · 17,9 | 46,7 · 59,4 · 13,5 · 16,0 | **54,1** · 59,5 · 13,9 · 11,2 |
| — fiesta (fiesta.py) | 30,3 · 59,6 · 18,8 · 31,5 | 33,5 · 59,9 · 19,0 · 27,0 | **48,4** · 59,9 · 18,5 · 17,1 |

Coste de un lote con varios finales (`[D40]`, µs de pared por lote, `tools/d40.py`): camino viejo agua **15 300**
(1 164 lotes, 30,8 k órdenes/lote), castillo 14 800, desierto/cielo/nubes 9 000-10 900; camino barato agua **3 460**
(828 lotes, 22,6 k órdenes, 3,0 k conservadas por lote), castillo 2 190, resto 1 400-2 000. Además el fotograma
bueno conserva las entradas del parser (decode agua pesada 8-9 → 7-8 ms).

Lectura (inferida de las ventanas): la fiesta ya no se queda en el régimen lento (trabajador 27 → 17 ms/fot; lo que
queda es render: 18-20 ms/fot en su segunda mitad, con 7-13 retiradas/s → límite ~50 fps, terreno de R2). En agua
pesada (7 200 draws/fot) el trabajador sigue SATURADO: k ≈ 1,4-1,7 fotogramas por lote; por fotograma preparado
parse 9-15 (≈7,7 por fotograma del juego), descarte 3-6, decode 7-8,5 → ~37-45 dib. Un fotograma tirado sigue
costando su parse entero (~7,7 ms) + ~3 ms de descarte, frente a ~16 ms uno dibujado: con el juego a 55 fot/s el
núcleo 2 no da para todos. **Criterio no cumplido en agua (43,4 < 50) ni en la fiesta (48,4 < 50).**

## Riesgos
- Exactitud: la lista que recibe el render es la misma que la del camino viejo (D40CHECK 0 distintos en 10,8 M
  órdenes, PC); FRAMECHECK y coalesce iguales. Depende de rcoal (sus barreras 0x52/0x64/0x65), ya en m5.
- `FramePacket::start`: todo lector de `pkt.frame` debe empezar ahí. Revisados: RenderOnce (también en las pasadas
  de FRAMECHECK), RetractToPrefix (arreglado), R7CHECK, `[WPROF] cmds`, FREECHUNK/LATEFREE (liberan el vector
  entero, bien). NO ajustados (depuración, sin hilo trabajador): FRAMESIG cuenta también los draws tirados.
- Los draws tirados sueltan sus DrawRaw/vértices en el trabajador (como antes); sus restos quedan en el vector hasta
  liberar el paquete (memoria: el vector del fotograma lleva 1-2 fotogramas más de órdenes vacías).
- Duplica la idea de `NWII_V_DROPFAST` (r37v, sin publicar): hay que quedarse con UNO. El de V borra el tramo con
  `frame.erase` (mueve el fotograma bueno y destruye en el trabajador); este no mueve nada y tiene comprobación.
- El parche aplica limpio (solo desplazamientos) sobre el árbol actual de r39r2 (R2).

## Qué queda
- Agua pesada: el límite es el trabajador en el núcleo 2 (parse ~7,7 + decode ~7,7 ms por fotograma, con la CPU de
  sonido quitándole ~26 %). Palancas: (1) parse «ligero» de los fotogramas que se sabe que se tiran (al tomar el FIFO
  `s_fifo_frames` dice cuántos finales trae el lote; los fotogramas anteriores al último no necesitan DrawRaw ni las
  órdenes de draw, solo el estado) — cambio en fifo_parser.cpp con riesgo (anillos de índices, memo de listas);
  (2) abaratar parse/decode en general; (3) política de ritmo: frenar al juego cuando el trabajador no da abasto
  (`NWII_FIFO_MAX_FRAMES` ya existe; es terreno de V y puede retener S/sonido: no probado).
- `DropPart` aún 2-3,5 ms por lote en consola (~150 ns por orden: fallos de caché + liberar DrawRaw); se puede
  bajar algo (prefetch de las dos líneas, más adelantado) pero en agua es ~15 % del trabajador como mucho.
- Fiesta: render 18-20 ms/fot (R2).

## Archivos
- NRO: `bin/d2_pgo.nro` (sha256 f33736987bb4e461dcb059a7e2c8f6faf81bfc0fd8455c36da6f5298bed45bcd), elf `bin/d2_pgo.elf`.
  PC: `bin/NSMBW_d2[_input_test]`.
- Parche: `r40d_vs_v37a.patch` (`cd <padre de nWiiRuntime v37a> && patch -p0 < r40d_vs_v37a.patch`; gx.cpp y
  renderer_gl.cpp; comprobado que reproduce `nWiiRuntime/`).
- Pruebas PC: `test/fc_d2_on.txt`, `test/checks_d2_on.txt`; corridas en `pc/runs/`.

# Fase 2 (14:00-): abaratar el trabajador «gxw» en agua_pinguino

## Desglose medido (D02 + D03 = d2 solo agua con `NWII_WPROF=2`, `bench_D03_d2_agua_diag_1115_1400`)
- Agua pesada (7 200-7 800 draws GX/fot, 21-24 k órdenes), por fotograma preparado (µs de PARED, consola):
  ventanas en régimen ~rápido (≈400 llamadas/2 s): **busy 16,5-18,6 = parse 7,8-9,0 · split 1,0-2,3 · decode
  4,9-5,1 · prep 2,0-2,1 · lock 0,02**; ventanas con lotes de 1,3-1,7 fotogramas: parse 9-12, split 3,5-5,6.
  `lock` (`g_fifo_mutex` + copia) ~25 µs/fot: despreciable. Expulsión por la CPU de sonido: núcleo 2 (`hilos.py`)
  gxw 63-66 % de CPU frente a ~91 % de pared ocupada (WPROF) → ~30 % del tiempo «ocupado» es expulsión.
- Draws (PC, `NWII_D_STATS=1`, línea `[D40S]`): de ~7 500 draws/fot, ~5 900 salen del memo de listas (ya
  preparados por el parser) y **~1 600 son draws inmediatos del FIFO** (quads de 4 vértices, ~6 500 vértices y
  ~130 KB de bytes por fotograma), sin entrada: TODO el decode/prep es de ellos (~3 µs decode + ~1,3 µs prep por
  draw en consola). Solo el 17-36 % repite exacto el del fotograma anterior (partículas que se mueven): no compensa
  una caché entre fotogramas. Formatos (`[D40V]`): 66 % pos F32 xyz directa + color RGBA8 directo + tex0/tex1
  directas; casi todos directos (77 de 1 600 usan índices).
- Tras el corte del fotograma queda una «cola» del siguiente fotograma ya parseada (`[GXPROF] carry`): **13-24 k
  órdenes en agua pesada** (el juego escribe casi todo el fotograma siguiente antes de que el trabajador llegue),
  que se mueven una a una (128 B) fuera del vector: en PC 10-16 % del hilo (`_M_range_insert`).
- Perfil de muestras de consola (D03, solo agua, `profile_gxw.txt` + elf d2): DecodeDraw 9,2 % de las muestras,
  ParseStream 5,7, operator new 5,3, vector<GXCommand> (crecer/mover/emplace) ~10, ReplayMemo 5,0, ReadVectorAttr
  2,9, xf::Analyze 2,6, WriteVerts 1,8, DLContentHash 1,9, ParseBP 1,8, DropPart 1,6, espera 34,6 %.

## Interruptores nuevos (todos apagados; valor distinto de "0")
| Variable | Qué hace | Por qué es exacto |
|---|---|---|
| `NWII_D_PARSESTOP=1` | el parse se para justo tras el último final de fotograma de los bytes tomados (`s_fifo_frames`); el resto se parsea en la llamada siguiente en vez de moverlo como «cola» | mismas órdenes, mismo orden; solo cambia el estado del parser que viaja con el paquete (CP/VAT/arrays/semillas BP), que pasa a ser el del final del fotograma (el caso «sin cola», que ya ocurre) |
| `NWII_D_LIGHTPARSE=1` (requiere PARSESTOP y DROPFAST) | si los bytes traen P ≥ 2 finales, los P−1 primeros fotogramas se parsean sin escribir sus draws (inmediatos saltados, listas reproducidas sin draws, memo sin tocar) | sus draws se tiran de todos modos; el parser no tiene estado que dependa de los draws; si la cuenta fallara se tira también el último (nunca se dibuja un fotograma sin draws). `NWII_D_LIGHTPARSE_CHECK=1`: parse completo contando fallos de predicción |
| `NWII_D_FASTDECODE=1` | decode sin divisiones (x/2^s = x·2^-s; colores por tablas llenadas con la misma expresión) y vértices construidos en su sitio | `NWII_D_FASTDECODE_CHECK=1` decodifica otra vez a la antigua y compara campo a campo |
| `NWII_D_EAGERDECODE=1` | draw inmediato con todos los atributos directos: se decodifica al parsear desde un DrawRaw reutilizado (sin reservar DrawRaw ni copiar sus bytes por draw) | solo lee sus bytes y el estado del parser de ese momento (lo mismo que copiaría al DrawRaw); `NWII_D_EAGERDECODE_CHECK=1` compara con el decode diferido |
| `NWII_D_FASTPREP=1` | GX_PrepDrawForRender (renderer_gl.cpp, draws empaquetados del trabajador): xf::Analyze y la caja en una pasada | misma Info (también cuando Analyze se rinde a medias) y misma caja; `NWII_D_FASTPREP_CHECK=1` compara |
| `NWII_D_STATS=1` | diagnóstico: `[D40S]`/`[D40V]` (draws por tipo, reutilización, formatos) | — |

Nota PARSESTOP (desde d10): los bytes del juego se toman con `swap` bajo `g_fifo_mutex` y se añaden fuera (ver 15:45).
Líneas nuevas: `[D40] parses detenidos … | finales contados por el juego N, vistos por el parser N, resto max B`,
`[D40L] parse ligero: µs por fotograma condenado`, `[D40L] … draws no escritos … predicción fallida`.

## Bitácora fase 2
- 14:05 D03 (diagnóstico solo agua, d2+DROPFAST+WPROF=2) encolado por delante. Perfil PC (perf LBR, `tools/prof_agua.sh`).
- 14:20 PARSESTOP en PC (agua, trabajador normal): split 155-235 → 27 µs/fot, carry 13-16 k → 0, busy 1,86-2,30 →
  1,64-1,85 ms/fot (−15 %).
- 14:25 FASTDECODE: `[D40DEC_CHECK]` 3 M draws, 0 distintos. En PC no se nota (la división es barata en x86).
- 14:30 LIGHTPARSE en PC (trabajador ×6): coste por lote 731 → 255 µs, órdenes por lote 28,8 k → 16,5 k, ~7 500
  draws no escritos por fotograma tirado, predicción fallida 0 (también 0 en modo comprobación); D40CHECK 0.
- 14:35 EAGERDECODE: `[D40EAGER_CHECK]` 2,8 M draws 0 distintos; en PC neutro (parse +0,45, decode −0,6 ms; malloc
  de PC barato). NRO **d5** (`bin/d5_pgo.nro`, sha256 0dbfd421…652c; fastmem 0). `build_pgo_d.sh` ahora recompila
  también fifo_parser.cpp con el include de r40d (solo difiere fifo_parser.h, sin cambio de disposición;
  fifo_parser.cpp base = el de m4src remoto, md5 6eebf77c).
- 14:55 **PC, d5 con todo** (DROPFAST, PARSESTOP, LIGHTPARSE, FASTDECODE, EAGERDECODE + sus comprobaciones,
  trabajador ×6, render lento 12 ms): `test/fc_d5_all.txt` **5/5 FRAMECHECK idénticas** (desierto 540, agua 480,
  cielo 540, nubes 560, castillo 540), coalesce 0, D40CHECK 484 lotes/11,4 M órdenes 0 distintos, DEC_CHECK 8,6 M
  draws 0, EAGER_CHECK 6,6 M draws 0, light: 4,4 M draws no escritos, predicción fallida 0; sin fallos.
- 14:50 Consola: D04_d5_on (d5, solo DROPFAST = base de esta fase) / D05_d5_all (todo), contiguas, por delante.
- 15:00 `test/checks_d5_all.txt` (title/map/walk/lvl11 1x, todo + comprobaciones, trabajador ×6, render lento):
  FRAMECHECK idénticas en las cuatro (220/140/160/200; menos comprobaciones porque se compilaba a la vez),
  diff_lines=0, D40CHECK 1 193 lotes / 34 M órdenes 0 distintos, coalesce 0, salida normal.
- 15:00 `NWII_D_FASTPREP=1` (renderer_gl.cpp, GX_PrepDrawForRender, solo draws empaquetados del trabajador):
  xf::Analyze + BBoxOf en una pasada; `NWII_D_FASTPREP_CHECK=1` compara Info y caja. Binario d6.
- 15:10 **Consola D04/D05** (NRO d5, contiguas, 84 líneas a 1020/460, 80 ventanas, 5 escenas; IMAGEN OK 3/38 y
  2/37; hoja de D05 mirada: limpia, iris/fundidos normales). `bench_D04_d5_on_1115_1452` (solo DROPFAST) /
  `bench_D05_d5_all_1115_1501` (DROPFAST+PARSESTOP+LIGHTPARSE+FASTDECODE+EAGERDECODE):
  dib (por escena) · juego · render ms/fot · trabajador ms/fot (pared):
  desierto 58,3→**59,1** · 59,8 · 14,7→14,2 · 7,5→7,2 | **agua 39,9→48,2** (ventanas 41,1→49,2, mín. 28,5→39,5)
  · 55,5 · 16,9→15,4 · 23,4→18,2 | cielo 57,9→57,9 · 15,6→14,8 · 8,2→8,0 | nubes 59,9→60,0 | castillo 51,7→**55,9**
  · 14,4→13,6 · 11,3→8,6 | **fiesta 45,4→53,3** (trabajador 17,2→11,3; render 19,6→17,2).
  OJO variación entre corridas: D04 (= configuración de D02) dio agua 39,9 frente a 43,4 de D02.
  Agua pesada D05: aún 1,1-1,4 fotogramas por lote; por fotograma preparado parse 13-16 (con el decode
  adelantado dentro), decode+prep 5-6, split 0,3-0,7; lotes con varios finales 1,3 ms (antes 3,5); `[D40L]`
  predicción fallida 0. En tramos sin descartes (juego a 44-49): 18-19 ms/fot (parse 13, decode 5): el decode
  adelantado NO sale más barato que el diferido en consola (parse +5,7, decode −2,5 frente a V02) → D06/D07 lo
  aíslan. Perfil D05: `Tabs()` (tablas del decode rápido con guarda estática) 2 % → ahora objeto global (d7).
- 15:15 NRO **d7** (`bin/d7_pgo.nro`, sha256 26b458f5…16b6; fastmem 0) = d5 + FASTPREP + Tabs global. PC con
  todas las comprobaciones (trabajador ×6): DEC/EAGER/PREP 1,6 M draws 0 distintos, D40CHECK 81 lotes 0.
  Encolados D06_d7_noeager (todo salvo EAGERDECODE, + FASTPREP) y D07_d7_allp (todo + FASTPREP), contiguas.
- 15:36 **D06_d7_noeager se congeló** a los ~10 s de desierto (5 ventanas; la consola volvió sola al hbmenu, 1020/460).
  Perfil de muestras (`profile_gxw/main/cpu`, elf d7): trabajador 88 % parado DENTRO de `memmove` ←
  `vector<uint8_t>::_M_range_insert` ← PrepFrame = la copia `s_parse_buf.insert(g_hw_fifo)` que se hace CON
  `g_fifo_mutex` tomado; render y juego 90 % en `sched_yield` (FifoLock). Es la misma firma que Z22 (m5, frente C):
  el trabajador expulsado en el núcleo 2 con el candado en la mano y nunca vuelve a correr (inversión de
  prioridades con las esperas activas de m5; mis NRO d2/d5/d7 no llevan el arreglo de C). Mi parte (INFERIDO):
  con `NWII_D_PARSESTOP` `s_parse_buf` casi nunca está vacío, así que cada toma COPIA los bytes del juego bajo el
  candado en vez del intercambio O(1) de antes → la ventana de esa inversión se agranda mucho. No reproducido en PC
  (2 corridas desierto+agua con el entorno de D06: normales; cuentas de finales juego/parser idénticas, 8 881/8 881;
  resto sin parsear ≤ 218 KB). D07 (d7 + EAGER) también se colgó, pero con `[NullCall] … site 0x8027b930` (cuelgue
  de S2 que el frente M reproduce con m6a TODO apagado; lo lleva C): no es de estos interruptores.
- 15:45 **Arreglo en d10**: con PARSESTOP, los bytes del juego se toman con un `swap` (O(1)) a un búfer de reserva
  bajo el candado y se añaden a `s_parse_buf` FUERA del candado. PC: `test/fc_d10_noeager.txt` (combinación de D06 +
  comprobaciones, trabajador ×6, render lento) 5/5 FRAMECHECK idénticas, D40CHECK/DEC 0, predicción 0, sin cuelgues.
  NRO `bin/d10_pgo.nro` (sha256 26197a0e…a7b4) — base v37a, sin el arreglo de C: NO lo uso en consola.
- 15:50 **Rebase sobre m6a** (orden del coordinador): `nWiiRuntime_m6` = `r41m/nWiiRuntime` (m6a = m5+V+C+D d2; copia
  intacta en `ref/m6a`) + `d10_vs_d2.patch` (fase 2 de D sobre d2; `ref/d2` = base_v37a + r41m/parches/d2_vs_v37a.diff).
  Aplica con desplazamientos. `tools/build_m6d.sh` (PC) y `tools/build_pgo_m6d.sh` (NRO; copia de la de M que además
  acepta fifo_parser.h cambiado). NRO **m6d1** `bin/m6d1_pgo.nro` sha256 3820fc3a…647d (fastmem 0); PC `bin/NSMBW_m6d1`.
- 16:00 **PC sobre m6d1** (m6a + fase 2), todo encendido + comprobaciones, trabajador ×6, render lento:
  `test/fc_m6d1_all.txt`: desierto 480/480, agua 400/400, nubes 480/480, castillo 460/460 idénticas, D40CHECK/DEC/
  EAGER/PREP 0, predicción 0; **cielo: 298/300 (2 diferencias) y SIGSEGV** — el hilo de render se queda casi parado
  (`[VIGIA]` #1-#3: 5 s sin progreso, render 2810→2820 fot.; S2 «espera petición al juego 5,8 s»; 557 draws GL/fot
  frente a ~290 normales), las dos diferencias de FRAMECHECK caen justo en ese tramo y luego revienta dentro del
  compilador de shaders de Mesa (`CompileTevProgram` ← SetupDrawState ← RenderOnce; core en coredumpctl, pid 793787).
  **No se repite**: `test/cielo_ab.txt` cielo 120 s con FRAMECHECK: m6d1 todo 560/560, m6a+DROPFAST 520/520,
  m6d1+DROPFAST 620/620 (y una cuarta en curso), sin [VIGIA] ni NullCall. El PC estaba cargado (compilación remota +
  local a la vez). Sin causa encontrada: queda anotado como riesgo abierto.
  `test/checks_m6d1_all.txt` (title/map/walk/lvl11 1x, `NWII_S2_CHECK=1`, todo + comprobaciones): 200/120/120/200
  idénticas, diff_lines=0, D40CHECK 45 M órdenes 0 distintos, coalesce 0, salida normal.
- 16:20 Consola encoladas detrás de M07/M09 (no por delante de M02/M04/M03): D08_m6d1_on (DROPFAST) y D09_m6d1_allp
  (DROPFAST+PARSESTOP+LIGHTPARSE+FASTDECODE+EAGERDECODE+FASTPREP), con WAKE+LATE=8, lista fija.
- 17:15 Consola **D08_m6d1_on** (`bench_D08_m6d1_on_1115_1705`, DROPFAST solo; 80 ventanas, 1020/460, IMAGEN OK
  2/35): desierto 58,6 · agua 41,9 (juego 55,8, render 16,0, trabajador 22,0) · cielo 57,9 · nubes 59,9 ·
  castillo 54,3 · fiesta 49,3.
- 17:05-17:30 **`NWII_D_RENDERPREP=1`** (idea del coordinador: pasar trabajo del núcleo 2 al hilo de render, núcleo 0,
  que en agua con R2 tiene ~4-5 ms libres). Evaluación con mis números (agua pesada, por fotograma preparado, pared):
  parse 7,7-9 (estrictamente en orden: no se puede repartir sin rehacer el parser), split ≤0,7 con PARSESTOP,
  **decode+prep 5-7,5 = los ~1 600 draws inmediatos del FIFO** (todos directos salvo ~77), espera por el candado
  ~0,02, expulsión por la CPU de sonido ~30 % del tiempo de pared. El decode+empaquetado de un draw inmediato con
  atributos directos depende solo de sus bytes y de la instantánea del parser (no lee RAM del invitado) y su
  DrawRaw es solo suyo (no comparte cuenta con la caché de listas) → **viable y exacto** en otro hilo. Hecho sobre
  **m6e** (`nWiiRuntime_m6e` = `r41m/nWiiRuntime_m6e` + `d10_vs_d2.patch` + esto; copia intacta en `ref/m6e`):
  el trabajador deja esos draws sin entrada (`FramePacket::rprep`) y el render los decodifica y empaqueta
  (mismas funciones) justo antes de `Render()`, dentro del tiempo medido del render (`[D40R]`). Arreglo necesario
  de paso: `g_fastdec` y `g_track` (puntero global del rastreo de rangos del decode) pasan a `thread_local`
  (`g_track` era global y compartido: con dos hilos decodificando se pisarían).
  PC agua: trabajador 1,8-2,2 → 1,1-1,4 ms/fot, render +0,6-0,8 ms (1 560 draws/fot). `test/fc_m6e1_rp.txt`:
  **5/5 FRAMECHECK idénticas** (desierto 540, agua 520, cielo 540, nubes 560, castillo 540), coalesce 0, D40CHECK 0,
  DEC_CHECK 0 (también en el render), PREP_CHECK 0, predicción 0, sin [VIGIA].
  NRO **m6e1** `bin/m6e1_pgo.nro` sha256 430bee2b…646f (fastmem 0). Encoladas D11_m6e1_def (m6e por defecto) y
  D12_m6e1_rp (+PARSESTOP, LIGHTPARSE, FASTDECODE, FASTPREP, RENDERPREP), contiguas, detrás de M11.
- 17:22 **Consola D09_m6d1_allp** (`bench_D09_m6d1_allp_1115_1713`; m6a + fase 2: DROPFAST+PARSESTOP+LIGHTPARSE+
  FASTDECODE+EAGERDECODE+FASTPREP, WAKE+LATE=8; 80 ventanas, 84 líneas a 1020/460, IMAGEN OK 0/36, hoja mirada:
  limpia — fundido en 120, iris en 370). Contra **D08** (mismo NRO, solo DROPFAST, contigua):
  desierto 58,6→**59,1** · agua **41,9→52,0** (ventanas 42,5→52,0, mín. 42,0; juego 56,2; render 16,0→15,4;
  trabajador 22,0→16,4 ms/fot) · cielo 57,9→58,3 · nubes 59,9→60,0 · castillo 54,3→**56,5** · fiesta 49,3→**53,7**
  (trabajador 16,0→11,3). **Cumple el criterio** (agua ≥ 50 sin ventanas < 40, fiesta ≥ 50, resto ≥ V03).
  Fotograma condenado (parse ligero, `[D40L]`): 5,3-6,6 ms + descarte 1,5 ms/lote → el objetivo ≤ 2 ms NO se cumple.
- 17:30 PC m6e1 (`test/checks_m6e1_rp.txt`, title/map/walk/lvl11 1x con RENDERPREP y comprobaciones): 220/220/220/200
  idénticas, D40CHECK 0, coalesce 0.
- 17:30 **Flujo del agua** (pedido del coordinador / frente W; PC, `NWII_D_STATS`, línea `[D40M]`): ~1 630 draws del FIFO
  por fotograma (quads de 4 vértices; 66 % pos F32 xyz + color RGBA8 + tex0/tex1 S16 directos), de los cuales
  **~1 355 van pegados al anterior con el MISMO formato y estado** (VAT, arrays, matrices por defecto, sin ninguna
  orden en medio): ~275 tramos por fotograma, el mayor de **1 100 draws seguidos**. Fundirlos en el parser (quads/
  triángulos, no tiras) dejaría ~275 draws: es la siguiente palanca (sin hacer). El «split» de 4,4 ms de M04 era la
  cola del fotograma siguiente movida + el descarte viejo: PARSESTOP + DROPFAST lo dejan en 0,1-0,3 ms.

## FASE 2 CONGELADA (para M / m6f) — 17:45
- **Fuentes**: `r9/r40d/fuentes_fase2_final/nWiiRuntime/` (solo lectura) = EXACTAMENTE las fuentes con las que se
  compiló `bin/m6e1_pgo.nro` (17:11): copiadas de vuelta de la otra laptop (`r9/r40d/src_m6e1`, lo que rsync subió
  para esa compilación). Sin nada de MERGE.
- **Parche**: `r9/r40d/fase2_final_vs_m6e.patch` contra `r41m/nWiiRuntime_m6e` (= `ref/m6e`, comprobado idéntico al
  actual de r41m). Se aplica con `cd <copia de nWiiRuntime_m6e> && patch -p1 < fase2_final_vs_m6e.patch`; comprobado
  que reproduce `fuentes_fase2_final/nWiiRuntime` exacto (diff -r vacío).
- **NRO**: recompilado de esa copia con `tools/build_pgo_m6d.sh f2final fuentes_fase2_final/nWiiRuntime` →
  `bin/f2final_pgo.nro` sha256 90b4445d…c3fe (elf 84a68ca4…69c7; fastmem 0). Frente a `bin/m6e1_pgo.nro`
  (430bee2b…646f) el ELF solo difiere en las tres cadenas `__DATE__ __TIME__` y el GNU build-id (mismo tamaño) →
  m6e1 ES la fase 2 final; D11/D12 (NRO m6e1) valen como medida.
- **Qué pasó con `m6e1_vs_m6e.patch` de las 17:28** (renombrado `OBSOLETO_m6e1_vs_m6e_17h28.patch`): se generó del árbol
  de trabajo, que a las 17:23 ya llevaba el diagnóstico `[D40M]` (bloque en PrepFrame, solo activo con
  `NWII_D_STATS=1`, añadido para medir los tramos fundibles) → PrepFrame más grande. Funcionalmente igual con
  `NWII_D_STATS` sin poner, pero NO es lo compilado: usar el parche congelado.
- MERGE se trabaja aparte en `nWiiRuntime_merge` (antes `nWiiRuntime_m6e`).

## Fase 3: NWII_D_MERGE (árbol `nWiiRuntime_merge` = fase 2 congelada + esto)
- 17:35 **Cielo parado + SIGSEGV (PC, m6d1) — causa**: el núcleo dice `i915 … GPU HANG: ecode 12:1:85dffffb, in
  NSMBW_m6d1_inpu [793787]` y `context reset due to GPU hang` a las 16:01:31, :39 y :49 (journalctl -k), justo cuando
  el render se quedó casi parado ([VIGIA]) y salieron las 2 diferencias; el SIGSEGV es Mesa copiando
  (`rep movsb`) 18 848 bytes a una dirección de una correspondencia de la GPU que ya no existe en el core
  (`anon_inode:i915.gem`), dentro del compilador de shaders tras el reinicio del contexto. En una semana de corridas
  de PC no hay ningún otro «GPU HANG» en el registro del núcleo. Nuestro código: en m6d1 el decode solo corre en el
  trabajador (renderer_gl.cpp no llama a DecodeDraw), así que la carrera de `g_fastdec`/`g_track` NO es posible en
  ese binario. Conclusión (con esa evidencia): cuelgue de la GPU/driver i915 del PC (Mesa 26.2.6) que deja el
  contexto perdido; no puedo descartar al 100 % que un flujo GL concreto lo dispare, pero no se repite (4 corridas
  más de cielo) y la imagen de las comparaciones fuera de esos 18 s es idéntica.
- 17:35 Implementado `NWII_D_MERGE=1` (fifo_parser.cpp, ParseStream): un draw del FIFO que va JUSTO detrás de otro
  draw del FIFO (ninguna orden en medio = mismo estado del render), con la misma primitiva (quads 0x80 o triángulos
  0x90: listas, la concatenación son los mismos primitivos en el mismo orden), la misma instantánea (VAT, arrays,
  matrices por defecto, sin transformación al parsear) y el anterior con un número entero de primitivos, se añade
  al anterior (bytes y cuenta) en vez de ser un draw nuevo; máx. 4 096 vértices por draw fundido. No toca listas
  (memo), ni draws ya decodificados (incompatible con EAGERDECODE: ese no funde), ni cruza finales de fotograma
  (la copia XFB es una orden en medio). La caja de recorte se calcula de todos los vértices = la unión.
  `NWII_D_MERGE_CHECK=<n>`: cada n-ésimo fotograma con fusiones el render lo dibuja también desde una copia con cada
  draw fundido partido en sus trozos originales (mismas listas apagadas en ambas pasadas) y compara la XFB píxel a
  píxel (`[D40MERGE_CHECK]`). Línea `[D40G]`: draws fundidos por fotograma.
- 17:40 PC agua (`pc_m6e3_mchk`, 1,5789x): **1 353-1 358 draws fundidos por fotograma** (draws del FIFO 1 630 → ~280,
  tramo máx. 1100 → 2); `[D40MERGE_CHECK]` 600/600 idénticas, 0 píxeles. A/B PC agua (perfil «power-saver» del PC
  en ese momento, mismas condiciones): GX draws 7 100-7 300 → 5 800-6 000; preparación en el render (RENDERPREP)
  2,2-2,3 → 1,4 ms/fot; render 5,8-7,0 → 6,0-6,7 ms; trabajador 3,1-3,5 → 3,0-3,3 ms (en PC el decode ya lo hacía el
  render). Draws GL iguales (~150-170: el render ya los agrupaba). NRO `bin/m6e3_pgo.nro` (sha256 7d970ed2…f209;
  incluye el diagnóstico [D40M] y MERGE_CHECK).
- 17:57 **PC MERGE verificado** (NSMBW_m6e3, fase 2 + RENDERPREP + MERGE + todas las comprobaciones, trabajador ×6,
  render lento; PC en «power-saver»): `test/fc_m6e3_merge.txt` **5/5 FRAMECHECK idénticas** (300/300/280/380/320),
  **D40MERGE_CHECK 2 000 comparaciones fundido↔partido, 0 píxeles distintos** (agua 1 161 fundidos/fot, nubes 290,
  desierto 72, cielo 68), coalesce 0, D40CHECK/DEC/PREP 0, predicción 0, sin fallos. `test/checks_m6e3_merge.txt`:
  title/map/walk/lvl11 idénticas (100/40/40/140), D40MERGE_CHECK 1 980 comparaciones 0 píxeles, D40CHECK 0, coalesce 0.
  Parches: `merge_vs_fase2.patch` (sobre `fuentes_fase2_final`) y `merge_vs_m6e.patch` (sobre r41m m6e); el NRO
  m6e3 se compiló de este mismo árbol (comprobado con las fuentes subidas). Consola: D13_m6e3_rp / D14_m6e3_rpm
  (mismo NRO; rpm = rp + MERGE) encoladas al final, contiguas.
- 18:35 **Consola D10/D11/D12** (todas 80 ventanas, 84 líneas a 1020/460, IMAGEN OK 2/37, 2/36, 2/35).
  * D10_m6d1_noeager (`bench_D10_m6d1_noeager_1115_1722`, base VIEJA con perfilador, contigua a D09): agua 50,9
    (mín. 41,5) · fiesta 54,1 · castillo 56,5 · desierto 59,2 · cielo 59,1 · nubes 60,0. Frente a D09 (con EAGER)
    agua 52,0 / fiesta 53,7: EAGERDECODE no aporta de forma medible (y no deja fundir) → fuera.
  * D11_m6e1_def / D12_m6e1_rp (base NUEVA sin perfilador, contiguas; m6e = V+DROPFAST+R2 por defecto):
    `bench_D11_m6e1_def_1115_1816` / `bench_D12_m6e1_rp_1115_1824` (rp = PARSESTOP+LIGHTPARSE+FASTDECODE+FASTPREP+
    RENDERPREP): desierto 59,1→59,5 · **agua 42,9→47,5** (ventanas 43,0→47,8, mín. 30,0→39,5) · cielo 57,1→57,0 ·
    nubes 59,9→60,0 · castillo 56,9→57,6 · fiesta 54,2→55,5. En agua el trabajador baja **21,7→11,0 ms/fot** pero el
    render sube **12,3→18,4 ms/fot**: RENDERPREP le pasa 6,0-6,5 ms/fot (`[D40R]`, ~1 450 draws, ~4,3 µs/draw en el
    A57), más de los ~4-5 ms que tenía libres → ahora manda el render (7,7 retiradas/s). RENDERPREP tal cual se pasa
    de frenada: NO por defecto; falta medir fase 2 sin él sobre esta base (D13).
- 18:40 Cola: retiradas D13_m6e3_rp/D14_m6e3_rpm (`.retirado`); en su lugar, mismo NRO m6e3, contiguas, al final:
  **D13_m6e3_p2** (DROPFAST+PARSESTOP+LIGHTPARSE+FASTDECODE+FASTPREP), **D14_m6e3_p2m** (+MERGE),
  **D15_m6e3_rpm** (+RENDERPREP+MERGE).
- 18:45 PC MERGE sin RENDERPREP (`test/fc_m6e3_p2m.txt`, combinación de D14 + comprobaciones, trabajador ×6, render
  lento): 5/5 FRAMECHECK idénticas (400/340/400/440/380), D40MERGE_CHECK 2 640 comparaciones 0 píxeles, coalesce 0,
  D40CHECK 0, predicción 0.
