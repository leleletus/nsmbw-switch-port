# r25dec: frente D — ¿sustituir las funciones más calientes por versiones nativas sacadas de NSMBW-Decomp? (10-nov-2026)

Solo investigación: sin compilar, sin tocar otros frentes, ni `nand/`, ni la consola. Sin commits.

> **Resumen.** El hilo del juego está muy repartido (las 60 funciones más calientes = 34 % del tiempo ocupado) y
> casi nada de lo caliente está en la decomp: **las funciones decompiladas suman ~10 % del hilo ocupado**
> (623 funciones, la mayor 0,87 %), y **de las 60 más calientes solo 3 están decompiladas (1,7 % del hilo)**.
> Lo caro es nw4r (31 %), RVL SDK (20 %), código del juego **sin** decompilar (17 %), MSL (6 %) y EGG (3 %); la decomp
> solo cubre ~17 % del `.text` del DOL (bases, framework, mLib, sLib/cLib) y nada de g3d/snd/ef/GX/MTX/OS/AX.
> Con lo que midió el frente N (nativo por llamada ×1,0–×1,15 en código de enteros/punteros, ×1,4–×1,6 en coma
> flotante hoja), la ganancia realista es **0,2–0,5 ms/frame con 5-10 candidatas** y **≤ 1,3 ms aun portando toda la
> decomp caliente**, frente a los ~9,3 ms que faltan (26 → 16,7 ms). **Recomendación: no abrir D como frente
> principal.** Como mucho un piloto barato (familia `searchNodeByID` con índice nativo + `dCc_c`) si sobra tiempo;
> la decomp sirve más como **mapa de tipos** (cabeceras de `dBc_c`, `dBg_ctr_c`, nw4r g3d...) para escribir nativas
> a mano de funciones no decompiladas (estilo N8+ de r19n) que como código fuente para pegar.

## 1. De dónde salen los números

- Perfiles de la consola **más nuevos a 1020 MHz** (portátil, 720p, lista fija de escenas de S):
  `switch/real_logs/bench_F01_r7f_lista1020_1110_0132/profile_cpu.txt` (350 s, 220 791 muestras) y
  `switch/real_logs/bench_E02_dtorre_on_1110_0119/profile_cpu.txt` (320 s, 217 502). `thread cpu` = hilo del juego.
- ELF: los dos dicen `built Nov  9 2026 06:12:19` → `r9/r22r7/bin/nsmbw_r7f.elf` (md5 b4334147…, = `nx/hot/r7f_dk3drel/hot`;
  la cabecera `build` del perfil es la hora del archivo del muestreador, no del binario).
- `tools/rank.py <nm> <perfiles...>`: PC → función con `nm`; quita lo ocioso (`svcSleepThread`, `idle_wait`: 34-38 %
  de las muestras); a cada función invitada (`func_8XXXXXXX` / `nombre_8XXXXXXX`) le suma sus muestras propias y
  las de los ayudantes del runtime (`psq_load/store`, `ps_madds*`, `GX_WGPIPE_*`, `MMU::*_slow`...) cuyo LR cae en ella;
  media de los dos perfiles en % del hilo **ocupado**. Código invitado = 80 % propio + ~10 % por ayudantes; el resto
  es runtime puro (svcSendSyncRequest, interrupciones, reverb A2...).
- `tools/decomp_map.py` (con `tools/venv`, reutiliza `scripts/sym_match.py`): para cada objeto compilado de la decomp
  (`symbols/decomp_build`) y su rebanada PAL (`slices/*.json`) calcula el desplazamiento PAL→USA con las funciones que
  `sym_match` ya casó y saca el rango USA de cada rebanada y la dirección USA de cada función → `datos/decomp_map.tsv`.
- `tools/table.py <dir> N`: "decompilada = sí" si la dirección USA es una función de la decomp con origen
  `match`/`layout` en `symbols/usa_symbols.txt` (patrón de instrucciones idéntico, con las reubicaciones enmascaradas)
  o la dirección exacta calculada; "¿rango?" si solo cae dentro del rango USA de una rebanada decompilada.
  Las sin nombre llevan "tras <símbolo conocido>+desplazamiento" para ubicarlas.
- Comprobación con otro perfil/escena: `bench_m1_perfil4j_1108_2330` (m1, 4 jugadores, `r18m/bin/nsmbw_m1.elf`):
  9,9 % del hilo en decompiladas, 1,3 % en las 60 primeras (`datos/table_m1_perfil4j.tsv`). Mismo cuadro.

