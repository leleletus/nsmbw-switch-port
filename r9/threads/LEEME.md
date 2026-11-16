# r9:threads — audio AX y FIFO de GX fuera del hilo del juego, rebasado sobre r8 (31-oct-2026)

@@RESUMEN@@

## Entregables

| Qué | Dónde |
|---|---|
| Parche del runtime, contra `r9/base/nWiiRuntime` | `r9_threads.patch` (desde `nWiiRuntime/`: `patch -p2 < r9_threads.patch`; comprobado sobre una copia limpia de `r9/base/nWiiRuntime`: sale idéntico a `nWiiRuntime/` de esta carpeta) |
| Cambios del arnés de pruebas (no van en el juego) | `r9_threads_harness.patch` (contra `r9/base`: `build_nx.sh` y `tools/`) |
| Código | `nWiiRuntime/` |
| PC | `bin/NSMBW_t9d`, `bin/NSMBW_t9d_input_test` (solo pruebas) |
| Switch | `bin/nsmbw_t9d.nro`; `bin/nsmbw_t9d_input_test.nro` (solo citron, **nunca a la Switch real**) |
| Pasos intermedios (solo para comparar) | `t9a` = rebase puro de r8:threads; `t9b` = + nueva regla de vaciado del FIFO; `t9c` = + camino rápido de escritura; `t9d` = + barrera del DMA de audio (el entregable) |
| Pruebas | `verify/chain5.sh` (cadena de citron que espera a que la máquina esté en calma), `verify/pc_chain5.sh` (perfil sw en PC, ABBA), `verify/pc_verify_all.sh`, `verify/pc_verify_ax.sh`; corridas en `runs/` (citron), `pc/runs/` y `pc/prof/` (PC) |
| Herramientas | `tools/nx_thread_diag.cpp` (registro `[THR]` de hilos y núcleos, solo en los `_input_test.nro`), `tools/game_thread_cats.sh` (reparto del hilo del juego en un perfil), `tools/pc_cpu_per_inst.py` (CPU del hilo del juego por instrucción del juego) |

No se toca `export/` ni el traductor ni ninguna cabecera que incluya el código generado: basta la compilación rápida.

```bash
JOBS=3 ~/nsmbw-port/scripts/lim ~/nsmbw-port/r9/threads/build_pc.sh ~/nsmbw-port/r9/threads/bin/NSMBW_x
JOBS=3 ~/nsmbw-port/scripts/lim ~/nsmbw-port/r9/threads/build_nx.sh ~/nsmbw-port/r9/threads/bin/nsmbw_x.nro
```

@@CUERPO@@
