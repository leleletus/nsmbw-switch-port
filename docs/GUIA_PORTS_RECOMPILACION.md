# Guía para futuros ports por recompilación (Wii, GameCube, 3DS y en general)

Lo aprendido portando **New Super Mario Bros. Wii** a PC y Switch por recompilación estática (NWiiRecomp), con lo
medido en hardware real. Pensado para quien empiece el siguiente port: qué hacer desde el día 1, qué evitar, cómo
probar.

Detalle técnico de la Switch: [switch/README.md](switch/README.md) y los documentos de esa carpeta. Medidas
reales de la consola: [switch/BENCHMARK_SWITCH_REAL.md](switch/BENCHMARK_SWITCH_REAL.md).

---

## 1. Decide el objetivo de hardware antes de optimizar

- Fija el **peor hardware que debe ir a 60 fps** (en NSMBW: Switch v1 sin overclock, portátil, CPU 1020 MHz) y
  convierte todo a **ciclos por frame**: 16,7 ms × 1020 MHz ≈ 17 M ciclos por hilo.
- Las medidas en una consola con overclock o en un PC hay que escalarlas. Registra siempre el reloj real de la CPU
  junto a cada medida (en Switch: `clkrstGetClockRate(PcvModuleId_CpuBus)`).
- **Los emuladores mienten en rendimiento.** citron (Switch) daba otra distribución de costes que la consola real
  (p. ej. leer el contador del sistema es caro en su JIT y barato en la consola). Mide en el hardware real lo antes
  posible y deja el emulador para pruebas funcionales.

## 2. Mide en el hardware con tus propias herramientas

- **Microbenchmark propio** (`tools/nxbench`): latencia/rendimiento de instrucciones, cachés, RAM, syscalls,
  mutex, atómicos, malloc, saltos, comunicación entre núcleos y los patrones del código recompilado. En ciclos no
  depende del reloj. Una pasada son 2 minutos y responde preguntas que la documentación no responde.
- **Perfilador por muestreo en la consola** (en NSMBW: `switch_profiler.cpp`, un hilo que pausa los otros y lee su
  PC con `svcGetThreadContext3`) + **simbolizador** en el PC con el `.elf` exacto de esa compilación. Guarda siempre
  el `.elf` junto a cada binario que vaya al hardware.
- Línea de log periódica con fps, % de cada hilo, reloj del juego, audio y contadores clave.
- Congela cada binario que pruebes (copia + sha256 + `.elf`) y anota qué se probó y con qué configuración.

## 3. El traductor (recompilador estático): lo que más rinde

Ordenado por impacto medido en NSMBW:

1. **Registros del CPU original en registros del host.** Guardarlos en una estructura en memoria (`ctx.gpr[N]`)
   convierte cada instrucción en cargar-operar-guardar: medido ×7,5 más lento en un A57 (ida y vuelta por memoria
   ~7 ciclos). Lo correcto es lo que hace un JIT tipo Dolphin: variables locales dentro de cada función y
   sincronizar con la estructura solo en llamadas, sondeos de interrupción que pueden entregar algo, y salidas.
   Diseñarlo así **desde el principio** ahorra el problema más caro.
2. **El compilador cree que las escrituras a la RAM emulada pueden tocar la estructura de registros** (todo
   `uint8_t*` puede apuntar a cualquier cosa) y recarga registros tras cada store. Mitigación: `__restrict`, tipos
   distintos, o registros en locales (punto 1).
3. **Interrupciones baratas.** No sondear todo en cada salto hacia atrás: una cuenta atrás en línea y el camino
   completo solo cada N sondeos o cuando algo lo "despierta" (escritura del decrementador, interrupción externa,
   evento programado). En NSMBW el sondeo era el 21 % del hilo del juego; tras el cambio, 0 diferencias en 600 M
   sondeos verificados.
4. **Memoria del invitado**: comprobaciones de rango baratas (límites en registros, inmediatos codificables) y, mejor,
   **fastmem** (reservar el espacio de direcciones del invitado y dejar que un fallo de página atienda MMIO). En
   Switch es viable con Atmosphère (lo hace melonDS).
5. **Flags condicionales perezosos.** No escribir 4 bytes de CR/flags por comparación si el salto siguiente es el
   único lector: comparar y saltar directo; materializar solo cuando algo lee el registro.
