# Horizon, libnx y Atmosphère: lo que importa para el port de NSMBW

Este documento recoge lo que el sistema de la Switch permite y lo que cuesta, pensando en un `.nro` de libnx
que corre una recompilación estática de NSMBW y que está limitado por la CPU. Cubre dos formas de arranque:
desde el hbmenu (modo applet, a través del Álbum) y desde un forwarder (modo aplicación).

**Cómo se ha verificado.** Casi todo sale de leer el código fuente, no solo la wiki:

- libnx en `master`, commit `b156b47` (19-oct-2026). La copia instalada en devkitPro es `libnx 4.12.0-1`.
- Atmosphère en `master`, commit `6bfe5b8` (7-nov-2026). Su changelog llega a la 1.12.1, que soporta el firmware 23.0.0.
- nx-hbloader v2.4.5 (`82b9512`), sys-clk (`1fd9509`), sys-botbase (`ae13548`).
- Las partes de melonDS, flycast, PPSSPP y RetroArch que tratan la Switch.

Cuando algo es deducción mía o no lo he podido comprobar, lo digo. **No pongo cifras de coste (ns o µs) que no
estén publicadas.** Donde faltan, la sección 9.4 explica cómo medirlas.

---

## 0. Resumen en 12 líneas

1. **Fastmem es viable** desde el hbmenu sobre Atmosphère. melonDS ya lo usa en la Switch: reserva 8 GB de espacio
   virtual, mapea la RAM emulada en distintas posiciones y con espejos, y recupera los fallos de acceso con
   `__libnx_exception_handler` para seguir ejecutando (§2).
2. El método es `svcMapProcessCodeMemory` (una sola vez) más `svcMapProcessMemory` (una vez por vista). Las vistas
   pueden repetirse, así que **los espejos sí se pueden hacer**. `svcMapMemory` no vale para eso (§2.3).
3. Los fallos de datos solo llegan al programa porque Atmosphère activa por defecto los manejadores de excepción de
   usuario. En el firmware original no llegarían (§2.4).
4. El manejador de libnx usa **una sola pila y un solo volcado globales**: no es seguro con varios hilos que fallen
   a la vez. Hay que sobrescribir `__libnx_exception_entry` (es un símbolo débil) o fallar en un único hilo (§2.5).
5. Memoria: en modo aplicación el pool de aplicaciones es de **3285 MB**. En modo applet, el pool de *todos* los
   applets es de 507 MB menos lo que se queda Atmosphère, y hbloader además reserva 134 MB para otros applets.
   Lo fiable es medirlo en tiempo de ejecución (§1.2).
6. Núcleos: hbloader solo pide los **núcleos 0 a 2**. El 3 es del sistema. Dos hilos de la misma prioridad en el
   mismo núcleo **no se reparten el tiempo** salvo en la prioridad 59 (0x3B) (§3).
7. `armGetSystemTick()` es una lectura de `cntpct_el0` sin syscall. `clock_gettime` y `std::chrono` de libnx usan
   ese mismo contador (§4.1).
8. El `mutex` de libnx no hace syscall si nadie más lo tiene. El `condvar` sí hace syscall en cada aviso (§4.3).
9. **El boost oficial (`appletSetCpuBoostMode`) pone la CPU a 1785 MHz pero baja la GPU a 76,8 MHz.**
   RetroArch y sys-clk ponen la CPU a 1785 MHz sin tocar la GPU, con `clkrstSetClockRate` (§5).
10. Para saber la frecuencia real de la CPU: `clkrstGetClockRate` sobre `PcvModuleId_CpuBus` (§5.3).
11. sys-clk elige el perfil por title ID. En modo applet sin juego abierto ve el TID del menú, `0100000000001000`.
    Con el forwarder ve el TID del forwarder (§5.4).
12. Si `read()` escribe en memoria que no es heap normal (por ejemplo una vista fastmem), libnx divide la lectura en
    trozos de 4 KB: **hay que leer al heap y copiar después** (§6).

---

## 1. Proceso y memoria

### 1.1 El espacio de direcciones de 39 bits

Tamaños en Mesosphère, el kernel de Atmosphère, que reproduce el de Nintendo
([svc_memory_map.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/svc/svc_memory_map.hpp)):

| Región (39 bits) | Tamaño | Qué va ahí |
|---|---|---|
| Espacio total | 512 GB | |
| Heap | 8 GB | `svcSetHeapSize` (memoria en estado *Normal*) |
| Alias | 64 GB | `svcMapPhysicalMemory`, buffers de IPC |
| Stack | 2 GB | destino de `svcMapMemory`, pilas |
| "Small"/KernelMap | 64 GB | TLS (*ThreadLocal*) y mapeos *Static* |
| Resto ("alias-code"/ASLR) | lo que queda de los 512 GB | código, `CodeOut`, `SharedCode`, `Transfered`… |

El kernel ordena las regiones de mayor a menor y las coloca alrededor del código del proceso
([kern_k_page_table_base.cpp, InitializeForProcess](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_page_table_base.cpp)).
Qué región corresponde a cada estado de memoria lo decide `GetRegionAddress`/`GetRegionSize` en ese mismo archivo.
Las vistas de fastmem (`SharedCode`, `CodeOut`, `Transfered`) caen en la región grande "alias-code", que en 39 bits
ocupa casi todo el espacio. **Sobra sitio para reservar 4 u 8 GB.**

