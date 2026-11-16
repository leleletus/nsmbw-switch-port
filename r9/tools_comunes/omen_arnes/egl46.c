// [verif_omen] egl46.c -> libegl46.so (LD_PRELOAD en la Omen, la compila verif_omen.sh allí con gcc).
// Por qué: el port pide un contexto GL 3.2 core (nWiiRuntime src/core/main.cpp: SDL_GL_CONTEXT_MAJOR/MINOR 3/2) pero
// usa funciones de 3.3+ (p. ej. glGenSamplers en gx::DrawPrimitive). Mesa (la laptop: iris; también llvmpipe) sube un
// 3.2 core a 4.6 core y todo va; el EGL de NVIDIA da EXACTAMENTE 3.2, glad no carga lo de 3.3+ y el juego se cae
// (puntero nulo). Esta capa sube la versión pedida a 4.6 core (= lo que da la laptop), sin tocar el binario.
// Cómo: SDL va enlazado estático y saca eglCreateContext con dlsym(libEGL, ...): se intercepta dlsym y, para ese
// nombre, se devuelve un envoltorio que reescribe EGL_CONTEXT_MAJOR/MINOR_VERSION. NWII_EGL46_OFF=1 lo desactiva.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int EGLint;
typedef void *EGLDisplay, *EGLConfig, *EGLContext;
typedef EGLContext (*cc_fn)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
#define EGL_NONE 0x3038
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB

static cc_fn real_cc;

static EGLContext cc46(EGLDisplay d, EGLConfig c, EGLContext s, const EGLint *a) {
  EGLint b[128]; int n = 0, major = -1;
  for (const EGLint *p = a; p && p[0] != EGL_NONE && n < 124; p += 2)
    if (p[0] == EGL_CONTEXT_MAJOR_VERSION) major = p[1];
  for (const EGLint *p = a; p && p[0] != EGL_NONE && n < 124; p += 2) {
    b[n] = p[0]; b[n + 1] = p[1];
    if (major == 3 && p[0] == EGL_CONTEXT_MAJOR_VERSION) b[n + 1] = 4;
    if (major == 3 && p[0] == EGL_CONTEXT_MINOR_VERSION) b[n + 1] = 6;
    n += 2;
  }
  b[n] = EGL_NONE;
  if (major == 3) fprintf(stderr, "[EGL46] contexto 3.x pedido -> 4.6 (capa de verif_omen)\n");
  return real_cc(d, c, s, major == 3 ? b : a);
}

void *dlsym(void *h, const char *name) {
  static void *(*real)(void *, const char *);
  if (!real) {
    real = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!real) real = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
  }
  void *p = real(h, name);
  if (p && name && !strcmp(name, "eglCreateContext") && !getenv("NWII_EGL46_OFF")) {
    real_cc = (cc_fn)p;
    return (void *)cc46;
  }
  return p;
}
