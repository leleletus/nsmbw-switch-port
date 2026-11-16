# r39r2: frente R2 — hilo de render en desierto4j / agua_pinguino / cielo_paratroopas — 15-nov-2026

Meta: render ≤ 11 ms por fotograma dibujado en esas tres escenas (m5: ~14 · 14,8 · 14,3 ms), imagen idéntica.
Runtime: copia (--reflink) de `r9/r35m4/nWiiRuntime_m5` en `nWiiRuntime/` (no se toca r35m4 ni r36r).
Compilar PC: `JOBS=5 ~/nsmbw-port/scripts/lim tools/build_r2.sh <nombre> m4 pc` (objetos del juego de r35m4/m4, sin PGO).
Switch: `tools/build_pgo_r2.sh <tag>` (objetos PGO de m5 + 4 archivos de este runtime, órdenes exactas de ninja; con
las fuentes de m5 reproduce m5_pgo.nro byte a byte). Arnés de PC propio: `pc/`; `test/demos_fc.sh`, `test/checks.sh`.

## RESUMEN (15:50)
- **Desglose (antes, R2_01/R2_02, m5, ms por fotograma dibujado)**: render 13,3-13,9 = copias EFB 2,9-4,4 (34 copias +
  29 borrados por fotograma en el desierto, ~110 us cada una) · estado por draw 2,5-3,1 · draws GL 2,4-3,8 (144-290
  draws, 13-18 us cada uno: Mesa 20.1) · emit 2,0-2,9 · regs 0,9 · tex 0,7 (casi todo hash_ram) + 0,5-1,0 de liberar
  comandos entre Render() y el swap. Driver/libc ~8 ms/fot en las tres; espera de GPU 0,4-0,6 %.
- **Cambios** (12 interruptores, todos apagados por defecto; tabla más abajo): LATEFREE, CLEARTEX, COPYFBO, LAZYNAT,
  COPYUNI, COPYST, LAZYEFB, PREHASH, TEXMEMO, XDUBO, COPYMRT (+ usar el `NWII_R_CULL="2"` que ya existía). HASHPF no da
  nada (no usar).
- **Consola** (S2 apagado, mismo binario): R2_04 → R2_05 (los 8 primeros, contiguas, con RPHASE): render ms/fot
  13,9 → 10,8 · 14,9 → 11,8 · 14,2 → 12,4. R2_07 → R2_06 (r2k, los 11, sin RPHASE, no contiguas): 14,5 → **10,9** ·
  15,7 → **11,8** · 15,4 → **12,7**. R2_08 (los 11 + RPHASE): 10,5 · 11,5 · 11,9. Imagen: todas IMAGEN OK, hojas
  mirada(s) limpias; FRAMECHECK 5/5 demos y title/map/walk/lvl11 idénticos con cada combinación.
- Pendiente en cola: R2_09 (r2k sin XDUBO), **R2_10 = r2m con los 12 (incluye COPYMRT) / R2_11 = r2m base**
  (contiguas; leer con `tools/ab.sh R2_11_r2m_base R2_10_r2m_todo12`).
- Candidato: `bin/r2_r2m.nro` (sha256 d860f98d…f198a; elf 69d99626…fc73) = m5 PGO + parche; parche limpio
  `r39r2_vs_m5.patch` (`cd <runtime m5> && patch -p2 < r39r2_vs_m5.patch`; aplica sin avisos sobre m5, r37v y r38c).
- Línea de debug.toml para encender los 11 medidos: `NWII_R2_LATEFREE = "1"`, `NWII_R2_CLEARTEX = "1"`,
  `NWII_R2_COPYFBO = "1"`, `NWII_R2_LAZYNAT = "1"`, `NWII_R2_COPYUNI = "1"`, `NWII_R2_COPYST = "1"`,
  `NWII_R2_LAZYEFB = "1"`, `NWII_R2_PREHASH = "1"`, `NWII_R2_TEXMEMO = "1"`, `NWII_R2_XDUBO = "1"`, `NWII_R_CULL = "2"`
  (+ `NWII_R2_COPYMRT = "1"` cuando R2_10 lo confirme).

## Bitácora
- 11:30 inicio. Leídos r36r/LEEME, r35m4/LEEME. Encolada **R2_01** = `bin/m5_pgo.nro` (copia exacta de
  r35m4/bin/m5_pgo.nro, sha256 dc0430ca…07fd) con `NWII_S2_OFF=1 NWII_TPROF=1 NWII_RPHASE=1` (desglose).
  Cola detrás de Z22 (que se quedó congelado en un iris a los ~5 min; ajeno).
