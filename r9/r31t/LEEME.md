# r31t: frente T — reparto del trabajo entre núcleos (12-nov-2026)

Copia propia del runtime de m2 (`nWiiRuntime/` = r7f de `r24m2/nWiiRuntime_r7f_src` + lo de abajo); el código
generado es el de m2 (G6 e2, mismos objetos y mismo orden de enlace). Sin commits. No se tocó ninguna otra
carpeta de `r9/` salvo las salidas del arnés de PC (`r9/render/pc/run.sh`, `r9/render/test/det.sh` y
`checks.sh` escriben en sus `runs/`: carpetas `t_tp*`, `t31_*`, `chk_t31_*`, `t31det_*`), ni `nand/`, ni la SD.

## 0. Lo más importante (para decidir)

1. **La métrica «ms juego/fot» estaba inflada en las escenas donde el render no da abasto.** La línea
   `[NX] game X fps` cuenta fotogramas **dibujados** (`g_stat_frames`), no fotogramas del juego. En desierto4j,
   agua y castillo el hilo de render descarta fotogramas, así que `cpu% × 1000/fps` reparte el trabajo del
   juego entre menos fotogramas de los que hizo. Con los fotogramas reales del juego (líneas `[PIPE] game
   frames=`; guion nuevo `tools/por_escena_real.py`) y confirmado con el perfil nuevo (contador de copias XFB):

   | Escena | m2: fórmula vieja | m2: **real** | PGO m2: vieja | PGO m2: **real** | g6c3: real | lo que tarda el render en dibujar 1 fot. |
   |---|---|---|---|---|---|---|
   | desierto4j | 23,8 | **20,7** (juego a 33 fot/s) | 27,1-28,6 | **16,0-16,8** (juego a 43-47 fot/s) | 19,8 | **33-36 ms** |
   | agua_pinguino | 29,4 | **26,4** | 27,1-27,5 | **22,4-23,0** | 25,2 | **29-30 ms** |
   | cielo_paratroopas | 23,2 | 23,2 | 19,1-20,2 | 19,2-20,1 | 22,1 | 13-14 ms |
   | nubes_plataformas | 19,6 | 19,5 | 15,8 | 15,8 | 18,8 | 10 ms |
   | castillo | 24,5 | **15,0** (juego a 55 fot/s) | 14,1 | **9,7** | 13,8 | 23 ms (+ trabajador 17-20 ms) |

   Es decir: **el PGO sí mejora el desierto (−21 %)**; lo que pasa es que el juego va más rápido (la demo dura
   61 s en vez de 79-80 s), produce más fotogramas, el render sigue dibujando ~27-29 por segundo y el núcleo 2
   (trabajador) sube porque prepara más fotogramas que luego se descartan.
2. **En el desierto (y en agua) el límite de lo que se ve era el hilo de render, no el del juego**, y la causa
   está medida y arreglada (§3, §5, §6): cada fotograma se **creaban y destruían texturas de copias EFB**
   (una misma dirección recibe copias de dos tamaños: 2 reasignaciones + 2 «nativas» nuevas + 2 borradas por
   fotograma), y en la Switch cada una es memoria nueva de la GPU (ioctl al núcleo, puesta a cero) y, al
   soltar la vieja, **esperar a que la GPU acabe todo lo enviado**. El hilo de render pasaba en el desierto
   el 35 % esperando a la GPU y el 17 % en llamadas al núcleo por esas texturas; en cielo y nubes, 0 %.
   Con `NWII_T_EFBVAR` (una textura por tamaño, sin reasignar): **dibujar un fotograma del desierto pasa de
   33,1 a 13,5 ms y el de agua de 29,2 a 15,5 ms**; ya no se descartan fotogramas y lo que se ve vuelve a
   depender solo del hilo del juego. Binario con el arreglo encendido por defecto: `bin/nsmbw_t1.nro`.
3. **Reparto del hilo del juego (m2, consola, por fotograma del juego):** no hay esperas a mitad de trabajo
   (todo el tiempo ocioso es la espera al retrazo en `beginFrame`). El trabajo es casi todo código del juego
   en serie; lo único grande que es separable por naturaleza es **el sonido: 3,6-5,7 ms por fotograma a
   ~31 fot/s = 11-17 % del núcleo**, y es proporcional al tiempo, no a los fotogramas (333 fotogramas AX por
   segundo): a 60 fot/s serían 1,9-3,0 ms por fotograma.
4. **«ms por fotograma» medido a 30 fot/s exagera lo que costaría a 60**: la parte proporcional al tiempo
   (sonido) se reparte entre el doble de fotogramas. Proyección a 60 fot/s del hilo del juego (m2 / con PGO
   ×0,8): desierto 17,9 / 14,3 · agua 23,1 / 18,5 · cielo 20,5 / 16,4 · nubes 16,9 / 13,5 ms. Con PGO **y**
   el sonido en otro núcleo: 12,2 · 16,2 · 14,9 · 11,9 → las cuatro por debajo de 16,7.

## 1. Cómo se midió: perfil con la pila invitada (`NWII_TPROF=1`)

