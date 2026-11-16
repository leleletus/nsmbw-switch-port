# r36r: frente R — tiempo del HILO DE RENDER por fotograma (castillo) — 15-nov-2026

Runtime: copia (--reflink) de `r9/r31t/nWiiRuntime` (= runtime de m3). Código generado: G6c3 (`CODE=c3`).
Compilar: `CODE=c3 JOBS=4 ~/nsmbw-port/scripts/lim tools/build_t.sh <nombre> both` (solo runtime + enlace;
escribe solo en r9/r36r). Cambios protegidos con `NWII_R_*` (apagado = comportamiento de m3).

## Resumen (en curso; números finales en «Resultado final» al final)

- **Castillo = dos partes**: la fiesta (~25 s; 12 200 draws GX/fot, ~160 draws GL, se descartan ~50 fot/2 s) y el
  resto (6 600-11 000 draws, sin descartes). Antes: fiesta 24,8 ms de render/fot y 32 fps dibujados (juego a 57-58);
  resto 14,8 ms / 57 fps. **No es la GPU** (espera de fence 0,3 %): es CPU del render (runtime 54 %, Mesa 32 %) y,
  en la fiesta, también del trabajador (19-20 ms/fot preparado).
- **Cambios** (gx.cpp y renderer_gl.cpp, cada uno con su interruptor, apagados = m3):
  `NWII_R_COALESCE=1`, `NWII_R_PREFIX=1`, `NWII_R_FREECHUNK=1` (trabajador/descartes), `NWII_R_CULL=1` (no emitir
  draws enteros fuera del volumen de recorte), `NWII_R_TGMEMO=1` (reutilizar el registro de texgen). Detalle en la
  bitácora.
- **Final (r14, dos pares contiguos R08/R09 y R10/R11)**: fiesta 22,7-22,9 → **16,9 ms**, 34 → **53-54 fps
  dibujados**; castillo entero 16,7-16,8 → 13,4 ms, 46-47 → 55,9 fps; resto 14,0 → 10,9; agua 14,9-15,0 → 14,4;
  desierto 13,0 → 12,8; cielo y nubes iguales. Imagen limpia en las cuatro (comprobador + hojas miradas) y
  FRAMECHECK idéntico en PC (5 demos + title/map/walk/lvl11). Tablas en «Resultado final».

## Bitácora
- 00:30 inicio. Leído r31t/LEEME.md. Datos ya existentes: `bench_S11_s2g_off_tprof` (s2g con S2 apagado,
  NWII_TPROF=1) tiene muestras de los hilos de render/trabajador en el castillo (~6 s, la demo empieza a los
  391 s de 400). En el log de G02 (m3n) el castillo hace **~12 200 draws GX por fotograma** (las demás escenas
  2 500-3 900) y ~160 draws GL; render ~22 ms/fot; retracts ~35 y dropped ~50 por ventana.
- 00:36 encolado R01 = m3n (`bin/nsmbw_m3n.nro`, copia de r31t) con NWII_TPROF=1 + NWII_RPHASE=1, 470 s
  (castillo ~80 s) para el desglose.
- 00:44 Hallazgo 1 (worker, consola X01/G02): en el castillo el trabajador hace `split` 10-12 ms y parse 10-12 ms por
  fotograma preparado (WPROF), y el render hace ~27-29 ms por fotograma dibujado en la fiesta (PIPE draw avg).
  Cada fotograma descartado deja TODAS sus escrituras de registro (sin los draws) delante del siguiente: el
  trabajador las mueve una a una y el render las reaplica. Cambio 1 = `NWII_R_COALESCE=1` (gx.cpp, namespace
  rcoal): en la parte descartada se quitan las escrituras muertas (BP reescrito después en el mismo tramo;
  XF con todas sus palabras reescritas), con 0x52/0x64/0x65 como barreras. Comprobación exacta
  `NWII_R_COALESCE_CHECK=1` (simula el estado BP/TEV/XF completo y compactado en cada barrera y al final).
  PC castillo con render lento simulado (`NWII_R11_SLOW_RENDER_US=20000`): por parte descartada ~20k comandos
  (~10k draws), 8k escrituras muertas quitadas, quedan ~1,5-2k; **0 discrepancias en 2 200 listas**
  (`r9/render/pc/runs/r36_coal_chk`). Binario r1 (`bin/nsmbw_r1.nro`, elf `bin/nsmbw_r1_R02R03.elf`).
  Encolados R02 (r1 apagado) y R03 (r1 + NWII_R_COALESCE=1), 470 s, contiguos.
