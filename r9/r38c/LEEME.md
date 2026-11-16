# r38c: frente C — reparto de núcleos de la CPU de sonido (S2) y cuelgues de m5 — 15-nov-2026

Base: runtime de m5 (`r9/r35m4/nWiiRuntime_m5`, copiado a `nWiiRuntime/`). Código generado y objetos PGO: los de m5
(no se rehace el PGO: el runtime va sin perfil). No se edita `r9/r35m4`. Sin commits.

## Herramientas
- `tools/hilos.py <bench>...`: por escena (y fiesta del castillo aparte) % de CPU por hilo (ticks del núcleo), línea
  [S2] (AID/s, irq/hilo ms/s, esperas de S de cada lado), listas AX, kHz de salida, underruns, ventanas con AID < 320.
- `tools/build_c38.sh <n> pc`: binario de PC (runtime r38c + objetos m4 de r35m4) → `bin/NSMBW_<n>[_input_test]`.
- `tools/nx_c38_remote.sh <n>` (otra laptop): NRO PGO = objetos PGO de m5 (`r17pgo/nx/m5_pgo`) + runtime r38c,
  recompilando solo las fuentes cambiadas y las que incluyen `s2.h`/`switch_platform.h` (órdenes exactas de
  `ninja -t commands`) → `bin/<n>_pgo.{nro,elf}`. No toca `m5src` ni `nx/m5_pgo`.
- `tools/pcrun.sh <bin> <nombre> <s> [env]`: corrida de PC sin volcado de audio. `tools/pc_checks_c38.sh <n>`.
- **PC con prioridades de Horizon**: `NWII_C_PCSCHED=1` fija render/juego/trabajador/CPU de sonido a 3 CPUs del PC
  (`NWII_C_PCSCHED_CPUS`, por defecto 2,3,4) con SCHED_FIFO y las prioridades de la Switch (respeta `NWII_S2_CORE`,
  `NWII_S2_PRIO`, `NWII_RENDER_WORKER_*`). Así un bloqueo por prioridades se reproduce en PC. Válvula: el vigilante,
  al segundo volcado, devuelve los hilos a SCHED_OTHER (si el programa sigue, era un bloqueo por prioridades).
- Pruebas de estrés (solo PC/pruebas): `NWII_C_STRESS_FIFO_US=<us>` (el trabajador retiene el candado del FIFO de GX),
  `NWII_C_STRESS_SFIFO=1` (el juego toca ese candado con S tomado en cada interrupción).

## Interruptores nuevos (runtime r38c)
- `NWII_C_SPIN_OFF=1`: esperas activas como m5 (giro + `sched_yield`). Por defecto (arreglo): girar poco y DORMIR.
- `NWII_C_VIGIA_OFF=1`: sin vigilante de cuelgues (por defecto encendido).

## Bitácora
- 11:30 Medido con los registros existentes (Z07, Z10-Z12, Z21): ver §1.
- 11:35 **Encargo nuevo prioritario (coordinador): cuelgue de m5 en Z22** (carga de agua_pinguino). Ver §2.
- 11:50 Reproducido en PC con prioridades de Horizon + estrés: se cuelga al arrancar con las esperas de m5
  (`runs/pc_old_stress2`: vigilante: S lo tiene el juego en una interrupción, la CPU de sonido espera S desde 5,8 s,
  candado del FIFO tomado; al deshacer las prioridades el juego SIGUE: bloqueo por prioridades). Mismo estrés con el
  arreglo: 150 s sin cuelgue, AID 333/s (`runs/pc_new_stress2`). Disposición de Z20 en PC (S2 núcleo 0 / 0x2B): 90 s
  sin cuelgue con y sin arreglo (`runs/pc_z20_*`).

