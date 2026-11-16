# NSMBW nativo: documentación

Port nativo de **New Super Mario Bros. Wii** (USA, `SMNE01`) para PC Linux y Nintendo Switch (homebrew),
hecho por **recompilación estática** con NWiiRecomp.

El código PowerPC del juego se traduce una vez a C++ y se compila como un programa normal (x86-64 o ARM64).
Las partes del sistema de la Wii se reimplementan en el runtime: gráficos GX → OpenGL, audio, Wiimotes y NAND.
No es un emulador.

> **Este repositorio no incluye el juego.** Para generar el código y para jugar hace falta tu propio
> disco NSMBW USA (SMNE01, revisión 1) extraído. No subas a GitHub `game/`, `bin_usa/`, `recomp/export/`
> (es código derivado del juego), `nand/`, ni binarios compilados.

## Documentos

| Documento | Para quién | Contenido |
|---|---|---|
| [JUGAR_PC.md](JUGAR_PC.md) | jugadores en Linux | paquete listo, controles, multijugador, reportar fallos (F11/F12) |
| [JUGAR_SWITCH.md](JUGAR_SWITCH.md) | jugadores en Switch | carpeta de la SD, cómo abrirlo, mandos y Joy-Con, `config.toml` |
| [COMPILAR_PC_LINUX.md](COMPILAR_PC_LINUX.md) | desarrolladores | dependencias, extraer el disco, recompilar, compilar el ejecutable de PC, empaquetar |
| [COMPILAR_SWITCH.md](COMPILAR_SWITCH.md) | desarrolladores | devkitPro/libnx, compilar el `.nro`, armar la carpeta de la SD, citron |
| [DEPURAR.md](DEPURAR.md) | desarrolladores | `debug.toml`, variables `NWII_*`, capturas, pruebas sin ventana, Dolphin como referencia |
| [KIT_PORTS_WII.md](KIT_PORTS_WII.md) | desarrolladores | diseño de un kit para portear otros juegos de Wii |
| [GUIA_PORTS_RECOMPILACION.md](GUIA_PORTS_RECOMPILACION.md) | desarrolladores | lecciones y consejos para futuros ports (Wii, 3DS, en general): traductor, runtime, gráficos, pruebas 1:1 |
| [switch/README.md](switch/README.md) | desarrolladores | referencia técnica de la Switch (CPU, sistema, GPU, benchmark real, funciones calientes) y plan de rendimiento R10 |

## Estructura del proyecto

```
NWiiRecomp/        recompilador + runtime (rama nsmbw con los cambios de este port)
recomp/            nsmbw.toml (config del recompilador), build.sh, export/ (C++ generado, NO se sube)
switch/            proyecto del .nro (cmake/, nsmbw/, runtime/, build.sh, sync_sources.sh)
perf/              apply_fastdispatch.py (paso posterior obligatorio) + experimentos de rendimiento
scripts/           utilidades (lim, estado.sh, dolphin_rels.py, dolcg.py ...)
oracle/            comparación 1:1 con Dolphin
docs/              esta documentación
```