- 00:55 R01 hecha (`bench_R01_m3n_tprof_rphase_1115_0044`, imagen OK 2/45 raras). Castillo completo (59,5 s): fps dib
  44,5, juego 58,0, render 17,6 ms/fot (con RPHASE). La demo tiene dos partes: la **fiesta** (~25 s, 12 200 draws/fot,
  se descartan ~50 fot/2 s) y el resto (6 600-11 000 draws, sin descartes, 58-60 fps dibujados).
  **Desglose del render en la fiesta (RPHASE, us por fotograma dibujado; total 21,7-23,9 ms):**
  emit 8 700 (xf::Emit, 0,71 us/draw) · state 3 500-4 400 (resto por draw en DrawPrimitive/RenderOnce) ·
  gldraw 3 400-4 000 (160 draws GL, ~21 us cada uno: validación de Mesa) · copy 2 300-2 600 (27 copias EFB) ·
  regs 2 000 (sin descartes: 730 → ~1 300 son las escrituras de los fotogramas descartados) · tex 1 100-1 500 ·
  upload 500 · frame 270-390. PIPE draw 23-25 ms (incluye liberar los comandos: 2,3-2,8 ms cada vez que el render
  libera, ~40 % de los fotogramas). Espera a la GPU: 0,3 %. **No es GPU: es CPU del render y del trabajador.**
  Hilo de render (muestras tprof, castillo entero): runtime 53,7 %, driver/libc 32,4 %, sin fotograma 12,6 %.
  EmitPrimitive 23,9 % (las muestras caen en cargas de matrices de g_state y de las paletas s_pal_*: fallos de caché).
  **En la fiesta el que limita es el trabajador**: loop 30,5 ms vs draw 23-25 → el render espera ~6 ms por fotograma.
  Trabajador (WPROF): ocupado 17-23 ms por fotograma preparado: parse 7-11, **split 6,4-10**, decode 1,5-2,4, clear 1-1,6.
  `split` es mover comandos: el fotograma retirado (retract) va por el camino lento (cada comando movido uno a uno,
  ~40k, y todos sus draws preparados otra vez); además la cola del siguiente fotograma ya parseada (~27k comandos).
- 01:00 Cambio 2 = `NWII_R_FREECHUNK=1` (gx.cpp, ProcessGXFifo): el render libera en trozos de 2048 comandos y para en
  cuanto el trabajador entrega otro fotograma (el resto lo libera el trabajador). Cambio 3 = `NWII_R_PREFIX=1`
  (gx.cpp + renderer_gl.cpp): lo que queda de un paquete retirado va a `pkt.pre` (se dibuja antes, mismo orden) y el
  fotograma nuevo toma el camino rápido (sin mover comandos uno a uno, con las entradas del parser). Binario r3.
- 01:12 PC (arnés propio `r9/r36r/pc`, copia de render/pc: el de render estaba ocupado por checks de S2) r3 con los
  tres interruptores + `NWII_R_COALESCE_CHECK=1` y render lento simulado: sin fallos, 0 discrepancias en 431 listas
  (`pc/runs/r36_r3_on`). OJO: en zsh `env $X` no parte la variable (la primera prueba "on" solo tenía COALESCE).
- 01:15 R02/R03 de la cola cambiados (misma posición) a r3: **R02 = r3 sin interruptores (base), R03 = r3 +
  COALESCE + FREECHUNK + PREFIX**. elf: `bin/nsmbw_r3_R02R03.elf`.
- 01:20 **Cambio 4 = `NWII_R_CULL=1`** (renderer_gl.cpp, xf::Culled/Emit + caja de cada entrada ecache en
  GX_PrepDrawForRender): un draw de lista de visualización con una sola matriz de posición cuya caja (posiciones
  crudas) transformada por la matriz y la proyección del shader queda entera fuera de un plano x=±w / y=±w no se
  emite (GL lo recortaría entero; depth clamp solo quita near/far). Prueba afín por esquinas en doble con margen
  relativo 1e-3. Medida en PC (DS: en el castillo solo 14-58 % de los vértices caen dentro del volumen).
  PC castillo a 1,5789x con `NWII_R9_FRAMECHECK=10` (cada 10.º fotograma se dibuja también por el camino r8 sin
  recorte y se compara píxel a píxel): **640 comprobaciones, 640 idénticas, 0 píxeles distintos**
  (`pc/runs/r36_cull_fc`). En la fiesta se recortan 4 200-6 400 de ~10 000 draws por fotograma (52-66 %).
  Binario r4. R02/R03 cambiados otra vez (misma posición): R02 = r4 base, R03 = r4 + los cuatro interruptores.
