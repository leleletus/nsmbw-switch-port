# r10b / r10b2 (8-nov-2026)

- r10b = r10a (r9combo + guestloop + arreglo del reloj) + frente render t4 (HostPtr no atómico, carga XF dentro del
  comando, caché de emisión, hash de shader reutilizado; SIN su búfer de escrituras GX, que duplica el de R8 hilos)
  + log de la frecuencia real de CPU/GPU en la línea `[NX] game`.
- r10b2 = r10b + `src/platform/switch/switch_alloc.cpp` (operator new/delete por hilo, clases de 16 B hasta 1 KB).
- Código generado: el de r10a (objetos de `r9/r10a/nx/build`), enlazado con el runtime de `nx/build`.
- Verificación PC (`r9/render/test/checks.sh`, NWII_R9_FRAMECHECK=30 + NWII_ECACHE_CHECK=1, 1x, 120 s):
  título/mapa/caminar/1-1 = 820 frames idénticos, 0 diferencias de píxeles, 92 M vértices recomprobados, 0 fallos
  (`checks_1x.txt`).
- Congelados: `switch/real_builds/r10b_1108/`.
