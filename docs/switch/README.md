# Switch: referencia técnica y plan de rendimiento (8-nov-2026)

**Objetivo:** NSMBW a **60 fps estables en una Switch v1 sin overclock, modo portátil** (CPU 1020 MHz, GPU
307-384 MHz). Presupuesto: 16,7 ms por frame y por hilo = **~17 M ciclos de CPU**.

## Documentos de esta carpeta

| Documento | Qué tiene |
|---|---|
| [BENCHMARK_SWITCH_REAL.md](BENCHMARK_SWITCH_REAL.md) | Medidas propias en la consola real con `tools/nxbench`: latencias de instrucciones, cachés, RAM, syscalls, mutex, atómicos, malloc, saltos, núcleos. |
| [CPU_TEGRA_X1.md](CPU_TEGRA_X1.md) | El Cortex-A57 a fondo: pipeline, cachés, tablas de latencia de la guía de ARM, coma flotante (FP16/32/64), NEON, qué es caro y barato, banderas de GCC, trucos para el código recompilado. |
| [SISTEMA_HORIZON_LIBNX.md](SISTEMA_HORIZON_LIBNX.md) | Horizon/libnx/Atmosphère: memoria y regiones, **fastmem (viable)**, núcleos y planificador, relojes y boost, coste de syscalls/IPC/mutex, automatización de pruebas (nxlink, sys-botbase, gdbstub). |
| [GPU_MAXWELL_DEKO3D.md](GPU_MAXWELL_DEKO3D.md) | GPU Maxwell GM20B, Mesa/nouveau en homebrew (memoria sin caché, coste por draw), deko3d, estrategias para nuestro renderer. |
| [MANDOS_SWITCH.md](MANDOS_SWITCH.md) | Mandos con libnx + SDL: selector de mandos (y el de 8 que abre SDL solo), Joy-Con suelto en horizontal (girar el stick a mano), acoplar/desacoplar, cerrar la pantalla de "añadir mando". |
| [FUNCIONES_CALIENTES_NSMBW.md](FUNCIONES_CALIENTES_NSMBW.md) | Qué funciones del juego/SDK gastan el hilo del juego en la Switch real, cuáles se pueden reemplazar por nativas **idénticas** y cómo verificarlo. |

Guía general para otros ports (Wii, 3DS, …): [../GUIA_PORTS_RECOMPILACION.md](../GUIA_PORTS_RECOMPILACION.md).

## Dónde estamos (medido en la Switch del usuario)

| Versión | Dónde | Resultado |
|---|---|---|
| r8 + hilos | Switch real, 1785 MHz (OC) | ~10-11 fps |
| r9combo (extras + hilos + cpucode c2) | Switch real, 1785 MHz (OC), 1-1 | **~29-30 fps** estables; mapa 13-17 fps |
| r10a (r9combo + sondeo de interrupciones inline) | Switch real | **pendiente** (probar sin OC) |

Con r9combo a 1785 MHz: hilo del juego ~25 ms/frame (≈45 M ciclos), hilo de render ~20 ms (≈36 M ciclos); en el
mapa el render llega al 100 %. **A 1020 MHz hace falta ~×2,7 en el hilo del juego y ~×2 en el de render (×4 en el
mapa).**

## Por qué va lento (resumen de los hallazgos)

**Es la CPU, no la GPU.** La GPU está casi ociosa.

Hilo del juego (código PowerPC traducido):
1. **Registros de la Wii en memoria** (`ctx.gpr[N]`, 2,49 M usos en el código generado): cada operación es
   cargar-operar-guardar; la ida y vuelta por memoria cuesta ~7 ciclos en el A57. Medido: 15 ciclos frente a 2 (×7,5).
   GCC además vuelve a cargar `ctx` tras cada escritura a la RAM del juego (cree que pueden solaparse).
2. **Sondeo de interrupciones en cada salto hacia atrás** (21 % del hilo en r9combo; r10a lo arregla).
3. **Bucles de caché de la Wii** (`DCFlushRange`/`DCInvalidateRange`: 8,6 %) que para nosotros no hacen nada.
4. **Coma flotante "estilo PowerPC"**: cada operación single se hace en double y se redondea (15 ciclos frente a 5).
5. Código enorme (121 MB de `.text`, 10-26 instrucciones ARM por instrucción PowerPC): presión en la caché de
   instrucciones (48 KB) y en el decodificador (3 por ciclo).
6. Reverb de audio AXFX (8,8 %), paired-singles (2,6 %), `strcmp`/`memcpy`/matrices.