- 01:25 r5 = r4 con la prueba de recorte al principio de xf::Emit (antes de PrepTexGen; también vale para el camino
  CPU de texgen por color: misma matriz). PC FRAMECHECK castillo r5 con los 4 interruptores: 640/640 idénticas,
  coalesce 0 discrepancias; en la fiesta **los draws GL bajan de ~160 a 85-93** (lotes enteros recortados).
  `test/demos_fc.sh` (FRAMECHECK por demo a 1,5789x + comprobación de coalesce, render lento simulado):
  desierto 560/560 idénticas, agua 560/560 (se recortan hasta ~1 800 draws/fot). Encolados R04 (r5 todo) y
  R05 (r5 base), detrás de los trabajos de G7/S2. Patch: `r36r_vs_r31t.patch` (solo gx.cpp y renderer_gl.cpp;
  ambos archivos son idénticos en r33g7, r34s2 y r35m4: el parche aplica tal cual).
- 01:35 r6 = r5 + recorte también de los draws empaquetados por el trabajador (no listas: PackHdr lleva la caja).
  PC FRAMECHECK r6: agua 640/640 idénticas (recortados 1 160-2 584 draws/fot, de ellos 857-1 138 empaquetados),
  castillo 640/640. R04/R05 cambiados a r6 (todo / base). `test/fc_r4todo.txt`: FRAMECHECK por demo.
- 01:38 `test/fc_r4todo.txt` completo (r4, los 4 interruptores + render lento simulado 12 ms, 1,5789x): las cinco demos
  FRAMECHECK idénticas (desierto 560/560, agua 560/560, cielo 560/560, nubes 580/580, castillo 560/560) y la
  comprobación de coalesce 0 discrepancias (1 218 listas). «pass1 differs» de agua (83-146) sale igual con el
  recorte apagado (145, `pc/runs/r36_r6_fc_agua_off`): es el efecto conocido de copias EFB usadas antes de rehacerse.
- 01:40 R02 (r4 base) hecha: imagen OK (1/46 raras), hoja mirada: limpia. Castillo render 17,5 ms/fot (entero),
  fiesta 24,3 ms / 32,3 fps dib, resto 14,6 ms / 56,4 fps (`tools/fiesta.py`).
- 01:50 **R03 (r4 + los 4) contra R02 (r4 base), contiguas.** Imagen OK (2/47 raras, hoja mirada: limpia).
  Fiesta: fps dibujados **32,3 → 38,6**, loop 30,9 → 25,9 ms, **trabajador 18,8 → 10,0 ms/fot (split 7,5 → 0,3)**,
  pero render draw 24,3 → 24,0 (igual) y en las demás escenas el render **empeora**: desierto 13,2 → 14,9,
  agua 15,4 → 17,7, cielo 13,5 → 15,0, nubes 9,9 → 11,1 (Render() mediana +1,2 a +2,4 ms). Causa: la prueba de
  recorte r4-r6 (8 esquinas en doble, conversiones float→double) cuesta ~0,5 us por draw en el A57, tanto como
  el emit que ahorra. **r8**: prueba por plano con el mínimo de la caja (4 planos, float, salida temprana),
  prefetch de la caja con la entrada. Lección: COALESCE+PREFIX son los que dan los fps (trabajador).
- 02:00 r8 FRAMECHECK (`test/fc_r8todo.txt`): las 5 demos idénticas (2 840 comprobaciones), coalesce 0 discrepancias.
  r9 = r8 + recorte temprano en RenderOnce (draw con estado limpio y lote abierto que la prueba rechaza: ni
  DrawPrimitive). Cola: R04 (r8→r9 todo), R05 (base), R06 (r9 sin recorte: COALESCE+FREECHUNK+PREFIX),
  R07 (r9 todo + TPROF + RPHASE: desglose después). `tools/set_r0405.sh <rN>` reapunta R04/R05.