- Datos previos (R07 de r36r = r11 con los 5 NWII_R_*, TPROF+RPHASE; `tools/rfases.py`), ms por fotograma dibujado:
  desierto total 13,0 = copy 4,0 · state 2,5 · emit 2,4 · gldraw 2,3 · regs 0,8 · tex 0,6;
  agua 14,2 = emit 3,6 · copy 3,4 · state 3,0 · gldraw 2,2 · regs 0,9 · tex 0,7;
  cielo 13,6 = gldraw 3,4 · emit 2,75 · state 2,76 · copy 2,74 · regs 0,9 · tex 0,7.
  → las copias EFB pesan 2,7-4 ms en las tres (en la fiesta del castillo solo 2,0).
- 11:42 **R2_01 hecha** (`bench_R2_01_m5_tprof_rphase_1115_1133`; 80 ventanas, 5 escenas, 1020/460 MHz, IMAGEN OK 1/38,
  S2 apagado). RPHASE (ms por fotograma dibujado; `tools/rfases.py`):
  | escena | total | copy | state | gldraw | emit | regs | tex | upload+frame | draws GX / GL | render ms/fot (PIPE) |
  |---|---|---|---|---|---|---|---|---|---|---|
  | desierto4j | 13,3 | **4,36** | 2,59 | 2,56 | 1,96 | 0,85 | 0,68 | 0,35 | 3 654 / 144 | 13,9 |
  | agua_pinguino | 13,9 | **3,55** | 3,06 | 2,39 | 2,85 | 0,87 | 0,74 | 0,38 | 6 564 / 144 | 15,1 |
  | cielo_paratroopas | 13,7 | 2,86 | 2,78 | **3,76** | 2,31 | 0,93 | 0,73 | 0,33 | 3 282 / 287 | 14,3 |
  PIPE − RPHASE (0,6 · 1,25 · 0,6 ms) = liberar los comandos del fotograma ENTRE Render() y el swap (WPROF render_freed:
  desierto ~0,47, agua ~1,0, cielo ~0,6 ms por fotograma, Z21).
  Muestras del hilo de render (`datos/rprof_R2_01_*.txt`, ms/fot = % × ms de pared por fotograma dibujado):
  driver/libc **~8 ms/fot en las tres** (desierto 42 %, agua 27,5 %, cielo 30,5 %), runtime 6,6 · 7,9 · 6,6 ms; espera
  sin fotograma 21 · 44 · 44 % (el render espera al juego; espera de GPU 0,4-0,6 %, de vsync/buffer ~1 % → dato para V:
  el hilo de render casi no espera al swap). Funciones nuestras: **hash_ram 6,1 % desierto (~1,15 ms/fot)**, 2,6 % agua
  (~0,75), 2,1 % cielo (~0,57); EmitPrimitive 6,1/5,7/5,1 %; RenderOnce 6,3/5,2/4,3 %; vector::_M_erase+delete (liberar)
  2,3/3,7/2,1 %; xf::Culled 2,1/2,1/1,2 %; SetupDrawState 2,3/1,6/2,4 %.
  Copias EFB por fotograma (PC, NWII_DRAWLOG, desierto): **34** = 19 de 32x32 (tiles animados, con borrado, EFB sin
  alfa) + 6 de 32x32 sin borrado (cosidas a una textura mayor) + 2 de 32x32 + 320x248 + 640x456 (mitad) + 640x456 (Z)
  + 228x320 ×2 + 320x228 + XFB; 29 borrados con tijera.
- 11:47 `tools/build_pgo_r2.sh <tag>`: NRO de Switch = objetos PGO de m5 (`r17pgo/nx/m5_pgo`, copia en la otra laptop
  en `r9/r39r2/nx/<tag>`) + gx.cpp/renderer_gl.cpp/texture_cache_gl.cpp de este runtime, con las órdenes exactas de
  ninja. Comprobado: con las fuentes de m5 sale **byte a byte** `m5_pgo.nro`/`.elf`.
