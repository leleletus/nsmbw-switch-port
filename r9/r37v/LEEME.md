# r37v: frente V — acoplamiento juego → trabajador → render → swap → retrazo (15-nov-2026)

Sin commits. Runtime propio `nWiiRuntime/` = copia de `r9/r35m4/nWiiRuntime_m5` (m5) + lo de abajo. Solo lectura de
r35m4, r36r, r31t. Código de juego: objetos m4 de r35m4 (PC) / objetos PGO de m4 en la otra laptop (Switch).
Interruptores nuevos (todos apagados por defecto, valores distintos de "0"):

| Variable | Qué hace |
|---|---|
| `NWII_V_TL=<ruta>` | diagnóstico: línea de tiempo binaria (retrazo, interrupciones, ocioso del juego, XFB, trabajador, render, swap) |
| `NWII_V_WAKE=1` | el bucle ocioso del invitado no duerme en la vuelta en que se le entregó una interrupción |
| `NWII_V_LATE=<ms>` | retrazo VI elástico: si llega con el juego ocupado se retiene hasta que el juego espera (máx. <ms>) |
| `NWII_V_SLOWTEST=<base>,<azar>` | SOLO PC: el hilo del juego gira base+azar µs tras cada copia XFB (simula la Switch) |

## Bitácora
- 11:30 inicio. Leídos r35m4 §6-§13, r36r, r31t §1-§3. Z21 (m5, agua): `[PIPE]` swap 0,2-0,3 ms (el swap NO
  bloquea por vsync), render draw 15-16 ms, trabajador fin→listo 10-11 ms, backlog waits 0 (el juego nunca espera
  al render por la cola de 8 fotogramas: el GPU temprano señala los tokens/draw-done al escribirlos).
- 11:35 Z09 (tprof) con `tools/vfase.py`: en agua, por fotograma, fin del trabajo del hilo principal (ms desde la
  copia XFB) → % de fotogramas de 2+ retrazos: <12 ms 1 % · 12-14 41 % · 14-15 95 % · 15-16,7 97-99 % · >16,7 100 %.
  El corte está en ~13,5-14 ms, no en 16,7: el juego pierde ~2,7 ms por fotograma. Las copias XFB caen en una fase
  fija respecto a la rejilla de 16,67 ms.
- 11:40 `tools/ppcdis.py` (desensamblador mínimo del DOL): `EGG::AsyncDisplay::beginFrame` (802BB660) espera un
  mensaje en su cola (OSReceiveMessage BLOCK) que manda `postVRetrace` (802BB840) en cada retrazo; luego copia
  EFB→XFB (802BAED0). No hay ninguna espera de tiempo propia del juego tras el retrazo.
- 11:45 runtime con `NWII_V_TL` (PC, `bin/NSMBW_v1`): agua en PC: retrazo → INT 24 (VI) inmediato → el juego sigue
  OCIOSO **2,07 ms exactos** → copia XFB. Con `NWII_NO_IDLE_SLEEP=1`: 0,02 ms. Causa: el retrazo despierta al hilo
  principal desde la interrupción, pero el bucle ocioso del SO invitado (801B4FF0) ya había evaluado su condición
  y llama a `idle_wait`, que duerme hasta el siguiente suceso con tope de 2 ms (`min(us-100, 2000)`); el hilo
  despertado corre al acabar ese sueño. En la Switch el sueño es `s2::game_idle_sleep` (mismo tope) → ~2-2,7 ms.
  Es decir, el presupuesto real del fotograma era 16,67 − ~2,3 ≈ 14,4 ms, no 16,67.
- 11:50 **cambio 1 `NWII_V_WAKE=1`** (ios.cpp cuenta entregas al invitado `g_v_dispatch_seq` en las 6 salidas de
  despacho; os_hle_stubs.cpp `idle_wait` no duerme si la cuenta cambió desde la vuelta anterior): PC agua retrazo →
  XFB 0,03 ms, el juego sigue durmiendo el 80 % del tiempo (no gira).