- 02:10 r9 FRAMECHECK (`test/fc_r9todo.txt`): las 5 demos idénticas (2 840), coalesce 0 discrepancias. R04/R05 → r9.
- 02:20 r10 = r9 + `NWII_R_CULL=2` (el draw recortado tampoco prepara estado: ni texturas ni clave de lote).
  FRAMECHECK 5 demos idénticas (`test/fc_r10c2.txt`), pero los draws GL por fotograma salen iguales que con =1
  (los draws recortados no separan lotes del mismo estado): no aporta; R04-R07 siguen con r9 y CULL=1.
- 02:22 r11 = r9/r10 + **`NWII_R_TGMEMO=1`** (renderer_gl.cpp, xf::Emit): el registro de texgen del último draw se
  reutiliza si la configuración de texgen y los índices de matriz de textura son iguales y ninguna fila de
  matriz (pos/post) que leyó se ha escrito desde entonces (RenderOnce marca filas escritas, un bit por fila de 4
  floats; validez por fotograma con s_epoch). Evita las cargas/comparaciones de matrices y paletas (~40 % del
  emit). FRAMECHECK en marcha (`test/fc_r11todo.txt`).
- 02:31 r11 FRAMECHECK (`test/fc_r11todo.txt`, los 5 interruptores + render lento): 5 demos idénticas (2 860),
  coalesce 0 discrepancias. Aciertos de TGMEMO: desierto 75-87 %, castillo 81-87 % de los draws emitidos.
  Cola cambiada (`tools/set_r0407_r11.sh`, mismas posiciones): **R04 = r11 todo (COALESCE+FREECHUNK+PREFIX+CULL=1+
  TGMEMO), R05 = r11 base, R06 = r11 todo sin TGMEMO, R07 = r11 todo + TPROF + RPHASE** (desglose después).
- 02:44 **R04 (r11 todo)**: imagen OK (1/45 raras; hoja mirada: limpia; la 350 negra es el fundido entre demos).
  Contra R02 (base, 1 h antes; el control contiguo R05 viene ahora): **castillo render 17,5 → 14,2 ms/fot,
  fps dib 46,2 → 54,4; fiesta render 24,3 → 18,0 ms, fps dib 32,3 → 50,0, trabajador 18,8 → 9,9 ms/fot;
  resto del castillo 14,6 → 11,5 ms**. Otras escenas: desierto 13,2 → 13,4, agua 15,4 → 15,1, cielo 13,5 → 14,1,
  nubes 9,9 → 10,3 (a confirmar con R05). Fallo visto: los draws que pasan el recorte temprano de RenderOnce se
  volvían a probar en xf::Emit (dos pruebas por draw dibujado). **r12** lo evita (s_cull_done). FC en marcha.
- 02:54 **R05 (r11 base, contigua a R04)**: imagen OK (2/46; hoja limpia). **R05 → R04 (render ms/fot / fps dib)**:
  castillo 17,9 → **14,2** / 45,6 → **54,4** (fiesta 24,8 → **18,0** / 32,1 → **50,0**; trabajador 20,0 → 9,9 ms;
  resto 14,8 → 11,5 / 57,0 → 57,7); desierto 13,4 → 13,4; agua 15,6 → **15,1**; cielo 13,7 → 14,1 (+0,4);
  nubes 10,0 → 10,3 (+0,3). r12 (sin doble prueba) FRAMECHECK 5/5 idénticas (`test/fc_r12todo.txt`);
  R08/R09 → r12 base / r12 todo (`tools/set_r0809.sh r12`).
- 02:56 r13 = r12 + dos cambios sin efecto en el resultado (no llevan interruptor; afectan igual a base y todo):
  (1) los `static` con guarda de DrawPrimitive (NWII_DRAWLOG*) y del XF de RenderOnce (NWII_XFLOG) pasan a
  globales leídos una vez en InitFlags (eran 4-5 `ldar` por draw: ~26 % de las muestras de DrawPrimitive en R01);
  (2) Same48 con cargas vectoriales de 16 bytes (antes copias a la pila). FC en marcha (`test/fc_r13todo.txt`).