- 11:50 binario **r2b** (`bin/r2_r2b.nro`, sha256 421a326d…12e2) = m5 + :
  `NWII_R2_LATEFREE=1` (gx.cpp ProcessGXFifo: los comandos del fotograma dibujado se liberan en la llamada SIGUIENTE,
  después del swap y solo si no espera otro fotograma; si espera, el paquete vuelve al trabajador, que ya sabía
  liberarlo), `NWII_R2_HASHPF=1` (texture_cache_gl.cpp hash_ram: prefetch 4 iteraciones por delante; mismas palabras,
  mismo valor) y, solo con RPHASE, la línea `[RPHASE2]` (subfases de las copias, borrados, y el primer draw GL tras
  una copia). Encoladas R2_02 (r2b base + RPHASE) y R2_03 (r2b + LATEFREE + HASHPF + RPHASE), S2 apagado.
- 12:00 **R2_02** (r2b base + RPHASE, `bench_R2_02_r2b_base_rphase_1115_1151`) — subfases de las copias (`[RPHASE2]`,
  us por fotograma): desierto 34 copias + 29 borrados: setup 727 · draw 1 085 · back 980 · done ~1 000 · clear 590;
  y el primer draw GL tras cada grupo de copias cuesta 44-47 us frente a 16-17 us los demás (14 por fotograma: +0,4 ms).
  agua (32 copias): 640 · 1 000 · 845 · 370-540 · 550; cielo (28): 510 · 780 · 690 · 300 · 510. Por copia ~110 us.
  «back» = la reducción a tamaño nativo (glBlitFramebuffer LINEAR) que se hace tras CADA copia escalada aunque solo
  se usa al coser (6 cosidas/fot en el desierto, que ya no la hacen) + volver a enlazar el EFB.
  «done» = hash_ram del destino (≈ 4 096 palabras por copia grande: 640x456, 320x248...) + 6 cosidos.
- 12:09 **R2_03** (r2b + LATEFREE + HASHPF, contigua a R2_02, ambas con RPHASE): render ms/fot (PIPE) desierto
  14,3 → **13,6**, agua 15,4 → **14,1**, cielo 15,1 → **13,9**, nubes 10,9 → 10,3, castillo 14,0 → 13,4. IMAGEN OK
  (1/37). LATEFREE da lo esperado; **HASHPF no da nada** (done igual: 950-1 050 us) → se deja apagado.
- Traza PC (`NWII_R2_COPYTRACE=<n>`, desierto): las 20 copias de tiles van **seguidas, sin ningún draw entre ellas**
  (los draws de los tiles van todos antes, con tijera 0,456-640,528); luego 6 parejas draw→copia (32x32, sin borrado,
  cosidas) y el resto.
- 12:15 binario **r2e** (`bin/r2_r2e.nro`, sha256 249d92c0…cf36f) = r2b + interruptores de copias:
  `NWII_R2_CLEARTEX=1` (borrado de solo color con máscara completa → glClearTexSubImage sobre la textura del EFB con
  los bytes del color; Mesa 20.1 hace un glClear con tijera dibujando un cuadrado y guardando/restaurando TODO el
  estado; comprobación propia `NWII_R2_CLEARTEX_CHECK=1`: 0 píxeles distintos del color en 32 400 borrados, PC),
  `NWII_R2_COPYFBO=1` (un FBO por textura destino/fuente de copia en vez de re-enganchar la textura a un FBO común
  en cada copia; borrar una textura de la caché borra antes su FBO), `NWII_R2_LAZYNAT=1` (la reducción nativa de una
  copia escalada se hace solo si alguien la cose, desde la misma textura escalada), `NWII_R2_COPYUNI=1` (uniformes del
  programa de copia solo si cambian), `NWII_R2_COPYST=1` (las copias/borrados/XFB pasan por la caché de estado GL en
  vez de vaciarla con GLInvalidate), `NWII_R2_LAZYEFB=1` (tras una copia el EFB vuelve como destino de dibujo solo
  cuando algo dibuja o borra con glClear: 20 copias seguidas = 1 cambio de FBO de vuelta).
  FRAMECHECK: en la pasada r8 de `NWII_R9_FRAMECHECK` estos interruptores se apagan (copias y borrados por el camino
  viejo), así que el FRAMECHECK compara camino nuevo contra viejo también en copias y borrados.
  r2c (LATEFREE+HASHPF+CLEARTEX+COPYFBO): `test/fc_r2c_todo.txt` **5/5 demos idénticas** (2 720 comprobaciones).
- 12:17 r2e FRAMECHECK con los 7 interruptores (LATEFREE, CLEARTEX, COPYFBO, LAZYNAT, COPYUNI, COPYST, LAZYEFB) + render
  lento simulado: `test/fc_r2e_todo.txt` **5/5 demos idénticas** (2 720), coalesce 0 discrepancias.
  r2f = r2e + `[TCSTAT3]` (tiempo de hash_ram por llamador, solo con RPHASE). Encoladas R2_04/R2_05 (detrás de 12
  trabajos de C y V).