- 12:00 **cambio 2 `NWII_V_LATE=<ms>`** (retrazo elástico) y el banco de pruebas `NWII_V_SLOWTEST`. PC agua con
  `NWII_V_SLOWTEST=11000,5000` (juego de ~14-19 ms/fot) reproduce la Switch: 34,9 fot/s, 72 % de fotogramas de 33 ms.
- 12:05 PC con `NWII_V_SLOWTEST=11000,5000` (juego ~14-19 ms/fot, como agua en la Switch), agua, ventana de 45 s
  (`tools/pc_ab.sh`, `tools/vtl.py … resumen`), fotogramas del juego/s · % de fotogramas > 25 ms:

  | variante | fot/s juego (= dibujados) | fot. > 25 ms | retrazo → XFB |
  |---|---|---|---|
  | m5 tal cual | 34,9 | 71,7 % | 2,09 ms |
  | `NWII_V_WAKE=1` | 42,2 | 41,9 % | 0,02 ms |
  | + `NWII_V_LATE=4` | 52,1 | 14,4 % | |
  | + `NWII_V_LATE=8` | 54,3 | 8,2 % | |
  | + `NWII_V_LATE=12` | 56,3 | 2,1 % | |

  Tubería en PC (s_base): XFB → trabajador listo 1,6 ms, listo → render lo toma 0,01 ms, render 1,8 ms, fin → swap
  0,16 ms; 1 de 1 523 fotogramas no dibujado. Nada aguas abajo frena al juego.
- 12:20 Switch: `tools/v37_pgo_remote.sh v37a` en la otra laptop (copia `r17pgo/v37asrc` = m5src + 4 fuentes de aquí,
  `nx/v37a_pgo` = copia de `nx/m5_pgo`; ordenes exactas de ninja para gx.o, ios.o, os_hle_stubs.o, main.o, ar,
  enlace y nro; objetos PGO de m4/m5 sin tocar). Tabla fastmem 866 754 entradas, 0 mal formadas (= m5_pgo).
  `bin/v37a_pgo.nro` sha256 3694589551…0903a (`diag/sha256_v37a.txt`).
- 12:15 PC checks 1x (`tools/checks.sh` = copia de r36r/test/checks.sh con T=r37v; `bin/NSMBW_v4_input_test` = mismas
  fuentes que v37a): con `NWII_V_WAKE=1 NWII_V_LATE=8` title/map/walk/lvl11 **220/220 FRAMECHECK idénticas**,
  diff_lines=0, 0 trampas/asserts de S2, salida normal; apagado title 220/220 (`diag/checks_v4{on,off}.txt`).
- 12:15 consola en cola (detrás de C01-C03): V01_v37a_off, V02_v37a_wake, V03_v37a_late8, V04_v37a_late12,
  V05_v37a_tl_off, V06_v37a_tl_late8 (`tools/encolar_v.sh bin/v37a_pgo.nro v37a 1`; mismo NRO, lista fija, 400 s).
- Notas del coordinador (frente C): (1) cuelgue Z22 = inversión de prioridades con esperas activas (sched_yield) en
  el núcleo 2. Mis cambios no añaden esperas activas: `NWII_V_WAKE` solo hace que el bucle ocioso del invitado dé una
  vuelta más sin dormir tras una interrupción; `NWII_V_LATE` usa el planificador de sucesos invitado (sin hilos ni
  esperas nuevas). Existente y NO tocado: `FifoLock` (gx.cpp) gira con `std::this_thread::yield()` tras 64 vueltas.
  (2) régimen lento biestable del trabajador (lote con dos finales de fotograma → camino lento de PrepFrame, mueve
  ~31 000 órdenes): NO lo toco (ver «Qué queda»).