6. **Coma flotante exacta pero barata.** PowerPC calcula las operaciones "single" en double y redondea. Si los
   operandos ya son exactamente single (tras `lfs` o una op single), sumar/restar/multiplicar/dividir/raíz en
   `float` da **bit a bit el mismo resultado** (doble redondeo inocuo: 53 ≥ 2·24+2). En el A57 pasa de 15 a 5 ciclos.
   No vale para multiplicar-sumar fusionado (`fmadds`). Cuidado con FMA: compila con `-ffp-contract=off`.
7. **Funciones del SDK que no hacen nada útil en el host** (mantenimiento de caché `DCFlushRange`/`ICInvalidate`,
   esperas activas) consumen mucho en su forma traducida: reemplázalas por versiones nativas que dejen **el mismo
   estado** (registros, contador de instrucciones), no por un `return`.
8. **Código generado gigante** (NSMBW: 121 MB de `.text`): poda las entradas que nunca se usan, evita
   expansiones por instrucción, `-fvisibility=hidden`/`#pragma GCC visibility push(hidden)` en ejecutables
   estáticos PIE (cada `extern` pasa por la GOT), sin `thread_local` en caminos calientes con `-mtp=soft`.
9. **Tablas de saltos (`switch`) y llamadas indirectas**: resuélvelas en el traductor. En NSMBW los "saltos salvajes"
   al intérprete eran fuente de cuelgues y lentitud.

## 4. El runtime (HLE de hardware y sistema)

- **Nada de `malloc`/`free`, `shared_ptr` atómicos ni mutex por evento pequeño** (por draw, por escritura de FIFO):
  en Switch `malloc+free` = ~420 ciclos, `shared_ptr` copia = ~73, mutex = ~78. Pools, búferes reutilizados, conteo
  no atómico si un solo hilo lo toca, acumular escrituras y pasarlas en bloque.
- **Nada de syscalls en caminos calientes** (~340 ciclos cada una en Switch). Relojes: lectura directa del contador
  (`armGetSystemTick`, 8 ciclos).
- **Repartir entre núcleos con bloques grandes**: despertar un hilo en otro núcleo cuesta ~8 400 ciclos y mover una
  línea de caché entre núcleos ~270. Un frame entero o una lista de audio, sí; tareas pequeñas, no.
- **Audio con su propio hilo y salida nativa** del sistema; nunca dependiente del bucle principal.
- **Regulador de sobrecarga**: si la consola no llega, que el reloj del juego se atrase un poco en vez de entrar en
  una espiral de interrupciones (NSMBW llegó a 0 fps por eso en la Switch).
- Prioridades en Horizon: el kernel solo reparte tiempo por turnos (10 ms) entre hilos de prioridad 59; con otras
  prioridades un hilo que no se bloquea acapara su núcleo.

### Mandos (Switch)
Una sola fuente de mandos (libnx **o** SDL: el driver de SDL abre su propio selector de 8 jugadores), exactamente los
jugadores que admite el juego, girar a mano el stick de un Joy-Con suelto en horizontal, esperar ~1,5 s antes de
reaccionar a un mando perdido y cerrar la pantalla de "añadir mando" del juego al terminar el selector. Detalle:
[switch/MANDOS_SWITCH.md](switch/MANDOS_SWITCH.md).

## 5. Gráficos (GX de Wii/GameCube y similares)

- **Transforma vértices en la GPU** (matrices en UBO/SSBO, atributos crudos), como Dolphin. Hacerlo en la CPU fue el
  mayor coste del hilo de render en NSMBW.
- Junta dibujos con el mismo estado; cachea display lists decodificadas, pero no las vacíes por frame (NSMBW: 650
  listas por frame contra un límite de 512 = 0 aciertos).
- **Subida de datos**: anillo persistente (`glBufferStorage` + fences) o deko3d; nunca crear/borrar búferes o
  texturas por frame (en Mesa/Switch borrar espera a la GPU). En Switch la memoria de los búferes GL es **sin caché**
  de CPU: escribir secuencialmente y no leerla nunca.
- **Shaders**: genera variantes por estado, precompílalas y guárdalas en disco (caché precalentada con barra de
  progreso). En Switch compilar en la consola cuesta segundos: llévalos compilados.
- **Formatos de textura**: no todos tienen equivalente 1:1 (CMPR de GX ≠ BC1: interpola 3/8). Decodifica en CPU
  si no es idéntico.
