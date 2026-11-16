#include <string>
#if defined(__linux__) && !defined(__SWITCH__)
#include <malloc.h>
#include <unistd.h>
#endif
#include "input/input_manager.h"
#include "input/sources/gamepad_source.h"
#include "input/sources/mouse_keyboard_source.h"
#include "input/sources/phone_source.h"
#include "loader/loader.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "runtime/virtual_disc.h"
#include "runtime/res_scale.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <SDL.h>
#include <glad/glad.h>
#include <thread>
#include <vector>

#include "runtime/gx_state.h"
#include "runtime/hw/hw.h"

#ifdef __SWITCH__
#include <pthread.h>
#include <switch.h>
#include "../platform/switch/switch_platform.h"
#include "../platform/switch/switch_audio.h"
#include "../hle/gx/dk/dkgl.h"  // r15dk: backend deko3d
#endif
#if defined(__linux__)
#include <pthread.h>
#include <ctime>
#endif

void vtl_note(uint32_t kind, uint32_t arg);  // r37v (gx.cpp): NWII_V_TL
void vtl_init();
// r44w (frente W): renderizador nativo de la superficie del agua (w_agua_gen.cpp, NWII_W_AGUA)
extern "C" void w_agua_install(nwii::runtime::CPUContext *ctx);
namespace nwii::runtime {
    namespace hw { extern uint32_t g_vi_top_field_base; }
    // CPU-thread sleeps in the guest OS idle loop (os_hle_stubs.cpp idle_wait).
    extern std::atomic<uint64_t> g_idle_sleeps, g_idle_sleep_us;
    // Overload governor stats (os_hle_stubs.cpp).
    void governor_report(char *buf, size_t n, double dt);
}

namespace nwii::runtime {
uint32_t g_debug_pc = 0;
MMU *g_mmu = nullptr;
CPUContext *g_ctx_ptr = nullptr;

bool init(const char *config_path = "config.toml") {
  Config::get().load(config_path);
  hw::register_all_hw(MMIODispatcher::get());
  return true;
}
void shutdown() {}
} 

#include "tprof.h"  // r31t
#include "s2.h"     // r34s2
extern "C" void run_game(nwii::runtime::CPUContext &ctx);

// Window-loop presentation counters. g_stat_frames (gx.cpp) counts host
// renders: one per FIFO drain that found a frame end, with any older complete
// frames in that drain merged into it. Per buffer swap: g_stat_presented
// counts swaps that showed a new render, g_stat_repeats swaps that showed the
// previous one again, g_stat_unshown renders replaced by a newer render
// before any swap showed them.
uint64_t g_stat_presented = 0, g_stat_repeats = 0, g_stat_unshown = 0;

void cpu_thread_func(nwii::runtime::CPUContext *ctx) {
#ifndef __SWITCH__
  nwii::runtime::s2::pc_sched(1);  // r38c: NWII_C_PCSCHED (solo PC)
#endif
  nwii::runtime::g_ctx_ptr = ctx;
  {
    // Leave 1 MiB of headroom below the guest call chain (stack: 8 MiB for a
    // Linux std::thread, 32 MiB for the Switch pthread created below).
#ifdef __SWITCH__
    constexpr uintptr_t kStack = 32u << 20;
#else
    constexpr uintptr_t kStack = 8u << 20;
#endif
    char top;
    // R9 guestloop: red zone [lo, lo + span] (r8: sp <= limit fails).
    const uintptr_t limit = (uintptr_t)&top - (kStack - (1u << 20));
    nwii::runtime::g_native_stack = {limit - (1u << 20), 1u << 20};
  }
  std::cout << "[Thread] CPU Core started. pc=" << std::hex << ctx->pc << " is_running=" << ctx->is_running << std::endl;
  std::flush(std::cout);
  nwii::runtime::tprof::attach_cpu_thread();  // r31t (NWII_TPROF, solo PC)
  run_game(*ctx);

  
  
  if (!ctx->is_running) {
    std::cout << "[Thread] CPU Core exited: shutdown requested (window closed "
                 "or quit). NOT a crash." << std::endl;
  } else {
    std::cout << "[Thread] CPU Core CRASHED (jumped to 0). Final PC: 0x"
              << std::hex << ctx->pc << ", LR: 0x" << ctx->lr << std::dec
              << std::endl;
  }
}

namespace nwii::runtime::gx {
void CaptureRecord(unsigned fbo, int x0, int y0, int x1, int y1, uint32_t unshown);
std::string CaptureSave(int kind, unsigned fbo, int x0, int y0, int x1, int y1);
}

// Internal resolution (renderer_gl.cpp): EFB storage at 640s x 528s, s a
// float (r16res: fractional scales; GL sizes from GX_GetEfbSize).
void GX_CreateEfb(float scale, unsigned *fbo, unsigned *tex);
bool GX_ResizeEfb(float scale, unsigned *tex);
float GX_GetEfbScale();
void GX_GetEfbSize(int *w, int *h);
int GX_GetXfbLines();
void GX_PresentBlit(unsigned src, int sx0, int sy0, int sx1, int sy1, int dx0,
                    int dy0, int dx1, int dy1);

// r15dk: backend deko3d (solo Switch). [graphics] backend = "deko3d" (config.cpp lo pasa a NWII_BACKEND) o
// NWII_BACKEND=deko3d en debug.toml. dkgl implementa el GL del runtime sobre deko3d y se carga en glad.
static bool g_use_dk = false;
static void HostDrawableSize(SDL_Window *win, int *w, int *h) {
#ifdef __SWITCH__
  if (g_use_dk) { dkgl::GetDrawableSize(w, h); return; }
#endif
  SDL_GL_GetDrawableSize(win, w, h);
}
static void HostSwap(SDL_Window *win) {
#ifdef __SWITCH__
  if (g_use_dk) { dkgl::SwapBuffers(); return; }
#endif
  SDL_GL_SwapWindow(win);
}

// The internal resolution wanted right now (r16res: a float scale, see
// runtime/res_scale.h). [graphics] internal_resolution = "auto" | 1..4 |
// "1.3636" | "720p" ...; NWII_INTERNAL_RES (same syntax) overrides it (tests).
//   PC, auto: the scale at which the presented 16:9 frame (the game's XFB,
//     456 lines) is exactly as tall as its place in the window, so the
//     present blit is 1:1 vertically; it follows window resizes.
//   Switch, auto: "1080p" docked (1920x1080 output) and "720p" handheld
//     (1280x720 screen): 1:1 present on both. [graphics]
//     internal_resolution_handheld sets the undocked scale separately.
//     Re-evaluated every host frame: docking and undocking change it at run
//     time (appletGetOperationMode is libnx's cached value, updated by
//     appletMainLoop).
static float DesiredInternalScale(SDL_Window *win) {
  namespace res = nwii::runtime::res;
  static const res::Spec env = [] {
    res::Spec sp;
    const char *e = std::getenv("NWII_INTERNAL_RES");
    if (e && *e && !res::Parse(e, sp)) sp = {res::Spec::Auto, 0.0f};
    return sp;
  }();
  // NWII_INTERNAL_RES_CYCLE=<n> (tests): 1x, 2x, 3x, 2x, 1x... changing every
  // n rendered frames, to exercise the run-time resize (Switch docking);
  // NWII_INTERNAL_RES_CYCLE_FRAC=1: 1x, 1.3636x, 2x, 1.5789x instead.
  static const long cycle = [] {
    const char *e = std::getenv("NWII_INTERNAL_RES_CYCLE");
    return e ? std::atol(e) : 0L;
  }();
  if (cycle > 0) {
    extern uint64_t g_stat_frames;
    static const bool frac = std::getenv("NWII_INTERNAL_RES_CYCLE_FRAC") != nullptr;
    static const float seq[2][4] = {{1, 2, 3, 2}, {1, 1.3636f, 2, 1.5789f}};
    return seq[frac][(g_stat_frames / (uint64_t)cycle) % 4];
  }
  res::Spec want = env.kind != res::Spec::Unset ? env : res::g_cfg[0];
  float auto_lines = 0.0f;
#ifdef __SWITCH__
  (void)win;
  const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
  if (!docked && env.kind == res::Spec::Unset && res::g_cfg[1].kind != res::Spec::Unset)
    want = res::g_cfg[1];
  auto_lines = docked ? 1080.0f : 720.0f;
#else
  int w = 0, h = 0;
  if (win) HostDrawableSize(win, &w, &h);
  auto_lines = (float)std::min(h, (int)(w * 9LL / 16));  // 16:9 frame height
#endif
  if (want.kind == res::Spec::Unset) want = {res::Spec::Auto, 0.0f};
  return res::Resolve(want, auto_lines, GX_GetXfbLines());
}