## Qué queda / para otros frentes
- **Régimen lento del trabajador (fiesta del castillo, dato del frente C)**: cuando un lote trae dos finales de
  fotograma (`prev_frame_end >= 0` en PrepFrame), el código va por el camino lento: mueve una a una TODAS las órdenes
  del lote (incluido el fotograma bueno entero, ~31 000) a `s_frame` y deja las entradas de los draws sin preparar
  (`kEntryUnknown`), así que el render también trabaja más; el siguiente fotograma llega tarde otra vez (biestable).
  Propuesta (no hecha aquí: toca `Render()` de renderer_gl.cpp, terreno de R2): hacer con el tramo descartado
  `[0, prev_frame_end]` lo mismo que `RetractToPrefix` (compactar con rcoal, sin draws, copias EFB `stale_copy`) y
  AÑADIRLO a `pkt.pre`; el fotograma bueno se queda donde está (camino rápido: `frame.swap(s_carry)` con índices de
  draws > prev_frame_end y sus entradas) y `Render()` empieza en `prev_frame_end + 1` (un desplazamiento como
  `g_render_prefix`). Coste: copiar ~1,5-2 k órdenes vivas del tramo descartado en vez de mover ~31 k.

## Consola, v37a (mismo NRO, contiguas, lista fija, 400 s, 1020/460 MHz en todas; 80 ventanas y 5 escenas)
`tools/tabla_v.py` (usa r31t/tools/por_escena_real.py). Celdas: fps dibujados · fps juego · ms juego/fot REAL · render ms/fot.

| escena | V01 apagado (= m5) | V02 `V_WAKE` | V03 `V_WAKE`+`V_LATE=8` | V04 `V_WAKE`+`V_LATE=12` |
|---|---|---|---|---|
| desierto4j | 53,6 · 54,0 · 11,6 · 14,1 | **57,8 · 59,2** · 11,4 · 14,4 | 58,4 · 59,8 · 11,5 · 14,2 | 57,9 · 59,7 · 11,8 · 14,7 |
| agua_pinguino | 34,9 · 35,9 · 15,5 · 15,0 | **40,2 · 47,8** · 14,6 · 15,5 | 31,7 · **56,8** · 14,6 · 15,2 | 33,2 · 55,3 · 15,1 · 16,0 |
| cielo_paratroopas | 43,6 · 43,7 · 14,5 · 14,4 | **58,8 · 59,8** · 14,3 · 14,8 | 58,2 · 59,9 · 14,2 · 14,5 | 57,2 · 59,8 · 14,5 · 15,4 |
| nubes_plataformas | 58,0 · 58,1 · 11,3 · 10,3 | **59,9 · 59,9** · 11,3 · 10,7 | 59,9 · 59,9 · 11,0 · 10,3 | 59,9 · 59,9 · 11,5 · 10,8 |
| castillo | 46,2 · 59,3 · 6,3 · 13,4 | 46,8 · 59,4 · 6,0 · 13,5 | 46,0 · 59,4 · 6,0 · 13,3 | 46,5 · 59,4 · 6,4 · 13,8 |

Corridas: `bench_V01_v37a_off_1115_1233`, `bench_V02_v37a_wake_1115_1242`, `bench_V03_v37a_late8_1115_1251`,
`bench_V04_v37a_late12_1115_1300`. Las cuatro IMAGEN OK; hojas de V01/V02 miradas (limpias; con el juego más rápido
caben más demos en 400 s). [V] en V03: 2 035 retrazos retenidos (1 680 lanzados al esperar, 355 por plazo, 9 perdidos).
- `NWII_V_WAKE` sola sube desierto +4,2, cielo **+15,2**, nubes +1,9, agua +5,3 fps dibujados (juego agua 35,9 → 47,8).
- Con `NWII_V_LATE` el JUEGO de agua llega a 55-57 fot/s, pero los DIBUJADOS bajan a 32-33: el trabajador «gxw» no da
  abasto y cae en el régimen lento (V03, tramo pesado de agua, `[WPROF]` 37-43 ms por fotograma preparado: parse
  15-19, **split 14-15**, decode 8; 45 de ~115 fotogramas por 2 s preparados) — el mismo régimen biestable que vio el
  frente C en la fiesta. El render también va a 17-18 ms/fot en ese tramo. Siguiente límite de agua: trabajador
  (camino de descarte) y render, no el juego.