Nuevo en el runtime (`nWiiRuntime/src/core/tprof.cpp`, enganchado al muestreador de `switch_profiler.cpp`;
en PC, una señal al hilo de la CPU). Cada milisegundo, con el hilo del juego detenido un instante, guarda:

- pc/lr del anfitrión, número de fotograma **del juego** (copias XFB), escena de `NWII_FORCE_DEMO`,
  hilo invitado actual;
- la **pila invitada**: se sigue la cadena de marcos PPC desde `r1` y se anota cada dirección de retorno
  (hasta 56 niveles) → se sabe bajo qué función raíz (fase) cae cada muestra, no solo en qué función está;
- si el hilo actual no es el principal del juego, la pila guardada del principal (dónde espera);
- si el pc está en una llamada al núcleo, los retornos del anfitrión (esperas del runtime);
- (tp3+) también una muestra por ms de los hilos de render y trabajador, con sus esperas → `tprof_host.bin`.

Archivos `logs/tprof_*.bin` (se bajan con `PULL_PREFIX=tprof`). El muestreador se queda ~3,8 s parado cada
10 s mientras escribe en la SD (el juego no se para): se pierde el 38 % de las muestras, sin sesgo. Coste:
la pausa por muestra sube de ~7 a ~9 µs (0,5-0,7 % del hilo).

Análisis: `tools/tprof.py <carpeta> <nm del .elf> <orden>` (órdenes `resumen`, `fases`, `cruce`, `linea`,
`esperas`, `top`, `arbol <dir>`, `hilos`), con las fases y rangos de `tools/fases.py`. Salidas guardadas en
`datos/T01_*.txt` y `datos/T05_hilos.txt`. Nombres: `symbols/usa_symbols.txt` + los puestos a mano en
`fases.py`.

Orden real de un fotograma (`dSys_c::execute`, decomp `d_system.cpp`): espera al retrazo y copia XFB
(`beginFrame`) → `m3d::calcMaterial` → `painter` (dibuja el estado que dejó la lógica del fotograma anterior:
CalcView, DrawOpa/DrawXlu, efectos, 2D) → `endRender` → `fManager_c::mainLoop` (lógica: execute de actores,
efectos, alta de modelos) → `calcAudio`/`endFrame` → espera al retrazo. El sonido corre aparte: una
interrupción cada 3 ms (AX) y, despertado por ella, el hilo de `nw4r::snd`.

## 2. Línea de tiempo del hilo del juego (m2, consola 1020 MHz, corrida T01)

ms por fotograma **del juego** (`datos/T01_fases.txt`; castillo queda fuera: empieza al final de los 400 s):

| Fase | desierto4j | agua | cielo | nubes |
|---|---|---|---|---|
| **Dibujo del fotograma anterior** | | | | |
| g3d CalcMaterial (todos los modelos) | 0,71 | 0,73 | 0,68 | 0,64 |
| g3d CalcView (matrices de vista por modelo + luces EGG) | 2,53 | 2,61 | 1,96 | 2,20 |
| g3d DrawOpa/DrawXlu → órdenes GX | 0,84 | 3,27 | 2,34 | 1,47 |
| efectos nw4r::ef (dibujo) | 1,75 | 1,55 | 0,40 | 0,59 |
| resto de `painter` (2D, captura de texturas de luz, EGG) | 1,22 | 0,89 | 0,81 | 0,79 |
| **Lógica** | | | | |
| execute de actores | 5,98 | 9,00 | 10,85 | 7,72 |
| efectos nw4r::ef (cálculo, `EffectManager_c::calcGroup`) | 1,16 | 0,79 | 0,16 | 0,31 |
| draw de actores (alta de modelos) + resto de fManager | 0,60 | 0,89 | 1,06 | 0,78 |
| **Sonido (proporcional al tiempo)** | | | | |
| hilo nw4r::snd (`SoundThread::FrameProcess`) | 2,29 | 3,39 | 1,40 | 1,82 |
| interrupción AX (`__AXSyncPBs`, aux/reverb, callbacks) | 1,99 | 1,95 | 1,88 | 1,53 |
| cambio de hilo / SO por el sonido | 0,52 | 0,38 | 0,30 | 0,25 |
| resto (`beginFrame` sin la espera, mandos, HBM…) | 0,47 | 0,45 | 0,40 | 0,42 |
| **TOTAL ocupado** | **20,1** | **25,9** | **22,3** | **18,5** |
| ocioso: espera al retrazo en `beginFrame` | 10,2 | 7,0 | 10,1 | 12,2 |
| fotograma (pared) | 30,2 | 32,9 | 32,3 | 30,7 |

Por subsistema (media de las 4, `datos/T01_cruce.txt`, 21,7 ms): g3d+m3d 4,4 · juego «resto» 3,3 ·
nw4r::snd 2,2 · GX 1,7 · colisión con el fondo 1,4 · AX/AXFX 1,1 · EGG 1,0 · MTX 1,0 · ef 1,0 · math 0,9 ·
jugador 0,8 · MSL 0,7 · resto 2,2. Dentro de la lógica (8,4 ms): juego 1,8+0,8+0,5+0,5, g3d 1,6, colisión
fondo 1,2, MTX 0,4, math 0,5.