- 03:04 **R06 (r11 todo sin TGMEMO)**: imagen OK (1/47). R05 → R06 → R04 (render ms/fot): fiesta 24,8 → 18,5 → 18,0;
  resto castillo 14,8 → 11,7 → 11,5; desierto 13,4 → 13,6 → 13,4; agua 15,6 → 15,4 → 15,1; cielo 13,7 → 14,1 → 14,1;
  nubes 10,0 → 10,3 → 10,3. TGMEMO aporta −0,2 a −0,5 ms; el grueso (fiesta −6,3 ms) es COALESCE+PREFIX+CULL.
  r13 FRAMECHECK 5/5 idénticas (`test/fc_r13todo.txt`). R08/R09 → **r13 base / r13 todo** (binario candidato final).
- 03:14 **R07 (r11 todo + TPROF + RPHASE)**, imagen OK (2/46). Desglose del render **después** (fiesta, ms/fot dibujado;
  antes R01 entre paréntesis): total 17,6 (23,2) = regs 1,2 (2,0) · state 4,8 (4,0; incluye ahora la prueba de recorte
  temprana) · tex 1,2 (1,5) · emit 5,2 (8,7) · upload 0,3 (0,5) · gldraw 2,7 (3,6) · copy 2,0 (2,4) · frame 0,2 (0,4).
  Resto del castillo: 11,1 (14,2). Muestras del render en el castillo: `xf::Culled` 7,5 % (r11 la llamaba dos veces
  por draw dibujado: r12 lo quita), EmitPrimitive 11,6 % (antes 23,9 %), RenderOnce 10,4 %. Trabajador: ocioso
  60,7 % (antes 12 %): ya no limita. Hilo de render ocioso por falta de fotograma 15 %.
- 03:16 r14 = r13 + prueba de recorte ortográfica (la única que sale en estas demos: DS «persp draws=0») con centro y
  medio tamaño de la caja: X ± R por eje (el mismo mínimo exacto; ~1/3 de las operaciones). FC en marcha
  (`test/fc_r14todo.txt`). Encolados R10 (r14 todo) y R11 (r14 base) tras los de S2 (Z01/Z02).
- 03:25 checks.sh (copia en `test/checks.sh`, arnés propio) a 1,5789x con r13: base y todo (5 interruptores +
  COALESCE_CHECK) → title/map/walk/lvl11 **220/220 FRAMECHECK idénticas en las 8 corridas**, diff_lines=0
  (`test/checks_r13_resumen.txt`). cmp_png base↔todo: map 0,1-0,7 %, lvl11 2-3 %, walk 54 %, title 99,6 % en una
  captura: miradas lado a lado (scratchpad), son el mismo juego en otro instante (Mario en otra casilla del mapa, la
  demo del título en otro punto): el arnés no es determinista (ya lo dice r31t §6), no hay imagen rota ni negra
  (negro ref = prueba en todas).
- 03:32 r14 FRAMECHECK 5/5 idénticas (`test/fc_r14todo.txt`, mismos recortes que r13). R08 ya corría (r13 base: el
  camino base es el mismo código que r14); **R09 → r14 todo**. Cola: R08 r13 base, R09 r14 todo, (Z01/Z02 de S2),
  R10 r14 todo, R11 r14 base, (Y05/Y06 de G7).

## Interruptores (todos apagados por defecto = m3; ponerlos en debug.toml o en el entorno)

| Variable | Archivo / función | Qué hace | Exacto porque |
|---|---|---|---|
| `NWII_R_COALESCE=1` | gx.cpp `rcoal::` (RetractToPrefix y camino lento de PrepFrame) | de la parte descartada de un fotograma quita las escrituras BP/XF que se reescriben después en el mismo tramo (barreras: BP 0x52, 0x64, 0x65) | el estado BP/TEV/XF en cada barrera y al final es el mismo; `NWII_R_COALESCE_CHECK=1` lo simula y cuenta discrepancias (0 en ~3 000 listas) |
| `NWII_R_PREFIX=1` | gx.cpp (FramePacket::pre, PrepFrame) + renderer_gl.cpp (RenderOnce, `g_render_prefix`) | lo que queda de un paquete retirado va a `pre` y se dibuja antes; el fotograma nuevo va por el camino rápido (sin mover ~40k comandos ni preparar otra vez todos sus draws) | mismos comandos en el mismo orden |
| `NWII_R_FREECHUNK=1` | gx.cpp ProcessGXFifo | el render libera los comandos en trozos de 2048 y para si el trabajador entrega otro fotograma | solo cambia quién libera |
| `NWII_R_CULL=1` | renderer_gl.cpp `xf::Culled`, xf::Emit, RenderOnce; caja en `ecache::Entry`/`PackHdr` | no emite un draw (lista o empaquetado, una matriz de posición) cuya caja queda entera fuera de x=±w o y=±w | GL lo recortaría entero (depth clamp solo quita near/far); mínimo exacto de una función afín sobre la caja + margen 1e-3; FRAMECHECK idéntico |
| `NWII_R_CULL=2` | idem | además el draw recortado no prepara estado | probado, sin ganancia (mismos draws GL) |
| `NWII_R_TGMEMO=1` | renderer_gl.cpp xf::Emit + RenderOnce (bits de filas escritas) | reutiliza el registro de texgen del draw anterior si config/índices son iguales y no se escribió ninguna fila de matriz que leyó | mismas entradas → mismo registro y mismas unidades de paleta (por fotograma) |