## Línea de tiempo medida en consola (V05 = m5 + `NWII_V_TL`, V06 = + `V_WAKE` + `V_LATE=8`; `diag/vtl_V0{5,6}.txt`)
`tools/vtl_escenas.sh <bench> <vtl.bin>`; medianas (p90) en ms; «trabajo» = desde la copia XFB hasta que el hilo del
juego entra en la espera del retrazo.

| | desierto V05 → V06 | agua V05 → V06 | cielo V05 → V06 | nubes V05 → V06 | castillo V05 → V06 |
|---|---|---|---|---|---|
| fot/s juego | 53,7 → 59,8 | 35,9 → 55,1 | 45,4 → 59,9 | 58,1 → 60,0 | 59,2 → 59,2 |
| fot/s dibujados (traza) | 51,4 → 56,1 | 33,9 → 30,8 | 43,5 → 56,1 | 56,0 → 57,7 | 48,8 → 50,0 |
| trabajo del juego | 11,6 (14,5) | 15,8 (20,4) → 15,8 (18,6) | 14,3 (15,4) | 11,4 (13,4) | 4,4 (8,7) |
| retrazo → copia XFB | **2,06** → 0,06 | **2,05** → 0,05 | **2,05** → 0,05 | **2,06** → 0,05 | **2,06** → 0,05 |
| fotogramas de 2+ retrazos | 11,7 % → 0,2 % | 64,6 % → 6,0 % | 32,1 % → 0,0 % | 3,1 % → 0 % | 0,2 % → 0 % |
| XFB → trabajador listo | 2,7 (5,9) | 8,6 (16,8) → 8,1 (15,6) | 3,7 (10,3) | 2,4 (4,2) | 2,2 (12,5) |
| listo → render lo toma | 0,02 | 0,02 | 0,02 | 0,02 | 0,02 (7,7) |
| render (RenderPacket) | 13,6 (15,0) | 14,3 (16,1) → 15,2 (17,5) | 13,8 (15,8) | 10,1 (11,0) | 12,2 (19,5) |
| fin render → swap hecho | 1,5 | 2,3 → 2,7 | 1,7 | 1,5 | 1,6 |
| XFB → swap (latencia) | 19,0 → 18,7 | 25,6 → 26,2 | 21,0 → 20,1 | 14,2 → 14,2 | 16,4 → 16,9 |
| fot. del juego no dibujados | 4 % | 6 % → **44 %** | 4 % | 4 % | 18 % |

Qué espera a qué (verificado con la traza, PC y consola):
1. El hilo del juego (núcleo 1) solo espera al retrazo VI (OSReceiveMessage en `AsyncDisplay::beginFrame`, que
   `postVRetrace` llena en cada retrazo). NO espera al render: la cola juego→trabajador admite 8 fotogramas
   (`backlog waits 0`), los tokens/draw-done se señalan al escribirlos.
2. Pero el despertar tras el retrazo llegaba **2,05 ms tarde** en TODAS las escenas: el retrazo despierta al hilo
   desde la interrupción, y el bucle ocioso del SO invitado vuelve a `idle_wait`, que duerme hasta 2 ms antes de
   mirar la cola de hilos listos. El presupuesto del fotograma era 14,6 ms, no 16,7: ahí estaba la «cadena».
3. Trabajador «gxw» (núcleo 2): toma el FIFO cada 1 ms; listo 2-9 ms después de la copia XFB. Si un lote trae dos
   finales de fotograma (va atrasado) descarta el primero por el camino lento (ver «Qué queda»).
4. Render (núcleo 0): toma el fotograma listo en 0,02 ms (no duerme con trabajo pendiente); «render ms/fot» es solo
   `RenderPacket` (CPU + espera de GPU, 0,3 %), no incluye esperas. Después: liberar órdenes (FREECHUNK, ~2 ms
   cuando no hay otro fotograma esperando) + blit de presentación + swap.
5. Swap: 0,2-0,3 ms, NO bloquea por vsync en ningún caso medido (hilo de render).
6. Se descarta: el trabajador junta lotes (fotogramas del juego no dibujados: 4 % normal, 18 % castillo, 44 % agua
   con el juego a 55 fot/s) y, si el render no ha tomado el anterior, lo retira (retract → prefijo).