Runtime puro en el hilo del juego (media, por fotograma de 31,5 ms): sondeo de interrupciones 0,75 ms
(`process_pending_callbacks_slow` 0,32 + `poll_tb_refresh` 0,32 + reloj 0,11), escritura GX
(`GX_WGPIPE_*`) 1,0, reverb nativa 0,25, psq/ps 0,6.

Esperas (`datos/T01_esperas.txt`): todo el tiempo ocioso es el hilo principal dormido en
`EGG Display::beginFrame` (espera al retrazo). Esperas al candado del FIFO, al hilo AX o lecturas de disco
dentro de un fotograma: < 0,1 ms. No hay esperas evitables a mitad de trabajo.

Línea de tiempo (desierto, `datos/T01_linea.txt`): tras la copia XFB, 0-8 ms dibujo (CalcMaterial →
CalcView → DrawOpa → efectos), 9-17 ms lógica, 18-30 ms ocioso; el sonido cae repartido por todo el
fotograma. Duración mediana 33,1 ms, p10 16,6 ms (el desierto alterna fotogramas de 1 y 2 retrazos).

## 3. Qué tiene de distinto el desierto (corrida T05, `datos/T05_hilos.txt`)

| Hilo de render (núcleo 0), % de su tiempo | desierto4j | agua | cielo | nubes |
|---|---|---|---|---|
| esperando a la GPU (fence de nouveau, dentro de `pushbuf_flush`) | **35 %** | **32 %** | 0 % | 0 % |
| llamadas al núcleo por memoria de texturas (`nvMapCreate/Free`, `MapBufferEx/Unmap`) | **17 %** | **10 %** | 1 % | 2 % |
| memset + `svcSetMemoryAttribute` + flush de caché (memoria nueva de textura) | 7,5 % | ~2 % | 0 | 0 |
| sin fotograma que dibujar (ocioso) | 1 % | 13 % | 54 % | 58 % |
| CPU del runtime (EmitPrimitive, RenderOnce, hash_ram…) | 18 % | 23 % | 20 % | 16 % |

Las cadenas de llamadas: `st_TexImage → st_AllocTextureImageBuffer → nv50_miptree_destroy / nvc0_miptree_create
→ nouveau_bo_new → nvMapCreate / nvAddressSpaceMap`, `_mesa_delete_texture_image → nouveau_bo_ref → nvMapClose`,
y la espera `pushbuf_flush → nouveau_bo_ref → nvFenceWait`. Contado en PC a la escala de la Switch (1,5789x)
con el contador nuevo `[TCSTAT2]`: en el desierto **una misma dirección de destino recibe dos copias EFB de
distinto tamaño en cada fotograma** → `acquire_efb_copy` reasigna la textura 2 veces por fotograma y la
reducción «nativa» se crea 2 veces y se borra 2 veces. En PC no cuesta; en la Switch cada reasignación/borrado
obliga a esperar a la GPU en mitad del fotograma.

El hilo del juego en el desierto no tiene nada raro: 20,1 ms, de los que 4,8 son sonido; es la escena con
menos lógica (6,0 ms) y más efectos (2,9 ms entre cálculo y dibujo).

## 4. Candidatos para sacar trabajo del hilo del juego

ms por fotograma medidos a ~31 fot/s (m2) → proyección a 60 fot/s (lo proporcional al tiempo se divide por ~2).

| # | Candidato | ms/fot hoy | a 60 fot/s | ¿Idéntico? riesgo | Verificación | Coste |
|---|---|---|---|---|---|---|
| d | **Sonido en otro núcleo** («CPU de sonido»: la interrupción AX y el hilo `nw4r::snd` ejecutados por un segundo contexto invitado en el núcleo 2) | 3,6-5,7 | 1,9-3,0 | Mismo resultado lógico (el sonido ya es asíncrono respecto al juego: interrupción cada 3 ms), no el mismo instante. **Riesgo alto**: el SO invitado es de un solo procesador | escenas completas con sonido, comparar WAV de `NWII_AUDIODUMP`, corridas largas | 1-2 semanas (diseño en §4.1) |
| e1 | **Sondeo de interrupciones menos frecuente** (`NWII_POLL_EVERY`) | perfil: 0,75; **medido con N=128: −1,0 a −1,5** | ~−0,8 | Lógica igual; cambia el instante en que se atienden los eventos (hasta ~50 µs más tarde con 128) y, en la medida, el ritmo del audio baja ~1 % (§9.3). Riesgo bajo | `checks.sh`, lista | **hecho** (§5) |
| d2 | Reverb/aux de AX (`nwii_a2_axfx`, ya nativa) aplicada en el hilo AX al mezclar | 0,35-0,47 | ~0,2 | Sí si nadie lee el búfer aux tras el callback; riesgo medio (nw4r aplica volumen después) | comparar WAV | 2-3 días (toca `output_24`) |
| c | Cálculo de efectos (`EffectManager_c::calcGroup`) en otro hilo entre el final de la lógica y el dibujo de efectos | 0,2-1,2 | igual | Riesgo alto: hay que probar que nadie toca partículas en ese tramo (draw de actores, creación de efectos) y que nw4r::ef no usa el generador de azar del juego ni montículos con mutex | `checks.sh` + comparador de RAM | 1 semana |
| a | g3d por modelo en paralelo (CalcView 1,2 ms, CalcMaterial 0,7) | ≤ 1,0 con 2 hilos | igual | **No recomendable**: los modelos comparten el caché bloqueado (0xE0000000) como borrador, CalcMaterial llama a GX (FIFO y estado sombra `__GXData`), y el código generado usa una sola ventana de memoria global | — | — |
| b | Todo el dibujo (CalcMaterial+painter, 4,9-8,3 ms) en otro hilo mientras el juego hace la lógica siguiente | 4,9-8,3 | igual | **No es posible con resultado idéntico**: la lógica reescribe en el sitio lo que el dibujo lee (matrices de modelos, partículas, paneles); haría falta una instantánea de la RAM invitada (copia al escribir por fallo de página: >1000 fallos × ~10 µs por fotograma en la Switch) | — | — |
| e2 | Esperas a mitad de fotograma | 0 | 0 | No hay nada que quitar | — | — |
| f1 | (fuera del hilo del juego) texturas de copias EFB que se reasignan | render: ~17 ms de 33 en el desierto | — | Sí (misma textura, mismo tamaño y contenido; solo no se destruye) | determinista PC A/B, capturas | **hecho** (§5) |