namespace nwii::runtime::gx { void DrawPerfOverlay(); }  // renderer_gl.cpp (r10)

// r41m (frente M) m6d/m6e: interruptores de los frentes V, D y R2 ENCENDIDOS POR DEFECTO (el arreglo de C, NWII_C_*,
// ya va encendido en su propio codigo; NWII_C_AXLIST_OFF / NWII_C_SPIN_OFF / NWII_C_VIGIA_OFF lo quitan). Se ponen en el entorno al
// arrancar (antes de que nadie los lea; NWII_ENV guarda el primer valor) SOLO si no estan ya: un valor explicito
// (debug.toml / env.txt / PC) manda. Apagar (apply_env de la Switch borra los "0", por eso son variables aparte):
//   <NOMBRE>_OFF=1  apaga uno (p. ej. NWII_V_LATE_OFF, NWII_R2_XDUBO_OFF; NWII_R_CULL_OFF deja CULL como en m5 = 1)
//   NWII_V_OFF / NWII_D_OFF / NWII_R2_OFF = 1  apagan su grupo (NWII_R_OFF de m5 tambien quita el CULL=2)
//   NWII_M6_OFF=1  ninguno de estos valores por defecto (= m6c con todo apagado).
static void m6_defaults() {
  // `def` = false: entrada preparada pero APAGADA (no se pone; se activa a mano con la variable o cambiando a true).
  struct Def { const char *name, *val, *group; bool def = true; };
  static const Def k[] = {
      {"NWII_V_WAKE", "1", "NWII_V_OFF"},       {"NWII_V_LATE", "8", "NWII_V_OFF"},
      {"NWII_D_DROPFAST", "1", "NWII_D_OFF"},
      // m6f: fase 2 de D (consola D09: agua 41,9 -> 52,0, castillo 54,3 -> 56,5; EAGERDECODE fuera a proposito)
      {"NWII_D_PARSESTOP", "1", "NWII_D_OFF"},  {"NWII_D_LIGHTPARSE", "1", "NWII_D_OFF"},
      {"NWII_D_FASTDECODE", "1", "NWII_D_OFF"}, {"NWII_D_FASTPREP", "1", "NWII_D_OFF"},
      {"NWII_D_RENDERPREP", "1", "NWII_D_OFF", false},  // preparado: hasta ver D11/D12
      {"NWII_D_MERGE", "1", "NWII_D_OFF"},             // m6i: encendido (M23/M25: agua 52 -> 56)
      {"NWII_R2_LATEFREE", "1", "NWII_R2_OFF"}, {"NWII_R2_CLEARTEX", "1", "NWII_R2_OFF"},
      {"NWII_R2_COPYFBO", "1", "NWII_R2_OFF"},  {"NWII_R2_LAZYNAT", "1", "NWII_R2_OFF"},
      {"NWII_R2_COPYUNI", "1", "NWII_R2_OFF"},  {"NWII_R2_COPYST", "1", "NWII_R2_OFF"},
      {"NWII_R2_LAZYEFB", "1", "NWII_R2_OFF"},  {"NWII_R2_PREHASH", "1", "NWII_R2_OFF", false},  // m6h: preparado (M17: +1 ms render, 0 fps; exactitud)
      {"NWII_R2_TEXMEMO", "1", "NWII_R2_OFF"},  {"NWII_R2_XDUBO", "1", "NWII_R2_OFF"},
      {"NWII_R_CULL", "2", "NWII_R_OFF"},
      // m6g: frente K (sondeo lento cada 128; consola K02: -0,4 a -0,9 ms juego, FC 5/5 en PC). Encendido.
      {"NWII_POLL_EVERY", "128", "NWII_K_OFF"},
      // m6i: encendidos tras M22-M28 en consola (frentes W, A, M)
      {"NWII_W_AGUA", "1", "NWII_W_OFF"},               // frente W: renderizador de líquidos nativo (w3, 5 rutas)
      {"NWII_W_WGP", "1", "NWII_W_OFF"},                // frente W: escritura al pipe ligera
      {"NWII_A_MIX", "2", "NWII_A_OFF"},                // frente A: mezclador AX nativo + ADPCM en lote
      {"NWII_A_NOSPIN", "1", "NWII_A_OFF"},             // frente A: esperas cortas duermen
      {"NWII_A_REQSPIN", "200", "NWII_A_OFF"},          // frente A: giro de peticiones
      {"NWII_AXFX_KERNEL", "4", "NWII_A_OFF", false},    // frente A: reverb vectorizada (objeto output_24 de A), preparado
      {"NWII_M_LOGBG", "1", "NWII_M_OFF"},              // frente M: registro en hilo de fondo (lo lee ANTES logbg_start, misma regla)
      // r45p (frente P, selector de mundos): consola P31-P48; decisión del coordinador (tanda P42-P47: P43 DLMERGE+CULLSKIP 59,8)
      {"NWII_P_DLMERGE", "1", "NWII_P_OFF"},            // draws seguidos de una lista fundidos (selector: render 21,7 -> 13,7 ms)
      {"NWII_P_CULLSKIP", "1", "NWII_P_OFF"},           // sin prueba de recorte tras un fotograma sin recortes
      {"NWII_P_TRIPLE", "1", "NWII_P_OFF", false},      // preparado: tres paquetes (selector +1 fps solo; junto con el resto, peor)
      {"NWII_P_KEEPCOPY", "1", "NWII_P_OFF", false},    // preparado (necesita TRIPLE): no descartar un fotograma con una copia EFB única
      {"NWII_R2_COPYMRT", "1", "NWII_P_OFF", false},    // preparado: hundía el desierto con copias grandes (P28: 59 -> 47)
      {"NWII_P_MRT_SMALL", "32", "NWII_P_OFF", false},  // preparado: con COPYMRT, solo copias de hasta 32x32 (P36: desierto 59,1)
  };
  if (std::getenv("NWII_M6_OFF")) {
    std::printf("[M6] NWII_M6_OFF: sin valores por defecto de m6\n");
    return;
  }
  std::string on, off;
  for (const Def &d : k) {
    const std::string o = std::string(d.name) + "_OFF";
    if (const char *v = std::getenv(d.name)) {
      on += std::string(" ") + d.name + "=" + v + "(explicito)";
    } else if (!d.def) {
      off += std::string(" ") + d.name + "(preparado)";
    } else if (std::getenv(o.c_str()) || std::getenv(d.group)) {
      off += std::string(" ") + d.name;
    } else {
      setenv(d.name, d.val, 1);
      on += std::string(" ") + d.name + "=" + d.val;
    }
  }
  std::printf("[M6] por defecto:%s | apagados:%s\n", on.c_str(), off.empty() ? " ninguno" : off.c_str());
}

