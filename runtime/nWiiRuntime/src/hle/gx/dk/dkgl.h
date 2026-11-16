#pragma once
// r15dk: dkgl = el subconjunto de OpenGL que usa el runtime (renderer_gl.cpp, texture_cache_gl.cpp, gx.cpp,
// main.cpp), implementado sobre deko3d. Se instala como "driver" de glad: con backend deko3d, main.cpp llama
// gladLoadGLLoader(dkgl::GetProcAddress) en vez de SDL_GL_GetProcAddress, y todas las llamadas gl* del
// runtime (punteros de glad) van a dkgl sin cambiar el código del renderer. Ver r9/r15dk/DISENO_DEKO3D.md §3.
// Solo Switch (__SWITCH__).
namespace dkgl {

// Crea el dispositivo deko3d, la cola, la memoria y el swapchain sobre nwindowGetDefault() (que nadie más
// debe tener conectado: ni EGL ni la consola de texto). false si algo falla (entonces se usa GL).
bool Init();
bool Active();
// La función de carga para glad y para las que el renderer pide por nombre (glBufferStorage, SSO...).
void* GetProcAddress(const char* name);
// = SDL_GL_SwapWindow: presenta la imagen del framebuffer 0 y cierra el frame (vallas, reciclado).
void SwapBuffers();
void GetDrawableSize(int* w, int* h);
void SetSwapInterval(int interval);
void Shutdown();

}  // namespace dkgl

// Para el código que pide funciones GL por nombre (renderer_gl.cpp): dkgl si está activo, si no SDL.
void* GX_GLProc(const char* name);
bool GX_DkActive();

// Ventana/presentación para el renderer (barra del prewarm, overlay): SDL_GL_* o dkgl.
struct SDL_Window;
SDL_Window* GX_HostWindow();
void GX_HostDrawable(SDL_Window* win, int* w, int* h);
int GX_HostGetSwapInterval();
int GX_HostSetSwapInterval(int interval);
void GX_HostSwap(SDL_Window* win);
