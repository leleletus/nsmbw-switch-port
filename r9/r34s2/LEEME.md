# r34s2: frente S2 — el sonido fuera del hilo del juego (14-nov-2026)

Base: runtime `r9/r31t/nWiiRuntime` (copiado a `nWiiRuntime/`) + código generado G6c3 (`r9/r30g6c3/c3`).
Interruptor: **`NWII_S2=1`** (apagado por defecto; mismo binario para A/B). Solo SMNE01 (USA): las direcciones
del DOL están fijadas y se comprueban al arrancar. Sin commits; no se tocó ninguna otra carpeta de `r9/`.

> **ESTADO 15-nov 00:30:** imagen negra con S2 RESUELTA (§9: parche viejo del DEC en ios.cpp). Variante verificada: **s2g** (§10).

## 1. Qué se mueve

En el hilo del juego el sonido cuesta por tiempo (r31t §2): hilo `nw4r::snd` 1,4-3,4 ms + interrupción AX
1,5-2,0 ms + cambios de hilo 0,25-0,5 ms por fotograma a ~31 fot/s. Con `NWII_S2=1` un **segundo contexto
invitado** («CPU de sonido»: otro `CPUContext` con la MISMA memoria invitada, ejecutado por un hilo anfitrión
propio, núcleo 2 por defecto) ejecuta:

1. las interrupciones del DSP que son de sonido: **AID** (fin de pasada del DMA de audio, cada 3 ms → manejador
   de AX: `__AXSyncPBs`, aux/reverb, callbacks de nw4r, lista de órdenes al DSP) y **DSP** (correos del
   microcódigo AX), con los manejadores del propio juego (`__AIDHandler` 0x8019F370, `__DSPHandler` 0x801D5990,
   leídos de la tabla 0x80003040);
2. el cuerpo del hilo **`nw4r::snd::detail::SoundThread`** (bucle de `SoundThreadProc`, entra por 0x8027B990:
   `OSReceiveMessage` → `FrameProcess` 0x8027BAF0).

El hilo invitado SoundThread de verdad sigue existiendo, dormido en su cola de mensajes (nadie le manda nada):
su `OSThread` sirve de «dueño» de los mutex que toma la CPU de sonido y su pila (la parte libre por debajo de
su marco dormido) es la pila invitada de la CPU de sonido. La mezcla (HLE de AX) sigue en su hilo de siempre.

Es la variante (b) del encargo (AX + SoundThread). La (a) «solo la interrupción AX» no es más simple: el
SoundThread toca el estado de voces de AX dentro de secciones `OSDisableInterrupts`, así que haría falta la
misma exclusión y habría más contienda. Lo que NO se mueve: el hilo de tareas de nw4r (lecturas de DVD de los
streams), los callbacks de DVD/alarmas (altavoz del mando) y todo lo que el juego hace al llamar a nw4r::snd.

## 2. Estado compartido y sincronización (razonamiento de corrección)

En la consola real, respecto al hilo del juego: (i) un manejador de interrupción es atómico frente a toda
sección con interrupciones apagadas; (ii) el SoundThread (prioridad 4) entra entre dos instrucciones
cualesquiera con EE=1 y solo lo frenan los mutex. El SO invitado es de un procesador: «interrupciones
apagadas» es su único candado. Con dos CPU hay que reponer esa exclusión:

**S — candado anfitrión que sustituye a «interrupciones apagadas» entre las dos CPU** (`s2.cpp`).
- CPU de sonido: tiene S **siempre que su MSR[EE]=0**: todo manejador AID/DSP entero y toda sección
  `OSDisableInterrupts` del SoundThread. Invariante simple: `S tomado ⟺ EE=0`.
