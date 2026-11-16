/* libnx has no dynamic loader. glad only reaches dlopen from gladLoadGL();
 * the runtime uses gladLoadGLLoader(SDL_GL_GetProcAddress), so these stubs
 * just make glad.c compile. */
#pragma once
#define RTLD_NOW 0
#define RTLD_GLOBAL 0
#define RTLD_LAZY 0
static inline void *dlopen(const char *f, int m) { (void)f; (void)m; return 0; }
static inline void *dlsym(void *h, const char *s) { (void)h; (void)s; return 0; }
static inline int dlclose(void *h) { (void)h; return 0; }