- 12:00 **c1** = m5 + arreglo de esperas + vigilante (+ PCSCHED y estrés, inertes sin variables). PC checks 1x con S2
  por defecto y `NWII_S2_CHECK=1`: title/map/walk/lvl11 220/220, `diff_lines=0`, 0 trampas/asserts/perdidos, 0 avisos
  del vigilante. cmp_png contra m5: map 2,0 %, lvl11 2,3 %, title ≤2,5 % salvo t_006300 (otra demo de atracción,
  conocido), walk 59 % desde t_004500 = cámara del mapa desplazada (conocido desde s2a; capturas miradas: misma
  escena). NRO `bin/c1_pgo.nro` (sha256 61cbe268…83c9; fastmem 0 mal formadas). Consola: C01 (desierto↔agua 400 s),
  C02 (lista fija), C03 (desierto↔agua + estrés, arreglo). C04 (estrés con esperas de m5 = cuelgue provocado) RETIRADO
  de la cola (`.job.retirado`): si colgara al arrancar podría dejar debug.toml sin restaurar y parar la cola de todos.
- 12:20 **c2** = c1 + prioridad dinámica (`NWII_C_PRIO_HI`, `NWII_C_LATE_US`) + recuperación de pasadas del DMA
  (`NWII_C_CATCHUP`, `NWII_C_CATCHUP_GAP_US`) + línea `[C38]`. PC con PCSCHED (trabajador y sonido en un núcleo LP-E,
  sonido a 0x2E): sin cambios AID con bajones (305-329); con herencia la espera máx. de S del juego baja de 1 172 a
  150 µs (subidas juego-S 9/s, juego-mutex 5-9/s, 1,5-2,4 ms/s en alta); en PC el sonido casi nunca va > 3 ms tarde
  (el PC es mucho más rápido: no reproduce la fiesta). Sin cuelgues. NRO `bin/c2_pgo.nro` (sha256 b035ec70…a54a).
  Consola (detrás de V01-V06): C05 c2 control, C06 dyn 0x2E/0x2C tarde 3 ms + catchup 8, C07 igual con 6 ms,
  C08 sonido 0x2D (= trabajador) + subidas.

## 1. Medidas (registros existentes, todo a 1020/460; `tools/hilos.py`, `r36r/tools/fiesta.py`)
**CPU de la CPU de sonido** (ticks del núcleo, % de un núcleo; Z21 = m5): desierto 23 · agua 23 · cielo 19 · nubes 23 ·
castillo 19 · fiesta 18 % → **3,0-3,8 ms por 16,7 ms**. Coincide con lo que ella misma mide (irq 118-158 + hilo 37-72
ms/s): **no gira** de forma apreciable mientras espera (espera de S de su lado 0,6-0,9 ms/s). El juego espera S
7-11 ms/s. En el núcleo 2 no hay hilo AX aparte (con S2 la mezcla va dentro de la CPU de sonido); además está el
perfilador de muestras (debug.toml `profile = true`, 0x2C, núcleo 2) y la salida de audio (0x2B, ~1 %).