- El parpadeo puede ser el sistema y no el juego: límites de compilaciones de shader por frame que saltan dibujos,
  claves de shader infladas, copias EFB leídas antes de escribirse.

## 6. Cómo probar que todo sigue siendo 1:1

- **Oráculo**: un emulador de referencia (Dolphin con stub GDB) para comparar estados (matrices de huesos, un frame
  de lógica). Encontró errores de acarreo (`addic` negativo) que no se veían a simple vista.
- **Verificación incorporada en el binario**, activable por variable de entorno:
  - camino nuevo y viejo con el mismo estado y comparación de toda la RAM y registros (reverb nativa: 651/651);
  - "dibujar el frame dos veces" y comparar píxel a píxel (render);
  - comprobar que cada sondeo saltado no habría hecho nada (interrupciones: 600 M, 0 errores).
- **Entrada guionizada por escenas** (`NWII_INPUT_SCRIPT`, fases por escena) para llegar a título/mapa/nivel en un
  minuto y repetir exactamente.
- **A/B alternado (A B B A)** con hilos fijados a núcleos; dos corridas iguales varían 3-7 %: una mejora menor no se
  puede afirmar con dos corridas.
- **Verificador independiente**: otro agente/persona que intente refutar la mejora corriendo los binarios él mismo.

## 7. El entorno de trabajo (lecciones caras)

- **Limita la memoria** de compilaciones y pruebas (en Linux: un `systemd` slice, `scripts/lim`). Compilar el
  código generado con `-j3` usa ~4,5 GB; el OOM killer mató compilaciones y pruebas varias veces.
- **Perfil de energía "performance"** y cargador puesto para medir; vigila la temperatura y la batería (un portátil
  a tope puede gastar más de lo que da el cargador y suspenderse).
- **No midas con la máquina cargada** (otros procesos pesados invalidan los A/B: pasó con un trabajo de `demucs`).
- Guarda el estado del proyecto en un archivo vivo (`status/ESTADO.md`) con lo último arriba, y cada ronda en su
  carpeta con parches, binarios congelados y su informe.
- No toques la partida/NAND del usuario: copias privadas para las pruebas.

## 8. Notas por plataforma

### Wii / GameCube (origen)
- CPU PowerPC Broadway/Gekko (729/486 MHz), big-endian (`rev` en ARM cuesta 1 ciclo: el byteswap no es problema),
  **paired singles** (dos floats por registro, con cuantización GQR), redondeos propios (`Force25Bit` en `fmuls`).
- Caché bloqueada (locked L1) usada como scratchpad por algunos juegos: hay que emularla.
- GPU GX: TEV de hasta 16 etapas, texgen, EFB/XFB con copias a textura, display lists. Audio DSP con microcódigo
  AX (HLE).
- Recursos: el decomp (NSMBW-Decomp), Dolphin como referencia de comportamiento, símbolos del SDK.

### Nintendo Switch (destino)
- 4× Cortex-A57 (3 para homebrew), 1020 MHz en juegos (1785 máx. oficial), L1D 32 KB, L2 2 MB compartida, RAM
  ~250 ciclos. Sin FP16 aritmético en la CPU; NEON 4×float a 1 por ciclo. Detalle en `switch/`.
- GPU Maxwell 256 núcleos, memoria unificada; Mesa/nouveau caro por draw, deko3d mucho más barato.

### Nintendo 3DS (si se apunta a ella; **no medido** en este proyecto)
- CPU ARM11 MPCore (ARMv6K): 268 MHz en 3DS antigua, 804 MHz en New 3DS, VFPv2 sin NEON, cachés pequeñas. Un
  juego de Wii no cabe; para ports *desde* 3DS hacia otra plataforma, el origen es ARM little-endian (más fácil de
  traducir que PowerPC) y la GPU PICA200 tiene vertex shaders programables y combinadores de textura fijos
  (parecidos en espíritu a TEV).
- Aplica todo lo de las secciones 3-6; mide primero con un microbenchmark propio como `nxbench`.

### En general
- Antes de una optimización grande, haz un microbenchmark de la idea en el hardware objetivo (el de "registros en
  memoria frente a registros" dio ×7,5 en un minuto y decidió el plan).
- Prefiere funciones nativas de la plataforma cuando el resultado es **idéntico**; si no lo es, opción en el
  `config.toml`, desactivada por defecto.