Sin interruptor (r13+, mismo resultado, afectan a base y todo): globales leídos una vez en vez de `static` con guarda en
DrawPrimitive/RenderOnce, Same48 vectorial. Parche completo: `r36r_vs_r31t.patch` (solo `src/hle/gx.cpp` y
`src/hle/gx/renderer_gl.cpp`; idénticos hoy en r31t, r33g7, r34s2 y r35m4).
Pruebas en PC: `test/demos_fc.sh <bin_input_test> <tag> 100 VAR=1 ...` (FRAMECHECK por demo, 1,5789x),
`test/checks.sh` (copia de render/test con T=r36r), `tools/prof_castillo.sh` (perf PC). Consola: `tools/fiesta.py`,
`tools/comparar.sh R05 R04`, `tools/rprof.py <bench> <nm> castillo 0|4 [N] [función]`.
- 03:42 **R08 (r13 base: sin interruptores, con los 2 cambios de código sin efecto)**: imagen OK (2/46, hoja limpia).
  Ya mejora a R05 (r11 base): fiesta 24,8 → 22,9 ms (34,1 fps dib), resto 14,8 → 13,9, desierto 13,4 → 13,0,
  agua 15,6 → 15,0, cielo 13,7 → 13,5, nubes 10,0 → 9,8 (los `ldar` de los static con guarda por draw pesaban).
- 03:52 **R09 (r14 todo, contigua a R08 r13 base)**: imagen OK (1/46; hoja mirada: limpia, la 350 es el iris al título).
  **R08 → R09**: fiesta **22,9 → 16,9 ms, 34,1 → 53,0 fps dibujados** (trabajador 18,6 → 9,8); castillo entero
  16,8 → 13,4 ms, 46,8 → 55,9 fps; resto 13,9 → 10,8; desierto 13,0 → 12,8; agua 15,0 → 14,4; cielo 13,5 → 13,5;
  nubes 9,8 → 9,8. Ya no empeora ninguna escena. Pendiente: R10 (r14 todo) / R11 (r14 base) como repetición.

## Resultado final (binario r14: `bin/nsmbw_r14.nro`, elf `bin/nsmbw_r14.elf`; PC `bin/NSMBW_r14[_input_test]`)

Encender: `NWII_R_COALESCE=1 NWII_R_PREFIX=1 NWII_R_FREECHUNK=1 NWII_R_CULL=1 NWII_R_TGMEMO=1`.

Desglose del render en la fiesta del castillo (RPHASE, ms por fotograma dibujado):

| fase | antes (R01, m3n) | después (R07, r11 todo) |
|---|---|---|
| emit (xf::Emit) | 8,7 | 5,2 |
| state (por draw: RenderOnce/DrawPrimitive; después incluye la prueba de recorte) | 4,0 | 4,8 |
| gldraw (draws GL; ~160 → ~110) | 3,6 | 2,7 |
| copy (20-27 copias EFB) | 2,4 | 2,0 |
| regs (escrituras BP/XF; antes con las de los fotogramas descartados) | 2,0 | 1,2 |
| tex | 1,5 | 1,2 |
| upload + frame | 0,9 | 0,5 |
| **total Render()** | **23,2** | **17,6** |

Consola, corridas contiguas (lista fija, 470 s, `por_escena_real.py` + `fiesta.py`), render ms/fot y fps dibujados:

| escena | R08 r13 base | R09 r14 todo | R10 r14 todo | R11 r14 base |
|---|---|---|---|---|
| castillo (entero) | 16,8 ms / 46,8 | **13,4 / 55,9** | **13,4 / 55,9** | 16,7 / 46,1 |
| — fiesta | 22,9 / 34,1 | **16,9 / 53,0** | **16,9 / 53,7** | 22,7 / 34,3 |
| — resto | 13,9 / 57,9 | 10,8 / 58,1 | 10,9 / 57,7 | 14,0 / 57,0 |
| desierto4j | 13,0 / 38,2 | 12,8 / 38,1 | 12,8 / 38,0 | 13,0 / 38,0 |
| agua_pinguino | 15,0 / 32,1 | 14,4 / 32,2 | 14,4 / 32,0 | 14,9 / 31,5 |
| cielo_paratroopas | 13,5 / 31,0 | 13,5 / 30,9 | 13,5 / 30,9 | 13,4 / 30,9 |
| nubes_plataformas | 9,8 / 36,2 | 9,8 / 35,8 | 9,7 / 36,2 | 9,8 / 36,2 |

Imagen: las cuatro corridas «IMAGEN OK» (1-2 capturas raras de ~46, como las corridas de referencia) y hojas
`capturas.jpg` miradas: limpias (las negras son fundidos/iris entre demos). Trabajador en la fiesta: 17-18,6 → 9,8 ms
por fotograma preparado. fps del juego sin cambios (el hilo del juego no se toca). Contra la m3n original del día
(R05 r11 base) la fiesta pasa de 24,8 ms / 32,1 fps a 16,9 ms / 53-54 fps.

### Riesgos
- `NWII_R_CULL`: exacto por construcción (GL recorta entero un triángulo con todos los vértices fuera de un plano
  x/y; caja = cota de los vértices; margen 1e-3 frente a errores de ~1e-7); probado con FRAMECHECK (camino R9/GPU-XF
  con recorte contra el camino r8 sin recorte, píxel a píxel) en las 5 demos y en title/map/walk/lvl11 a 1,5789x:
  0 píxeles distintos. Si algún día se emulan los contadores/caja de PE (pixel count, bounding box) habría que
  apagarlo. Las demos solo usan proyección ortográfica; la rama de perspectiva está escrita igual pero solo la
  cubren las pruebas de PC si aparecen draws en perspectiva (no en estas escenas).
- `NWII_R_COALESCE`: depende de que las únicas escrituras BP con efecto inmediato en el render sean 0x52/0x64/0x65
  (revisado en ApplyBPRegister y RenderOnce); la comprobación `NWII_R_COALESCE_CHECK=1` sale 0 en ~4 000 listas.
- `NWII_R_PREFIX`: los modos de depuración que leen `pkt.frame` (FRAMESIG, FRAMELOG) no ven el prefijo.
- `NWII_R_FREECHUNK`: si el render para a medias, `p->draws` queda apuntando a comandos ya destruidos hasta que el
  trabajador recicla el paquete (nadie los lee entretanto).
- `NWII_R_TGMEMO`: cualquier escritura futura a g_state.posMatrices/postMatrices fuera de RenderOnce tendría que
  marcar `xf::s_rows_*_w` (hoy no hay ninguna: comprobado con grep).
- El arnés de PC no es determinista entre corridas (cmp_png base↔todo difiere por instante, no por imagen); la
  identidad se apoya en FRAMECHECK dentro de la misma corrida y en las comprobaciones propias.

### Qué queda
- Fiesta a 16,9 ms: lo que más pesa ahora es «state» (bucle por draw de RenderOnce + prueba de recorte, ~4-5 ms;
  `xf::Culled` sigue siendo ~5-7 % del hilo por los fallos de caché de la matriz/caja), emit de los draws que se
  dibujan (~5 ms), driver GL (gldraw ~2,7 ms, ~110 draws) y copias EFB (~2 ms: 20-28 copias de 32x32 de tiles
  animados con FBO/clear cada una; agrupar copias seguidas sin draws entre medias es el siguiente candidato, pero
  necesita una prueba de imagen propia porque FRAMECHECK comparte el código de copias).
- En agua/desierto el render ya está por debajo del hilo del juego; el recorte ahorra 0,2-0,6 ms ahí.
- Llevar el parche a la compilación fusionada/PGO: `r36r_vs_r31t.patch` aplica tal cual sobre r33g7/r34s2/r35m4
  (mismos gx.cpp y renderer_gl.cpp); poner los 5 interruptores por defecto solo tras una corrida del juego real.