int main(int argc, char **argv) {
#ifdef __SWITCH__
  // An NRO gets no arguments and no environment: default paths on the SD
  // card, NWII_* switches from env.txt in the NRO folder, quiet logging, game-file
  // and memory checks (platform/switch/switch_platform.cpp).
  // r15dk: una NRO de prueba cuyo nombre lleva "dk3d" arranca con deko3d salvo NWII_BACKEND explícito
  // (debug.toml / env.txt; NWII_BACKEND = "gl" fuerza OpenGL)
  const bool dk_by_name = argc > 0 && argv[0] && std::strstr(argv[0], "dk3d");
  if (!nwii::runtime::nx::early_init(&argc, &argv))
    return 0;
  if (dk_by_name && !std::getenv("NWII_BACKEND")) setenv("NWII_BACKEND", "deko3d", 1);
#else
  // Real-time clock by default: wall-clock timebase and a single 60 Hz VI
  // source (perf/PROPOSALS.md P5). NWII_REALTIME=0 (or off/false/no) selects
  // the virtual clock (timebase = instruction count, VI from the host loop).
  // The runtime tests the variable for presence, so "off" unsets it. The
  // Switch sets the same default in nx::early_init (env.txt overrides it).
  if (const char *rt = std::getenv("NWII_REALTIME")) {
    std::string v(rt);
    for (char &c : v) c = (char)std::tolower((unsigned char)c);
    if (v.empty() || v == "0" || v == "off" || v == "false" || v == "no")
      unsetenv("NWII_REALTIME");
  } else {
    setenv("NWII_REALTIME", "1", 1);
  }
#endif
  m6_defaults();  // r41m (frente M) m6d
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <path_to_unpacked_game_dir> [config_path]\n";
    return 1;
  }

  const char *config_path = (argc >= 3) ? argv[2] : "config.toml";
  if (!nwii::runtime::init(config_path))
    return 1;
  nwii::runtime::Config::get().game_dir = argv[1];

  {
    using namespace nwii::runtime::input;
    auto& im = InputManager::get();
    int mode = nwii::runtime::Config::get().input_mode;
    switch (mode) {
    case 1: 
      std::cout << "[Input] Mode 1 (Bluetooth Wiimote) not implemented, "
                   "falling back to keyboard+mouse" << std::endl;
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 2: 
    case 3: 
    case 4: 
      im.add_source(std::make_unique<GamepadSource>());
      
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 5: 
      im.add_source(std::make_unique<PhoneSource>());
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 7: 
      std::cout << "[Input] Mode 7 (touch): using pointer-as-touch mapping"
                << std::endl;
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 6:
    default:
      im.add_source(std::make_unique<MouseKeyboardSource>());
      im.add_source(std::make_unique<GamepadSource>());
      break;
    }
  }

  const char *headless_env = std::getenv("NWII_HEADLESS");
  bool headless = headless_env && headless_env[0] == '1';

  SDL_Window* window = nullptr;
  SDL_GLContext gl_ctx = nullptr;
  SDL_AudioDeviceID audio_dev = 0;
  GLuint efb_fbo = 0, efb_tex = 0;
  GLuint xfb_tex = 0;

  if (!headless) {
    // Offscreen test runs (SDL_VIDEODRIVER=offscreen) stay silent unless an
    // audio driver is chosen explicitly; NWII_AUDIODUMP still captures output.
    const char *video_drv = std::getenv("SDL_VIDEODRIVER");
    if (video_drv && std::strcmp(video_drv, "offscreen") == 0)
      setenv("SDL_AUDIODRIVER", "dummy", 0);
#ifdef __SWITCH__
    // r10e: no SDL game controllers on the Switch: the players come from libnx
    // (switch_input.cpp), and SDL's Switch joystick driver opens the system's
    // own 8-player controller applet (hidLaShowControllerSupportForSystem) on
    // its own, right after ours.
    const Uint32 sdl_flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO;
#else
    const Uint32 sdl_flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER;
#endif
    if (SDL_Init(sdl_flags) < 0) {
      std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
      return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    {
      SDL_DisplayMode dm;
      if (SDL_GetCurrentDisplayMode(0, &dm) == 0)
        std::cout << "[GL] display 0: " << dm.w << "x" << dm.h << " fmt="
                  << SDL_GetPixelFormatName(dm.format) << std::endl;
      else
        std::cerr << "[GL] no display mode: " << SDL_GetError()
                  << " (screen locked or no display attached?)" << std::endl;
    }
#ifdef __APPLE__

    
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                        SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    bool use_gl = (nwii::runtime::Config::get().backend == nwii::runtime::Backend::OpenGL);
#ifdef __SWITCH__
    {
      const char *be = std::getenv("NWII_BACKEND");
      g_use_dk = use_gl && be && (!std::strcmp(be, "deko3d") || !std::strcmp(be, "dk"));
    }
#endif
    uint32_t window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    extern void GX_SetWindow(void *);
#ifdef __SWITCH__
    if (g_use_dk) {
      // r15dk: deko3d toma la ventana nativa (nwindowGetDefault) con su swapchain. NO se crea ventana SDL:
      // el driver Switch de SDL configura esa misma ventana (dimensiones/EGL) y entonces dkSwapchainCreate
      // falla, y en deko3d cualquier error es fatal (applet de error). SDL sigue para eventos y audio.
      std::cout << "[GL] NWII_BACKEND=deko3d" << std::endl;
      // la base del lote GPU XF va por el uniform uXDraw (un push constant de 4 B en dkgl, casi gratis): las
      // autopruebas de base instance de R5 usan transform feedback, que deko3d no tiene
      if (!std::getenv("NWII_GPU_XF_BASEINST")) setenv("NWII_GPU_XF_BASEINST", "0", 1);
      if (dkgl::Init() && gladLoadGLLoader((GLADloadproc)dkgl::GetProcAddress)) {
        std::cout << "[GL] backend deko3d: " << (const char *)glGetString(GL_RENDERER) << " | "
                  << (const char *)glGetString(GL_VERSION) << std::endl;
        static int s_dk_window;  // el renderer solo mira que haya "ventana" (no headless)
        GX_SetWindow(&s_dk_window);
      } else {
        std::cout << "[GL] backend deko3d NO disponible: se usa OpenGL" << std::endl;
        dkgl::Shutdown();
        g_use_dk = false;
      }
    }
#endif
    if (!g_use_dk) {
      if (use_gl) {
        window_flags |= SDL_WINDOW_OPENGL;
      } else {
#ifdef SDL_WINDOW_METAL
        window_flags |= SDL_WINDOW_METAL;
#endif
      }
      window = SDL_CreateWindow("NWiiRecomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                nwii::runtime::Config::get().window_width,
                                nwii::runtime::Config::get().window_height,
                                window_flags);
      if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        return 1;
      }
      GX_SetWindow(window);
    }
    if (use_gl && !g_use_dk) {
      gl_ctx = SDL_GL_CreateContext(window);
      if (!gl_ctx) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << "\n";
        return 1;
      }
      if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        std::cerr << "glad: failed to load GL functions\n";
        return 1;
      }
      std::cout << "[GL] " << (const char *)glGetString(GL_RENDERER) << " | "
                << (const char *)glGetString(GL_VERSION) << std::endl;
      SDL_GL_SetSwapInterval(1);
    }
    
    // GX EFB is 640x528; with an internal resolution s the GL EFB is
    // 640s x 528s (renderer_gl.cpp GX_CreateEfb, RGBA8 + 24-bit depth).
    GX_CreateEfb(DesiredInternalScale(window), &efb_fbo, &efb_tex);
    {
      int ew = 0, eh = 0;
      GX_GetEfbSize(&ew, &eh);
      std::cout << "[GL] internal resolution " << GX_GetEfbScale() << "x (EFB " << ew
                << "x" << eh << ")" << std::endl;
    }
    {
      
      GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      std::cout << "[GL] EFB FBO status: 0x" << std::hex << st << std::dec
                << (st == GL_FRAMEBUFFER_COMPLETE ? " (complete)" : " (INCOMPLETE!)")
                << std::endl;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glGenTextures(1, &xfb_tex);
    glBindTexture(GL_TEXTURE_2D, xfb_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

#ifdef __SWITCH__
    // Native audren output on its own thread (switch_audio.cpp); the SDL
    // device below only with NWII_NX_AUDIO=sdl or if audren fails.
    const bool nx_audio = nwii::runtime::nx::audio_start();
#else
    const bool nx_audio = false;
#endif
    SDL_AudioSpec want = {0}, have;
    want.freq = 32000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    if (!nx_audio)
      audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (nx_audio) {
      // its own "[NX] audio:" line says how it was set up
    } else if (audio_dev > 0) {
      SDL_PauseAudioDevice(audio_dev, 0);
      std::cout << "[Audio] " << SDL_GetCurrentAudioDriver() << " device open, game "
                << "audio 32000 Hz stereo (device " << have.freq << " Hz)" << std::endl;
    } else {
      std::cout << "[Audio] no audio device: " << SDL_GetError() << std::endl;
    }
  }

  auto ctx = std::make_unique<nwii::runtime::CPUContext>();
  nwii::runtime::g_mmu = &ctx->mmu;

  nwii::loader::Executable exe;
  if (!exe.load_unpacked_game(argv[1]))
    return 1;

  uint32_t arena_lo = 0x80000000;
  
  for (const auto &sec : exe.sections) {
    uint32_t end_addr = sec.address + sec.size;
    if (end_addr > arena_lo)
      arena_lo = end_addr;

    if (sec.is_bss) {
      for (size_t i = 0; i < sec.size; ++i)
        ctx->mmu.write8(sec.address + i, 0);
    }
  }

  for (const auto &sec : exe.sections) {
    if (!sec.is_bss) {
      std::cout << "[Loader] Loading section at 0x" << std::hex << sec.address << " size 0x" << sec.size << std::dec << std::endl;
      for (size_t i = 0; i < sec.size; ++i) {
        if ((sec.address + i) == 0x80003448) {
            std::cout << "[Loader] Found 0x80003448 inside section! val=" << std::hex << (uint32_t)sec.data[i] << std::dec << std::endl;
        }
        ctx->mmu.write8(sec.address + i, sec.data[i]);
      }
    }

    

    
    
    if (sec.is_text) {
      uint32_t start = sec.address;
      if (std::getenv("NWII_INTERP_LOW"))
        start = std::max<uint32_t>(sec.address, 0x80004000);
      uint32_t end = sec.address + sec.size;
      if (end > start)
        nwii::runtime::add_recompiled_range(start, end);
    }
  }

  arena_lo = (arena_lo + 31) & ~31;

  

  {
    uint32_t stack_top = 0;
    uint32_t hi = 0;
    bool have_hi = false;
    for (uint32_t off = 0; off < 0x200; off += 4) {
      uint32_t insn = ctx->mmu.read32(exe.entry_point + off);
      if ((insn & 0xFFFF0000) == 0x3C200000) { 
        hi = (insn & 0xFFFF) << 16;
        have_hi = true;
      } else if (have_hi && (insn & 0xFFFF0000) == 0x60210000) { 
        stack_top = hi | (insn & 0xFFFF);
        break;
      } else if (have_hi && (insn & 0xFFFF0000) == 0x38210000) { 
        stack_top = hi + (int16_t)(insn & 0xFFFF);
        break;
      }
    }
    if (stack_top >= arena_lo && stack_top < 0x81800000) {
      std::cout << "[Loader] crt stack top 0x" << std::hex << stack_top
                << " above section end 0x" << arena_lo
                << ", raising ArenaLo" << std::dec << std::endl;

      arena_lo = (stack_top + 0x100 + 31) & ~31u;
    }
  }

  bool is_gc = nwii::runtime::Config::get().platform ==
               nwii::runtime::Platform::GameCube;

  
  uint32_t arena_hi = 0x81700000;
  auto &vdisc = nwii::runtime::VirtualDisc::get();
  if (vdisc.init(argv[1])) {
    const auto &boot = vdisc.boot_data();
    
    for (size_t i = 0; i < 0x20 && i < boot.size(); ++i)
      ctx->mmu.write8(0x80000000 + (uint32_t)i, boot[i]);

    const auto &fst = vdisc.fst_data();
    uint32_t fst_size = (uint32_t)fst.size();
    uint32_t fst_addr = (0x81800000 - fst_size) & ~63u;
    for (uint32_t i = 0; i < fst_size; ++i)
      ctx->mmu.write8(fst_addr + i, fst[i]);

    ctx->mmu.write32(0x80000038, fst_addr); 
    ctx->mmu.write32(0x8000003C, fst_size); 
    if (fst_addr < 0x81700000)
      arena_hi = fst_addr & ~31u;
    std::cout << "[Loader] FST loaded at 0x" << std::hex << fst_addr
              << " size 0x" << fst_size << std::dec << std::endl;
  }

  // Console type (Dolphin BootManager): Wii retail Hollywood = 0x10000006,
  
  uint32_t console_type = is_gc ? 0x00000003 : 0x10000006;
  uint32_t mem1_size = 24 * 1024 * 1024; 
  uint32_t mem2_size = 64 * 1024 * 1024; 

  ctx->mmu.write32(0x80000020, is_gc ? 0x0D15EA5E : console_type); 
  
  ctx->mmu.write32(0x80000024, mem1_size);
  
  ctx->mmu.write32(0x80000028, mem1_size);
  ctx->mmu.write32(0x8000002C, console_type);
  // Arena bounds, apploader-style (matches Dolphin BS2 emulation):

  

  if (!is_gc) {
    ctx->mmu.write32(0x80000030, arena_lo);
    ctx->mmu.write32(0x80000034, arena_hi);
  }
  if (!vdisc.valid()) {
    
    ctx->mmu.write32(0x80000038, arena_hi);
  }
  if (is_gc) {

    

    uint32_t gc_arena_hi = arena_hi;
    uint32_t fst_base = ctx->mmu.read32(0x80000038);
    if (fst_base >= 0x80000000 && fst_base < 0x81800000)
      gc_arena_hi = fst_base & ~31u;
    std::filesystem::path bi2_path =
        std::filesystem::path(argv[1]) / "sys" / "bi2.bin";
    if (std::filesystem::exists(bi2_path)) {
      std::ifstream bi2(bi2_path, std::ios::binary);
      std::vector<char> bi2_data((std::istreambuf_iterator<char>(bi2)),
                                 std::istreambuf_iterator<char>());
      uint32_t bi2_addr = 0x81200000; 
      for (size_t i = 0; i < bi2_data.size(); ++i)
        ctx->mmu.write8(bi2_addr + (uint32_t)i, (uint8_t)bi2_data[i]);
      ctx->mmu.write32(0x800000F4, bi2_addr);
      std::cout << "[Loader] BI2 loaded at 0x" << std::hex << bi2_addr
                << std::dec << std::endl;
    }

    
    
    ctx->mmu.write32(0x80000030, arena_lo);
    ctx->mmu.write32(0x80000034, gc_arena_hi);
    std::cout << "[Loader] GC Arena = 0x" << std::hex << arena_lo << " - 0x"
              << gc_arena_hi << " (" << std::dec
              << (gc_arena_hi - arena_lo) / 1024 << " KiB)" << std::endl;
  }
  
  ctx->mmu.write32(0x800000F0, mem1_size);

  

  
  
  auto poke_global = [&](uint32_t addr, uint32_t val) {
    for (const auto &sec : exe.sections)
      if (!sec.is_bss && addr >= sec.address && addr < sec.address + sec.size)
        return; 
    ctx->mmu.write32(addr, val);
  };

  if (!is_gc) {

    poke_global(0x80003118, mem2_size);
    poke_global(0x8000311C, mem2_size);
    
    poke_global(0x80003124, 0x90000800);
    poke_global(0x80003128, 0x93e00000);
    poke_global(0x80003130, 0x93e00000);
    poke_global(0x80003134, 0x94000000);
    
    poke_global(0x80003158, 0x00000023);
  }

  

  

  {
    const std::string &gid = nwii::runtime::Config::get().game_id;
    char region = gid.size() >= 4 ? gid[3] : 'E';
    
    bool pal = (region == 'P' || region == 'D' || region == 'F' ||
                region == 'I' || region == 'S' || region == 'H' ||
                region == 'U' || region == 'X' || region == 'Y' ||
                region == 'Z');
    poke_global(0x800000CC, pal ? 1u : 0u);
  }

  ctx->gpr[1] = 0x816FFFF0;

  // Low memory 0xF8/0xFC hold the BUS and CPU clocks (Dolphin writes

  
  
  if (is_gc) {
    ctx->mmu.write32(0x800000F8, 162000000); 
    ctx->mmu.write32(0x800000FC, 486000000); 
    ctx->tb_freq = 40500000;                 
  } else {
    ctx->mmu.write32(0x800000F8, 243000000); 
    ctx->mmu.write32(0x800000FC, 729000000); 
    ctx->tb_freq = 60750000;                 
  }

  ctx->tb_start = std::chrono::steady_clock::now();
  // r10a fix: fix the host clock origin of the guest timebase now, while this
  // is the only thread (see tick_timebase in os_hle_stubs.cpp).
  (void)ctx->wall_timebase_now();

  nwii::runtime::init_ipc_client(*ctx);

  // VI interrupts also come from the guest timebase (ios.cpp, every
  // tb_freq/60 ticks). The host loop raises one more per presented frame; once
  // the CPU runs faster than real time the two together over-drive the game
  // (measured: 145 game frames/s). NWII_REALTIME=1 keeps only the timebase
  // source on a wall clock: exactly 60 game frames/s.
  const bool host_vi = !NWII_ENV("NWII_REALTIME");

  // With the real-time clock the game makes its frames on that guest VI, so
  // the window loop presents each one as soon as it is complete (see
  // wait_next_frame below) instead of on its own free-running 60 Hz timer:
  // two unsynchronised 60 Hz clocks made the host merge or skip about a third
  // of the game's frames (38-40 presented/s for 60 produced). NWII_FIXED_PACE=1
  // restores the fixed host timer (NWII_HOST_FPS / NWII_HOST_PACE); it is a
  // presence switch so it also works from the Switch env.txt.
  const bool frame_paced = !host_vi && !NWII_ENV("NWII_FIXED_PACE");
  std::cout << "[Timing] clock="
            << (host_vi ? "virtual (instruction count, VI from the host loop)"
                        : "real-time (NWII_REALTIME=0 for the virtual clock)")
            << ", presentation="
            << (frame_paced ? "per game frame" : "fixed host timer") << std::endl;

  // Host loop pacing. Without vsync (SDL offscreen driver, headless, high
  // refresh displays) the present loop would spin uncapped; cap it at
  // NWII_HOST_FPS (default 60, 0 = uncapped). With vsync at 60 Hz the swap
  // already takes a full period and this never sleeps.
  struct HostPacer {
    std::chrono::steady_clock::duration period{};
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();
    void wait() {
      if (period.count() == 0) return;
      next += period;
      auto now = std::chrono::steady_clock::now();
      if (now < next)
        std::this_thread::sleep_until(next);
      else if (now - next > period)
        next = now;  // fell behind: don't try to catch up with a burst
    }
  } host_pacer;
  {
    const char *hf = NWII_ENV("NWII_HOST_FPS");
    double fps = hf ? std::atof(hf) : 60.0;
    if (fps > 0)
      host_pacer.period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(1.0 / fps));
  }

  ::vtl_init();  // r37v: NWII_V_TL=<ruta>
  w_agua_install(ctx.get());  // r44w: NWII_W_AGUA (antes del primer bctrl: la tabla hash de g7 se monta después)
  nwii::runtime::s2::init(ctx.get());  // r34s2 (NWII_S2=1): CPU de sonido