## Cambio 3: `NWII_V_DROPFAST=1` (descarte barato en el trabajador; gx.cpp PrepFrame) — v37b
- Cuando un lote trae dos o más finales de fotograma, la parte descartada `[0, prev_frame_end]` (sin draws, sin
  escrituras muertas de rcoal, copias EFB `stale_copy`) va a `pkt.pre` como en `RetractToPrefix`, y el fotograma
  bueno sigue el camino rápido (conserva las entradas del parser: no se vuelven a preparar sus draws). Requiere
  `NWII_R_PREFIX` (encendido por defecto en m5). Sigue moviendo el fotograma bueno una vez (`erase` del tramo
  descartado) y destruyendo los draws descartados en el trabajador (cuentas no atómicas compartidas con la caché de
  listas: no se pueden liberar en el render).
- PC, trabajador frenado (`NWII_V_SLOWWORKER_US=22000`, `tools/df_ab.sh`), FRAMECHECK=10 + COALESCE_CHECK a 1,5789x:
  castillo apagado/encendido 360/360 y 380/380 idénticas, 0 discrepancias de coalesce, 1 939 descartes baratos;
  trabajador por fotograma preparado 2,1-3,7 → 1,9-2,4 ms (decode 0,3-0,7 → 0,04-0,13). Agua (2.ª tanda) 380/380 y
  380/380, ocupado 2,0-4,9 → 1,4-3,9 ms. (Primera tanda de agua con la máquina cargada por otros agentes: 339/340 con
  UNA comprobación de diferencia máx. 1 en 282 037 píxeles; no se repitió en las tres tandas siguientes.)
- PC checks 1x con los tres interruptores + trabajador frenado 20 ms + COALESCE_CHECK: title/map/walk/lvl11 160/160
  idénticas, ~2 000 descartes baratos por corrida, 0 discrepancias (`diag/checks_v5all_slowworker.txt`).
- Switch: `tools/v37_pgo_remote.sh v37b` → `bin/v37b_pgo.nro` sha256 337e812b…aeb98ac (`diag/sha256_v37b.txt`), fastmem
  0 mal formadas. En cola: V07_v37b_late8 (WAKE+LATE=8), V08_v37b_late8df (+DROPFAST).

## Cierre de WAKE / LATE (15:00)
- **Recomendado: `NWII_V_WAKE=1` y `NWII_V_LATE=8`.** LATE=12 no mejora a 8 en consola (V04 ≈ V03); con 8 la
  retención mediana es 2,6 ms (p90 7,2) en agua y casi nula en el resto; menos jitter de retrazo/mando.
- Velocidad del juego: los retrazos siguen siendo 60/s (el reloj invitado no se toca: solo se retrasa la
  ENTREGA de la interrupción, como mucho LATE ms, y los siguientes siguen en la rejilla). La lógica del juego, que
  va por fotogramas, pasa a ir a 55-60 fot/s en vez de 36-54 (agua: velocidad del juego 60 % → 92-95 % del tiempo
  real). Nunca puede pasar de 60 (un fotograma por mensaje de retrazo).
- Audio (corridas sin traza V01-V04): listas AX 326,4 / 325,7 / 325,6 / 325,5 por s, salida 31,32 / 31,25 / 31,23 /
  31,22 kHz, underruns 64 / 72 / 67 / 70 en 400 s, 0 ms descartados: igual.
- Reloj invitado: el deslizamiento total (gobernador + esperas de la cola de 8 fotogramas) pasa de 7,5 s (6,4 en el
  arranque) a 8,5-8,9 s: +0,4-0,9 s en 400 s por más esperas de la cola («backlog waits» 17 → 38-82) cuando el
  trabajador va atrasado. Durante esas esperas se paran a la vez juego y audio invitados (no se desincronizan).