- CPU del juego: toma S (a) al abrir una sección `OSDisableInterrupts` **cuyo llamador no es SDK ajeno al
  sonido** (se clasifica por la dirección de retorno: todo menos OS/GX/DVD/MEM/WPAD…, nw4r::ut/math/g3d/ef/lyt y
  MSL; es decir AX/AXFX/AI/DSP, nw4r::snd, Snd* del juego, HBM, EGG y código del juego/REL), y (b) durante
  **todo** manejador de interrupción o callback que el runtime entrega (VI, alarmas, DVD, IPC…). Lo suelta
  cuando EE vuelve a 1. Si el hilo invitado se duerme con la sección abierta (`OSSleepThread` dentro de
  `AutoInterruptLock`, p. ej. `TaskManager::WaitTask`) lo suelta en el cambio de hilo y lo retoma cuando ese
  hilo se reanuda (lista `stash` por `OSThread`), igual que en la consola corren otros hilos mientras duerme.
- Por qué no «toda sección EE=0 del juego»: el hilo del juego abre secciones del SO (mutex del montículo,
  etc.) cada pocas decenas de µs y un manejador AX dura ~200 µs: chocaría casi siempre y esperaría el
  manejador entero (no se ganaría nada). Las secciones del SO no tocan estado de sonido y la CPU de sonido
  nunca toca estructuras del SO (ver M), así que no necesitan excluirse.
- Los ganchos: cada `mtmsr`/`rfi` del código generado (323 sitios, incluidas las 97+154 copias pegadas de
  `OSDisableInterrupts`/`OSRestoreInterrupts`) llama a `nwii_s2_ee(ctx, ee_antes, LR, sitio)` (pasada
  `tools/apply_s2.py`, §4); las entradas de interrupción y la carga de contexto del runtime llaman a
  `s2::game_irq_enter` / `s2::game_sync` (`ios.cpp`).

**M — mutex (el de `SoundThread`, +852 del objeto) y despertar hilos.** La CPU de sonido **no ejecuta** las
funciones del SO que tocan el planificador o el «hilo actual»: sus entradas tienen un gancho
(`nwii_s2_os`) que, en el contexto de sonido, las resuelve en nativo:
- `OSLockMutex`/`OSUnlockMutex`/`OSTryLockMutex`: petición síncrona a la CPU del juego, que la atiende **en un
  sondeo con EE=1 y a nivel de hilo** (atómico respecto a todas las secciones críticas del SO invitado, que
  es lo que dijo T) y hace en nativo lo mismo que el SDK sobre la estructura del mutex a nombre del `OSThread`
  SoundThread (cuenta, lista de mutex del hilo). Si el mutex es de un hilo del juego, la CPU de sonido espera
  (atendiendo sus interrupciones, como en la consola) y reintenta. Si un hilo del juego se bloquea en un mutex
  que tiene el sonido, se duerme en la cola del mutex con el código del SDK de siempre y al soltarlo la CPU del
  juego ejecuta el `OSWakeupThread` invitado como un callback más (`queue_callback`, con replanificación).
  Si el hilo del juego duerme en el bucle ocioso, la CPU de sonido atiende la petición ella misma (`g_park`).
- `OSWakeupThread` (TaskManager → hilo de tareas): igual, petición → callback en la CPU del juego.
- `OSSendMessage`/`OSReceiveMessage` sobre la cola del SoundThread: cola nativa de 4 (misma capacidad; si se
  llena se pierde el mensaje, como en la consola). La espera de `OSReceiveMessage` es el punto de reposo.
- `OSSleepThread` (los 3 que hay en nw4r::snd vuelven a mirar su condición en bucle): siesta de 150 µs
  atendiendo interrupciones = despertar espurio. `OSSetCurrentContext` (y sus copias pegadas en los
  manejadores): se salta (el «contexto actual» 0x800000D4 es de la CPU del juego). `OSGetCurrentThread`: el
  `OSThread` SoundThread. `OSYieldThread`: nada. Otras 12 funciones (alarmas, hilos, `DVDReadPrio`…) llevan
  **trampa**: si la CPU de sonido entra, se cuenta y se registra (`trampas` en la línea `[S2]`).