- 12:25 Por qué cielo tiene ~290 draws GL (PC, `NWII_R2_KEYSTAT=1`): de ~240-340 cambios de clave por fotograma,
  casi todos cambian **programa y textura a la vez** (shader 217-321, tex 235-337): sprites con TEV distinto
  intercalados; agrupar exigiría reordenar (no exacto) o un ubershader (fuera de alcance). Desierto/agua ~110.
  Coste de un draw GL en la Switch ~13-18 us (validación de Mesa 20.1).
- Dato para V (R2_01, `tools/leafchain.py`): en el hilo de render, 990 de 1 751 muestras de svcSendSyncRequest son
  `bqDequeueBuffer`/`bqQueueBuffer` (swap); además `frame_tick` hace fflush+fsync del registro cada fotograma (~0,4 %).
- 12:30 `NWII_R2_PREHASH=1` (texture_cache_gl.cpp + gx.cpp): los hash de RAM que el fotograma siguiente necesitará
  (texeles de las texturas que usó el anterior y destinos de sus copias EFB) se calculan en el tiempo OCIOSO del hilo
  de render (después del swap, mientras no espera ningún fotograma; para en cuanto llega uno) y el fotograma los toma
  en vez de calcularlos en línea. Misma función, mismos rangos; la RAM se lee un poco antes (el hilo de render nunca
  la ha leído en un punto fijo de la línea de tiempo del juego). `NWII_R2_PREHASH_CHECK=1` recalcula en línea y cuenta
  diferencias. PC desierto: 74,5 de ~76 hash por fotograma salen del tiempo ocioso.
- 12:38 r2g FRAMECHECK con los 8 interruptores (+ PREHASH + PREHASH_CHECK): `test/fc_r2g_todo.txt` **5/5 idénticas**
  (2 820), coalesce 0 discrepancias, **prehash: 1 093 975 hash comparados con el cálculo en línea, 0 distintos**.
  R2_04/R2_05 de la cola re-apuntadas (misma posición) a r2g (`bin/r2_r2g.nro`, sha256 3d12f2db…ef98):
  R2_04 = base + RPHASE, R2_05 = los 8 + RPHASE. (Los .job/.nro r2f quedan en `.cola/retirados_r2/`.)
- 12:41 r2h = r2g + el prehash mira si espera un fotograma tras CADA hash (antes cada 8: hasta ~1 ms de retraso).
  `bin/r2_r2h.nro` (sha256 ce4866d5…14fb). Encoladas detrás, contiguas y SIN RPHASE (la medida de verdad):
  **R2_06 = r2h con los 8, R2_07 = r2h sin interruptores** (S2 apagado en ambas).
  Parche: `r39r2_vs_m5.patch` (`cd <runtime m5> && patch -p2 < r39r2_vs_m5.patch`; gx.cpp, renderer_gl.cpp,
  texture_cache_gl.cpp; comprobado que reproduce los tres archivos).

## Interruptores (todos APAGADOS por defecto; valor "1" para encender; ninguno usa "0")