El resto del hilo del juego (lógica de actores, colisiones, g3d dentro de execute, escritura GX) es código
invitado en serie con estado compartido: no se puede repartir; ahí solo sirve abaratar el código (PGO, G6).

### 4.1 Diseño del «CPU de sonido» (no implementado)

Por qué es separable: en la consola real la interrupción AX y el hilo de sonido ya interrumpen al juego en
cualquier instrucción; el juego solo se protege con el mutex de `SoundThread` y con «interrupciones
apagadas». Lo que complica: el SO invitado (planificador, colas, «hilo actual» en 0x800000E4) es de un solo
procesador. Auditoría hecha: en todo el DOL solo 9 funciones tocan `OSCurrentThread`/`OSCurrentContext`
(`OSGetCurrentThread`, `OSSleepThread`, `SelectThread`, `OSSetCurrentContext`…), y los caminos de sonido
(194 funciones vistas en el perfil) solo salen de sus bibliotecas hacia `OSLockMutex`, `OSUnlockMutex`,
`OSSendMessage`, `OSReceiveMessage`, `OSWakeupThread`, `OSDisable/RestoreInterrupts`, listas de MEM,
`memcpy` y matemáticas. Diseño:

1. Segundo `CPUContext` + hilo anfitrión en el núcleo 2 con su pila invitada; ejecuta el callback de
   fotograma de AX (`0x801A1BA0`, sin pasar por `__AIDHandler`, que cambia el contexto actual) y el cuerpo
   de `SoundThread` (`0x8027B960`).
2. Esas funciones del SO se interceptan **al enlazar** (`-Wl,--wrap=`, sin tocar el generador; hay que
   comprobar que G6 no las pegó dentro de quien las llama) y, cuando el contexto es el de sonido, se
   resuelven en nativo; todo lo que toca el planificador (despertar a quien espera un mutex) se manda al
   hilo del juego como una «interrupción» más (se entrega en un sondeo con interrupciones encendidas, así
   que es atómico respecto a sus secciones críticas).
3. La interrupción del DSP deja de entregarse al hilo del juego.
4. Exclusión: el mutex de sonido (el juego lo toma en cada llamada a nw4r::snd) y que el manejador AX no
   corra mientras el juego lo tiene. Falta leer nw4r::snd para confirmar que todo acceso del juego al
   estado de voces pasa por ese mutex (los `InstancePool` usan «interrupciones apagadas»: riesgo).
5. Estado global del runtime que hoy supone un solo contexto (MMIO del DSP/AI, `EventScheduler`,
   estadísticas, reverb) → revisar uno a uno.

Si el generador se vuelve a tocar, dos cosas lo harían mucho más seguro: base de la ventana de memoria por
contexto (permite páginas privadas por CPU: las variables del SO en 0x80000000) y un gancho en los cambios de
MSR[EE].

## 5. Qué se implementó (todo con interruptor; mismo binario para A/B)

| Interruptor | Qué hace | Dónde |
|---|---|---|
| `NWII_T_EFBVAR` | una dirección que recibe copias EFB de varios tamaños guarda una textura (y su reducción nativa) por tamaño, en vez de reasignar y borrar en cada copia. La textura que se usa tiene el mismo tamaño y formato que antes y la copia la reescribe entera; el muestreo usa objetos sampler, así que el resultado es el mismo. **tp4: apagado salvo `="1"`. t1: encendido salvo `="0"`** (añadido a `keeps_zero`) | `src/hle/texture_cache_gl.cpp` (`acquire_efb_copy`) + contador `[TCSTAT2]` (reasignaciones por fotograma) |
| `NWII_POLL_EVERY=N` | el sondeo lento (reloj + agenda) cada N sondeos en vez de 16 | `src/hle/ios.cpp` (`process_pending_callbacks_slow`) |
| `NWII_TPROF=1` | perfil con pila invitada (§1); necesita `profile = true` en `debug.toml` | `src/core/tprof.cpp/.h`, `switch_profiler.cpp`, `force_demo.cpp`, `main.cpp` |
| `NWII_T_NORENDER=N` | **solo medir**: dibuja 1 de cada N fotogramas | `src/hle/gx.cpp` (`RenderPacket`) |