**Lo que queda sin candado (como diseño de nw4r, pero ahora con paralelismo real):** el código del juego que
llama a nw4r::snd pasa por el mutex de SoundThread (88 funciones lo toman) o por `AutoInterruptLock` (103);
los campos sueltos que el juego escribe sin candado (volumen, tono…) son escrituras alineadas de 32 bits
(atómicas). El riesgo residual es código del SoundThread que lea dos veces un dato que el juego cambia sin
candado: en la consola el juego no puede correr «en medio» del SoundThread; aquí sí. No se ha encontrado
ningún caso, pero no está demostrado (riesgo abierto nº 1).

**DSP/AI (`hw_dsp.cpp`).** Con la CPU de sonido en marcha: los bits AID (0x08) y DSP (0x80) de DSPCSR no
levantan la interrupción PI del hilo del juego (solo ARAM 0x20); cada CPU ve en DSPCSR solo sus bits; los
eventos de tiempo (fin de pasada del DMA, correos SYNC/YIELD) van a una agenda propia del hilo de sonido,
con el fin de pasada programado **desde su hora teórica** (32 kHz y 333,3 listas/s justos); todo el estado
del archivo va bajo un candado recursivo por hilo.

**Toma y devolución.** El gancho de `OSReceiveMessage` ve al SoundThread invitado entrar a dormir (LR =
0x8027B9A0). En un sondeo del hilo del juego con EE=1, cuando ese hilo está WAITING en su cola, la cola vacía,
el mutex libre y el DSP en reposo (sin correos en vuelo ni interrupciones pendientes), se pasa el evento del
DMA a la agenda de sonido y arranca la CPU de sonido. Si el juego manda un mensaje a esa cola (apagado del
sonido: `SoundThread::Shutdown`), se hace la devolución: la CPU de sonido acaba, se aparca en su punto de
reposo, el DSP vuelve al hilo del juego y el hilo invitado de verdad recibe el mensaje.

**Otras piezas:** `fm_alloc` no reserva memoria para el segundo contexto (comparte los bloques);
`governor_step` no actúa para el contexto de sonido; `idle_wait` duerme con una espera interrumpible.

## 3. Criterios de aceptación y cómo se comprueban

(i) lógica del juego: `render/test/checks.sh` (title/map/walk/lvl11) con `NWII_S2=1` idéntico; corridas largas
de PC sin cuelgues, con `NWII_S2_CHECK=1` (asserts de propiedad) a 0. (ii) audio: `NWII_AUDIODUMP=<wav>` +
`tools/wavcmp.py` contra la base (duración, espectro por ventanas, huecos, clics); línea `[S2]`: AID ~333/s,
tramas ~333/s, 0 mensajes perdidos. (iii) consola: lista de demos 15 min. (iv) carreras: `NWII_S2_CHECK=1`
y compilación con ThreadSanitizer del runtime de PC (§7).

## 4. Pasada sobre el código generado (`tools/apply_s2.py`, NO toca el generador)

Copia a `s2/export/` solo los 14 `output_N.cpp` que cambian (1, 21, 24-28, 30, 36-38, 41, 44, 45):
- **E1** (323 sitios): tras cada `ctx.msr = …` de `mtmsr`/`rfi`: `if (nwii_s2_on) nwii_s2_ee(ctx, ee_was, LR, sitio);`
- **E2** (22 funciones del SO): en la etiqueta de entrada: `if (nwii_s2_on && nwii_s2_os(ctx, F, G5lr)) return G6RETF(G5ic, G5lr);`
- **E3** (23 copias pegadas `[g6i]` de `OSSetCurrentContext` y `OSGetCurrentThread`): se saltan en el contexto de sonido.
Con `nwii_s2_on == false` (sin `NWII_S2`) cada gancho es una carga y un salto. Objetos: `s2/{nx_obj,pc_obj}`
(compilados en la otra laptop con las plantillas de G6c3: `tools/rbuild_s2.sh`). **Para el PGO final** hay que
pasar `apply_s2.py` sobre la exportación antes de compilar (o enlazar estos 14 objetos sin PGO: no recomendado).

## 5. Archivos