#ifndef __SWITCH__
  nwii::runtime::s2::pc_sched(0);  // r38c: NWII_C_PCSCHED (solo PC): este hilo es el de render
#endif
#ifdef __SWITCH__
  // Every guest call is a native call, so the host stack grows as deep as the
  // game's call chain; libnx gives std::thread no way to size it.
  pthread_attr_t cpu_attr;
  pthread_attr_init(&cpu_attr);
  pthread_attr_setstacksize(&cpu_attr, 32 * 1024 * 1024);
  pthread_t cpu_pthread;
  pthread_create(
      &cpu_pthread, &cpu_attr,
      [](void *p) -> void * {
        nwii::runtime::nx::cpu_thread_start();  // own core, off the render one
        cpu_thread_func((nwii::runtime::CPUContext *)p);
        nwii::runtime::nx::cpu_thread_finished();
        return nullptr;
      },
      ctx.get());
#else
  std::thread cpu_thread(cpu_thread_func, ctx.get());
#endif

  if (headless) {
    
    uint64_t tick = 0;
    while (ctx->is_running) {
      host_pacer.wait();

      
      nwii::runtime::input::InputManager::get().update();

      extern void ProcessGXFifo();
      ProcessGXFifo();
      if (host_vi) { ctx->vblank_pending = true; ctx->poll_kick(); }  // R9: poll sees it
      if (std::getenv("NWII_SAMPLE") && (++tick % 60) == 0) {
        std::cout << "[Sample] pc=0x" << std::hex << ctx->pc << " lr=0x"
                  << ctx->lr << " msr=0x" << ctx->msr << " r3=0x" << ctx->gpr[3]
                  << " cb=" << (ctx->in_callback ? 1 : 0) << " inst=" << std::dec
                  << ctx->inst_count << "\n";
        
        if (const char *env = std::getenv("NWII_PEEK")) {
          uint32_t pa = 0, pw = 8;
          if (std::sscanf(env, "%x,%u", &pa, &pw) >= 1 && pa) {
            std::cout << "[Peek] " << std::hex << pa << ":";
            for (uint32_t i = 0; i < pw && i < 32; i++)
              std::cout << " " << ctx->mmu.read32(pa + i * 4);
            std::cout << std::dec << "\n";
          }
        }

        
        if ((tick % 300) == 0 && ctx->gpr[13] >= 0x80000000u) {
          uint32_t r13 = ctx->gpr[13];
          std::cout << "  [RunQ] bitmap=0x" << std::hex
                    << ctx->mmu.read32(r13 - 0x21c8) << " resched=0x"
                    << ctx->mmu.read32(r13 - 0x21c4) << " cur=0x"
                    << ctx->mmu.read32(0x800000E4)

                    << " retrace=" << std::dec << ctx->mmu.read32(r13 - 0x2358)
                    << std::hex << " postCB=0x" << ctx->mmu.read32(r13 - 0x2344)
                    << std::dec << "\n";

          
          uint32_t vs = ctx->mmu.read32(r13 - 0x29c0);

          
          uint32_t ef = ctx->mmu.read32(r13 - 0x2AF0);
          std::cout << "  [FlipG] vs=0x" << std::hex << vs
                    << " vsA2=" << std::dec << (int)ctx->mmu.read8(vs + 0xa2)
                    << " pend29b6=" << (int)ctx->mmu.read8(r13 - 0x29b6)
                    << " b29b5=" << (int)ctx->mmu.read8(r13 - 0x29b5)
                    << " b4808=" << (int)ctx->mmu.read8(r13 - 0x4808)
                    << " ef=0x" << std::hex << ef
                    << " efB0=0x" << (ef ? ctx->mmu.read32(ef + 0xb0) : 0)
                    << " efB4=0x" << (ef ? ctx->mmu.read32(ef + 0xb4) : 0)
                    << " efB8=" << std::dec << (ef ? (int)ctx->mmu.read8(ef + 0xb8) : -1)
                    << " efBE=" << (ef ? (int)ctx->mmu.read8(ef + 0xbe) : -1)
                    << " ef9C=" << (ef ? (int)ctx->mmu.read32(ef + 0x9c) : -1)
                    << " ddflag=" << (int)ctx->mmu.read8(r13 - 0x2368)
                    << std::hex << " tok=0x" << ctx->mmu.read16(r13 - 0x29b8)
                    << " pe_sr=0x" << nwii::runtime::hw::g_pe_sr
                    << std::dec << "\n";

          
          std::cout << "  [VIwrap] hwp=0x" << std::hex
                    << ctx->mmu.read32(r13 - 0x4118) << " shp=0x"
                    << ctx->mmu.read32(r13 - 0x239c) << " sdkPre=0x"
                    << ctx->mmu.read32(r13 - 0x2348) << " wrapPre=0x"
                    << ctx->mmu.read32(r13 - 0x237c) << std::dec << "\n";
        }

        
        
        if ((tick % 300) == 0) {
          uint32_t cur = ctx->mmu.read32(0x800000E4);
          uint32_t th = ctx->mmu.read32(0x800000DC);
          int guard = 0;
          while (th >= 0x80000000u && th < 0x81800000u && guard++ < 16) {
          uint32_t q = 0x803540e0;
          std::cout << "Queue " << std::hex << q << ": msgArray=" << ctx->mmu.read32(q+16) << " count=" << ctx->mmu.read32(q+20) << " first=" << ctx->mmu.read32(q+24) << " used=" << ctx->mmu.read32(q+28) << "\n";

            uint16_t state = ctx->mmu.read16(th + 0x2C8);
            uint32_t prio = ctx->mmu.read32(th + 0x2D0);
            uint32_t wq = ctx->mmu.read32(th + 0x2DC);
            uint32_t srr0 = ctx->mmu.read32(th + 0x198);
            std::cout << "  [Thr] 0x" << std::hex << th << (th == cur ? "*" : " ")
                      << " st=" << std::dec << state << " pr=" << prio
                      << " wq=0x" << std::hex << wq << " srr0=0x" << srr0;
            if (wq >= 0x80000000u && wq < 0x81800000u) {
              std::cout << " wq[-2..5]:";
              for (int i = -2; i < 6; ++i)
                std::cout << " " << ctx->mmu.read32(wq + i * 4);
            }
            std::cout << std::dec << "\n";
            th = ctx->mmu.read32(th + 0x2FC);
          }
        }
      }
    }
  } else {
    
    std::vector<unsigned char> xfb_px;
    unsigned xfb_w = 0, xfb_h = 0;
    bool quit = false;

    // Frame-paced presentation (real-time clock). The game completes a frame
    // (its XFB copy) once per guest VI, 1/60 s apart. wait_next_frame is the
    // only FIFO drain in this mode (the loop-top one is skipped): it returns
    // as soon as a drain rendered a frame, and the loop's next pass presents
    // exactly that render, so no render is overwritten before it is shown.
    // Between frames it sleeps until 4 ms before the next one is expected,
    // then drains every NWII_PRESENT_POLL_US (default 1000). It never sleeps
    // when that time has already passed (host behind: render, present,
    // render). After 25 ms without a frame (loading, a stalled game) it
    // returns anyway and the loop shows the last frame again, so audio
    // feeding, input and the window keep running.
    auto wait_next_frame = [&] {
      extern uint64_t g_stat_frames;
      extern void ProcessGXFifo();
      using clk = std::chrono::steady_clock;
      static const auto poll = std::chrono::microseconds([] {
        const char *e = NWII_ENV("NWII_PRESENT_POLL_US");
        long v = e ? std::atol(e) : 1000;
        return v < 100 ? 100L : v > 8000 ? 8000L : v;
      }());
      constexpr auto period = std::chrono::microseconds(16667);
      constexpr auto margin = std::chrono::microseconds(4000);
      // When the next frame is expected. A frame found by a drain that
      // followed an empty one arrived within the last poll interval, which
      // fixes the phase: the next one is due a period later. A frame that
      // was already waiting at the first drain arrived at some unknown
      // earlier time (the host was late), so the next expectation is pulled
      // in to half a period and the phase is found again from there.
      static clk::time_point next_due = clk::now();
      static bool idle = false;  // the last wait timed out: poll less often
      const uint64_t before = g_stat_frames;
      const auto entry = clk::now();
      const auto deadline = entry + std::chrono::microseconds(25000);
      if (next_due - margin > entry)
        std::this_thread::sleep_until(std::min(next_due - margin, deadline));
      // ProcessGXFifo issues GL calls only when it renders, so the target
      // set here holds for every drain below.
      glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo);
      { int ew, eh; GX_GetEfbSize(&ew, &eh); glViewport(0, 0, ew, eh); }
      bool drained_empty = false;
      for (;;) {
        const auto t = clk::now();
        ProcessGXFifo();
        if (g_stat_frames != before) {
          next_due = t + (drained_empty ? period : period / 2);
          idle = false;
          return;
        }
        drained_empty = true;
        const auto now = clk::now();
        if (now >= deadline || !ctx->is_running) {
          next_due = now;
          idle = true;
          return;
        }
        // R11: with the render worker, wake as soon as it has a frame ready
        // (up to the deadline); without it, sleep one poll interval.
        extern void GX_HostWait(int64_t poll_us, int64_t max_us);
        using std::chrono::duration_cast;
        using std::chrono::microseconds;
        GX_HostWait(duration_cast<microseconds>(
                        std::min<clk::duration>(idle ? 4 * poll : poll, deadline - now)).count(),
                    duration_cast<microseconds>(deadline - now).count());
      }
    };

    // Called right after every buffer swap (see g_stat_presented).
    auto count_present = [] {
      extern uint64_t g_stat_frames;
      static uint64_t last = g_stat_frames;
      const uint64_t renders = g_stat_frames;
      if (renders == last) {
        ++g_stat_repeats;
      } else {
        ++g_stat_presented;
        g_stat_unshown += renders - last - 1;
      }
      last = renders;
    };

    // NWII_FPS=1: every 5 s, swaps that showed a new frame per second, then
    // repeated frames, host renders and renders never shown (see
    // g_stat_presented), guest instructions/s and how busy the two threads
    // are (the CPU thread's idle-loop sleeps included).
    auto perf_report = [&] {
      static const bool on = NWII_ENV("NWII_FPS") != nullptr;
      if (!on) return;
      extern uint64_t g_stat_frames;
      using clk = std::chrono::steady_clock;
      static clk::time_point t0 = clk::now();
      static uint64_t frames0 = g_stat_frames, inst0 = ctx->inst_count;
      static uint64_t pres0 = g_stat_presented, rep0 = g_stat_repeats,
                      unshown0 = g_stat_unshown;
      static uint64_t sleeps0 = 0, sleep_us0 = 0;
      static double host_cpu0 = 0, cpu_cpu0 = 0;
      const auto now = clk::now();
      const double dt = std::chrono::duration<double>(now - t0).count();
      if (dt < 5.0) return;
      double host_cpu = -1, cpu_cpu = -1;
#if defined(__linux__)
      auto thread_secs = [](clockid_t cid) {
        timespec ts{};
        clock_gettime(cid, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
      };
      host_cpu = thread_secs(CLOCK_THREAD_CPUTIME_ID);
      clockid_t cid;
      if (pthread_getcpuclockid(cpu_thread.native_handle(), &cid) == 0)
        cpu_cpu = thread_secs(cid);
#endif
      const uint64_t frames = g_stat_frames, inst = ctx->inst_count;
      const uint64_t pres = g_stat_presented, rep = g_stat_repeats,
                     unshown = g_stat_unshown;
      const uint64_t sleeps = nwii::runtime::g_idle_sleeps.load();
      const uint64_t sleep_us = nwii::runtime::g_idle_sleep_us.load();
      std::printf("[FPS] %.1f presented/s (repeats %.1f/s, renders %.1f/s, "
                  "never shown %.1f/s), guest %.1f Minst/s",
                  (pres - pres0) / dt, (rep - rep0) / dt,
                  (frames - frames0) / dt, (unshown - unshown0) / dt,
                  (inst - inst0) / dt / 1e6);
      if (host_cpu >= 0)
        std::printf(", host thread %.0f%%", 100.0 * (host_cpu - host_cpu0) / dt);
      if (cpu_cpu >= 0)
        std::printf(", cpu thread %.0f%%", 100.0 * (cpu_cpu - cpu_cpu0) / dt);
      {
        // R11: the render worker (FIFO parse + decode), when it runs.
        extern double GX_WorkerCpuSeconds();
        static double gxw0 = -1;
        const double gxw = GX_WorkerCpuSeconds();
        if (gxw >= 0 && gxw0 >= 0) std::printf(", gx worker %.0f%%", 100.0 * (gxw - gxw0) / dt);
        gxw0 = gxw;
      }
      char gov[384];
      nwii::runtime::governor_report(gov, sizeof gov, dt);
      std::printf(", idle sleeps %.0f/s (%.0f%% of the time)%s\n",
                  (sleeps - sleeps0) / dt, (sleep_us - sleep_us0) / dt / 1e4, gov);
      if (nwii::runtime::s2::g_enabled) {  // r34s2
        char s2l[2048];  // r38c: + linea [C38]; r41m m6h: + [A43]
        nwii::runtime::s2::stats_line(s2l, sizeof s2l, dt);
        std::printf("%s\n", s2l);
      }
#if defined(__linux__) && defined(__GLIBC__)
      {
        // R8 memory: process RSS, malloc heap (in use / free) and what the
        // host caches hold ([NX] heap detail / [NX] mem on the Switch).
        extern size_t GX_MemReport(char *out, size_t n);
        char buf[512];
        const size_t acc = GX_MemReport(buf, sizeof buf);
        const struct mallinfo2 mi = mallinfo2();
        long rss_pages = 0;
        if (FILE *f = std::fopen("/proc/self/statm", "r")) {
          long size = 0;
          if (std::fscanf(f, "%ld %ld", &size, &rss_pages) != 2) rss_pages = 0;
          std::fclose(f);
        }
        const double rss = rss_pages * (double)sysconf(_SC_PAGESIZE) / 1048576.0;
        std::printf("[MEM] rss %.1f MB, malloc in use %.1f MB (arena %.1f MB + mmap %.1f MB),"
                    " free %.1f MB (top %.1f MB); %s\n",
                    rss, mi.uordblks / 1048576.0 + mi.hblkhd / 1048576.0,
                    mi.arena / 1048576.0, mi.hblkhd / 1048576.0, mi.fordblks / 1048576.0,
                    mi.keepcost / 1048576.0, buf);
        (void)acc;
      }
#endif
      std::fflush(stdout);
      t0 = now;
      frames0 = frames; inst0 = inst; sleeps0 = sleeps; sleep_us0 = sleep_us;
      pres0 = pres; rep0 = rep; unshown0 = unshown;
      host_cpu0 = host_cpu; cpu_cpu0 = cpu_cpu;
    };

    // Capture tool (renderer_gl.cpp): F12 saves the last ~2 s of presented
    // frames + a state log, F11 one full-size screenshot, into
    // capturas/<date_time>/ next to the NAND folder. NWII_CAPTURE_AT=<n> /
    // NWII_SHOT_AT=<n> trigger them at render n (tests without a keyboard).
    int capture_request = 0;  // 1 ring (F12), 2 screenshot (F11)
    while (!quit && ctx->is_running) {
      if (!headless) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
          if (e.type == SDL_QUIT) quit = true;
          if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (e.key.keysym.sym == SDLK_F12) capture_request = 1;
            else if (e.key.keysym.sym == SDLK_F11) capture_request = 2;
          }
        }