Ficheros: `datos/rank.tsv` (todas, 3 042 funciones: dir, % total, % propio, % ayudantes), `datos/table5000.tsv`
(con nombre, estado decomp y archivo), `datos/top60.md` (la tabla de abajo).

## 2. Reparto del hilo del juego ocupado (r7f, 1020 MHz)

| Zona (por dirección USA) | % del hilo ocupado | funciones con muestras | decompilado |
|---|---|---|---|
| nw4r (g3d, snd, ef, lyt, math, ut) | 30,6 | 845 | 0,2 (solo `ut_list`) |
| juego DOL | 27,0 | 1 348 | **10,0** |
| RVL SDK (GX, MTX, OS, AX, DC) | 20,3 | 284 | 0 |
| MSL / runtime CW (strcmp, memcpy, fdlibm, `__ptmf_scall`, savegpr) | 5,9 | 71 | 0,05 |
| EGG | 3,2 | 195 | 0 |
| juego REL (d_profileNP, d_basesNP, d_enemiesNP, d_en_bossNP) | 0,9 | 262 | ~0 |
| HBM + bucle ocioso 801B4EA0 | 1,0 | | 0 |
| **total invitado** | **~90** | 3 042 | **~10,2** (+0,2 dudoso por rango) |

Acumulado: las 10 más calientes = 13 %, 20 = 20 %, 60 = 34 %, 100 = 42 %, 400 = 68 %.
Decompiladas: las 10 más calientes de la decomp = 2,9 %, 30 = 4,8 %, 100 = 7,5 %, todas (623) = 10,2 %.
Por archivo PAL: `d_a_player_base.cpp` 1,78 % (116 funciones), `d_a_player.cpp` 1,48 % (146), `f_line.cpp` 1,01 % (2),
`m_3d/mdl.cpp` 0,79 % (11), `d_cc.cpp` 0,67 % (12), `d_actor.cpp` 0,33, `f_base.cpp` 0,33, `f_tree.cpp` 0,24, resto < 0,23.

## 3. Las 60 más calientes

"% ocupado" = media de F01 y E02; "propio / ayudantes" = muestras con el PC en la función / en ayudantes del runtime
llamados desde ella. "frente N" = estado en `r9/r19n/runs/ranking_lib.md` (N2-N5 existen en r19n, **no** en r7f:
PSMTXConcat pesa igual que en m1).