- `nWiiRuntime/src/core/s2.cpp`, `s2.h`: todo el mecanismo. Cambios pequeños en `src/hle/ios.cpp` (sondeo,
  entrada de interrupciones, carga de contexto), `src/hw/hw_dsp.cpp` (rutas del DSP), `src/core/os_hle_stubs.cpp`
  (gobernador, siesta), `src/core/fastmem_impl.inc` (alias), `src/core/main.cpp`, `switch_platform.cpp` (línea
  `[S2]`), `CMakeLists.txt`. Ninguna cabecera que incluya el código generado cambió.
- `tools/apply_s2.py` (pasada), `tools/rbuild_s2.sh` (objetos, otra laptop), `tools/build_s2.sh <nombre> [pc|nx|both]`
  (= `build_t.sh` con `CODE=c3` y los 14 objetos de S2), `tools/callgraph.py`, `cg_query.py`, `fn.py` (lectura
  del código generado), `tools/wavcmp.py` (comparar audio).
- Interruptores: `NWII_S2=1`; `NWII_S2_CHECK=1` (asserts); `NWII_S2_CORE` (2), `NWII_S2_PRIO` (0x2C) en la Switch.
- Línea de registro `[S2] …` junto a cada `[NX] game` / `[FPS]`: interrupciones y tramas por segundo, ms/s de
  la CPU de sonido, esperas de S en cada lado, peticiones de mutex, trampas, asserts.

## 6. Cómo rehacerlo

```
cd ~/nsmbw-port/r9/r34s2
python3 tools/apply_s2.py ../r30g6c3/c3/export s2/export && cp ../r30g6c3/c3/export/{g6c_decls.h,functions.h} s2/export/
cd ~/nsmbw-port && scripts/remoto.sh push r9/r34s2/tools r9/r34s2/s2/export
scripts/remoto.sh run s2_objs 'JOBS=8 r9/r34s2/tools/rbuild_s2.sh both'      # ~5 min
scripts/remoto.sh pull r9/r34s2/s2/nx_obj r9/r34s2/s2/pc_obj
cd r9/r34s2 && JOBS=4 ~/nsmbw-port/scripts/lim tools/build_s2.sh <nombre> both   # runtime + enlace
```

## 7. Verificación — ver §8

## 8. Estado y números

### 14-nov 22:05 — en curso
- **Funciona en PC** (binario s2b: AX + SoundThread + el «hilo de sonido del juego» 0x80174560 en la CPU de
  sonido). Línea `[S2]`: AID 332/s, DSP 664/s, 664 vueltas de hilo/s, 0 mensajes perdidos, 0 asserts;
  el hilo del juego en PC baja de 15 % a 6 % en el título. Audio (60 s, `tools/wavq.py` contra la base):
  0 huecos, mismos clics (14/14), correlación de envolvente 0,995, espectral mediana 1,000 (mín. 0,979).
- `checks.sh` 1x con `NWII_S2=1 NWII_S2_CHECK=1` (binario s2a): title, map, walk, lvl11 → 220/220, `diff_lines=0`
  (`runs/checks_s2a_on_1x.txt`).
- Hallazgos por el camino (ya arreglados): (1) `run_game` escribe su historial de pc al salir → la CPU de sonido
  usa un despachador propio (`g_disp_table`); (2) hay **copias del cuerpo de `OSWakeupThread`** dentro de 7
  funciones que saltan a ella con `b` (entre ellas la de AX 0x801A1CC0, que despierta a quien espera una trama):
  sin gancho, la CPU de sonido ejecutó el `OSWakeupThread` invitado (trampa `SelectThread` vista una vez en la
  película del título) → gancho **E4** en la pasada; (3) un tercer hilo movido por AX (0x80174560, callback AX
  0x801744F0, 333 mensajes/s) → también va en la CPU de sonido («fibra» 2; `NWII_S2_F2=0` lo deja en el juego);
  (4) la entrada de excepciones de fastmem de la Switch usaba un área y una pila únicas → candado de giro.
- ThreadSanitizer: `TSAN=1 tools/build_s2.sh <n> pc`. La primera prueba murió en el `longjmp` de la entrega de
  interrupciones (el `setjmp` está en código sin instrumentar); arreglo preparado en `ios.cpp` (`NWII_LONGJMP`).