- **¿El NRO tiene 39 bits?** El NPDM de hbloader pide `"address_space_type": 1`, es decir, 36 bits
  ([hbl.json](https://github.com/switchbrew/nx-hbloader/blob/master/hbl.json)). Pero el loader de Atmosphère lo
  cambia a **39 bits** por defecto para cualquier arranque de hbl
  ([ldr_meta.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/stratosphere/loader/source/ldr_meta.cpp),
  [cfg_override.board.nintendo_nx.inc](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/source/cfg/cfg_override.board.nintendo_nx.inc),
  claves `override_address_space` y `override_any_app_address_space` en
  [override_config.ini](https://github.com/Atmosphere-NX/Atmosphere/blob/master/config_templates/override_config.ini)).
- **Forwarder.** Si el forwarder es su propia NSP con su propio NPDM (sin pasar por el override de hbl), el tamaño
  del espacio es el que diga ese NPDM. Con 36 bits la región grande es de unos 64 GB (2 GB "small" más 62 GB
  "large"), que también basta. Conviene comprobarlo al arrancar con `svcGetInfo(InfoType_AslrRegionAddress/Size)`.
  `InfoType_AddressSpaceSize` (id 36) solo existe desde el firmware 23.0.0
  ([SVC#GetInfo](https://switchbrew.org/wiki/SVC)).

**Páginas.** La página base es de 4 KB: todas las syscalls de mapeo exigen alineación a `PageSize`
([kern_svc_memory.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_memory.cpp)).
El kernel usa el bloque más grande posible (1 GB, 32 MB, 2 MB, 64 KB o 4 KB) cuando la dirección virtual y la
física tienen la misma alineación
([kern_k_page_table_entry.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/include/mesosphere/arch/arm64/kern_k_page_table_entry.hpp),
[kern_k_page_table.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/arch/arm64/kern_k_page_table.cpp)).
Por eso conviene **alinear las vistas a 2 MB** (en las direcciones Wii 0x80000000 y 0x90000000 ya lo están si la base
lo está), para tener menos fallos de TLB. Esto último es deducción mía: no lo he medido.

### 1.2 Cuánta RAM hay en modo applet y en modo aplicación

Pools de memoria en una consola de 4 GB
([kern_k_system_control.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/board/nintendo/nx/kern_k_system_control.cpp)):

- **Aplicación: 3285 MB.** El forwarder y el modo "title override" (abrir un juego manteniendo R) corren aquí.
- **Applets: 507 MB, para todos los applets juntos.** Atmosphère le quita 40 MB en el firmware 23 o superior
  (7 MB en el 21 y 22, 14 MB en el 20), más el búfer de KTrace si está activo (16 MB si está compilado). Ver el
  changelog [1.12.0/1.12.1](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/changelog.md).

Dentro de eso, hbloader calcula el heap del NRO a partir de `TotalMemorySize - UsedMemorySize - 2 MB`, redondeado a
2 MB ([nx-hbloader main.c, calculateMaxHeapSize/setupHbHeap](https://github.com/switchbrew/nx-hbloader/blob/master/source/main.c)):

- **En modo applet** resta además `applet_heap_reservation_size`, que por defecto es `0x8600000` (134 MB), para que
  otros applets (teclado, errores…) puedan abrirse. Se puede cambiar con `applet_heap_size` o
  `applet_heap_reservation_size` en `[hbloader]` de
  [system_settings.ini](https://github.com/Atmosphere-NX/Atmosphere/blob/master/config_templates/system_settings.ini).
- **Si la grabación automática de vídeo está activa**, resta 96 MB (`0x6000000`) más.

No doy una cifra exacta para el modo applet porque depende del firmware, de Atmosphère y de lo que tenga abierto el
sistema. **Recomendación:** registrar al arrancar `svcGetInfo(InfoType_TotalMemorySize)` y `UsedMemorySize` (ids 6
y 7, [SVC#GetInfo](https://switchbrew.org/wiki/SVC)). La memoria de la Wii (24 MB de MEM1 y 64 MB de MEM2) cabe de
sobra incluso en modo applet. Lo que aprieta es el resto: Mesa, texturas, caché de shaders.

### 1.3 El heap y `svcSetHeapSize`

- `svcSetHeapSize` exige un tamaño múltiplo de 2 MB y como mucho 4 GB (desde el 2.0.0)
  ([SVC](https://switchbrew.org/wiki/SVC)).
- **Un NRO bajo hbloader no debe llamarlo.** hbloader ya fijó el heap y **copió el propio NRO dentro de él**.
  Al NRO le pasa el resto como `EntryType_OverrideHeap`
  ([main.c](https://github.com/switchbrew/nx-hbloader/blob/master/source/main.c)), y libnx usa esa zona para `malloc`.
- `svcMapPhysicalMemory` (heap en cualquier dirección de la región Alias) necesita `SystemResourceSize` en el NPDM
  ([SVC](https://switchbrew.org/wiki/SVC)). `hbl.json` no declara ninguno, así que **en hbloader no se puede usar**.
  Esto lo deduzco del NPDM; no lo he probado.

### 1.4 Syscalls de mapeo: qué permite cada una

Hbloader concede al NRO todas las syscalls relevantes: 0x01–0x05, 0x15, 0x2C, 0x51, 0x52, 0x73–0x78 y el resto
([hbl.json](https://github.com/switchbrew/nx-hbloader/blob/master/hbl.json)). Además pasa al NRO el handle de su
propio proceso (`EntryType_ProcessHandle`, que se lee con `envGetOwnProcessHandle()`). Varias de estas syscalls lo
necesitan, porque el kernel **no acepta el pseudo-handle** `CUR_PROCESS_HANDLE`
(`GetObjectWithoutPseudoHandle` en
[kern_svc_process_memory.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_process_memory.cpp)).
Sin hbloader, `envGetOwnProcessHandle()` devuelve `INVALID_HANDLE`
([env.c](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/env.c)).

| Syscall | Destino | El origen queda… | ¿Varias vistas del mismo origen? |
|---|---|---|---|
| `svcMapMemory` (0x04) | solo la región **Stack** (2 GB) | inaccesible (bloqueado) | no |
| `svcSetMemoryPermission` (0x02) | – | solo `---`, `r--` o `rw-` | – |
| `svcMapProcessCodeMemory` (0x77) | región alias-code | inaccesible (pasa a *AliasCode*) | no (una sola vez) |
| `svcSetProcessMemoryPermission` (0x73) | – | rechaza W+X | – |
| `svcMapProcessMemory` (0x74) | región alias-code, estado *SharedCode*, **RW** | sin cambios | **sí** |
| `svcCreateTransferMemory` (0x15) + `svcMapTransferMemory` (0x51) | región alias-code | conserva el permiso pedido (RW posible) | una vez por objeto |
| `svcMapPhysicalMemory` (0x2C) | región Alias | – | necesita SystemResource |

Fuentes:

- `svcMapMemory`: [kern_svc_memory.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_memory.cpp)
  (comprueba `CanContain(..., KMemoryState_Stack)`) y
  [KPageTableBase::MapMemory](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_page_table_base.cpp)
  (el origen pasa a `KernelRead|NotMapped` con el atributo `Locked`).
- `svcMapProcessMemory`: [kern_svc_process_memory.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_process_memory.cpp).
  Exige que el origen tenga `FlagCanMapProcess` y que no tenga atributos; no comprueba si ya está mapeado.
- Memoria de transferencia: [kern_k_transfer_memory.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_transfer_memory.cpp).
- Memoria de código: [kern_k_code_memory.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_code_memory.cpp)
  (una vista RW como mucho y otra R/RX hacia el dueño).
- La tabla de la wiki: [SVC](https://switchbrew.org/wiki/SVC).

### 1.5 virtmem de libnx

La API actual ([virtmem.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/virtmem.h)):

- `virtmemLock()`/`virtmemUnlock()`: protegen todo el ciclo de buscar y luego mapear o reservar.
- `virtmemFindAslr(size, guard)`: un hueco libre al azar en la región general.
- `virtmemFindStack(size, guard)`: lo mismo en la región Stack.
- `virtmemFindCodeMemory(size, guard)`: lo mismo, para memoria de código.
- `virtmemAddReservation(mem, size)` y `virtmemRemoveReservation(rv)`: apartan un rango **sin mapearlo**, para
  que libnx no lo use para otra cosa (pilas de hilos, buffers de servicios…).

`virtmemReserve()`, que todavía usa PPSSPP, **ya no existe** en libnx 4.x: no está en el `virtmem.h` instalado.

---

## 2. FASTMEM: ¿se puede hacer?

**Sí.** Hay al menos tres emuladores de código abierto que lo hacen en la Switch con libnx desde el hbmenu.

### 2.1 Proyectos que ya lo hacen

| Proyecto | Qué hace | Fuente |
|---|---|---|
| **melonDS** (rama principal; también melonDS-switch) | `aligned_alloc` de toda la RAM, una vez `svcMapProcessCodeMemory` y `svcSetProcessMemoryPermission(Perm_Rw)`, luego `virtmemFindAslr` dos veces (4 GB para el ARM9 y 4 GB para el ARM7: "8 GB of address space, just don't ask...") con `virtmemAddReservation`. Cada región y **cada espejo** se mapea con `svcMapProcessMemory`. Los fallos se recuperan en `__libnx_exception_handler`: parchea el código y restaura el contexto con su propia rutina (`ARM_RestoreContext`). | [ARMJIT_Memory.cpp](https://github.com/melonDS-emu/melonDS/blob/master/src/ARMJIT_Memory.cpp) |
| **flycast** | `virtmemFindCodeMemory` y `virtmemAddReservation`. Cada región usa `svcMapProcessCodeMemory` + `svcSetProcessMemoryPermission(Perm_Rw)`. Los espejos se **omiten** ("already mapped once so ignore other mirrors"). Un `__libnx_exception_handler` convierte el fallo en un `fault_handler` al estilo POSIX y vuelve con `context_switch_aarch64`. | [shell/switch/libnx_vmem.cpp](https://github.com/flyinghead/flycast/blob/master/shell/switch/libnx_vmem.cpp) |
| **PPSSPP** (M4xw, 2023) | 256 MB con `memalign`, `svcMapProcessCodeMemory` sobre una base reservada y luego vistas con `svcMapProcessMemory`. **No** tiene manejador de excepciones para la Switch: `ExceptionHandlerSetup.cpp` no tiene rama para Horizon. | [MemArenaHorizon.cpp](https://github.com/hrydgard/ppsspp/blob/master/Common/MemArenaHorizon.cpp), [ExceptionHandlerSetup.cpp](https://github.com/hrydgard/ppsspp/blob/master/Common/ExceptionHandlerSetup.cpp) |

**Dolphin.** No he encontrado código público de un Dolphin para la Switch con fastmem. El núcleo Dolphin de libretro
no tiene código de libnx en su repositorio. Hay una noticia de abril de 2026 sobre "Tico", con GameCube y Wii
experimentales mediante Dolphin
([Android Authority](https://www.androidauthority.com/nintendo-switch-tico-play-gamecube-wii-emulation-3659487/)),
pero no he podido ver cómo lo implementa.

### 2.2 La receta (la de melonDS, adaptada a la Wii)

```c
// 1) Memoria real: MEM1 (24 MB) + MEM2 (64 MB), alineada a 2 MB para tener bloques grandes de TLB.
u8 *src = aligned_alloc(0x200000, MEM_TOTAL);
virtmemLock();
u8 *code = virtmemFindCodeMemory(MEM_TOTAL, 0x1000);
svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)code, (u64)src, MEM_TOTAL); // src deja de ser accesible
svcSetProcessMemoryPermission(envGetOwnProcessHandle(), (u64)code, MEM_TOTAL, Perm_Rw);
// 2) Hueco virtual de 4 GB (+2 MB para alinear la base a mano).
u8 *base = virtmemFindAslr(0x100000000ull + 0x200000, 0x1000);
base = (u8*)(((uintptr_t)base + 0x1FFFFF) & ~0x1FFFFFull);
VirtmemReservation *rv = virtmemAddReservation(base, 0x100000000ull);
virtmemUnlock();
// 3) Vistas (todas RW, con espejos):
svcMapProcessMemory(base + 0x80000000, envGetOwnProcessHandle(), (u64)code,            MEM1_SIZE); // MEM1 en caché
svcMapProcessMemory(base + 0xC0000000, envGetOwnProcessHandle(), (u64)code,            MEM1_SIZE); // MEM1 sin caché (espejo)
svcMapProcessMemory(base + 0x90000000, envGetOwnProcessHandle(), (u64)code + MEM1_OFF2, MEM2_SIZE);
svcMapProcessMemory(base + 0xD0000000, envGetOwnProcessHandle(), (u64)code + MEM1_OFF2, MEM2_SIZE);
// El resto de los 4 GB queda SIN mapear: acceder ahí produce un data abort.
```

Es un esquema basado en melonDS: no lo he ejecutado así. Notas:

- **El puntero `src` deja de servir** después de `svcMapProcessCodeMemory`: el origen pasa a *AliasCode*.
  Hay que acceder siempre por `code` o por las vistas.
- **Las vistas están en estado *SharedCode*.** Seguramente no valen como búfer de IPC; ver §6. Es deducción mía:
  las comprobaciones de estado de IPC están en `KPageTableBase`, pero no las he trazado para *SharedCode*.
- **Cada mapeo consume bloques de memoria del kernel.** Muchos mapeos pequeños (por página, como hace melonDS para
  proteger el código) podrían agotar el *slab* y devolver `0xCE01`/`OutOfResource`
  ([SVC](https://switchbrew.org/wiki/SVC), errores de las syscalls de mapeo). Con cuatro mapeos grandes no hay
  problema.
- **Alternativa sin vistas:** si el recompilador ya traduce `ea → base + (ea & máscara)`, los espejos los da la
  máscara y no hace falta remapear nada. Fastmem con huecos solo aporta algo si se quiere atrapar los accesos a
  zonas no mapeadas (MMIO, punteros nulos o fuera de rango) **sin poner comprobaciones en el código generado**.

### 2.3 ¿Se puede ver la misma memoria en dos direcciones?

**Sí, con `svcMapProcessMemory`**, todas las veces que se quiera (melonDS lo hace para los espejos de la NDS). Las
otras dos opciones dan menos:

- `svcCreateTransferMemory(..., Perm_Rw)` + `svcMapTransferMemory` en el mismo proceso deja el origen RW y añade
  **una** vista RW. No hay comprobación de "mismo proceso" en
  [KTransferMemory::Map](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_transfer_memory.cpp).
  Lo deduzco del código; no lo he probado.
- `svcMapMemory` **no** sirve: deja el origen inaccesible y solo puede mapear a la región Stack.

### 2.4 ¿Llegan los data aborts al programa? Sí, pero solo gracias a Atmosphère

- En Mesosphère, `HandleUserException` entrega al proceso **todos** los fallos (data abort, instruction abort,
  alineación…) si `KTargetSystem::IsUserExceptionHandlersEnabled()` es verdadero. Solo los breakpoints van al
  depurador
  ([kern_exception_handlers.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/arch/arm64/kern_exception_handlers.cpp)).
- Ese indicador viene de `disable_user_exception_handlers` en `exosphere.ini`, **que por defecto vale 0, es decir,
  activados** ([exosphere.ini](https://github.com/Atmosphere-NX/Atmosphere/blob/master/config_templates/exosphere.ini)).
- En el kernel original, según la wiki, esa opción **no** está activa en las consolas de venta. Un data abort solo
  iría al programa en casos muy concretos (memoria de código no legible); los demás acabarían en KDebug o en un
  crash ([SVC#Exception handling](https://switchbrew.org/wiki/SVC)). Como el homebrew ya necesita Atmosphère, no
  es un problema en la práctica.

**El camino de un fallo en un NRO lanzado desde el hbmenu:**

1. El kernel guarda x0–x8, lr, sp, pc, pstate, afsr0/1, esr y far en la *ProcessLocalRegion*, de 0x200 bytes. Le
   deja al hilo una pila de unos 0x148 bytes dentro de esa región y salta al **punto de entrada del proceso** con
   `x0 = tipo` y `x1 = &exception_info`
   ([kern_exception_handlers.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/arch/arm64/kern_exception_handlers.cpp),
   [svc_types_common.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/svc/svc_types_common.hpp)).
2. Ese punto de entrada es el de **hbloader**. Su `__libnx_exception_entry` reenvía la excepción al punto de entrada
   del NRO cargado (`br g_nroAddr`)
   ([trampoline.s](https://github.com/switchbrew/nx-hbloader/blob/master/source/trampoline.s)). **Así que funciona
   con NROs del hbmenu.**
3. El `crt0` de libnx detecta que es una excepción (`x0 != 0` y `x1 != -1`) y salta a `__libnx_exception_entry`
   ([switch_crt0.s](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/switch_crt0.s),
   [exception.s](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/exception.s)). Esa rutina:
   - llama a `svcGetInfo(DebuggerAttached)`, una syscall más, y **aborta si hay un depurador conectado**, salvo que
     `__nx_exception_ignoredebug = 1`;
   - copia todo el contexto, incluidos los 32 registros Q, a `__nx_exceptiondump`;
   - cambia el `pc` guardado por `__libnx_exception_returnentry` y el `sp` por `__nx_exception_stack`, y llama a
     `svcReturnFromException(0)`.
4. El kernel restaura ese contexto y el hilo continúa en `__libnx_exception_returnentry`, que llama a
   **`__libnx_exception_handler(ThreadExceptionDump*)`**. Si esa función **vuelve**, libnx ejecuta `svcBreak`
   (cuelgue). Para seguir hay que **no volver**: restaurar los registros a mano y saltar al pc corregido, como
   hacen melonDS (`ARM_RestoreContext`) y flycast (`context_switch_aarch64`).

Sobre `svcReturnFromException`: el código 0 vuelve con el contexto que el programa dejó escrito. Un código distinto
de 0 repite la excepción y la pasa al depurador o al informe de errores
([SVC](https://switchbrew.org/wiki/SVC), `ReturnFromException` en
[kern_exception_handlers.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/arch/arm64/kern_exception_handlers.cpp)).

### 2.5 Hilos: el punto débil

- **En el kernel:** solo un hilo por proceso puede estar dentro del manejo de excepciones. Los demás esperan en
  `KProcess::EnterUserException`
  ([kern_k_process.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_process.cpp)).
- **En libnx:** la entrada llama a `svcReturnFromException` *antes* de ejecutar el manejador, así que el kernel ya
  ha soltado ese candado. Pero `__nx_exceptiondump` y `__nx_exception_stack` (0x400 bytes por defecto, ampliables
  porque los símbolos son débiles) son **globales y únicos**
  ([init.c](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/init.c)).
  **Si dos hilos fallan a la vez, uno pisa la pila y el volcado del otro.** melonDS no lo sufre porque solo falla
  el hilo del emulador.
- **Opción robusta**, que no he probado: sobrescribir el símbolo débil `__libnx_exception_entry` en el NRO.
  - Leer y modificar la `ExceptionInfo` que apunta `x1`: arreglar el pc y los registros (x0–x8, lr y sp están ahí;
    x9–x29 y los registros NEON siguen intactos en el hilo, como muestra que la entrada de libnx los lee directamente).
  - Llamar a `svcReturnFromException(0)`. El kernel continúa con el contexto editado y **mantiene el candado por
    proceso mientras tanto**: es seguro con varios hilos.
  - Todo debe caber en la pila de unos 0x148 bytes, o hay que cambiar a una pila propia de cada hilo.

### 2.6 ¿Cuánto cuesta un fallo?

**No he encontrado ninguna medida publicada.** Cuento solo los pasos:

- Con el camino estándar de libnx: entrada al kernel por el data abort, `svcGetInfo`, `svcReturnFromException`,
  la copia de unos 0x338 bytes de contexto (el `ThreadExceptionDump`, con 32 registros Q de 16 bytes) y la restauración en modo usuario. Son **al menos 3 cruces de la
  frontera usuario/kernel** y una toma del candado del planificador (`KScopedSchedulerLock`) en
  `EnterUserException`.
- Con una entrada propia: 2 cruces (el fallo y `svcReturnFromException`).

En cualquier caso es caro comparado con un acceso a memoria. Por eso todos los emuladores **parchean la instrucción
que falló** (*backpatching*): cada sitio falla una sola vez. **No hay que diseñar nada que dependa de fallar a menudo
en el camino caliente.** Para tener un número, ver §9.4.

### 2.7 Interacción con los depuradores

- Con gdb (el gdbstub de Atmosphère) o cualquier herramienta que se conecte como depurador al proceso, libnx
  **aborta** el manejo de la excepción salvo que `__nx_exception_ignoredebug = 1`
  ([exception.s](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/exception.s)).
- sys-botbase se conecta así a la **aplicación** (`svcDebugActiveProcess` en `attach()`,
  [commands.c](https://github.com/olliz0r/sys-botbase/blob/master/sys-botbase/source/commands.c)) para leer y escribir
  su memoria (§8.3). En modo applet nuestro NRO
  no es "la aplicación"; con el forwarder sí lo es.

---

## 3. Hilos y núcleos

- **Núcleos permitidos:** el NPDM de hbloader declara `lowest_cpu_id 0`, `highest_cpu_id 2` y prioridades de 28 a 59
  ([hbl.json](https://github.com/switchbrew/nx-hbloader/blob/master/hbl.json)). **El núcleo 3 no está disponible.**
  En el forwarder depende de su NPDM; si viene de hbloader, será lo mismo. El código actual ya consulta
  `InfoType_CoreMask` antes de fijar el núcleo (`cpu_thread_start()` en
  `NWiiRecomp/nWiiRuntime/src/platform/switch/switch_platform.cpp`), y eso está bien.
- **Prioridades:** de 0 (la más alta) a 63. Según la documentación de `threadCreate`, `0x2C` (44) es la habitual del
  hilo principal, y `0x3B` (59) "enables preemptive multithreading" en los núcleos 0–2 (`0x3F` en el 3)
  ([thread.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/thread.h)). **Bajo hbl en modo
  applet, Atmosphère baja la prioridad del hilo principal de 44 a 40**
  ([ldr_meta.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/stratosphere/loader/source/ldr_meta.cpp)).
- **Reparto del tiempo:** cada 10 ms el kernel solo rota la cola de la prioridad 59 en los núcleos 0–2 y la de la 63
  en el núcleo 3 (`DpcManagerPreemptionThreadFunction`,
  [kern_k_dpc_manager.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_dpc_manager.cpp),
  [kern_k_dpc_manager.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/include/mesosphere/kern_k_dpc_manager.hpp)).
  **En cualquier otra prioridad, un hilo que no se bloquea ni cede no suelta nunca el núcleo** a otro hilo de su
  misma prioridad. Con un hilo de CPU que gira sin parar (espera activa), hay que darle su propio núcleo o usar 59.
- **Ceder el núcleo:** `svcSleepThread` con `0`, `-1` y `-2` no duerme, cede
  ([kern_svc_thread.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_thread.cpp),
  [svc_types_common.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/svc/svc_types_common.hpp)):
  - `0`: cede sin cambiar de núcleo, **solo a hilos de la misma prioridad**.
  - `-1`: cede y deja migrar a otro núcleo. Es lo que usa `sched_yield()` de libnx
    ([newlib.c](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/newlib.c)).
  - `-2`: cede a cualquier hilo.
  - Una espera activa con `yield` (0 o -1) **no deja correr a hilos de menor prioridad** en ese núcleo.
- **`sched_getcpu()`** de libnx hace una syscall (`svcGetCurrentProcessorNumber`)
  ([newlib.c](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/newlib.c)). No conviene usarla en el
  camino caliente.
- **`svcSetThreadCoreMask`** es una syscall con candado del planificador y quizá una migración. No hay cifras; basta
  con llamarla una vez al empezar el hilo. También se puede crear el hilo ya en su núcleo con
  `threadCreate(..., cpuid)` y evitar la migración.
- **`svcGetThreadContext3`**, la que usa nuestro profiler:
  - Solo funciona sobre otro hilo del mismo proceso, y **solo si está pausado con `svcSetThreadActivity(h, Paused)`**;
    si no, devuelve `InvalidState`
    ([kern_svc_thread.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_thread.cpp),
    [KThread::GetThreadContext3](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_thread.cpp)).
  - `SetActivity(Paused)` **espera hasta que el hilo deja de estar en ejecución en todos los núcleos** (un bucle con
    el candado del planificador).
  - Cada muestra cuesta, por tanto, 3 syscalls (pausar, leer, reanudar) y **saca al hilo medido de su núcleo**. Con
    muchas muestras, el profiler cambia lo que mide.

---

## 4. Tiempos y sincronización

### 4.1 Relojes

- `armGetSystemTick()` es una sola instrucción `mrs cntpct_el0`, **sin syscall**. La frecuencia sale de
  `cntfrq_el0`: **19,2 MHz** (unos 52 ns por tick)
  ([counter.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/arm/counter.h)).
- `svcGetSystemTick` devuelve el mismo contador. La wiki dice que "Official sw reads cntpct_el0 directly from
  usermode without using this SVC" ([SVC](https://switchbrew.org/wiki/SVC)). **No hay motivo para usar la syscall.**
- `clock_gettime(CLOCK_MONOTONIC/REALTIME)`, `gettimeofday` y por tanto `std::chrono::steady_clock` en libnx calculan
  la hora como "hora de arranque + `armGetSystemTick()`": no hacen syscall, solo unas divisiones
  ([newlib.c](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/newlib.c)). La resolución que dan es
  `1e9/19.2e6`, unos 52 ns. **Ojo:** si el servicio de hora fallara al iniciar (`__boottime == UINT64_MAX`), ambas
  devuelven `EIO`.
- **Contadores de ciclos (PMU):** con `enable_user_pmu_access=1` en `exosphere.ini`
  ([exosphere.ini](https://github.com/Atmosphere-NX/Atmosphere/blob/master/config_templates/exosphere.ini)),
  Mesosphère pone `PMUSERENR_EL0 = 1` en cada núcleo
  ([kern_cpu.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/arch/arm64/kern_cpu.cpp)).
  Así se puede leer `PMCCNTR_EL0` (ciclos de verdad) desde el programa. **No lo he probado.** Supongo que los
  contadores son por núcleo y el kernel no los guarda en cada cambio de hilo, así que cuentan también lo que corra
  en ese núcleo. Es una opción solo para depurar, no para la release.

### 4.2 El coste de una syscall o de una IPC

**No he encontrado medidas publicadas para la Switch.** La búsqueda solo da cifras de la 3DS usadas por Citra, que
no sirven aquí. Lo que se puede afirmar:

- Una syscall cruza el kernel y muchas toman el candado del planificador (por ejemplo `SetActivity`,
  `EnterUserException`).
- Una IPC (`svcSendSyncRequest`) además despierta al hilo del servidor en *otro proceso*, que suele correr en el
  núcleo 3, y espera su respuesta. Es al menos un orden de magnitud más cara que una syscall simple. Esto último es
  razonamiento, no una medida.
- **GPU:** cada `nvIoctl` (envío de *pushbuffers* o fences de nvdrv) y cada `DequeueBuffer`/`QueueBuffer` de binder
  (vi/nvnflinger) es una IPC. Con Mesa/nouveau, cada *flush* que llega al kernel es al menos una IPC. Hay que
  agrupar envíos.
- Para tener números, ver §9.4.

### 4.3 mutex, condvar y eventos de libnx

- **`mutexLock`:** si el mutex está libre, `ldaxr/stlxr` y nada más. Si está ocupado, marca `HANDLE_WAIT_MASK` y
  llama **directamente** a `svcArbitrateLock`, **sin girar antes**. `mutexUnlock` solo llama a
  `svcArbitrateUnlock` si había alguien esperando
  ([mutex.c](https://github.com/switchbrew/libnx/blob/master/nx/source/kernel/mutex.c)). Con mucha contención,
  cada choque cuesta dos syscalls. **Para secciones muy cortas entre dos núcleos, un spinlock propio corto puede
  salir más barato.** Es deducción mía: hay que medirlo.
- **`condvarWake*`:** siempre llama a `svcSignalProcessWideKey`, aunque nadie espere. `condvarWait` llama a
  `svcWaitProcessWideKeyAtomic`
  ([condvar.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/condvar.h)). **Conviene no
  avisar en cada frame si no hay nadie esperando**: llevar una bandera atómica propia.
- `std::mutex`, `std::condition_variable` y pthreads de devkitA64 usan estas mismas primitivas a través de libnx.
- `ueventSignal` y `leventSignal` (eventos de usuario) también acaban en las syscalls de arbitraje cuando hay que
  despertar a alguien ([uevent.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/uevent.h),
  [levent.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/levent.h)).

---

## 5. Frecuencia de la CPU

### 5.1 Configuraciones oficiales (apm)

De la tabla `PerformanceConfiguration` ([PTM services](https://switchbrew.org/wiki/PTM_services)):

- Normal: **CPU a 1020 MHz** en todas las configuraciones de venta. La de 1224 MHz es solo para SDEV.
- `0x92220009` y `0x9222000A` (7.0.0+): **CPU a 1785 MHz, GPU a 76,8 MHz**, EMC a 1600 o 1331 MHz.
- `CpuBoostMode 1` ("FastLoad") usa precisamente esas: **sube la CPU y deja la GPU al mínimo**.
  `CpuBoostMode 2` solo baja la GPU.

libnx: `appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad)` (7.0.0+, comando 66 de `ICommonStateGetter`) y
`appletCancelCpuBoostMode` (10.0.0+)
([applet.c](https://github.com/switchbrew/libnx/blob/master/nx/source/services/applet.c),
[apm.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/apm.h)).

- **El problema:** a 76,8 MHz la GPU no da para renderizar el juego. El boost solo sirve para pantallas de carga.
- No he encontrado documentado si funciona como applet de librería (modo hbmenu).
  `SetCpuBoostModeForApplet` sí exige el indicador de depuración
  ([Applet Manager services](https://switchbrew.org/wiki/Applet_Manager_services)). El código actual ya registra si
  la llamada funciona: hay que mirarlo en el log.

### 5.2 La CPU a 1785 MHz sin tocar la GPU

Es lo que hace RetroArch para la Switch: `clkrstOpenSession(&s, PcvModuleId_CpuBus, 3)` y
`clkrstSetClockRate(&s, 1785000000)` (y `pcvSetClockRate` antes del 8.0.0). Lo vuelve a aplicar cuando recupera el
foco y baja a 1020 MHz cuando lo pierde
([platform_switch.c](https://github.com/libretro/RetroArch/blob/master/frontend/drivers/platform_switch.c)).
sys-clk usa las mismas llamadas `clkrst*`
([board.cpp](https://github.com/retronx-team/sys-clk/blob/master/sysmodule/src/board.cpp)).

- El NPDM de hbl da acceso a todos los servicios (`"service_access": ["*"]`,
  [hbl.json](https://github.com/switchbrew/nx-hbloader/blob/master/hbl.json)), así que el NRO puede abrir `clkrst`.
- 1785 MHz es la frecuencia que Nintendo usa en su propio boost. En ese sentido es "legítima"; el resto (1963 MHz o
  más) es overclock.
- **Riesgos:**
  - el sistema (PCV/apm) puede volver a poner su frecuencia cuando cambia la configuración (dock, foco), y por eso
    RetroArch la reaplica;
  - más calor y más consumo en modo portátil. No he encontrado cifras fiables.

### 5.3 Leer la frecuencia real (para el log)

- **8.0.0 o superior:** `clkrstInitialize()`, `clkrstOpenSession(&s, PcvModuleId_CpuBus, 3)`,
  `clkrstGetClockRate(&s, &hz)`, y lo mismo con `PcvModuleId_GPU` y `PcvModuleId_EMC`.
- **Antes del 8.0.0:** `pcvGetClockRate(PcvModule_CpuBus, &hz)`.
- Fuentes: [clkrst.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/clkrst.h),
  [pcv.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/pcv.h); sys-clk lee así sus
  frecuencias (`Board::GetHz`).
- `appletGetCurrentPerformanceConfiguration` (comando 91) devuelve el **id de la configuración**, no la frecuencia
  real. Si sys-clk u otro programa cambia la frecuencia, no se refleja ahí.
- Recomendación: registrar las tres frecuencias al arrancar y cada 5 s, junto a los fps.

### 5.4 sys-clk: modo applet frente a forwarder

- sys-clk elige el perfil por title ID usando `pmdmntGetApplicationProcessId` y `pminfoGetProgramId`. **Si no hay
  ninguna aplicación abierta, usa el TID del menú, `0100000000001000`**
  ([process_management.cpp](https://github.com/retronx-team/sys-clk/blob/master/sysmodule/src/process_management.cpp)).
- En la práctica:

  | Cómo se lanza | Perfil que aplica sys-clk |
  |---|---|
  | hbmenu desde el Álbum, sin juego abierto | `[0100000000001000]` (afecta también al menú HOME) |
  | hbmenu desde el Álbum, con un juego en segundo plano | el TID de ese juego |
  | "Title override" (abrir un juego manteniendo R) | el TID de ese juego |
  | Forwarder | **el TID del forwarder**: es lo más limpio |

- sys-clk solo toca una frecuencia si tiene un valor configurado (de una anulación temporal o del perfil). Si no,
  deja la del sistema ([clock_manager.cpp, Tick](https://github.com/retronx-team/sys-clk/blob/master/sysmodule/src/clock_manager.cpp)).
  Por eso no pelea con `clkrstSetClockRate` del juego salvo que haya un perfil para ese TID.
- Frecuencias de CPU disponibles en sys-clk oficial: hasta **1785 MHz** ("max clock, boost mode"). GPU portátil
  limitada a 460 MHz (614 en Mariko) y a 768 MHz con cargador USB
  ([README](https://github.com/retronx-team/sys-clk), `GetMaxAllowedHz` en clock_manager.cpp).

### 5.5 Límites de Erista y Mariko

- **Oficial:** 1785 MHz de CPU en los dos modelos (boost).
- **Switch-OC-Suite** (loader.kip parcheado y sys-clk-OC):

  | Modelo | CPU "segura" | GPU "segura" | DRAM "segura" | CPU/GPU desbloqueables (inseguro) |
  |---|---|---|---|---|
  | Erista | 1785 MHz | 921 MHz | 1862,4 MHz | hasta 2091 / 998 MHz |
  | Mariko | 1963 MHz | 998 MHz | 1996,8 MHz | hasta 2295 / 1267 MHz |

  El propio proyecto avisa de que "can possibly damage your console" y no lo recomienda
  ([Switch-OC-Suite](https://github.com/hanai3Bi/Switch-OC-Suite)).
- **Recomendación:** el port no debe depender de nada por encima de 1785 MHz.

---

## 6. Sistema de archivos (SD)

- **Velocidad:** Nintendo recomienda tarjetas UHS-I de 60–95 MB/s para la Switch original
  ([Nintendo AU](https://nintendo.com/au/support/articles/which-microsd-cards-are-supported),
  [Nintendo UK](https://www.nintendo.co.uk/Support/Nintendo-Switch/microSD-Card-FAQ-1500171.html)). No he
  encontrado una medida fiable de lectura desde homebrew. El techo del bus UHS-I SDR104 es de 104 MB/s; eso lo sé
  por conocimiento general, no por una fuente citada.
- **Cada `read()` es una IPC.** `fsdev_read` llama a `fsFileRead` con todo el tamaño pedido (una IPC a `fs`).
  **No hay caché en libnx:** solo el búfer de stdio de newlib si se usa `FILE*`.
- **Si la IPC devuelve `0xD401`** (memoria no apta para IPC), libnx repite la lectura en **trozos de 4 KB** con un
  búfer en la pila (`fsdev_read_safe`), es decir, una IPC por cada 4 KB
  ([fs_dev.c](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/devices/fs_dev.c)). **Con fastmem,
  no hay que leer directamente a una vista *SharedCode*:** se lee a un búfer del heap y se copia.
- **Recomendaciones:**
  - lecturas grandes (cientos de KB o MB) en vez de muchas pequeñas;
  - `setvbuf` con un búfer grande si se usa `FILE*`;
  - cargar al arranque o en un hilo aparte lo que se pueda (la caché de shaders, los archivos del disco).

---

## 7. Audio (audren)

- Un *frame* de audren son 240 muestras a 48 kHz (**5 ms**) o 160 a 32 kHz (también 5 ms)
  ([audren.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/audren.h),
  [audren.c](https://github.com/switchbrew/libnx/blob/master/nx/source/services/audren.c)).
- `audrenWaitFrame()` espera al evento de "frame nuevo".
- La latencia real la decide **cuántos búferes de onda (*wavebuf*) hay en cola por delante**: 5 ms por frame de
  audren más lo que haya encolado. No he encontrado una cifra oficial de la latencia de salida.
- El búfer de trabajo es memoria de transferencia creada con `Perm_None`: esa zona del heap queda inaccesible para
  el programa mientras dure la sesión.
- Encaja con AX de la Wii: unas 1000 interrupciones por segundo, es decir, 1 ms
  ([DEPURAR.md](../DEPURAR.md)). Hay que juntar 5 frames de AX por cada frame de audren, y no despertar un hilo
  por cada ms.

---

## 8. Automatización y depuración en la consola

### 8.1 nxlink (enviar, ejecutar y ver stdout)

- **Lado Switch:** en el hbmenu, pulsar **Y** abre el *netloader*.
- **En el PC:** `nxlink -s -a <ip> NSMBWnx.nro [--args ...]`.
  - `-s` deja un servidor escuchando para recibir stdout y stderr.
  - `-p` fija la ruta donde se sube el NRO.
  - `-r` es el número de reintentos.
  - Es el `nxlink` de devkitPro, en `/opt/devkitpro/tools/bin/nxlink` (opciones vistas con `nxlink --help`).
- **En el NRO:** `socketInitializeDefault()` y luego `nxlinkStdio()`. Los puertos son 28280 (servidor) y 28771
  (cliente) ([nxlink.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/runtime/nxlink.h)).
  El port ya lo tiene en `NWII_LOG = "nxlink"`.
- **Límite:** el NRO se lanza *desde el hbmenu*, es decir, en modo applet salvo que el hbmenu se abriera con
  "title override". Para probar en modo aplicación con nxlink, hay que abrir el hbmenu manteniendo R sobre un juego
  ([override_config.ini](https://github.com/Atmosphere-NX/Atmosphere/blob/master/config_templates/override_config.ini):
  `override_any_app=true`, tecla R).

### 8.2 FTP para copiar NROs, configuraciones y logs

- **sys-ftpd** (*sysmodule*):
  - configuración en `sdmc:/config/sys-ftpd/config.ini`, puerto **5000**;
  - **hay que poner usuario y contraseña** o no deja entrar;
  - se pausa y reanuda con PLUS+MINUS+X;
  - es más lento que el ftpd completo ([sys-ftpd](https://github.com/cathery/sys-ftpd)).
- **ftpd de mtheall:** es una app; más rápido para archivos grandes.

### 8.3 Mando virtual y capturas: sys-botbase y USB-Botbase

- **sys-botbase** (*sysmodule*, TID `430000000000000B`, puerto TCP **6000**)
  ([repo](https://github.com/olliz0r/sys-botbase), [toolbox.json](https://github.com/olliz0r/sys-botbase/blob/master/toolbox.json),
  [util.c](https://github.com/olliz0r/sys-botbase/blob/master/sys-botbase/source/util.c),
  [commands.md](https://github.com/olliz0r/sys-botbase/blob/master/commands.md)):
  - Comandos útiles para nosotros: `click A`, `press`/`release`, `setStick LEFT 0x7FFF 0x0`,
    `clickSeq A,W1000,B,...` (secuencias con esperas), `pixelPeek` (devuelve un JPG de la pantalla),
    `screenOff`/`screenOn`, `getTitleID`, `charge`.
  - Ajustes: `configure buttonClickSleepTime <ms>` (50 ms por defecto; no debe ser menos que un frame del juego) y
    `configure mainLoopSleepTime`.
  - Los comandos `peek`/`poke` trabajan sobre **la aplicación** y se conectan como depurador. En modo applet nuestro
    NRO no es la aplicación, y con el forwarder hay que tener en cuenta §2.7.
  - Se instala descomprimiendo la release en la raíz de la SD y reiniciando.
- **USB-Botbase:** la misma idea por USB, sin red (Python 3, pyusb y libusbK con Zadig en Windows). El protocolo no
  está documentado en el README y no consta que se mantenga ([repo](https://github.com/fishguy6564/USB-Botbase)).
- Las dos usan `hiddbg` (dispositivos virtuales "HDLS", desde el 7.0.0,
  [hiddbg.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/hiddbg.h)).
- **Alternativa más barata y reproducible:** que el propio port reproduzca entradas grabadas (un *replay*) sin
  pasar por HID. No necesita sysmodules y da exactamente las mismas entradas en el PC y en la Switch.

### 8.4 nxbt (mando Pro por Bluetooth desde Linux)

- Instalación con `sudo pip3 install nxbt`. Usa BlueZ y necesita root para desactivar el plugin de entrada de BlueZ.
- Macros desde la línea de órdenes:
  - `sudo nxbt macro -c "B 0.1s\n 0.1s" -r` (`-r` reconecta con una consola ya emparejada);
  - también desde la API de Python: `nx.macro(...)`.
- **Problemas conocidos:**
  - un adaptador Bluetooth solo puede ser un mando;
  - el menú "Cambiar orden/agarre" es delicado;
  - algunos menús van a 15 Hz;
  - el README habla de menos de 8 ms de retraso añadido en la webapp.
- La página no da fechas de la última versión: hay que comprobar que siga funcionando con el firmware actual
  ([nxbt](https://github.com/Brikwerk/nxbt)).
- Ventaja: **no necesita nada en la Switch**. Funciona igual en modo applet y en modo aplicación, y también sin CFW.

### 8.5 gdbstub de Atmosphère

- Está desde la 1.2.3. Se activa con `atmosphere!enable_standalone_gdbstub = u8!0x1` en `system_settings.ini`
  (requiere `enable_htc = 0`).
- Puerto **22225**. Desde el PC: `aarch64-none-elf-gdb`, luego `target extended-remote <ip>:22225`,
  `info os processes` y `attach <pid>`.
- Comandos útiles: `monitor get mappings` y `monitor wait application`.
- Soporta breakpoints software y hardware, watchpoints y paso a paso.
- Changelog: [1.2.3 y 1.6.0](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/changelog.md). La opción
  sigue en el código actual ([dmnt2_main.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/stratosphere/dmnt.gen2/source/dmnt2_main.cpp)).
- **Con gdb conectado, el manejador de excepciones de libnx se aborta** salvo `__nx_exception_ignoredebug = 1`
  (§2.7).
- Si el proceso se cuelga sin depurador, Atmosphère deja un informe en
  `sdmc:/atmosphere/crash_reports/<timestamp>_<programid>.log`
  ([creport_crash_report.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/stratosphere/creport/source/creport_crash_report.cpp)).
  En modo applet el program id será el del applet en el que corre hbl.

### 8.6 Que la consola no se duerma ni atenúe la pantalla

- `appletSetMediaPlaybackState(true)`: "screen dimming and auto sleep is disabled". Usa el comando de *Application*
  o el genérico según el tipo de applet ([applet.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/applet.h)).
- `appletSetAutoSleepDisabled(true)` (5.0.0+).
- `appletSetIdleTimeDetectionExtension(...)` (0 = desactivado, 1 = extendido, 2 = "ExtendedUnsafe").
- `appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend)`: solo para *Application*; RetroArch lo usa.
- Comandos de AM: [Applet Manager services](https://switchbrew.org/wiki/Applet_Manager_services).
- **Para las pruebas automáticas:** media playback y auto-sleep desactivados, y además `screenOff` de sys-botbase
  para no quemar la pantalla.

### 8.7 Log por USB (`usbComms`)

- `usbCommsInitialize()`, luego `usbCommsWrite(buf, n)` (bloqueante) o la versión asíncrona `usbCommsWriteAsync`
  ([usb_comms.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/runtime/devices/usb_comms.h)).
- En el PC hace falta un lector propio con libusb. Ocupa la interfaz USB de dispositivo, así que **no se puede usar a
  la vez que USB-Botbase**.
- Útil si la Wi-Fi da problemas o no se quiere que los sockets afecten a las medidas.

---

## 9. Fortalezas, debilidades y recomendaciones

### 9.1 Fortalezas

- **Fastmem:** reservar 4 u 8 GB, mapear vistas con espejos y recuperarse de los fallos funciona desde el hbmenu con
  Atmosphère. melonDS es el ejemplo (§2).
- **Relojes y medidas de tiempo baratos:** `armGetSystemTick` y `clock_gettime` sin syscall (§4.1).
- **Sincronización barata cuando no hay contención:** el mutex no entra al kernel (§4.3).
- **Hbloader da al NRO todas las syscalls y servicios:** `clkrst`, `hiddbg`, memoria de código, el handle del
  propio proceso.
- **Hay herramientas maduras para automatizar:** nxlink, ftpd, sys-botbase, nxbt y gdbstub.

### 9.2 Debilidades

- **Solo 3 núcleos.** Sin reparto de tiempo salvo en la prioridad 59: un hilo con espera activa bloquea a los de su
  misma prioridad.
- **El boost oficial mata la GPU.** Para pasar de 1785 MHz hay que hacer overclock con riesgo.
- **Poca memoria en modo applet:** el pool compartido es de 507 MB, y de ahí salen Atmosphère y la reserva de
  134 MB.
- **El manejador de excepciones de libnx** no es seguro con varios hilos y se desactiva con un depurador.
- **IPC caras:** el sistema de archivos, la GPU y la presentación pasan por IPC. Y no hay cifras publicadas.

### 9.3 Recomendaciones concretas para el port

1. **Medir siempre con `armGetSystemTick()`.** `std::chrono::steady_clock` también vale, porque usa el mismo
   contador. Nunca usar `svcGetSystemTick` ni `sched_getcpu()` en el camino caliente.
2. **Ninguna syscall por instrucción ni por acceso de la Wii.** Hay que revisar si el bucle del juego llama a
   `condvarWake`, `svcSleepThread(0)`, `ueventSignal`… en cada iteración, y protegerlo con banderas atómicas.
3. **Reparto de núcleos:**

   | Núcleo | Qué corre ahí |
   |---|---|
   | 1 (o 2) | el hilo de la CPU recompilada, **solo** (como ahora); se puede crear directamente ahí con `threadCreate(..., cpuid)` |
   | 0 | el hilo principal (render y audio) |
   | 2 | hilos auxiliares (compilación de shaders, E/S); sin espera activa a la misma prioridad que otro hilo de ese núcleo |

   El núcleo 3 no está disponible.
4. **Frecuencia:**
   - Para jugar: CPU a 1785 MHz con `clkrstSetClockRate(PcvModuleId_CpuBus, 1785000000)`, dejando la GPU como
     está.
   - Reaplicarla al recuperar el foco y volver a 1020 MHz al perderlo (como RetroArch).
   - Dejar `appletSetCpuBoostMode` solo para las pantallas de carga.
   - Registrar las frecuencias reales de CPU, GPU y EMC con `clkrstGetClockRate`.
   - Si el usuario tiene sys-clk, darle el TID del forwarder en la documentación.
5. **Fastmem (si se quiere):**
   - seguir la receta de melonDS (§2.2);
   - parchear la instrucción que falló (*backpatching*) para que nada falle dos veces;
   - entrada de excepción propia (§2.5) si puede fallar más de un hilo;
   - `__nx_exception_stack` más grande (melonDS usa 0x8000);
   - `__nx_exception_ignoredebug = 1` solo en compilaciones de depuración.
6. **E/S:** lecturas grandes a búferes del heap (nunca a vistas *SharedCode*) y precarga en otro hilo.
7. **Memoria:** registrar Total/UsedMemorySize al arrancar en los dos modos (applet y forwarder) y avisar al jugador
   si está en modo applet con poca memoria.
8. **Profiler:** que `svcGetThreadContext3` no muestree a más de unos pocos cientos de Hz. Como
   alternativa, contadores con `armGetSystemTick` dentro del propio código, o PMU con `enable_user_pmu_access`.
   Lo de los pocos cientos de Hz es un criterio mío, no una cifra medida.

### 9.4 Medir lo que falta (microbenchmark para el `.nro` de pruebas)

Con `armGetSystemTick()` y N = 100000 repeticiones, sin otros hilos y registrando la frecuencia de la CPU:

| Qué | Cómo |
|---|---|
| Syscall vacía | `svcGetInfo(&x, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)` o `svcGetThreadPriority` |
| Ceder | `svcSleepThread(0)` y `svcSleepThread(-1)` con un solo hilo |
| Mutex sin contención | `mutexLock`/`mutexUnlock` |
| Mutex con contención | ping-pong de un mutex entre los núcleos 1 y 2 |
| Avisar sin nadie esperando | `condvarWakeOne` |
| IPC | `appletGetCurrentPerformanceConfiguration` (una IPC simple a `am`) |
| Fallo de memoria | leer `base + 0x10000000` (sin mapear) con el manejador que solo suma 4 al pc y vuelve; con la entrada de libnx y con una entrada propia |
| Lectura de la SD | `fsFileRead` de 4 KB, 64 KB y 1 MB, en caliente y en frío |

Así se pueden sustituir por medidas reales las estimaciones de §2.6 y §4.2.