| Variable | Archivo / función | Qué hace | Por qué es exacto |
|---|---|---|---|
| `NWII_R2_LATEFREE=1` | gx.cpp `ProcessGXFifo` | los comandos del fotograma dibujado se liberan en la llamada siguiente (tras el swap) y solo si no espera otro fotograma (si espera, los libera el trabajador al reusar el paquete, como sin NWII_RENDER_FREE) | solo cambia cuándo/quién libera; el paquete sigue siendo `s_rendering` hasta entonces (el trabajador no lo toca) |
| `NWII_R2_CLEARTEX=1` | renderer_gl.cpp `ClearEfbCopySource` | borrado de solo color con máscara completa (sin Z) = glClearTexSubImage de la textura del EFB sobre el mismo rectángulo, con el color en bytes RGBA | EFB RGBA8: mismos bytes que glClear; `NWII_R2_CLEARTEX_CHECK=1` lee el rectángulo: 0 píxeles distintos (ambos caminos) |
| `NWII_R2_COPYFBO=1` | texture_cache_gl.cpp `r2_tex_fbo`, renderer_gl.cpp `CopyEfbToTexture`, `stitch_one` | un FBO por textura de copia (destino, reducción nativa, fuente de cosido) en vez de re-enganchar la textura a un FBO común en cada copia | mismas operaciones sobre las mismas texturas; toda textura que borra la caché borra antes su FBO (`r2_del_tex`), así un nombre reusado nunca encuentra un FBO viejo |
| `NWII_R2_LAZYNAT=1` | texture_cache_gl.cpp `r2_native_defer/resolve`, renderer_gl.cpp | la reducción a tamaño nativo de una copia escalada solo se hace si un cosido la lee (antes: tras cada copia escalada, ~27/fot en el desierto, nunca leídas) | el mismo glBlitFramebuffer desde la misma textura escalada, que nada ha cambiado (una copia nueva al mismo par sustituye el registro; borrar la textura lo quita); tijera y máscara de color como en el original |
| `NWII_R2_COPYUNI=1` | renderer_gl.cpp `CopyEfbToTexture` | uniformes del programa de copia solo si cambian | el programa conserva sus uniformes |
| `NWII_R2_COPYST=1` | renderer_gl.cpp (copia, borrado, XFB, fin de 0x52) | las copias usan los setters con caché (CEnable, CViewport, CUseProgram…) y el borrado/XFB anotan lo que dejan; ya no se vacía la caché de estado GL tras cada copia | la caché refleja el estado real; donde no se sabe (unidad activa tocada por la caché de texturas, XFB nuevo, modos de depuración) se invalida |
| `NWII_R2_LAZYEFB=1` | renderer_gl.cpp (`R2EfbBack`) | tras una copia a textura, el EFB vuelve como destino de dibujo solo antes de un draw, un borrado con glClear o al final del fotograma (las ~20 copias seguidas de tiles: 1 vuelta en vez de 20) | el destino de dibujo solo importa a draws y glClear; la lectura (READ) vuelve al EFB enseguida |
| `NWII_R2_PREHASH=1` | texture_cache_gl.cpp (`r2_prehash_idle`, `end_frame`), gx.cpp | hash de RAM (texeles y destinos de copia que usó el fotograma anterior) en el tiempo ocioso del hilo de render; el fotograma los toma en vez de calcularlos | misma función y rangos; la RAM se lee algo antes (sin punto fijo respecto al juego en ningún caso). `NWII_R2_PREHASH_CHECK=1`: 0 diferencias en ~1,1 M comparaciones (PC) |
| `NWII_R2_HASHPF=1` | texture_cache_gl.cpp `hash_ram_` | prefetch en hash_ram | mismo valor; **no da nada en consola (R2_03): no usar** |
| `NWII_R2_TEXMEMO=1` | renderer_gl.cpp DrawPrimitive | búsquedas de textura recordadas (32 entradas) mientras no haya copia/borrado EFB, carga de TLUT, fotograma nuevo ni búsqueda que suba/cosa | mismas entradas → mismo resultado; apagado en la pasada r8 |
| `NWII_R2_XDUBO=1` | renderer_gl.cpp xf::Init/FlushBatch, tev_shader_gen.cpp (modo 3) | la base de dibujo del GPU XF va en un bloque uniforme de 16 B enlazado por draw (glBindBufferRange) en vez del uniforme uXDraw, que obligaba a Mesa a re-subir ~800 B de uniformes por draw | el sombreador lee el mismo entero; si no compila, vuelve al uniforme |
| `NWII_R2_COPYMRT=1` | renderer_gl.cpp R2QueueCopy/R2FlushCopies, texture_cache_gl.cpp r2_mrt_fbo | copias «simples» seguidas en un draw a hasta 8 adjuntos; borrados contiguos juntos | misma lectura + selección de canales por salida; sin destinos repetidos ni borrado sobre fuente posterior; necesita COPYFBO+COPYST+LAZYEFB+LAZYNAT+CLEARTEX |
| `NWII_R_CULL="2"` (ya existía en m5) | renderer_gl.cpp RenderOnce | el draw recortado tampoco prepara estado | FRAMECHECK 5/5 con los demás |

Solo medida/depuración: `[RPHASE2]` y `[TCSTAT3]` (con NWII_RPHASE), `NWII_R2_COPYTRACE=<n>`, `NWII_R2_KEYSTAT=1`.
En la pasada r8 de `NWII_R9_FRAMECHECK` se apagan CLEARTEX, COPYFBO, LAZYNAT, COPYST y LAZYEFB (g_r2_off_pass /
s_r8_pass): el FRAMECHECK compara así el camino nuevo de copias/borrados contra el viejo.

