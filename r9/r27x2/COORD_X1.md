# Nota de coordinación X2 → X1 (r27x2, 10-nov)

X2 **no** sustituye ninguna función de matemáticas/matrices (PSMTX*, nw4r::math, GXLoad*MtxImm como función).
Lo único que toca X2 en el código generado: en `output_1, 16, 24, 26, 27, 35` cambia las llamadas de almacenamiento
MMIO ya listadas (`mv_.write8_io/16/32/_f32_io(`, `nwii::runtime::psq_st_io(`) por `nwii::runtime::x2_*(` (cabecera
nueva `include/runtime/x2_wgpipe.h`), con `r27x2/tools/apply_x2.py` (sustitución textual, re-aplicable sobre otro
`export/`). Dentro de GXLoadTexMtxImm/GXLoadPosMtxImm (output_27) solo cambian los `psq_st_io` al FIFO.
Si X1 engancha esas funciones con una línea al principio, los dos cambios se combinan aplicando ambos scripts.
Runtime: `hle/gx.cpp` (estado del write-gather en `g_wgi`), `hw/hw_pi.cpp` (1 línea), `switch_platform.cpp` (keeps_zero).
