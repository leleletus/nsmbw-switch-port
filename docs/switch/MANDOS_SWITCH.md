# Mandos en Switch (homebrew con libnx): lo aprendido en NSMBW

Problemas reales vistos en la consola del usuario (8-nov-2026) y cómo se resolvieron. Aplica a cualquier port
homebrew de Switch que use libnx + SDL2 de devkitPro. Código: `nWiiRuntime/src/platform/switch/switch_input.cpp`
y `src/core/main.cpp` (r10e/r10f).

## Resumen de reglas

| Regla | Por qué |
|---|---|
| **Usa una sola fuente de mandos: libnx (`pad*`/`hid*`) o SDL, nunca las dos.** | El driver de mandos de SDL2 para Switch configura 8 jugadores y abre por su cuenta el selector del sistema (`hidLaShowControllerSupportForSystem`, función `SWITCH_UpdateControllerSupport` de `libSDL2.a`) cuando cambian los mandos. Si tú también abres el tuyo, salen dos o tres selectores seguidos, uno de ellos con **8 casillas**. Si lees los mandos con libnx, arranca SDL **sin** `SDL_INIT_GAMECONTROLLER`/`SDL_INIT_JOYSTICK`. |
| **Configura exactamente los jugadores que el juego admite**: `padConfigureInput(N, HidNpadStyleSet_NpadStandard)` y `player_count_max = N` en `HidLaControllerSupportArg`. | Con N mayor, el sistema y el selector ofrecen más mandos de los que el juego puede usar. Reafírmalo justo antes de abrir el selector, por si otra biblioteca lo cambió. |
| **Joy-Con suelto en horizontal: gira el stick tú mismo.** | `hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal)` y `hidGetNpadJoyHoldType()` decían "horizontal", pero en la consola real el stick de un Joy-Con suelto llegaba **sin girar**: el jugador movía a Mario como si el Joy-Con estuviera en vertical. La solución fiable es girarlo a mano según el estilo del mando (abajo). |
| **No abras el selector en cuanto un mando "se pierde": espera ~1,5 s.** | Al acoplar o desacoplar Joy-Con la consola desconecta un instante todos los mandos mientras los reasigna. Abrir el selector en ese momento apilaba varios selectores (dos fallaban con `rc=0x5d59`) y dejaba el par de Joy-Con separado, sin poder volver a jugar en portátil con los dos. |
| **Deja al menos ~3 s entre un selector y el siguiente**, y no lo abras si el jugador 1 tiene el mando portátil (Joy-Con en la consola). | Evita la cascada de selectores. |
| **Un solo selector a la vez**: ninguno nuevo mientras uno está pendiente o abierto, ni hasta ~3 s después; al cerrarse, olvida los "mando perdido" anteriores. | Dos causas reales de selectores repetidos: (1) mientras el selector está abierto el juego se para (espera al hilo de render) y, al volver, su petición de sincronizar (que repite cada frame) parece nueva; (2) al abrir el selector el sistema reasigna los Joy-Con y eso parece un "mando perdido". |
| **Cuando el juego tiene su propia pantalla de "añadir mando", ciérrala tú al terminar el selector.** | En Wii el jugador pulsa + al acabar de sincronizar. Tras el selector de Switch el jugador no sabe que debe pulsar +: se inyecta un toque corto de + en el jugador 1 (r10f). |
| **Muestra el selector con colores y nombres por jugador** (`enable_identification_color`, `hidLaSetExplainText`). | Se ve como un juego de Switch: en NSMBW, Mario/Luigi/Toad/Toad con sus colores. |

## Girar el stick de un Joy-Con suelto (sostenido en horizontal)

Valores del stick en coordenadas "vertical" `(vx, vy)` (y hacia la punta con el botón L/R):

| Mando | Cómo se sostiene | Pantalla |
|---|---|---|
| Joy-Con izquierdo | la punta L hacia la izquierda, el riel hacia arriba | `x = -vy`, `y = vx` |
| Joy-Con derecho | la punta R hacia la derecha, el riel hacia arriba | `x = vy`, `y = -vx` |

Los botones también cambian de sitio al sostenerlo de lado: en el izquierdo, las flechas hacen de botones de cara
(Izquierda abajo, Abajo a la derecha, Arriba a la izquierda, Derecha arriba); SL/SR quedan arriba. En NSMBW,
`NWII_NX_JOY_ROTATE=0` desactiva el giro y `=auto` vuelve a fiarse del sistema.

## Abrir el selector en el momento justo

- Lo que funciona: el juego llama a una función conocida cuando muestra su pantalla de sincronizar (en el SDK de
  Wii, `WPADStartSimpleSync`). Se engancha esa función (HLE) y se abre el selector.
- Lo que **no** sirvió en NSMBW:
  - el interruptor de "aceptar mandos nuevos" (`WPADSetAcceptConnection`, de `dRemoconMng_c::execute`): está
    activo desde el título;
  - la creación del objeto de la pantalla de número de jugadores (`NUMBER_OF_PEOPLE_CHANGE`, `classInit` en
    `0x8079F630`): se crea en cada cambio de escena, no al elegir "2 o más".
- Para encontrar el punto justo de otra pantalla: buscar su perfil o layout en el decomp
  (`alias_db.txt`, `files/Layout/*.arc`), resolver la dirección con las relocaciones del REL (script en
  `docs/switch/FUNCIONES_CALIENTES_NSMBW.md` y en esta sesión: tabla de imports del REL, tipo 1 = ADDR32) y
  **comprobar en PC** con `NWII_TRACE_CALLS=1 NWII_TRACE_ONLY=<dirección>` y la entrada guionizada cuándo se llama.
  No enganchar nada sin esa comprobación.
- Siempre debe haber además un atajo manual (en NSMBW: L3+R3, o SL+SR+stick en un Joy-Con suelto).

## Cómo diagnosticar en la consola

- Escribe en el log cada evento de mandos: conectado/perdido por jugador, estilo (portátil, par, Joy-Con L/R, Pro),
  cada selector abierto (motivo, `rc`, jugadores que devolvió) y el tipo de agarre que informa el sistema.
- Busca en el binario quién llama al selector del sistema:
  `aarch64-none-elf-nm libSDL2.a | grep -i hidLa` mostró la llamada oculta de SDL.
- Prueba siempre: portátil, Joy-Con en par desacoplado, un Joy-Con suelto por jugador, mando Pro en dock, y acoplar y
  desacoplar a mitad de partida.