- Consola: en cola S01 (s2a apagado), S02 (encendido), S03 (encendido + tprof), S04 (apagado + tprof), S05
  (apagado). OJO: los NRO de S02-S05 son **s2b** aunque el nombre del trabajo diga s2a (se reemplazó la copia).
- Siguiente: s2c (= s2b + E4) → `tools/pc_verify.sh s2c` (checks, lista de demos con/sin, CPU tipo Switch, TSan)
  → consola.

### 14-nov 23:00 — RELEVO (otro agente; el anterior se cortó por cuota). Imagen negra con S2
- **Consola S02/S03 = binario s2b** (no s2a: ver md5; S01 = s2a). Modelos 3D negros desde el título.
- **Se reproduce en PC, determinista**: con la fibra 2 («hilo de sonido del juego» 0x80174560) en la CPU de
  sonido el juego hace 0-8 copias EFB/fot en vez de 20-28 (texturas de luz de EGG::LightTextureManager) →
  modelos negros (`render/pc/runs/chk_s2d_on_title_1x/t_004500.png`). Con `NWII_S2_F2=0` (solo SoundThread
  nw4r + AX) sale limpio (`runs/bis_f2off`, `bis_s2a_rt`). Resumen rápido: `tools/bis.sh <bin> <nombre> env...`.
- `NWII_S2_F2DBG=1` (fibra 2 tomada pero su cuerpo NO corre) → igual de negro: no es lo que ejecuta la fibra,
  es que el hilo invitado deja de despertarse en la CPU del juego. Hipótesis en prueba: esos despertares
  (333/s, prioridad mayor que el hilo principal) provocaban un cambio de hilo que da turno a otro hilo de la
  misma prioridad que el principal (round-robin de SelectThread); sin ellos ese hilo se muere de hambre.
  Prueba: `NWII_S2_F2DBG=3` (= OSYieldThread en la CPU del juego por cada mensaje) y volcado de hilos
  (`[S2] hilos:` cada 10 s con F2DBG≥1).

## 9. Imagen negra con S2: CAUSA Y ARREGLO (14-nov 23:15, relevo)
- **Causa**: un parche viejo del runtime en `ios.cpp` `ppc_tail` («si el DEC dispara 64 veces seguidas en el
  mismo pc, recorrer 512 palabras por debajo de r13 y poner la hora en +8/+0xC de todo lo que parezca un
  OSAlarm»). Sin S2 nunca salta: las ~1000 interrupciones/s de AX/DSP en la CPU del juego rompen la racha
  (y cada retorno de callback reinicia el DEC). Con S2 la CPU del juego se queda en el bucle ocioso del SO y
  las alarmas disparan el DEC 64 veces seguidas en el mismo pc → el parche escribe en **0x8042f65C/60** (lo
  toma por alarma: «handler=0x80000000») → datos del juego corrompidos → EGG::LightTextureManager deja de
  dibujar/copiar las texturas de luz (0-8 copias EFB/fot en vez de 20-28) → modelos 3D negros.
  Evidencia: la línea `[HLE DEC] force-expired alarm 0x8042f654 handler=0x80000000` sale en TODAS las corridas
  rotas (PC `bis_on`, `dbg3_on`; consola `bench_S02`, `bench_S03`) y en NINGUNA limpia (`bis_off`, `s2_t2`,
  `dbg3_f2off`, consola S01, X02). s2a (sin fibra 2) se salvaba porque los despertares del hilo de sonido del
  juego en la CPU del juego (333/s) rompían la racha. Pruebas (`NWII_S2_F2DBG`, solo depuración):
  1 = fibra 2 tomada sin ejecutar → negro; 2 = con EE=0 → negro; 3 = OSYieldThread por mensaje en la CPU del
  juego → limpio; 4 = solo reiniciar el DEC → limpio; 5 = callback inocuo → limpio; 9 = solo volcados
  (`[S2] actual(muestras)` histograma de hilos, `[S2] hilos:` lista). Todo en `runs/bis_serie2.txt`.
