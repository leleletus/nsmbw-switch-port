# Workspace NSMBW -> Switch

- game/SMNE01/        Juego extraído (USA). sys/main.dol, files/rels/*.rel.LZ
- bin_usa/            main.dol (como wiimj2d.dol) + 4 RELs descomprimidos (USA) -> para Ghidra/REA
- NSMBW-Decomp/       Decomp oficial de la comunidad (solo PALv1). compilers/ ya instalado
- NSMBW-Decomp/original/  VACÍO: necesita main.dol + RELs de un disco PALv1 (SMNP01, md5 en README)
- tools/wibo          Ejecuta CodeWarrior (mwcceppc.exe) en Linux
- tools/ghidra_12.1.4_PUBLIC/  Ghidra + extensión GameCubeLoader (DOL/REL)
- ghidra_projects/    Proyectos de Ghidra
- env.fish            `source env.fish` para PATH/variables

Compilar decomp (cuando tengas PALv1 en original/):
  cd NSMBW-Decomp; ./configure.py; ninja; ./progress.py --verify-bin

## Recompilación estática (NWiiRecomp)
- NWiiRecomp/build/nWiiRecomp/nwiirecomp   recompilador (PPC -> C++)
- recomp/nsmbw.toml                         config
- recomp/export/                            C++ generado (~15k funciones) + runtime
- recomp/export/build/NSMBW                 ejecutable nativo x86_64

Regenerar:  cd recomp; rm -rf export; ../NWiiRecomp/build/nWiiRecomp/nwiirecomp nsmbw.toml
Compilar:   cd export; cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSDL_PIPEWIRE=OFF -DCMAKE_C_FLAGS="-Wno-error=incompatible-pointer-types"; ninja -C build
Ejecutar:   NWII_HEADLESS=1 ./export/build/NSMBW ~/nsmbw-port/game/SMNE01 nsmbw.toml

Compilar rápido:  recomp/build.sh        (solo export/)
                  recomp/build.sh sync   (copia cambios del runtime a export/ y recompila)
                  recomp/build.sh regen  (re-traduce DOL+RELs; compilación completa)

Cambios propios en NWiiRecomp (rama git `nsmbw`):
- nWiiRuntime/src/hle/wpad_hle.cpp  Wiimote virtual de lado (botones, inclinación, sacudida, vibración)
- nWiiRuntime/src/hw/hw_ai.cpp      AI con bits reales + espejo 0xCD006C00
- nWiiRuntime/src/hw/hw_ipc.cpp     IPC acotado a sus registros, resto 0xCD00xxxx silencioso
- nWiiRecomp/src/recompiler.cpp     declara las funciones de [hle_hooks]

Herramientas:
- scripts/dolcg.py         call-graph/desensamblado del DOL (tools/venv/bin/python -I scripts/dolcg.py ...)
- scripts/dolphin_rels.py  lee de Dolphin en ejecución dónde quedaron los RELs (-> [[rel_aot]])

Controles PC: flechas mover · X saltar(2) · Z correr(1) · C sacudir · Q/E inclinar · Enter(+) · Backspace(-) · H(HOME)
Mando: cruceta/stick · A/B saltar · X/Y correr · R/ZR sacudir · sensor o stick derecho inclinar
Variables: NWII_HEADLESS=1 (sin ventana) · NWII_WPAD_AUTO=1 (pulsa 2+A solo, pruebas) · NWII_TILT_INVERT=1

Estado 2026-10-22: Bluetooth y audio (AI/AX) superados; el juego entra a su bucle y dibuja
(~10 draws/frame, proyección 2D 608x456) pero la imagen sale negra (efb_nonblack=0), ~20 fps.
RELs añadidos a rel_aot con bases de Dolphin. Siguiente: depurar renderer GX (pantalla negra).

Estado 2026-10-22 (tarde): llega al TÍTULO y al menú de partidas, en español, sin error de NAND.
  Ejecutar:  cd recomp; env NWII_REALTIME=1 ./export/build/NSMBW ~/nsmbw-port/game/SMNE01 nsmbw.toml
  2D (layouts/texto) OK; 3D (G3D: Mario/Luigi/Toads, niveles, cinemáticas) NO se dibuja aún.
  Velocidad ~30 fps del juego con NWII_REALTIME=1 (sin él ~13).
  Guardado: ~/nsmbw-port/nand/ · Idioma: [system] language en nsmbw.toml
  En curso: renderer 3D (render/), rendimiento 30->60 (perf/).