**Fiesta del castillo** (trabajador = WPROF busy ms por fotograma preparado; % = ticks del núcleo):
| corrida | S2 | R | dibujados | trabajador ms/fot (split) | gxw % | snd % |
|---|---|---|---|---|---|---|
| Z12 m5 apagado | no | no | 32,3 | 23,5 (10,1) | 88 | – |
| R08/R09 (r36r) | no | sí | 53-54 | 9,8 (0,3) | | – |
| Z21 m5 | 0x2C | sí | 35,1 | 23,7 (8,7) | 72 | 18 |
| Z10 m4r | 0x2E | sí | 52,6 | 9,4 (0,4) | 54 | 13 |
**Por qué el trabajador «pierde» ~17 ms/fot si el sonido solo usa ~3 ms**: no es expulsión pura. El trabajador tiene
dos regímenes. Rápido: parsea el fotograma a trozos cada 1 ms y lo entrega antes del siguiente final (WPROF
calls ≈ 8×frames, split 0,3-0,5 ms, busy 9,4-12 ms). Lento: si un fotograma termina tarde, el siguiente lote ya trae
DOS finales de fotograma → camino de descarte (`PrepFrame`, `prev_frame_end >= 0`: cada comando movido uno a uno,
~31 k, y las entradas de todos los draws rehechas) → split 14-15 ms, busy 33-37 ms (calls = frames) → el siguiente
también llega tarde → se queda ahí (biestable; en Z21 las ventanas de 2 s alternan entre 13-18 ms y 33-37 ms).
En la fiesta el núcleo 2 en régimen rápido ya va casi lleno: trabajador 9,4-12 + sonido ~3,1 + salida ~0,2 ms por
16,7 ms; con la CPU de sonido POR ENCIMA del trabajador, sus ráfagas (manejador AID ~0,4 ms cada 3 ms + trama del
SoundThread) caen en medio del fotograma y basta un fotograma tarde para entrar en el régimen lento. El juego no se
entera (59 fot/s), pero se dibuja la mitad. Por debajo (Z10) el trabajador no pierde nunca el núcleo → régimen rápido.
**Por qué 0x2E (Z10) rompe el audio**: (a) el DMA de audio solo recupera hasta UNA pasada (3 ms) de retraso
(`adma_start_pass`): con ráfagas del trabajador de varios ms se pierden ticks → AID 250-322/s, 28 kHz, 1 757 underruns;
(b) inversión de prioridad: si la CPU de sonido tiene S (o el mutex del SoundThread) y la expulsa el trabajador, el
juego la espera: espera de S del juego 15-37 ms/s (Z21: 7-11) y nubes 58,3 → 54,8 fot/s.
**Núcleo 0 (Z15)**: allí el render va al 60-95 % y tiene 0x2C: la CPU de sonido a la MISMA prioridad no le quita
nunca el núcleo (Horizon no reparte por tiempo) y solo corre cuando el render se bloquea → AID 274-313/s.

## 2. Cuelgue de Z22 (m5 sin cambios, carga de agua_pinguino)
**Hechos** (`switch/real_logs/bench_Z22_m5_escucha_1115_1125`, perfilador de muestras `profile_*.txt` con el ELF de m5):
- render (`main`): 252 k muestras (~250 s) en `sched_yield` dentro de `GX_MemReport` ← `log_memory` ← `frame_tick`:
  gira esperando `g_fifo_mutex` (FifoLock, giro + yield).
- juego (`cpu`): 255 k muestras en `sched_yield` dentro de `wg_flush_now` ← `process_pending_callbacks_slow` (vaciado por
  edad del write-gather desde un sondeo), pc invitado 0x802DCFC0 (`__div2i`) ← 0x801AB990 (código de alarmas del SO,
  con interrupciones apagadas): gira esperando el mismo `g_fifo_mutex`.
- trabajador (`gxw`): 255 k muestras en `memcpy` ← `PrepFrame` (la copia de `g_hw_fifo` que se hace CON `g_fifo_mutex`
  tomado) y ninguna en otro sitio: no avanza, está expulsado con el candado en la mano.
- La CPU de sonido no se muestrea, pero es el único hilo del núcleo 2 por encima del trabajador que puede ocuparlo
  entero (0x2C; el de salida de audio 0x2B espera eventos cada 5 ms).