Ninguna cabecera que incluya el código generado cambió (`tools/build_t.sh` lo comprueba). Parche completo
respecto al runtime de m2/r7f: `r31t_vs_r7f.patch` (para llevarlo a la compilación con PGO de `r17pgo`: basta
`texture_cache_gl.cpp` y la línea de `keeps_zero` en `switch_platform.cpp`).

## 6. Medidas en la consola (1020 MHz, 720p, GL, lista fija, 400 s; `tools/por_escena_real.py`)

A/B de `NWII_T_EFBVAR` con el mismo binario (tp4): T07 apagado, T06 encendido. T02 = tp2 sin interruptores.

| Escena | render ms/fot: apagado → **encendido** | fot/s dibujados: apagado → **encendido** | fot/s del juego | ms juego/fot real | núcleo 2 |
|---|---|---|---|---|---|
| desierto4j | 33,0 → **13,5** | 28,2 → **34,4** | 31,9 → 34,0 | 20,8 → 20,2 | 19 → 16 % |
| agua_pinguino | 29,3 → **15,5** | 27,9 → **30,9** | 30,6 → 30,8 | 26,6 → 26,1 | 35 → 31 % |
| cielo_paratroopas | 13,7 → 14,3 | 31,0 → 30,8 | 30,8 → 30,8 | 23,4 → 23,5 | 17 → 16 % |
| nubes_plataformas | 10,0 → 10,0 | 32,7 → 33,5 | 32,7 → 33,1 | 19,8 → 19,8 | 15 → 15 % |
| castillo | 24,6 → 24,3 | 33,2 → 31,4 | 53,2 → 53,5 | 15,2 → 14,9 | 75 → 77 % |

Con el arreglo ya no se descarta ningún fotograma en desierto ni agua (dibujados = del juego): lo que se ve
queda limitado por el hilo del juego (20 y 26 ms). Con el PGO de m2 (hilo del juego del desierto a 16-17 ms,
43-47 fot/s) el desierto debería verse a ~45 fot/s en vez de 26: **hay que recompilar el PGO con este
parche** (no se hizo aquí: `r17pgo` es de otro frente). El castillo sigue limitado por render + trabajador
(24 + 17-20 ms por fotograma; el juego va a 53 fot/s).

Verificación de imagen:
- Consola: hojas `capturas.jpg` de T01, T05, T06 y T08 revisadas: limpias. En T06/T08 las capturas 020 y 100
  salen negras: son los fundidos (unos 3 s en negro entre el título y la demo, y entre demos). Comprobado con
  T09 (arreglo) y T10 (base), una captura por segundo: el título se ve hasta el segundo 20, negro en 21-23 y
  desierto desde el 24 **en las dos**, con el mismo brillo medio segundo a segundo. En PC (1,5789x, volcado
  cada 20 fotogramas, 3 680 fotogramas de título → fundido → desierto) el brillo medio de cada volcado es
  idéntico con y sin el arreglo (`r9/render/pc/runs/t31_trans0`, `t31_trans1`).
- PC a la escala de la Switch (1,5789x), `render/test/checks.sh` con t1 (`runs/checks_t1_720p.txt`): title,
  map, walk y lvl11 → 180/180 fotogramas idénticos en cada una, `diff_lines=0`.
- PC, `[TCSTAT2]`: de 2-4,3 reasignaciones por fotograma a 0 (desierto y demos del título).
- A/B por hash de fotogramas (`tools/det_efbvar.sh` → `render/test/det.sh`, título sin demo forzada, 1 167
  fotogramas volcados, `runs/det_efbvar_title.txt`): el arnés **no es del todo determinista** con este binario
  (el control apagado/apagado ya difiere en la pantalla de título y en fotogramas sueltos de la última demo:
  867 iguales); apagado contra encendido da 862 iguales y **los tramos que difieren son los mismos que en el
  control** (ningún tramo distinto propio del cambio; la película y la primera demo salen idénticas en los
  tres). Con la demo del desierto forzada el control diverge desde el título (145 de 353), así que ahí no
  prueba nada. La identidad se apoya en: misma textura (tamaño y formato), reescrita entera por cada copia,
  muestreo con objetos sampler; más lo anterior.

Otras corridas: T01 (tp1, perfil del hilo del juego), T05 (tp3, perfil de los tres hilos), T08 (t1: arreglo
encendido por defecto), T03 (tp2 con `NWII_POLL_EVERY=128`, contra T02): ver §8.

## 7. Cómo rehacerlo

