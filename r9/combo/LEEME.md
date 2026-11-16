# r9combo (7-nov-2026): versión de prueba para la Switch real

NRO = runtime de `build_usuario/` (r8 + hilos de R8 + extras: audio audren nativo, barra de shaders, perfilador)
+ frente `r9/cpucode` versión c2 (`cpu_context.h` con MemView corto, `constinit` en os_hle_stubs.cpp,
toolchain con `-fomit-frame-pointer -fvisibility=hidden`, reverb AXFX nativa verificada 651/651 bit a bit en PC).
El código generado son los objetos de Switch de `r9/cpucode/nx/build` tal como se compilaron para `nsmbw_c2.nro`
(08:05 + output_24 de las 10:06); NO lleva el experimento posterior c3/crdse del export de cpucode.

Compilación: runtime en `nx/build` (cmake con `cmake/nsmbw-switch.cmake` de cpucode), enlace con la línea de
`r9/cpucode/nx/build` cambiando main.o, libnwiiruntime.a y libglad.a. Logs: `build_rt.log`, `link.log`.
Sin verificar en citron (citron borrado por el usuario al usar otro fork). Copia congelada con .elf:
`switch/real_builds/r9combo_1107/`. Simbolizar el perfilador con ese .elf.