## Bitácora (sigue)
- 13:05 `test/checks.sh` (copia de r36r con el arnés propio) a 1,5789x con r2h y los 8 interruptores + PREHASH_CHECK +
  CLEARTEX_CHECK (`test/checks_r2h_todo.txt`): title/map/walk/lvl11 **220/220 FRAMECHECK idénticas en las cuatro**
  (lvl11 «pass1 differs 116»: el efecto conocido de copias leídas antes de rehacerse, ver r36r), prehash 2,56 M
  comparaciones 0 distintas, cleartex 545 k borrados 0 píxeles distintos del color.
- 12:45-13:05 Encargo del coordinador: régimen lento del trabajador en la fiesta (camino de descarte de PrepFrame).
  Empecé `NWII_R2_FASTDROP` (tramo descartado → pkt.pre, fotograma bueno por el camino rápido, Render() desde
  `start`), pero el coordinador lo **pasó al frente D** (`r9/r40d`) a las 13:05: retirado de mi runtime (las fuentes
  vuelven a ser las de r2h; la versión a medias queda en `datos/fastdrop_retirado/` solo como referencia, NO probada:
  en PC sin parar el trabajador ningún lote trae dos finales, hace falta `NWII_R2_TEST_WSTALL` de ese borrador).
  El parche `r39r2_vs_m5.patch` NO lo lleva.
- 13:10 `r39r2_vs_m5.patch` aplica limpio (solo desplazamientos) sobre `r37v/nWiiRuntime` (V) y `r38c/nWiiRuntime` (C).
  Encolada R2_09 = r2h con los 8 + `NWII_R_CULL="2"` (el draw recortado tampoco prepara estado; ya existía en m5;
  r36r lo descartó mirando solo el nº de draws GL, no el tiempo). FC en PC con los 8 + CULL=2 en marcha.
- 13:15 FC r2h + los 8 + CULL=2: `test/fc_r2h_todo_cull2.txt` 5/5 idénticas. Nuevo `NWII_R2_TEXMEMO=1` (renderer_gl.cpp
  DrawPrimitive): la búsqueda de textura (get_texture: mapas, hash de paleta…) se recuerda mientras no haya copia/
  borrado EFB (BP 0x52), carga de TLUT (0x64/0x65), cambio de fotograma ni una búsqueda que haya subido o cosido
  algo (gl_touches); tabla de 32 entradas por parámetros del mapa. Apagado en la pasada r8 del FRAMECHECK.
  OJO (aprendido): tras restaurar fuentes con rsync -a (mtime viejo) ninja NO recompila → `touch` antes de compilar.
- 13:35 r2j (= r2h + TEXMEMO; `bin/r2_r2j.nro` sha256 b2b66964…a5af0) FC con los 8 + TEXMEMO: `test/fc_r2j_todo.txt`
  **5/5 idénticas** (2 800); TEXMEMO recuerda 68-86 % de las búsquedas (cielo 86 %). Cola re-apuntada (mismas
  posiciones, `tools/repoint.py`): **R2_06 = r2j «todo10»** (los 8 + TEXMEMO + `NWII_R_CULL="2"`), **R2_07 = r2j
  base**, R2_08 = todo10 + TPROF + RPHASE (desglose final), **R2_09 = r2j «todo8»** (sin TEXMEMO ni CULL=2: dice
  cuánto aportan esos dos). Todas S2 apagado, sin RPHASE salvo R2_08.
- 13:40 checks.sh con r2j «todo10» (los 8 + TEXMEMO + CULL=2): `test/checks_r2j_todo10.txt` title/map/walk/lvl11
  **220/220 idénticas** las cuatro.
- 13:50 Nuevo `NWII_R2_XDUBO=1` (renderer_gl.cpp xf::Init/FlushBatch + tev_shader_gen.cpp, modo de base 3): en la
  Switch la base de dibujo del GPU XF va como uniforme uXDraw (gl_BaseInstance falla en Mesa 20.1: «draw-base tests
  FAIL»), y cambiar un uniforme del programa de vértices compartido hace que Mesa vuelva a subir TODO su bloque de
  uniformes por defecto (~800 B, luces incluidas) en cada draw GL. Con el interruptor la base va en un bloque
  uniforme de 16 B (`R2XDraw`, punto de enlace 1) en el anillo persistente (alineado a 256) y se enlaza por draw con
  glBindBufferRange; los uniformes por defecto no cambian. Si el sombreador no compila vuelve al uniforme. Solo
  actúa en modo uniforme (en PC hace falta `NWII_GPU_XF_BASEINST=0` y `NWII_SHADER_SSO=1` para probarlo como en la
  Switch). PC agua: FRAMECHECK 460/460 idénticas. `tools/build_pgo_r2.sh` compila ahora también tev_shader_gen.cpp.
  r2k = r2j + XDUBO (`bin/r2_r2k.nro`, sha256 d9999952…0b7e). FC 5 demos con los 11 en marcha.