**Mecanismo** (inferido de lo anterior + código, y reproducido en PC): el juego entra en un manejador (alarma/DEC)
con S tomado (`game_irq_enter`), y dentro un sondeo vacía el write-gather → pide `g_fifo_mutex`, que tiene el
trabajador (0x2D, núcleo 2) → la CPU de sonido (0x2C, núcleo 2) quiere S y gira con `sched_yield`, que en Horizon solo
cede a hilos de su MISMA prioridad → el trabajador no vuelve a correr nunca → nadie suelta nada. Intermitente porque
hace falta que coincidan las tres cosas (más probable en cargas: muchas alarmas/callbacks de DVD con S tomado).
**Arreglo** (`NWII_C_SPIN_OFF` lo quita): toda espera activa de la CPU de sonido (S, candado del DSP) gira ~2000
vueltas y después duerme (`svcSleepThread` 20 µs, luego 200 µs): duerme = cede el núcleo a los de menos prioridad.
Lo mismo en el candado del asignador rápido (`switch_alloc.cpp`, giro PURO sin ceder: mismo riesgo con cualquier
pareja de hilos del mismo núcleo; ahora duerme 10 µs tras 4096 vueltas).
**Z20** (S2 en el núcleo 0 a 0x2B, pantalla negra desde el arranque, registro vacío): mismo tipo de ciclo con el hilo
de render en lugar del trabajador (render toma `g_fifo_mutex` en cada fotograma en `ProcessGXFifo`, en `PrintProf` y
en `GX_MemReport`; con S2 a 0x2B por encima de él en su núcleo, la espera de S de la CPU de sonido lo deja sin CPU).
INFERIDO, no reproducido en PC (90 s sin cuelgue con y sin arreglo).

## 3. Vigilante de cuelgues (`s2.cpp`, encendido por defecto)
Hilo normal (0x2C, duerme 1 s; no es de prioridad alta). Si ni el render ni el juego (copias XFB) avanzan en 5 s,
escribe unas líneas `[VIGIA]` en nwii.log y hace flush (máx. 4 por parada, 8 en total): % de CPU por hilo en el último
segundo, pc/lr del juego y si espera S, estado de S (quién, de dónde lo tomó), qué espera la CPU de sonido y desde
cuándo, petición pendiente, fibras, candado del FIFO de GX (dueño, cuántos esperan, fotogramas en cola), candado del
DSP, hilos invitados (estado, prioridad, cola, dueño del mutex, srr0/lr guardados).

## 4. Reparto (c2 en consola, contiguas C05-C08, lista fija, 1020/460, IMAGEN OK las cuatro)
| corrida | fiesta dib | AID min por escena / ventanas bajas | underruns (suma escenas) | ms juego REAL des/agua/cielo/nubes/cast |
|---|---|---|---|---|
| C05 c2 control (= m5 + esperas) | 30,4 | 328-333 / 0 | 12 | 11,5 · 15,1 · 14,2 · 11,4 · 6,2 |
| C06 base 0x2E, alta 0x2C, tarde 3 ms, catchup 8 | **50,9** | 322-332 / 5 | 118 | 11,4 · 15,6 · 14,4 · 11,5 · 6,3 |
| C07 igual, tarde 6 ms | 50,9 | | más que C06 | 11,4 · 15,4 · 14,3 · 11,5 · 6,3 |
| C08 base 0x2D (= trabajador), alta 0x2C, catchup 8 | 49,0 | 323-333 / 1 | 42 | 11,6 · 15,1 · 14,3 · 11,2 · 6,2 |
Ninguna cumple todo: C06 da la fiesta pero empeora audio (underruns ×10) y agua/nubes; C08 casi (fiesta 49,0, juego =
Z21, underruns ×3,5). Queda abierto.