```
cd ~/nsmbw-port/r9/r31t
JOBS=2 ~/nsmbw-port/scripts/lim tools/build_t.sh <nombre> both     # solo compila el runtime y enlaza como m2
# -> bin/nsmbw_<nombre>.nro/.elf (+ _input_test.nro SOLO pruebas), bin/NSMBW_<nombre>[_input_test] (PC)
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm -n -S --defined-only bin/nsmbw_<nombre>.elf > datos/<nombre>.nm

# consola (cola): perfil con pila
cd ~/nsmbw-port/tools/switchctl
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 PULL_PREFIX=tprof DEBUG_EXTRA="$L;NWII_TPROF=\"1\"" ./cola.sh add <nro> <trabajo> 400
# leer
cd ~/nsmbw-port/r9/r31t; B=../../switch/real_logs/bench_<trabajo>_*
python3 tools/tprof.py $B datos/<nombre>.nm fases      # | cruce | linea | esperas | top 60 | arbol 800E4E40 6 2 | hilos
python3 tools/por_escena_real.py ../../switch/real_logs/bench_A ../../switch/real_logs/bench_B   # métrica corregida

# PC: perfil (señal al hilo de la CPU) y A/B determinista de imagen
cd ~/nsmbw-port/r9/render
flock pc/.runlock env NWII_TPROF=1 NWII_TPROF_DIR=<carpeta> NWII_FORCE_DEMO=... NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 \
  EVERY=600 PROFILE=free RES=1 pc/run.sh <bin_input_test> <nombre> 330 mp '0-330:100:20:4'
EXTRA="NWII_FORCE_DEMO=desierto4j NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 NWII_T_EFBVAR=1" RES=1.5789 SCALED=1 \
  flock pc/.runlock test/det.sh <bin_input_test> <nombre> title 150 10     # -> test/runs/<nombre>/hashes.txt
```

Binarios (`bin/`): tp1 = m2 + tprof; tp2 = + `NWII_POLL_EVERY` + `NWII_T_NORENDER`; tp3 = + muestras de los
hilos de render/trabajador; **tp4 = todo lo anterior + `NWII_T_EFBVAR`** (el que hay que usar). Los `.elf`
de cada corrida de consola: `bin/nsmbw_tpN_T0x.elf`.

## 8. Corridas de consola de este frente (`switch/real_logs/`)

| Corrida | Binario | Interruptores | Qué da |
|---|---|---|---|
| `bench_T01_tp1_tprof_1112_0330` | tp1 (`bin/nsmbw_tp1_T01.elf`) | `NWII_TPROF=1` | perfil con pila del hilo del juego (§2) |
| `bench_T05_tp3_tprof_hilos_1112_0339` | tp3 (`bin/nsmbw_tp3_T05.elf`) | `NWII_TPROF=1` | + hilos de render y trabajador por escena (§3) |
| `bench_T02_tp2_base_1112_0348` | tp2 (`bin/nsmbw_tp2_T02.elf`) | — | base (= m2: desierto 20,9 real / 33,1 ms de render) |
| `bench_T07_tp4_base_1112_0405` | tp4 | — | base del A/B de `NWII_T_EFBVAR` |
| `bench_T06_tp4_efbvar_1112_0356` | tp4 (`bin/nsmbw_tp4_T06.elf`) | `NWII_T_EFBVAR=1` | el arreglo (§6) |
| `bench_T08_t1_lista_1112_0414` | **t1** | — (arreglo encendido por defecto) | igual que T06: desierto 13,5 ms de render, 34,4 fot/s dibujados; agua 15,6 ms, 30,9 fot/s |
| `bench_T09_tp4_efbvar_rafaga_1112_0431`, `bench_T10_tp4_base_rafaga_1112_0435` | tp4 | con / sin `NWII_T_EFBVAR=1` | 110 s con una captura por segundo entre los 8 y los 53 s: mismo fundido en las dos; repite el A/B (desierto: render 33,5 → 13,2 ms, 28,4 → 34,3 fot/s dibujados) |
| `bench_T03_tp2_poll128_1112_0439` | tp2 | `NWII_POLL_EVERY=128` | contra T02 (mismo binario): ms juego/fot real desierto 20,9 → **19,9**, agua 26,6 → **25,3**, cielo 23,5 → **22,0**, nubes 19,7 → **18,5**, castillo 15,4 → **13,9** (−1,0 a −1,5 ms; el ruido entre bases es ±0,2). Sondeos lentos 160k/s → 25k/s. Imagen limpia |

Cancelada: T04 (`NWII_T_NORENDER`, solo medir) para no ocupar la consola.

## 9. Qué falta / siguiente paso recomendado

1. **Recompilar el PGO de m2 con `texture_cache_gl.cpp` de aquí** (o enlazar el código PGO con el runtime de
   r31t) y medir con `por_escena_real.py`: es la combinación que debe subir el desierto de ~26 a ~45 fot/s
   vistos. A partir de ahora, comparar siempre con la columna «ms juego/fot REAL» y con los fot/s dibujados.
2. Si t1 se da por buena (más escenas, juego real, no solo demos), instalarla la decide el coordinador; aquí no
   se instaló nada.