- 14:05 r2k FC (los 8 + TEXMEMO + CULL=2 + XDUBO, SSO y modo uniforme como en la Switch): `test/fc_r2k_todo11.txt`
  **5/5 idénticas** (2 840). Cola re-apuntada a r2k (mismas posiciones): **R2_06 = «todo11»**, **R2_07 = base**,
  R2_08 = todo11 + TPROF + RPHASE, **R2_09 = «todo10»** (todo11 sin XDUBO: aísla XDUBO). (Arreglado a mano un `\;`
  que dejaba TEXMEMO/CULL fuera del DEBUG_EXTRA de R2_09; comprobado el entorno de las 6 con bash.)
  Parche: ahora 4 archivos (gx.cpp, renderer_gl.cpp, texture_cache_gl.cpp, tev_shader_gen.cpp).

## Consola: R2_04 (r2g base) → R2_05 (r2g con los 8), contiguas, ambas con RPHASE y S2 apagado
`bench_R2_04_r2g_base_rphase_1115_1426`, `bench_R2_05_r2g_todo_rphase_1115_1435`: las dos 80 ventanas, 5 escenas,
1020/460 MHz, IMAGEN OK (1/37 y 2/38 raras); hoja de R2_05 mirada: limpia (tiles animados del desierto/agua/nubes
bien; las negras son fundidos entre demos, igual que en R2_03).

| escena | render ms/fot (PIPE) base → 8 | RPHASE total | copy | tex | fps dibujados |
|---|---|---|---|---|---|
| desierto4j | 13,9 → **10,8** | 13,35 → 10,76 | 4,39 → 2,09 | 0,68 → 0,37 | 54,4 → 54,9 |
| agua_pinguino | 14,9 → **11,8** | 13,86 → 11,74 | 3,57 → 1,70 | 0,74 → 0,35 | 34,9 → 35,0 |
| cielo_paratroopas | 14,2 → **12,4** | 13,58 → 12,28 | 2,81 → 1,49 | 0,72 → 0,41 | 38,1 → 38,2 |
| nubes_plataformas | 10,3 → 8,1 | 9,79 → 8,11 | 3,06 → 1,48 | | 59,1 → 59,1 |
| castillo | 13,5 → 12,1 | 13,15 → 12,22 | 2,15 → 1,04 | | 56,3 → 55,7 |
(fps dibujados iguales: con S2 apagado y m5 el juego va en cadena con el render; el render ya no es el que limita
en estas corridas → con la sincronía de V el ahorro se vuelve fps.) Copias en el desierto (us/fot): setup 700 → 335,
draw 1 065 → 740, back 960 → 104, done 1 000 → 340, clear 580 → 280. hash_ram en línea: tex 360-480 → 30-140,
efb_done 130-520 → 50-160 us/fot (PREHASH usa ~98 % de sus hash).
- 14:50 Nuevo `NWII_R2_COPYMRT=1` (renderer_gl.cpp `R2QueueCopy`/`R2FlushCopies` + texture_cache_gl.cpp
  `r2_mrt_fbo`; necesita COPYFBO, COPYST, LAZYEFB, LAZYNAT y CLEARTEX): una copia «simple» (escalada, no mitad, sin
  intensidad, no cosida, borrado de solo color o ninguno) se encola; la cola se hace antes del siguiente draw, de
  cualquier otra copia/borrado o al final del fotograma: hasta 8 copias del mismo tamaño y formato en UN draw a 8
  adjuntos de color (cada salida es la misma lectura + selección de canales del sombreador de una copia: sin
  aritmética), y luego, en orden, efb_copy_done y su borrado (glClearTexSubImage) de cada una. Sin destinos repetidos
  en la cola y sin borrado encolado sobre la fuente de una copia posterior (si no, se hace la cola antes). En la
  pasada r8 del FRAMECHECK va apagado → el FRAMECHECK compara MRT contra el camino viejo. PC desierto: 380/380
  idénticas. r2l = r2k + COPYMRT (`bin/r2_r2l.nro`). FC 5 demos + checks.sh con los 12 en marcha.