## 5. SEGUNDO cuelgue (D07/m6a): la lista AX de SoundThread (16:00)
**Causa (verificado en código + reproducido en PC):** `SoundThread::AxCallbackFunc` (0x8027B850) corre en la
interrupción AID (CPU de sonido, con S) y recorre la lista +0x388 del objeto 0x80412BF0 (centinela 0x80412F78): lee el
siguiente nodo (r31) ANTES de llamar al virtual +0x10 del actual. El juego da de alta/baja nodos con
`SoundThread::Register` 0x8027BA20 / `Unregister` 0x8027BA90, protegidos SOLO por el mutex del SoundThread (+0x354 =
0x80412F44): en la Wii basta porque la interrupción nunca corre a la vez que un hilo. El borrado (0x80229190) pone a 0
next/prev del nodo quitado: si la CPU de sonido tenía ese nodo en r31, sigue por next = 0 → lee la «lista» desde la
dirección 0 (MEM1[0] = "SMNE01…") → virtual nulo → `[NullCall] site 0x8027b930` sin fin con S tomado → el juego espera S
para siempre (D07, m6a). Altas/bajas del juego: 3-19/s (sobre todo al empezar demo); con la CPU de sonido dentro de su
interrupción en ese instante: 50-80 por corrida de 150 s.
**Reproducción en PC** (`tools/axl_serie.sh`, prueba (c) de M + `NWII_C_STRESS_IRQ_US=100`: la CPU de sonido se entretiene
en cada vuelta de ESE bucle): sin arreglo 1 cuelgue de 3 (axl5_off_1: misma firma, 8 500-10 000 NullCall/s, el vigilante
salta como ALARMA); sin estrés de bucle 1 lista rota vista de 6. Con arreglo: 0 de 5 (axl5_on), 0 rotas, 0 NullCall.
**Arreglo (encendido; `NWII_C_AXLIST_OFF=1` lo quita):** en el hilo del juego, en la entrada de OSLockMutex llamada desde
Register/Unregister (lr 0x8027BA4C/0x8027BABC) se toma S y el mutex en nativo (si está libre o es suyo; si lo tiene la
CPU de sonido, se atienden sus peticiones hasta que lo suelte; si lo tiene otro hilo del juego se deja al SDK y se
cuenta como «sin S»); S (tipo kHoldList) se mantiene aunque EE=1 y se suelta en la entrada de OSUnlockMutex desde
esas funciones (lr 0x8027BA6C/0x8027BAD0). Si el hilo invitado cambia en medio (interrupción), S se suelta y se retoma
al volver ese hilo. Así la alta/baja nunca coincide con la interrupción de sonido, como en la Wii. Coste medido (PC):
espera de S del juego 0,08-0,31 ms/s, S retenido ~0,01 ms/s. No reabre el bloqueo por prioridades (se toma S ANTES del
mutex solo si el mutex está libre; nunca se espera S con el mutex de otro).
**Regla general opcional** `NWII_C_AXLIST_MUTEX=1`: S durante TODO uso del mutex del SoundThread por el juego (protege
cualquier dato de ese mutex que lea la interrupción). Sin medir en consola.
**Auditoría** (parcial, INFERIDO): lo que la interrupción de sonido toca y el juego cambia CON interrupciones apagadas
ya va con S (snd_class); el patrón «solo mutex + recorrido en interrupción» encontrado es esta lista (la +0x37C la
recorre FrameProcess con el mutex). Los virtuales +0x10 de los nodos (reproductores de nw4r) no se han auditado uno a uno.
**[NullCall]**: contador atómico (antes `static int` sin proteger y D07 escribió 928 917 líneas), 20 líneas y el resto
contado; el vigilante da ALARMA si hay > 1000/s y vuelca (también las últimas 32 altas/bajas de la lista).
**D06** (congelado sin NullCall, último registro: lectura DVD a 0x80412FA0, 0x4000): es el búfer de flujo que va justo
DESPUÉS del objeto SoundThread (que acaba en ~0x80412F88) y se lee en cada demo (también en Z21 y en D07 sin
problema): no se solapa con la lista. Mismo cuelgue probable con un bucle que no pasa por un virtual nulo (INFERIDO).
**Parche para M** (contra el runtime de m6a; s2.cpp/s2.h de m6a = los de c2): `c5_vs_m6a.patch` (s2.cpp + os_hle_stubs.cpp).
NRO c5 (`bin/c5_pgo.nro`, sha256 4333b52f…e394; fastmem 0 mal formadas) = c2 + todo esto. Consola: C10 (arreglo) y C11
(sin arreglo) contiguas al final de la cola.