3. `NWII_POLL_EVERY=128` gana 1,0-1,5 ms por fotograma en el hilo del juego (una corrida; más de lo que
   pesaba el sondeo en el perfil, 0,75 ms: probablemente también ensucia menos la caché). **Antes de ponerlo
   por defecto**: en T03 la línea de audio marca `out 31,3-31,7 kHz` frente a 31,8-31,9 en la base (misma
   cantidad de cortes, `gap max` igual): parece que los eventos periódicos (AX cada 3 ms) se rearman desde el
   momento en que se atienden y no desde su hora, así que un sondeo más espaciado baja un ~1 % el ritmo del
   audio. Probar 32 y 64, y/o rearmar los eventos con su hora teórica; repetir la corrida para confirmar la
   ganancia.
4. Sonido en otro núcleo (§4.1): es lo único grande que queda por repartir en el hilo del juego (1,9-3,0 ms a
   60 fot/s); proyecto de 1-2 semanas y riesgo alto. Con PGO + sonido fuera, las cuatro escenas de juego bajan
   de 16,7 ms.
5. Castillo: sigue limitado por render (24 ms) + trabajador (17-20 ms); con PGO el juego va a 59 fot/s y se
   dibujan 46.
6. El arnés determinista de PC (`render/test/det.sh`) ya no es reproducible en la pantalla de título con los
   binarios m2: conviene averiguar por qué antes de usarlo como prueba de identidad.

## 10. PAUSA 12-nov ~05:25: m3 (runtime r31t + código G6c3 + PGO) — estado y cómo seguir

### Hecho
- **m3n** (sin PGO) = runtime r31t (arreglo EFB encendido por defecto, como t1) + objetos de G6c3
  (`r30g6c3/c3/{nx_obj,pc_obj}`, su línea de enlace y su orden L). c3 no necesita nada del runtime
  (`g6c_decls.h` va en el export). Se enlaza con `CODE=c3 JOBS=2 ~/nsmbw-port/scripts/lim tools/build_t.sh m3n both`.
  Binarios: `bin/nsmbw_m3n.nro`, `bin/nsmbw_m3n.elf` (= `nsmbw_m3n_U01.elf`), `bin/NSMBW_m3n[_input_test]`.
  - PC `checks.sh` 1x (+PINDEX/DLMEMO): title, map, walk, lvl11 → 180/180 idénticas, `diff_lines=0`, índice y
    memo 0 diferencias, FASTMEM 0 (`runs/checks_m3n_1x.txt`). A 1,5789x: las cuatro 180/180, `diff_lines=0`
    (`runs/checks_m3n_720p.txt`, terminó a las 05:19).
  - Consola `bench_U01_m3n_lista_1112_0506` (capturas limpias) contra t1 (`bench_T08_t1_lista_1112_0414`):

| Escena | t1: ms juego/fot real | **m3n** | t1: fot/s juego / dibujados | **m3n** | render ms/fot (t1 → m3n) |
|---|---|---|---|---|---|
| desierto4j | 20,2 | **19,1** | 34,0 / 34,4 | **36,1 / 36,6** | 13,5 → 13,7 |
| agua_pinguino | 26,0 | **24,8** | 30,9 / 30,9 | **31,8 / 32,0** | 15,6 → 15,6 |
| cielo_paratroopas | 23,4 | **22,3** | 30,8 / 30,8 | 30,9 / 31,0 | 14,4 → 14,5 |
| nubes_plataformas | 19,8 | **18,7** | 32,9 / 33,4 | **35,5 / 35,4** | 10,0 → 10,2 |
| castillo | 14,8 | **13,3** | 55,0 / 32,0 | 57,3 / 33,6 | 24,7 → 23,5 |

  (m2 de referencia, real: 20,7 · 26,4 · 23,2 · 19,5 · 15,0; m2 dibujaba 28-29 fot/s en desierto y agua.)
- **PGO de m3, preparado**: `r9/r17pgo/m3src/` (export = copia de `r30g6c3/c3/export`, runtime = `r31t/nWiiRuntime`)
  y `r9/r17pgo/conf/m3.env`, hechos a mano con los mismos pasos de `pgo_release.sh prep` (para no compilar aquí).
  Están iguales en las dos laptops (`sha256sum -c SHA256SUMS` bien en ambas).
  Ojo: tras compilar la instrumentada se actualizó `m3src/nWiiRuntime/src/hw/hw_dsp.cpp` (interruptor
  `NWII_ADMA_ABS`, apagado por defecto; solo runtime, el perfil es del código generado) y se regeneró
  `SHA256SUMS` (el original: `SHA256SUMS.prep_0452`; anotado en `ORIGEN.txt`).
- **Instrumentada compilada** en la otra laptop (trabajo remoto `pgo_m3_gen`, rc 0, 17 min) y traída:
  `r9/r17pgo/bin/m3_pgogen.nro`, `.elf`, `m3_pgogen_input_test.nro`, `r9/r17pgo/nx/m3_pgogen/`.
- `NWII_ADMA_ABS=1` (nuevo, en `hw_dsp.cpp`): el fin de cada pasada de audio (reloj de AX, 3 ms) se programa
  desde su hora teórica y no desde cuando se atendió. Motivo: hoy se rearma con `schedule_after` desde «ahora»,
  así que el retraso del sondeo se acumula (por eso el audio va a 31,8 kHz en vez de 32, y a 31,3-31,7 con
  `NWII_POLL_EVERY=128`). Binario para probarlo: `bin/nsmbw_t2.nro/.elf` (= t1 + ese interruptor). **Sin medir.**

