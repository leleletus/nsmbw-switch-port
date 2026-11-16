# Proyecto de terceros del que parte este trabajo

- **Proyecto:** NWiiRecomp (recompilador estático Wii → C++ nativo: `nWiiAnalyzer`, `nWiiRecomp`, `nWiiRuntime`).
- **Origen:** https://github.com/BlackLineInteractive/NWiiRecomp (autor: Volodymyr Vovchok / BlackLine Interactive).
- **Commit de partida:** `595f176f1d24cc54ff2e8389feed12d7fb553cc2` («Update README.md», 7-ago-2026).
- **Licencia:** «Custom License (Modified MIT with Commercial Restrictions)». Texto íntegro en
  [`LICENSE_NWiiRecomp`](LICENSE_NWiiRecomp). Resumen (léela entera, no es MIT estándar): uso solo no comercial,
  atribución visible al autor, las obras derivadas no pueden tener términos menos restrictivos y **se prohíbe usar el
  software para entrenar modelos de IA**.

## Qué hay aquí

No se copia el proyecto original (hay que clonarlo aparte, en ese commit). Solo lo que se cambió en local sin commit:

- `ficheros_modificados/`: los ficheros completos modificados de `nWiiAnalyzer/` y `nWiiRecomp/` (analizador y recompilador).
- `cambios_locales_completos.patch`: `git diff` completo contra ese commit (analizador, recompilador y runtime;
  45 ficheros, ~8 800 líneas añadidas).
- El runtime final (incluye los ficheros nuevos no versionados: `wii_remotes`, `nwii_env.h`, `hle/ax/`, `wpad_hle.cpp`,
  `platform/switch/`) está entero en [`../runtime/nWiiRuntime`](../runtime/nWiiRuntime); no depende de la carpeta
  `third_party` del original, que NO se incluye.

Para reconstruir: clonar el original en el commit indicado, aplicar `cambios_locales_completos.patch`
(`git apply`), y añadir los ficheros nuevos desde `runtime/nWiiRuntime`.