- **Arreglo** (`ios.cpp`): ese parche no se aplica con S2 encendido (`&& !s2::g_enabled`); sin S2 nada cambia.
- **s2g** = s2f + ese arreglo (+ modos de depuración inertes). PC: título limpio con la fibra 2 (`runs/s2g_on`,
  `s2g_on_virt`: 28/48 copias EFB, 0 forzadas). Binarios: `bin/nsmbw_s2g.nro/.elf`, `bin/NSMBW_s2g[_input_test|_tsan]`.
- En cola de consola: S08_s2g_off, S09_s2g_on, S10_s2g_on_tprof, S11_s2g_off_tprof (detrás de G02-G04 ajenos).
  Cadena de PC: `tools/pc_verify.sh s2g` (lanzada 23:16; marca `runs/pc_verify_s2g.done`).
- PC s2g, `checks.sh` 1x con `NWII_S2=1 NWII_S2_CHECK=1`: title 200/200, map/walk/lvl11 220/220, `diff_lines=0`,
  0 trampas, 0 asserts (`runs/checks_s2g_on_1x.txt`). OJO: checks.sh NO detecta la imagen negra (s2d también
  daba 220/220). Comparación de imagen nueva: `tools/cmp_png.py <ref> <prueba>` (capturas t_*.png del mismo
  fotograma; % píxeles distintos y % negros). s2g contra m3n (`chk_t31_m3n_*`): title 2,6 %, map 2,0 %, walk 0,0 %,
  lvl11 2,8 % como máximo, sin aumento de negro (s2d daba 29-67 % y 25-47 % de negro). Lo que difiere son
  animaciones que van al ritmo de la música (flores, bloques «?», hierba: mismo fotograma, otra fase); s2a
  contra m3n da lo mismo (2,5 %).

### 15-nov 00:05 — consola s2g (mismo NRO, corridas contiguas, lista de 5 demos, 400 s)
- `bench_S08_s2g_off_1114_2343`: IMAGEN OK (2/38 raras, normales de transición). `bench_S09_s2g_on_1114_2352`:
  **IMAGEN OK (1/38)**, `capturas.jpg` mirada: modelos con luz, personajes del título visibles, misma secuencia
  que apagado. 0 `force-expired` en los dos. Línea [S2]: AID 332/s, DSP 664/s, 0 trampas, 0 asserts; 116
  mensajes perdidos en 400 s, en ráfagas (cargas de escena: el SoundThread espera el mutex que el juego tiene
  al cargar; en la consola real la cola también se llena entonces). Núcleo 2 (sonido): irq ~145 + hilo ~85 ms/s.
- ms juego/fot REAL (`por_escena_real.py`), apagado → encendido (fot/s juego):
  desierto4j 19,4 → **14,4** (35,5 → 39,7) · agua_pinguino 25,0 → **18,7** (31,6 → 32,9) ·
  cielo_paratroopas 22,2 → **17,5** (30,9 → 30,9) · nubes_plataformas 18,8 → **14,4** (35,6 → 41,5) ·
  castillo 13,9 → **8,0** (56,9 → 58,9).
- Perfil (NWII_TPROF=1, `tprof.py … fases`, mismo NRO): apagado `bench_S11_s2g_off_tprof_1115_0010` tiene en el
  hilo del juego S1 «hilo nw4r::snd» 1,4-3,1 ms/fot + S2 «interrupción AX» 1,5-1,9 ms/fot y «otro hilo» 7-12 %;
  encendido `bench_S10_s2g_on_tprof_1115_0001`: las filas S1/S2 desaparecen y «otro hilo» baja a 0,2-1,5 %.
  ms ocupados/fot (tprof): 18,5→14,3 · 24,0→18,2 · 21,1→17,0 · 17,8→14,2 · 13,8→7,8. Ambos IMAGEN OK.
  `por_escena_real` de S10/S11 repite S08/S09 con ±0,2 ms.
