# r46g8: frente G8 — plan (b) de W (psil + bcache en la pasada G7) — 16-nov-2026 — DESCARTADO

- `construir_nro/`: copia de tools/construir_nro con dos feats nuevas de G7 (la publicada NO se ha tocado):
  - `pasadas/g7.h`: ps_madd/msub/nmadd/nmsub/madds0/madds1 `_i` en línea (empate sigue fuera); `psq_lq/psq_stq`
    cuantizados en línea para GQR2-5 si el tipo es el esperado (u8/u16/s8/s16, misma cuenta que psq_l_gen/psq_st_gen),
    si no lo de siempre; `G7_BCACHE` = caché de un destino por sitio de bctrl (casilla de 64 bits
    addr<<32 | desplazamiento del cuerpo; fallo -> `g7_bctrl_c_miss` -> `g7_bctrl_g6`).
  - `pasadas/apply_g7.py`: feats `psil` y `bcache` (bcache solo en las 1417 funciones = 97 % de las muestras,
    `material/g8_hot.txt`, sacado de r33g7/fn_weights.tsv). `construir_nro.py`: env `G8_FEAT=",psil,bcache"`.
  - Sitios: psil 457 ps_* + 753 psq_l + 50 psq_st; bcache 994.
- Omen: `~/construir_nro_prueba/g8b` (variante, sin PGO, `--permitir-distinto`); control sin PGO = `t2` (mismo runtime m6q).
- Consola (contiguas, 400 s, 5 demos, cpu 1020 / gpu 460 en las 84 líneas, IMAGEN OK, capturas revisadas):

  | corrida | NRO sha256 | ms juego desierto·agua·cielo·nubes·castillo |
  |---|---|---|
  | G8A_ctl_sinpgo | 9b95bd95… | 12,1 · 15,0 · 14,5 · 11,9 · 6,5 |
  | G8B_pb_sinpgo  | 4b2e6ddb… | 12,5 · 15,1 · 14,7 · 11,9 · 6,6 |

  Agua +0,1 ms (umbral pedido: −0,3) -> DESCARTADO; no se hizo el ciclo de PGO.
- PC: `tools/build_pcobj.sh` + `tools/build_pc_g8.sh` -> `bin/NSMBW_g8b`; verificación en la Omen lanzada (`g8b`),
  traer con `r9/tools_comunes/verif_omen.sh --traer g8b` (ya no decide nada).
- Nota: la etapa `herramientas` de construir_nro volvió a clonar NWiiRecomp/uam/toml++ de GitHub (solo lectura)
  porque la carpeta de la herramienta cambió; para evitarlo, pasar `--recompilador/--uam/--tomlplusplus` locales.
