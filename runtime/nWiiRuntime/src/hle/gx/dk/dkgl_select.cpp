// r15dk: elección GL / deko3d para el código que pide funciones GL por nombre. Se compila en todas las
// plataformas; dkgl (deko3d) solo existe en la Switch.
#include "dkgl.h"
#include <SDL.h>

#ifdef __SWITCH__
void* GX_GLProc(const char* name) {
  return dkgl::Active() ? dkgl::GetProcAddress(name) : SDL_GL_GetProcAddress(name);
}
bool GX_DkActive() { return dkgl::Active(); }
#else
void* GX_GLProc(const char* name) { return SDL_GL_GetProcAddress(name); }
bool GX_DkActive() { return false; }
#endif

#ifdef __SWITCH__
static int s_dk_interval = 1;
SDL_Window* GX_HostWindow() {
  static int dummy;
  return dkgl::Active() ? reinterpret_cast<SDL_Window*>(&dummy) : SDL_GL_GetCurrentWindow();
}
void GX_HostDrawable(SDL_Window* win, int* w, int* h) {
  if (dkgl::Active()) dkgl::GetDrawableSize(w, h); else SDL_GL_GetDrawableSize(win, w, h);
}
int GX_HostGetSwapInterval() { return dkgl::Active() ? s_dk_interval : SDL_GL_GetSwapInterval(); }
int GX_HostSetSwapInterval(int i) {
  if (!dkgl::Active()) return SDL_GL_SetSwapInterval(i);
  s_dk_interval = i;
  dkgl::SetSwapInterval(i);
  return 0;
}
void GX_HostSwap(SDL_Window* win) { if (dkgl::Active()) dkgl::SwapBuffers(); else SDL_GL_SwapWindow(win); }
#else
SDL_Window* GX_HostWindow() { return SDL_GL_GetCurrentWindow(); }
void GX_HostDrawable(SDL_Window* win, int* w, int* h) { SDL_GL_GetDrawableSize(win, w, h); }
int GX_HostGetSwapInterval() { return SDL_GL_GetSwapInterval(); }
int GX_HostSetSwapInterval(int i) { return SDL_GL_SetSwapInterval(i); }
void GX_HostSwap(SDL_Window* win) { SDL_GL_SwapWindow(win); }
#endif