| # | USA | bib. | función | % ocupado | propio / ayudantes | decomp | archivo PAL | frente N (r19n) |
|---|---|---|---|---|---|---|---|---|
| 1 | `8000D000` | juego DOL | (sin nombre; tras findWindowByName__Q23d2d7Multi_cFPCc+0x5b90) | 2.07 | 0.54 / 1.53 | no | – |  |
| 2 | `800ECAE0` | juego DOL | (sin nombre; tras create__6mVideoFv+0x4030) | 1.84 | 1.71 / 0.13 | no | – |  |
| 3 | `801C0540` | SDK | PSMTXConcat | 1.59 | 0.76 / 0.82 | no | – | **N2** nativa rápida (float, host) |
| 4 | `802403C0` | nw4r | (sin nombre; tras GXSetTevKColor__Q34nw4r3g3d14ResMatTevColorF14_GX…) | 1.24 | 0.89 / 0.34 | no | – | **N4** nativa (cálculo exacto en double, GQR… |
| 5 | `802E8868` | MSL/rt | (sin nombre; tras fmod+0x254) | 1.23 | 1.17 / 0.06 | no | – |  |
| 6 | `801A2B10` | SDK | (sin nombre; tras fn_8019ee20__9Snd2DCalcFRfRQ34nw4r4math4VEC2Ul+0x…) | 1.08 | 1.06 / 0.02 | no | – | __AXSyncPBs: N4 |
| 7 | `80237BD0` | nw4r | SinFIdx__Q24nw4r4mathFf | 1.03 | 0.87 / 0.16 | no | – | **N2** nativa rápida (float, host) |
| 8 | `802821E0` | nw4r | (sin nombre; tras detail_PrepareSound__Q34nw4r3snd14SoundStartableF…) | 1.02 | 1.02 / 0.00 | no | – | pendiente |
| 9 | `801C9A80` | SDK | GXLoadTexMtxImm | 0.98 | 0.24 / 0.73 | no | – | **N2** nativa rápida (float, host) |
| 10 | `80250E70` | nw4r | (sin nombre; tras Invalidate__Q34nw4r3g3d8G3DStateFUl+0x3890) | 0.89 | 0.87 / 0.02 | no | – | pendiente |
| 11 | `801AC440` | SDK | (sin nombre; tras OSSetMEM1ArenaLo+0x550) | 0.87 | 0.71 / 0.16 | no | – | N1 a mano |
| 12 | `80162C30` | juego DOL | searchNodeByID__9fLiMgBa_cCF9fBaseID_e | 0.87 | 0.81 / 0.06 | **sí** | dol/framework/f_line.cpp |  |
| 13 | `802E1AB4` | MSL/rt | strcmp | 0.75 | 0.72 / 0.03 | no | – | **N5** nativa (los tres bucles en el anfitri… |
| 14 | `80119C50` | juego DOL | (sin nombre; tras __dt__Q23m2d8Simple_cFv+0xa7e0) | 0.70 | 0.67 / 0.04 | no | – |  |
| 15 | `801C0500` | SDK | PSMTXCopy | 0.69 | 0.69 / 0.00 | no | – | **N2** nativa rápida (float, host) |
| 16 | `801AC470` | SDK | (sin nombre; tras OSSetMEM1ArenaLo+0x580) | 0.66 | 0.53 / 0.13 | no | – | N1 a mano |
| 17 | `80248330` | nw4r | (sin nombre; tras Construct__Q34nw4r3g3d14AnmObjChrBlendFP12MEMAllo…) | 0.66 | 0.64 / 0.02 | no | – | pendiente |
| 18 | `80267F40` | nw4r | (sin nombre; tras SetPan__Q44nw4r3snd6detail10BasicSoundFf+0xa80) | 0.64 | 0.64 / 0.01 | no | – | pendiente |
| 19 | `802382B0` | nw4r | (sin nombre; tras CosFIdx__Q24nw4r4mathFf+0x670) | 0.62 | 0.53 / 0.09 | no | – | **N5** nativa (exacta, macros N1 sobre local… |
| 20 | `80273560` | nw4r | (sin nombre; tras DetachSound__Q34nw4r3snd14SeqSoundHandleFv+0x820) | 0.59 | 0.59 / 0.01 | no | – | pendiente |
| 21 | `801B4EA0` | SDK | (sin nombre; tras OSGetCurrentThread+0x370) | 0.58 | 0.52 / 0.06 | no | – |  |
| 22 | `80169600` | juego DOL | ExecCallbackA | 0.52 | 0.52 / 0.00 | **sí** | dol/mLib/m_3d/mdl.cpp |  |
| 23 | `8010AA60` | juego DOL | (sin nombre; tras DrawIcon__Q24dHbm8Manage_cFv+0x1270) | 0.51 | 0.17 / 0.34 | no | – |  |
| 24 | `801C7350` | SDK | (sin nombre; tras GXInitTexObjLOD+0x360) | 0.50 | 0.26 / 0.24 | no | – | GXLoadTexObjPreLoaded: N4 (GX) |
| 25 | `80101AA0` | juego DOL | getLoopPosX__10dScStage_cFf | 0.49 | 0.49 / 0.00 | no | – |  |
| 26 | `802DCBFC` | MSL/rt | __ptmf_scall | 0.47 | 0.47 / 0.00 | no | – | pendiente |
| 27 | `801C52F0` | SDK | (sin nombre; tras GXSetNumTexGens+0x8d0) | 0.47 | 0.34 / 0.13 | no | – | __GXSetDirtyState: N4 (GX, escribe __GXData … |
| 28 | `802405B0` | nw4r | (sin nombre; tras GXSetTevKColor__Q34nw4r3g3d14ResMatTevColorF14_GX…) | 0.47 | 0.45 / 0.01 | no | – | pendiente |
| 29 | `801C5570` | SDK | GXBegin | 0.45 | 0.27 / 0.18 | no | – | pendiente |
| 30 | `802CC730` | EGG | (sin nombre; tras initialize__Q23EGG12LightTextureFUsPQ23EGG4Heap+0…) | 0.45 | 0.43 / 0.01 | no | – | pendiente |
| 31 | `8029D300` | nw4r | (sin nombre; tras __ct__Q34nw4r2ef6EffectFv+0x17af0) | 0.44 | 0.34 / 0.10 | no | – | pendiente |
| 32 | `80004364` | MSL/rt | memcpy | 0.37 | 0.35 / 0.02 | no | – | N1 traducida (sin ganancia medida) → rehacer… |
| 33 | `80292C90` | nw4r | (sin nombre; tras __ct__Q34nw4r2ef6EffectFv+0xd480) | 0.37 | 0.36 / 0.00 | no | – | pendiente |
| 34 | `801C07A0` | SDK | PSMTXInverse | 0.35 | 0.25 / 0.11 | no | – | **N3** nativa rápida (float, host) |
| 35 | `801C9940` | SDK | GXLoadPosMtxImm | 0.34 | 0.12 / 0.22 | no | – | **N2** nativa rápida (float, host) |
| 36 | `80082BE0` | juego DOL | (sin nombre; tras getLoopScrollDispPosX__14dBgParameter_cFf+0x9a0) | 0.34 | 0.33 / 0.01 | no | – |  |
| 37 | `80237C40` | nw4r | CosFIdx__Q24nw4r4mathFf | 0.34 | 0.27 / 0.07 | no | – | **N2** nativa rápida (float, host) |
| 38 | `8025DB30` | nw4r | (sin nombre; tras Construct__Q34nw4r3g3d6ScnMdlFP12MEMAllocatorPUlQ…) | 0.34 | 0.34 / 0.00 | no | – | pendiente |
| 39 | `8024C960` | nw4r | (sin nombre; tras TestAnmFlag__Q34nw4r3g3d6AnmObjCFQ44nw4r3g3d6AnmO…) | 0.32 | 0.32 / 0.01 | no | – | pendiente |
| 40 | `8008C6E0` | juego DOL | checkCollision__5dCc_cFP5dCc_cP5dCc_ci | 0.32 | 0.32 / 0.00 | **sí** | dol/bases/d_cc.cpp |  |
| 41 | `8024C2B0` | nw4r | (sin nombre; tras TestAnmFlag__Q34nw4r3g3d6AnmObjCFQ44nw4r3g3d6AnmO…) | 0.31 | 0.21 / 0.10 | no | – | pendiente |
| 42 | `8024E180` | nw4r | (sin nombre; tras Invalidate__Q34nw4r3g3d8G3DStateFUl+0xba0) | 0.30 | 0.30 / 0.00 | no | – | pendiente |
| 43 | `80280910` | nw4r | (sin nombre; tras detail_PrepareSound__Q34nw4r3snd14SoundStartableF…) | 0.30 | 0.29 / 0.00 | no | – | pendiente |
| 44 | `802CC050` | EGG | (sin nombre; tras initialize__Q23EGG12LightTextureFUsPQ23EGG4Heap+0…) | 0.29 | 0.23 / 0.06 | no | – | pendiente |
| 45 | `801C04D0` | SDK | PSMTXIdentity | 0.28 | 0.28 / 0.00 | no | – | **N2** nativa rápida (float, host) |
| 46 | `8000D0D0` | juego DOL | (sin nombre; tras findWindowByName__Q23d2d7Multi_cFPCc+0x5c60) | 0.28 | 0.28 / 0.00 | no | – |  |
| 47 | `801C8920` | SDK | GXSetFog | 0.28 | 0.22 / 0.07 | no | – | pendiente |
| 48 | `801C0610` | SDK | (sin nombre; tras PSMTXConcat+0xd0) | 0.28 | 0.09 / 0.18 | no | – | N1 traducida (sin ganancia medida) → rehacer… |
| 49 | `8024B720` | nw4r | (sin nombre; tras TestAnmFlag__Q34nw4r3g3d6AnmObjCFQ44nw4r3g3d6AnmO…) | 0.28 | 0.26 / 0.01 | no | – | pendiente |
| 50 | `801B1180` | SDK | (sin nombre; tras DCStoreRangeNoSync+0x4c80) | 0.27 | 0.26 / 0.01 | no | – | su coste es la entrega de interrupciones: no |
| 51 | `801C8430` | SDK | (sin nombre; tras GXSetTevAlphaOp+0x60) | 0.27 | 0.07 / 0.20 | no | – | pendiente |
| 52 | `802390D0` | nw4r | (sin nombre; tras Set__Q34nw4r4math4AABBFPCQ34nw4r4math4VEC3Ui+0xa40) | 0.27 | 0.22 / 0.05 | no | – |  |
| 53 | `802DCC4C` | MSL/rt | __cvt_fp2unsigned | 0.27 | 0.27 / 0.00 | no | – | **N3** nativa rápida (float, host) |
| 54 | `80240240` | nw4r | (sin nombre; tras GXSetTevKColor__Q34nw4r3g3d14ResMatTevColorF14_GX…) | 0.26 | 0.20 / 0.07 | no | – | N1 traducida (sin ganancia medida) → rehacer… |
| 55 | `801C78D0` | SDK | (sin nombre; tras GXLoadTexObj+0x410) | 0.26 | 0.25 / 0.01 | no | – | pendiente |
| 56 | `8029C820` | nw4r | (sin nombre; tras __ct__Q34nw4r2ef6EffectFv+0x17010) | 0.26 | 0.25 / 0.00 | no | – | pendiente |
| 57 | `8027E080` | nw4r | (sin nombre; tras detail_PrepareSound__Q34nw4r3snd14SoundStartableF…) | 0.24 | 0.24 / 0.00 | no | – | pendiente |
| 58 | `802631F0` | nw4r | (sin nombre; tras Construct__Q34nw4r3g3d7ScnProcFP12MEMAllocatorPUl…) | 0.24 | 0.24 / 0.00 | no | – | pendiente |
| 59 | `801ACE30` | SDK | (sin nombre; tras DCStoreRangeNoSync+0x930) | 0.24 | 0.24 / 0.00 | no | – | pendiente |
| 60 | `801C7D90` | SDK | (sin nombre; tras GXLoadTexObj+0x8d0) | 0.23 | 0.12 / 0.12 | no | – | pendiente |

Notas a filas sin nombre (de r19n/r20g6): `8000D000`/`8000D0D0` = código del juego que escribe en el FIFO de GX
(frente W; `8000D0D0` ya tiene nativa N1 ×1,06); `800ECAE0` = función enorme del juego (≈5 000 líneas generadas, guarda
f14-f31 con paired singles: matemática pesada) fusionada con otra entrada, no decompilada; `801A2B10` = `__AXSyncPBs`;
`801AC440`/`801AC470` = DCInvalidateRange/DCFlushRange; `801C52F0` = `__GXSetDirtyState`; `801C7350` =
GXLoadTexObjPreLoaded; `80250E70` = g3d CalcWorld; `802403C0`/`80240240` = animación g3d; `801B4EA0` = bucle ocioso del
SO; `801B1180` = OSRestoreInterrupts (coste = entrega de interrupciones). Juego sin decompilar en el top: `80119C50`,
`8010AA60`, `80101AA0` (`dScStage_c::getLoopPosX`), `80082BE0` (zona `dBgParameter_c`), `80081960` (`dBg_ctr_c`),
`80070C40` (`dBc_c`, colisión con el fondo). Sus clases sí tienen cabecera en la decomp (`include/game/bases/d_bc.hpp`,
`d_bg_ctr.hpp`, `d_bg_parameter.hpp`, `d_s_stage.hpp`), pero no el cuerpo.

### Decompiladas más calientes (todas las de > 0,09 %)

| # global | USA | función | % ocupado | archivo PAL |
|---|---|---|---|---|
| 12 | `80162C30` | `fLiMgBa_c::searchNodeByID` | 0,87 | dol/framework/f_line.cpp |
| 22 | `80169600` | `m3d::mdl_c::mdlCallback_c::ExecCallbackA` | 0,52 | dol/mLib/m_3d/mdl.cpp |
| 40 | `8008C6E0` | `dCc_c::checkCollision` | 0,32 | dol/bases/d_cc.cpp |
| 73 | `80163090` | `fTrMgBa_c::searchNodeByProfName` | 0,20 | dol/framework/f_tree.cpp |
| 82 | `80228FF0` | `nw4r::ut::List_GetNext` | 0,19 | lib/nw4r/ut/ut_list.cpp |
| 90 | `80169A30` | `m3d::mdl_c::mdlCallback_c::ExecCallbackB` | 0,18 | dol/mLib/m_3d/mdl.cpp |
| 104 | `80168DE0` | `m3d::fanm_c::play` | 0,15 | dol/mLib/m_3d/fanm.cpp |
| 110 | `80168CB0` | `m3d::calcRatio_c::isEnd` | 0,14 | dol/mLib/m_3d/calc_ratio.cpp |
| 118 | `80162D00` | `fManager_c::searchBaseByID` | 0,14 | dol/framework/f_manager.cpp |
| 123 | `80056CF0` | `daPlBase_c::isStatus` | 0,14 | dol/bases/d_a_player_base.cpp |
| 126 | `80162BB0` | `fLiMgBaFu_c::lineListProc` | 0,14 | dol/framework/f_line.cpp |
| 132 | `8008C2C0` | `dCc_c::clear` | 0,14 | dol/bases/d_cc.cpp |
| 136 | `80142660` | `dAcPy_c::setBcData` | 0,13 | dol/bases/d_a_player.cpp |
| 148 | `8004DB40` | `daPlBase_c::isDemoType` | 0,13 | dol/bases/d_a_player_base.cpp |
| 170 | `80161CC0` | `fBase_c::commonPack` | 0,11 | dol/framework/f_base.cpp |
| 177 | `800643F0` | `dActor_c::preExecute` | 0,11 | dol/bases/d_actor.cpp |
| 196 | `8006C980` | `dBaseActor_c::postExecute` | 0,10 | dol/bases/d_base_actor.cpp |
| 199 | `8016E3A0` | `mFrustum_c::intersectAABB` | 0,10 | dol/mLib/m_frustum.cpp |
| 214 | `8015FAE0` | `sStateMethod_c::executeStateMethod` | 0,10 | dol/sLib/s_StateMethod.cpp |

Muchas son diminutas (`isStatus`, `isEnd`, `List_GetNext`, `isDemoType`): su coste es el **protocolo de llamada**
(volcar/recargar `ctx`), no el cuerpo. Eso lo ataca G6 (en línea / etapa 3), no una versión nativa aislada.

## 4. Qué haría falta (adaptador) y de qué dependen

**Patrón existente (frente N, `r9/r19n`):** `tools/apply_n1.py` mete al principio de la función generada
`if (ctx.pc == 0x8XXXXXXX && n1_hook(n1s_8XXXXXXX, ctx)) return;`; el cuerpo nativo vive en el runtime
(`nWiiRuntime/src/hle/native_n1.cpp`, `native_n2.cpp`), lee la memoria con `MemView`/punteros del anfitrión (fastmem),
deja **exactamente** los mismos registros (también volátiles), memoria, CR, `inst_count` y sondeos de interrupción, y
cae al código generado en los casos raros (NaN, subnormales, memoria que no es RAM, `NWII_WATCH`). Interruptores
`NWII_NATIVE[_<NOMBRE>]`, `NWII_NATIVE_TIME` (A/B por llamada) y `NWII_NATIVE_VERIFY` (instantánea de toda la RAM +
registros, nativo vs generado). Solo hojas: N nunca llamó de vuelta al código generado.

Para compilar código de la decomp como nativo haría falta, además:

1. **Tipos de memoria invitada.** Las cabeceras usan punteros nativos de 32 bits; en el anfitrión (64 bits, little
   endian) hay que reescribir cada campo como `be<T>` (lectura/escritura con `bswap`) y cada puntero como `gptr<T>`
   (u32 invitado → `MemView`). Las disposiciones de las clases son las mismas en USA y PAL (código idéntico), así que las
   cabeceras valen; hay que reescribirlas, no se pueden superponer tal cual.
2. **Direcciones USA de globales/datos.** "match" = mismas instrucciones con las reubicaciones enmascaradas: la lógica
   es la de USA, pero cada global, vtable, constante de `.sdata2`/`.rodata` y cada `r13`/`r2`-relativo tiene otra
   dirección. Automatizable: las reubicaciones del `.o` PAL dicen dónde mirar en el código USA ya casado.
3. **Llamadas de vuelta al código generado** (funciones no portadas, virtuales, punteros a función, `__ptmf_scall`,
   heap de EGG para `new`/`delete`): poner r3-r10/f1-f8, `ctx.lr`, `ctx.pc`, `nwii_dispatch(ctx)` y comprobar
   `ctx.pc == retorno`. **El problema serio:** en el código generado cualquier llamada puede "deshacer la pila"
   (`if (ctx.pc != 0x...) return;`) si dentro se entrega una interrupción que cambia de hilo, y la función se
   **reanuda** después saltando a la etiqueta de ese retorno (`switch (ctx.pc)`). Una función nativa con su estado en
   registros del ARM no se puede reanudar. Opciones: (a) solo hojas o caminos sin llamadas (lo que hizo N); (b)
   ejecutar el cuerpo con la entrega de interrupciones aplazada hasta el final (cambia el momento de llegada unos µs:
   pierde la exactitud de N; no vale si un llamado bloquea: mutex de heap, mensajes); (c) volver al código generado
   si `ctx.pc` no es el esperado, cosa que solo es posible antes de haber escrito nada.
4. **Coma flotante.** El hilo del juego emula los paired singles en double; N2 mostró que la operación en single del
   anfitrión es idéntica para normales. Pero CodeWarrior decide dónde fusiona `a*b+c` en `fmadds`: hay que compilar con
   `-ffp-contract=off` y poner `fmaf` justo donde el asm PPC tiene `fmadd(s)`/`fnmsub(s)`, más la conversión
   float→int (`fctiwz`: NaN y desbordes distintos de ARM) y `frsqrte`/`fres`. Un bit distinto en la física desincroniza
   las demos del título y las escenas forzadas de S (son entradas grabadas).
5. **`inst_count` y sondeos.** N los reproduce en forma cerrada. Un cuerpo compilado de C++ no sabe cuántas
   instrucciones PPC habría ejecutado: o se acepta reloj inexacto (solo con `NWII_REALTIME=1`, que es lo que usa la
   Switch) y se desactiva en los modos de verificación/reloj virtual, o se cuenta por caminos (caro, mata la ventaja).
   Los bucles largos nativos tienen que sondear (latencia de VI/audio).
6. **Verificación.** `NWII_NATIVE_VERIFY` compara todos los registros; para código compilado distinto hay que
   relajarla al estado visible por la ABI (r1, r2, r13, r14-r31, f14-f31, r3/r4/f1 de vuelta, CR no volátil, memoria
   salvo la pila muerta bajo r1). El difftest de r19n sirve de base.

Dependencias de las candidatas: `f_line`/`f_tree`/`f_manager` solo listas enlazadas de `fBase_c` (hojas, enteros);
`mdl.cpp` llama a `C_QUATMtx`/`C_QUATSlerp`/PSMTX del SDK (no decompilados aquí; triviales) y su llamador es g3d
CalcWorld (`80250E70`, nw4r, generado) mediante un functor virtual; `d_cc.cpp` es autocontenido salvo la tabla
`_hitCheck[][]` (en el mismo archivo) y los callbacks de impacto de cada colisionador (punteros a función invitados
→ llamada de vuelta, riesgo del punto 3); `daPlBase_c`/`dAcPy_c` dependen de casi todo el juego (dBc_c, dBg, estados,
efectos, sonido) y están repartidas en 262 funciones pequeñas: descartadas.

## 5. Candidatas recomendadas (si se hace un piloto)

| Orden | Grupo | % ocupado | Por qué | Ganancia esperable |
|---|---|---|---|---|
| 1 | `fLiMgBa_c::searchNodeByID` + `fManager_c::searchBaseByID` (+ `fTrMgBa_c::searchNodeByProfName`, `cTreeNd_c::getTreeNext`) | 1,0 (+0,3) | hojas, enteros; 24 000 llamadas/s; la decomp dice el porqué del coste (búsqueda lineal en 8 listas por `id & 7`; los ID crecen y las listas van en orden decreciente → **corte temprano**, o un índice id→nodo nativo mantenido al enganchar alta/baja de nodos) | la versión nativa "igual" de N1 dio solo ×1,12; con cambio de algoritmo exacto ~×3-5 → 0,6-0,8 % |
| 2 | `dCc_c::execute` + `checkCollision` + `_hitCheck*` + `clear` | 0,67 | bucle O(n²) autocontenido, máscaras de bits y comparaciones float sin `fma` dudosas | ×1,3-1,5 → 0,15-0,25 %; riesgo: callbacks de impacto (punto 3 de §4) |
| 3 | `m3d::mdl_c::mdlCallback_c::ExecCallbackA/B` | 0,70 | camino común = copia de `ChrAnmResult` (memcpy sin byteswap); mezcla con slerp en single | ×1,5-2 en el camino común → 0,25-0,35 % |
| 4 | `m3d::fanm_c::play`, `calcRatio_c::isEnd`, `mFrustum_c::intersectAABB`, `mMtx_c::YrotS` | 0,47 | hojas pequeñas de float, como N2 | ×1,2-1,5 → ~0,1 % |
| 5 | `sStateMethod_c::executeStateMethod`, `sStateID_c::isEqual`, `fBase_c::commonPack` | 0,4 | despachos virtuales: se quedan en llamadas de vuelta | casi nada; solo si se resuelve el punto 3 |

Total piloto 1-3: ~2,4 % del hilo → **~0,25-0,5 ms por frame** de ~26 ms a 1020 MHz (1 % ≈ 0,26 ms).

## 6. Estimación de la ganancia

- Un 1 % del hilo ocupado ≈ 0,26 ms con frames de ~26 ms; faltan ~9,3 ms (−36 %).
- Lo decompilado caliente es ~10 % del hilo. Si **todo** se portara (623 funciones, 140 archivos, el adaptador completo
  y llamadas de vuelta resueltas) con ×1,3 → −2,3 % (0,6 ms); con un ×2 muy optimista → −5 % (1,3 ms).
- Piloto de 5-10 candidatas: 0,25-0,5 ms. Comparación: PGO dio −16 %; cada ronda de N dio ~1 %.
- Medida de referencia del frente N (r19n §6): nativo exacto por llamada ×0,9-×1,2 en enteros/punteros
  (searchNodeByID ×1,12, `8000D0D0` ×1,06, memcpy ×0,96) y ×1,4-×1,6 solo en coma flotante hoja (SinFIdx,
  PSMTXIdentity, DC*). El código generado de enteros ya es bueno (G5 con registros en locales, fastmem, PGO): lo que
  cuesta es la FP emulada en double y el protocolo de llamada, y la decomp no cambia ninguna de las dos cosas por sí sola.

## 7. Riesgos

1. **Cobertura**: lo caliente es nw4r/SDK/EGG/juego sin decompilar (~80 % del hilo). La decomp avanza, pero hoy
   no da material para lo que pesa. Otras decomps (p. ej. la de Wii Sports con nw4r/EGG/SDK) serían de otra versión de
   nw4r: habría que casarlas con `sym_match` antes de fiarse (no comprobado aquí).
2. **Reanudación tras cambio de hilo** dentro de una llamada de vuelta (§4.3): incompatible con cuerpos nativos
   que no sean hoja salvo aplazando interrupciones.
3. **Exactitud de FP** (contracción de CW, `fctiwz`, `fres`/`frsqrte`, NaN): desincroniza demos/escenas forzadas.
4. **Diferencias PAL/USA**: "match" garantiza mismas instrucciones, pero las funciones solo "layout"/por rango
   pueden diferir; usar solo las `match` (y comprobar con el difftest).
5. **`inst_count`/reloj virtual y verificación**: hay que relajar el contrato de N (solo tiempo real en la Switch).
6. **Mantenimiento**: G6 etapa 3 cambia la firma de las funciones (`F_g6(ctx, ic)`); el gancho de entrada (como N)
   sigue valiendo, pero las llamadas de vuelta tendrían que seguir el protocolo nuevo. PGO y el orden L se rehacen
   como siempre.
7. **Coste de trabajo**: adaptador (`be<T>`, `gptr<T>`, direcciones USA de globales, llamada de vuelta, verificación
   ABI) ~1-2 días; cada grupo de candidatas 0,5-2 días con verificación. Mucho trabajo por <0,5 ms.

## 8. Recomendación

- **No** abrir D como frente principal: con la decomp de hoy toca ~10 % del hilo, muy repartido, y la ganancia
  realista es de décimas de ms.
- Si se quiere un piloto, **solo el grupo 1** (búsqueda por ID) con cambio de algoritmo exacto: es hoja, de enteros,
  sin FP ni llamadas de vuelta, el gancho de N ya existe para `80162C30`, y es donde la decomp aporta algo que el
  código generado no puede (el porqué del bucle). Antes, medir con `NWII_NATIVE_TIME` (r19n) cuántos nodos recorre cada
  búsqueda en las escenas lentas para confirmar que vale la pena.
- Aprovechar la decomp como **documentación**: cabeceras de `dBc_c`/`dBg_ctr_c`/`dScStage_c` y de nw4r g3d para
  escribir a mano (desde el asm USA) nativas de funciones grandes no decompiladas que sí pesan (`800ECAE0` 1,8 %,
  `80119C50`, colisión con el fondo `80070C40`/`80081960`/`80082BE0`, g3d `80250E70`), como el plan N8+ de r19n.
- Lo que más mueve el hilo del juego sigue siendo G6 etapa 3 (protocolo de llamada: ataca justo el coste de las
  funciones diminutas), PGO+L en cada versión y quitarle trabajo de dibujo (FIFO de GX: frente W, ~5 % entre
  `GX_WGPIPE_*` y los MTX/GX del SDK).

## 9. Cómo repetirlo

```
S=<carpeta temporal>; cd ~/nsmbw-port
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm -n -S --defined-only r9/r22r7/bin/nsmbw_r7f.elf > $S/r7f.nm
python3 -I r9/r25dec/tools/rank.py $S/r7f.nm switch/real_logs/bench_F01_r7f_lista1020_1110_0132/profile_cpu.txt \
    switch/real_logs/bench_E02_dtorre_on_1110_0119/profile_cpu.txt > $S/rank.tsv
tools/venv/bin/python -I r9/r25dec/tools/decomp_map.py $S/decomp_map.tsv
python3 -I r9/r25dec/tools/table.py $S 5000 > $S/table5000.tsv     # lee $S/rank.tsv y $S/decomp_map.tsv
```
Nada pesado: unos segundos y < 300 MB.