- 15:00 r2l FC con los 12 (SSO + modo uniforme): `test/fc_r2l_todo12.txt` **5/5 idénticas** (2 720), coalesce 0.
  Encoladas **R2_10 = r2l «todo12»** y **R2_11 = r2l base** (`bin/r2_r2l.nro`, sha256 f51037e2…87df), contiguas,
  sin RPHASE, S2 apagado (detrás de D04/D05, R2_07-09, V07/V08 y M01-M03). OJO: D04/D05 se colaron entre R2_06
  (todo11) y R2_07 (base): ese par ya no es contiguo.
- R2_06 (r2k todo11) hoja mirada: limpia (también las escenas 3D del castillo).
- 15:05 checks.sh r2l con los 12: `test/checks_r2l_todo12.txt` title/map/walk/lvl11 **220/220 idénticas**.
  r2m = r2l + en la cola MRT los borrados contiguos (mismas filas, mismo color, x seguidas: las celdas de 32x32 de los
  tiles) se juntan en un glClearTexSubImage (mismos píxeles, mismos bytes). `bin/r2_r2m.nro` (sha256 d860f98d…f198a).
  FC + checks de r2m en marcha; si pasan, R2_10/R2_11 se re-apuntan a r2m (aún no han empezado).
- 15:15 r2m FC con los 12: `test/fc_r2m_todo12.txt` **5/5 idénticas** (2 780). R2_10/R2_11 re-apuntadas a r2m
  (**R2_10 = r2m todo12, R2_11 = r2m base**).
- 15:19 **R2_06 (r2k todo11) / R2_07 (r2k base)**, sin RPHASE, mismo binario, NO contiguas (D04/D05 en medio):
  render ms/fot base → todo11: desierto 14,5 → **10,9**, agua 15,7 → **11,8**, cielo 15,4 → **12,7**, nubes 10,9 → 8,2,
  castillo 14,2 → 11,8. fps dibujados ≈ iguales (53-55 · 35 · 37 · 58-59 · 55-56; juego en cadena con m5 y S2
  apagado). Ambas IMAGEN OK (3/38, 1/38), 80 ventanas, 1020/460, XDUBO activo en consola (alineación 256).
  OJO ruido: R2_07 (base sin RPHASE) sale 0,6-1,2 ms PEOR que R2_04 (base con RPHASE, 1 h antes): la variación entre
  corridas no contiguas es de ese orden; las diferencias pequeñas (TEXMEMO, CULL=2, XDUBO) solo se pueden juzgar con
  pares contiguos (R2_09 contra R2_06/R2_08).
- 15:30 checks.sh r2m con los 12: `test/checks_r2m_todo12.txt` title/map/walk/lvl11 **220/220 idénticas**.
- 15:28 **R2_08** (r2k todo11 + TPROF + RPHASE; `bench_R2_08_r2k_todo11_tprof_1115_1518`), IMAGEN OK 2/38, 80 ventanas.
  Desglose DESPUÉS (ms/fot dibujado, RPHASE; antes = R2_01/R2_02):
  | escena | total | copy | state | gldraw | emit | regs | tex | upload+frame | PIPE |
  |---|---|---|---|---|---|---|---|---|---|
  | desierto4j | 10,50 (13,3) | 2,01 (4,36) | 2,46 | 2,46 | 1,92 | 0,91 | 0,33 (0,68) | 0,41 | 10,5 (13,9-14,3) |
  | agua_pinguino | 11,50 (13,9) | 1,65 (3,55) | 2,93 | 2,34 | 2,85 | 0,97 | 0,31 (0,74) | 0,44 | 11,5 (15,1-15,4) |
  | cielo_paratroopas | 11,80 (13,7) | 1,40 (2,86) | 2,72 | 3,59 (3,76) | 2,27 | 1,03 | 0,35 (0,73) | 0,43 | 11,9 (14,3-15,1) |
  Contra R2_05 (los 8, también con RPHASE, 50 min antes): PIPE 10,8 → 10,5 · 11,8 → 11,5 · 12,4 → 11,9 (TEXMEMO +
  CULL=2 + XDUBO; no contiguas). Muestras (`datos/rprof_R2_08_*.txt`): hash_ram y liberar ya caen fuera de Render()
  (tiempo ocioso: PREHASH/LATEFREE); lo que queda en el render son EmitPrimitive/RenderOnce (~1,5 ms cada uno), el
  driver (~6 ms) y la espera.