### No hecho (se quedó en la cola y alguien retiró los trabajos al pausar; no hay nada mío pendiente)
En `tools/switchctl/.cola/` solo quedan las copias `.nro` (V01, V02, W01, W02, W03); los `.job` ya no están.
1. Entrenamiento (2 × 900 s, solo demos forzadas). Lo encola todo `tools/cadena_pgo_m3.sh` (ya no espera: la
   compilación remota terminó), o a mano:
```
cd ~/nsmbw-port/tools/switchctl; C=~/nsmbw-port/r9/r17pgo
G='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_GCOV_DUMP_EVERY="60";NWII_GCOV_DUMP_AT="880";NWII_FORCE_DEMO_LOOP="1"'
PULL="$C/tools/pull_gcda.sh $C/profiles/raw"
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=30 PULL_CMD="$PULL" DEBUG_EXTRA="$G;NWII_FORCE_DEMO=\"desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo,torre3,castillo6\"" ./cola.sh add $C/bin/m3_pgogen.nro V01_m3gen_demosA 900
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=30 PULL_CMD="$PULL" DEBUG_EXTRA="$G;NWII_FORCE_DEMO=\"nieve,casa_oscura,lava,jungla,playa_yoshi,playa_hielo,desierto_helices,pradera,cielo_hoja\"" ./cola.sh add $C/bin/m3_pgogen.nro V02_m3gen_demosB 900
# arrancar la cola: comando systemd-run de status/PAUSA_1110.md
```
   Al acabar cada una: `ls r9/r17pgo/profiles/raw/` (dos carpetas nuevas `261112_HHMMSS`); comprobar que cada
   una tiene `dumps.txt` y el mismo número de `.gcda` (`ls <carpeta> | grep -c gcda`).
2. PGO final en la otra laptop (~25 min con JOBS=11; mirar antes `scripts/remoto.sh estado`):
```
cd ~/nsmbw-port
scripts/remoto.sh push r9/r17pgo/profiles/raw/<A> r9/r17pgo/profiles/raw/<B> r9/r17pgo/nx/m3_pgogen
scripts/remoto.sh run pgo_m3_fin 'cd r9/r17pgo && JOBS=11 ./pgo_release.sh finish m3 profiles/raw/<A> profiles/raw/<B>'
scripts/remoto.sh log pgo_m3_fin      # hasta que acabe (rc 0 en scripts/remoto.sh estado)
rsync -a nsmbw-build:nsmbw-port/r9/r17pgo/bin/m3_pgo_m3_all.nro nsmbw-build:nsmbw-port/r9/r17pgo/bin/m3_pgo_m3_all.elf nsmbw-build:nsmbw-port/r9/r17pgo/bin/m3_pgo_m3_all_input_test.nro r9/r17pgo/bin/
```
3. Medir m3+PGO contra t1 (corridas seguidas) y leer:
```
cd ~/nsmbw-port/tools/switchctl
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L" ./cola.sh add ~/nsmbw-port/r9/r17pgo/bin/m3_pgo_m3_all.nro X01_m3pgo_lista 400
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L" ./cola.sh add ~/nsmbw-port/r9/r31t/bin/nsmbw_t1.nro X02_t1_control 400
cd ~/nsmbw-port/switch/real_logs && python3 ../../r9/r31t/tools/por_escena_real.py bench_T02_tp2_base_* bench_X02_t1_control_* bench_U01_m3n_lista_* bench_X01_m3pgo_lista_*
# y mirar bench_X01_*/capturas.jpg
```
4. Barrido del sondeo (sobre t2; control = la corrida de t1 de arriba). Mirar en `nwii.log` las líneas `[NX] game`:
   `out NN.N kHz` (debe quedar en ~31,8-32,0) y `ax thread NNN lists/s` (333 = exacto), además de ms juego/fot:
```
T=~/nsmbw-port/r9/r31t/bin/nsmbw_t2.nro
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L;NWII_POLL_EVERY=\"32\"" ./cola.sh add $T W01_t2_poll32 400
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L;NWII_POLL_EVERY=\"64\"" ./cola.sh add $T W02_t2_poll64 400
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L;NWII_POLL_EVERY=\"128\";NWII_ADMA_ABS=\"1\"" ./cola.sh add $T W03_t2_poll128_adma 400
grep -a "\[NX\] game" ../../switch/real_logs/bench_W0*/nwii.log | grep -o "bench_W0[^/]*\|out [0-9.]* kHz\|ax thread [0-9]* lists/s" | paste - - - | sort | uniq -c | sort -rn | head -30
```
   Ya medido: `NWII_POLL_EVERY=128` sin `ADMA_ABS` gana 1,0-1,5 ms/fot pero baja el audio a 31,3-31,7 kHz (§9.3).
   Si con `ADMA_ABS=1` el audio queda en 32,0 kHz y la ganancia se mantiene, 128 + ADMA_ABS serían candidatos a
   valor por defecto (cambiar los dos valores por defecto en `ios.cpp` y `hw_dsp.cpp`; solo runtime).