#ifdef __SWITCH__
        if (!appletMainLoop()) quit = true;
        nwii::runtime::nx::frame_tick();
        if (quit) {
          // Stop the CPU before SDL is torn down after the loop: the Wii
          // Remote HLE polls SDL from the CPU thread.
          ctx->is_running = false;
          for (int i = 0; i < 200 && !nwii::runtime::nx::cpu_thread_done(); ++i)
            svcSleepThread(10'000'000);
        }
#endif
      }

      nwii::runtime::input::InputManager::get().update();

      if (!headless) {
        // Internal resolution follows the window (PC auto) or the console's
        // dock state (Switch): a change that holds for 30 host frames
        // (~0.5 s; window drags do not reallocate every frame) re-sizes the
        // EFB here, between two renders.
        static float pending_scale = 0;
        static int pending_frames = 0;
        const float want = DesiredInternalScale(window);
        if (want == GX_GetEfbScale()) {
          pending_frames = 0;
        } else {
          if (want != pending_scale) { pending_scale = want; pending_frames = 0; }
          if (++pending_frames >= 30) {
            GX_ResizeEfb(want, &efb_tex);
            pending_frames = 0;
          }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo);
        { int ew, eh; GX_GetEfbSize(&ew, &eh); glViewport(0, 0, ew, eh); }
      }

      extern void GX_GetXfb(uint32_t*, unsigned*, unsigned*, unsigned*);
      uint32_t xfb_addr; unsigned xw, xh, xstride;
      GX_GetXfb(&xfb_addr, &xw, &xh, &xstride);
      if (nwii::runtime::hw::g_vi_top_field_base != 0) {
        xfb_addr = nwii::runtime::hw::g_vi_top_field_base;
      }

      if (xfb_addr && xw && xh && xw <= 720 && xh <= 576 && std::getenv("NWII_XFB") && !headless) {
        xfb_px.resize((size_t)xw * xh * 4);
        auto clamp8 = [](float v) -> unsigned char { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); };
        for (unsigned y = 0; y < xh; ++y) {
          uint32_t row = (xfb_addr | 0x80000000u) + (uint32_t)y * xstride;
          for (unsigned x = 0; x < xw; x += 2) {
            uint8_t y0 = ctx->mmu.read8(row + x * 2 + 0);
            uint8_t cb = ctx->mmu.read8(row + x * 2 + 1);
            uint8_t y1 = ctx->mmu.read8(row + x * 2 + 2);
            uint8_t cr = ctx->mmu.read8(row + x * 2 + 3);
            float fcb = (float)cb - 128.0f, fcr = (float)cr - 128.0f;
            for (int k = 0; k < 2; ++k) {
              float yy = (float)(k ? y1 : y0);
              size_t p = ((size_t)y * xw + x + k) * 4;
              if (x + (unsigned)k >= xw) break;
              xfb_px[p + 0] = clamp8(yy + 1.371f * fcr);
              xfb_px[p + 1] = clamp8(yy - 0.336f * fcb - 0.698f * fcr);
              xfb_px[p + 2] = clamp8(yy + 1.732f * fcb);
              xfb_px[p + 3] = 255;
            }
          }
        }
        glBindTexture(GL_TEXTURE_2D, xfb_tex);
        if (xw != xfb_w || xh != xfb_h) {
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, xw, xh, 0, GL_RGBA, GL_UNSIGNED_BYTE, xfb_px.data());
          xfb_w = xw; xfb_h = xh;
        } else {
          glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, xw, xh, GL_RGBA, GL_UNSIGNED_BYTE, xfb_px.data());
        }
      }

      // Frame-paced: the last pass's wait_next_frame already drained the FIFO
      // and rendered the frame this pass presents; draining again here could
      // render a newer frame over it before it is ever shown.
      extern void ProcessGXFifo();
      if (!frame_paced) ProcessGXFifo();

      if (!headless) {
        // Live frame rate in the window title, for testing: swaps that showed
        // a new frame (g_stat_presented), not renders.
        static auto fps_t0 = std::chrono::steady_clock::now();
        static uint64_t fps_frames0 = 0;
        auto fps_now = std::chrono::steady_clock::now();
        double fps_dt = std::chrono::duration<double>(fps_now - fps_t0).count();
        if (fps_dt >= 1.0) {
          char title[64];
          std::snprintf(title, sizeof title, "NSMBW Recomp - %.1f FPS",
                        (g_stat_presented - fps_frames0) / fps_dt);
          if (window) SDL_SetWindowTitle(window, title);
          fps_t0 = fps_now;
          fps_frames0 = g_stat_presented;
        }
      }

      if (!headless) {
        static auto t0 = std::chrono::steady_clock::now();
        static int stat_n = 0;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >=
            5) {
          t0 = now;
          extern uint64_t g_stat_frames, g_stat_draws;
          extern uint64_t g_stat_parse_us, g_stat_render_us;
          extern uint64_t nwii_stat_hash0(), nwii_stat_shaders();
          std::vector<unsigned char> p(640 * 480 * 4);
          glBindFramebuffer(GL_READ_FRAMEBUFFER, efb_fbo);
          glReadPixels(0, 0, 640, 480, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
          glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
          size_t nonblack = 0;
          for (size_t i = 0; i < p.size(); i += 4)
            if (p[i] | p[i + 1] | p[i + 2])
              ++nonblack;
          std::cout << "[STAT] " << ++stat_n * 5 << "s frames=" << std::dec
                    << g_stat_frames << " presented=" << g_stat_presented
                    << " draws=" << g_stat_draws
                    << " parse_ms=" << g_stat_parse_us / 1000
                    << " render_ms=" << g_stat_render_us / 1000
                    << " efb_nonblack=" << nonblack << "/307200"
                    << " shaders=" << nwii_stat_shaders() << " hash0=0x"
                    << std::hex << nwii_stat_hash0();
          g_stat_parse_us = 0;
          g_stat_render_us = 0;

          std::cout << " E0..E7=";
          for (int i = 0xE0; i <= 0xE7; ++i)
            std::cout << nwii::runtime::gx::g_state.bp[i] << ",";

          
          std::cout << std::dec << " projType="
                    << nwii::runtime::gx::g_state.projType
                    << " projSet=" << (int)nwii::runtime::gx::g_state.projSet
                    << " proj=";
          for (int i = 0; i < 6; ++i)
            std::cout << nwii::runtime::gx::g_state.projection[i] << ",";

          
          uint32_t base = nwii::runtime::gx::g_state.texStages[0].base_addr;
          uint32_t toff = nwii::runtime::gx::g_state.texStages[0].tlut_offset;
          uint32_t tex_nz = 0, tlut_nz = 0;
          if (nwii::runtime::g_ctx_ptr) {
            for (uint32_t i = 0; i < 2048; ++i)
              if (nwii::runtime::g_ctx_ptr->mmu.read8(base + i))
                ++tex_nz;
          }
          for (uint32_t i = 0; i < 512; ++i) {
            uint32_t idx = toff + i;
            if (idx < sizeof(nwii::runtime::gx::g_state.tlutMem) &&
                nwii::runtime::gx::g_state.tlutMem[idx])
              ++tlut_nz;
          }
          std::cout << " tex0=" << std::hex << base << std::dec << "/"
                    << nwii::runtime::gx::g_state.texStages[0].width << "x"
                    << nwii::runtime::gx::g_state.texStages[0].height << "/f"
                    << (int)nwii::runtime::gx::g_state.texStages[0].format
                    << " texnz=" << tex_nz << "/2048 tlutnz=" << tlut_nz << "/512"
                    << std::endl;
        }
      }

      if (!headless) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        int draw_w = 0, draw_h = 0;
        HostDrawableSize(window, &draw_w, &draw_h);
        glViewport(0, 0, draw_w, draw_h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, efb_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        // Present what the game copied to the XFB; before its first copy,
        // fall back to the live EFB.
        extern void GX_GetPresentRect(int *, int *, int *, int *);
        extern bool GX_GetXfbFramebuffer(unsigned *, int *, int *);
        unsigned src_fbo = efb_fbo;
        int sx0, sy0, sx1, sy1, xw, xh;
        if (GX_GetXfbFramebuffer(&src_fbo, &xw, &xh)) {
          sx0 = 0; sy0 = 0; sx1 = xw; sy1 = xh;
        } else {
          src_fbo = efb_fbo;
          GX_GetPresentRect(&sx0, &sy0, &sx1, &sy1);
        }
        // The game renders 16:9 (SYSCONF IPL.AR = 1, an anamorphic 640-wide
        // XFB): keep that aspect in any window shape, with black bars.
        // NWII_STRETCH=1 fills the whole window instead.
        int dx0 = 0, dy0 = 0, dx1 = draw_w, dy1 = draw_h;
        static const bool stretch = NWII_ENV("NWII_STRETCH") != nullptr;
        if (!stretch && draw_w > 0 && draw_h > 0) {
          constexpr double kAspect = 16.0 / 9.0;
          if ((double)draw_w / draw_h > kAspect + 0.01) {
            const int w = (int)(draw_h * kAspect + 0.5);
            dx0 = (draw_w - w) / 2;
            dx1 = dx0 + w;
          } else if ((double)draw_w / draw_h < kAspect - 0.01) {
            const int h = (int)(draw_w / kAspect + 0.5);
            dy0 = (draw_h - h) / 2;
            dy1 = dy0 + h;
          }
        }
        // (Supersampled in steps when a scaled frame is over twice the
        // window size; a single bilinear blit otherwise, as before.)
        GX_PresentBlit(src_fbo, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1);
        {
          extern uint64_t g_stat_frames;
          static uint64_t cap_frames = 0;
          if (g_stat_frames != cap_frames) {  // a new frame is being shown
            nwii::runtime::gx::CaptureRecord(
                src_fbo, sx0, sy0, sx1, sy1,
                (uint32_t)(cap_frames ? g_stat_frames - cap_frames - 1 : 0));
            cap_frames = g_stat_frames;
          }
          static const long cap_at = NWII_ENV("NWII_CAPTURE_AT") ? std::atol(NWII_ENV("NWII_CAPTURE_AT")) : -1;
          static const long shot_at = NWII_ENV("NWII_SHOT_AT") ? std::atol(NWII_ENV("NWII_SHOT_AT")) : -1;
          static bool cap_done = false, shot_done = false;
          if (cap_at >= 0 && !cap_done && (long)g_stat_frames >= cap_at) { cap_done = true; capture_request = 1; }
          if (shot_at >= 0 && !shot_done && (long)g_stat_frames >= shot_at) { shot_done = true; capture_request = 2; }
          if (capture_request) {
            const std::string dir = nwii::runtime::gx::CaptureSave(
                capture_request, src_fbo, sx0, sy0, sx1, sy1);
            std::printf("[CAPTURE] %s -> %s\n",
                        capture_request == 1 ? "F12: last frames" : "F11: screenshot",
                        dir.c_str());
            std::fflush(stdout);
            capture_request = 0;
          }
        }
        // NWII_WINDUMP=<prefix>: every 600th present, the whole window as
        // <prefix>_<n>.ppm (checks the letterboxing and the final blit).
        if (const char *wd = NWII_ENV("NWII_WINDUMP")) {
          static int wn = 0;
          if ((++wn % 600) == 0 && draw_w > 0 && draw_h > 0) {
            std::vector<unsigned char> px((size_t)draw_w * draw_h * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadPixels(0, 0, draw_w, draw_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            char path[512];
            std::snprintf(path, sizeof(path), "%s_%06d.ppm", wd, wn);
            if (FILE *f = std::fopen(path, "wb")) {
              std::fprintf(f, "P6\n%d %d\n255\n", draw_w, draw_h);
              for (int y = draw_h - 1; y >= 0; --y)
                for (int x = 0; x < draw_w; ++x)
                  std::fwrite(&px[((size_t)y * draw_w + x) * 4], 1, 3, f);
              std::fclose(f);
            }
          }
        }

        if (const char *shot_pfx = std::getenv("NWII_SCREENSHOT")) {
          static int shot_frame = 0;
          if ((++shot_frame % 300) == 0) {
            // Dump exactly the presented source rectangle.
            const int sw = sx1 - sx0, sh = sy1 - sy0;
            std::vector<unsigned char> px((size_t)sw * sh * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
            glReadPixels(sx0, sy0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            std::vector<unsigned char> flip(px.size());
            for (int y = 0; y < sh; ++y)
              std::memcpy(&flip[(size_t)y * sw * 4],
                          &px[(size_t)(sh - 1 - y) * sw * 4], (size_t)sw * 4);
            SDL_Surface *s = SDL_CreateRGBSurfaceFrom(
                flip.data(), sw, sh, 32, sw * 4, 0x000000FF, 0x0000FF00,
                0x00FF0000, 0xFF000000);
            if (s) {
              std::string p = std::string(shot_pfx) + "_" +
                              std::to_string(shot_frame) + ".bmp";
              SDL_SaveBMP(s, p.c_str());
              SDL_FreeSurface(s);
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
          }
        }

        nwii::runtime::gx::DrawPerfOverlay();  // r10: fps_overlay (off by default)
        {
          // R14 R5: swap time and loop period for [PIPE] (gx.cpp)
          extern void GX_PipeNoteSwap(int64_t);
          const auto ts0 = std::chrono::steady_clock::now();
          HostSwap(window);  // r15dk: SDL_GL_SwapWindow o dkgl::SwapBuffers
          ::vtl_note(9, (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - ts0).count());
          GX_PipeNoteSwap(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - ts0).count());
        }
        count_present();

        // Offscreen (and any vsync-less) presentation would spin the host
        // loop thousands of times a second, and that loop also paces audio
        // and vblank emulation. Hold it to the 60 Hz a real display gives.
        // (Frame-paced presentation waits for the game's frames instead.)
        {
          static const bool pace = [] {
            const char *drv = std::getenv("SDL_VIDEODRIVER");
            return (drv && std::strcmp(drv, "offscreen") == 0) ||
                   std::getenv("NWII_HOST_PACE") != nullptr;
          }();
          if (pace && !frame_paced) {
            using clk = std::chrono::steady_clock;
            static clk::time_point next = clk::now();
            next += std::chrono::microseconds(16667);
            const auto now = clk::now();
            if (next < now - std::chrono::milliseconds(100))
              next = now;  // fell far behind: don't try to catch up
            std::this_thread::sleep_until(next);
          }
        }
      }

      if (audio_dev > 0) {
        // Feed the device everything the DSP played. After an underrun wait
        // for ~64 ms of audio so playback restarts cleanly; past ~250 ms
        // queued, drop frames so latency cannot grow without bound.
        static bool audio_priming = true;
        static int16_t abuf[4096 * 2];
        const Uint32 queued = SDL_GetQueuedAudioSize(audio_dev) / 4;
        if (queued == 0)
          audio_priming = true;
        if (!audio_priming || nwii::runtime::hw::dsp_audio_available() >= 2048) {
          audio_priming = false;
          const size_t n = nwii::runtime::hw::dsp_audio_pull(abuf, 4096);
          if (n && queued < 8000)
            SDL_QueueAudio(audio_dev, abuf, (Uint32)(n * 4));
        }
      }

      if (host_vi) { ctx->vblank_pending = true; ctx->poll_kick(); }  // R9: poll sees it
      if (frame_paced)
        wait_next_frame();
      else
        host_pacer.wait();
      perf_report();
    }

    // R11: stop the render worker while the GL context and the renderer
    // still exist (it frees frames whose emit-cache entries return slots).
    extern void GX_StopWorker();
    GX_StopWorker();

    if (!headless) {
      if (audio_dev > 0) SDL_CloseAudioDevice(audio_dev);
#ifdef __SWITCH__
      nwii::runtime::nx::audio_stop();
#endif
      if (gl_ctx) SDL_GL_DeleteContext(gl_ctx);
#ifdef __SWITCH__
      if (g_use_dk) dkgl::Shutdown();
#endif
      if (window) SDL_DestroyWindow(window);
      SDL_Quit();
    }
  }

  ctx->is_running = false;
  ctx->pc = 0; 
  nwii::runtime::s2::shutdown();
#ifdef __SWITCH__
  // A guest that never polls is_running again must not keep the console
  // from closing the app: wait up to 2 s, then end the process without
  // running destructors under the still-running CPU thread.
  for (int i = 0; i < 200 && !nwii::runtime::nx::cpu_thread_done(); ++i)
    svcSleepThread(10'000'000);
  const bool cpu_stopped = nwii::runtime::nx::cpu_thread_done();
  if (cpu_stopped) pthread_join(cpu_pthread, nullptr);
  nwii::runtime::nx::shutdown();
  if (!cpu_stopped) svcExitProcess();
#else
  if (cpu_thread.joinable())
    cpu_thread.join();
#endif

  nwii::runtime::shutdown();
  return 0;
}