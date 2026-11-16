# Cronología resumida del port

Extraída de [`../status/ESTADO.md`](../status/ESTADO.md) (bitácora completa, con todas las mediciones). Fechas de 2026.
«Fps» = fotogramas dibujados por segundo medidos en la Switch real salvo que se indique PC.

## Octubre: primer port funcional (PC) y primeras pruebas
- **27–29 oct.** El juego recompilado corre en PC (Linux) a ~59 fps: título, fiesta, mapa, niveles, multijugador de 2 a 4
  jugadores (ronda **R6**), partidas guardadas, arreglo del gráfico negro a pantalla completa, accesos a memoria más
  rápidos. Primer NRO para Switch (probado solo en el emulador citron: 7–22 fps).
- **30–31 oct.** Rondas **R7/R8**: shaders precompilados, memoria estable, generación de código más rápida, hilo de
  render y de audio por separado. Primer paso a la **Switch real** (copia por MTP): 10–11 fps; el hilo del juego al
  91–96 %. Se lanza **R9**: varios frentes en paralelo (hilos, CPU, render, fastmem…).

## Noviembre, semana 1: a la consola real y control remoto
- **7–8 nov.** Combinación `r9combo`, luego `r10e/r10j`: 42–59 fps en niveles (con la consola con overclock), mapa 11–13.
  Frentes R11: render repartido entre núcleos, transferencia de buffers (GPU), **fastmem** (accesos a memoria del
  invitado por tabla, texto −44 %). La fuga del asignador rápido tumba `r13` a los 37 min (corregida).
- **8 nov.** Control remoto de la Switch sin cable (sys-botbase + nxlink + ftpd). Pruebas de **deko3d** (API gráfica
  nativa de Switch): funciona, pero aparece una «vertex explosion» en la demo submarina; se mantiene OpenGL (Mesa).
  Dos caídas del PC por falta de RAM (`systemd-oomd`): se crean límites de memoria y un vigilante.
- **9 nov.** Primer **PGO** con perfil real de la consola (−18 % de trabajo del hilo del juego). Banco con escenas
  forzadas y protocolo repetible (dos corridas coinciden < 0,5 fps). Trabajador de render en el núcleo 2 (**R7**):
  castillo 46 → 59 fps.

## 10–12 nov: relojes reales de portátil
- Se mide a los relojes de una Switch v1 en portátil (**1020 MHz CPU / 460 MHz GPU**, sin OC): las bases rondan 25–32 fps
  y el hilo del juego tarda ~26 ms (meta 16,7). Muchas ideas dan ~0.
- **12 nov.** Frente **T**: la métrica «ms de juego por fotograma» estaba inflada por los fotogramas descartados;
  se corrige. **m2** y después **m3** (r31t + G6c3 + PGO) se instalan en la consola.

## 14–15 nov: la gran subida
- **14 nov.** m3 + PGO: desierto 20,0 → 15,6 ms por fotograma del hilo del juego.
- **15 nov, madrugada.** **S2** arreglado: el sonido sale del hilo del juego (desierto 19,4 → 14,4 ms, castillo 13,9 → 8,0).
  **g7d + PGO** instalada: 55·35·41·59·47 fps (punto de partida de la tabla del README).
- **15 nov, mañana.** El usuario fija la meta: 60 fps estables en el peor caso. Se explica un cuelgue de **m5** (bloqueo
  por prioridades alrededor del candado del FIFO; frente **C**). Frente **V**: tras cada retrazo el bucle ocioso
  dormía hasta 2 ms; con `NWII_V_WAKE`+`NWII_V_LATE=8` el agua pasa de 35 a 40–57 fps de juego.
- **15 nov, tarde.** Frente **D**: descarte barato de fotogramas (agua 34 → 43 → 52 dibujados). **R2** baja el render ~4 ms.
  Integración **m6a/m6b** (frente M). Segundo cuelgue de S2 (nodo AX sin vtable) explicado y arreglado (frente C).
  Frentes **A** (sonido), **W** (agua: una sola rutina de 0x8000D000 emite ~85 000 escrituras al pipe por fotograma),
  **K** (código frío + sondeo de interrupciones).
- **15 nov, noche.** **m6h** instalada (59,4·55,8·59,2·60,0·58,2 con MERGE; luego 59,6·57,4·59,4·60,0·57,5 con agua nativa
  y audio). Limpieza reversible del disco (~98 GB movidos a una papelera). **m6i**: tirada limpia de 1500 s
  (300 ventanas, 0 NullCall). El usuario reporta que el selector de mundos («−») va lento.

## 16 nov: selector de mundos y m6q
- **00:12.** Frente **P**: el selector baja a 43,5 fps dibujados (render 20,7 ms, ~10 000 draws GX por fotograma).
- **03:15.** `NWII_P_DLMERGE` (fusión de draws en la caché de display lists) lo sube de 39,8 a **59,7**; con `CULLSKIP`
  59,8. Combinar todo (TRIPLE, KEEPCOPY, MRT) empeora: se dejan apagadas.
- **03:38.** `NWII_W_SIM=2` (simulación de olas) no gana en consola; queda apagada. El agua solo mejoraría
  regenerando el código con PGO nuevo (7–8 h), fuera de plazo.
- **05:28.** **m6q** = m6i + DLMERGE + CULLSKIP por defecto: **59,5·57,6·59,5·60,0·58,2**; pendiente su tirada larga y su
  instalación.