- **Proyección a 60 fot/s** (el sonido cuesta por segundo: ahorro/s = (apagado−encendido)·fot/s apagado; a 60
  fot/s el apagado pagaría ese ahorro/60 por fotograma; el encendido queda ≈ su valor medido):
  desierto4j 17,4 → **14,4** · agua_pinguino 22,0 → **18,7** · cielo_paratroopas 19,9 → **17,5** ·
  nubes_plataformas 17,0 → **14,4** · castillo 13,6 → **8,0** ms/fot. Bajo 16,7: desierto, nubes, castillo;
  agua y cielo siguen por encima (≈2 y ≈1 ms; PGO de m3 debería dar ~1-1,5 ms más).
- Audio PC: título 60 s (tiempo real) encendido contra apagado: correlación de envolvente 0,999, espectral
  mediana 1,000, 0 huecos, mismos clics (15 vs 14). CPU «tipo Switch» (`PROFILE=sw`, 200 s): 0 huecos,
  espectral mediana 0,993; el encendido produce ~3 % menos tramas AX en ese perfil (12452 vs 12857 en 200 s);
  en la consola la tasa es igual: [NX] «ax thread» media 326,9 (on) vs 326 (off) listas/s.
- ThreadSanitizer: la primera vez murió en el `longjmp` de la fibra («can't find longjmp buf»): arreglado en
  s2.cpp (macros `S2_NO_TSAN`/`s2_longjmp`, sin efecto fuera de TSan). 150 s completos (`runs/s2g_tsan2`,
  `runs/tsan2_s2g.260651`): 56 avisos; casi todos del render GL/IOS (ya existían, no son de S2). De S2: (a)
  `fiber_prepare` escribe `f.n/head` mientras el bucle del hilo de sonido lee `f.n` de una fibra aún no tomada
  (inocuo: lee `hosted` después; arreglo trivial pendiente: comprobar `hosted` antes que `n` en
  `snd_thread_main`), (b) `request` lee `rq.result` vs `service` (protegido por `g_req` acquire/release;
  probablemente falso positivo por el longjmp real; vigilar).
- Audio PC, lista de demos 330 s: encendido vs apagado envolvente 0,846, espectral mediana 0,969, 0 huecos;
  pero **apagado vs apagado** (segunda corrida, `runs/wav_s2g_off_vs_off2.txt`) da 0,714 / 0,641: en corridas
  largas manda la deriva de tiempos de las demos, no S2. Encendido queda dentro del ruido.

## 10. ENTREGA (15-nov 00:30): variante **s2g** verificada
- NRO/ELF: `r9/r34s2/bin/nsmbw_s2g.nro`, `bin/nsmbw_s2g.elf` (= `nsmbw_s2g_S08.elf`, nm en `datos/s2g.nm`);
  PC: `bin/NSMBW_s2g`, `_input_test`, `_tsan`. Interruptor `NWII_S2=1` (apagado por defecto).
- Cambios respecto a r31t: los de §5 + arreglo de §9 en `src/hle/ios.cpp` (`&& !s2::g_enabled`). Los modos
  `NWII_S2_F2DBG` (1-5, 9) son de depuración, inertes sin la variable.
- Criterios: imagen consola OK encendido/apagado (S08-S11), PC `checks.sh` 220/220 + capturas iguales a m3n
  (≤2,8 %, animaciones al ritmo de la música), audio dentro del ruido, 0 trampas/asserts, sonido fuera del hilo
  del juego en el perfil.
- **Para el PGO final**: pasar `tools/apply_s2.py` sobre la exportación antes de compilar (§4) y usar este runtime.
- Pendiente/riesgos: (1) el parche viejo de `ios.cpp` también puede corromper memoria SIN S2 si alguna vez
  salta (nunca visto en los registros revisados): convendría quitarlo del todo tras mirar para qué servía;
  (2) mensajes perdidos del SoundThread en cargas (116 en 400 s en consola); (3) riesgo nº 1 de §2 (lecturas
  dobles sin candado) sigue sin demostrar; (4) orden `hosted`/`n` (TSan, inocuo); (5) no se ha escuchado el
  audio de la consola (solo tasas y WAV de PC).
