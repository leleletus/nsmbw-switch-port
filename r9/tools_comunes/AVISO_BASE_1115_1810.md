# Cambio de base de medida (15-nov 18:10)

Desde la corrida que empiece después de las 18:10 (D11 en adelante) `tools/switchctl/debug.toml.maestro` lleva
`profile = false`: el perfilador por muestreo (profile_*.txt) queda APAGADO en todas las corridas.

- Medido: M12 (m6e sin perfilador) frente a M10 (m6e con él), contiguas: dibujados 59,3·47,2·59,2·59,9·56,8
  frente a 59,1·43,2·58,3·60,0·54,3 (desierto·agua·cielo·nubes·castillo).
- Consecuencia: NO compares una corrida posterior a las 18:10 con un control anterior. Control y variante siempre
  contiguos y los dos después del cambio.
- Los profile_main/cpu/gxw.txt que baje el banco son VIEJOS (de la última corrida con perfilador). Quien necesite
  perfil: `DEBUG_EXTRA` no puede duplicar la clave; usa el interruptor propio `NWII_NX_PROFILE="1"` en DEBUG_EXTRA.
- `bench_title.sh` acepta `DEBUG_DROP='<regex>'` para quitar líneas del maestro en una corrida.
- NUNCA ejecutar ni hacer source de un .job. La cola ordena con `sort` regional (ignora «_»): comprobar con `ls | sort`.
- (18:30, frente W) `NWII_TPROF="1"` NO graba con `profile = false`: cuelga del muestreador de switch_profiler.cpp. Toda corrida con TPROF necesita además `NWII_NX_PROFILE="1"` en DEBUG_EXTRA.

# Limpieza (15-nov 20:50)
- A petición del usuario se MOVIERON (no borraron) ~90 GB de artefactos de frentes viejos a `/home/djvemo/nsmbw-port/.papelera_1115/`
  conservando la ruta relativa (lista: NIVEL 1 de `r9/tools_comunes/LIMPIEZA_plan.md`; guion `r9/tools_comunes/papelera.py`).
  Son `bin/`, objetos, ELF, NRO y trazas de frentes anteriores que ningún guion vivo nombraba.
- Si a un guion le falta un fichero de un frente viejo: `mv .papelera_1115/<ruta> <ruta>` y avisar al coordinador. No borrar la papelera.
- También se borraron las copias .nro de corridas ya terminadas en `tools/switchctl/.cola/` (los originales siguen en `r9/*/bin`).