- Riesgo de LATE: si el plazo se cumple tarde (traza: máx. 21 ms en agua) puede perderse un retrazo (9 de 2 277 en
  V03, «perdidos»), igual que hoy cuando un retrazo coincide con otro pendiente. Interacción con S2: ninguna (los
  retrazos los gestiona solo el contexto del juego; no hay esperas activas nuevas).
- V05/V06 («tl») explicados: son V01/V03 con la línea de tiempo; la traza cuesta ~0-1 fot/s y algo de underruns (99
  frente a 67) por escribir ~20 KB cada 2 s a la SD desde el hilo de render. Solo diagnóstico.

## Política del trabajador en agua (propuesta, sin hacer)
Medido en V03 (agua pesada, `[WPROF]` por 2 s): juego 114-122 fotogramas, trabajador prepara 45-52; parse 15-19 ms
por fotograma preparado ≈ 6,5 ms por fotograma del juego (de ello 6,5-8,4 ms por preparado son aciertos de la
caché de listas copiando órdenes). Unos 70 fotogramas cada 2 s se parsean enteros y se tiran: ~455 ms/2 s ≈ **23 %
del núcleo 2** en fotogramas que nunca se dibujan (más el split del camino lento, que ya quita el DROPFAST de D).
Propuesta: el trabajador sabe ANTES de parsear cuántos finales de fotograma trae lo que toma (`s_fifo_frames`,
contado por el hilo del juego en cada copia XFB). Si son ≥ 2, todos menos el último se van a tirar: parsearlos en
modo «solo estado» (sin crear órdenes de draw, saltando los bytes de vértices; en un acierto de lista aplicar solo
las escrituras de registro netas de la lista, precalculadas una vez por entrada de la caché), y crear órdenes
normales solo desde el último final. Estimación (inferida): si ese modo cuesta ~30 % de un parse normal, libera
~15 % del núcleo 2 ≈ 13-15 fotogramas dibujados más por segundo en agua (cada uno ~11 ms de trabajador). Toca el
parser (FifoParser) y la caché de listas: no lo hago aquí.

## CIERRE (15:45)
- V07/V08 (DROPFAST propio) cancelados por el coordinador: el descarte barato de la troncal es el del frente D
  (`NWII_D_DROPFAST`, r9/r40d, hace lo mismo sin mover el fotograma bueno: `g_render_start`). **`NWII_V_DROPFAST`
  queda RETIRADO**: solo en `r37v_dropfast_vs_v37a.patch` (aparte) y en `nWiiRuntime/` de esta carpeta; no va en el
  parche final. Revisado el de D: no veo fallos (resetea DrawRaw/vértices en el trabajador, así `render_frees` sigue
  valiendo; `RetractToPrefix` respeta `pkt.start`).
- **Parche final** (= fuentes exactas del NRO medido v37a): `r9/r37v/r37v_v37a_vs_m5.patch`, aplicar con
  `patch -p1` dentro de una copia de `r9/r35m4/nWiiRuntime_m5` (comprobado: queda idéntico a `fuentes_v37a/`).
  Toca main.cpp, os_hle_stubs.cpp, gx.cpp, ios.cpp. Interruptores: `NWII_V_WAKE`, `NWII_V_LATE`, `NWII_V_TL`,
  `NWII_V_SLOWTEST` (solo PC); todos apagados por defecto. Encender en m6: `NWII_V_WAKE = "1"`, `NWII_V_LATE = "8"`.
- Riesgos conocidos: (a) `V_LATE` puede perder un retrazo si su plazo se cumple tarde (9 de 2 277 en V03; hoy ya se
  pierden cuando coinciden dos); (b) con el juego a 55-60 en agua el trabajador del núcleo 2 queda saturado y aumentan
  las esperas de la cola de 8 fotogramas (+0,4-0,9 s de reloj invitado parado en 400 s, juego y audio a la vez);
  (c) `V_WAKE` hace que el bucle ocioso del invitado dé una vuelta más sin dormir tras cada interrupción (sin coste
  medible: el juego sigue durmiendo el 80 % en PC); (d) `NWII_V_TL` escribe ~20 KB/2 s a la SD desde el hilo de render
  (solo diagnóstico; sube algo los underruns).