Hilo de render:
1. Por cada dibujo GX: `malloc`/`free` (423 ciclos c/u), `shared_ptr` atómicos (73 ciclos), mutex, copias de
   comandos; vértices decodificados "gordos" (~170 B c/u).
2. Transformación de vértices y texgen en la CPU.
3. En Switch está forzado `NWII_NORING=1`: ~230 `glBufferData` nuevos por frame en memoria **sin caché** de CPU.
4. Coste fijo de Mesa/nouveau por draw (3-30 µs).

## Plan R10 (por orden)

Cada paso: A/B en la Switch real **sin overclock** + verificación 1:1 en PC.

### Hilo del juego
| # | Qué | Estado | Ganancia esperada |
|---|---|---|---|
| G1 | Sondeo de interrupciones inline con cuenta atrás (guestloop) | **r10a, en la Switch** | −20 % del hilo |
| G2 | `DC*`/`IC*` en forma cerrada (nativas exactas, mismos registros e `inst_count`) | por hacer | hasta −8 % (menos tras G1) |
| G3 | Pasadas ya escritas: `apply_r9_axfx_fast.py` (reverb), `apply_r9_psq.py` (paired-singles) | por verificar | −5 % |
| G4 | Barato en el traductor: `__restrict` en `ctx`, `#pragma GCC visibility push(hidden)` | por probar | −10-20 % del código generado (estimado) |
| G5 | **Registros de la Wii en registros ARM** dentro de cada función (sincronizar `ctx` solo en llamadas, sondeos lentos y salidas); CR empaquetado / comparar y saltar directo | diseño | **la mayor**: ×2 o más en el código generado |
| G6 | Floats directos cuando los operandos ya son single (resultado idéntico) | diseño | ×3 en cadenas de coma flotante |
| G7 | Fastmem (rango virtual + manejador de fallos, como melonDS) para quitar el camino lento de cada acceso | **antes de G5** | menos código y mejor uso de registros |

> **Orden G7 → G5 (lección de R8):** R8 ya probó registros en locales (`r8/codegen/tools/apply_reglocals.py`) y
> en PC no ganó nada (hilo CPU 48,7 % → 48,9 %): cada acceso a memoria del juego tiene una llamada al camino lento
> (MMIO, FIFO GX, direcciones raras), y alrededor de esas llamadas GCC manda los locales a la pila
> (`func_801A5C10`: 184 → 663 stores en AArch64). Con fastmem el acceso normal no tiene llamada; el MMIO se sabe
> de antemano por sitio (lista sacada de corridas en PC: las escrituras a la tubería GX, ~650 k/s, **no** pueden
> fallar a cada vez) y lo raro cae en el manejador de fallos. Solo entonces los registros pueden vivir en
> registros ARM.
| G8 | HLE exacto de `memcpy`/`strcmp`/`PSMTX*` (verificado) | después | −3-5 % |

### Hilo de render
| # | Qué | Estado |
|---|---|---|
| R1 | r10b: `HostPtr` no atómico, carga XF dentro del comando, caché de emisión (frente render t4) | preparado, falta verificar |
| R2 | Anillo de vértices (`NWII_NORING=0`) → anillo persistente `glBufferStorage` + vértices compactos + `MESA_NO_ERROR` | A/B en la Switch |
| R3 | Transformación y texgen en el vertex shader (atributos crudos + matrices en UBO/SSBO), como Dolphin | 1-3 semanas |
| R4 | Repartir el render: decodificar GX en el núcleo 2 y enviar a la GPU en el 0 (bloques de un frame) | por diseñar |
| R5 | Backend deko3d (shaders TEV precompilados con uam) | después de R3 |

### Infraestructura
- Pruebas automáticas en la Switch real: nxlink (enviar y ejecutar con salida en el PC), sys-ftpd (logs),
  sys-botbase (mando virtual), `appletSetMediaPlaybackState(true)` para que no se apague la pantalla.
- Verificación por función (código viejo frente a nuevo con el mismo estado, comparando registros y escrituras),
  generalizando `NWII_AXFX_VERIFY`.
- Registrar en `fps_log` el reloj real de la CPU (`clkrstGetClockRate`) para no confundir medidas con OC.

### Opción para el usuario (no cuenta para el objetivo)
`clkrstSetClockRate(PcvModuleId_CpuBus, 1785000000)` sube solo la CPU al máximo oficial de Nintendo (el que usan
las pantallas de carga) sin bajar la GPU, como hace RetroArch. Puede ir en `config.toml` como opción, desactivada
por defecto. **No usar `appletSetCpuBoostMode`**: baja la GPU a 76,8 MHz.
