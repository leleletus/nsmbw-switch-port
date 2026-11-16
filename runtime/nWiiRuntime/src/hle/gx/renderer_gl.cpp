#include "runtime/nwii_env.h"
#define NWII_NOINLINE_R5 __attribute__((noinline))
#include "runtime/cpu_context.h"
#include "runtime/gx/fifo_parser.h"
#include "runtime/gx/renderer.h"
#include "runtime/gx/tev_shader_gen.h"
#include "runtime/gx_state.h"
#include "runtime/texture_cache_gl.h"
#include "runtime/config.h"
#include "dk/dkgl.h"  // r15dk: GX_GLProc, GX_DkActive
#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <glad/glad.h>
#include <string>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <vector>
#ifdef __SWITCH__
#include <unistd.h>  // fsync
#if defined(__SSE2__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif
#endif

namespace nwii::runtime {
extern MMU *g_mmu;
extern CPUContext *g_ctx_ptr;
} // namespace nwii::runtime

void GX_GetClearColor(unsigned char rgba[4]);

namespace nwii::runtime::gx {

static bool gx_trace() {
  static bool t = NWII_ENV("NWII_GXTRACE") != nullptr;
  return t;
}

struct GLShader {
  GLuint id;         // program, or (separable mode, see sso::) the pipeline
  GLuint fprog = 0;  // program owning the fragment-stage uniforms
  GLint uTex[8];
  GLint uTexMtx[8];
  GLint uTevKColor;
  GLint uTevColor;
  GLint uProjMtx;
  GLint uMatColor, uAmbColor, uChanCtrl;
  GLint uLightCol, uLightPos, uLightDir, uLightCosAtt, uLightDistAtt;
  // Indirect texturing (only in shaders with active indirect stages).
  GLint uTexSize = -1, uCoordScale = -1, uIndMtx = -1, uIndScale = -1;
  // R11 GPU XF vertex shader (xf::): buffer textures and the batch's first
  // draw entry.
  GLint uXW = -1, uXQ = -1, uXDraw = -1;
  // Last values uploaded to this program: uniforms persist per program, so a
  // value equal to the last upload is skipped.
  bool have_last = false;
  float last_proj[16];
  float last_mat[8], last_amb[8];
  GLint last_cc[4];
  float last_lights[5][32];
  float last_tev[16], last_kon[16];
};
// GX state hash -> program (programs live in s_programs, keyed by source).
static std::unordered_map<uint64_t, GLShader *> s_shader_cache;

uint64_t g_stat_hash0 = 0;
uint64_t g_stat_shaders = 0;
} // namespace nwii::runtime::gx
// Render profiling ([RPROF] in gx.cpp): GL draw calls and GX draws rendered.
uint64_t g_prof_gl_draws = 0, g_prof_gx_draws = 0;
bool GX_OnRenderWorker();  // gx.cpp (R11)
bool GX_RenderWorkerOn();  // gx.cpp (R14 R5)
// Batches that a texture upload would have drawn with the wrong texture
// before the texture cache got its own unit (see DrawPrimitive).
uint64_t g_prof_tex_bind_saves = 0;
namespace nwii::runtime::gx {

// ---- Capture tool: per-frame record (F11/F12, see GX_Capture* below) -------
struct CapCopy {
  uint32_t dest;
  uint16_t w, h;
  uint8_t fmt, flags;  // 1 XFB, 2 half scale, 4 stale (frame-skip), 8 clear
};
struct CapStats {
  long xfb = 0;  // XFB copy number of the frame (as NWII_XFBDUMP names it)
  uint32_t gx_draws = 0, gl_draws = 0, uploads = 0, bind_saves = 0;
  uint32_t new_shaders = 0, stale_frames = 0, ncopies = 0, nprog = 0;
  float render_ms = 0;
  uint32_t prog[48];
  CapCopy copies[24];
};
static CapStats s_cap_cur, s_cap_last;
static inline void CapNoteProgram(uint32_t id) {
  for (uint32_t i = 0; i < s_cap_cur.nprog; ++i)
    if (s_cap_cur.prog[i] == id) return;
  if (s_cap_cur.nprog < 48) s_cap_cur.prog[s_cap_cur.nprog++] = id;
}

struct BatchVertex {
  float x, y, z;
  float r, g, b, a;
  float u, v;
  float nx, ny, nz;
  float u1, v1, u2, v2, u3, v3;  // texgen outputs 1-3 (0 is u, v)
};

static std::vector<BatchVertex> s_batch;

// ---- R9 render: NWII_RPHASE=1, Render() cost by phase -------------------------
// One timestamp per phase change (a few per GX draw); off by default (one
// static bool test). Printed every 120 renders as
// "[RPHASE] us/frame: total=.. state=.. tex=.. emit=.. upload=.. gldraw=.. regs=.. copy=.. frame=..".
namespace rph {
enum { kRegs, kState, kTex, kEmit, kUpload, kGlDraw, kCopy, kFrame, kN };
static const char *const kNames[kN] = {"regs", "state", "tex", "emit", "upload",
                                        "gldraw", "copy", "frame"};
// NWII_RPHASE=2: coarse, for slow clocks (citron reads the counter through a
// host call): regs/state/tex/emit are one phase ("regs" = all CPU work per
// command) and only GL calls, copies and frame start/end are timed apart.
// R14 R5: read once (InitFlags, first Render()) into a plain global: Level()
// and On() run several times per GX command; a function-local static costs a
// call plus an acquire load of its guard each time.
static int s_level = 0;
static void InitLevel() {
  const char *e = NWII_ENV("NWII_RPHASE");
  s_level = e ? (e[0] == '2' ? 2 : 1) : 0;
}
static inline int Level() { return s_level; }
static inline bool On() { return s_level != 0; }
static inline uint64_t Now() {
#if defined(__SWITCH__)
  uint64_t v;
  asm volatile("mrs %0, cntpct_el0" : "=r"(v));
  return v;  // 19.2 MHz
#else
  return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}
static inline double TicksToUs(uint64_t t) {
#if defined(__SWITCH__)
  return t / 19.2;
#else
  return t / 1000.0;
#endif
}
static uint64_t s_acc[kN], s_t = 0;
static int s_cur = -1;
// r39r2: EFB copy sub-phases (NWII_RPHASE=1 only), per frame in the report.
enum { cAcq, cSetup, cDraw, cBack, cDone, cClear, cXfb, kCN };
static const char *const kCNames[kCN] = {"acq", "setup", "draw", "back", "done", "clear", "xfb"};
static uint64_t s_cacc[kCN], s_ncopy = 0, s_nclear = 0, s_nxfb = 0;
static uint64_t s_gld_after = 0, s_n_after = 0, s_gld_n = 0;
static bool s_after = false;  // a copy/clear happened; the next GL draw is timed apart
static inline uint64_t T();
static inline void C(int k, uint64_t &t0) {  // add [t0, now) to sub-phase k
  if (!s_level) return;
  const uint64_t t = T();
  s_cacc[k] += t - t0;
  t0 = t;
}
static uint64_t s_frames = 0, s_draws = 0, s_dirty = 0, s_keychg = 0, s_verts = 0,
                s_idx = 0, s_bytes = 0, s_flushes = 0, s_hits = 0;
// Switch to phase p; returns the previous one.
static inline int Set(int p) {
  if (!On()) return -1;
  if (Level() == 2 && p >= 0 && p <= kEmit) {
    if (s_cur >= 0 && s_cur <= kEmit) return s_cur;  // still CPU work: no read
    p = kRegs;
  }
  const uint64_t t = Now();
  if (s_cur >= 0) s_acc[s_cur] += t - s_t;
  if (s_cur == kGlDraw) {  // r39r2
    ++s_gld_n;
    if (s_after) { s_gld_after += t - s_t; ++s_n_after; s_after = false; }
  }
  s_t = t;
  const int o = s_cur;
  s_cur = p;
  return o;
}
static void Report() {
  if (!On() || ++s_frames < 120) return;
  uint64_t tot = 0;
  for (auto a : s_acc) tot += a;
  const double f = (double)s_frames;
  printf("[RPHASE] us/frame: total=%.0f", TicksToUs(tot) / f);
  for (int i = 0; i < kN; ++i) printf(" %s=%.0f", kNames[i], TicksToUs(s_acc[i]) / f);
  printf(" | per frame: gx=%.0f dirty=%.0f keychg=%.0f flush=%.0f verts=%.0f idx=%.0f "
         "upKB=%.0f emit_hits=%.0f\n",
         s_draws / f, s_dirty / f, s_keychg / f, s_flushes / f, s_verts / f, s_idx / f,
         s_bytes / f / 1024.0, s_hits / f);
  // r39r2: copy sub-phases (us per frame) and the GL draw right after a copy
  printf("[RPHASE2] per frame: copies=%.1f clears=%.1f xfb=%.1f |", s_ncopy / f, s_nclear / f, s_nxfb / f);
  for (int i = 0; i < kCN; ++i) printf(" %s=%.0f", kCNames[i], TicksToUs(s_cacc[i]) / f);
  printf(" | gldraw n=%.1f, first after copy n=%.1f %.1f us each, others %.1f us each\n", s_gld_n / f,
         s_n_after / f, s_n_after ? TicksToUs(s_gld_after) / s_n_after : 0.0,
         s_gld_n > s_n_after ? (TicksToUs(s_acc[kGlDraw]) - TicksToUs(s_gld_after)) / (s_gld_n - s_n_after) : 0.0);
  for (auto &a : s_cacc) a = 0;
  s_ncopy = s_nclear = s_nxfb = s_gld_after = s_n_after = s_gld_n = 0;
  std::fflush(stdout);
  for (auto &a : s_acc) a = 0;
  s_frames = s_draws = s_dirty = s_keychg = s_verts = s_idx = s_bytes = s_flushes = s_hits = 0;
}
static inline uint64_t T() { return s_level ? Now() : 0; }
}  // namespace rph

// ---- R9 render: persistent vertex/index buffers (vbuf) ------------------------
// One vertex buffer mapped once for good (ARB_buffer_storage, persistent +
// coherent) holding two regions: an arena where the emit cache (ecache::)
// keeps the output of each repeating display-list draw, and a ring for the
// vertices of every other draw; plus an index ring. A draw whose output is
// cached writes no vertex at all: its triangles index its arena slot, which
// stays put frame after frame. Everything else is transformed once into a
// scratch array and copied straight into the mapped ring (no glBufferSubData
// and no orphaning, which reallocated the 24 MB buffer every few frames).
// Batches are unchanged (same draws, same triangles in the same order, same
// GL state); only where the vertex bits live differs, so the image is the
// same. GPU reuse is guarded by one fence per Render(): ring space and freed
// arena slots are reused only once the frames that used them are complete.
// NWII_R9VB=0 (or a GL without ARB_buffer_storage, or a debug mode that reads
// s_batch) keeps the r8 path.
static void FlushBatch();
// NWII_R9_FRAMECHECK (see RendererGL::Render): set while a frame is rendered
// a second time the r8 way (no vbuf, no emit cache) to compare the images.
static bool s_r8_pass = false;
static bool s_r2_full_inval = false;
// r39r2 NWII_R2_LAZYEFB=1: after an EFB copy to a texture the EFB goes back as
// the draw framebuffer only when something draws or clears with glClear (the
// ~20 animated-tile copies of a frame come one after the other: one switch
// back instead of 20). The read framebuffer is the EFB again at once.
static bool s_r2_efb_off = false;
static GLuint s_r2_efb_fbo_ref = 0;
static inline void R2EfbBack() {
  if (s_r2_efb_off) {
    glBindFramebuffer(GL_FRAMEBUFFER, s_r2_efb_fbo_ref);
    s_r2_efb_off = false;
  }
}  // r39r2 NWII_R2_COPYST: something outside the cache moved: GLInvalidate after all
// R11 GPU XF (NWII_GPU_XF, see namespace xf further down): vertex transform and
// texgen in the vertex shader. Declared here for the code above it.
namespace xf {
static bool s_on = false;  // in use (Init passed)
static inline bool Active() { return s_on && !s_r8_pass; }
static const std::string &VsText();
static void Disable(const char *why);
static void Init();
static void BeginFrame();
static void EndFrame();
static void FlushBatch();
static void Emit(const GXCommand &cmd, uint8_t prim);
static void ArenaFree(uint32_t off, uint8_t cls);
// What a draw's vertices need (xf::Analyze): ok = transformable on the GPU;
// the position matrix indices they use (pos[], first-use order) and those of
// each texgen's texture matrix (tex[t][]); with more than one of either, each
// vertex carries its ranks in a last word. Layout: stride words per vertex.
struct Info {
  bool ok = false, hp = false, hn = false, hc = false, ranks = false;
  uint8_t tmask = 0, stride = 0, npos = 0, ntex[4] = {};
  uint8_t pos[16], tex[4][16];
};
}  // namespace xf
namespace vbuf {
typedef void(APIENTRYP PFNBufferStorage)(GLenum, GLsizeiptr, const void *, GLbitfield);
constexpr GLbitfield kMapPersistent = 0x0040, kMapCoherent = 0x0080;
constexpr uint32_t kArenaVerts = 512u << 10;   // 36 MB of BatchVertex
constexpr uint32_t kStreamVerts = 384u << 10;  // 27 MB ring (~5 world-map frames)
constexpr uint64_t kIdxBytes = 8u << 20;       // index ring (~10 frames)
// Arena slot sizes: 4, 8, ... 256 vertices (64 classes), then 512 ... 8192.
constexpr int kClasses = 69;
static bool s_init = false, s_on = false;
static GLuint s_vao = 0, s_vbo = 0, s_ibo = 0;
static BatchVertex *s_v = nullptr;  // mapped vertex buffer: [arena | stream]
static uint8_t *s_i = nullptr;      // mapped index buffer
struct Frame {
  uint64_t id;
  GLsync fence;
  uint64_t v0, i0;  // first ring positions this frame wrote
};
static std::deque<Frame> *s_inflight = new std::deque<Frame>();
static uint64_t s_frame = 1, s_done = 0;  // frame being built / newest complete
static uint64_t s_vw = 0, s_iw = 0;       // ring write positions (virtual, monotonic)
static uint64_t s_frame_v0 = 0, s_frame_i0 = 0;
static uint32_t s_bump = 0;               // arena: never-used space starts here
struct Retired { uint64_t frame; uint32_t off; uint8_t cls; };
// Heap-allocated for good: emit-cache entries free their slots from static
// destructors at exit.
static std::vector<uint32_t> *s_free = new std::vector<uint32_t>[kClasses];
static std::deque<Retired> *s_retired = new std::deque<Retired>();
static uint64_t s_waits = 0, s_finishes = 0, s_arena_full = 0;

static void Init() {
  if (s_init) return;
  s_init = true;
  // With the GPU XF path the BatchVertex buffers are not used (the r8 pass of
  // NWII_R9_FRAMECHECK runs without them as always).
  if (xf::s_on) { printf("[GL] R9 vertex buffers: not needed (GPU XF)\n"); return; }
  const char *e = NWII_ENV("NWII_R9VB");
  if (e && e[0] == '0') { printf("[GL] R9 vertex buffers: off (NWII_R9VB=0)\n"); return; }
  // Debug paths that read or rebuild s_batch keep the r8 path.
  static const char *const kDebug[] = {"NWII_NORING", "NWII_NOINDEX", "NWII_SLOWEMIT",
                                       "NWII_FLATZ", "NWII_VTXTRACE", "NWII_DRAWSTATS",
                                       "NWII_DRAWTRACE", "NWII_PIXWATCH", "NWII_EMITCHECK",
                                       "NWII_NOTEXGEN4", "NWII_GXTRACE"};
  for (const char *d : kDebug)
    if (std::getenv(d)) { printf("[GL] R9 vertex buffers: off (%s)\n", d); return; }
  bool ext = false;
  GLint major = 0, minor = 0, next = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  glGetIntegerv(GL_NUM_EXTENSIONS, &next);
  for (GLint i = 0; i < next && !ext; i++) {
    const char *x = (const char *)glGetStringi(GL_EXTENSIONS, i);
    ext = x && !std::strcmp(x, "GL_ARB_buffer_storage");
  }
  glGetError();
  auto pStorage = (PFNBufferStorage)GX_GLProc("glBufferStorage");
  if (!(ext || major > 4 || (major == 4 && minor >= 4)) || !pStorage) {
    printf("[GL] R9 vertex buffers: unavailable (no ARB_buffer_storage), r8 path\n");
    return;
  }
  const GLbitfield fl = GL_MAP_WRITE_BIT | kMapPersistent | kMapCoherent;
  const size_t vbytes = (size_t)(kArenaVerts + kStreamVerts) * sizeof(BatchVertex);
  glGenVertexArrays(1, &s_vao);
  glGenBuffers(1, &s_vbo);
  glGenBuffers(1, &s_ibo);
  glBindVertexArray(s_vao);
  glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
  pStorage(GL_ARRAY_BUFFER, (GLsizeiptr)vbytes, nullptr, fl);
  s_v = (BatchVertex *)glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)vbytes, fl);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);  // part of the VAO
  pStorage(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)kIdxBytes, nullptr, fl);
  s_i = (uint8_t *)glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)kIdxBytes, fl);
  const GLenum err = glGetError();
  if (!s_v || !s_i || err != GL_NO_ERROR) {
    printf("[GL] R9 vertex buffers: mapping failed (err 0x%x), r8 path\n", err);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteBuffers(1, &s_vbo);
    glDeleteBuffers(1, &s_ibo);
    s_vao = s_vbo = s_ibo = 0;
    s_v = nullptr;
    s_i = nullptr;
    return;
  }
  // Same attribute layout as the r8 VAO (FlushBatch).
  const struct { int n; size_t off; } attr[7] = {
      {3, offsetof(BatchVertex, x)},  {4, offsetof(BatchVertex, r)},
      {2, offsetof(BatchVertex, u)},  {3, offsetof(BatchVertex, nx)},
      {2, offsetof(BatchVertex, u1)}, {2, offsetof(BatchVertex, u2)},
      {2, offsetof(BatchVertex, u3)}};
  for (int a = 0; a < 7; ++a) {
    glEnableVertexAttribArray(a);
    glVertexAttribPointer(a, attr[a].n, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                          (void *)attr[a].off);
  }
  glBindVertexArray(0);
  s_on = true;
  printf("[GL] R9 vertex buffers: on (persistent, %zu MB vertices + %llu MB indices)\n",
         vbytes >> 20, (unsigned long long)(kIdxBytes >> 20));
}

static inline bool Signaled(GLsync f) {
  const GLenum r = glClientWaitSync(f, 0, 0);
  return r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
}
static void PopOldest(bool wait) {
  Frame &f = s_inflight->front();
  if (wait && !Signaled(f.fence)) {
    ++s_waits;
    while (glClientWaitSync(f.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) ==
           GL_TIMEOUT_EXPIRED) {
    }
  }
  glDeleteSync(f.fence);
  s_done = f.id;
  s_inflight->pop_front();
}
// Frames complete without waiting (start of a Render()).
static void Poll() {
  while (!s_inflight->empty() && Signaled(s_inflight->front().fence)) PopOldest(false);
  while (s_inflight->size() > 6) PopOldest(true);
}
// The data a ring write at [lim - size, ...) replaces must be unused: wait for
// the frames that wrote below `lim` (v: vertex ring, else index ring). If the
// frame being built wrote there itself (a frame bigger than the ring), draw
// what is pending and wait for the GPU.
static void Reserve(uint64_t lim, bool v) {
  while (!s_inflight->empty() && (v ? s_inflight->front().v0 : s_inflight->front().i0) < lim)
    PopOldest(true);
  if ((v ? s_frame_v0 : s_frame_i0) < lim) {
    ++s_finishes;
    if (v) FlushBatch();
    glFinish();
    s_frame_v0 = s_vw;
    s_frame_i0 = s_iw;
  }
}
// n consecutive ring vertices; returns the absolute index of the first.
static inline uint32_t StreamAlloc(uint32_t n) {
  uint64_t w = s_vw;
  const uint32_t phys = (uint32_t)(w % kStreamVerts);
  if (phys + n > kStreamVerts) w += kStreamVerts - phys;  // never wrap inside
  if (w + n > kStreamVerts) Reserve(w + n - kStreamVerts, true);
  s_vw = w + n;
  return kArenaVerts + (uint32_t)(w % kStreamVerts);
}
// Byte offset in the index buffer for `bytes` (4-aligned).
static inline uint64_t IndexAlloc(uint64_t bytes) {
  uint64_t w = s_iw;
  const uint64_t phys = w % kIdxBytes;
  if (phys + bytes > kIdxBytes) w += kIdxBytes - phys;
  if (w + bytes > kIdxBytes) Reserve(w + bytes - kIdxBytes, false);
  s_iw = w + bytes;
  return w % kIdxBytes;
}
static inline int ClassOf(uint32_t n) {
  if (n <= 256) return (int)((n + 3) / 4) - 1;
  int c = 64;
  for (uint32_t sz = 512; sz < n; sz <<= 1) ++c;
  return c;
}
static inline uint32_t ClassSize(int c) {
  return c < 64 ? (uint32_t)(c + 1) * 4 : 512u << (c - 64);
}
static void Recycle() {
  while (!s_retired->empty() && s_retired->front().frame <= s_done) {
    s_free[s_retired->front().cls].push_back(s_retired->front().off);
    s_retired->pop_front();
  }
}
// An arena slot for n vertices (absolute vertex index), or -1 (full / too big).
static inline int64_t ArenaAlloc(uint32_t n, uint8_t &cls) {
  if (n == 0 || n > ClassSize(kClasses - 1)) return -1;
  cls = (uint8_t)ClassOf(n);
  auto &fl = s_free[cls];
  if (!fl.empty()) {
    const uint32_t off = fl.back();
    fl.pop_back();
    return off;
  }
  const uint32_t sz = ClassSize(cls);
  if (s_bump + sz > kArenaVerts) { ++s_arena_full; return -1; }
  const uint32_t off = s_bump;
  s_bump += sz;
  return off;
}
// The slot may still be read by draws of the frame being built (or earlier).
static inline void ArenaFree(uint32_t off, uint8_t cls) {
  s_retired->push_back({s_frame, off, cls});
}
// R11 render worker: an emit-cache entry dies with the last reference to its
// decoded vertices, which can be dropped on the render worker (gx.cpp) when it
// frees a rendered frame or replaces a stale decode. Its slot is then handed to
// the render thread, which retires it at its next Render() (TakeDeferred): a
// frame later than the slot's last use, so never too early.
// r13 (R11 worker + R11 GPU XF): the same holds for the entry's GPU XF arena
// slot (x_slot, xf::ArenaFree): `gpu_xf` picks the arena. TakeDeferred runs at
// every Render() whatever the vertex path.
struct Deferred { uint32_t off; uint8_t cls; bool gpu_xf; };
static std::mutex *s_defer_m = new std::mutex;
static std::vector<Deferred> *s_defer = new std::vector<Deferred>;
static inline void ArenaFreeAny(uint32_t off, uint8_t cls, bool gpu_xf = false) {
  if (!::GX_OnRenderWorker()) {
    if (gpu_xf) xf::ArenaFree(off, cls);
    else ArenaFree(off, cls);
    return;
  }
  std::lock_guard<std::mutex> lk(*s_defer_m);
  s_defer->push_back({off, cls, gpu_xf});
}
static void TakeDeferred() {
  std::lock_guard<std::mutex> lk(*s_defer_m);
  for (const auto &d : *s_defer) {
    if (d.gpu_xf) xf::ArenaFree(d.off, d.cls);
    else ArenaFree(d.off, d.cls);
  }
  s_defer->clear();
}
// End of a Render(): fence everything drawn so far.
static void EndFrame() {
  s_inflight->push_back({s_frame, glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), s_frame_v0,
                         s_frame_i0});
  ++s_frame;
  s_frame_v0 = s_vw;
  s_frame_i0 = s_iw;
}
}  // namespace vbuf
// Indexed batches: every GX vertex is transformed once; primitives become
// triangle indices (a strip or fan used to re-transform each vertex up to
// three times).
static std::vector<uint32_t> s_batch_idx;
static GLuint s_ibo = 0;
static float s_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static GLuint s_vao = 0, s_vbo = 0;
static GLShader *s_active = nullptr;

// ---- GL state cache --------------------------------------------------------
// A frame is ~1900 GX draws whose GL state mostly repeats; redundant GL calls
// are filtered here. GLInvalidate() forgets everything: at frame start (the
// frontend touches GL between frames) and after any code that changes GL
// state behind it (EFB copies and clears, texture uploads).
namespace {
enum { kEnBlend, kEnDepth, kEnCull, kEnScissor, kEnLogic, kEnCount };
struct GLStateCache {
  int en[kEnCount];
  GLuint program, vao, pipeline;
  GLenum eq_rgb, eq_a, bs_rgb, bd_rgb, bs_a, bd_a;
  GLenum depth_func;
  int depth_mask, color_mask;
  GLenum logic_op, cull_face, front_face;
  GLint vp[4], sc[4];
  GLuint tex[8], sampler[8];
  int unit;
};
GLStateCache g_gl;
} // namespace

static void GLInvalidate() {
  std::memset(&g_gl, 0xFF, sizeof(g_gl));
}
static inline void CEnable(int slot, GLenum cap, bool on) {
  if (g_gl.en[slot] != (int)on) {
    if (on) glEnable(cap); else glDisable(cap);
    g_gl.en[slot] = on;
  }
}
static inline void CUseProgram(GLuint p) {
  if (g_gl.program != p) { glUseProgram(p); g_gl.program = p; }
}
static inline void CBindVertexArray(GLuint v) {
  if (g_gl.vao != v) { glBindVertexArray(v); g_gl.vao = v; }
}
static inline void CBlendEquation(GLenum rgb, GLenum a) {
  if (g_gl.eq_rgb != rgb || g_gl.eq_a != a) {
    glBlendEquationSeparate(rgb, a);
    g_gl.eq_rgb = rgb; g_gl.eq_a = a;
  }
}
static inline void CBlendFunc(GLenum s, GLenum d, GLenum sa, GLenum da) {
  if (g_gl.bs_rgb != s || g_gl.bd_rgb != d || g_gl.bs_a != sa || g_gl.bd_a != da) {
    glBlendFuncSeparate(s, d, sa, da);
    g_gl.bs_rgb = s; g_gl.bd_rgb = d; g_gl.bs_a = sa; g_gl.bd_a = da;
  }
}
static inline void CDepthFunc(GLenum f) {
  if (g_gl.depth_func != f) { glDepthFunc(f); g_gl.depth_func = f; }
}
static inline void CDepthMask(bool m) {
  if (g_gl.depth_mask != (int)m) { glDepthMask(m ? GL_TRUE : GL_FALSE); g_gl.depth_mask = m; }
}
static inline void CColorMask(bool c, bool a) {
  const int m = (c ? 1 : 0) | (a ? 2 : 0);
  if (g_gl.color_mask != m) { glColorMask(c, c, c, a); g_gl.color_mask = m; }
}
static inline void CLogicOp(GLenum op) {
  if (g_gl.logic_op != op) { glLogicOp(op); g_gl.logic_op = op; }
}
static inline void CCull(GLenum face, GLenum front) {
  if (g_gl.cull_face != face) { glCullFace(face); g_gl.cull_face = face; }
  if (g_gl.front_face != front) { glFrontFace(front); g_gl.front_face = front; }
}
static inline void CViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (g_gl.vp[0] != x || g_gl.vp[1] != y || g_gl.vp[2] != w || g_gl.vp[3] != h) {
    glViewport(x, y, w, h);
    g_gl.vp[0] = x; g_gl.vp[1] = y; g_gl.vp[2] = w; g_gl.vp[3] = h;
  }
}
static double s_depth_range[2] = {-1.0, -1.0};
static inline void CDepthRange(double n, double f) {
  if (s_depth_range[0] != n || s_depth_range[1] != f) {
    glDepthRange(n, f);
    s_depth_range[0] = n; s_depth_range[1] = f;
  }
}
static inline void CScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (g_gl.sc[0] != x || g_gl.sc[1] != y || g_gl.sc[2] != w || g_gl.sc[3] != h) {
    glScissor(x, y, w, h);
    g_gl.sc[0] = x; g_gl.sc[1] = y; g_gl.sc[2] = w; g_gl.sc[3] = h;
  }
}
static inline void CActiveUnit(int u) {
  if (g_gl.unit != u) { glActiveTexture(GL_TEXTURE0 + u); g_gl.unit = u; }
}
static inline void CBindTexture(int u, GLuint t) {
  if (g_gl.tex[u] != t) { CActiveUnit(u); glBindTexture(GL_TEXTURE_2D, t); g_gl.tex[u] = t; }
}
static inline void CBindSampler(int u, GLuint smp) {
  if (g_gl.sampler[u] != smp) { glBindSampler(u, smp); g_gl.sampler[u] = smp; }
}
// A texture upload/bind outside the cache leaves the active unit's binding
// unknown.
static inline void GLForgetActiveBinding() {
  if (g_gl.unit >= 0 && g_gl.unit < 8) g_gl.tex[g_gl.unit] = ~0u;
  else for (auto &t : g_gl.tex) t = ~0u;
}

// ---- TEV program cache (R4) -------------------------------------------------
// Programs are keyed by their generated source: GX states that differ only in
// things the shader does not read (Z/blend/cull mode, registers of unused TEV
// stages, formats of unbound maps, the VCD...) used to hash apart and each
// compiled its own copy of the same program (3400 in a 1-1 session). The
// state hash now maps to the program of its source; the vertex shader, the
// same text for every TEV program, is compiled once and attached to all.
// Linked programs are also kept on disk (glGetProgramBinary, keyed by source
// hash + driver), so a shader compiled once is a ~0.2 ms load next session.
// NWII_SHADERCACHE=<file> sets the cache file (default: shadercache.bin next to
// the NAND directory), NWII_SHADERCACHE=0 disables it.
} // namespace nwii::runtime::gx
extern uint64_t g_prof_tex_uploads;  // texture_cache_gl.cpp
uint64_t g_prof_shader_compiles = 0, g_prof_shader_loads = 0,
         g_prof_shader_us = 0, g_prof_shader_skips = 0;
namespace nwii::runtime::gx {
static std::unordered_map<uint64_t, GLShader> s_programs;  // source hash

static uint64_t Fnv64(const std::string &s, uint64_t h = 14695981039346656037ull) {
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

// Uniform locations of a TEV program (VS and FS names; -1 when absent).
static void LocateUniforms(GLShader &sh, GLuint id) {
  for (int i = 0; i < 8; i++) {
    sh.uTex[i] = glGetUniformLocation(id, ("uTex" + std::to_string(i)).c_str());
    sh.uTexMtx[i] = glGetUniformLocation(id, ("uTexMtx[" + std::to_string(i) + "]").c_str());
  }
  sh.uTevKColor = glGetUniformLocation(id, "uTevKColor[0]");
  if (sh.uTevKColor < 0) sh.uTevKColor = glGetUniformLocation(id, "uTevKColor");
  sh.uTevColor = glGetUniformLocation(id, "uTevColor[0]");
  if (sh.uTevColor < 0) sh.uTevColor = glGetUniformLocation(id, "uTevColor");
  sh.uProjMtx = glGetUniformLocation(id, "mvp");
  sh.uMatColor = glGetUniformLocation(id, "uMatColor[0]");
  sh.uAmbColor = glGetUniformLocation(id, "uAmbColor[0]");
  sh.uChanCtrl = glGetUniformLocation(id, "uChanCtrl[0]");
  sh.uLightCol = glGetUniformLocation(id, "uLightCol[0]");
  sh.uLightPos = glGetUniformLocation(id, "uLightPos[0]");
  sh.uLightDir = glGetUniformLocation(id, "uLightDir[0]");
  sh.uLightCosAtt = glGetUniformLocation(id, "uLightCosAtt[0]");
  sh.uLightDistAtt = glGetUniformLocation(id, "uLightDistAtt[0]");
  sh.uTexSize = glGetUniformLocation(id, "uTexSize[0]");
  sh.uCoordScale = glGetUniformLocation(id, "uCoordScale[0]");
  sh.uIndMtx = glGetUniformLocation(id, "uIndMtx[0]");
  sh.uIndScale = glGetUniformLocation(id, "uIndScale[0]");
  sh.uXW = glGetUniformLocation(id, "uXW");
  sh.uXQ = glGetUniformLocation(id, "uXQ");
  sh.uXDraw = glGetUniformLocation(id, "uXDraw");
  // r39r2 NWII_R2_XDUBO: the draw-base block on binding point 1
  const GLuint r2b = glGetUniformBlockIndex(id, "R2XDraw");
  if (r2b != GL_INVALID_INDEX) glUniformBlockBinding(id, r2b, 1);
}

// ---- Separable programs (R8 shaders) ----------------------------------------
// On the Switch (Mesa 20.1 nouveau) linking a TEV program costs ~1 s in citron
// and ~2.2 s on the console, and almost all of it is the shared vertex shader
// (its 8-light loops, unrolled), relinked into every program: a fragment
// shader alone links in ~10 ms. The driver has no program binaries
// (GL_NUM_PROGRAM_BINARY_FORMATS = 0) and no disk shader cache, so the cost
// cannot be shipped precompiled. With ARB_separate_shader_objects the vertex
// shader is one separable program built once at boot, and each TEV program is
// a separable fragment program plus a pipeline object. Same GLSL, same
// arithmetic: the image is identical. The only source changes are explicit
// varying locations (so both stages agree without being linked together) and
// the gl_PerVertex redeclaration the extension asks for.
// NWII_SHADER_SSO=1/0 forces it on/off (default: on on Switch, off on PC).
namespace sso {
typedef void(APIENTRYP PFNGenPP)(GLsizei, GLuint *);
typedef void(APIENTRYP PFNDelPP)(GLsizei, const GLuint *);
typedef void(APIENTRYP PFNBindPP)(GLuint);
typedef void(APIENTRYP PFNUseStages)(GLuint, GLbitfield, GLuint);
typedef void(APIENTRYP PFNPParami)(GLuint, GLenum, GLint);
typedef void(APIENTRYP PFNPU1i)(GLuint, GLint, GLint);
typedef void(APIENTRYP PFNPUiv)(GLuint, GLint, GLsizei, const GLint *);
typedef void(APIENTRYP PFNPUfv)(GLuint, GLint, GLsizei, const GLfloat *);
typedef void(APIENTRYP PFNPUMfv)(GLuint, GLint, GLsizei, GLboolean, const GLfloat *);
constexpr GLenum kSeparable = 0x8258;
constexpr GLbitfield kVsBit = 0x1, kFsBit = 0x2;
static PFNGenPP pGen = nullptr;
static PFNDelPP pDel = nullptr;
static PFNBindPP pBind = nullptr;
static PFNUseStages pStages = nullptr;
static PFNPParami pParam = nullptr;
static PFNPU1i pU1i = nullptr;
static PFNPUiv pU1iv = nullptr, pU4iv = nullptr;
static PFNPUfv pU2fv = nullptr, pU4fv = nullptr;
static PFNPUMfv pUM4fv = nullptr;
static bool s_init = false, s_on = false;
static GLuint s_vprog = 0;   // the separable vertex program (one per process)
static GLShader s_vsh = {};  // its uniform locations + last uploaded values
// R11: the GPU XF vertex program (xf::VsText) and its uniforms. Each one is
// built when first needed (the classic one only for NWII_GPU_XF=0, or for
// the r8 pass of NWII_R9_FRAMECHECK).
static GLuint s_vprog_x = 0;
static GLShader s_vsh_x = {};
static bool s_tried = false, s_tried_x = false;

// Source of one stage made separable (see above). The varyings are the
// generator's fixed list, in its order.
static std::string Patch(const std::string &src, bool vs) {
  std::string s = src;
  const size_t nl = s.find('\n');
  if (nl == std::string::npos) return s;
  std::string ins = "#extension GL_ARB_separate_shader_objects : require\n";
  if (vs) ins += "out gl_PerVertex { vec4 gl_Position; };\n";
  // After the leading preprocessor lines (#version, #extension, #define): an
  // #extension must come before any declaration.
  size_t at0 = nl + 1;
  while (at0 < s.size() && s[at0] == '#') {
    const size_t e = s.find('\n', at0);
    if (e == std::string::npos) break;
    at0 = e + 1;
  }
  s.insert(at0, ins);
  static const char *const kVar[10] = {"vec4 vColor0", "vec4 vColor1", "vec2 vTex0",
                                       "vec2 vTex1",   "vec2 vTex2",   "vec2 vTex3",
                                       "vec2 vTex4",   "vec2 vTex5",   "vec2 vTex6",
                                       "vec2 vTex7"};
  for (int i = 0; i < 10; ++i) {
    const std::string decl = std::string("\n") + (vs ? "out " : "in ") + kVar[i] + ";\n";
    const size_t at = s.find(decl);
    if (at == std::string::npos) continue;
    s.insert(at + 1, "layout(location = " + std::to_string(i) + ") ");
  }
  return s;
}

// A separable one-stage program, or 0 (the log of the first failures printed).
static GLuint Build(GLenum type, const std::string &src) {
  GLuint sh = glCreateShader(type);
  const char *c = src.c_str();
  glShaderSource(sh, 1, &c, nullptr);
  glCompileShader(sh);
  GLuint p = glCreateProgram();
  pParam(p, kSeparable, GL_TRUE);
  glAttachShader(p, sh);
  glLinkProgram(p);
  GLint linked = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &linked);
  static int s_failures = 0;
  if (!linked && s_failures++ < 8) {
    char log[1024] = {0};
    glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
    if (!log[0]) glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
    printf("[GL] separable %s program failed to build: %s\n",
           type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
  }
  glDetachShader(p, sh);
  glDeleteShader(sh);
  if (!linked) { glDeleteProgram(p); return 0; }
  return p;
}

// The separable vertex program of the classic (xv false) or GPU XF path,
// built on first use (0 if it fails).
static GLuint VProg(bool xv) {
  GLuint &p = xv ? s_vprog_x : s_vprog;
  bool &tried = xv ? s_tried_x : s_tried;
  if (p || tried) return p;
  tried = true;
  const auto t0 = std::chrono::steady_clock::now();
  p = Build(GL_VERTEX_SHADER,
            Patch(xv ? xf::VsText() : GenerateTEVShader(g_state, 0).vertex_source, true));
  if (!p) return 0;
  GLShader &sh = xv ? s_vsh_x : s_vsh;
  LocateUniforms(sh, p);
  sh.id = sh.fprog = p;
  if (sh.uXW >= 0) pU1i(p, sh.uXW, 12);
  if (sh.uXQ >= 0) pU1i(p, sh.uXQ, 13);
  printf("[SHADERCACHE] separable %s vertex program built in %lld ms\n",
         xv ? "GPU XF" : "classic",
         (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0).count());
  return p;
}
static GLShader &Vsh(bool xv) { return xv ? s_vsh_x : s_vsh; }

static void Init() {
  if (s_init) return;
  s_init = true;
  const char *e = NWII_ENV("NWII_SHADER_SSO");
#ifdef __SWITCH__
  const bool want = !(e && !std::strcmp(e, "0"));
#else
  const bool want = e && std::strcmp(e, "0") != 0;
#endif
  if (!want) return;
  bool ext = false;
  GLint major = 0, minor = 0, next = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  glGetIntegerv(GL_NUM_EXTENSIONS, &next);
  for (GLint i = 0; i < next && !ext; i++) {
    const char *x = (const char *)glGetStringi(GL_EXTENSIONS, i);
    ext = x && !std::strcmp(x, "GL_ARB_separate_shader_objects");
  }
  glGetError();
  auto get = [](const char *n) { return GX_GLProc(n); };
  pGen = (PFNGenPP)get("glGenProgramPipelines");
  pDel = (PFNDelPP)get("glDeleteProgramPipelines");
  pBind = (PFNBindPP)get("glBindProgramPipeline");
  pStages = (PFNUseStages)get("glUseProgramStages");
  pParam = (PFNPParami)get("glProgramParameteri");
  pU1i = (PFNPU1i)get("glProgramUniform1i");
  pU1iv = (PFNPUiv)get("glProgramUniform1iv");
  pU4iv = (PFNPUiv)get("glProgramUniform4iv");
  pU2fv = (PFNPUfv)get("glProgramUniform2fv");
  pU4fv = (PFNPUfv)get("glProgramUniform4fv");
  pUM4fv = (PFNPUMfv)get("glProgramUniformMatrix4fv");
  const bool fns = pGen && pDel && pBind && pStages && pParam && pU1i && pU1iv && pU4iv &&
                   pU2fv && pU4fv && pUM4fv;
  const char *why = nullptr;
  if (!(ext || major > 4 || (major == 4 && minor >= 1))) why = "no ARB_separate_shader_objects";
  else if (!fns) why = "entry points missing";
  if (!why) {
    // The vertex shader text is the same for every GX state: one program for
    // the classic path, one for the GPU XF path. Only the one in use is built
    // here (the Switch takes ~1-2 s per vertex program).
    s_on = true;
    if (xf::s_on && !VProg(true)) xf::Disable("separable vertex program failed to build");
    if (xf::s_on || VProg(false)) {
      printf("[SHADERCACHE] separable programs: on\n");
      return;
    }
    s_on = false;
    why = "vertex program failed";
  }
  printf("[SHADERCACHE] separable programs: unavailable (%s), linked programs\n", why);
}

// Separable fragment program of one TEV source, or 0.
static GLuint BuildFs(const std::string &fs_src) {
  return Build(GL_FRAGMENT_SHADER, Patch(fs_src, false));
}
} // namespace sso

static inline void CBindPipeline(GLuint pp) {
  if (g_gl.pipeline != pp) { sso::pBind(pp); g_gl.pipeline = pp; }
}
// Uniform uploads to `prog`: glUniform* on the bound program (linked mode) or
// glProgramUniform* (separable mode: each stage has its own program).
static inline void U1iv(GLuint p, GLint l, GLsizei n, const GLint *v) {
  if (sso::s_on) sso::pU1iv(p, l, n, v); else glUniform1iv(l, n, v);
}
static inline void U4iv(GLuint p, GLint l, GLsizei n, const GLint *v) {
  if (sso::s_on) sso::pU4iv(p, l, n, v); else glUniform4iv(l, n, v);
}
static inline void U2fv(GLuint p, GLint l, GLsizei n, const GLfloat *v) {
  if (sso::s_on) sso::pU2fv(p, l, n, v); else glUniform2fv(l, n, v);
}
static inline void U4fv(GLuint p, GLint l, GLsizei n, const GLfloat *v) {
  if (sso::s_on) sso::pU4fv(p, l, n, v); else glUniform4fv(l, n, v);
}
static inline void UM4fv(GLuint p, GLint l, const GLfloat *v) {
  if (sso::s_on) sso::pUM4fv(p, l, 1, GL_FALSE, v); else glUniformMatrix4fv(l, 1, GL_FALSE, v);
}

namespace shcache {
// File layout: "NSC2", u64 driver id, then entries
//   u64 key, u32 binary format, u32 binary length, u32 source length, binary, FS source.
// Sources are kept even when the driver changes (binaries are then dropped),
// so every program seen once is rebuilt at startup (Prewarm) instead of in
// the middle of a level.
typedef void(APIENTRYP PFNGetProgramBinary)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
typedef void(APIENTRYP PFNProgramBinary)(GLuint, GLenum, const void *, GLsizei);
typedef void(APIENTRYP PFNProgramParameteri)(GLuint, GLenum, GLint);
constexpr GLenum kRetrievableHint = 0x8257, kBinaryLength = 0x8741,
                 kNumBinaryFormats = 0x87FE;
static PFNGetProgramBinary pGet = nullptr;
static PFNProgramBinary pLoad = nullptr;
static PFNProgramParameteri pParam = nullptr;
static bool s_init = false, s_on = false, s_bin = false;
static FILE *s_out = nullptr;
struct Entry { GLenum fmt = 0; std::vector<uint8_t> bin; std::string fs; };
static std::unordered_map<uint64_t, Entry> s_entries;
static std::vector<uint64_t> s_order;  // file order, for the prewarm
static const uint32_t kMagic = 0x3243534E;  // "NSC2"

static void Write(uint64_t key, GLenum fmt, const void *bin, uint32_t blen,
                  const std::string &fs, bool sync = true) {
  if (!s_out) return;
  const uint32_t f32 = fmt, slen = (uint32_t)fs.size();
  // One record, one fwrite: a process killed mid-write (HOME on Switch)
  // leaves at most one torn record at the end, which Init() repairs.
  std::vector<uint8_t> rec(20 + (size_t)blen + slen);
  std::memcpy(rec.data(), &key, 8);
  std::memcpy(rec.data() + 8, &f32, 4);
  std::memcpy(rec.data() + 12, &blen, 4);
  std::memcpy(rec.data() + 16, &slen, 4);
  if (blen) std::memcpy(rec.data() + 20, bin, blen);
  if (slen) std::memcpy(rec.data() + 20 + blen, fs.data(), slen);
  std::fwrite(rec.data(), 1, rec.size(), s_out);
  if (!sync) return;
  std::fflush(s_out);
#ifdef __SWITCH__
  fsync(fileno(s_out));  // libnx: commit to the SD card (fsFileFlush)
#endif
}

// A stored fragment shader source looks like one (guards against a torn or
// corrupted record whose length field swallowed the next record).
static bool ValidSource(const std::string &fs) {
  if (fs.compare(0, 9, "#version ") != 0) return false;
  for (unsigned char c : fs)
    if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c > 126) return false;
  return true;
}

static void Init() {
  if (s_init) return;
  s_init = true;
  const char *env = NWII_ENV("NWII_SHADERCACHE");
  sso::Init();
  if (env && !std::strcmp(env, "0")) return;
  pGet = (PFNGetProgramBinary)GX_GLProc("glGetProgramBinary");
  pLoad = (PFNProgramBinary)GX_GLProc("glProgramBinary");
  pParam = (PFNProgramParameteri)GX_GLProc("glProgramParameteri");
  GLint nfmt = 0;
  glGetIntegerv(kNumBinaryFormats, &nfmt);
  glGetError();
  // (Separable mode keeps sources only: a binary is one linked program.)
  s_bin = pGet && pLoad && pParam && nfmt > 0 && !NWII_ENV("NWII_SHADERCACHE_NOBIN") &&
          !sso::s_on;
  std::string path;
  if (env) {
    path = env;
  } else {
    std::string nand = nwii::runtime::Config::get().nand_dir;
    while (nand.size() > 1 && nand.back() == '/') nand.pop_back();
    const size_t sl = nand.find_last_of('/');
    path = (sl == std::string::npos ? std::string(".") : nand.substr(0, sl)) +
           "/shadercache.bin";
  }
  // Driver identity: binaries are only valid for the same GL implementation.
  std::string drv;
  for (GLenum e : {GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION})
    if (const GLubyte *s = glGetString(e)) drv += (const char *)s, drv += '|';
  if (sso::s_on) drv += "sso|";
  const uint64_t drv_id = Fnv64(drv);
  bool same_driver = false, readable = false, clean = true;
  if (FILE *f = std::fopen(path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    const long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    uint32_t magic = 0;
    uint64_t id = 0;
    if (std::fread(&magic, 4, 1, f) == 1 && std::fread(&id, 8, 1, f) == 1 &&
        magic == kMagic) {
      readable = true;
      same_driver = id == drv_id;
      long good_end = std::ftell(f);  // end of the last complete record
      for (;;) {
        uint64_t key; uint32_t fmt, blen, slen;
        if (std::fread(&key, 8, 1, f) != 1 || std::fread(&fmt, 4, 1, f) != 1 ||
            std::fread(&blen, 4, 1, f) != 1 || std::fread(&slen, 4, 1, f) != 1 ||
            blen > (64u << 20) || slen > (1u << 20))
          break;
        std::vector<uint8_t> bin(blen);
        std::string fs(slen, '\0');
        if ((blen && std::fread(bin.data(), 1, blen, f) != blen) ||
            (slen && std::fread(&fs[0], 1, slen, f) != slen))
          break;  // torn tail
        if (!ValidSource(fs)) { clean = false; continue; }  // corrupted record
        good_end = std::ftell(f);
        auto ins = s_entries.emplace(key, Entry{});
        if (ins.second) s_order.push_back(key);
        Entry &e = ins.first->second;
        if (blen && same_driver) { e.fmt = fmt; e.bin = std::move(bin); }
        if (slen) e.fs = std::move(fs);
      }
      // Bytes after the last complete record (a torn write): appending after
      // them would make the next record unreadable, so rewrite the file.
      if (good_end != fsize) clean = false;
    }
    std::fclose(f);
  }
  if (readable && same_driver && clean) {
    s_out = std::fopen(path.c_str(), "ab");
  } else {
    // New file, another driver (keep the sources, drop the binaries), or a
    // damaged file (keep every complete record).
    s_out = std::fopen(path.c_str(), "wb");
    if (s_out) {
      std::fwrite(&kMagic, 4, 1, s_out);
      std::fwrite(&drv_id, 8, 1, s_out);
      for (uint64_t k : s_order) {
        const Entry &e = s_entries[k];
        if (e.fs.empty()) continue;
        // (One commit for the whole rewrite, not one per record: the shipped
        // cache is ~200 records and the first boot on a console rewrites it.)
        Write(k, e.bin.empty() ? 0 : e.fmt, e.bin.data(), (uint32_t)e.bin.size(), e.fs, false);
      }
      std::fflush(s_out);
#ifdef __SWITCH__
      fsync(fileno(s_out));
#endif
    }
  }
  s_on = s_out != nullptr;
  printf("[SHADERCACHE] %s: %zu programs (%s%s)%s%s\n", path.c_str(), s_entries.size(),
         same_driver ? "same driver" : readable ? "driver changed: rebuilding" : "new",
         readable && !clean ? ", damaged file repaired" : "",
         s_bin ? "" : " (no program binaries on this driver: sources only)",
         s_on ? "" : " (not writable)");
}

// A cached binary of `key` loaded into a new program, or 0.
static GLuint LoadBinary(uint64_t key) {
  if (!s_bin) return 0;
  auto it = s_entries.find(key);
  if (it == s_entries.end() || it->second.bin.empty()) return 0;
  GLuint p = glCreateProgram();
  pLoad(p, it->second.fmt, it->second.bin.data(), (GLsizei)it->second.bin.size());
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  it->second.bin.clear();
  it->second.bin.shrink_to_fit();  // in memory only until loaded once
  if (!ok) { glDeleteProgram(p); glGetError(); return 0; }
  return p;
}

static void Store(uint64_t key, GLuint p, const std::string &fs) {
  if (!s_on) return;
  std::vector<uint8_t> buf;
  GLenum fmt = 0;
  GLsizei got = 0;
  if (s_bin) {
    GLint len = 0;
    glGetProgramiv(p, kBinaryLength, &len);
    if (len > 0) {
      buf.resize((size_t)len);
      pGet(p, len, &got, &fmt, buf.data());
      if (got <= 0) { got = 0; glGetError(); }
    }
  }
  auto &e = s_entries[key];
  if (!got && e.fs == fs) return;  // nothing new (no binaries on this driver)
  if (e.fs.empty()) s_order.push_back(key);
  e.fs = fs;
  Write(key, fmt, buf.data(), (uint32_t)got, fs);
}
} // namespace shcache

static GLuint CompileShader(const char *vs, const char *fs);

// Builds (or loads from the disk cache) the TEV program for this source.
// Compiles a TEV program from source (the shared vertex shader + fs_src).
static GLuint CompileTevProgram(const std::string &vs_src, const std::string &fs_src) {
  if (sso::s_on) {  // the fragment stage only (MakeShader adds the pipeline)
    ++g_prof_shader_compiles;
    return sso::BuildFs(fs_src);
  }
  // (The vertex source is the same for every TEV program of a path: one
  // compiled shader per vertex source, classic and GPU XF.)
  static GLuint s_vsc[2] = {0, 0};
  static uint64_t s_vsc_hash[2] = {0, 0};
  const uint64_t vh = Fnv64(vs_src);
  int slot = 0;
  while (slot < 2 && s_vsc[slot] && s_vsc_hash[slot] != vh) ++slot;
  if (slot == 2) slot = 1;  // (never: two vertex sources at most)
  if (!s_vsc[slot] || s_vsc_hash[slot] != vh) {
    s_vsc[slot] = glCreateShader(GL_VERTEX_SHADER);
    const char *v = vs_src.c_str();
    glShaderSource(s_vsc[slot], 1, &v, nullptr);
    glCompileShader(s_vsc[slot]);
    s_vsc_hash[slot] = vh;
  }
  const GLuint s_vs = s_vsc[slot];
  ++g_prof_shader_compiles;
  GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
  const char *fsrc = fs_src.c_str();
  glShaderSource(f, 1, &fsrc, nullptr);
  glCompileShader(f);
  GLuint p = glCreateProgram();
  if (shcache::s_bin) shcache::pParam(p, shcache::kRetrievableHint, GL_TRUE);
  glAttachShader(p, s_vs);
  glAttachShader(p, f);
  glLinkProgram(p);
  GLint linked = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &linked);
  static int s_failures = 0;
  if (!linked && s_failures++ < 8) {
    char log[1024] = {0};
    glGetShaderInfoLog(f, sizeof(log) - 1, nullptr, log);
    if (!log[0]) glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
    printf("[GL] TEV shader failed to build: %s\n", log);
  }
  glDetachShader(p, s_vs);
  glDetachShader(p, f);
  glDeleteShader(f);
  if (!linked) { glDeleteProgram(p); return 0; }
  return p;
}

// Builds (or loads from the disk cache) the TEV program for this source.
static GLuint BuildTevProgram(uint64_t key, const std::string &vs_src,
                              const std::string &fs_src) {
  shcache::Init();
  if (GLuint p = shcache::LoadBinary(key)) {
    ++g_prof_shader_loads;
    return p;
  }
  GLuint p = CompileTevProgram(vs_src, fs_src);
  if (p) shcache::Store(key, p, fs_src);
  return p;
}

static GLShader MakeShader(GLuint id, bool xv);

// NWII_SHADER_BENCH=<n> (diagnostic): before the prewarm, time the first n
// cached programs phase by phase (FS compile, link, first draw) with the real
// vertex shader, a vertex shader whose light loops cannot be unrolled, and a
// pass-through one; prints [SHBENCH] lines. Nothing is kept.
static double BenchMs(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
static void ShaderBench(const std::string &vs_real) {
  const char *e = NWII_ENV("NWII_SHADER_BENCH");
  if (!e) return;
  const int n = std::max(1, std::atoi(e));
  GLint nfmt = -1;
  glGetIntegerv(0x87FE, &nfmt);
  glGetError();
  GLint next = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &next);
  bool sso = false, pbin = false;
  for (GLint i = 0; i < next; i++) {
    const char *x = (const char *)glGetStringi(GL_EXTENSIONS, i);
    if (!x) continue;
    if (!std::strcmp(x, "GL_ARB_separate_shader_objects")) sso = true;
    if (!std::strcmp(x, "GL_ARB_get_program_binary")) pbin = true;
  }
  printf("[SHBENCH] GL_VENDOR=%s GL_RENDERER=%s GL_VERSION=%s GLSL=%s\n",
         (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
         (const char *)glGetString(GL_VERSION),
         (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
  printf("[SHBENCH] NUM_PROGRAM_BINARY_FORMATS=%d ARB_get_program_binary=%d "
         "ARB_separate_shader_objects=%d extensions=%d NV50_PROG_OPTIMIZE=%s\n",
         nfmt, pbin, sso, next, getenv("NV50_PROG_OPTIMIZE") ? getenv("NV50_PROG_OPTIMIZE") : "-");
  std::string vs_loop = vs_real;
  {
    const std::string from = "for (int i = 0; i < 8; ++i)", to = "for (int i = 0; i < uLoopN; ++i)";
    for (size_t at; (at = vs_loop.find(from)) != std::string::npos;) vs_loop.replace(at, from.size(), to);
    const size_t at = vs_loop.find("uniform mat4 mvp;");
    if (at != std::string::npos) vs_loop.insert(at, "uniform int uLoopN;\n");
  }
  std::string vs_triv = "#version 330 core\nlayout(location = 0) in vec3 vertexPosition;\n"
                        "out vec4 vColor0;\nout vec4 vColor1;\n";
  for (int i = 0; i < 8; ++i) vs_triv += "out vec2 vTex" + std::to_string(i) + ";\n";
  vs_triv += "void main() {\n gl_Position = vec4(vertexPosition, 1.0);\n vColor0 = vec4(1.0);\n vColor1 = vec4(1.0);\n";
  for (int i = 0; i < 8; ++i) vs_triv += " vTex" + std::to_string(i) + " = vertexPosition.xy;\n";
  vs_triv += "}\n";
  GLint old_fbo = 0, vp[4];
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);
  glGetIntegerv(GL_VIEWPORT, vp);
  GLuint tex = 0, fbo = 0, vao = 0, vbo = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glViewport(0, 0, 4, 4);
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  const float tri[9] = {-1, -1, 0, 3, -1, 0, -1, 3, 0};
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
  glFinish();
  const char *names[3] = {"real", "noloopunroll", "passthrough"};
  const std::string *vss[3] = {&vs_real, &vs_loop, &vs_triv};
  GLuint vsh[3];
  for (int k = 0; k < 3; k++) {
    const auto t0 = std::chrono::steady_clock::now();
    vsh[k] = glCreateShader(GL_VERTEX_SHADER);
    const char *v = vss[k]->c_str();
    glShaderSource(vsh[k], 1, &v, nullptr);
    glCompileShader(vsh[k]);
    GLint ok = 0;
    glGetShaderiv(vsh[k], GL_COMPILE_STATUS, &ok);
    printf("[SHBENCH] VS %s: compile %.1f ms ok=%d (%zu bytes)\n", names[k], BenchMs(t0), ok,
           vss[k]->size());
  }
  double tot[3][3] = {}, worst[3] = {};
  int count = 0;
  for (uint64_t key : shcache::s_order) {
    if (count >= n) break;
    const auto it = shcache::s_entries.find(key);
    if (it == shcache::s_entries.end() || it->second.fs.empty()) continue;
    ++count;
    for (int k = 0; k < 3; k++) {
      auto t0 = std::chrono::steady_clock::now();
      GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
      const char *fsrc = it->second.fs.c_str();
      glShaderSource(f, 1, &fsrc, nullptr);
      glCompileShader(f);
      GLint ok = 0;
      glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
      const double tc = BenchMs(t0);
      t0 = std::chrono::steady_clock::now();
      GLuint p = glCreateProgram();
      glAttachShader(p, vsh[k]);
      glAttachShader(p, f);
      glLinkProgram(p);
      GLint linked = 0;
      glGetProgramiv(p, GL_LINK_STATUS, &linked);
      const double tl = BenchMs(t0);
      t0 = std::chrono::steady_clock::now();
      glUseProgram(p);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();
      const double td = BenchMs(t0);
      glUseProgram(0);
      glDeleteProgram(p);
      glDeleteShader(f);
      tot[k][0] += tc; tot[k][1] += tl; tot[k][2] += td;
      worst[k] = std::max(worst[k], tc + tl + td);
      if (count <= 3 || NWII_ENV("NWII_SHADER_BENCH_ALL"))
        printf("[SHBENCH] #%d %016llx vs=%s fs=%zuB compile %.1f link %.1f (ok=%d/%d) draw %.1f ms\n",
               count, (unsigned long long)key, names[k], it->second.fs.size(), tc, tl, ok, linked, td);
    }
  }
  for (int k = 0; k < 3; k++)
    printf("[SHBENCH] SUMMARY vs=%s n=%d avg: fs_compile %.1f link %.1f first_draw %.1f total %.1f ms "
           "(worst %.1f)\n", names[k], count, tot[k][0] / std::max(1, count),
           tot[k][1] / std::max(1, count), tot[k][2] / std::max(1, count),
           (tot[k][0] + tot[k][1] + tot[k][2]) / std::max(1, count), worst[k]);
  if (sso::s_on) {
    // The separable path: fragment program + pipeline + first draw (the
    // shared vertex program was built once in sso::Init).
    double st[2] = {}, sw = 0;
    int sn = 0;
    for (uint64_t key : shcache::s_order) {
      if (sn >= n) break;
      const auto it = shcache::s_entries.find(key);
      if (it == shcache::s_entries.end() || it->second.fs.empty()) continue;
      ++sn;
      auto t0 = std::chrono::steady_clock::now();
      const GLuint f = sso::BuildFs(it->second.fs);
      GLuint pp = 0;
      sso::pGen(1, &pp);
      sso::pStages(pp, sso::kVsBit, sso::VProg(xf::s_on));
      sso::pStages(pp, sso::kFsBit, f);
      const double tb = BenchMs(t0);
      t0 = std::chrono::steady_clock::now();
      glUseProgram(0);
      sso::pBind(pp);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();
      const double td = BenchMs(t0);
      sso::pBind(0);
      sso::pDel(1, &pp);
      glDeleteProgram(f);
      st[0] += tb; st[1] += td;
      sw = std::max(sw, tb + td);
      if (sn <= 3 || NWII_ENV("NWII_SHADER_BENCH_ALL"))
        printf("[SHBENCH] #%d %016llx separable: build %.1f (ok=%d) first_draw %.1f ms\n", sn,
               (unsigned long long)key, tb, f != 0, td);
    }
    printf("[SHBENCH] SUMMARY separable n=%d avg: build %.1f first_draw %.1f total %.1f ms "
           "(worst %.1f)\n", sn, st[0] / std::max(1, sn), st[1] / std::max(1, sn),
           (st[0] + st[1]) / std::max(1, sn), sw);
  }
  for (GLuint v : vsh) glDeleteShader(v);
  glBindVertexArray(0);
  glDeleteBuffers(1, &vbo);
  glDeleteVertexArrays(1, &vao);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_fbo);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &tex);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  GLInvalidate();
}

// Boot progress bar while the prewarm runs (seconds on the console, black
// screen otherwise): the window's framebuffer is cleared and a bar drawn with
// scissored clears (no shader, no geometry), then swapped without waiting for
// vsync. All GL state it touches is put back.
static void PrewarmProgressBar(int done, int total) {
  SDL_Window *win = GX_HostWindow();
  if (!win || total <= 0) return;
  int w = 0, h = 0;
  GX_HostDrawable(win, &w, &h);
  if (w <= 0 || h <= 0) return;
  GLint fbo = 0, sc[4];
  GLfloat cc[4];
  GLboolean cm[4];
  const GLboolean sc_on = glIsEnabled(GL_SCISSOR_TEST);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
  glGetIntegerv(GL_SCISSOR_BOX, sc);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
  glGetBooleanv(GL_COLOR_WRITEMASK, cm);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  auto rect = [](int x, int y, int rw, int rh, float r, float g, float b) {
    glScissor(x, y, rw, rh);
    glClearColor(r, g, b, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
  };
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  const int bw = w / 2, bh = std::max(6, h / 48), bx = (w - bw) / 2, by = h / 8;
  const int pad = std::max(2, bh / 4);
  rect(bx - pad, by - pad, bw + 2 * pad, bh + 2 * pad, 0.35f, 0.35f, 0.35f);  // frame
  rect(bx, by, bw, bh, 0.08f, 0.08f, 0.08f);                                  // track
  const int fill = (int)((long long)bw * done / total);
  if (fill > 0) rect(bx, by, fill, bh, 0.92f, 0.16f, 0.12f);                  // progress
  // Test hook: NWII_SHADER_PREWARM_BAR_DUMP=<prefix> writes the first bar of
  // each quarter as <prefix>_<done>.ppm (the window is invisible in offscreen
  // runs and in the emulator test).
  static int dumped_quarter = -1;
  const char *dump = NWII_ENV("NWII_SHADER_PREWARM_BAR_DUMP");
  if (dump && done * 4 / total != dumped_quarter) {
    dumped_quarter = done * 4 / total;
    std::vector<unsigned char> px((size_t)w * h * 3);
    GLint rfbo = 0, align = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfbo);
    const std::string p = std::string(dump) + "_" + std::to_string(done) + ".ppm";
    if (FILE *f = std::fopen(p.c_str(), "wb")) {
      std::fprintf(f, "P6\n%d %d\n255\n", w, h);
      for (int y = h - 1; y >= 0; --y) std::fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
      std::fclose(f);
    }
  }
  static int interval = -2;
  if (interval == -2) interval = GX_HostGetSwapInterval();
  if (interval != 0) GX_HostSetSwapInterval(0);
  GX_HostSwap(win);
  if (interval != 0) GX_HostSetSwapInterval(interval);
  if (!sc_on) glDisable(GL_SCISSOR_TEST);
  glScissor(sc[0], sc[1], sc[2], sc[3]);
  glClearColor(cc[0], cc[1], cc[2], cc[3]);
  glColorMask(cm[0], cm[1], cm[2], cm[3]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)fbo);
}

// At the first frame: every program of the cache file (binary, or rebuilt from
// its source) goes into s_programs before any draw needs it, and draws one
// triangle into a 4x4 target so the driver's own backend compile (nouveau
// translates a program at its first draw) also happens here.
// NWII_SHADER_PREWARM=0 disables; NWII_SHADER_PREWARM_DRAW=0 skips the draws.
// Progress goes to the log ([SHADERCACHE] prewarm i/n).
static void PrewarmPrograms() {
  static bool done = false;
  if (done) return;
  done = true;
  if (const char *e = NWII_ENV("NWII_SHADER_PREWARM"); e && !std::strcmp(e, "0")) return;
  shcache::Init();
  // (Also when the file is not writable: the sources read are still built.)
  if (shcache::s_order.empty()) return;
  const auto t0 = std::chrono::steady_clock::now();
  auto ms_since = [](std::chrono::steady_clock::time_point t) {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t).count();
  };
  const std::string vs_old = GenerateTEVShader(g_state, 0).vertex_source;
  ShaderBench(vs_old);
  if (NWII_ENV("NWII_SHADER_BENCH_ONLY")) return;
  // R11: programs of the path in use (GPU XF or classic). In-memory key: FS
  // + the vertex source in use; disk key: the same for linked programs (their
  // binaries hold the vertex shader), but FS + the classic vertex source for
  // separable ones, whose cache entry is the fragment source alone: the
  // shipped cache stays valid whichever vertex shader runs.
  const bool xv = xf::s_on;
  const std::string &vs = xv ? xf::VsText() : vs_old;
  const uint64_t vh = Fnv64(vs), vh_disk = sso::s_on ? Fnv64(vs_old) : vh;
  const char *de = NWII_ENV("NWII_SHADER_PREWARM_DRAW");
  const bool warm_draw = !(de && !std::strcmp(de, "0"));
  GLint old_fbo = 0, vp[4];
  GLuint tex = 0, fbo = 0, vao = 0, vbo = 0;
  if (warm_draw) {
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 4, 4);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    const float tri[9] = {-1, -1, 0, 3, -1, 0, -1, 3, 0};
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
  }
  int nbin = 0, nsrc = 0, nfail = 0, done_n = 0;
  long long worst = 0;
  const std::vector<uint64_t> order = shcache::s_order;
  const int total = (int)order.size();
  printf("[SHADERCACHE] prewarm: %d programs (%s)...\n", total,
         sso::s_on ? "separable" : "linked");
  fflush(stdout);
  auto last_log = std::chrono::steady_clock::now();
  // NWII_SHADER_PREWARM_BAR=0: no progress bar (black screen as before).
  const char *be = NWII_ENV("NWII_SHADER_PREWARM_BAR");
  const bool bar = !(be && !std::strcmp(be, "0"));
  auto last_bar = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  for (uint64_t key : order) {
    if (bar && std::chrono::steady_clock::now() - last_bar >= std::chrono::milliseconds(100)) {
      PrewarmProgressBar(done_n, total);
      last_bar = std::chrono::steady_clock::now();
    }
    ++done_n;
    auto it = shcache::s_entries.find(key);
    if (it == shcache::s_entries.end()) continue;
    const std::string fs = it->second.fs;
    if (fs.empty()) continue;
    const uint64_t k2 = Fnv64(fs, vh);  // differs if the vertex shader changed
    const uint64_t kd = Fnv64(fs, vh_disk);
    if (s_programs.count(k2)) continue;
    const auto tp = std::chrono::steady_clock::now();
    GLuint p = kd == key ? shcache::LoadBinary(key) : 0;
    if (p) {
      ++nbin;
    } else if ((p = CompileTevProgram(vs, fs))) {
      ++nsrc;
      shcache::Store(kd, p, fs);
    } else {
      ++nfail;
      continue;
    }
    GLShader &sh = s_programs[k2] = MakeShader(p, xv);
    if (warm_draw && sh.id) {
      if (sso::s_on) { glUseProgram(0); sso::pBind(sh.id); }
      else glUseProgram(sh.id);
      glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    worst = std::max(worst, ms_since(tp));
    if (ms_since(last_log) >= 1000 || done_n == total) {
      if (warm_draw) glFinish();
      last_log = std::chrono::steady_clock::now();
      printf("[SHADERCACHE] prewarm %d/%d (%lld ms)\n", done_n, total, ms_since(t0));
      fflush(stdout);
    }
  }
  if (bar) PrewarmProgressBar(total, total);
  if (warm_draw) {
    glFinish();
    glUseProgram(0);
    if (sso::s_on) sso::pBind(0);
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_fbo);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
  }
  GLInvalidate();
  g_prof_shader_compiles = g_prof_shader_loads = 0;  // not in-game cost
  printf("[SHADERCACHE] prewarm: %d from binaries, %d compiled, %d failed in %lld ms "
         "(worst program %lld ms, %s)\n",
         nbin, nsrc, nfail, ms_since(t0), worst, sso::s_on ? "separable" : "linked");
  fflush(stdout);
}

static GLuint CompileShader(const char *vs, const char *fs) {
  GLuint v = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(v, 1, &vs, nullptr);
  glCompileShader(v);

  GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(f, 1, &fs, nullptr);
  glCompileShader(f);

  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);

  // A failed TEV shader draws nothing, silently: report the first few.
  GLint linked = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &linked);
  static int s_failures = 0;
  if (!linked && s_failures++ < 8) {
    char log[1024] = {0};
    glGetShaderInfoLog(f, sizeof(log) - 1, nullptr, log);
    if (!log[0])
      glGetShaderInfoLog(v, sizeof(log) - 1, nullptr, log);
    if (!log[0])
      glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
    printf("[GL] TEV shader failed to build: %s\n", log);
  }

  glDeleteShader(v);
  glDeleteShader(f);
  return p;
}

static void FlushBatch() {
  if (xf::Active()) {
    xf::FlushBatch();
    return;
  }
  if (s_batch_idx.empty()) {
    s_batch.clear();
    return;
  }
  const int ph0 = rph::Set(rph::kUpload);
  if (rph::On()) {
    ++rph::s_flushes;
    rph::s_idx += s_batch_idx.size();
    rph::s_bytes += s_batch.size() * sizeof(BatchVertex) + s_batch_idx.size() * 4;
  }
  if (vbuf::s_on) {
    // R9: the vertices are already in the mapped buffer (arena slots and
    // ring); only the batch's indices are copied, into the index ring.
    const uint64_t ib = (uint64_t)s_batch_idx.size() * 4;
    const uint64_t off = vbuf::IndexAlloc(ib);
    std::memcpy(vbuf::s_i + off, s_batch_idx.data(), ib);
    CBindVertexArray(vbuf::s_vao);
    rph::Set(rph::kGlDraw);
    glDrawElements(GL_TRIANGLES, (GLsizei)s_batch_idx.size(), GL_UNSIGNED_INT,
                   (const void *)(uintptr_t)off);
    ++g_prof_gl_draws;
    ++s_cap_cur.gl_draws;
    s_batch.clear();
    s_batch_idx.clear();
    rph::Set(ph0);
    return;
  }

  if (!s_vao) {
    g_gl.vao = ~0u;
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                          (void *)offsetof(BatchVertex, x));
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                          (void *)offsetof(BatchVertex, r));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                          (void *)offsetof(BatchVertex, u));
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                          (void *)offsetof(BatchVertex, nx));
    glEnableVertexAttribArray(4);
    glEnableVertexAttribArray(5);
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                          (void *)offsetof(BatchVertex, u1));
    glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                          (void *)offsetof(BatchVertex, u2));
    glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                          (void *)offsetof(BatchVertex, u3));
    glGenBuffers(1, &s_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);  // part of the VAO
  }

  CBindVertexArray(s_vao);
  glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
  // Streaming ring (R4): each batch is appended to one large vertex and one
  // index buffer (glBufferSubData into a range the GPU has not used since the
  // last orphan, which Mesa writes unsynchronized) and drawn with a base
  // vertex. glBufferData per batch reallocated two buffers ~200 times a
  // frame, most of the GL driver thread's work. NWII_NORING=1: old path.
  static const bool s_noring = NWII_ENV("NWII_NORING") != nullptr;
  const size_t vbytes = s_batch.size() * sizeof(BatchVertex);
  const size_t ibytes = s_batch_idx.size() * sizeof(uint32_t);
  constexpr size_t kVCap = 24u << 20, kICap = 8u << 20;
  static size_t s_voff = kVCap, s_ioff = kICap;  // full: orphan on first use
  GLint base_vertex = 0;
  const void *ioffset = nullptr;
  if (s_noring || vbytes > kVCap || ibytes > kICap) {
    glBufferData(GL_ARRAY_BUFFER, vbytes, s_batch.data(), GL_STREAM_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ibytes, s_batch_idx.data(), GL_STREAM_DRAW);
    s_voff = kVCap; s_ioff = kICap;  // the ring's storage is gone
  } else {
    if (s_voff + vbytes > kVCap) {
      glBufferData(GL_ARRAY_BUFFER, kVCap, nullptr, GL_STREAM_DRAW);
      s_voff = 0;
    }
    if (s_ioff + ibytes > kICap) {
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, kICap, nullptr, GL_STREAM_DRAW);
      s_ioff = 0;
    }
    glBufferSubData(GL_ARRAY_BUFFER, s_voff, vbytes, s_batch.data());
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, s_ioff, ibytes, s_batch_idx.data());
    base_vertex = (GLint)(s_voff / sizeof(BatchVertex));
    ioffset = (const void *)s_ioff;
    // Next vertex range starts on a whole vertex; indices stay 4-aligned.
    s_voff += vbytes;
    s_ioff += ibytes;
  }

  static const bool s_gxtrace1 = NWII_ENV("NWII_GXTRACE") != nullptr;
  if (s_gxtrace1) {
    static int dn = 0;
    if (dn++ < 5)
      printf("[GXTRACE] flush #%d verts=%zu\n", dn, s_batch.size());
  }
  rph::Set(rph::kGlDraw);
  glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)s_batch_idx.size(),
                           GL_UNSIGNED_INT, ioffset, base_vertex);
  ++g_prof_gl_draws;
  ++s_cap_cur.gl_draws;
  static const bool s_gxtrace2 = NWII_ENV("NWII_GXTRACE") != nullptr;
  if (s_gxtrace2) {
    static int en = 0;
    if (en++ < 8) {
      GLint prog = 0, linked = 0;
      glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
      if (prog)
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
      GLenum err = glGetError();
      printf("[GXTRACE] draw: prog=%d linked=%d glErr=0x%x verts=%zu\n", prog,
             linked, err, s_batch.size());
      if (prog && !linked) {
        char log[512] = {0};
        GLsizei n = 0;
        glGetProgramInfoLog(prog, 511, &n, log);
        printf("[GXTRACE] link log: %s\n", log);
      }
    }
  }
  s_batch.clear();
  s_batch_idx.clear();
  rph::Set(ph0);
}

static void ApplyProjection(GLShader &vsh) {
  const GLint uProj = vsh.uProjMtx;
  if (uProj < 0)
    return;
  const float *sp = g_state.projection;
  float p[16] = {0};

  if (!g_state.projSet) {
    p[0] = 1;
    p[5] = 1;
    p[10] = 1;
    p[15] = 1;
  } else if (g_state.projType == 0) {
    p[0] = sp[0];
    p[4] = 0;
    p[8] = sp[1];
    p[12] = 0;
    p[1] = 0;
    p[5] = sp[2];
    p[9] = sp[3];
    p[13] = 0;
    p[2] = 0;
    p[6] = 0;
    p[10] = 2.0f * sp[4] - 1.0f;
    p[14] = 2.0f * sp[5];
    p[3] = 0;
    p[7] = 0;
    p[11] = -1.0f;
    p[15] = 0;
  } else {
    p[0] = sp[0];
    p[4] = 0;
    p[8] = 0;
    p[12] = sp[1];
    p[1] = 0;
    p[5] = sp[2];
    p[9] = 0;
    p[13] = sp[3];
    p[2] = 0;
    p[6] = 0;
    // GX clip z is [-1, 0] near..far; stretch it over GL's [-1, 1].
    p[10] = 2.0f * sp[4];
    p[14] = 2.0f * sp[5] + 1.0f;
    p[3] = 0;
    p[7] = 0;
    p[11] = 0;
    p[15] = 1.0f;
  }
  static const bool s_vtxtrace_p = NWII_ENV("NWII_VTXTRACE") != nullptr;
  if (s_vtxtrace_p) {
    static int pn = 0;
    if (pn++ < 2)
      printf("[PROJ] type=%u set=%d raw=[%.5f %.2f %.5f %.2f %.2f %.2f]\n",
             g_state.projType, (int)g_state.projSet, sp[0], sp[1], sp[2], sp[3],
             sp[4], sp[5]);
  }
  std::memcpy(s_proj, p, sizeof(s_proj));
  if (vsh.have_last && !std::memcmp(vsh.last_proj, p, sizeof(p)))
    return;
  UM4fv(vsh.id, uProj, p);
  std::memcpy(vsh.last_proj, p, sizeof(p));
}

static bool CurrentIsPerspective() {
  static const bool flatz = NWII_ENV("NWII_FLATZ") != nullptr;
  if (flatz)
    return false;
  return g_state.projSet && g_state.projType == 0;
}

// GX ZMode (BP 0x40): bit 0 enable, bits 1-3 compare, bit 4 update. With the
// test disabled GX also skips the depth write, as GL does.
static void ApplyZMode() {
  static const bool s_legacy = NWII_ENV("NWII_FLATZ") != nullptr;
  if (s_legacy) {
    if (CurrentIsPerspective()) {
      CEnable(kEnDepth, GL_DEPTH_TEST, true);
      CDepthFunc(GL_LEQUAL);
    } else {
      CEnable(kEnDepth, GL_DEPTH_TEST, false);
    }
    return;
  }
  static const GLenum funcs[8] = {GL_NEVER,  GL_LESS,     GL_EQUAL,  GL_LEQUAL,
                                  GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
  uint32_t zm = g_state.bp[0x40];
  if (!(zm & 1)) {
    CEnable(kEnDepth, GL_DEPTH_TEST, false);
    return;
  }
  CEnable(kEnDepth, GL_DEPTH_TEST, true);
  CDepthFunc(funcs[(zm >> 1) & 7]);
  CDepthMask((zm >> 4) & 1);
}

// Apply the GX blend state (BP 0x41 = PE_CMODE0). Without it every draw is
// opaque, so alpha fades (the WARNING "press" blink), transparent UI and the
// dialogue window never composite. The src/dst factor tables follow the
// documented GX->GL factor order (src uses DST_COLOR at index 2/3, dst uses
// SRC_COLOR).
// Destination alpha (BP 0x42): with bit 8 set and alpha updates on, the
// EFB alpha receives the constant in bits 0-7 instead of the TEV alpha. The
// shader then outputs the TEV alpha as the second (dual-source) colour for
// the RGB blend factors -- Dolphin's approach. Only meaningful in RGBA6.
static bool DstAlphaActive() {
  return (g_state.bp[0x42] & 0x100) && ((g_state.bp[0x41] >> 4) & 1) &&
         (g_state.bp[0x43] & 7) == 1;
}

static void ApplyBlend() {
  uint32_t cm = g_state.bp[0x41];
  const bool has_alpha = (g_state.bp[0x43] & 7) == 1;
  // Write masks (bits 3-4; alpha only exists in the RGBA6 pixel format) and
  // the logic op (bit 1, op in bits 12-15) used when blending is off.
  {
    bool cu = (cm >> 3) & 1;
    bool au = ((cm >> 4) & 1) && has_alpha;
    if (!(cm & 1) && (cm & 2)) {
      const int op = (cm >> 12) & 0xF;
      if (op == 5) {  // NOOP: nothing but (optionally) constant alpha
        cu = false;
        au = au && (g_state.bp[0x42] & 0x100);
        CEnable(kEnLogic, GL_COLOR_LOGIC_OP, false);
      } else {
        static const GLenum ops[16] = {
            GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED, GL_NOOP,
            GL_XOR, GL_OR, GL_NOR, GL_EQUIV, GL_INVERT, GL_OR_REVERSE,
            GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND, GL_SET};
        CEnable(kEnLogic, GL_COLOR_LOGIC_OP, true);
        CLogicOp(ops[op]);
      }
    } else {
      CEnable(kEnLogic, GL_COLOR_LOGIC_OP, false);
    }
    CColorMask(cu, au);
  }
  bool enable = cm & 1;
  bool subtract = (cm >> 11) & 1;
  int dstF = (cm >> 5) & 7;
  int srcF = (cm >> 8) & 7;
  const bool dst_alpha = DstAlphaActive();
  if (!enable) {
    // (With constant alpha the shader already outputs it in .a.)
    CEnable(kEnBlend, GL_BLEND, false);
    return;
  }
  // With dual-source output the TEV alpha lives in the second colour.
  const GLenum sa = dst_alpha ? GL_SRC1_ALPHA : GL_SRC_ALPHA;
  const GLenum isa = dst_alpha ? GL_ONE_MINUS_SRC1_ALPHA : GL_ONE_MINUS_SRC_ALPHA;
  // Without EFB alpha, destination alpha reads as 1 (Dolphin
  // RemoveDstAlphaUsage).
  const GLenum da = has_alpha ? GL_DST_ALPHA : GL_ONE;
  const GLenum ida = has_alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ZERO;
  const GLenum srcMap[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR,
                            GL_ONE_MINUS_DST_COLOR, sa, isa, da, ida};
  const GLenum dstMap[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR,
                            GL_ONE_MINUS_SRC_COLOR, sa, isa, da, ida};
  CEnable(kEnBlend, GL_BLEND, true);
  if (subtract) {
    if (dst_alpha) {
      CBlendEquation(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
      CBlendFunc(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
    } else {
      CBlendEquation(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT);
      CBlendFunc(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    }
  } else {
    CBlendEquation(GL_FUNC_ADD, GL_FUNC_ADD);
    if (dst_alpha)
      CBlendFunc(srcMap[srcF], dstMap[dstF], GL_ONE, GL_ZERO);
    else
      CBlendFunc(srcMap[srcF], dstMap[dstF], srcMap[srcF], dstMap[dstF]);
  }
}

// Backface culling from BP 0x00 (GEN_MODE), bits 14-15: 0 none, 1 cull back,
// 2 cull front, 3 cull all.
//
// The subtlety is which way round GL is told about it. Our projection negates
// Y, so a triangle that is counter-clockwise on the console arrives clockwise
// here. Rather than fight that per draw, front-facing is declared clockwise and
// the cull target left at GL's default back face; culling the front is then a
// matter of declaring counter-clockwise instead. Switching glCullFace while
// leaving the default GL_CCW winding removes the opposite half of the scene --
// that is what took out foliage and text on the first attempt.
//
// NWII_NOCULL=1 disables culling entirely.
// The EFB lives in a 640x528 GL framebuffer whose origin is bottom-left; GX
// addresses it top-left. Every GX rectangle is flipped through kEfbHeight.
static constexpr int kEfbWidth = 640, kEfbHeight = 528;

// Internal resolution (Dolphin's "internal resolution"): the GL EFB is
// kEfbWidth*s x kEfbHeight*s and every GX rectangle (viewport, scissor,
// clears, copy sources) is multiplied by s. GX-side sizes (texture sizes,
// copy sizes the texture cache matches) stay native; only GL storage grows.
// EFB copies to textures are made at the scaled size, the XFB copy too.
// s = 1 is the console's EFB, bit for bit the renderer before scaling.
// Changed only between frames (GX_ResizeEfb).
//
// r16res: s may be fractional (1.3636, 1.5789 = "720p" ...). Rule: GX
// coordinates map to scaled ones as X = s * x (geometry, through a float
// viewport), and scaled pixel p (centre p + 0.5) belongs to native pixel
// floor((p + 0.5) / s). Every GX rectangle edge x (scissor, clears, copy
// sources, the XFB) becomes SE(x), the first scaled pixel whose centre is at
// or past s * x: rectangles that share an edge share it exactly (no seams or
// overlaps), and they cover the same pixels the rasterizer gives geometry
// with that edge. For integer s, SE(x) = x * s and the float viewport is not
// used: integer scales are bit for bit what they were.
static float s_scale = 1.0f;
static int s_iscale = 1;  // s when it is an integer, else 0
static int s_efb_w = kEfbWidth, s_efb_h = kEfbHeight;  // GL size of the EFB
static inline int SE(int x) {
  return s_iscale ? x * s_iscale : (int)std::ceil(x * (double)s_scale - 0.5);
}
static void SetScale(float s) {
  s_scale = s;
  s_iscale = s == std::round(s) ? (int)std::round(s) : 0;
  s_efb_w = SE(kEfbWidth);
  s_efb_h = SE(kEfbHeight);
}
// glViewportIndexedf (GL 4.1 / ARB_viewport_array; glad here is 3.3): the
// subpixel viewport of fractional scales. Null: viewports are rounded edge
// by edge instead.
typedef void (APIENTRYP PFNViewportIndexedf)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
static PFNViewportIndexedf s_viewport_f = nullptr;
static float s_vpf[4];
static inline void CViewportF(float x, float y, float w, float h) {
  constexpr GLint kFloatVp = -0x7FFFFFF0;  // g_gl.vp[0] while a float viewport is set
  if (g_gl.vp[0] != kFloatVp || s_vpf[0] != x || s_vpf[1] != y || s_vpf[2] != w ||
      s_vpf[3] != h) {
    s_viewport_f(0, x, y, w, h);
    g_gl.vp[0] = kFloatVp;
    s_vpf[0] = x; s_vpf[1] = y; s_vpf[2] = w; s_vpf[3] = h;
  }
}

// BP 0x59: scissor/viewport offset in 2-pixel units, biased by 342 like the
// rectangles it offsets. Unset (0) means no offset.
static void ScissorOffset(int &ox, int &oy) {
  uint32_t v = g_state.bp[0x59];
  ox = v ? (int)(v & 0x3FF) * 2 - 342 : 0;
  oy = v ? (int)((v >> 10) & 0x3FF) * 2 - 342 : 0;
}

// XF 0x101A-0x101F: wd, ht (negative), zRange, xOrig, yOrig, farZ; origins
// carry the 342 bias. A game that never set one draws to the whole EFB.
static void ApplyViewport() {
  float vp[6];
  std::memcpy(vp, &g_state.xf[0x1A], sizeof(vp));
  float w = 2.0f * vp[0], h = -2.0f * vp[1];
  if (!(w > 0.0f && h > 0.0f)) {
    CViewport(0, 0, s_efb_w, s_efb_h);
    CDepthRange(0.0, 1.0);
    return;
  }
  // Depth: GX writes farZ + zRange * z_clip (z_clip in [-1, 0]) in 24-bit
  // units, i.e. the viewport's [near, far] = [farZ - zRange, farZ] / 2^24-1.
  // The projection maps z_clip onto GL's [-1, 1], so glDepthRange(near, far)
  // reproduces the same window depth. NWII_NODEPTHRANGE=1: always [0, 1].
  static const bool s_nodr = NWII_ENV("NWII_NODEPTHRANGE") != nullptr;
  if (s_nodr) {
    CDepthRange(0.0, 1.0);
  } else {
    const double zr = vp[2], fz = vp[5];
    double n = (fz - zr) / 16777215.0, f = fz / 16777215.0;
    n = std::min(1.0, std::max(0.0, n));
    f = std::min(1.0, std::max(0.0, f));
    CDepthRange(n, f);
    static const bool s_vplog = NWII_ENV("NWII_VPLOG") != nullptr;
    if (s_vplog) {
      static std::unordered_map<uint64_t, int> seen;
      uint64_t k;
      const float pr[2] = {vp[2], vp[5]};
      std::memcpy(&k, pr, 8);
      if (seen.size() < 32 && seen.emplace(k, 1).second)
        printf("[VPLOG] zRange=%.1f farZ=%.1f -> depth [%.6f, %.6f] proj=%u p4=%g p5=%g\n",
               vp[2], vp[5], n, f, g_state.projType, g_state.projection[4],
               g_state.projection[5]);
    }
  }
  int ox, oy;
  ScissorOffset(ox, oy);
  float left = vp[3] - vp[0] - 342.0f - ox;
  float top = vp[4] + vp[1] - 342.0f - oy;
  if (!s_iscale) {  // r16res: fractional scale
    const double s = s_scale, x0 = s * left, x1 = s * (left + w);
    const double y0 = s_efb_h - s * (top + h), y1 = s_efb_h - s * top;  // GL rows
    if (s_viewport_f) {
      CViewportF((float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0));
    } else {
      const GLint ix = (GLint)std::lround(x0), iy = (GLint)std::lround(y0);
      CViewport(ix, iy, (GLsizei)std::lround(x1) - ix, (GLsizei)std::lround(y1) - iy);
    }
    return;
  }
  // Scaled in float before rounding (x 1.0f is exact: native is unchanged).
  const float fs = (float)s_iscale;
  CViewport((GLint)std::lround(left * fs),
            (GLint)std::lround((kEfbHeight - (top + h)) * fs),
            (GLsizei)std::lround(w * fs), (GLsizei)std::lround(h * fs));
}

// BP 0x20 (top-left) / 0x21 (bottom-right, inclusive): y in bits 0-10, x in
// bits 12-22, both biased by 342.
static void ApplyScissor() {
  uint32_t tl = g_state.bp[0x20], br = g_state.bp[0x21];
  if (!tl && !br) {
    CEnable(kEnScissor, GL_SCISSOR_TEST, false);
    return;
  }
  int ox, oy;
  ScissorOffset(ox, oy);
  int x0 = (int)((tl >> 12) & 0x7FF) - 342 - ox;
  int y0 = (int)(tl & 0x7FF) - 342 - oy;
  int x1 = (int)((br >> 12) & 0x7FF) - 342 - ox + 1;
  int y1 = (int)(br & 0x7FF) - 342 - oy + 1;
  x0 = std::max(x0, 0); y0 = std::max(y0, 0);
  x1 = std::min(x1, kEfbWidth); y1 = std::min(y1, kEfbHeight);
  if (x1 <= x0 || y1 <= y0) {
    CEnable(kEnScissor, GL_SCISSOR_TEST, true);  // empty: nothing passes
    CScissor(0, 0, 0, 0);
    return;
  }
  CEnable(kEnScissor, GL_SCISSOR_TEST, true);
  CScissor(SE(x0), s_efb_h - SE(y1), SE(x1) - SE(x0), SE(y1) - SE(y0));
}

// TX_SETMODE0 (BP 0x80-0x83 for maps 0-3, 0xA0-0xA3 for 4-7): wrap S in bits
// 0-1, wrap T in bits 2-3 (0 clamp, 1 repeat, 2 mirror), mag filter bit 4
// (1 = linear), min filter bits 5-7 (4-6 = linear variants). Cached GL
// textures are shared across draws, so the mode is set on every bind.
static void ApplyTexMode(int map) {
  uint32_t mode = g_state.bp[(map < 4 ? 0x80 : 0xA0) + (map & 3)] & 0xFF;
  // One sampler object per distinct mode, so a texture shared by two maps
  // with different modes samples correctly on both.
  static std::unordered_map<uint32_t, GLuint> s_samplers;
  GLuint &smp = s_samplers[mode];
  if (!smp) {
    auto wrap = [](uint32_t w) -> GLint {
      return w == 1 ? GL_REPEAT : w == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
    };
    glGenSamplers(1, &smp);
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, wrap(mode & 3));
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_T, wrap((mode >> 2) & 3));
    glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER,
                        (mode >> 4) & 1 ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER,
                        ((mode >> 5) & 7) >= 4 ? GL_LINEAR : GL_NEAREST);
  }
  CBindSampler(map, smp);
}

// EFB -> XFB copies land in their own framebuffer, which is what the frontend
// presents: the live EFB is mid-frame scratch space (render-to-texture,
// clears) by the time the host gets to present it.
// The EFB framebuffer and its colour texture (owned by the frontend), read
// once: glGet* calls stall Mesa's GL thread.
static GLuint s_efb_fbo = 0, s_efb_tex = 0;
static GLuint s_xfb_fbo = 0, s_xfb_tex = 0;
// s_xfb_w/h: GL size of the last XFB copy (scaled); s_xfb_lw/lh: its guest
// (native) size. s_xfb_cap_w/h: allocated size of s_xfb_tex.
static int s_xfb_w = 0, s_xfb_h = 0, s_xfb_lw = 0, s_xfb_lh = 0;
static int s_xfb_cap_w = 0, s_xfb_cap_h = 0;
static long s_xfb_copies = 0;   // guest EFB->XFB copies seen
static bool s_xfb_copied = false;  // a copy happened in this Render() batch

static void CopyEfbToXfb() {
  int w = g_state.efbW, h = g_state.efbH;
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
    return;
  const GLuint efb = s_efb_fbo;
  // Scaled source rectangle (r16res: SE edges, so fractional scales too).
  const int sx = SE(g_state.efbSrcX), sy = s_efb_h - SE(g_state.efbSrcY + h);
  const int pw = SE(g_state.efbSrcX + w) - sx, ph = s_efb_h - SE(g_state.efbSrcY) - sy;
  // 1024x1024 as always at native scale; grown when a scaled copy needs more.
  if (!s_xfb_fbo || pw > s_xfb_cap_w || ph > s_xfb_cap_h) {
    s_r2_full_inval = true;  // r39r2: binds a texture on the active unit
    const bool first = !s_xfb_fbo;
    if (first) glGenTextures(1, &s_xfb_tex);
    s_xfb_cap_w = std::max(1024, pw);
    s_xfb_cap_h = std::max(1024, ph);
    glBindTexture(GL_TEXTURE_2D, s_xfb_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_xfb_cap_w, s_xfb_cap_h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    if (first) glGenFramebuffers(1, &s_xfb_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_xfb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           s_xfb_tex, 0);
  }
  glDisable(GL_SCISSOR_TEST);
  g_gl.en[kEnScissor] = 0;  // r39r2
  glBindFramebuffer(GL_READ_FRAMEBUFFER, efb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_xfb_fbo);
  glBlitFramebuffer(sx, sy, sx + pw, sy + ph, 0, 0, pw, ph, GL_COLOR_BUFFER_BIT,
                    GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, efb);
  s_r2_efb_off = false;  // r39r2
  s_xfb_w = pw;
  s_xfb_h = ph;
  s_xfb_lw = w;
  s_xfb_lh = h;

  ++s_xfb_copies;
  s_xfb_copied = true;
  // NWII_XFBDUMP_ALL=1 (with NWII_NOFRAMESKIP=1): dump every XFB copy as it
  // is made, also the ones a batch does not present.
  static const bool s_dump_all = NWII_ENV("NWII_XFBDUMP_ALL") != nullptr;
  if (s_dump_all) {
    void DumpPresentedXfb();
    DumpPresentedXfb();
  }
}

// NWII_DUMP_SCALED=1: NWII_XFBDUMP and F11 write the internal (scaled)
// resolution instead of the native 640-wide frame.
static bool DumpScaled() {
  static const bool on = [] {
    const char *e = NWII_ENV("NWII_DUMP_SCALED");
    return e && e[0] && e[0] != '0';
  }();
  return on;
}

// RGBA rows (any orientation) of w x h -> nw x nh by averaging each source
// block (exact box filter for integer scales).
static void BoxDownsample(std::vector<unsigned char> &px, int w, int h, int nw, int nh) {
  if (nw <= 0 || nh <= 0) return;
  std::vector<unsigned char> out((size_t)nw * nh * 4);
  for (int y = 0; y < nh; ++y) {
    const int y0 = (int)((int64_t)y * h / nh), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * h / nh));
    for (int x = 0; x < nw; ++x) {
      const int x0 = (int)((int64_t)x * w / nw), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * w / nw));
      unsigned acc[4] = {0, 0, 0, 0};
      for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx)
          for (int c = 0; c < 4; ++c) acc[c] += px[((size_t)yy * w + xx) * 4 + c];
      const unsigned n = (unsigned)((y1 - y0) * (x1 - x0));
      for (int c = 0; c < 4; ++c)
        out[((size_t)y * nw + x) * 4 + c] = (unsigned char)((acc[c] + n / 2) / n);
    }
  }
  px.swap(out);
}

// NWII_XFBDUMP=<prefix>: dump the presented XFB about every
// NWII_XFBDUMP_EVERY-th guest XFB copy (default 120) as
// <prefix>_<copy#>.ppm. Only the last copy of a Render() batch is dumped:
// earlier copies in the batch belong to frames whose draws frame-skip dropped
// (they show the bare clear color), and they are never presented.
void DumpPresentedXfb() {
  static const char *dump = NWII_ENV("NWII_XFBDUMP");
  if (!dump || !s_xfb_copied || !s_xfb_w)
    return;
  s_xfb_copied = false;
  static const long every = NWII_ENV("NWII_XFBDUMP_EVERY")
                                ? std::atol(NWII_ENV("NWII_XFBDUMP_EVERY"))
                                : 120;
  // NWII_XFBDUMP_FROM=<n>: start at guest XFB copy n (dense sequences of a
  // late scene without thousands of early dumps).
  static long next = NWII_ENV("NWII_XFBDUMP_FROM")
                         ? std::max(every, std::atol(NWII_ENV("NWII_XFBDUMP_FROM")))
                         : every;
  if (s_xfb_copies < next)
    return;
  while (next <= s_xfb_copies)
    next += every;
  int w = s_xfb_w, h = s_xfb_h;
  std::vector<unsigned char> px((size_t)w * h * 4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_xfb_fbo);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
  // Native size (640 wide) unless NWII_DUMP_SCALED=1, so dumps of any
  // internal resolution compare against each other and older runs.
  if (!DumpScaled() && (w != s_xfb_lw || h != s_xfb_lh)) {
    BoxDownsample(px, w, h, s_xfb_lw, s_xfb_lh);
    w = s_xfb_lw;
    h = s_xfb_lh;
  }
  char path[512];
  std::snprintf(path, sizeof(path), "%s_%06ld.ppm", dump, s_xfb_copies);
  if (FILE *f = std::fopen(path, "wb")) {
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y)
      for (int x = 0; x < w; ++x)
        std::fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
    std::fclose(f);
  }
}

// EFB pixel format (BP 0x43 bits 0-2). Only RGBA6_Z24 (1) keeps alpha: in
// every other format EFB alpha reads back as 1 (Dolphin efb_has_alpha).
static inline uint32_t EfbPixelFormat() { return g_state.bp[0x43] & 7; }
static inline bool EfbHasAlpha() { return EfbPixelFormat() == 1; }

// A copy with the clear bit (BP 0x52 bit 11) clears its source rectangle to
// the clear color (BP 0x4F/0x50) and clear depth (BP 0x51), in stream order.
// Like Dolphin ClearScreen: colour, alpha and Z are cleared only when their
// update bits (BP 0x41 bits 3/4, BP 0x40 bit 4) are on; formats without
// alpha clear it to 0 (it reads back as 1 anyway).
static inline bool DrawTraceOn();
// r39r2 NWII_R2_CLEARTEX_CHECK=1: after each colour-only clear, read the
// rectangle back and count pixels that are not the clear colour (both paths).
static void R2ClearCheck(bool tex_path, int x0, int y0, int x1, int y1, const unsigned char *cc) {
  static const bool on = NWII_ENV("NWII_R2_CLEARTEX_CHECK") != nullptr;
  if (!on || x1 <= x0 || y1 <= y0) return;
  static uint64_t n[2] = {0, 0}, bad[2] = {0, 0}, px[2] = {0, 0};
  std::vector<unsigned char> b((size_t)(x1 - x0) * (y1 - y0) * 4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
  glReadPixels(x0, y0, x1 - x0, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, b.data());
  uint64_t d = 0;
  for (size_t i = 0; i < b.size(); i += 4)
    if (std::memcmp(&b[i], cc, 4)) ++d;
  ++n[tex_path]; bad[tex_path] += d; px[tex_path] += b.size() / 4;
  if ((n[0] + n[1]) % 600 == 0 || (d && bad[tex_path] == d))
    printf("[R2CLR] cleartex: %llu clears %llu px %llu differ | glClear: %llu clears %llu px %llu differ\n",
           (unsigned long long)n[1], (unsigned long long)px[1], (unsigned long long)bad[1],
           (unsigned long long)n[0], (unsigned long long)px[0], (unsigned long long)bad[0]);
}
// r39r2 NWII_R2_COPYTRACE=<n>: every n-th frame, the order of draws (GX scissor
// rectangle, texture addresses) and EFB copies (source rectangle, destination).
static long s_r2ct_every = -1, s_r2ct_frame = 0;
static uint32_t UsedTexMaps();
static bool R2CopyTraceOn() {
  if (s_r2ct_every < 0) {
    const char *e = NWII_ENV("NWII_R2_COPYTRACE");
    s_r2ct_every = e ? std::max(1L, std::atol(e)) : 0;
  }
  return s_r2ct_every && (s_r2ct_frame % s_r2ct_every) == 0;
}
static void R2TraceDraw() {
  const uint32_t tl = g_state.bp[0x20], br = g_state.bp[0x21];
  int ox, oy;
  ScissorOffset(ox, oy);
  printf("[R2CT] D sc=%d,%d,%d,%d tex", (int)((tl >> 12) & 0x7FF) - 342 - ox, (int)(tl & 0x7FF) - 342 - oy,
         (int)((br >> 12) & 0x7FF) - 342 - ox + 1, (int)(br & 0x7FF) - 342 - oy + 1);
  const uint32_t used = UsedTexMaps();
  for (int i = 0; i < 8; ++i)
    if ((used >> i) & 1) printf(" %08X", g_state.texStages[i].base_addr);
  printf("\n");
}
static void ClearEfbCopySource() {
  g_state.pe_clear_pending = false;
  const uint32_t pf = EfbPixelFormat();
  bool c = (g_state.bp[0x41] >> 3) & 1;
  bool a = (g_state.bp[0x41] >> 4) & 1;
  const bool z = (g_state.bp[0x40] >> 4) & 1;
  if (pf != 1)
    a = false;
  static const bool s_legacy = NWII_ENV("NWII_CLEARALL") != nullptr;
  if (s_legacy)
    c = a = true;
  if (!c && !a && !z && !s_legacy)
    return;
  unsigned char cc[4];
  ::GX_GetClearColor(cc);
  if (pf != 1) {
    a = true;  // Dolphin ClearEFB: force the alpha write, value 0
    cc[3] = 0;
  }
  int w = g_state.efbW, h = g_state.efbH;
  // r39r2 NWII_R2_CLEARTEX=1: a colour-only clear with the full colour mask
  // (the animated-tile copies: ~25 a frame) is glClearTexSubImage on the EFB
  // texture over the same rectangle the scissor would leave, with the clear
  // colour as RGBA bytes (the EFB is RGBA8: the same bytes glClear writes).
  // Mesa 20.1 does a scissored glClear as a quad draw that saves and restores
  // the whole pipeline state; nouveau clears a texture box with the 3D
  // engine's clear (pipe->clear_texture).
  {
    typedef void (*PFNClearTexSub)(GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei,
                                   GLenum, GLenum, const void *);
    static PFNClearTexSub s_cts = [] {
      const char *e = NWII_ENV("NWII_R2_CLEARTEX");
      if (!(e && e[0] == '1')) return (PFNClearTexSub) nullptr;
      PFNClearTexSub f = (PFNClearTexSub)GX_GLProc("glClearTexSubImage");
      printf("[R2] NWII_R2_CLEARTEX=1: glClearTexSubImage %s\n", f ? "on" : "NOT available (glClear)");
      return f;
    }();
    if (s_cts && c && a && !z && !s_legacy && !DrawTraceOn() && !s_r8_pass) {
      int x0 = SE(g_state.efbSrcX), x1 = SE(g_state.efbSrcX + w);
      int y0 = s_efb_h - SE(g_state.efbSrcY + h), y1 = s_efb_h - SE(g_state.efbSrcY);
      x0 = std::max(x0, 0); y0 = std::max(y0, 0);
      x1 = std::min(x1, s_efb_w); y1 = std::min(y1, s_efb_h);
      if (x1 > x0 && y1 > y0)
        s_cts(s_efb_tex, 0, x0, y0, 0, x1 - x0, y1 - y0, 1, GL_RGBA, GL_UNSIGNED_BYTE, cc);
      R2ClearCheck(true, x0, y0, x1, y1, cc);
      return;
    }
  }
  R2EfbBack();  // r39r2 NWII_R2_LAZYEFB: glClear clears the draw framebuffer
  glEnable(GL_SCISSOR_TEST);
  glScissor(SE(g_state.efbSrcX), s_efb_h - SE(g_state.efbSrcY + h),
            SE(g_state.efbSrcX + w) - SE(g_state.efbSrcX),
            SE(g_state.efbSrcY + h) - SE(g_state.efbSrcY));
  glClearColor(cc[0] / 255.f, cc[1] / 255.f, cc[2] / 255.f, cc[3] / 255.f);
  glClearDepth((g_state.bp[0x51] & 0xFFFFFF) / 16777215.0);
  glColorMask(c, c, c, a);
  glDepthMask((z || s_legacy) ? GL_TRUE : GL_FALSE);
  if (NWII_ENV("NWII_DRAWSTATS")) {
    static int n = 0;
    if (n++ % 600 == 0)
      printf("[DS] copy-clear z=%06X color=%02X%02X%02X%02X c=%d a=%d z=%d pf=%u\n",
             g_state.bp[0x51] & 0xFFFFFF, cc[0], cc[1], cc[2], cc[3], c, a, z,
             pf);
  }
  glClear(((c || a) ? GL_COLOR_BUFFER_BIT : 0) |
          ((z || s_legacy) ? GL_DEPTH_BUFFER_BIT : 0));
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glDisable(GL_SCISSOR_TEST);
  // r39r2 NWII_R2_COPYST: what this clear left set, in the state cache
  g_gl.en[kEnScissor] = 0;
  std::memset(g_gl.sc, 0xFF, sizeof g_gl.sc);
  g_gl.color_mask = 3;
  g_gl.depth_mask = 1;
  if (c && a && !z && !s_legacy)  // r39r2 check (reference path)
    R2ClearCheck(false, std::max(SE(g_state.efbSrcX), 0), std::max(s_efb_h - SE(g_state.efbSrcY + h), 0),
                 std::min(SE(g_state.efbSrcX + w), s_efb_w), std::min(s_efb_h - SE(g_state.efbSrcY), s_efb_h), cc);
}

// Real EFB copy format: BP 0x52 bits 3-6 hold it rotated (Dolphin
// UPE_Copy::tp_realFormat). 0 R4, 1 R8, 2 RA4, 3 RA8, 4 RGB565, 5 RGB5A3,
// 6 RGBA8, 7 A8, 8 R8, 9 G8, 10 B8, 11 RG8, 12 GB8.
static inline uint32_t CopyRealFormat(uint32_t val) {
  const uint32_t f = (val >> 3) & 0xF;
  return f / 2 + (f & 1) * 8;
}

// Texture maps referenced by the active TEV stages (and indirect stages):
// stale maps are neither decoded nor bound.
static uint32_t UsedTexMaps() {
  uint32_t used = 0;
  const int n = g_state.numTevStages ? g_state.numTevStages : 1;
  for (int i = 0; i < n && i < 16; ++i)
    if (g_state.tevStages[i].texMap != 0xFF)
      used |= 1u << (g_state.tevStages[i].texMap & 7);
  const uint32_t nind = (g_state.bp[0x00] >> 16) & 7;
  for (uint32_t k = 0; k < nind && k < 4; ++k)
    used |= 1u << ((g_state.bp[0x27] >> (6 * k)) & 7);
  return used;
}

// EFB -> texture copy shader (Dolphin TextureConverterShaderGen, colour
// copies): box-filtered half scale, intensity (YUV) conversion, per-format
// channel selection, alpha forced to 1 when the EFB has none. Destination
// row 0 is the top GX row of the source rectangle.
static GLuint s_copy_prog = 0, s_copy_vao = 0, s_copy_fbo = 0;
static GLint s_cp_src = -1, s_cp_half = -1, s_cp_fmt = -1, s_cp_int = -1,
             s_cp_alpha = -1, s_cp_efb = -1, s_cp_down = -1;

static void InitCopyShader() {
  if (s_copy_prog)
    return;
  static const char *vs =
      "#version 330 core\n"
      "void main() {\n"
      "  vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0, (gl_VertexID & 2) * 2.0 - 1.0);\n"
      "  gl_Position = vec4(p, 0.0, 1.0);\n"
      "}\n";
  static const char *fs =
      "#version 330 core\n"
      "uniform sampler2D uEfb;\n"
      "uniform ivec3 uSrc;   // x, top GX row, EFB height\n"
      "uniform int uHalf, uFmt, uIntensity, uHasAlpha;\n"
      "uniform float uDown;  // r16res: >0 = native-size copy from an EFB at this scale\n"
      "out vec4 o;\n"
      "vec4 fetch(int x, int gy) {\n"
      "  vec4 c = texelFetch(uEfb, ivec2(x, uSrc.z - 1 - gy), 0);\n"
      "  if (uHasAlpha == 0) c.a = 1.0;\n"
      "  return c;\n"
      "}\n"
      "int SE(int x) { return int(ceil(float(x) * uDown - 0.5)); }\n"
      "void main() {\n"
      "  ivec2 d = ivec2(gl_FragCoord.xy);\n"
      "  vec4 c;\n"
      "  if (uDown > 0.0) {\n"
      "    // uSrc.xy native: average the scaled pixels of the 1 (2x2 if half)\n"
      "    // native source pixels of this texel (renderer_gl.cpp SE rule).\n"
      "    int n = uHalf != 0 ? 2 : 1;\n"
      "    int gx = uSrc.x + n * d.x, gy = uSrc.y + n * d.y;\n"
      "    int x0 = SE(gx), x1 = max(SE(gx + n), x0 + 1);\n"
      "    int y0 = SE(gy), y1 = max(SE(gy + n), y0 + 1);\n"
      "    c = vec4(0.0);\n"
      "    for (int y = y0; y < y1; ++y)\n"
      "      for (int x = x0; x < x1; ++x) c += fetch(x, y);\n"
      "    c /= float((x1 - x0) * (y1 - y0));\n"
      "  } else if (uHalf != 0) {\n"
      "    int x = uSrc.x + 2 * d.x, y = uSrc.y + 2 * d.y;\n"
      "    c = 0.25 * (fetch(x, y) + fetch(x + 1, y) + fetch(x, y + 1) + fetch(x + 1, y + 1));\n"
      "  } else {\n"
      "    c = fetch(uSrc.x + d.x, uSrc.y + d.y);\n"
      "  }\n"
      "  if (uIntensity != 0 && uFmt <= 3) {\n"
      "    float yv = clamp(dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0, 0.0, 1.0);\n"
      "    c = vec4(yv, yv, yv, (uFmt >= 2) ? c.a : yv);\n"
      "  } else if (uFmt == 0 || uFmt == 1 || uFmt == 8) {\n"
      "    c = c.rrrr;\n"
      "  } else if (uFmt == 2 || uFmt == 3) {\n"
      "    c = c.rrra;\n"
      "  } else if (uFmt == 4) {\n"
      "    c = vec4(c.rgb, 1.0);\n"
      "  } else if (uFmt == 7) {\n"
      "    c = c.aaaa;\n"
      "  } else if (uFmt == 9) {\n"
      "    c = c.gggg;\n"
      "  } else if (uFmt == 10) {\n"
      "    c = c.bbbb;\n"
      "  } else if (uFmt == 11) {\n"
      "    c = c.rrrg;\n"
      "  } else if (uFmt == 12) {\n"
      "    c = c.gggb;\n"
      "  }\n"
      "  o = c;\n"
      "}\n";
  s_copy_prog = CompileShader(vs, fs);
  s_cp_src = glGetUniformLocation(s_copy_prog, "uSrc");
  s_cp_half = glGetUniformLocation(s_copy_prog, "uHalf");
  s_cp_fmt = glGetUniformLocation(s_copy_prog, "uFmt");
  s_cp_int = glGetUniformLocation(s_copy_prog, "uIntensity");
  s_cp_alpha = glGetUniformLocation(s_copy_prog, "uHasAlpha");
  s_cp_efb = glGetUniformLocation(s_copy_prog, "uEfb");
  s_cp_down = glGetUniformLocation(s_copy_prog, "uDown");
  glGenVertexArrays(1, &s_copy_vao);
  glGenFramebuffers(1, &s_copy_fbo);
}

// Set when the renderer changed GL state behind SetupDrawState's back (copy
// shader, clears): the next draw re-applies everything.
static bool s_gl_state_dirty = true;
// r39r2 NWII_R2_COPYST=1: EFB copies, clears and XFB copies go through the GL
// state cache (C* setters) or update it with what they changed, instead of
// forgetting the whole cache after each copy (GLInvalidate): the first draw
// after a copy then re-issues only the state that really differs.
static bool R2CopySt() {
  static const bool on = [] {
    const char *e = NWII_ENV("NWII_R2_COPYST");
    const bool v = e && e[0] == '1';
    if (v) printf("[R2] NWII_R2_COPYST=1: GL state cache kept across EFB copies\n");
    return v;
  }();
  return on && !s_r8_pass;
}


static inline bool DrawTraceOn();
}  // namespace nwii::runtime::gx
namespace nwii { namespace runtime { namespace hle {
bool r2_copyfbo_on();                       // r39r2 (texture_cache_gl.cpp)
extern bool g_r2_off_pass;
GLuint r2_tex_fbo(GLuint tex, GLenum target);
bool r2_lazynat_on();
void r2_native_defer(GLuint nat, GLuint src, uint32_t pw, uint32_t ph, uint32_t dw, uint32_t dh);
void r2_native_made(GLuint nat);
GLuint r2_mrt_fbo(const GLuint *tex, int n);
}}}
namespace nwii::runtime::gx {
static struct { GLint v[8]; float down; bool ok; } s_copy_cu = {{0}, 0.0f, false};  // r39r2 COPYUNI cache
static void CopyEfbToTexture(uint32_t val) {
  uint32_t dest = g_state.efbCopyDest;
  uint32_t w = g_state.efbW, h = g_state.efbH;
  // Dolphin BPStructs: skip copies starting outside the EFB, clamp the rest.
  if (g_state.efbSrcX >= kEfbWidth || g_state.efbSrcY >= kEfbHeight)
    return;
  w = std::min<uint32_t>(w, kEfbWidth - g_state.efbSrcX);
  h = std::min<uint32_t>(h, kEfbHeight - g_state.efbSrcY);
  if (!dest || w == 0 || h == 0 || w > 1024 || h > 1024)
    return;
  const bool half = (val >> 9) & 1;
  const uint32_t dw = half ? std::max<uint32_t>(1, w / 2) : w;
  const uint32_t dh = half ? std::max<uint32_t>(1, h / 2) : h;
  const uint32_t fmt = CopyRealFormat(val);
  const bool intensity = ((val >> 15) & 1) && ((val >> 16) & 1);
  if (EfbPixelFormat() == 3) {
    static int warned = 0;
    if (!warned++)
      printf("[GX] EFB depth copy (pixel format Z24) not emulated; copying colour\n");
  }

  const GLuint efb = s_efb_fbo, efb_tex = s_efb_tex;
  // Scaled internal resolution: the copy is made at dw*s x dh*s from the
  // scaled source rectangle (the shader's half-scale box filter and format
  // conversion work per GL pixel, unchanged); the texture cache keeps dw x dh
  // as the copy's guest size.
  // r16res: at a fractional scale the copy is the source rectangle's scaled
  // pixels 1:1 (SE edges), halved (floor) for half-scale copies.
  // Copies the texture cache stitched into a RAM texture before (animated
  // tiles: ~20 32x32 copies a frame in the title demo) are only ever used at
  // native size: at any scale above 1 they are made native directly (one
  // box-filter pass) instead of scaled + a downsample blit.
  const int sx0 = SE(g_state.efbSrcX), sy0 = SE(g_state.efbSrcY);
  const bool down = s_scale != 1.0f &&
                    nwii::runtime::hle::TextureCache::get().efb_copy_stitched(dest);
  uint32_t pw, ph;
  if (down) {
    pw = dw;
    ph = dh;
  } else if (s_iscale) {
    pw = dw * (uint32_t)s_iscale;
    ph = dh * (uint32_t)s_iscale;
  } else {
    pw = (uint32_t)(SE(g_state.efbSrcX + (int)w) - sx0);
    ph = (uint32_t)(SE(g_state.efbSrcY + (int)h) - sy0);
    if (half) { pw = std::max<uint32_t>(1, pw / 2); ph = std::max<uint32_t>(1, ph / 2); }
  }
  uint64_t ct = rph::T();
  GLuint tex = nwii::runtime::hle::TextureCache::get().acquire_efb_copy(dest, dw, dh, pw, ph);
  rph::C(rph::cAcq, ct);
  InitCopyShader();
  const bool r2fbo = nwii::runtime::hle::r2_copyfbo_on();  // r39r2 NWII_R2_COPYFBO
  if (r2fbo) {
    nwii::runtime::hle::r2_tex_fbo(tex, GL_DRAW_FRAMEBUFFER);
  } else {
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_copy_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         tex, 0);
  }
  const bool r2st = R2CopySt();
  if (r2st) {
    CViewport(0, 0, pw, ph);
    CEnable(kEnScissor, GL_SCISSOR_TEST, false);
    CEnable(kEnBlend, GL_BLEND, false);
    CEnable(kEnDepth, GL_DEPTH_TEST, false);
    CEnable(kEnCull, GL_CULL_FACE, false);
    CEnable(kEnLogic, GL_COLOR_LOGIC_OP, false);
    CColorMask(true, true);
    CUseProgram(s_copy_prog);
  } else {
  glViewport(0, 0, pw, ph);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_COLOR_LOGIC_OP);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glUseProgram(s_copy_prog);
  }
  // r39r2 NWII_R2_COPYUNI=1: the copy program keeps its uniforms between
  // copies; only the ones whose value changed are set (same values).
  static const bool s_cu_on = [] { const char *e = NWII_ENV("NWII_R2_COPYUNI"); return e && e[0] == '1'; }();
  auto &s_cu = s_copy_cu;  // r45p: file scope (NWII_P_MRT_CHECK also uses the program)
  {
    const GLint sx = down ? (GLint)g_state.efbSrcX : sx0, sy = down ? (GLint)g_state.efbSrcY : sy0;
    const GLint nv[8] = {sx, sy, s_efb_h, (GLint)half, (GLint)fmt, (GLint)intensity, (GLint)EfbHasAlpha(), 0};
    const float nd = down ? s_scale : 0.0f;
    const bool all = !s_cu_on || !s_cu.ok;
    if (all || nv[0] != s_cu.v[0] || nv[1] != s_cu.v[1] || nv[2] != s_cu.v[2]) glUniform3i(s_cp_src, nv[0], nv[1], nv[2]);
    if (all || nd != s_cu.down) glUniform1f(s_cp_down, nd);
    if (all || nv[3] != s_cu.v[3]) glUniform1i(s_cp_half, nv[3]);
    if (all || nv[4] != s_cu.v[4]) glUniform1i(s_cp_fmt, nv[4]);
    if (all || nv[5] != s_cu.v[5]) glUniform1i(s_cp_int, nv[5]);
    if (all || nv[6] != s_cu.v[6]) glUniform1i(s_cp_alpha, nv[6]);
    if (all) glUniform1i(s_cp_efb, 0);
    std::memcpy(s_cu.v, nv, sizeof nv);
    s_cu.down = nd;
    s_cu.ok = true;
  }
  if (r2st) {
    g_gl.unit = -1;  // the texture cache may have moved the active unit (scratch unit)
    CBindTexture(0, (GLuint)efb_tex);
    CBindVertexArray(s_copy_vao);
  } else {
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, (GLuint)efb_tex);
  glBindVertexArray(s_copy_vao);
  }
  rph::C(rph::cSetup, ct);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  if (r2st) CBindTexture(0, 0);
  else glBindTexture(GL_TEXTURE_2D, 0);
  rph::C(rph::cDraw, ct);
  // NWII_COPYDUMP=<dir> (with NWII_DRAWTRACE): EFB copies of at least 64x64
  // made in the traced frames, as <dir>/c<frame>_<dest>.pam (RGBA).
  if (const char *cd = NWII_ENV("NWII_COPYDUMP")) {
    if (DrawTraceOn() && dw >= 64 && dh >= 64) {
      std::vector<unsigned char> px((size_t)pw * ph * 4);
      if (r2fbo) nwii::runtime::hle::r2_tex_fbo(tex, GL_READ_FRAMEBUFFER);
      else glBindFramebuffer(GL_READ_FRAMEBUFFER, s_copy_fbo);
      glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      char path[512];
      std::snprintf(path, sizeof path, "%s/c%06ld_%08X.pam", cd, s_xfb_copies + 1, dest);
      if (FILE *f = std::fopen(path, "wb")) {
        std::fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", pw, ph);
        std::fwrite(px.data(), 1, px.size(), f);
        std::fclose(f);
      }
    }
  }
  if (pw != dw || ph != dh) {
    // Native-size downsample for stitching into RAM-decoded textures (tile
    // animation frames copied into a tileset). Bilinear at an exact 2:1 is a
    // 2x2 box; scissor is off and the colour mask fully on here.
    static GLuint s_native_fbo = 0;
    if (!s_native_fbo) glGenFramebuffers(1, &s_native_fbo);
    GLuint nat = nwii::runtime::hle::TextureCache::get().efb_copy_native(dest);
    if (nat && nwii::runtime::hle::r2_lazynat_on()) {  // r39r2 NWII_R2_LAZYNAT: on demand
      nwii::runtime::hle::r2_native_defer(nat, tex, pw, ph, dw, dh);
      nat = 0;
    } else if (nat) {
      nwii::runtime::hle::r2_native_made(nat);
    }
    if (nat) {
      if (r2fbo) {
        nwii::runtime::hle::r2_tex_fbo(tex, GL_READ_FRAMEBUFFER);
        nwii::runtime::hle::r2_tex_fbo(nat, GL_DRAW_FRAMEBUFFER);
      } else {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, s_copy_fbo);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_native_fbo);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, nat, 0);
      }
      glBlitFramebuffer(0, 0, pw, ph, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
  }
  static const bool s_lazyefb = [] {
    const char *e = NWII_ENV("NWII_R2_LAZYEFB");
    const bool v = e && e[0] == '1';
    if (v) printf("[R2] NWII_R2_LAZYEFB=1: EFB rebound as draw target on demand after copies\n");
    return v;
  }();
  if (s_lazyefb && !s_r8_pass && !DrawTraceOn()) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, efb);
    s_r2_efb_fbo_ref = efb;
    s_r2_efb_off = true;
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, efb);
    s_r2_efb_off = false;
  }
  s_gl_state_dirty = true;
  rph::C(rph::cBack, ct);
  nwii::runtime::hle::TextureCache::get().efb_copy_done(
      g_ctx_ptr, dest, dw, dh, fmt, (g_state.bp[0x4D] & 0x3FF) << 5);
  rph::C(rph::cDone, ct);
  if (rph::On()) ++rph::s_ncopy;
}

// ---- r45p NWII_P_RUNSTAT=1: entre dos draws seguidos, ¿qué órdenes hay? ----------
// Clases del hueco: nada (draw tras draw), solo cargas de matrices XF (0x000-0x0FF posición/textura,
// 0x400 normal, 0x500 post), otras escrituras XF, BP, CP, copia EFB. Por fotograma: draws, huecos de
// cada clase (la más «fuerte» que contenga), y draws cuyo hueco anterior solo tuvo matrices.
static bool s_p_runstat = false, s_p_stalelog = false;  // r45p: read once (RenderOnce's InitFlags), no guard per command
static inline bool PRunStatOn() { return s_p_runstat; }
static uint32_t s_prs_cls = 0;  // bits del hueco actual: 1 mtx, 2 xf otro, 4 bp, 8 cp, 16 copia, 32 otro
static bool s_prs_seen = false;
static uint64_t st_prs_draws = 0, st_prs_frames = 0, st_prs_gap[7] = {};
static uint64_t st_prs_bpregs[256] = {};
static void PRunStatCmd(const GXCommand &c) {
  if (c.type == GXCommandType::DrawPrimitive) {
    ++st_prs_draws;
    if (s_prs_seen) {
      int k = 0;
      if (s_prs_cls == 0) k = 0;
      else if (s_prs_cls == 1) k = 1;
      else if (s_prs_cls & 16) k = 6;
      else if (s_prs_cls & 4) k = 4;
      else if (s_prs_cls & 8) k = 5;
      else if (s_prs_cls & 2) k = 2;
      else k = 3;
      ++st_prs_gap[k];
    }
    s_prs_seen = true;
    s_prs_cls = 0;
    return;
  }
  if (c.type == GXCommandType::XFRegister) {
    const int a = (int)c.reg;
    if (a < 0x100 || (a >= 0x400 && a < 0x460) || (a >= 0x500 && a < 0x600)) s_prs_cls |= 1;
    else s_prs_cls |= 2;
  } else if (c.type == GXCommandType::BPRegister) {
    s_prs_cls |= (c.reg == 0x52) ? 16 : 4;
    if (c.reg != 0x52) ++st_prs_bpregs[c.reg & 0xFF];
  } else if (c.type == GXCommandType::CPRegister) {
    s_prs_cls |= 8;
  } else {
    s_prs_cls |= 32;
  }
}
static void PRunStatFrame() {
  s_prs_seen = false;
  s_prs_cls = 0;
  if (++st_prs_frames < 120) return;
  const double f = (double)st_prs_frames;
  printf("[P45RUN] por fotograma: draws %.0f | hueco antes del draw: nada %.0f, solo matrices %.0f, otro XF %.0f, "
         "otro %.0f, BP %.0f, CP %.0f, copia %.0f | BP mas frecuentes:",
         st_prs_draws / f, st_prs_gap[0] / f, st_prs_gap[1] / f, st_prs_gap[2] / f, st_prs_gap[3] / f, st_prs_gap[4] / f,
         st_prs_gap[5] / f, st_prs_gap[6] / f);
  for (int t = 0; t < 8; ++t) {
    int best = -1;
    for (int r = 0; r < 256; ++r)
      if (st_prs_bpregs[r] && (best < 0 || st_prs_bpregs[r] > st_prs_bpregs[best])) best = r;
    if (best < 0) break;
    printf(" %02X:%.0f", best, st_prs_bpregs[best] / f);
    st_prs_bpregs[best] = 0;
  }
  printf("\n");
  st_prs_draws = st_prs_frames = 0;
  for (auto &g : st_prs_gap) g = 0;
  for (auto &b : st_prs_bpregs) b = 0;
}

// ---- r39r2 NWII_R2_COPYMRT=1: EFB copies made together -----------------------
// The ~20 animated-tile copies of a frame come one after another with no draw
// between them. A copy that needs nothing but the plain path (scaled, not
// half-size, no intensity conversion, not stitched, clear colour-only or none)
// is queued; the queue is made before the next draw, the next other copy or
// clear, or at the end of the frame: up to 8 copies of the same size and
// format in ONE draw into 8 colour attachments (each output is the same
// fetch + channel selection the one-copy shader does: no arithmetic), then, in
// order, each copy's bookkeeping (efb_copy_done) and its clear
// (glClearTexSubImage). Queued copies have different destinations and no
// queued clear covers a later copy's source (else the queue is made first),
// so every copy reads what it read before.
struct R2PendCopy {
  uint32_t dest, dw, dh, pw, ph, fmt, stride;
  int sx0, sy0;          // scaled source origin (GL x, GX row)
  int sr[4];             // source rectangle, GL coords (x0, y0, x1, y1)
  bool alpha, clear;
  unsigned char cc[4];
  int cr[4];             // clear rectangle, GL coords
  GLuint tex;
  GLuint chk = 0;        // r45p NWII_P_MRT_CHECK: the same copy made at queue time by the one-copy shader
  bool intensity = false;
};
// r45p NWII_P_MRT_CHECK=1: every queued copy is ALSO made at once (queue time, the
// EFB as the old path reads it) with the one-copy program into a scratch texture;
// when the queue is made, each MRT output is read back and compared byte by byte.
static bool PMrtCheckOn() {
  static const bool on = [] {
    const char *e = NWII_ENV("NWII_P_MRT_CHECK");
    const bool v = e && e[0] == '1';
    if (v) printf("[P45] NWII_P_MRT_CHECK=1: cada copia de la cola MRT comparada con la copia de una en una\n");
    return v;
  }();
  return on;
}
static uint64_t st_pmc_copies = 0, st_pmc_bad = 0, st_pmc_px = 0;
static GLuint PMrtCheckSnap(const struct R2PendCopy &p);
static void PMrtCheckCompare(const struct R2PendCopy &p);
static std::vector<R2PendCopy> s_r2_pend;
static GLuint s_copy8_prog = 0;
static GLint s_c8_src = -1, s_c8_h = -1, s_c8_fmt = -1, s_c8_alpha = -1, s_c8_efb = -1;
static bool R2MrtOn() {
  static const bool on = [] {
    const char *e = NWII_ENV("NWII_R2_COPYMRT");
    auto has = [](const char *n) { const char *v = std::getenv(n); return v && v[0] == '1'; };
    const bool want = e && e[0] == '1';
    const bool deps = has("NWII_R2_COPYFBO") && has("NWII_R2_COPYST") && has("NWII_R2_LAZYEFB") &&
                      has("NWII_R2_LAZYNAT") && has("NWII_R2_CLEARTEX") && !NWII_ENV("NWII_COPYDUMP") &&
                      !NWII_ENV("NWII_CLEARALL");
    if (want) printf("[R2] NWII_R2_COPYMRT=1: plain EFB copies made together%s\n",
                     deps ? "" : " -- NOT used (needs COPYFBO, COPYST, LAZYEFB, LAZYNAT, CLEARTEX)");
    return want && deps;
  }();
  return on;
}
static void InitCopy8Shader() {
  if (s_copy8_prog) return;
  static const char *vs =
      "#version 330 core\n"
      "void main() {\n"
      "  vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0, (gl_VertexID & 2) * 2.0 - 1.0);\n"
      "  gl_Position = vec4(p, 0.0, 1.0);\n"
      "}\n";
  std::string fs =
      "#version 330 core\n"
      "uniform sampler2D uEfb;\n"
      "uniform ivec2 uSrc8[8];\n"
      "uniform int uEfbH, uFmt, uHasAlpha;\n";
  for (int i = 0; i < 8; ++i) fs += "layout(location = " + std::to_string(i) + ") out vec4 o" + std::to_string(i) + ";\n";
  fs +=
      "vec4 fetch(int x, int gy) {\n"
      "  vec4 c = texelFetch(uEfb, ivec2(x, uEfbH - 1 - gy), 0);\n"
      "  if (uHasAlpha == 0) c.a = 1.0;\n"
      "  return c;\n"
      "}\n"
      "vec4 conv(vec4 c) {\n"
      "  if (uFmt == 0 || uFmt == 1 || uFmt == 8) {\n"
      "    c = c.rrrr;\n"
      "  } else if (uFmt == 2 || uFmt == 3) {\n"
      "    c = c.rrra;\n"
      "  } else if (uFmt == 4) {\n"
      "    c = vec4(c.rgb, 1.0);\n"
      "  } else if (uFmt == 7) {\n"
      "    c = c.aaaa;\n"
      "  } else if (uFmt == 9) {\n"
      "    c = c.gggg;\n"
      "  } else if (uFmt == 10) {\n"
      "    c = c.bbbb;\n"
      "  } else if (uFmt == 11) {\n"
      "    c = c.rrrg;\n"
      "  } else if (uFmt == 12) {\n"
      "    c = c.gggb;\n"
      "  }\n"
      "  return c;\n"
      "}\n"
      "void main() {\n"
      "  ivec2 d = ivec2(gl_FragCoord.xy);\n";
  for (int i = 0; i < 8; ++i)
    fs += "  o" + std::to_string(i) + " = conv(fetch(uSrc8[" + std::to_string(i) + "].x + d.x, uSrc8[" +
          std::to_string(i) + "].y + d.y));\n";
  fs += "}\n";
  s_copy8_prog = CompileShader(vs, fs.c_str());
  s_c8_src = glGetUniformLocation(s_copy8_prog, "uSrc8[0]");
  s_c8_h = glGetUniformLocation(s_copy8_prog, "uEfbH");
  s_c8_fmt = glGetUniformLocation(s_copy8_prog, "uFmt");
  s_c8_alpha = glGetUniformLocation(s_copy8_prog, "uHasAlpha");
  s_c8_efb = glGetUniformLocation(s_copy8_prog, "uEfb");
  printf("[R2] copy8 program %u\n", s_copy8_prog);
}
static inline bool R2RectsMeet(const int a[4], const int b[4]) {
  return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}
static void R2FlushCopies() {
  if (s_r2_pend.empty()) return;
  const int r2ph0 = rph::Set(rph::kCopy);
  auto &tc = nwii::runtime::hle::TextureCache::get();
  for (auto &p : s_r2_pend) p.tex = tc.acquire_efb_copy(p.dest, p.dw, p.dh, p.pw, p.ph);
  InitCopyShader();
  InitCopy8Shader();
  CEnable(kEnScissor, GL_SCISSOR_TEST, false);
  CEnable(kEnBlend, GL_BLEND, false);
  CEnable(kEnDepth, GL_DEPTH_TEST, false);
  CEnable(kEnCull, GL_CULL_FACE, false);
  CEnable(kEnLogic, GL_COLOR_LOGIC_OP, false);
  CColorMask(true, true);
  CUseProgram(s_copy8_prog);
  g_gl.unit = -1;
  CBindTexture(0, s_efb_tex);
  CBindVertexArray(s_copy_vao);
  static bool s_once = false;
  if (!s_once) { s_once = true; glUniform1i(s_c8_efb, 0); }
  glUniform1i(s_c8_h, s_efb_h);
  const size_t n = s_r2_pend.size();
  for (size_t i = 0; i < n;) {
    const R2PendCopy &a = s_r2_pend[i];
    size_t j = i + 1;
    while (j < n && j - i < 8 && s_r2_pend[j].pw == a.pw && s_r2_pend[j].ph == a.ph &&
           s_r2_pend[j].fmt == a.fmt && s_r2_pend[j].alpha == a.alpha)
      ++j;
    GLuint tex[8];
    GLint src[16];
    for (size_t k = i; k < j; ++k) {
      tex[k - i] = s_r2_pend[k].tex;
      src[2 * (k - i)] = s_r2_pend[k].sx0;
      src[2 * (k - i) + 1] = s_r2_pend[k].sy0;
    }
    for (size_t k = j - i; k < 8; ++k) { src[2 * k] = src[0]; src[2 * k + 1] = src[1]; }
    nwii::runtime::hle::r2_mrt_fbo(tex, (int)(j - i));
    CViewport(0, 0, (GLsizei)a.pw, (GLsizei)a.ph);
    glUniform2iv(s_c8_src, 8, src);
    glUniform1i(s_c8_fmt, (GLint)a.fmt);
    glUniform1i(s_c8_alpha, a.alpha ? 1 : 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    i = j;
  }
  CBindTexture(0, 0);
  if (PMrtCheckOn())  // r45p
    for (auto &p : s_r2_pend)
      if (p.chk) {
        PMrtCheckCompare(p);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);  // detach
        glDeleteTextures(1, &p.chk);
        p.chk = 0;
      }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
  s_r2_efb_fbo_ref = s_efb_fbo;
  s_r2_efb_off = true;
  typedef void (*PFNClearTexSub)(GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum,
                                 GLenum, const void *);
  static PFNClearTexSub s_cts2 = (PFNClearTexSub)GX_GLProc("glClearTexSubImage");
  for (auto &p : s_r2_pend) {
    if (p.pw != p.dw || p.ph != p.dh)  // scaled: its native downsample on demand (LAZYNAT)
      if (GLuint nat = tc.efb_copy_native(p.dest)) nwii::runtime::hle::r2_native_defer(nat, p.tex, p.pw, p.ph, p.dw, p.dh);
    tc.efb_copy_done(g_ctx_ptr, p.dest, p.dw, p.dh, p.fmt, p.stride);
    if (rph::On()) { ++rph::s_ncopy; if (p.clear) ++rph::s_nclear; }
  }
  // The clears, in order; a clear that continues the previous one on the same
  // rows with the same colour (the tiles' 32x32 cells side by side) joins it:
  // the same pixels get the same bytes.
  int run[4] = {0, 0, 0, 0};
  unsigned char rcc[4] = {0, 0, 0, 0};
  bool have = false;
  auto emit = [&]() {
    if (have && s_cts2 && run[2] > run[0] && run[3] > run[1])
      s_cts2(s_efb_tex, 0, run[0], run[1], 0, run[2] - run[0], run[3] - run[1], 1, GL_RGBA, GL_UNSIGNED_BYTE, rcc);
    have = false;
  };
  for (auto &p : s_r2_pend) {
    if (!p.clear || p.cr[2] <= p.cr[0] || p.cr[3] <= p.cr[1]) continue;
    if (have && p.cr[1] == run[1] && p.cr[3] == run[3] && p.cr[0] == run[2] && !std::memcmp(p.cc, rcc, 4)) {
      run[2] = p.cr[2];
      continue;
    }
    emit();
    std::memcpy(run, p.cr, sizeof run);
    std::memcpy(rcc, p.cc, 4);
    have = true;
  }
  emit();
  s_r2_pend.clear();
  rph::Set(r2ph0);
}
static GLuint PMrtCheckSnap(const R2PendCopy &p) {
  GLuint t = 0;
  glGenTextures(1, &t);
  CActiveUnit(0);
  glBindTexture(GL_TEXTURE_2D, t);
  g_gl.tex[0] = t;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)p.pw, (GLsizei)p.ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  CBindTexture(0, 0);
  InitCopyShader();
  // own framebuffer (r2_tex_fbo keys its cache by texture name: these names are deleted and reused)
  static GLuint s_pmc_fbo = 0;
  if (!s_pmc_fbo) glGenFramebuffers(1, &s_pmc_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_pmc_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
  CViewport(0, 0, (GLsizei)p.pw, (GLsizei)p.ph);
  CEnable(kEnScissor, GL_SCISSOR_TEST, false);
  CEnable(kEnBlend, GL_BLEND, false);
  CEnable(kEnDepth, GL_DEPTH_TEST, false);
  CEnable(kEnCull, GL_CULL_FACE, false);
  CEnable(kEnLogic, GL_COLOR_LOGIC_OP, false);
  CColorMask(true, true);
  CUseProgram(s_copy_prog);
  glUniform3i(s_cp_src, p.sx0, p.sy0, s_efb_h);
  glUniform1f(s_cp_down, 0.0f);
  glUniform1i(s_cp_half, 0);
  glUniform1i(s_cp_fmt, (GLint)p.fmt);
  glUniform1i(s_cp_int, 0);
  glUniform1i(s_cp_alpha, p.alpha ? 1 : 0);
  glUniform1i(s_cp_efb, 0);
  s_copy_cu.ok = false;  // the one-copy path sets all its uniforms again
  g_gl.unit = -1;
  CBindTexture(0, s_efb_tex);
  CBindVertexArray(s_copy_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  CBindTexture(0, 0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
  s_r2_efb_fbo_ref = s_efb_fbo;
  s_r2_efb_off = true;
  s_gl_state_dirty = true;
  return t;
}
static void PMrtCheckCompare(const R2PendCopy &p) {
  static std::vector<unsigned char> a, b;
  a.resize((size_t)p.pw * p.ph * 4);
  b.resize(a.size());
  nwii::runtime::hle::r2_tex_fbo(p.tex, GL_READ_FRAMEBUFFER);
  glReadPixels(0, 0, (GLsizei)p.pw, (GLsizei)p.ph, GL_RGBA, GL_UNSIGNED_BYTE, a.data());
  static GLuint s_pmc_rfbo = 0;
  if (!s_pmc_rfbo) glGenFramebuffers(1, &s_pmc_rfbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_pmc_rfbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p.chk, 0);
  glReadPixels(0, 0, (GLsizei)p.pw, (GLsizei)p.ph, GL_RGBA, GL_UNSIGNED_BYTE, b.data());
  size_t d = 0;
  for (size_t i = 0; i < a.size(); i += 4)
    if (std::memcmp(&a[i], &b[i], 4)) ++d;
  ++st_pmc_copies;
  if (d) {
    ++st_pmc_bad;
    st_pmc_px += d;
    if (st_pmc_bad <= 16)
      printf("[P45MRT] DISTINTA copia dest=%08X %ux%u (gl %ux%u) fmt=%u alpha=%d: %zu px | xfb %ld\n", p.dest, p.dw,
             p.dh, p.pw, p.ph, p.fmt, (int)p.alpha, d, s_xfb_copies);
  }
  if (st_pmc_copies % 5000 == 0)
    printf("[P45MRT] copias comprobadas %llu, distintas %llu (%llu px)\n", (unsigned long long)st_pmc_copies,
           (unsigned long long)st_pmc_bad, (unsigned long long)st_pmc_px);
}
// Queues the copy of BP 0x52 `val` (a TEX copy, not stale) if it can wait; false: make it now.
static bool R2QueueCopy(uint32_t val) {
  if (!R2MrtOn() || s_r8_pass || DrawTraceOn() || !R2CopySt()) return false;
  uint32_t dest = g_state.efbCopyDest;
  uint32_t w = g_state.efbW, h = g_state.efbH;
  if (g_state.efbSrcX >= kEfbWidth || g_state.efbSrcY >= kEfbHeight) return false;
  w = std::min<uint32_t>(w, kEfbWidth - g_state.efbSrcX);
  h = std::min<uint32_t>(h, kEfbHeight - g_state.efbSrcY);
  if (!dest || w == 0 || h == 0 || w > 1024 || h > 1024) return false;
  // r45p NWII_P_MRT_SMALL=<n>: only copies of at most n x n texels go to the MRT queue (the 32x32 tile
  // cells of the world selector and the animated tiles); larger ones (screen captures) the one-copy path.
  static const uint32_t s_mrt_small = [] {
    const char *e = NWII_ENV("NWII_P_MRT_SMALL");
    const uint32_t v = e ? (uint32_t)std::atoi(e) : 0u;
    if (v) printf("[P45] NWII_P_MRT_SMALL=%u: solo copias de hasta %ux%u a la cola MRT\n", v, v, v);
    return v;
  }();
  if (s_mrt_small && (w > s_mrt_small || h > s_mrt_small)) return false;
  const bool half = (val >> 9) & 1;
  const bool intensity = ((val >> 15) & 1) && ((val >> 16) & 1);
  if (half || intensity || s_scale == 1.0f || s_iscale) return false;
  if (nwii::runtime::hle::TextureCache::get().efb_copy_stitched(dest)) return false;
  R2PendCopy p{};
  p.dest = dest;
  p.dw = w;
  p.dh = h;
  p.sx0 = SE(g_state.efbSrcX);
  p.sy0 = SE(g_state.efbSrcY);
  p.pw = (uint32_t)(SE(g_state.efbSrcX + (int)w) - p.sx0);
  p.ph = (uint32_t)(SE(g_state.efbSrcY + (int)h) - p.sy0);
  p.fmt = CopyRealFormat(val);
  p.stride = (g_state.bp[0x4D] & 0x3FF) << 5;
  p.alpha = EfbHasAlpha();
  p.sr[0] = p.sx0; p.sr[2] = p.sx0 + (int)p.pw;
  p.sr[1] = s_efb_h - p.sy0 - (int)p.ph; p.sr[3] = s_efb_h - p.sy0;
  if (val & 0x800) {
    // the colour-only clear ClearEfbCopySource would make with NWII_R2_CLEARTEX
    const uint32_t pf = EfbPixelFormat();
    bool c = (g_state.bp[0x41] >> 3) & 1, a = (g_state.bp[0x41] >> 4) & 1;
    const bool z = (g_state.bp[0x40] >> 4) & 1;
    if (pf != 1) a = false;
    if (c || a || z) {
      ::GX_GetClearColor(p.cc);
      if (pf != 1) { a = true; p.cc[3] = 0; }
      if (!(c && a && !z)) return false;
      const int cw = g_state.efbW, ch = g_state.efbH;
      p.cr[0] = std::max(SE(g_state.efbSrcX), 0);
      p.cr[1] = std::max(s_efb_h - SE(g_state.efbSrcY + ch), 0);
      p.cr[2] = std::min(SE(g_state.efbSrcX + cw), s_efb_w);
      p.cr[3] = std::min(s_efb_h - SE(g_state.efbSrcY), s_efb_h);
      p.clear = true;
    }
  }
  // a queued copy to the same place, or a queued clear over this source: make the queue first
  for (const auto &q : s_r2_pend)
    if (q.dest == dest || (q.clear && R2RectsMeet(q.cr, p.sr))) { R2FlushCopies(); break; }
  if (val & 0x800) g_state.pe_clear_pending = false;  // as ClearEfbCopySource
  if (PMrtCheckOn()) p.chk = PMrtCheckSnap(p);  // r45p
  s_r2_pend.push_back(p);
  if (s_r2_pend.size() >= 32) R2FlushCopies();
  return true;
}

static void ApplyCullMode() {
  static const bool no_cull = NWII_ENV("NWII_NOCULL") != nullptr;
  const uint8_t m = g_state.cullMode;
  if (no_cull || m == 0) {
    CEnable(kEnCull, GL_CULL_FACE, false);
    return;
  }
  CEnable(kEnCull, GL_CULL_FACE, true);
  CCull(m == 3 ? GL_FRONT_AND_BACK : GL_BACK, m == 2 ? GL_CCW : GL_CW);
}

// NWII_DRAWSTATS=1: per 120 rendered frames, how many draws/vertices were
// perspective vs orthographic and how many vertices landed inside the clip
// volume -- tells "not submitted" apart from "submitted but off-screen".
static struct {
  uint64_t draws[2], verts[2], inside[2], nonfinite, frames;
  uint64_t nrm_verts, nrm_inside;
} s_ds;
static bool s_drawstats_on = false;  // R14 R5: set once by InitFlags()
static inline bool DrawStatsOn() { return s_drawstats_on; }
static void DrawStatsVertex(const BatchVertex &bv) {
  const float *p = s_proj;
  float cx = p[0] * bv.x + p[4] * bv.y + p[8] * bv.z + p[12];
  float cy = p[1] * bv.x + p[5] * bv.y + p[9] * bv.z + p[13];
  float cz = p[2] * bv.x + p[6] * bv.y + p[10] * bv.z + p[14];
  float cw = p[3] * bv.x + p[7] * bv.y + p[11] * bv.z + p[15];
  int k = (g_state.projSet && g_state.projType == 0) ? 0 : 1;
  ++s_ds.verts[k];
  if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cw)) {
    ++s_ds.nonfinite;
    return;
  }
  bool in = cw > 0 && std::fabs(cx) <= cw && std::fabs(cy) <= cw && std::fabs(cz) <= cw;
  if (g_state.bp[0x40] & 1) {
    static long zn = 0;
    if (zn++ % 200000 == 0)
      printf("[DS] ztest draw: zmode=%X view.z=%.3f clip.z=%.4f w=%.3f proj45=(%g,%g) bp51=%06X\n",
             g_state.bp[0x40], bv.z, cz, cw, g_state.projection[4],
             g_state.projection[5], g_state.bp[0x51] & 0xFFFFFF);
  }
  if (in)
    ++s_ds.inside[k];
  if (bv.nx != 0.0f || bv.ny != 0.0f || bv.nz != 0.0f) {
    ++s_ds.nrm_verts;
    if (in)
      ++s_ds.nrm_inside;
    static long nshown = 0;
    if (nshown < 12 && s_ds.frames > 1500) {
      ++nshown;
      printf("[DS] nrm vtx view=(%.1f,%.1f,%.1f) clip=(%.2f,%.2f,%.2f,%.2f) col=(%.2f,%.2f,%.2f,%.2f) uv=(%.2f,%.2f) chan0=%06X mat0=%08X amb0=%08X zmode=%X cm0=%06X\n",
             bv.x, bv.y, bv.z, cx, cy, cz, cw, bv.r, bv.g, bv.b, bv.a, bv.u, bv.v,
             g_state.xf[0x00E], g_state.xf[0x00C], g_state.xf[0x00A],
             g_state.bp[0x40], g_state.bp[0x41]);
    }
  }
  static int shown = 0;
  if (k == 0 && shown < 6 && s_ds.frames > 60) {
    ++shown;
    printf("[DS] persp vtx view=(%.2f,%.2f,%.2f) clip=(%.2f,%.2f,%.2f,%.2f) proj=[%g %g %g %g %g %g]\n",
           bv.x, bv.y, bv.z, cx, cy, cz, cw, g_state.projection[0],
           g_state.projection[1], g_state.projection[2], g_state.projection[3],
           g_state.projection[4], g_state.projection[5]);
  }
}
static void DrawStatsFrame() {
  if (++s_ds.frames % 120)
    return;
  printf("[DS] persp draws=%llu verts=%llu inside=%llu | ortho draws=%llu verts=%llu inside=%llu | nonfinite=%llu\n",
         (unsigned long long)s_ds.draws[0], (unsigned long long)s_ds.verts[0],
         (unsigned long long)s_ds.inside[0], (unsigned long long)s_ds.draws[1],
         (unsigned long long)s_ds.verts[1], (unsigned long long)s_ds.inside[1],
         (unsigned long long)s_ds.nonfinite);
  printf("[DS] normal-bearing verts=%llu inside=%llu\n",
         (unsigned long long)s_ds.nrm_verts, (unsigned long long)s_ds.nrm_inside);
  uint64_t f = s_ds.frames;
  s_ds = {};
  s_ds.frames = f;
}

// GX texture coordinate generation (XF 0x1040 TexMtxInfo, 0x1018 dual-tex,
// 0x1050 PostMtxInfo), following Dolphin's VertexShaderGen: pick the input
// row, transform by the coord's texture matrix (2 or 3 rows), optionally
// normalise and post-transform, then divide by q for STQ. Coords 0-3 reach the
// shader; NW4R models use normal-sourced coords for their light maps.
static void TexGen(const VertexData &vtx, BatchVertex &bv) {
  float out[4][2] = {};
  const int n = g_state.numTexGens ? std::min<int>(g_state.numTexGens, 4) : 1;
  // XF 0x1012 (DUALTEX); the SDK always sets it, so "never written" (the
  // write may predate the renderer) counts as enabled.
  static const bool s_force_dual = true;
  const bool dual = (g_state.xf[0x12] & 1) || (s_force_dual && !g_state.xf[0x12]);
  for (int t = 0; t < n; ++t) {
    const uint32_t info = g_state.numTexGens ? g_state.xf[0x40 + t] : 0x280;
    const bool stq = (info >> 1) & 1;
    const bool abc1 = (info >> 2) & 1;
    const int type = (info >> 4) & 7;
    const int row = (info >> 7) & 0x1F;
    float in[4] = {0, 0, 1, 1};
    if (row == 0) {
      in[0] = vtx.raw_pos[0]; in[1] = vtx.raw_pos[1]; in[2] = vtx.raw_pos[2];
    } else if (row == 1) {
      in[0] = vtx.raw_norm[0]; in[1] = vtx.raw_norm[1]; in[2] = vtx.raw_norm[2];
    } else if (row == 2) {
      in[0] = vtx.color[0]; in[1] = vtx.color[1];
    } else if (row >= 5 && row <= 12) {
      int c = row - 5;
      if (vtx.has_tex[c]) { in[0] = vtx.tex[c][0]; in[1] = vtx.tex[c][1]; }
    }
    if (!abc1)
      in[2] = 1.0f;
    float s = in[0], tt = in[1], q = 1.0f;
    if (type == 0) {
      int mi = vtx.texMtxIdx[t];
      const float *m = (mi != 60 && mi * 4 + 12 <= 256)
                           ? &g_state.posMatrices[mi * 4] : nullptr;
      if (m && !(m[0] || m[1] || m[2] || m[4] || m[5] || m[6]))
        m = nullptr;  // never loaded: treat as identity
      if (m) {
        s = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + m[3] * in[3];
        tt = m[4] * in[0] + m[5] * in[1] + m[6] * in[2] + m[7] * in[3];
        q = stq ? m[8] * in[0] + m[9] * in[1] + m[10] * in[2] + m[11] * in[3]
                : 1.0f;
      } else {
        q = stq ? in[2] : 1.0f;
      }
      if (dual) {
        const uint32_t post = g_state.xf[0x50 + t];
        float v[3] = {s, tt, q};
        if ((post >> 8) & 1) {
          float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
          if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
        }
        int pi = post & 0x3F;
        if (pi != 61) {
          const float *pm = &g_state.postMatrices[pi * 4];
          s = pm[0] * v[0] + pm[1] * v[1] + pm[2] * v[2] + pm[3];
          tt = pm[4] * v[0] + pm[5] * v[1] + pm[6] * v[2] + pm[7];
          q = stq ? pm[8] * v[0] + pm[9] * v[1] + pm[10] * v[2] + pm[11] : 1.0f;
        } else {
          s = v[0]; tt = v[1]; q = stq ? v[2] : 1.0f;
        }
      }
    }
    if (stq && q != 0.0f) { s /= q; tt /= q; }
    out[t][0] = s;
    out[t][1] = tt;
  }
  bv.u = out[0][0]; bv.v = out[0][1];
  bv.u1 = out[1][0]; bv.v1 = out[1][1];
  bv.u2 = out[2][0]; bv.v2 = out[2][1];
  bv.u3 = out[3][0]; bv.v3 = out[3][1];
}

static void EmitVertex(const VertexData &vtx) {
  BatchVertex bv = {0};
  if (vtx.has_color) {
    bv.r = vtx.color[0];
    bv.g = vtx.color[1];
    bv.b = vtx.color[2];
    bv.a = vtx.color[3];
  } else {
    bv.r = bv.g = bv.b = bv.a = 1.f;
  }
  if (vtx.has_norm && vtx.pre_xf) {
    bv.nx = vtx.norm[0]; bv.ny = vtx.norm[1]; bv.nz = vtx.norm[2];
  } else if (vtx.has_norm) {
    // Normal matrix for a position matrix index: 3 * (idx & 31), three rows
    // of three (XF normal matrix bank layout).
    int nb = 3 * (vtx.posMtxIdx & 31);
    const float *nm = g_state.normalMatrices;
    if (nb + 9 <= 96) {
      bv.nx = vtx.norm[0] * nm[nb + 0] + vtx.norm[1] * nm[nb + 1] +
              vtx.norm[2] * nm[nb + 2];
      bv.ny = vtx.norm[0] * nm[nb + 3] + vtx.norm[1] * nm[nb + 4] +
              vtx.norm[2] * nm[nb + 5];
      bv.nz = vtx.norm[0] * nm[nb + 6] + vtx.norm[1] * nm[nb + 7] +
              vtx.norm[2] * nm[nb + 8];
    } else {
      bv.nx = vtx.norm[0]; bv.ny = vtx.norm[1]; bv.nz = vtx.norm[2];
    }
  }
  TexGen(vtx, bv);

  float x = vtx.pos[0], y = vtx.pos[1], z = vtx.pos[2];
  int mtx_base = (vtx.posMtxIdx & 0x3F) * 4;
  if (vtx.pre_xf) {
    // Already transformed at decode with the per-draw matrix snapshot.
    bv.x = x;
    bv.y = y;
    bv.z = z;
  } else if (vtx.has_pos && mtx_base + 12 <= 256) {
    const float *mm = g_state.posMatrices;
    bv.x = x * mm[mtx_base + 0] + y * mm[mtx_base + 1] + z * mm[mtx_base + 2] +
           mm[mtx_base + 3];
    bv.y = x * mm[mtx_base + 4] + y * mm[mtx_base + 5] + z * mm[mtx_base + 6] +
           mm[mtx_base + 7];
    bv.z = x * mm[mtx_base + 8] + y * mm[mtx_base + 9] + z * mm[mtx_base + 10] +
           mm[mtx_base + 11];
  } else {
    bv.x = x;
    bv.y = y;
    bv.z = z;
  }

  // NWII_FLATZ=1 restores the old behaviour of flattening orthographic draws.
  static const bool s_flatz = NWII_ENV("NWII_FLATZ") != nullptr;
  if (s_flatz && !CurrentIsPerspective())
    bv.z = 0.0f;
  static const char *s_vtxtrace_env = NWII_ENV("NWII_VTXTRACE");
  static const long s_vtxtrace_from = s_vtxtrace_env ? std::atol(s_vtxtrace_env) : 0;
  static long s_vtx_seq = 0;
  if (s_vtxtrace_env && s_vtx_seq++ >= s_vtxtrace_from) {
    static int vn = 0;
    if (vn++ < 14)
      printf("[VTX] raw=(%.1f,%.1f,%.1f) view=(%.1f,%.1f,%.1f) uv=(%.3f,%.3f) "
             "tex0=%ux%u fmt=%u\n",
             vtx.pos[0], vtx.pos[1], vtx.pos[2], bv.x, bv.y, bv.z, bv.u, bv.v,
             g_state.texStages[0].width, g_state.texStages[0].height,
             g_state.texStages[0].format);
  }
  if (DrawStatsOn())
    DrawStatsVertex(bv);
  s_batch.push_back(bv);
}

// r45p NWII_P_DLMERGE: the parts (vertex counts) of the draw being emitted, set by DrawPrimitive
// around EmitPrimitive; EmitIndices then makes each part's indices as its own draw did.
static const std::vector<uint16_t> *s_emit_parts = nullptr;
// r45p: parts of the fused draw that the cull test rejected (xf::Emit; null: none): no indices for them
// (a part entirely outside the clip volume makes no fragment: the same image, less GPU work).
static const std::vector<uint8_t> *s_emit_part_skip = nullptr;
static void EmitIndices1(uint8_t prim, uint32_t b, uint32_t n);
static void EmitIndices(uint8_t prim, uint32_t b, uint32_t n) {
  if (__builtin_expect(s_emit_parts != nullptr, 0)) {
    uint32_t at = 0;
    size_t j = 0;
    for (const uint16_t k : *s_emit_parts) {
      if (at + k > n) break;
      if (!(s_emit_part_skip && j < s_emit_part_skip->size() && (*s_emit_part_skip)[j])) EmitIndices1(prim, b + at, k);
      at += k;
      ++j;
    }
    return;
  }
  EmitIndices1(prim, b, n);
}
// Expand a GX primitive into triangle indices over its vertices, which start
// at `b` in s_batch.
static void EmitIndices1(uint8_t prim, uint32_t b, uint32_t n) {
  // Sized once and written in place (R5 perf: the per-triangle
  // insert(initializer_list) was ~7% of the host thread in Peach's party).
  auto &ix = s_batch_idx;
  size_t tris = 0;
  switch (prim) {
  case 0x80: case 0x88: tris = (n / 4) * 2; break;
  case 0x90: tris = n / 3; break;
  case 0x98: case 0xA0: tris = n >= 2 ? n - 2 : 0; break;
  default: return;  // lines / points: not drawn
  }
  if (!tris) return;
  const size_t at = ix.size();
  ix.resize(at + tris * 3);
  uint32_t *o = ix.data() + at;
  switch (prim) {
  case 0x80:  // quads
  case 0x88:  // quads (alternate encoding; Dolphin draws it as quads)
    for (uint32_t i = 0; i + 3 < n; i += 4) {
      const uint32_t q = b + i;
      o[0] = q; o[1] = q + 1; o[2] = q + 2; o[3] = q + 2; o[4] = q + 3; o[5] = q;
      o += 6;
    }
    break;
  case 0x90:  // triangles
    for (uint32_t i = 0; i + 2 < n; i += 3) {
      o[0] = b + i; o[1] = b + i + 1; o[2] = b + i + 2;
      o += 3;
    }
    break;
  case 0x98:  // triangle strip
    for (uint32_t i = 2; i < n; i++) {
      if (i & 1) { o[0] = b + i - 1; o[1] = b + i - 2; o[2] = b + i; }
      else       { o[0] = b + i - 2; o[1] = b + i - 1; o[2] = b + i; }
      o += 3;
    }
    break;
  case 0xA0:  // triangle fan
    for (uint32_t i = 2; i < n; i++) {
      o[0] = b; o[1] = b + i - 1; o[2] = b + i;
      o += 3;
    }
    break;
  }
}

static bool s_batch_open = false;
// Batching: consecutive GX draws whose GL-visible state is identical go out
// as one glDrawArrays (vertices are transformed and texgen'd on the CPU, so
// matrix loads between them do not matter). s_state_dirty is raised when a
// command changes state the draw key covers; only then is the key rebuilt.
static uint64_t s_batch_key = 0;
static bool s_state_dirty = true;
static GLuint s_draw_tex[8];
// r39r2 NWII_R2_TEXMEMO (see DrawPrimitive): texture lookups remembered while
// s_r2_tm_epoch stays (bumped at frame start, EFB copies/clears, TLUT loads and
// any lookup that made the texture cache upload or stitch).
struct R2TexMemo { uint32_t addr, w, h; uint8_t fmt; uint32_t toff; uint8_t tfmt; uint64_t epoch; GLuint tex; };
static R2TexMemo s_r2_tm[32];
static uint64_t s_r2_tm_epoch = 1, s_r2_tm_hits = 0, s_r2_tm_miss = 0;
static bool s_r2_texmemo_on = false;
static inline uint32_t R2TmSlot(const TexStage &st) {
  return ((st.base_addr >> 5) ^ (st.base_addr >> 11) ^ st.width ^ (st.height << 3) ^ st.format ^ (st.tlut_offset >> 9)) & 31;
}
static uint32_t s_draw_used = 0;

static inline uint64_t KeyMix(uint64_t h, uint32_t w) {
  return (h ^ w) * 1099511628211ull;
}

// Everything a batch's GL state is built from: shader (TEV, blend, Z, cull,
// texture formats...), textures and their sampler modes, projection,
// viewport, scissor, XF channel colours and lights, TEV colour registers.
// R9 render: the shader hash DrawKey() computed, for the SetupDrawState()
// that follows it in the same DrawPrimitive (g_state does not change between
// the two). Reset by DrawPrimitive after use.
static bool s_shader_hash_ok = false;
static uint64_t s_shader_hash = 0;
// r39r2 NWII_R2_KEYSTAT=1 (debug): per key change, which parts of the batch key
// changed from the previous batch ([R2KEY] every 300 frames).
static bool s_r2ks_on = false;
static uint64_t s_r2ks_part[10], s_r2ks_prev[10], s_r2ks_cnt[10], s_r2ks_n = 0, s_r2ks_frames = 0;
static void R2KeyStat(const uint64_t *part) {
  ++s_r2ks_n;
  for (int i = 0; i < 10; ++i)
    if (part[i] != s_r2ks_prev[i]) ++s_r2ks_cnt[i];
  std::memcpy(s_r2ks_prev, part, sizeof s_r2ks_prev);
}
static void R2KeyStatFrame() {
  if (!s_r2ks_on || ++s_r2ks_frames % 300) return;
  static const char *const nm[10] = {"shader", "cull", "scissor", "tex", "texmode", "xfchan", "viewport",
                                     "proj", "lights", "tevregs"};
  printf("[R2KEY] per frame: keychg=%.1f |", s_r2ks_n / 300.0);
  for (int i = 0; i < 10; ++i) printf(" %s=%.1f", nm[i], s_r2ks_cnt[i] / 300.0);
  printf("\n");
  s_r2ks_n = 0;
  std::memset(s_r2ks_cnt, 0, sizeof s_r2ks_cnt);
}
static uint64_t DrawKey() {
  uint64_t h = g_state.GetShaderHash(0);
  s_shader_hash = h;
  s_shader_hash_ok = true;
  h = KeyMix(h, g_state.cullMode);
  h = KeyMix(h, g_state.bp[0x20]);
  h = KeyMix(h, g_state.bp[0x21]);
  h = KeyMix(h, g_state.bp[0x59]);
  for (int i = 0; i < 8; ++i) {
    if (!((s_draw_used >> i) & 1)) continue;
    h = KeyMix(h, s_draw_tex[i]);
    h = KeyMix(h, g_state.bp[(i < 4 ? 0x80 : 0xA0) + (i & 3)] & 0xFF);
  }
  for (int a = 0x0A; a <= 0x11; ++a) h = KeyMix(h, g_state.xf[a]);
  for (int a = 0x1A; a <= 0x1F; ++a) h = KeyMix(h, g_state.xf[a]);
  for (int i = 0; i < 7; ++i) {
    uint32_t w;
    std::memcpy(&w, &g_state.projection[i], 4);
    h = KeyMix(h, w);
  }
  h = KeyMix(h, g_state.projType);
  h = KeyMix(h, g_state.projSet);
  // Lights only matter when a channel is lit.
  if ((g_state.xf[0x0E] | g_state.xf[0x0F] | g_state.xf[0x10] | g_state.xf[0x11]) & 2) {
    for (int l = 0; l < 8; ++l)
      for (int k = 3; k < 16; ++k) {
        uint32_t w;
        std::memcpy(&w, &g_state.lights[l][k], 4);
        h = KeyMix(h, w);
      }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 2; ++j) {
      h = KeyMix(h, g_state.tevColorReg[i][j]);
      h = KeyMix(h, g_state.tevKonstReg[i][j]);
    }
  if (s_r2ks_on) {  // r39r2 debug: the same parts, hashed one by one
    uint64_t pt[10] = {};
    pt[0] = s_shader_hash;
    pt[1] = g_state.cullMode;
    pt[2] = KeyMix(KeyMix(g_state.bp[0x20], g_state.bp[0x21]), g_state.bp[0x59]);
    for (int i = 0; i < 8; ++i) {
      if (!((s_draw_used >> i) & 1)) continue;
      pt[3] = KeyMix(pt[3], s_draw_tex[i]);
      pt[4] = KeyMix(pt[4], g_state.bp[(i < 4 ? 0x80 : 0xA0) + (i & 3)] & 0xFF);
    }
    for (int a = 0x0A; a <= 0x11; ++a) pt[5] = KeyMix(pt[5], g_state.xf[a]);
    for (int a = 0x1A; a <= 0x1F; ++a) pt[6] = KeyMix(pt[6], g_state.xf[a]);
    for (int i = 0; i < 7; ++i) { uint32_t w; std::memcpy(&w, &g_state.projection[i], 4); pt[7] = KeyMix(pt[7], w); }
    for (int l = 0; l < 8; ++l)
      for (int k = 3; k < 16; ++k) { uint32_t w; std::memcpy(&w, &g_state.lights[l][k], 4); pt[8] = KeyMix(pt[8], w); }
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 2; ++j) pt[9] = KeyMix(KeyMix(pt[9], g_state.tevColorReg[i][j]), g_state.tevKonstReg[i][j]);
    std::memcpy(s_r2ks_part, pt, sizeof pt);
  }
  // Indirect matrices/scales and texcoord scales (uniforms of indirect
  // shaders), plus the map sizes they divide by.
  bool ind = (g_state.bp[0x00] >> 16) & 7;
  for (int i = 0; i < g_state.numTevStages && !ind; ++i)
    ind = g_state.bp[0x10 + (i & 15)] & 0x1FF80;  // bs or mid set
  if (ind) {
    for (int r = 0x06; r <= 0x0E; ++r) h = KeyMix(h, g_state.bp[r]);
    h = KeyMix(h, g_state.bp[0x25]);
    h = KeyMix(h, g_state.bp[0x26]);
    for (int r = 0x30; r <= 0x3F; ++r) h = KeyMix(h, g_state.bp[r]);
    for (int i = 0; i < 8; ++i)
      h = KeyMix(h, g_state.texStages[i].width | (g_state.texStages[i].height << 16));
  }
  return h;
}

// BP registers that never change a draw's GL state (copy/clear parameters,
// sync tokens, TLUT source, the BP mask).
static inline bool BpAffectsDraw(uint8_t reg) {
  if (reg >= 0x01 && reg <= 0x04) return false;  // copy filter pattern
  if (reg >= 0x44 && reg <= 0x51) return false;  // sync, copy rect/dest, clear
  if (reg == 0x53 || reg == 0x54) return false;  // copy filter
  if (reg == 0x64 || (reg >= 0x66 && reg <= 0x6F)) return false;
  if (reg == 0xFE) return false;
  return true;
}

static int s_compiles_this_frame = 0;
// Set by SetupDrawState when the draw's shader isn't ready: DrawPrimitive then
// skips emitting its vertices instead of drawing them with the wrong shader.
static bool s_skip_draw = false;

static void SetupDrawState(const GXCommand &cmd);
static void EmitPrimitive(const GXCommand &cmd);

// NWII_DRAWTRACE=<from>-<to>: one line per draw of XFB frames [from, to]
// (numbered like the NWII_XFBDUMP files): vertex count, NDC bounding box,
// textures, blend/z/TEV state. Finds the draw behind a bad pixel.
static long s_dt_from = -1, s_dt_to = -1;
static bool s_dt_on = false;  // R14 R5: set once by InitFlags()
static inline bool DrawTraceOn() {
  if (!s_dt_on) return false;
  const long f = s_xfb_copies + 1;
  return f >= s_dt_from && f <= s_dt_to;
}
static long s_dt_seq = 0;
static int s_pw_x = -1, s_pw_y = -1;  // NWII_PIXWATCH pixel
static bool DrawTrace(const GXCommand &cmd, size_t base) {
  static long last_f = -1;
  long &seq = s_dt_seq;
  const long f = s_xfb_copies + 1;
  if (f != last_f) { last_f = f; seq = 0; }
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  const float *p = s_proj;
  for (size_t i = base; i < s_batch.size(); ++i) {
    const BatchVertex &bv = s_batch[i];
    float cx = p[0] * bv.x + p[4] * bv.y + p[8] * bv.z + p[12];
    float cy = p[1] * bv.x + p[5] * bv.y + p[9] * bv.z + p[13];
    float cw = p[3] * bv.x + p[7] * bv.y + p[11] * bv.z + p[15];
    if (cw == 0.0f) cw = 1e-6f;
    x0 = std::min(x0, cx / cw); x1 = std::max(x1, cx / cw);
    y0 = std::min(y0, cy / cw); y1 = std::max(y1, cy / cw);
  }
  // NDC -> EFB pixels (GX top-left origin) through the current viewport.
  {
    float vp[6];
    std::memcpy(vp, &g_state.xf[0x1A], sizeof(vp));
    float w = 2.0f * vp[0], h = -2.0f * vp[1];
    float left = vp[3] - vp[0] - 342.0f, top = vp[4] + vp[1] - 342.0f;
    if (!(w > 0.0f && h > 0.0f)) { w = kEfbWidth; h = kEfbHeight; left = top = 0; }
    const float px0 = left + (x0 + 1) * 0.5f * w, px1 = left + (x1 + 1) * 0.5f * w;
    const float py0 = top + (1 - y1) * 0.5f * h, py1 = top + (1 - y0) * 0.5f * h;
    x0 = px0; x1 = px1; y0 = py0; y1 = py1;
  }
  const bool covers = s_pw_x >= x0 && s_pw_x <= x1 && s_pw_y >= y0 && s_pw_y <= y1;
  const auto &v = cmd.verts();
  const auto &v0 = v[0];
  printf("[DT] f=%ld d=%ld prim=%02X n=%zu px=(%.0f,%.0f)-(%.0f,%.0f) prexf=%d pm=%u "
         "pos0=(%.1f,%.1f,%.1f) col0=(%.2f,%.2f,%.2f,%.2f) uv0=(%.3f,%.3f) proj=%u "
         "cm=%06X z=%X ac=%06X ntev=%u ntg=%u sh=%llx",
         f, seq++, cmd.prim_type, v.size(), x0, y0, x1, y1, v0.pre_xf ? 1 : 0,
         (unsigned)v0.posMtxIdx, v0.pos[0], v0.pos[1], v0.pos[2], v0.color[0],
         v0.color[1], v0.color[2], v0.color[3], v0.tex[0][0], v0.tex[0][1],
         g_state.projType, g_state.bp[0x41], g_state.bp[0x40], g_state.bp[0xF3],
         g_state.numTevStages, g_state.numTexGens,
         (unsigned long long)g_state.GetShaderHash(0));
  for (int i = 0; i < 8; ++i)
    if ((s_draw_used >> i) & 1)
      printf(" t%d=%08X/%ux%u/f%u/m%02X/gl%u", i, g_state.texStages[i].base_addr,
             g_state.texStages[i].width, g_state.texStages[i].height,
             g_state.texStages[i].format,
             g_state.bp[(i < 4 ? 0x80 : 0xA0) + (i & 3)] & 0xFF, s_draw_tex[i]);
  printf("\n");
  return covers;
}

// NWII_PIXWATCH=x,y (with NWII_DRAWTRACE): after every draw/copy of the traced
// frames, flush and read EFB pixel (x, y) (GX coordinates) into a PBO (no
// stall); PixWatchEnd prints the changes at the end of the batch.
static std::vector<std::string> s_pw_labels;
static GLuint s_pw_pbo = 0;
static void PixWatch(const char *what) {
  int &wx = s_pw_x, &wy = s_pw_y;
  static const bool on = [&] {
    const char *e = NWII_ENV("NWII_PIXWATCH");
    return e && std::sscanf(e, "%d,%d", &wx, &wy) == 2;
  }();
  if (!on) return;
  if (s_batch_open) { FlushBatch(); s_batch_open = false; }
  constexpr size_t kMax = 1 << 16;
  if (!s_pw_pbo) {
    glGenBuffers(1, &s_pw_pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pw_pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, kMax * 4, nullptr, GL_STREAM_READ);
  }
  if (s_pw_labels.size() >= kMax) return;
  glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pw_pbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
  // (GX pixel -> the centre GL pixel of its s x s block.)
  glReadPixels((SE(wx) + SE(wx + 1)) / 2,
               s_efb_h - 1 - (SE(wy) + SE(wy + 1)) / 2, 1, 1, GL_RGBA,
               GL_UNSIGNED_BYTE,
               (void *)(s_pw_labels.size() * 4));
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  char buf[64];
  std::snprintf(buf, sizeof buf, "f=%ld %s", s_xfb_copies + 1, what);
  s_pw_labels.push_back(buf);
}
static void PixWatchEnd() {
  if (s_pw_labels.empty()) return;
  glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pw_pbo);
  const uint8_t *m = (const uint8_t *)glMapBufferRange(
      GL_PIXEL_PACK_BUFFER, 0, s_pw_labels.size() * 4, GL_MAP_READ_BIT);
  static uint32_t last = 0xFFFFFFFFu;
  if (m) {
    for (size_t i = 0; i < s_pw_labels.size(); ++i) {
      const uint8_t *px = m + i * 4;
      const uint32_t v = (uint32_t)px[0] << 24 | px[1] << 16 | px[2] << 8 | px[3];
      if (v != last)
        printf("[PIX] %s: %02X %02X %02X %02X\n", s_pw_labels[i].c_str(), px[0],
               px[1], px[2], px[3]);
      last = v;
    }
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  s_pw_labels.clear();
}

static inline uint32_t DrawVertCount(const GXCommand &cmd);  // R14 R5 (after ecache::)
// r36r (r13): NWII_DRAWLOG / NWII_DRAWLOG_N / NWII_XFLOG, set once by InitFlags().
static bool s_dl_on = false;
static long s_dl_from = 0, s_dl_n = 40, s_xflog_n = 0;
static void DrawPrimitive(const GXCommand &cmd) {
  // R14 R5: the vertex count from the draw's emit-cache entry when the render
  // worker's list gave it (no read of the DecodedVerts).
  const size_t n = DrawVertCount(cmd);
  if (n == 0)
    return;

  // NWII_DRAWLOG=<n>: log 40 draws starting at draw number n (default 0).
  // r36r (r13): read once by InitFlags (were function-local statics: an
  // acquire load of each guard on every draw).
  const bool s_drawlog = s_dl_on;
  const long s_drawlog_from = s_dl_from;
  static long s_draw_seq = 0;
  const long s_drawlog_n = s_dl_n;
  if (s_drawlog && s_draw_seq++ >= s_drawlog_from) {
    const auto &v = cmd.verts();
    static int dn = 0;
    if (dn < s_drawlog_n) {
      const auto &v0 = v[0];
      printf("[DRAWX %04d] n=%zu pos0=(%.1f,%.1f,%.1f)%s col0=%s(%.2f,%.2f,%.2f,%.2f) "
             "uv0=(%.3f,%.3f) proj=%u z=%X cm0=%06X ac=%06X ntev=%u tex0base=%08X\n",
             dn, n, v0.pos[0], v0.pos[1], v0.pos[2], v0.pre_xf ? "x" : "",
             v0.has_color ? "" : "none", v0.color[0], v0.color[1], v0.color[2],
             v0.color[3], v0.tex[0][0], v0.tex[0][1], g_state.projType,
             g_state.bp[0x40], g_state.bp[0x41], g_state.bp[0xF3],
             g_state.numTevStages, g_state.texStages[0].base_addr);
      if (g_state.numTevStages > 1) {
        printf("          dualtex=%X post:", g_state.xf[0x12]);
        for (int t = 0; t < 4; ++t)
          printf(" %08X", g_state.xf[0x50 + t]);
        printf(" mtxidx:");
        for (int t = 0; t < 4; ++t)
          printf(" %d", (int)v[0].texMtxIdx[t]);
        printf("\n");
        printf("          texgen n=%u:", g_state.numTexGens);
        for (int t = 0; t < (int)g_state.numTexGens && t < 4; ++t)
          printf(" tg%d=%08X", t, g_state.xf[0x40 + t]);
        for (int m = 0; m < 3; ++m)
          printf(" | map%d %ux%u f%u @%08X", m, g_state.texStages[m].width,
                 g_state.texStages[m].height, g_state.texStages[m].format,
                 g_state.texStages[m].base_addr);
        printf(" | stages:");
        for (int st = 0; st < (int)g_state.numTevStages; ++st)
          printf(" [map%d coord%d chan%d]", (int)g_state.tevStages[st].texMap,
                 (int)g_state.tevStages[st].texCoord,
                 (int)g_state.tevStages[st].colorChan);
        printf("\n          regs PREV=%06X/%06X C0=%06X/%06X C1=%06X/%06X C2=%06X/%06X K3=%06X/%06X\n",
               g_state.tevColorReg[0][0], g_state.tevColorReg[0][1],
               g_state.tevColorReg[1][0], g_state.tevColorReg[1][1],
               g_state.tevColorReg[2][0], g_state.tevColorReg[2][1],
               g_state.tevColorReg[3][0], g_state.tevColorReg[3][1],
               g_state.tevKonstReg[3][0], g_state.tevKonstReg[3][1]);
      }

      printf("[DRAW %02d] prim=0x%02X verts=%zu hash=%llx tex0=%dx%d fmt=%d "
             "map=%d "
             "| bp00=%06X F3=%06X C0=%06X C1=%06X C2=%06X C3=%06X 28=%06X "
             "cp50=%08X\n",
             dn, cmd.prim_type, n,
             (unsigned long long)g_state.GetShaderHash(cmd.prim_type),
             g_state.texStages[0].width, g_state.texStages[0].height,
             g_state.texStages[0].format, (int)g_state.tevStages[0].texMap,
             g_state.bp[0x00], g_state.bp[0xF3], g_state.bp[0xC0],
             g_state.bp[0xC1], g_state.bp[0xC2], g_state.bp[0xC3],
             g_state.bp[0x28], g_state.cp[0x50]);

      uint32_t th = 2166136261u, nz = 0;
      uint32_t off = g_state.texStages[0].tlut_offset;
      for (uint32_t i = 0; i < 512 && off + i < sizeof(g_state.tlutMem); ++i) {
        uint8_t b = g_state.tlutMem[off + i];
        th = (th ^ b) * 16777619u;
        if (b)
          ++nz;
      }

      uint32_t dh = 2166136261u, dnz = 0;
      uint32_t base = g_state.texStages[0].base_addr;
      if (g_ctx_ptr) {
        for (uint32_t i = 0; i < 2048; ++i) {
          uint8_t b = g_ctx_ptr->mmu.read8(base + i);
          dh = (dh ^ b) * 16777619u;
          if (b)
            ++dnz;
        }
      }
      printf("          tlut off=%u hash=%08X nonzero=%u/512 src=%08X | "
             "texdata base=%08X hash=%08X nonzero=%u/2048\n",
             off, th, nz, g_state.tlutSrcAddr, base, dh, dnz);
      ++dn;
    }
  }

  ++g_prof_gx_draws;
  ++s_cap_cur.gx_draws;
  if (rph::On()) { ++rph::s_draws; rph::s_verts += n; }
  if (DrawStatsOn())
    ++s_ds.draws[(g_state.projSet && g_state.projType == 0) ? 0 : 1];
  if (!s_batch_open || s_state_dirty) {
    // Resolve the textures first: they are part of the key.
    const uint32_t pending_used = s_draw_used;  // maps of the batch not drawn yet
    if (rph::On()) ++rph::s_dirty;
    rph::Set(rph::kTex);
    s_draw_used = UsedTexMaps();
    for (int i = 0; i < 8; ++i) {
      s_draw_tex[i] = 0;
      const auto &stage = g_state.texStages[i];
      if (((s_draw_used >> i) & 1) && g_ctx_ptr && stage.base_addr != 0 &&
          stage.width > 0 && stage.height > 0) {
        // r39r2 NWII_R2_TEXMEMO=1: within a stretch with no EFB copy, TLUT
        // load or texture-cache upload/stitch, the same map parameters give
        // the same texture: remembered instead of looked up again.
        R2TexMemo *m = (s_r2_texmemo_on && !s_r8_pass) ? &s_r2_tm[R2TmSlot(stage)] : nullptr;
        if (m && m->epoch == s_r2_tm_epoch && m->addr == stage.base_addr && m->w == stage.width &&
            m->h == stage.height && m->fmt == stage.format && m->toff == stage.tlut_offset &&
            m->tfmt == stage.tlut_format) {
          s_draw_tex[i] = m->tex;
          ++s_r2_tm_hits;
        } else {
          auto &tc = nwii::runtime::hle::TextureCache::get();
          const uint64_t t0 = tc.gl_touches();
          s_draw_tex[i] = tc.get_texture(*g_ctx_ptr, stage);
          if (m) {
            if (tc.gl_touches() != t0) ++s_r2_tm_epoch;  // it uploaded or stitched: forget the rest
            *m = R2TexMemo{stage.base_addr, stage.width, stage.height, stage.format, stage.tlut_offset,
                           stage.tlut_format, s_r2_tm_epoch, s_draw_tex[i]};
            ++s_r2_tm_miss;
          }
        }
      }
    }
    rph::Set(rph::kState);
    // get_texture binds only on its own unit (kScratchUnit), never on 0-7, so
    // the batch still pending keeps its textures; only the active unit moved.
    {
      static uint64_t s_tc_touches = 0;
      const uint64_t t = nwii::runtime::hle::TextureCache::get().gl_touches();
      if (t != s_tc_touches) {
        s_tc_touches = t;
        // The old code bound on the active unit: count the batches it broke.
        if (s_batch_open && !s_batch_idx.empty() && g_gl.unit >= 0 &&
            g_gl.unit < 8 && ((pending_used >> g_gl.unit) & 1)) {
          ++g_prof_tex_bind_saves;
          ++s_cap_cur.bind_saves;
          static const bool s_bf_log = NWII_ENV("NWII_BINDFIX_LOG") != nullptr;
          if (s_bf_log)
            printf("[BINDFIX] xfb=%ld unit=%d\n", s_xfb_copies + 1, g_gl.unit);
        }
        static const bool s_old_bind = NWII_ENV("NWII_TEXBIND_OLD") != nullptr;
        if (s_old_bind) GLForgetActiveBinding();  // old path: active unit lost
        g_gl.unit = -1;
      }
    }
    if (DrawTraceOn()) {
      static uint64_t s_up_last = 0;
      if (g_prof_tex_uploads != s_up_last && s_batch_open && !s_batch_idx.empty())
        printf("[DT] f=%ld UPLOAD before d=%ld with a pending batch (active unit %d)\n",
               s_xfb_copies + 1, s_dt_seq, g_gl.unit);
      s_up_last = g_prof_tex_uploads;
    }
    const uint64_t key = DrawKey();
    s_state_dirty = false;
    if (s_batch_open && key == s_batch_key) s_shader_hash_ok = false;
    if (!s_batch_open || key != s_batch_key) {
      if (rph::On()) ++rph::s_keychg;
      if (s_r2ks_on) R2KeyStat(s_r2ks_part);
      FlushBatch();
      for (int i = 0; i < 8; ++i) {
        if (!((s_draw_used >> i) & 1) || !s_draw_tex[i]) continue;
        CBindTexture(i, s_draw_tex[i]);
        ApplyTexMode(i);
      }
      SetupDrawState(cmd);
      s_shader_hash_ok = false;
      s_batch_key = key;
      s_batch_open = true;
    }
  }
  if (s_skip_draw)
    return; // shader for this state not ready yet
  const size_t trace_base = s_batch.size();
  rph::Set(rph::kEmit);
  {
    const DecodedVerts *pdv = cmd.shared_vertices.get();  // r45p NWII_P_DLMERGE
    s_emit_parts = (pdv && !pdv->parts.empty()) ? &pdv->parts : nullptr;
  }
  EmitPrimitive(cmd);
  s_emit_parts = nullptr;
  s_emit_part_skip = nullptr;
  if (DrawTraceOn()) {
    if (DrawTrace(cmd, trace_base)) {
    char lbl[32];
    std::snprintf(lbl, sizeof lbl, "d=%ld", s_dt_seq - 1);
    PixWatch(lbl);
    }
  }
}

// Uniform locations of a TEV program; samplers fixed per program (map i
// always lives on unit i).
static GLShader MakeShader(GLuint id, bool xv) {
  GLShader sh = {};
  sh.id = sh.fprog = id;
  LocateUniforms(sh, id);
  if (sso::s_on) {
    // `id` is the separable fragment program: pair it with the shared vertex
    // program of its path (classic or GPU XF) in a pipeline (sh.id). The
    // VS-side uniforms live in sso::Vsh(xv).
    GLuint pp = 0;
    if (id) {
      sso::pGen(1, &pp);
      sso::pStages(pp, sso::kVsBit, sso::VProg(xv));
      sso::pStages(pp, sso::kFsBit, id);
    }
    sh.id = pp;
    for (int i = 0; i < 8; ++i)
      if (sh.uTex[i] >= 0) sso::pU1i(id, sh.uTex[i], i);
    return sh;
  }
  glUseProgram(id);
  g_gl.program = id;
  for (int i = 0; i < 8; ++i)
    if (sh.uTex[i] >= 0) glUniform1i(sh.uTex[i], i);
  if (sh.uXW >= 0) glUniform1i(sh.uXW, 12);  // GPU XF buffer textures
  if (sh.uXQ >= 0) glUniform1i(sh.uXQ, 13);
  return sh;
}

static void SetupDrawState(const GXCommand &cmd) {
  static const bool use_shader = (NWII_ENV("NWII_NOSHADER") == nullptr);
  if (!use_shader)
    return;

  // The primitive type does not reach the shader (everything is drawn as
  // triangles), so it is left out of the hash: quads, strips and fans batch.
  (void)cmd;
  // R11: the GPU XF path has its own programs (same fragment shaders, its own
  // vertex shader): its state keys are salted.
  const bool xv = xf::Active();
  const uint64_t hash0 = s_shader_hash_ok ? s_shader_hash : g_state.GetShaderHash(0);
  const uint64_t hash = xv ? hash0 ^ 0x5851F42D4C957F2Dull : hash0;

  if (!g_stat_hash0)
    g_stat_hash0 = hash0;
  g_stat_shaders = (uint64_t)s_programs.size();
  s_skip_draw = false;
  // NWII_SHADER_BUDGET=<n>: the old behaviour (skip draws whose program is not
  // ready after n new programs in a frame). Off by default: a skipped draw is
  // geometry missing from that frame -- the flicker when entering a level.
  static const int s_budget = NWII_ENV("NWII_SHADER_BUDGET")
                                  ? std::atoi(NWII_ENV("NWII_SHADER_BUDGET")) : 0;
  auto it = s_shader_cache.find(hash);
  if (it != s_shader_cache.end()) {
    s_active = it->second;
  } else if (s_budget > 0 && s_compiles_this_frame >= s_budget) {
    ++g_prof_shader_skips;
    s_skip_draw = true;
    return;
  } else {
    const auto sh_t0 = std::chrono::steady_clock::now();
    auto src = GenerateTEVShader(g_state, 0);
    const std::string &vsrc = xv ? xf::VsText() : src.vertex_source;
    const uint64_t src_key = Fnv64(src.fragment_source, Fnv64(vsrc));
    // Disk cache key (see PrewarmPrograms): separable entries are the
    // fragment source alone, keyed as with the classic vertex shader.
    const uint64_t disk_key =
        sso::s_on ? Fnv64(src.fragment_source, Fnv64(src.vertex_source)) : src_key;
    auto pit = s_programs.find(src_key);
    if (pit != s_programs.end()) {
      // Same program as an already-built state: alias it.
      s_active = s_shader_cache[hash] = &pit->second;
      goto have_program;
    }
    ++s_compiles_this_frame;
    // NWII_SHADERDIR=<dir>: write every generated shader pair to <dir>/<hash>.
    if (const char *dir = NWII_ENV("NWII_SHADERDIR")) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s/%016llx.glsl", dir,
                    (unsigned long long)hash);
      if (FILE *f = std::fopen(path, "w")) {
        std::fprintf(f, "// tex0 %ux%u fmt=%u\n%s\n// ---- FS ----\n%s",
                     g_state.texStages[0].width, g_state.texStages[0].height,
                     g_state.texStages[0].format, src.vertex_source.c_str(),
                     src.fragment_source.c_str());
        std::fclose(f);
      }
    }
    static const bool s_shdump1 = NWII_ENV("NWII_SHADERDUMP") != nullptr;
    if (s_shdump1) {
      static int sn = 0;
      if (sn++ < 2)
        printf("[SHADER %d] ==== FS ====\n%s\n============\n", sn,
               src.fragment_source.c_str());
    }
    // NWII_MEMTEST_SHADER_MS=<ms>, tests only: stall the host thread this
    // long on every in-game shader build, as the Switch's ~2 s compile does,
    // so the backlog the guest builds meanwhile can be reproduced on a PC.
    static const long s_sh_stall = [] {
      const char *e = NWII_ENV("NWII_MEMTEST_SHADER_MS");
      return e ? std::atol(e) : 0L;
    }();
    if (s_sh_stall > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(s_sh_stall));
    GLShader sh = MakeShader(BuildTevProgram(disk_key, vsrc, src.fragment_source), xv);
    s_active = s_shader_cache[hash] = &(s_programs[src_key] = sh);
    g_prof_shader_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - sh_t0).count();
    if (gx_trace())
      printf("[GXTRACE] Compiled new shader! Hash: %llx\n",
             (unsigned long long)hash);
  }
have_program:

  GLShader &sh = *s_active;
  // Vertex-stage uniforms: this program's own (linked mode) or the shared
  // separable vertex program's (sso::s_vsh, with its own last-upload record).
  GLShader &vsh = sso::s_on ? sso::Vsh(xv) : sh;
  const GLuint vp = vsh.id, fp = sh.fprog;
  CapNoteProgram(sh.id);
  if (sso::s_on) {
    CUseProgram(0);  // a bound program would override the pipeline
    CBindPipeline(sh.id);
  } else {
    CUseProgram(sh.id);
  }
  ApplyProjection(vsh);
  ApplyZMode();
  ApplyBlend();
  ApplyCullMode();
  ApplyViewport();
  ApplyScissor();

  // XF channel state changes every frame (material fades, light motion), so
  // it is uploaded per draw run rather than baked into the shader. Values
  // equal to this program's last upload are skipped.
  auto unpack = [](uint32_t c, float *o) {
    o[0] = ((c >> 24) & 0xFF) / 255.f;
    o[1] = ((c >> 16) & 0xFF) / 255.f;
    o[2] = ((c >> 8) & 0xFF) / 255.f;
    o[3] = (c & 0xFF) / 255.f;
  };
  const bool have = sh.have_last, vhave = vsh.have_last;
  if (vsh.uMatColor >= 0 || vsh.uAmbColor >= 0) {
    float mat[8], amb[8];
    unpack(g_state.xf[0x00C], mat);
    unpack(g_state.xf[0x00D], mat + 4);
    unpack(g_state.xf[0x00A], amb);
    unpack(g_state.xf[0x00B], amb + 4);
    if (vsh.uMatColor >= 0 && (!vhave || std::memcmp(vsh.last_mat, mat, sizeof(mat)))) {
      U4fv(vp, vsh.uMatColor, 2, mat);
      std::memcpy(vsh.last_mat, mat, sizeof(mat));
    }
    if (vsh.uAmbColor >= 0 && (!vhave || std::memcmp(vsh.last_amb, amb, sizeof(amb)))) {
      U4fv(vp, vsh.uAmbColor, 2, amb);
      std::memcpy(vsh.last_amb, amb, sizeof(amb));
    }
  }
  if (vsh.uChanCtrl >= 0) {
    GLint cc[4] = {(GLint)g_state.xf[0x00E], (GLint)g_state.xf[0x00F],
                   (GLint)g_state.xf[0x010], (GLint)g_state.xf[0x011]};
    if (!vhave || std::memcmp(vsh.last_cc, cc, sizeof(cc))) {
      U1iv(vp, vsh.uChanCtrl, 4, cc);
      std::memcpy(vsh.last_cc, cc, sizeof(cc));
    }
  }
  if (vsh.uLightCol >= 0 || vsh.uLightPos >= 0 || vsh.uLightDir >= 0 ||
      vsh.uLightCosAtt >= 0 || vsh.uLightDistAtt >= 0) {
    float L5[5][32];  // col, pos, dir, cosatt, distatt
    for (int i = 0; i < 8; i++) {
      const float *L = g_state.lights[i];
      uint32_t c;
      std::memcpy(&c, &L[3], 4);
      unpack(c, L5[0] + i * 4);
      for (int k = 0; k < 3; ++k) {
        L5[3][i * 4 + k] = L[4 + k];
        L5[4][i * 4 + k] = L[7 + k];
        L5[1][i * 4 + k] = L[10 + k];
        L5[2][i * 4 + k] = L[13 + k];
      }
      L5[1][i * 4 + 3] = L5[2][i * 4 + 3] = L5[3][i * 4 + 3] =
          L5[4][i * 4 + 3] = 0.f;
    }
    const GLint locs[5] = {vsh.uLightCol, vsh.uLightPos, vsh.uLightDir,
                           vsh.uLightCosAtt, vsh.uLightDistAtt};
    for (int k = 0; k < 5; ++k) {
      if (locs[k] < 0) continue;
      if (!vhave || std::memcmp(vsh.last_lights[k], L5[k], sizeof(L5[k]))) {
        U4fv(vp, locs[k], 8, L5[k]);
        std::memcpy(vsh.last_lights[k], L5[k], sizeof(L5[k]));
      }
    }
  }

  auto decode11 = [](uint32_t val, int shift) -> float {
    int v = (val >> shift) & 0x7FF;
    if (v & 0x400)
      v |= ~0x7FF;
    return (float)v / 255.0f;
  };
  float kcolor[16], color[16];
  for (int i = 0; i < 4; i++) {
    const uint32_t *c = g_state.tevColorReg[i];
    const uint32_t *k = g_state.tevKonstReg[i];
    color[i * 4 + 0] = decode11(c[0], 0);
    color[i * 4 + 3] = decode11(c[0], 12);
    color[i * 4 + 2] = decode11(c[1], 0);
    color[i * 4 + 1] = decode11(c[1], 12);
    kcolor[i * 4 + 0] = decode11(k[0], 0);
    kcolor[i * 4 + 3] = decode11(k[0], 12);
    kcolor[i * 4 + 2] = decode11(k[1], 0);
    kcolor[i * 4 + 1] = decode11(k[1], 12);
  }

  static const bool s_tevtrace = NWII_ENV("NWII_TEVTRACE") != nullptr;
  if (s_tevtrace) {
    static int tn = 0;
    if (tn++ < 4)
      printf("[TEV] bp E0=%06X E1=%06X E2=%06X E3=%06X | "
             "color0=(%.2f,%.2f,%.2f,%.2f) color1=(%.2f,%.2f,%.2f,%.2f)\n",
             g_state.bp[0xE0], g_state.bp[0xE1], g_state.bp[0xE2],
             g_state.bp[0xE3], color[0], color[1], color[2], color[3], color[4],
             color[5], color[6], color[7]);
  }
  if (sh.uTevKColor >= 0 && (!have || std::memcmp(sh.last_kon, kcolor, sizeof(kcolor)))) {
    U4fv(fp, sh.uTevKColor, 4, kcolor);
    std::memcpy(sh.last_kon, kcolor, sizeof(kcolor));
  }
  if (sh.uTevColor >= 0 && (!have || std::memcmp(sh.last_tev, color, sizeof(color)))) {
    U4fv(fp, sh.uTevColor, 4, color);
    std::memcpy(sh.last_tev, color, sizeof(color));
  }
  if (sh.uTexSize >= 0 || sh.uIndMtx >= 0) {
    // Indirect texturing constants (Dolphin PixelShaderManager): map sizes,
    // texcoord scales (SU_SSIZE/TSIZE, BP 0x30+2n), the three 2x3 matrices
    // (BP 0x06-0x0E, 11-bit signed, w = 17 - scale) and RAS1_SS shifts.
    float tsz[16], csz[16];
    for (int i = 0; i < 8; ++i) {
      tsz[i * 2] = (float)std::max<uint32_t>(1, g_state.texStages[i].width);
      tsz[i * 2 + 1] = (float)std::max<uint32_t>(1, g_state.texStages[i].height);
      csz[i * 2] = (float)((g_state.bp[0x30 + 2 * i] & 0xFFFF) + 1);
      csz[i * 2 + 1] = (float)((g_state.bp[0x31 + 2 * i] & 0xFFFF) + 1);
    }
    if (sh.uTexSize >= 0) U2fv(fp, sh.uTexSize, 8, tsz);
    if (sh.uCoordScale >= 0) U2fv(fp, sh.uCoordScale, 8, csz);
    if (sh.uIndMtx >= 0) {
      GLint m[24];
      auto s11 = [](uint32_t v) { int x = (int)(v & 0x7FF); return (x & 0x400) ? x - 0x800 : x; };
      for (int i = 0; i < 3; ++i) {
        const uint32_t a = g_state.bp[0x06 + 3 * i], b = g_state.bp[0x07 + 3 * i],
                       c = g_state.bp[0x08 + 3 * i];
        const int scale = (int)(((a >> 22) & 3) | (((b >> 22) & 3) << 2) |
                                (((c >> 22) & 3) << 4));
        GLint *r0 = &m[i * 8], *r1 = &m[i * 8 + 4];
        r0[0] = s11(a); r0[1] = s11(b); r0[2] = s11(c); r0[3] = 17 - scale;
        r1[0] = s11(a >> 11); r1[1] = s11(b >> 11); r1[2] = s11(c >> 11); r1[3] = 17 - scale;
      }
      U4iv(fp, sh.uIndMtx, 6, m);
    }
    if (sh.uIndScale >= 0) {
      const uint32_t s0 = g_state.bp[0x25], s1 = g_state.bp[0x26];
      GLint sc[8] = {(GLint)(s0 & 0xF), (GLint)((s0 >> 4) & 0xF), (GLint)((s0 >> 8) & 0xF),
                     (GLint)((s0 >> 12) & 0xF), (GLint)(s1 & 0xF), (GLint)((s1 >> 4) & 0xF),
                     (GLint)((s1 >> 8) & 0xF), (GLint)((s1 >> 12) & 0xF)};
      U4iv(fp, sh.uIndScale, 2, sc);
    }
  }
  if (!vhave) {
    // ApplyProjection ran before have_last was set: record its upload too.
    std::memcpy(vsh.last_proj, s_proj, sizeof(s_proj));
    vsh.have_last = true;
  }
  sh.have_last = true;
}

// Per-draw texgen setup hoisted out of the per-vertex loop (R5 perf: EmitVertex
// was the top host-thread symbol in Peach's party, ~12k draws per frame). The
// arithmetic is the same as TexGen()/EmitVertex(), term for term, so the
// output is bit-identical; only the XF register decode moved out of the loop.
struct TexGenCfg {
  int n;
  struct Coord {
    bool stq, abc1, mtx, post, post_norm;
    int row;
    const float *pm;  // post matrix, nullptr = 61 (identity)
  } c[4];
};

static void PrepTexGen(TexGenCfg &cfg) {
  cfg.n = g_state.numTexGens ? std::min<int>(g_state.numTexGens, 4) : 1;
  const bool dual = (g_state.xf[0x12] & 1) || !g_state.xf[0x12];
  for (int t = 0; t < cfg.n; ++t) {
    const uint32_t info = g_state.numTexGens ? g_state.xf[0x40 + t] : 0x280;
    auto &c = cfg.c[t];
    c.stq = (info >> 1) & 1;
    c.abc1 = (info >> 2) & 1;
    c.mtx = ((info >> 4) & 7) == 0;
    c.row = (info >> 7) & 0x1F;
    c.post = c.mtx && dual;
    const uint32_t post = g_state.xf[0x50 + t];
    c.post_norm = (post >> 8) & 1;
    const int pi = post & 0x3F;
    c.pm = (pi != 61) ? &g_state.postMatrices[pi * 4] : nullptr;
  }
}

static inline void TexGenFast(const VertexData &vtx, BatchVertex &bv,
                              const TexGenCfg &cfg) {
  float out[4][2] = {};
  for (int t = 0; t < cfg.n; ++t) {
    const auto &c = cfg.c[t];
    const bool stq = c.stq;
    float in[4] = {0, 0, 1, 1};
    const int row = c.row;
    if (row == 0) {
      in[0] = vtx.raw_pos[0]; in[1] = vtx.raw_pos[1]; in[2] = vtx.raw_pos[2];
    } else if (row == 1) {
      in[0] = vtx.raw_norm[0]; in[1] = vtx.raw_norm[1]; in[2] = vtx.raw_norm[2];
    } else if (row == 2) {
      in[0] = vtx.color[0]; in[1] = vtx.color[1];
    } else if (row >= 5 && row <= 12) {
      int k = row - 5;
      if (vtx.has_tex[k]) { in[0] = vtx.tex[k][0]; in[1] = vtx.tex[k][1]; }
    }
    if (!c.abc1)
      in[2] = 1.0f;
    float s = in[0], tt = in[1], q = 1.0f;
    if (c.mtx) {
      int mi = vtx.texMtxIdx[t];
      const float *m = (mi != 60 && mi * 4 + 12 <= 256)
                           ? &g_state.posMatrices[mi * 4] : nullptr;
      if (m && !(m[0] || m[1] || m[2] || m[4] || m[5] || m[6]))
        m = nullptr;
      if (m) {
        s = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + m[3] * in[3];
        tt = m[4] * in[0] + m[5] * in[1] + m[6] * in[2] + m[7] * in[3];
        q = stq ? m[8] * in[0] + m[9] * in[1] + m[10] * in[2] + m[11] * in[3]
                : 1.0f;
      } else {
        q = stq ? in[2] : 1.0f;
      }
      if (c.post) {
        float v[3] = {s, tt, q};
        if (c.post_norm) {
          float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
          if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
        }
        if (const float *pm = c.pm) {
          s = pm[0] * v[0] + pm[1] * v[1] + pm[2] * v[2] + pm[3];
          tt = pm[4] * v[0] + pm[5] * v[1] + pm[6] * v[2] + pm[7];
          q = stq ? pm[8] * v[0] + pm[9] * v[1] + pm[10] * v[2] + pm[11] : 1.0f;
        } else {
          s = v[0]; tt = v[1]; q = stq ? v[2] : 1.0f;
        }
      }
    }
    if (stq && q != 0.0f) { s /= q; tt /= q; }
    out[t][0] = s;
    out[t][1] = tt;
  }
  bv.u = out[0][0]; bv.v = out[0][1];
  bv.u1 = out[1][0]; bv.v1 = out[1][1];
  bv.u2 = out[2][0]; bv.v2 = out[2][1];
  bv.u3 = out[3][0]; bv.v3 = out[3][1];
}


// R7 perf: TexGenFast() for four vertices at once. On the world map (~77k
// vertices and ~2.8 texgens per vertex per frame, most with a normalised post
// transform) the per-vertex sqrt/divide chains of TexGenFast were ~25% of the
// host thread on a weak CPU. Each lane runs exactly the scalar arithmetic of
// TexGenFast (same operations, same order, IEEE single precision, so the same
// bits); per-vertex branches become per-lane selects. GCC vector extensions,
// so it builds for x86-64 (SSE) and the Switch (NEON) alike.
typedef float v4f __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));
static inline v4f VSqrt(v4f x) {
#if defined(__SSE2__)
  return (v4f)_mm_sqrt_ps((__m128)x);
#elif defined(__aarch64__)
  return (v4f)vsqrtq_f32((float32x4_t)x);
#else
  v4f r;
  for (int i = 0; i < 4; ++i) r[i] = std::sqrt(x[i]);
  return r;
#endif
}
static inline v4f VSel(v4i m, v4f a, v4f b) { return m ? a : b; }

// Texgen of vertices p[0..3] (padding lanes may repeat a vertex), written to
// bv[i] (i < lanes) exactly as TexGenFast(*p[i], bv[i], cfg) writes them.
static inline void TexGen4(const VertexData *const p[4], BatchVertex *bv,
                           int lanes, const TexGenCfg &cfg) {
  static const float kZeroMtx[12] = {};
  const v4f one = {1.f, 1.f, 1.f, 1.f}, zero = {};
  v4f os[4] = {}, ot[4] = {};
  for (int t = 0; t < cfg.n; ++t) {
    const auto &c = cfg.c[t];
    const bool stq = c.stq;
    v4f in0 = zero, in1 = zero, in2 = one;
    const int row = c.row;
    if (row == 0) {
      in0 = v4f{p[0]->raw_pos[0], p[1]->raw_pos[0], p[2]->raw_pos[0], p[3]->raw_pos[0]};
      in1 = v4f{p[0]->raw_pos[1], p[1]->raw_pos[1], p[2]->raw_pos[1], p[3]->raw_pos[1]};
      in2 = v4f{p[0]->raw_pos[2], p[1]->raw_pos[2], p[2]->raw_pos[2], p[3]->raw_pos[2]};
    } else if (row == 1) {
      in0 = v4f{p[0]->raw_norm[0], p[1]->raw_norm[0], p[2]->raw_norm[0], p[3]->raw_norm[0]};
      in1 = v4f{p[0]->raw_norm[1], p[1]->raw_norm[1], p[2]->raw_norm[1], p[3]->raw_norm[1]};
      in2 = v4f{p[0]->raw_norm[2], p[1]->raw_norm[2], p[2]->raw_norm[2], p[3]->raw_norm[2]};
    } else if (row == 2) {
      in0 = v4f{p[0]->color[0], p[1]->color[0], p[2]->color[0], p[3]->color[0]};
      in1 = v4f{p[0]->color[1], p[1]->color[1], p[2]->color[1], p[3]->color[1]};
    } else if (row >= 5 && row <= 12) {
      const int k = row - 5;
      for (int i = 0; i < 4; ++i)
        if (p[i]->has_tex[k]) { in0[i] = p[i]->tex[k][0]; in1[i] = p[i]->tex[k][1]; }
    }
    if (!c.abc1)
      in2 = one;
    v4f s = in0, tt = in1, q = one;
    if (c.mtx) {
      const float *m[4];
      v4i has_m = {};
      bool same = true;
      for (int i = 0; i < 4; ++i) {
        const int mi = p[i]->texMtxIdx[t];
        const float *mm = (mi != 60 && mi * 4 + 12 <= 256)
                              ? &g_state.posMatrices[mi * 4] : nullptr;
        if (mm && !(mm[0] || mm[1] || mm[2] || mm[4] || mm[5] || mm[6]))
          mm = nullptr;
        has_m[i] = mm ? -1 : 0;
        m[i] = mm ? mm : kZeroMtx;
        same = same && m[i] == m[0];
      }
      v4f mv[12];
      if (same) {
        for (int j = 0; j < 12; ++j) mv[j] = v4f{m[0][j], m[0][j], m[0][j], m[0][j]};
      } else {
        for (int j = 0; j < 12; ++j) mv[j] = v4f{m[0][j], m[1][j], m[2][j], m[3][j]};
      }
      const v4f ms = mv[0] * in0 + mv[1] * in1 + mv[2] * in2 + mv[3] * one;
      const v4f mt = mv[4] * in0 + mv[5] * in1 + mv[6] * in2 + mv[7] * one;
      const v4f mq = stq ? mv[8] * in0 + mv[9] * in1 + mv[10] * in2 + mv[11] * one : one;
      s = VSel(has_m, ms, s);
      tt = VSel(has_m, mt, tt);
      q = VSel(has_m, mq, stq ? in2 : one);
      if (c.post) {
        v4f v0 = s, v1 = tt, v2 = q;
        if (c.post_norm) {
          const v4f l = VSqrt(v0 * v0 + v1 * v1 + v2 * v2);
          const v4i pos = l > zero;
          v0 = VSel(pos, v0 / l, v0);
          v1 = VSel(pos, v1 / l, v1);
          v2 = VSel(pos, v2 / l, v2);
        }
        if (const float *pm = c.pm) {
          s = pm[0] * v0 + pm[1] * v1 + pm[2] * v2 + pm[3];
          tt = pm[4] * v0 + pm[5] * v1 + pm[6] * v2 + pm[7];
          q = stq ? pm[8] * v0 + pm[9] * v1 + pm[10] * v2 + pm[11] : one;
        } else {
          s = v0; tt = v1; q = stq ? v2 : one;
        }
      }
    }
    if (stq) {
      const v4i nz = q != zero;
      s = VSel(nz, s / q, s);
      tt = VSel(nz, tt / q, tt);
    }
    os[t] = s;
    ot[t] = tt;
  }
  for (int i = 0; i < lanes; ++i) {
    bv[i].u = os[0][i]; bv[i].v = ot[0][i];
    bv[i].u1 = os[1][i]; bv[i].v1 = ot[1][i];
    bv[i].u2 = os[2][i]; bv[i].v2 = ot[2][i];
    bv[i].u3 = os[3][i]; bv[i].v3 = ot[3][i];
  }
}
template <bool kTexGen>
static inline void EmitVertexFast(const VertexData &vtx, BatchVertex &bv,
                                  const TexGenCfg &cfg) {
  bv = BatchVertex{};
  if (vtx.has_color) {
    bv.r = vtx.color[0];
    bv.g = vtx.color[1];
    bv.b = vtx.color[2];
    bv.a = vtx.color[3];
  } else {
    bv.r = bv.g = bv.b = bv.a = 1.f;
  }
  if (vtx.has_norm && vtx.pre_xf) {
    bv.nx = vtx.norm[0]; bv.ny = vtx.norm[1]; bv.nz = vtx.norm[2];
  } else if (vtx.has_norm) {
    int nb = 3 * (vtx.posMtxIdx & 31);
    const float *nm = g_state.normalMatrices;
    if (nb + 9 <= 96) {
      bv.nx = vtx.norm[0] * nm[nb + 0] + vtx.norm[1] * nm[nb + 1] +
              vtx.norm[2] * nm[nb + 2];
      bv.ny = vtx.norm[0] * nm[nb + 3] + vtx.norm[1] * nm[nb + 4] +
              vtx.norm[2] * nm[nb + 5];
      bv.nz = vtx.norm[0] * nm[nb + 6] + vtx.norm[1] * nm[nb + 7] +
              vtx.norm[2] * nm[nb + 8];
    } else {
      bv.nx = vtx.norm[0]; bv.ny = vtx.norm[1]; bv.nz = vtx.norm[2];
    }
  }
  if (kTexGen) TexGenFast(vtx, bv, cfg);
  float x = vtx.pos[0], y = vtx.pos[1], z = vtx.pos[2];
  int mtx_base = (vtx.posMtxIdx & 0x3F) * 4;
  if (vtx.pre_xf) {
    bv.x = x; bv.y = y; bv.z = z;
  } else if (vtx.has_pos && mtx_base + 12 <= 256) {
    const float *mm = g_state.posMatrices;
    bv.x = x * mm[mtx_base + 0] + y * mm[mtx_base + 1] + z * mm[mtx_base + 2] +
           mm[mtx_base + 3];
    bv.y = x * mm[mtx_base + 4] + y * mm[mtx_base + 5] + z * mm[mtx_base + 6] +
           mm[mtx_base + 7];
    bv.z = x * mm[mtx_base + 8] + y * mm[mtx_base + 9] + z * mm[mtx_base + 10] +
           mm[mtx_base + 11];
  } else {
    bv.x = x; bv.y = y; bv.z = z;
  }
}

// ---- R9 render: emit cache -------------------------------------------------
// A display-list draw whose decoded vertices are shared with the decode cache
// (cmd.shared_vertices: the same object frame after frame while its guest
// bytes do not change) produces exactly the same BatchVertex array as the
// last time if every g_state value EmitVertexFast/TexGen4 read is unchanged:
// the position and normal matrix rows of its matrix indices, the texgen
// registers, the texture matrices its texcoords use and the post matrices.
// On the world map (~11 000 draws per frame) ~65 % of the draws repeat their
// output while the camera stands still. The entry keeps those inputs (raw
// bits, in a fixed order) with its output; a draw whose inputs compare equal
// word for word copies the stored output instead of transforming again, so
// the vertices are the same bits by construction. (Row version counters do
// not work: games load each object's matrix into the same slot, so slots
// change between every two draws.)
// NWII_ECACHE=0 disables it; NWII_ECACHE_CHECK=1 also recomputes every hit and
// counts differences ("[ECACHE] ... mismatches=").
namespace ecache {
struct Entry {
  uint64_t pos_mask = 0;      // bit i: posMatrices[i*4, +12) (position transform)
  uint32_t nrm_mask = 0;      // bit i: normalMatrices[3*i, +9)
  uint64_t tex_mask[4] = {};  // bit mi: posMatrices[mi*4, +12) as texture matrix t
  uint32_t misses = 0;        // consecutive misses (stops storing outputs)
  // Inputs of the stored output, see ForInputs(): those of the texgen part
  // (u .. v3) and those of the rest (position, normal: the position/normal
  // matrix rows). Texgen never reads the position/normal transform, so a draw
  // that only moved (camera scroll) keeps its texgen output.
  std::vector<uint32_t> in_tg, in_pos;
  std::vector<BatchVertex> out;  // stored output (r8 vertex path)
  std::vector<float> tg;         // R9 vertex buffers: u .. v3 of each stored vertex
  // R9 vertex buffers: the stored output lives in arena slot `slot` (absolute
  // vertex index, -1 none) of `slot_n` vertices.
  int64_t slot = -1;
  uint32_t slot_n = 0;
  uint8_t cls = 0;
  // R11 GPU XF: what xf::Analyze found in these vertices (x_ok: transform on
  // the GPU possible; matrix indices; layout) and their raw attributes'
  // arena slot (16-byte units, -1 none).
  bool x_init = false;
  xf::Info xi;
  uint8_t x_cls = 0;
  int64_t x_slot = -1;
  uint32_t x_n = 0;  // R14 R5: vertex count of the draw (set with the masks)
  // r36r NWII_R_CULL: bounding box of the raw positions (min xyz, max xyz),
  // set with xi when every vertex has a position (xi.ok, xi.hp).
  bool x_bb_ok = false;
  float x_bb[6] = {};
  // r45p NWII_P_DLMERGE: the raw-position box of each part of a fused draw (6 floats a part) and
  // whether it is usable (render thread only, made at its first cull test).
  bool p_init = false;
  bool p_freed = false;  // the parts' arena slots were given back (first emit of the fused draw)
  std::vector<float> p_bb;
  std::vector<uint8_t> p_ok;
  // R14 R5, NWII_GPU_XF_MDI: the draw's triangle indices (local vertex
  // numbers) kept in the GPU XF arena like its vertices: arena unit, count,
  // the primitive they were made for.
  int64_t x_islot = -1;
  uint32_t x_icount = 0;
  uint8_t x_icls = 0, x_iprim = 0;
  ~Entry() {
    // R11 worker: the entry may die on the render worker (any thread).
    if (slot >= 0) vbuf::ArenaFreeAny((uint32_t)slot, cls);
    if (x_slot >= 0) vbuf::ArenaFreeAny((uint32_t)x_slot, x_cls, /*gpu_xf=*/true);
    if (x_islot >= 0) vbuf::ArenaFreeAny((uint32_t)x_islot, x_icls, /*gpu_xf=*/true);
  }
};
// R14 R5: plain globals set once by InitFlags() (first Render(); also read
// by the render worker, which starts after the first frame end is parsed --
// see GX_WorkerPrepDraws -- and calls InitFlags() itself if needed).
static bool s_on = true, s_check_on = false;
static inline bool On() { return s_on; }
static inline bool CheckOn() { return s_check_on; }
static uint64_t s_frame = 1;
static uint64_t s_hits = 0, s_misses = 0, s_checked = 0, s_bad = 0, s_hit_verts = 0,
                s_entries = 0, s_tg_hits = 0, s_m_noslot = 0, s_m_tg = 0;
// R14 R5: entries made by GX_PrepDrawForRender (render worker) / made late by
// Get() on the render thread (should stay 0 with the prep on).
static std::atomic<uint64_t> s_entries_prep{0};
static uint64_t s_entries_late = 0;

static void InitDeps(Entry &e, const std::vector<VertexData> &v) {
  e.x_n = (uint32_t)v.size();
  for (const auto &vtx : v) {
    if (!vtx.pre_xf) {
      if (vtx.has_pos && (vtx.posMtxIdx & 0x3F) <= 61) e.pos_mask |= 1ull << (vtx.posMtxIdx & 0x3F);
      if (vtx.has_norm && (vtx.posMtxIdx & 31) <= 29) e.nrm_mask |= 1u << (vtx.posMtxIdx & 31);
    }
    for (int t = 0; t < 4; ++t) {
      const int mi = vtx.texMtxIdx[t];
      if (mi != 60 && mi <= 61) e.tex_mask[t] |= 1ull << mi;
    }
  }
}
// The input words, in order. Texgen part: numTexGens, XF 0x12, then per
// texgen t < n XF 0x40+t and 0x50+t; per texgen t < n that uses a matrix: its
// texture-matrix rows (12 floats per index) and its post matrix (12).
// Position part: the position rows (12 per matrix index), the normal rows (9).
// `Op` sees each run of words.
template <class Op>
static inline bool Rows(uint64_t mask, const float *bank, int stride, int words, Op &op) {
  while (mask) {
    const int i = __builtin_ctzll(mask);
    mask &= mask - 1;
    if (!op((const uint32_t *)(bank + i * stride), words)) return false;
  }
  return true;
}
template <class Op>
static inline bool ForTgInputs(const Entry &e, const TexGenCfg &cfg, Op &&op) {
  const uint32_t head[2] = {g_state.numTexGens, g_state.xf[0x12]};
  if (!op(head, 2)) return false;
  for (int t = 0; t < cfg.n; ++t)
    if (!op(&g_state.xf[0x40 + t], 1) || !op(&g_state.xf[0x50 + t], 1)) return false;
  for (int t = 0; t < cfg.n; ++t) {
    const auto &c = cfg.c[t];
    if (!c.mtx) continue;
    if (!Rows(e.tex_mask[t], g_state.posMatrices, 4, 12, op)) return false;
    if (c.post && c.pm && !op((const uint32_t *)c.pm, 12)) return false;
  }
  return true;
}
template <class Op>
static inline bool ForPosInputs(const Entry &e, Op &&op) {
  return Rows(e.pos_mask, g_state.posMatrices, 4, 12, op) &&
         Rows(e.nrm_mask, g_state.normalMatrices, 3, 9, op);
}
template <class F>
static inline bool SameAs(const std::vector<uint32_t> &in, F &&walk) {
  const uint32_t *p = in.data(), *const end = p + in.size();
  const bool ok = walk([&](const uint32_t *w, int n) {
    if (end - p < n || std::memcmp(p, w, (size_t)n * 4)) return false;
    p += n;
    return true;
  });
  return ok && p == end;
}
static inline bool SameTg(const Entry &e, const TexGenCfg &cfg) {
  return SameAs(e.in_tg, [&](auto &&op) { return ForTgInputs(e, cfg, op); });
}
static inline bool SamePos(const Entry &e) {
  return SameAs(e.in_pos, [&](auto &&op) { return ForPosInputs(e, op); });
}
// Does the current state give the inputs the stored output was made with?
static inline bool Same(const Entry &e, const TexGenCfg &cfg) {
  return SameTg(e, cfg) && SamePos(e);
}
static inline void StoreTgInputs(Entry &e, const TexGenCfg &cfg) {
  e.in_tg.clear();
  ForTgInputs(e, cfg, [&](const uint32_t *w, int k) {
    e.in_tg.insert(e.in_tg.end(), w, w + k);
    return true;
  });
}
static inline void StorePosInputs(Entry &e) {
  e.in_pos.clear();
  ForPosInputs(e, [&](const uint32_t *w, int k) {
    e.in_pos.insert(e.in_pos.end(), w, w + k);
    return true;
  });
}
static inline void StoreInputs(Entry &e, const TexGenCfg &cfg) {
  StoreTgInputs(e, cfg);
  StorePosInputs(e);
}
static inline void Store(Entry &e, const TexGenCfg &cfg, const BatchVertex *v, size_t n) {
  StoreInputs(e, cfg);
  e.out.assign(v, v + n);
}
// R14 R5: the entry of the draw being rendered, from the render worker's list
// (s_hint_ok: the list matches this frame's draws; s_hint null: that draw has
// no entry). Same object as cmd.shared_vertices->emit_cache, without reading
// the DecodedVerts.
static Entry *s_hint = nullptr;
static bool s_hint_ok = false;
// Entry of a draw's shared vertices (nullptr: not cacheable).
static inline Entry *Get(const GXCommand &cmd) {
  if (!cmd.shared_vertices || !On() || s_r8_pass) return nullptr;
  if (s_hint_ok && s_hint) return s_hint;
  const DecodedVerts &dv = *cmd.shared_vertices;
  if (!dv.emit_cache) {
    // R14 R5: normally made on the render worker (GX_PrepDrawForRender)
    // before the frame is handed over. A draw it missed is drawn without an
    // entry while a worker runs (same output, the entry-less path): making
    // one here could race with the worker's prep of a later frame.
    ++s_entries_late;
    if (GX_RenderWorkerOn()) return nullptr;
    auto e = MakeHost<Entry>();
    InitDeps(*e, dv);
    dv.emit_cache = e;
    ++s_entries;
  }
  return static_cast<Entry *>(dv.emit_cache.get());
}
static void EndFrame() {
  ++s_frame;
  static const bool log =
      NWII_ENV("NWII_RPHASE") != nullptr || NWII_ENV("NWII_ECACHE_LOG") != nullptr || CheckOn();
  if (log && (s_frame % 600) == 0) {
    printf("[ECACHE] frames=600 hits=%llu misses=%llu (texgen kept %llu, no slot %llu, texgen "
           "changed %llu) hit_verts=%llu new_entries=%llu "
           "checked=%llu mismatches=%llu | vb %s arena %u/%u verts, full %llu, waits %llu, "
           "finishes %llu, inflight %zu\n",
           (unsigned long long)s_hits, (unsigned long long)s_misses,
           (unsigned long long)s_tg_hits, (unsigned long long)s_m_noslot,
           (unsigned long long)s_m_tg,
           (unsigned long long)s_hit_verts,
           (unsigned long long)(s_entries + s_entries_prep.exchange(0, std::memory_order_relaxed)),
           (unsigned long long)s_checked, (unsigned long long)s_bad,
           vbuf::s_on ? "on" : "off", vbuf::s_bump, vbuf::kArenaVerts,
           (unsigned long long)vbuf::s_arena_full, (unsigned long long)vbuf::s_waits,
           (unsigned long long)vbuf::s_finishes, vbuf::s_inflight->size());
    std::fflush(stdout);
    if (s_entries_late) printf("[ECACHE] entries made late on the render thread: %llu\n",
                               (unsigned long long)s_entries_late);
    s_hits = s_misses = s_hit_verts = s_entries = s_tg_hits = s_m_noslot = s_m_tg = 0;
  }
}
}  // namespace ecache
static void SetBoundingBox(ecache::Entry &e, const std::vector<VertexData> &v);  // r36r
static inline uint32_t DrawVertCount(const GXCommand &cmd) {
  const ecache::Entry *he = ecache::Get(cmd);
  return he ? he->x_n : (uint32_t)cmd.verts().size();
}

// R9 render: EmitVertexFast<false> for four vertices at once (position,
// normal, colour; texgen is TexGen4). Taken when the four lanes agree on
// pre_xf / has_pos / has_norm / has_color and their matrix rows are in range
// (the common case: one object, one matrix); otherwise returns false and the
// caller runs the scalar code. Each lane does exactly the scalar arithmetic
// (same operations in the same order, IEEE single precision; the Switch build
// uses -ffp-contract=off, x86-64 has no FMA by default), so the bits match.
// NWII_NOEMIT4=1 disables it.
static inline bool EmitPos4(const VertexData *const p[4], BatchVertex *bv, int lanes) {
  const VertexData &a = *p[0];
  if (a.pre_xf || !a.has_pos) return false;
  for (int i = 0; i < 4; ++i) {
    const VertexData &b = *p[i];
    if (b.pre_xf || !b.has_pos || b.has_norm != a.has_norm || b.has_color != a.has_color ||
        (b.posMtxIdx & 0x3F) > 61 || (a.has_norm && (b.posMtxIdx & 31) > 29))
      return false;
  }
  const float *const mm = g_state.posMatrices;
  const float *m[4];
  bool same = true;
  for (int i = 0; i < 4; ++i) {
    m[i] = mm + (p[i]->posMtxIdx & 0x3F) * 4;
    same = same && m[i] == m[0];
  }
  v4f mv[12];
  if (same) {
    for (int j = 0; j < 12; ++j) mv[j] = v4f{m[0][j], m[0][j], m[0][j], m[0][j]};
  } else {
    for (int j = 0; j < 12; ++j) mv[j] = v4f{m[0][j], m[1][j], m[2][j], m[3][j]};
  }
  const v4f x = {p[0]->pos[0], p[1]->pos[0], p[2]->pos[0], p[3]->pos[0]};
  const v4f y = {p[0]->pos[1], p[1]->pos[1], p[2]->pos[1], p[3]->pos[1]};
  const v4f z = {p[0]->pos[2], p[1]->pos[2], p[2]->pos[2], p[3]->pos[2]};
  const v4f ox = x * mv[0] + y * mv[1] + z * mv[2] + mv[3];
  const v4f oy = x * mv[4] + y * mv[5] + z * mv[6] + mv[7];
  const v4f oz = x * mv[8] + y * mv[9] + z * mv[10] + mv[11];
  v4f onx = {}, ony = {}, onz = {};
  if (a.has_norm) {
    const float *const nm = g_state.normalMatrices;
    const float *q[4];
    bool nsame = true;
    for (int i = 0; i < 4; ++i) {
      q[i] = nm + 3 * (p[i]->posMtxIdx & 31);
      nsame = nsame && q[i] == q[0];
    }
    v4f nv[9];
    if (nsame) {
      for (int j = 0; j < 9; ++j) nv[j] = v4f{q[0][j], q[0][j], q[0][j], q[0][j]};
    } else {
      for (int j = 0; j < 9; ++j) nv[j] = v4f{q[0][j], q[1][j], q[2][j], q[3][j]};
    }
    const v4f n0 = {p[0]->norm[0], p[1]->norm[0], p[2]->norm[0], p[3]->norm[0]};
    const v4f n1 = {p[0]->norm[1], p[1]->norm[1], p[2]->norm[1], p[3]->norm[1]};
    const v4f n2 = {p[0]->norm[2], p[1]->norm[2], p[2]->norm[2], p[3]->norm[2]};
    onx = n0 * nv[0] + n1 * nv[1] + n2 * nv[2];
    ony = n0 * nv[3] + n1 * nv[4] + n2 * nv[5];
    onz = n0 * nv[6] + n1 * nv[7] + n2 * nv[8];
  }
  for (int i = 0; i < lanes; ++i) {
    BatchVertex &o = bv[i];
    o.x = ox[i]; o.y = oy[i]; o.z = oz[i];
    if (a.has_color) {
      o.r = p[i]->color[0]; o.g = p[i]->color[1]; o.b = p[i]->color[2]; o.a = p[i]->color[3];
    } else {
      o.r = o.g = o.b = o.a = 1.f;
    }
    o.nx = onx[i]; o.ny = ony[i]; o.nz = onz[i];
    // u .. v3: written by TexGen4 (or by the caller) afterwards.
  }
  return true;
}
static bool s_emit4_on = true;  // R14 R5: set once by InitFlags()
static inline bool Emit4On() { return s_emit4_on; }

// The fast emit (EmitVertexFast + TexGen4) of v[0, n) into dst.
static inline void EmitRange(const std::vector<VertexData> &v, size_t n, BatchVertex *dst,
                             const TexGenCfg &cfg) {
  const bool e4 = Emit4On();
  for (size_t k = 0; k < n; k += 4) {
    const int lanes = (int)std::min<size_t>(4, n - k);
    const VertexData *p[4];
    for (int i = 0; i < 4; ++i) p[i] = &v[k + (i < lanes ? i : lanes - 1)];
    if (!e4 || !EmitPos4(p, dst + k, lanes))
      for (int i = 0; i < lanes; ++i)
        EmitVertexFast<false>(*p[i], dst[k + i], cfg);
    TexGen4(p, dst + k, lanes, cfg);
  }
}

// R9 vertex buffers (vbuf::s_on): same vertices as EmitPrimitive's fast path,
// written into the mapped buffer: a cached draw only adds indices to its
// arena slot; any other draw is computed in a scratch array and copied to the
// ring (or to a new arena slot, for the emit cache).
static void EmitPrimitiveVB(const GXCommand &cmd, uint8_t prim) {
  const auto &v = cmd.verts();
  const uint32_t n = (uint32_t)v.size();
  TexGenCfg cfg;
  PrepTexGen(cfg);
  ecache::Entry *ce = ecache::Get(cmd);
  static std::vector<BatchVertex> s_tmp, s_ref;
  if (s_tmp.size() < n) s_tmp.resize(std::max<size_t>(n, 2 * s_tmp.size()));
  bool tg_kept = false;  // texgen part taken from the entry (position recomputed)
  if (ce && ce->slot >= 0 && ce->slot_n == n && ecache::SameTg(*ce, cfg)) {
    if (ecache::SamePos(*ce)) {
      ++ecache::s_hits;
      ecache::s_hit_verts += n;
      if (rph::On()) ++rph::s_hits;
      ce->misses = 0;
      if (ecache::CheckOn()) {
        EmitRange(v, n, s_tmp.data(), cfg);
        ++ecache::s_checked;
        if (std::memcmp(s_tmp.data(), vbuf::s_v + ce->slot, n * sizeof(BatchVertex)) &&
            ecache::s_bad++ < 8)
          printf("[ECACHE] MISMATCH draw verts=%u\n", n);
      }
      EmitIndices(prim, (uint32_t)ce->slot, n);
      return;
    }
    // Same texgen inputs, the draw only moved: position/normal/colour as
    // EmitVertexFast<false> makes them, texgen output as stored (TexGen4
    // writes exactly u .. v3).
    const TexGenCfg &none = cfg;  // unused by EmitVertexFast<false>
    const bool e4 = Emit4On();
    for (uint32_t k = 0; k < n; k += 4) {
      const int lanes = (int)std::min<uint32_t>(4, n - k);
      const VertexData *p[4];
      for (int i = 0; i < 4; ++i) p[i] = &v[k + (i < lanes ? i : lanes - 1)];
      if (!e4 || !EmitPos4(p, &s_tmp[k], lanes))
        for (int i = 0; i < lanes; ++i) EmitVertexFast<false>(*p[i], s_tmp[k + i], none);
    }
    const float *tg = ce->tg.data();
    for (uint32_t k = 0; k < n; ++k, tg += 8) std::memcpy(&s_tmp[k].u, tg, 2 * 4),
                                              std::memcpy(&s_tmp[k].u1, tg + 2, 6 * 4);
    tg_kept = true;
    ++ecache::s_tg_hits;
    if (ecache::CheckOn()) {
      if (s_ref.size() < n) s_ref.resize(n);
      EmitRange(v, n, s_ref.data(), cfg);
      ++ecache::s_checked;
      if (std::memcmp(s_ref.data(), s_tmp.data(), n * sizeof(BatchVertex)) &&
          ecache::s_bad++ < 8)
        printf("[ECACHE] MISMATCH (texgen kept) draw verts=%u\n", n);
    }
  } else {
    if (ce) ++(ce->slot >= 0 && ce->slot_n == n ? ecache::s_m_tg : ecache::s_m_noslot);
    EmitRange(v, n, s_tmp.data(), cfg);
  }
  int64_t base = -1;
  if (ce) {
    ++ecache::s_misses;
    // Keep this output in the arena for the next frames, unless the draw
    // changes every frame (animated): then only every 16th miss tries again.
    if (ce->misses < 4 || (ce->misses & 15) == 0) {
      uint8_t cls = 0;
      base = vbuf::ArenaAlloc(n, cls);
      if (base >= 0) {
        if (ce->slot >= 0) vbuf::ArenaFree((uint32_t)ce->slot, ce->cls);
        ce->slot = base;
        ce->slot_n = n;
        ce->cls = cls;
        ecache::StorePosInputs(*ce);
        if (!tg_kept) {
          ecache::StoreTgInputs(*ce, cfg);
          ce->tg.resize((size_t)n * 8);
          float *tg = ce->tg.data();
          for (uint32_t k = 0; k < n; ++k, tg += 8) {
            std::memcpy(tg, &s_tmp[k].u, 2 * 4);
            std::memcpy(tg + 2, &s_tmp[k].u1, 6 * 4);
          }
        }
      }
    }
    ++ce->misses;
  }
  if (base < 0) base = vbuf::StreamAlloc(n);
  std::memcpy(vbuf::s_v + base, s_tmp.data(), n * sizeof(BatchVertex));
  if (rph::On()) rph::s_bytes += n * sizeof(BatchVertex);
  EmitIndices(prim, (uint32_t)base, n);
}

static void EmitPrimitive(const GXCommand &cmd) {
  const uint8_t prim = cmd.prim_type & 0xF8;
  if (prim >= 0xA8)
    return;  // lines / points: not drawn
  if (xf::Active()) {
    xf::Emit(cmd, prim);
    return;
  }
  const auto &v = cmd.verts();
  const size_t n = v.size();
  if (vbuf::s_on) {
    EmitPrimitiveVB(cmd, prim);
    return;
  }
  const uint32_t base = (uint32_t)s_batch.size();
  // NWII_NOINDEX=1: the old path (one transform per triangle corner), for
  // A/B measurements.
  static const bool s_noindex = NWII_ENV("NWII_NOINDEX") != nullptr;
  if (s_noindex) {
    static std::vector<uint32_t> tmp;
    tmp.clear();
    s_batch_idx.swap(tmp);
    EmitIndices(prim, 0, (uint32_t)n);
    s_batch_idx.swap(tmp);
    for (uint32_t k : tmp) {
      s_batch_idx.push_back((uint32_t)s_batch.size());
      EmitVertex(v[k]);
    }
    return;
  }
  // Debug paths (NWII_FLATZ, NWII_VTXTRACE, draw stats) keep the original
  // per-vertex EmitVertex(); NWII_SLOWEMIT=1 forces it for A/B checks.
  static const bool s_slow = NWII_ENV("NWII_SLOWEMIT") != nullptr ||
                             NWII_ENV("NWII_FLATZ") != nullptr ||
                             NWII_ENV("NWII_VTXTRACE") != nullptr;
  if (s_slow || DrawStatsOn()) {
    s_batch.reserve(s_batch.size() + n);
    for (const auto &vtx : v)
      EmitVertex(vtx);
  } else {
    TexGenCfg cfg;
    PrepTexGen(cfg);
    s_batch.resize(base + n);  // geometric growth (reserve(size+n) was exact)
    BatchVertex *dst = s_batch.data() + base;
    ecache::Entry *ce = ecache::Get(cmd);
    if (ce) {
      const bool hit = ce->out.size() == n && ecache::Same(*ce, cfg);
      if (hit) {
        std::memcpy(dst, ce->out.data(), n * sizeof(BatchVertex));
        ++ecache::s_hits;
        ecache::s_hit_verts += n;
        if (rph::On()) ++rph::s_hits;
        ce->misses = 0;
        if (!ecache::CheckOn()) {
          EmitIndices(prim, base, (uint32_t)n);
          return;
        }
        // NWII_ECACHE_CHECK: recompute and compare with the copy.
        static std::vector<BatchVertex> ref;
        ref.assign(dst, dst + n);
        for (size_t k = 0; k < n; k += 4) {
          const int lanes = (int)std::min<size_t>(4, n - k);
          const VertexData *p[4];
          for (int i = 0; i < 4; ++i) p[i] = &v[k + (i < lanes ? i : lanes - 1)];
          for (int i = 0; i < lanes; ++i)
            EmitVertexFast<false>(*p[i], dst[k + i], cfg);
          TexGen4(p, dst + k, lanes, cfg);
        }
        ++ecache::s_checked;
        if (std::memcmp(ref.data(), dst, n * sizeof(BatchVertex))) {
          if (ecache::s_bad++ < 8)
            printf("[ECACHE] MISMATCH draw verts=%zu\n", n);
        }
        EmitIndices(prim, base, (uint32_t)n);
        return;
      }
      ++ecache::s_misses;
    }
    // NWII_NOTEXGEN4=1: the one-vertex-at-a-time texgen (A/B checks).
    static const bool s_tg1 = NWII_ENV("NWII_NOTEXGEN4") != nullptr;
    if (s_tg1) {
      for (size_t k = 0; k < n; ++k)
        EmitVertexFast<true>(v[k], dst[k], cfg);
    } else {
      for (size_t k = 0; k < n; k += 4) {
        const int lanes = (int)std::min<size_t>(4, n - k);
        const VertexData *p[4];
        for (int i = 0; i < 4; ++i) p[i] = &v[k + (i < lanes ? i : lanes - 1)];
        for (int i = 0; i < lanes; ++i)
          EmitVertexFast<false>(*p[i], dst[k + i], cfg);
        TexGen4(p, dst + k, lanes, cfg);
      }
    }
    if (ce) {
      // Keep this output for the next frame, unless the draw changes every
      // frame (animated): then only every 16th miss stores it again.
      if (ce->misses < 4 || (ce->misses & 15) == 0) {
        ecache::Store(*ce, cfg, dst, n);
      } else {
        ce->out.clear();
      }
      ++ce->misses;
    }
    // NWII_EMITCHECK=1: also run the original EmitVertex and compare bytes.
    static const bool s_check = NWII_ENV("NWII_EMITCHECK") != nullptr;
    if (s_check) {
      static uint64_t checked = 0, bad = 0;
      for (size_t k = 0; k < n; ++k) {
        EmitVertex(v[k]);
        const BatchVertex ref = s_batch.back();
        s_batch.pop_back();
        if (std::memcmp(&ref, &s_batch[base + k], sizeof(BatchVertex))) ++bad;
        if (++checked % 1000000 == 0)
          printf("[EMITCHECK] vertices=%llu mismatches=%llu\n",
                 (unsigned long long)checked, (unsigned long long)bad);
      }
    }
  }
  EmitIndices(prim, base, (uint32_t)n);
}

// ---- R11 GPU XF: vertex transform and texgen in the vertex shader -----------
// NWII_GPU_XF=0 keeps the CPU path (EmitPrimitiveVB / r8). Otherwise every
// draw goes through the GPU XF vertex shader (GenerateVertexShaderXF), which
// pulls its vertices from one persistent buffer seen as two buffer textures:
//   * a draw entry per GX draw (16 bytes, written at FlushBatch): where its
//     vertices are, their layout, its position/normal matrix and its texgen
//     record. Index values are (draw slot in the batch << 16) | vertex, so a
//     batch is still one glDrawElements; uXDraw = the batch's first entry.
//   * "GPU" draws carry their raw attributes (position, normal, RGBA8 colour,
//     texcoords): a display-list draw keeps them in an arena slot for as long
//     as its decoded vertices live (nothing is written for it per frame but
//     its entry and its indices), any other draw writes them to the ring.
//   * per frame, in the ring: the matrices the draws use (3x4 position rows +
//     3x3 normal rows, texture matrices, post matrices), each written once
//     per frame while it does not change, and the texgen records.
//   * "CPU" draws (per-vertex matrix indices, colour-sourced texgen, NWII_PREXF)
//     are made on the CPU exactly as before (EmitRange) and their BatchVertex
//     copied to the ring; the shader passes them through.
// The shader repeats the CPU arithmetic bit for bit (see tev_shader_gen.cpp);
// a self-test at start-up checks this GPU's division, square root and
// unfused multiply-add against the CPU and turns the path off if any result
// differs. NWII_GPU_XF_CHECK=<n>: every n-th frame, each GPU draw is also run
// through transform feedback and compared with the CPU path value by value.
namespace xf {
constexpr uint32_t kArenaUnits = (24u << 20) / 16;  // 24 MB of display-list vertices
constexpr uint32_t kRingUnits = (24u << 20) / 16;   // 24 MB ring (~10+ world-map frames)
constexpr uint64_t kBatchCapUnits = (2u << 20) / 16;  // a batch past ~2 MB is drawn early
constexpr int kClasses = 69;
static int s_variant = 0;
static std::string *s_vs = nullptr;
// R14 R5: the batch's first draw entry passed as the draw's base instance
// (gl_BaseInstanceARB) instead of the uXDraw uniform. NWII_GPU_XF_BASEINST=0: uniform.
typedef void(APIENTRYP PFNDrawElementsInstancedBaseInstance)(GLenum, GLsizei, GLenum, const void *,
                                                             GLsizei, GLuint);
typedef void(APIENTRYP PFNDrawArraysInstancedBaseInstance)(GLenum, GLint, GLsizei, GLsizei, GLuint);
static PFNDrawElementsInstancedBaseInstance s_draw_bi = nullptr;
static bool s_baseinst = false;
static int s_base_mode = 0;  // 0 uniform, 1 gl_BaseInstanceARB, 2 instanced attribute
static bool s_r2_xdubo = false;  // r39r2 NWII_R2_XDUBO: uniform mode, the base in a uniform block
static GLuint s_iota = 0;    // mode 2: element i = kArenaUnits + i
// The base instance of draw entry unit `du` (always in the ring).
static inline GLuint BaseOf(uint64_t du) {
  return (GLuint)(s_base_mode == 2 ? du - kArenaUnits : du);
}
// R14 R5, NWII_GPU_XF_MDI=1 (needs the base instance): a batch is one
// glMultiDrawElementsIndirect, one sub-draw per GX draw (base instance = its
// draw entry). A display-list draw's indices stay in the arena (written once);
// only its 20-byte indirect command is written per frame. Other draws' indices
// go to the ring as before, numbered from 0.
typedef void(APIENTRYP PFNMultiDrawElementsIndirect)(GLenum, GLenum, const void *, GLsizei, GLsizei);
#ifndef GL_DRAW_INDIRECT_BUFFER
#define GL_DRAW_INDIRECT_BUFFER 0x8F3F  // GL 4.0 (glad here is 3.3)
#endif
static PFNMultiDrawElementsIndirect s_mdi_draw = nullptr;
static bool s_mdi = false;
static std::vector<uint32_t> s_icmds;  // the batch's indirect commands, 5 words each
static constexpr uint32_t kRingIdx = 0x80000000u;  // firstIndex relative to the batch's ring indices
static uint64_t st_mdi_static = 0, st_mdi_ring = 0;
static GLuint s_buf = 0, s_vao = 0, s_texw = 0, s_texq = 0;
static uint8_t *s_map = nullptr;
struct Frame { uint64_t id; GLsync fence; uint64_t r0; };
static std::deque<Frame> *s_inflight = new std::deque<Frame>();
static uint64_t s_frame = 1, s_done = 0, s_rw = 0, s_frame_r0 = 0, s_batch_r0 = 0;
static uint32_t s_bump = 0;
struct Retired { uint64_t frame; uint32_t off; uint8_t cls; };
static std::vector<uint32_t> *s_free = new std::vector<uint32_t>[kClasses];
static std::deque<Retired> *s_retired = new std::deque<Retired>();
static std::vector<uint32_t> s_draws;  // the batch's draw entries, 4 words each
static uint64_t s_epoch = 1;           // per Render(): matrix palette validity
// Counters, printed every 600 frames ([GPUXF]).
static uint64_t st_packed = 0;  // R14 R5: ring draws packed on the worker
static uint64_t st_frames = 0, st_gpu = 0, st_cached = 0, st_cached_new = 0, st_cpu = 0,
                st_cpu_static = 0, st_cpu_row2 = 0, st_sverts = 0, st_cverts = 0, st_pal = 0,
                st_tg = 0, st_units = 0, st_waits = 0, st_finishes = 0, st_arena_full = 0,
                st_flushes = 0;

static const std::string &VsText() { return *s_vs; }
// R14 R5: s_on as seen by the render worker (GX_PrepDrawForRender packs the
// vertices of non-display-list draws only while GPU XF is in use).
static std::atomic<bool> s_on_w{false};
static void Disable(const char *why) {
  if (s_on) printf("[GPUXF] off: %s\n", why);
  s_on = false;
  s_on_w.store(false, std::memory_order_relaxed);
}
static bool Wanted() {
  const char *e = NWII_ENV("NWII_GPU_XF");
  if (e && e[0] == '0') { printf("[GPUXF] off (NWII_GPU_XF=0)\n"); return false; }
  // Debug paths that read or rebuild s_batch / BatchVertex, and the
  // parse-time transform, keep the CPU path.
  static const char *const kDebug[] = {"NWII_NOINDEX", "NWII_SLOWEMIT", "NWII_FLATZ",
                                       "NWII_VTXTRACE", "NWII_DRAWSTATS", "NWII_DRAWTRACE",
                                       "NWII_PIXWATCH", "NWII_EMITCHECK", "NWII_NOTEXGEN4",
                                       "NWII_GXTRACE", "NWII_PREXF", "NWII_NOSHADER"};
  for (const char *d : kDebug)
    if (std::getenv(d)) { printf("[GPUXF] off (%s)\n", d); return false; }
  return true;
}
static int CheckEvery() {
  static const int n = [] {
    const char *e = NWII_ENV("NWII_GPU_XF_CHECK");
    return e ? std::max(1, std::atoi(e)) : 0;
  }();
  return n;
}

// ---- buffers ----
static inline bool Signaled(GLsync f) {
  const GLenum r = glClientWaitSync(f, 0, 0);
  return r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
}
static void PopOldest(bool wait) {
  Frame &f = s_inflight->front();
  if (wait && !Signaled(f.fence)) {
    ++st_waits;
    while (glClientWaitSync(f.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) ==
           GL_TIMEOUT_EXPIRED) {
    }
  }
  glDeleteSync(f.fence);
  s_done = f.id;
  s_inflight->pop_front();
}
// Ring data at virtual positions below `lim` is about to be overwritten: the
// frames that wrote it must be complete. If the frame being built wrote there
// itself (it alone went round the ring), everything submitted so far is
// finished; the batch still pending keeps what it wrote (bounded by
// kBatchCapUnits, far below the ring size).
static void Reserve(uint64_t lim) {
  while (!s_inflight->empty() && s_inflight->front().r0 < lim) PopOldest(true);
  if (s_frame_r0 < lim) {
    ++st_finishes;
    glFinish();
    while (!s_inflight->empty()) PopOldest(false);
    s_frame_r0 = s_batch_r0;
  }
}
// n consecutive ring units; returns the absolute unit index of the first.
static inline uint64_t Alloc(uint32_t n) {
  uint64_t w = s_rw;
  const uint64_t phys = w % kRingUnits;
  if (phys + n > kRingUnits) w += kRingUnits - phys;  // never wrap inside
  if (w + n > kRingUnits) Reserve(w + n - kRingUnits);
  s_rw = w + n;
  st_units += n;
  return kArenaUnits + (w % kRingUnits);
}
static inline int ClassOf(uint32_t n) {
  if (n <= 256) return (int)((n + 3) / 4) - 1;
  int c = 64;
  for (uint32_t sz = 512; sz < n; sz <<= 1) ++c;
  return c;
}
static inline uint32_t ClassSize(int c) { return c < 64 ? (uint32_t)(c + 1) * 4 : 512u << (c - 64); }
// An arena slot of n units (unit index), or -1 (full / too big).
static int64_t ArenaAlloc(uint32_t n, uint8_t &cls) {
  if (n == 0 || n > ClassSize(kClasses - 1)) return -1;
  cls = (uint8_t)ClassOf(n);
  auto &fl = s_free[cls];
  if (!fl.empty()) {
    const uint32_t off = fl.back();
    fl.pop_back();
    return off;
  }
  const uint32_t sz = ClassSize(cls);
  if (s_bump + sz > kArenaUnits) { ++st_arena_full; return -1; }
  const uint32_t off = s_bump;
  s_bump += sz;
  return off;
}
// The slot may still be read by draws of the frame being built (or earlier).
static void ArenaFree(uint32_t off, uint8_t cls) { s_retired->push_back({s_frame, off, cls}); }
static void Recycle() {
  while (!s_retired->empty() && s_retired->front().frame <= s_done) {
    s_free[s_retired->front().cls].push_back(s_retired->front().off);
    s_retired->pop_front();
  }
}

// ---- self-test -------------------------------------------------------------
static GLuint CompileVs(const std::string &src, bool report) {
  GLuint sh = glCreateShader(GL_VERTEX_SHADER);
  const char *c = src.c_str();
  glShaderSource(sh, 1, &c, nullptr);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    if (report) {
      char log[2048] = {0};
      glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
      printf("[GPUXF] vertex shader failed to compile: %s\n", log);
    }
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}
// A vertex-only program whose outputs `vars` go to transform feedback.
// r15dk: deko3d no tiene transform feedback. Con dkgl, LinkTf/RunTf lo emulan para salidas "flat out uint"
// (las de la autoprueba y de las pruebas de base instance): cada punto escribe sus palabras + (índice+1) en un
// píxel de un destino RGBA32UI de 2048 de ancho, que se lee con glReadPixels.
static std::unordered_map<GLuint, int> *s_tfdk_nvars = new std::unordered_map<GLuint, int>();
static GLuint LinkTfDk(const std::string &src, const char *const *vars, int nvars) {
  if (nvars > 3) return 0;
  std::string vs = src;
  size_t last = std::string::npos;
  for (int i = 0; i < nvars; ++i) {
    const std::string decl = std::string("flat out uint ") + vars[i] + ";\n";
    const size_t at = vs.find(decl);
    if (at == std::string::npos) {
      printf("[GPUXF] transform feedback on deko3d: only 'flat out uint' outputs (%s)\n", vars[i]);
      return 0;
    }
    last = std::max(last == std::string::npos ? 0 : last, at + decl.size());
  }
  vs.insert(last, "flat out uint oCase;\n");
  const size_t m = vs.find("void main() {");
  if (m == std::string::npos) return 0;
  vs.insert(m + 13, "\n    oCase = uint(gl_VertexID) + 1u;\n"
                    "    gl_Position = vec4((float(gl_VertexID & 2047) + 0.5) / 1024.0 - 1.0, "
                    "(float(gl_VertexID >> 11) + 0.5) / 1024.0 - 1.0, 0.0, 1.0);\n");
  std::string fs = "#version 330 core\n";
  for (int i = 0; i < nvars; ++i) fs += std::string("flat in uint ") + vars[i] + ";\n";
  fs += "flat in uint oCase;\nout uvec4 FragColor;\nvoid main() { FragColor = uvec4(";
  for (int i = 0; i < 3; ++i) fs += i < nvars ? std::string(vars[i]) + ", " : std::string("0u, ");
  fs += "oCase); }\n";
  GLuint p = CompileShader(vs.c_str(), fs.c_str());
  if (p) (*s_tfdk_nvars)[p] = nvars;
  return p;
}
static void RunTfDk(GLuint p, uint32_t count, uint32_t words, std::vector<uint32_t> &out) {
  out.assign((size_t)count * words, 0xDEADBEEFu);
  GLint fbo0 = 0, vp0[4];
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo0);
  glGetIntegerv(GL_VIEWPORT, vp0);
  const uint32_t rows = std::max<uint32_t>(1, (count + 2047) / 2048);
  GLuint tex = 0, fbo = 0;
  glGenTextures(1, &tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, 2048, (GLsizei)rows, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glViewport(0, 0, 2048, 2048);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_COLOR_LOGIC_OP);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(p);
  glDrawArrays(GL_POINTS, 0, (GLsizei)count);
  std::vector<uint32_t> px((size_t)2048 * rows * 4);
  glReadPixels(0, 0, 2048, (GLsizei)rows, GL_RGBA_INTEGER, GL_UNSIGNED_INT, px.data());
  for (size_t i = 0; i < (size_t)2048 * rows; ++i) {
    const uint32_t c = px[i * 4 + 3];
    if (!c || c > count) continue;
    for (uint32_t k = 0; k < words && k < 3; ++k) out[(size_t)(c - 1) * words + k] = px[i * 4 + k];
  }
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo0);
  glViewport(vp0[0], vp0[1], vp0[2], vp0[3]);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &tex);
}

static GLuint LinkTf(const std::string &src, const char *const *vars, int nvars) {
  if (GX_DkActive()) return LinkTfDk(src, vars, nvars);
  GLuint sh = CompileVs(src, true);
  if (!sh) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, sh);
  glTransformFeedbackVaryings(p, nvars, vars, GL_INTERLEAVED_ATTRIBS);
  glLinkProgram(p);
  glDetachShader(p, sh);
  glDeleteShader(sh);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048] = {0};
    glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
    printf("[GPUXF] transform-feedback program failed to link: %s\n", log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}
// Runs `count` points of program p with transform feedback into tf (n words
// per point), returns the words.
static void RunTf(GLuint p, GLuint tf, uint32_t count, uint32_t words, std::vector<uint32_t> &out) {
  if (GX_DkActive()) { RunTfDk(p, count, words, out); return; }
  glUseProgram(p);
  glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, tf);
  glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, (GLsizeiptr)count * words * 4, nullptr, GL_STREAM_READ);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tf);
  glEnable(GL_RASTERIZER_DISCARD);
  glBeginTransformFeedback(GL_POINTS);
  glDrawArrays(GL_POINTS, 0, (GLsizei)count);
  glEndTransformFeedback();
  glDisable(GL_RASTERIZER_DISCARD);
  out.resize((size_t)count * words);
  glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, (GLsizeiptr)out.size() * 4, out.data());
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
}
static inline uint32_t FBits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static inline float BitsF(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
// Division, square root and the unfused 3x4 row of the shader against the CPU
// on `batches` x 65536 cases (a quarter of them near a rounding midpoint);
// with sqrt_all, also sqrt of every positive normal float. True if all agree.
static bool SelfTest(int variant, long batches, bool sqrt_all) {
  const auto t0 = std::chrono::steady_clock::now();
  const std::string src = GenerateXfSelfTestShader(variant);
  static const char *const kVars[3] = {"oDiv", "oSqrt", "oXf"};
  GLuint p = LinkTf(src, kVars, 3);
  if (!p) return false;
  GLuint vao = 0, in = 0, tex = 0, tf = 0;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &in);
  glGenBuffers(1, &tf);
  glGenTextures(1, &tex);
  glActiveTexture(GL_TEXTURE0 + 12);
  glBindTexture(GL_TEXTURE_BUFFER, tex);
  glUseProgram(p);
  glUniform1i(glGetUniformLocation(p, "uXW"), 12);
  const GLint uBase = glGetUniformLocation(p, "uXBase"), uGen = glGetUniformLocation(p, "uXGen");
  glUniform1i(uBase, 0);
  glUniform1i(uGen, 0);
  constexpr uint32_t kN = 65536;
  std::vector<uint32_t> cases((size_t)kN * 12), out;
  uint64_t rs = 0x9E3779B97F4A7C15ull ^ (uint64_t)variant;
  auto rnd = [&]() {
    rs += 0x9E3779B97F4A7C15ull;
    uint64_t z = rs;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 7);
  };
  auto rf = [&](int emin, int emax, bool neg) {
    const uint32_t e = (uint32_t)(emin + (int)(rnd() % (uint32_t)(emax - emin + 1)) + 127);
    return BitsF(((neg && (rnd() & 1)) ? 0x80000000u : 0u) | (e << 23) | (rnd() & 0x7FFFFFu));
  };
  auto ulp = [](float f) { return (double)std::nextafter(std::fabs(f), INFINITY) - std::fabs(f); };
  uint64_t n = 0, bad[3] = {0, 0, 0};
  int shown = 0;
  for (long b = 0; b < batches; ++b) {
    for (uint32_t i = 0; i < kN; ++i) {
      uint32_t *c = &cases[(size_t)i * 12];
      // Operands over the whole normal range (b, x: half the cases), the
      // quotient kept normal (as texgen values are).
      const bool wide = (i & 1) != 0;
      float a = rf(-30, 30, true), d = rf(-30, 30, true), x = rf(-40, 40, false);
      if (wide) {
        a = rf(-126, 127, true);
        x = rf(-126, 127, false);
        const int ea = (int)((FBits(a) >> 23) & 0xFF) - 127;
        d = rf(std::max(-126, ea - 120), std::min(127, ea + 120), true);
      }
      if ((i & 3) == 0) {
        // a/b and sqrt(x) close to a midpoint between two floats.
        // (Over the whole range in every 8th case.)
        const bool w = (i & 7) == 4;
        d = w ? rf(-60, 60, true) : rf(-10, 10, true);
        const float q = w ? rf(-60, 60, true) : rf(-10, 10, true);
        const double mid = (double)q + (rnd() & 1 ? 0.5 : -0.5) * ulp(q);
        a = (float)((double)d * mid);
        const float y = w ? rf(-62, 63, false) : rf(-15, 15, false);
        const double m2 = (double)y + 0.5 * ulp(y);
        x = (float)(m2 * m2);
      }
      c[0] = FBits(a); c[1] = FBits(d); c[2] = FBits(x);
      for (int k = 3; k < 6; ++k) c[k] = FBits(rf(-8, 8, true));
      for (int k = 6; k < 10; ++k) c[k] = FBits(rf(-4, 4, true));
      c[10] = c[11] = 0;
    }
    glBindBuffer(GL_TEXTURE_BUFFER, in);
    glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)cases.size() * 4, cases.data(), GL_STREAM_DRAW);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, in);
    RunTf(p, tf, kN, 3, out);
    for (uint32_t i = 0; i < kN; ++i) {
      const uint32_t *c = &cases[(size_t)i * 12];
      volatile float a = BitsF(c[0]), d = BitsF(c[1]), x = BitsF(c[2]);
      const float q = a / d;
      const float r = std::sqrt((float)x);
      volatile float t0 = BitsF(c[3]) * BitsF(c[6]);
      volatile float t1 = BitsF(c[4]) * BitsF(c[7]);
      volatile float t2 = BitsF(c[5]) * BitsF(c[8]);
      volatile float t01 = t0 + t1;
      volatile float t012 = t01 + t2;
      const float t = t012 + BitsF(c[9]);
      const uint32_t want[3] = {FBits(q), FBits(r), FBits(t)};
      for (int k = 0; k < 3; ++k) {
        if (out[(size_t)i * 3 + k] == want[k]) continue;
        ++bad[k];
        if (shown++ < 6)
          printf("[GPUXF] self-test variant %d %s: inputs %08X %08X %08X got %08X want %08X\n",
                 variant, k == 0 ? "div" : k == 1 ? "sqrt" : "row", c[0], c[1], c[2],
                 out[(size_t)i * 3 + k], want[k]);
      }
    }
    n += kN;
  }
  uint64_t nsq = 0, badsq = 0;
  if (sqrt_all) {
    // Every positive normal float, 4M at a time (the generated input).
    static const char *const kVarSq[1] = {"oSqrt"};
    GLuint ps = LinkTf(src, kVarSq, 1);
    if (ps) {
      glUseProgram(ps);
      glUniform1i(glGetUniformLocation(ps, "uXW"), 12);
      const GLint ub = glGetUniformLocation(ps, "uXBase"), ug = glGetUniformLocation(ps, "uXGen");
      glUniform1i(ug, 1);
      constexpr uint32_t kChunk = 1u << 22;
      for (uint64_t base = 0x00800000u; base < 0x7F800000u; base += kChunk) {
        const uint32_t cnt = (uint32_t)std::min<uint64_t>(kChunk, 0x7F800000u - base);
        glUniform1i(ub, (GLint)(uint32_t)base);
        RunTf(ps, tf, cnt, 1, out);
        for (uint32_t i = 0; i < cnt; ++i) {
          const float r = std::sqrt(BitsF((uint32_t)base + i));
          if (out[i] != FBits(r) && badsq++ < 6)
            printf("[GPUXF] self-test variant %d sqrt(%08X) got %08X want %08X\n", variant,
                   (uint32_t)base + i, out[i], FBits(r));
        }
        nsq += cnt;
      }
      glDeleteProgram(ps);
    }
  }
  glUseProgram(0);
  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_BUFFER, 0);
  glActiveTexture(GL_TEXTURE0);
  glDeleteTextures(1, &tex);
  glDeleteBuffers(1, &in);
  glDeleteBuffers(1, &tf);
  glDeleteVertexArrays(1, &vao);
  glDeleteProgram(p);
  const bool ok = !bad[0] && !bad[1] && !bad[2] && !badsq;
  printf("[GPUXF] self-test variant %d (%s): %llu cases, mismatches div=%llu sqrt=%llu row=%llu",
         variant, variant ? "opaque products" : "precise", (unsigned long long)n,
         (unsigned long long)bad[0], (unsigned long long)bad[1], (unsigned long long)bad[2]);
  if (sqrt_all)
    printf(", sqrt of all %llu positive normal floats: %llu mismatches",
           (unsigned long long)nsq, (unsigned long long)badsq);
  printf(" (%lld ms) -> %s\n",
         (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0).count(),
         ok ? "ok" : "FAILED");
  std::fflush(stdout);
  return ok;
}

static void Init() {
  static bool done = false;
  if (done) return;
  done = true;
  if (!Wanted()) return;
  GLint major = 0, minor = 0, next = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  glGetIntegerv(GL_NUM_EXTENSIONS, &next);
  bool storage = major > 4 || (major == 4 && minor >= 4), shader5 = major >= 4;
  for (GLint i = 0; i < next; i++) {
    const char *x = (const char *)glGetStringi(GL_EXTENSIONS, i);
    if (!x) continue;
    if (!std::strcmp(x, "GL_ARB_buffer_storage")) storage = true;
    if (!std::strcmp(x, "GL_ARB_gpu_shader5")) shader5 = true;
  }
  GLint maxtb = 0;
  glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxtb);
  glGetError();
  auto pStorage = (vbuf::PFNBufferStorage)GX_GLProc("glBufferStorage");
  const size_t bytes = (size_t)(kArenaUnits + kRingUnits) * 16;
  const char *why = nullptr;
  if (!storage || !pStorage) why = "no ARB_buffer_storage";
  else if (!shader5) why = "no ARB_gpu_shader5";
  else if ((uint64_t)(uint32_t)maxtb < bytes / 4) why = "buffer textures too small";
  if (why) { printf("[GPUXF] off: %s\n", why); return; }
  // The arithmetic of this GPU and driver, checked against the CPU.
  const char *st = NWII_ENV("NWII_GPU_XF_SELFTEST");
  const long batches = st ? std::max(1L, std::atol(st)) : 1;
  const bool sqrt_all = NWII_ENV("NWII_GPU_XF_SQRT_ALL") != nullptr;
  int variant = -1;
  const char *fv = NWII_ENV("NWII_GPU_XF_VARIANT");
  // (con deko3d, LinkTf/RunTf emulan el transform feedback con puntos a un destino RGBA32UI)
  for (int v = fv ? std::atoi(fv) : 0; v < 2 && variant < 0; ++v)
    if (SelfTest(v, batches, sqrt_all)) variant = v;
  glGetError();
  if (variant < 0) { printf("[GPUXF] off: the GPU does not compute like the CPU\n"); return; }
  // One buffer: [arena | ring], mapped for good.
  const GLbitfield fl = GL_MAP_WRITE_BIT | vbuf::kMapPersistent | vbuf::kMapCoherent;
  glGenBuffers(1, &s_buf);
  glBindBuffer(GL_TEXTURE_BUFFER, s_buf);
  pStorage(GL_TEXTURE_BUFFER, (GLsizeiptr)bytes, nullptr, fl);
  s_map = (uint8_t *)glMapBufferRange(GL_TEXTURE_BUFFER, 0, (GLsizeiptr)bytes, fl);
  glGenTextures(1, &s_texw);
  glGenTextures(1, &s_texq);
  glActiveTexture(GL_TEXTURE0 + 12);
  glBindTexture(GL_TEXTURE_BUFFER, s_texw);
  glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, s_buf);
  glActiveTexture(GL_TEXTURE0 + 13);
  glBindTexture(GL_TEXTURE_BUFFER, s_texq);
  glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, s_buf);
  glActiveTexture(GL_TEXTURE0);
  glBindBuffer(GL_TEXTURE_BUFFER, 0);
  glGenVertexArrays(1, &s_vao);
  glBindVertexArray(s_vao);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_buf);  // indices live in the ring
  glBindVertexArray(0);
  if (false) {  // (moved: set up once the draw base is chosen, below)
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_iota);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, 0, nullptr);
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }
  const GLenum err = glGetError();
  if (!s_map || err != GL_NO_ERROR) {
    printf("[GPUXF] off: buffer mapping failed (err 0x%x)\n", err);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteTextures(1, &s_texw);
    glDeleteTextures(1, &s_texq);
    glDeleteBuffers(1, &s_buf);
    s_vao = s_texw = s_texq = s_buf = 0;
    s_map = nullptr;
    return;
  }
  s_variant = variant;
  {
    // R14 R5: the batch's first draw entry as the draw's base instance, read
    // in the shader as gl_BaseInstanceARB (mode 1) or as an instanced
    // attribute fetched from an "iota" buffer at the start instance (mode 2:
    // no shader draw parameters needed); else the uXDraw uniform (mode 0).
    // Each is checked with transform feedback first; the results are logged
    // ("[GPUXF] draw-base tests: ...") so a console that fails one can be read.
    // NWII_GPU_XF_BASEINST=0: uniform; =1 / =2: only that mode is tried.
    const char *bi = NWII_ENV("NWII_GPU_XF_BASEINST");
    bool dp = major > 4 || (major == 4 && minor >= 6), binst = major > 4 || (major == 4 && minor >= 2);
    for (GLint i = 0; i < next; i++) {
      const char *x = (const char *)glGetStringi(GL_EXTENSIONS, i);
      if (!x) continue;
      if (!std::strcmp(x, "GL_ARB_shader_draw_parameters")) dp = true;
      if (!std::strcmp(x, "GL_ARB_base_instance")) binst = true;
    }
    auto pe = (PFNDrawElementsInstancedBaseInstance)GX_GLProc(
        "glDrawElementsInstancedBaseInstance");
    auto pa = (PFNDrawArraysInstancedBaseInstance)GX_GLProc(
        "glDrawArraysInstancedBaseInstance");
    const bool want0 = bi && bi[0] == '0';
    const bool only1 = bi && bi[0] == '1', only2 = bi && bi[0] == '2';
    // Runs program p on 4 points with transform feedback of one uint ("oB").
    // how: 0 glDrawArrays, 1 glDrawArraysInstancedBaseInstance(base 777000).
    auto run = [&](GLuint p, int how, GLuint vao, uint32_t out[4]) {
      GLuint tf = 0;
      glGenBuffers(1, &tf);
      glBindVertexArray(vao);
      glUseProgram(p);
      glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, tf);
      glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 16, nullptr, GL_STREAM_READ);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tf);
      glEnable(GL_RASTERIZER_DISCARD);
      glBeginTransformFeedback(GL_POINTS);
      if (how == 0) glDrawArrays(GL_POINTS, 0, 4);
      else pa(GL_POINTS, 0, 4, 1, 777000u);
      glEndTransformFeedback();
      glDisable(GL_RASTERIZER_DISCARD);
      for (int k = 0; k < 4; ++k) out[k] = 0xDEADBEEFu;
      glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 16, out);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
      glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
      glUseProgram(0);
      glBindVertexArray(0);
      glDeleteBuffers(1, &tf);
      return glGetError();
    };
    static const char *const kVar[1] = {"oB"};
    std::string log;
    char buf[160];
    GLuint tvao = 0;
    glGenVertexArrays(1, &tvao);
    glGetError();
    if (!want0 && !only2 && dp && binst && pa && pe) {
      const GLuint p = LinkTf("#version 330 core\n"
                              "#extension GL_ARB_shader_draw_parameters : require\n"
                              "flat out uint oB;\n"
                              "void main() { oB = uint(gl_BaseInstanceARB) + uint(gl_VertexID) * 1000000u; "
                              "gl_Position = vec4(0.0); }\n",
                              kVar, 1);
      if (p) {
        uint32_t o0[4], o1[4];
        const GLenum e0 = run(p, 0, tvao, o0), e1 = run(p, 1, tvao, o1);
        bool ok = !e1;
        for (uint32_t k = 0; k < 4; ++k) ok = ok && o1[k] == 777000u + k * 1000000u;
        std::snprintf(buf, sizeof buf, " sysval: arrays %u %u %u %u (err 0x%x), base 777000 -> %u %u %u %u (err 0x%x) %s;",
                      o0[0], o0[1], o0[2], o0[3], e0, o1[0], o1[1], o1[2], o1[3], e1, ok ? "ok" : "FAIL");
        log += buf;
        glDeleteProgram(p);
        if (ok) s_base_mode = 1;
      } else {
        log += " sysval: shader rejected;";
      }
    } else if (!want0 && !only2) {
      std::snprintf(buf, sizeof buf, " sysval: n/a (draw_parameters %d base_instance %d procs %d);", dp,
                    binst, pa && pe);
      log += buf;
    }
    if (!s_base_mode && !want0 && !only1 && binst && pa && pe) {
      // Mode 2: iota buffer over the ring (entries are always in the ring):
      // element i = kArenaUnits + i, base instance = ring-relative entry.
      GLuint ib = 0;
      glGenBuffers(1, &ib);
      glBindBuffer(GL_ARRAY_BUFFER, ib);
      {
        std::vector<uint32_t> iota(kRingUnits);
        for (uint32_t i = 0; i < kRingUnits; ++i) iota[i] = kArenaUnits + i;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)iota.size() * 4, iota.data(), GL_STATIC_DRAW);
      }
      glBindVertexArray(tvao);
      glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, 0, nullptr);
      glVertexAttribDivisor(0, 1);
      glEnableVertexAttribArray(0);
      glBindVertexArray(0);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
      const GLuint p = LinkTf("#version 330 core\n"
                              "layout(location = 0) in uint aB;\n"
                              "flat out uint oB;\n"
                              "void main() { oB = aB + uint(gl_VertexID) * 1000000u; "
                              "gl_Position = vec4(0.0); }\n",
                              kVar, 1);
      bool ok = false;
      if (p) {
        uint32_t o1[4];
        const GLenum e1 = run(p, 1, tvao, o1);
        ok = !e1;
        for (uint32_t k = 0; k < 4; ++k) ok = ok && o1[k] == kArenaUnits + 777000u + k * 1000000u;
        std::snprintf(buf, sizeof buf, " attribute: base 777000 -> %u %u %u %u (err 0x%x) %s;", o1[0],
                      o1[1], o1[2], o1[3], e1, ok ? "ok" : "FAIL");
        log += buf;
        glDeleteProgram(p);
      } else {
        log += " attribute: shader rejected;";
      }
      if (ok) {
        s_base_mode = 2;
        s_iota = ib;
      } else {
        glDeleteBuffers(1, &ib);
      }
    }
    glDeleteVertexArrays(1, &tvao);
    glGetError();
    if (s_base_mode) {
      s_draw_bi = pe;
      s_baseinst = true;
    }
    if (s_base_mode == 2) {
      // attribute 0 of the GPU XF vertex array = iota[start instance] (see BaseOf)
      glBindVertexArray(s_vao);
      glBindBuffer(GL_ARRAY_BUFFER, s_iota);
      glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, 0, nullptr);
      glVertexAttribDivisor(0, 1);
      glEnableVertexAttribArray(0);
      glBindVertexArray(0);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    printf("[GPUXF] draw-base tests:%s\n", log.empty() ? " none" : log.c_str());
    printf("[GPUXF] draw base: %s\n", s_base_mode == 1   ? "base instance, gl_BaseInstanceARB (no uniform per batch)"
                                      : s_base_mode == 2 ? "base instance, instanced attribute (no uniform per batch)"
                                                         : "uniform uXDraw");
  }
  if (s_baseinst) {
    const char *mo = NWII_ENV("NWII_GPU_XF_MDI");
    auto pm = (PFNMultiDrawElementsIndirect)GX_GLProc("glMultiDrawElementsIndirect");
    const char *why_m = nullptr;
    if (!(mo && mo[0] == '1')) why_m = "off (NWII_GPU_XF_MDI=1 turns it on)";
    else if (!(major > 4 || (major == 4 && minor >= 3)) || !pm) why_m = "no glMultiDrawElementsIndirect";
    if (!why_m) {
      // Self-test: two indexed sub-draws of points with base instances 777 000
      // and 888 000, transform feedback of gl_BaseInstanceARB + 10^6 * gl_VertexID.
      static const char *const kVar[1] = {"oB"};
      const GLuint p = LinkTf("#version 330 core\n"
                              "#extension GL_ARB_shader_draw_parameters : require\n"
                              "flat out uint oB;\n"
                              "void main() { oB = uint(gl_BaseInstanceARB) + uint(gl_VertexID) * 1000000u; "
                              "gl_Position = vec4(0.0); }\n",
                              kVar, 1);
      bool ok = false;
      if (p) {
        const uint32_t idx[5] = {0, 1, 2, 0, 1};
        const uint32_t cmd[10] = {3, 1, 0, 0, 777000u, 2, 1, 3, 0, 888000u};
        GLuint tf = 0, vao = 0, ib = 0, db = 0;
        glGenBuffers(1, &tf);
        glGenBuffers(1, &ib);
        glGenBuffers(1, &db);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, db);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof cmd, cmd, GL_STATIC_DRAW);
        glUseProgram(p);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, tf);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 20, nullptr, GL_STREAM_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tf);
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS);
        pm(GL_POINTS, GL_UNSIGNED_INT, nullptr, 2, 0);
        glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);
        uint32_t out[5] = {};
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 20, out);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        glUseProgram(0);
        glBindVertexArray(0);
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &tf);
        glDeleteBuffers(1, &ib);
        glDeleteBuffers(1, &db);
        glDeleteProgram(p);
        const uint32_t want[5] = {777000u, 1777000u, 2777000u, 888000u, 1888000u};
        ok = glGetError() == GL_NO_ERROR && !std::memcmp(out, want, sizeof want);
        if (!ok)
          printf("[GPUXF] multi-draw self-test: got %u %u %u %u %u\n", out[0], out[1], out[2],
                 out[3], out[4]);
      }
      if (!ok) why_m = "self-test failed";
    }
    glGetError();
    if (!why_m) {
      s_mdi_draw = pm;
      s_mdi = true;
    }
    printf("[GPUXF] multi-draw indirect: %s\n", s_mdi ? "on (static display-list indices)" : why_m);
  }
  if (!s_base_mode) {  // r39r2 NWII_R2_XDUBO
    const char *xe = NWII_ENV("NWII_R2_XDUBO");
    if (xe && xe[0] == '1') {
      GLint al = 0;
      glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &al);
      s_r2_xdubo = al > 0 && al <= 256;
      printf("[R2] NWII_R2_XDUBO=1: draw base in a uniform block (offset alignment %d)%s\n", al,
             s_r2_xdubo ? "" : ": NOT used");
    }
  }
  s_vs = new std::string(GenerateVertexShaderXF(variant, false, s_r2_xdubo ? 3 : s_base_mode));
  // (Linked mode: the vertex shader compiles with the first program; check it
  // now so a driver that rejects it keeps the CPU path.)
  if (GLuint t = CompileVs(*s_vs, true)) {
    glDeleteShader(t);
  } else if (s_baseinst) {
    // R14 R5: back to the uniform draw base.
    s_baseinst = false;
    s_base_mode = 0;
    s_draw_bi = nullptr;
    *s_vs = GenerateVertexShaderXF(variant, false, false);
    printf("[GPUXF] draw base: uniform uXDraw (base-instance shader rejected)\n");
    if (GLuint t2 = CompileVs(*s_vs, true)) glDeleteShader(t2);
    else { printf("[GPUXF] off: vertex shader rejected\n"); return; }
  } else if (s_r2_xdubo) {  // r39r2: back to the uniform
    s_r2_xdubo = false;
    *s_vs = GenerateVertexShaderXF(variant, false, 0);
    printf("[R2] NWII_R2_XDUBO: vertex shader rejected, uniform uXDraw\n");
    if (GLuint t2 = CompileVs(*s_vs, true)) glDeleteShader(t2);
    else { printf("[GPUXF] off: vertex shader rejected\n"); return; }
  } else {
    printf("[GPUXF] off: vertex shader rejected\n");
    return;
  }
  s_on = true;
  s_on_w.store(true, std::memory_order_relaxed);
  printf("[GPUXF] on: vertex transform and texgen on the GPU (variant %d, %zu MB buffer)%s\n",
         variant, bytes >> 20, CheckEvery() ? " + transform-feedback check" : "");
  std::fflush(stdout);
}

// ---- matrix palette ----------------------------------------------------------
// The rows a draw's vertices need, copied into the ring once per frame for as
// long as they do not change (compared with the copy last written, so it also
// holds after NWII_R9_FRAMECHECK restores g_state).
struct PalPN { uint64_t epoch = 0; bool p = false, n = false; float m[12], nm[9]; uint32_t unit = 0; };
struct Pal12 { uint64_t epoch = 0; float m[12]; uint32_t unit = 0; };
static PalPN s_pal_pn[64];
static Pal12 s_pal_tex[64], s_pal_post[64];
// Position rows of matrix mi (3 units) and, with nrm, the normal rows of
// (mi & 31) (3 more units, one row of three per unit).
// R14 R5: 48-byte equality without a memcmp call (bitwise, as memcmp).
static inline bool Same48(const float *a, const float *b) {
  // r36r (r13): three 16-byte vector loads per side (was two 48-byte arrays the
  // compiler spilled to the stack); the same bitwise comparison.
  typedef uint64_t v2u __attribute__((vector_size(16)));
  v2u a0, a1, a2, b0, b1, b2;
  std::memcpy(&a0, a, 16);
  std::memcpy(&a1, a + 4, 16);
  std::memcpy(&a2, a + 8, 16);
  std::memcpy(&b0, b, 16);
  std::memcpy(&b1, b + 4, 16);
  std::memcpy(&b2, b + 8, 16);
  const v2u x = (a0 ^ b0) | (a1 ^ b1) | (a2 ^ b2);
  return !(x[0] | x[1]);
}
static uint32_t PalPosNrm(int mi, bool pxf, bool nxf) {
  const float *m = &g_state.posMatrices[mi * 4];
  const float *nm = &g_state.normalMatrices[3 * (mi & 31)];
  PalPN &e = s_pal_pn[mi];
  if (e.epoch == s_epoch && e.p == pxf && e.n == nxf && (!pxf || Same48(e.m, m)) &&
      (!nxf || !std::memcmp(e.nm, nm, 36)))
    return e.unit;
  float buf[24] = {};
  if (pxf) std::memcpy(buf, m, 48);
  if (nxf)
    for (int r = 0; r < 3; ++r) std::memcpy(&buf[12 + 4 * r], &nm[3 * r], 12);
  const uint32_t n = nxf ? 6 : 3;
  const uint32_t u = (uint32_t)Alloc(n);
  std::memcpy(s_map + (size_t)u * 16, buf, n * 16);
  e.epoch = s_epoch;
  e.p = pxf;
  e.n = nxf;
  if (pxf) std::memcpy(e.m, m, 48);
  if (nxf) std::memcpy(e.nm, nm, 36);
  e.unit = u;
  ++st_pal;
  return u;
}
// 12 floats from `m` (a texture matrix in the position bank, or a post
// matrix: read exactly where TexGen4 reads them).
static inline uint32_t Pal3Slow(Pal12 &e, const float *m);
static inline uint32_t Pal3(Pal12 &e, const float *m) {
  if (e.epoch == s_epoch && Same48(e.m, m)) return e.unit;
  return Pal3Slow(e, m);
}
static NWII_NOINLINE_R5 uint32_t Pal3Slow(Pal12 &e, const float *m) {
  const uint32_t u = (uint32_t)Alloc(3);
  std::memcpy(s_map + (size_t)u * 16, m, 48);
  e.epoch = s_epoch;
  std::memcpy(e.m, m, 48);
  e.unit = u;
  ++st_pal;
  return u;
}
static uint32_t s_tg_last[16];
static uint64_t s_tg_epoch = 0;
static uint32_t s_tg_unit = 0;
// r36r NWII_R_TGMEMO=1: the texgen record of the last draw that built one, kept
// with what it was built from (texgen config, texture matrix indices) and the
// matrix rows it read. RenderOnce marks every position/post matrix row an XF
// write touches (s_rows_*_w, a bit per 4-float row); while none of the rows the
// record read was written, the same inputs give the same record (and the same
// palette units, valid for the frame: s_epoch), so it is reused as is.
static uint64_t s_rows_pos_w = ~0ull, s_rows_post_w = ~0ull;
struct TgMemo {
  bool valid = false;
  uint64_t epoch = 0, dep_pos = 0, dep_post = 0;
  uint32_t key[9] = {};
  uint32_t rec[16] = {};
};
static TgMemo s_tgm;
static uint64_t st_tgm_hit = 0, st_tgm_miss = 0;
// [r35m4 m5] interruptores del frente R ENCENDIDOS por defecto: NWII_R_<X>=1/0 explicito (PC) manda; si no esta,
// encendido salvo NWII_R_OFF (que si llega desde debug.toml: apply_env borra los "0"/"off").
static inline bool m5_rflag(const char *e) { return e ? e[0] == '1' : (NWII_ENV("NWII_R_OFF") == nullptr); }
static bool TgMemoOn() {
  static const bool on = [] {
    const char *e = NWII_ENV("NWII_R_TGMEMO");
    return m5_rflag(e);
  }();
  return on;
}

// ---- vertices ----------------------------------------------------------------
static inline uint8_t TexMask(const VertexData &x) {
  uint8_t m = 0;
  for (int k = 0; k < 8; ++k)
    if (x.has_tex[k]) m |= (uint8_t)(1u << k);
  return m;
}
// Rank of index i in set[0, n), or adds it (n grows); -1 when full (16).
static inline int Rank(uint8_t *set, uint8_t &n, uint8_t i) {
  for (int r = 0; r < n; ++r)
    if (set[r] == i) return r;
  if (n == 16) return -1;
  set[n] = i;
  return n++;
}
// Can these vertices be transformed on the GPU (one layout, at most 16
// position and 16 texture matrix indices per texgen, no parse-time
// transform), and how are they laid out.
static Info Analyze(const std::vector<VertexData> &v) {
  Info in;
  if (v.empty()) return in;
  const VertexData &a = v[0];
  in.hp = a.has_pos;
  in.hn = a.has_norm;
  in.hc = a.has_color;
  in.tmask = TexMask(a);
  for (const VertexData &x : v) {
    if (x.pre_xf || x.has_pos != in.hp || x.has_norm != in.hn || x.has_color != in.hc ||
        TexMask(x) != in.tmask || Rank(in.pos, in.npos, x.posMtxIdx) < 0)
      return in;
    for (int t = 0; t < 4; ++t)
      if (Rank(in.tex[t], in.ntex[t], x.texMtxIdx[t]) < 0) return in;
  }
  in.ranks = in.npos > 1 || in.ntex[0] > 1 || in.ntex[1] > 1 || in.ntex[2] > 1 || in.ntex[3] > 1;
  in.stride = (uint8_t)(3 + (in.hn ? 3 : 0) + (in.hc ? 1 : 0) + 2 * __builtin_popcount(in.tmask) +
                        (in.ranks ? 1 : 0));
  in.ok = true;
  return in;
}
// The colour as the classic vertex shader quantises it (round(c * 255.0):
// the product is the same IEEE single multiply; no colour of GX's formats
// lands on a .5, so the rounding direction of a tie never matters).
static inline uint32_t Q8(float c) {
  const float p = c * 255.0f;
  const float r = std::nearbyint(p);
  return r <= 0.f ? 0u : r >= 255.f ? 255u : (uint32_t)r;
}
static void WriteVerts(const std::vector<VertexData> &v, uint32_t n, const Info &in, uint32_t *d) {
  Info sets = in;  // (Rank only finds here: every index is in the sets)
  for (uint32_t i = 0; i < n; ++i) {
    const VertexData &x = v[i];
    std::memcpy(d, x.pos, 12);
    d += 3;
    if (in.hn) { std::memcpy(d, x.norm, 12); d += 3; }
    if (in.hc)
      *d++ = Q8(x.color[0]) | (Q8(x.color[1]) << 8) | (Q8(x.color[2]) << 16) |
             (Q8(x.color[3]) << 24);
    for (int k = 0; k < 8; ++k)
      if ((in.tmask >> k) & 1) { std::memcpy(d, x.tex[k], 8); d += 2; }
    if (in.ranks) {
      uint32_t rk = (uint32_t)Rank(sets.pos, sets.npos, x.posMtxIdx);
      for (int t = 0; t < 4; ++t)
        rk |= (uint32_t)Rank(sets.tex[t], sets.ntex[t], x.texMtxIdx[t]) << (4 + 4 * t);
      *d++ = rk;
    }
  }
}

// ---- transform-feedback check (NWII_GPU_XF_CHECK) ------------------------------
static GLuint s_chk_prog = 0, s_chk_tf = 0;
static GLint s_chk_loc = -1;
static bool s_chk_frame = false;
static uint64_t chk_draws = 0, chk_verts = 0, chk_bad_verts = 0, chk_bad[5] = {};
static void CheckDraw(const std::vector<VertexData> &v, uint32_t n, const uint32_t de[4],
                      const TexGenCfg &cfg) {
  if (!s_chk_prog && s_chk_loc != -2) {
    static const char *const kVars[5] = {"xP", "xN", "xT01", "xT23", "xC"};
    s_chk_prog = LinkTf(GenerateVertexShaderXF(s_variant, true), kVars, 5);
    if (!s_chk_prog) { s_chk_loc = -2; return; }
    glGenBuffers(1, &s_chk_tf);
    glUseProgram(s_chk_prog);
    glUniform1i(glGetUniformLocation(s_chk_prog, "uXW"), 12);
    glUniform1i(glGetUniformLocation(s_chk_prog, "uXQ"), 13);
    s_chk_loc = glGetUniformLocation(s_chk_prog, "uXDraw");
  }
  if (!s_chk_prog) return;
  static std::vector<BatchVertex> ref;
  if (ref.size() < n) ref.resize(n);
  EmitRange(v, n, ref.data(), cfg);
  const uint64_t du = Alloc(1);
  std::memcpy(s_map + du * 16, de, 16);
  glUseProgram(s_chk_prog);
  glUniform1i(s_chk_loc, (GLint)du);
  glBindVertexArray(s_vao);
  static std::vector<uint32_t> out;
  RunTf(s_chk_prog, s_chk_tf, n, 18, out);
  glUseProgram(g_gl.program == ~0u ? 0 : g_gl.program);
  glBindVertexArray(g_gl.vao == ~0u ? 0 : g_gl.vao);
  ++chk_draws;
  static int shown = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t *o = &out[(size_t)i * 18];
    const BatchVertex &b = ref[i];
    const float want[14] = {b.x, b.y, b.z, b.nx, b.ny, b.nz, b.u, b.v, b.u1, b.v1, b.u2, b.v2, b.u3, b.v3};
    bool vbad = false;
    for (int k = 0; k < 14; ++k) {
      if (o[k] == FBits(want[k])) continue;
      const int f = k < 3 ? 0 : k < 6 ? 1 : 2;
      ++chk_bad[f];
      vbad = true;
      if (shown++ < 12)
        printf("[GPUXF-CHECK] draw %llu vertex %u %s[%d] gpu %08X (%g) cpu %08X (%g) | flags %08X "
               "ntg %d cf %04X %04X\n",
               (unsigned long long)chk_draws, i, f == 0 ? "pos" : f == 1 ? "nrm" : "tex",
               f == 2 ? k - 6 : k % 3, o[k], BitsF(o[k]), FBits(want[k]), want[k], de[1], cfg.n,
               s_tg_last[1] & 0xFFFF, s_tg_last[1] >> 16);
    }
    const float cw[4] = {b.r, b.g, b.b, b.a};
    for (int k = 0; k < 4; ++k) {
      const int g = (int)std::lround(BitsF(o[14 + k]) * 255.0f);
      if (g == (int)Q8(cw[k])) continue;
      ++chk_bad[3];
      vbad = true;
    }
    if (vbad) ++chk_bad_verts;
  }
  chk_verts += n;
}

// R14 R5: a non-display-list draw packed on the render worker
// (GX_PrepDrawForRender): its Analyze() and the words WriteVerts() makes of its
// vertices (n * xi.stride words follow the header in the packet's buffer).
struct PackHdr {
  Info xi;
  uint32_t n, words;
  bool bb_ok;   // r36r NWII_R_CULL: raw position bounding box (see SetBoundingBox)
  float bb[6];
};
static constexpr uint32_t kPackHdrWords = (sizeof(PackHdr) + 3) / 4;
static const PackHdr *s_pack_hint = nullptr;  // the draw being rendered (null: none)
static const uint32_t *s_pack_words = nullptr;

// ---- r36r NWII_R_CULL: draws entirely outside the clip volume ----------------
// A cached (display-list) draw whose vertices all use one position matrix is
// skipped when its raw-position bounding box, transformed by that matrix and
// the projection the vertex shader uses (ApplyProjection), lies entirely on
// the outer side of one of the clip planes x = +-w, y = +-w. Each plane test
// d(P) = +-x_clip - w_clip is an affine function of the object-space position,
// so its minimum over the box is at a corner and every vertex (all inside the
// box) is outside too: GL clips such triangles away completely (depth clamp
// only turns off the near/far planes), so they never produce a fragment and
// the image is the same. Double precision and a relative margin of 1e-3 keep
// the test conservative against the GPU's float rounding. Nothing else of the
// draw is skipped (its state, textures and batch were set up before Emit).
// NWII_R_CULL=1 on (default off).
static uint64_t st_culled = 0, st_culled_packed = 0;
static uint64_t st_p_slots_freed = 0;  // r45p: arena slots of fused parts given back
// r12: RenderOnce already ran the test on this draw (and kept it): Emit skips it.
static bool s_cull_done = false;
static bool CullOn() {
  static const bool on = [] {
    const char *e = NWII_ENV("NWII_R_CULL");
    return e ? (e[0] == '1' || e[0] == '2') : (NWII_ENV("NWII_R_OFF") == nullptr);
  }();
  return on;
}
// r45p NWII_P_CULLSKIP=1: the cull test only saves work, never changes the image
// (a rejected draw makes no fragment either way). When a frame tests >= 1000
// draws and rejects none (the world selector: 10 000 draws, all on screen, the
// test ~0,8 ms a frame on the Switch), the next 15 frames skip the test; then
// it is tried again. Frames that reject draws keep testing every frame.
static bool s_p_cull_off = false;  // this frame: no cull test
static uint32_t s_p_cull_tested = 0, s_p_cull_hits = 0, s_p_cull_skip = 0;
static uint64_t st_p_cull_framesoff = 0, st_p_cull_frames = 0;
static bool PCullSkipOn() {
  static const bool on = [] {
    const char *e = NWII_ENV("NWII_P_CULLSKIP");
    const bool v = e && e[0] == '1';
    if (v) printf("[P45] NWII_P_CULLSKIP=1: sin prueba de recorte 15 fotogramas tras uno sin recortes\n");
    return v;
  }();
  return on;
}
static void PCullFrame() {  // start of each RenderOnce
  if (!PCullSkipOn()) return;
  if (!s_p_cull_off && s_p_cull_tested >= 1000 && s_p_cull_hits == 0) s_p_cull_skip = 15;
  s_p_cull_tested = s_p_cull_hits = 0;
  s_p_cull_off = s_p_cull_skip > 0;
  if (s_p_cull_skip) --s_p_cull_skip;
  ++st_p_cull_frames;
  if (s_p_cull_off) ++st_p_cull_framesoff;
  if (st_p_cull_frames == 600) {
    printf("[P45C] fotogramas sin prueba de recorte: %llu de %llu\n", (unsigned long long)st_p_cull_framesoff,
           (unsigned long long)st_p_cull_frames);
    st_p_cull_frames = st_p_cull_framesoff = 0;
  }
}
static inline bool PCullTest(const float bb[6], int mi);
static bool Culled(const float bb[6], int mi) {
  // Each clip plane d = +-x_clip - w_clip (or y) is an affine function of the
  // object-space position: d(P) = c0*x + c1*y + c2*z + c3, built from the
  // matrix rows and the projection exactly as ApplyProjection lays it out. Its
  // minimum over the box is c3 + sum_k min(c_k*lo_k, c_k*hi_k).
  // r8: single precision (the margin, 1e-3 of the terms' magnitude, is ~10^4
  // times the rounding error of these few operations) and the box minimum per
  // plane: the 8-corner double version of r4-r6 cost ~0.5 us a draw on the
  // Switch, as much as the emit it saved.
  const float *m = &g_state.posMatrices[mi * 4];
  const float *sp = g_state.projection;
  const float lo0 = bb[0], lo1 = bb[1], lo2 = bb[2], hi0 = bb[3], hi1 = bb[4], hi2 = bb[5];
  auto test = [&](float c0, float c1, float c2, float c3) {
    const float a0 = c0 * lo0, b0 = c0 * hi0, a1 = c1 * lo1, b1 = c1 * hi1, a2 = c2 * lo2, b2 = c2 * hi2;
    const float dmin = std::min(a0, b0) + std::min(a1, b1) + std::min(a2, b2) + c3;
    const float scale = std::max(std::fabs(a0), std::fabs(b0)) + std::max(std::fabs(a1), std::fabs(b1)) +
                        std::max(std::fabs(a2), std::fabs(b2)) + std::fabs(c3) + 1.0f;
    return dmin > 1e-3f * scale;  // false on NaN
  };
  if (!g_state.projSet || g_state.projType != 0) {
    // orthographic (or no projection: identity): w = 1. r14: the box as centre
    // c and half size h: over the box, x_clip ranges over X +- R with
    // X = sx*(m0.c + m3) + tx and R = |sx| * (|m0|.h) (the same minimum as the
    // per-plane form, both x planes from one centre/radius; same for y).
    const float sx = g_state.projSet ? sp[0] : 1.0f, tx = g_state.projSet ? sp[1] : 0.0f;
    const float sy = g_state.projSet ? sp[2] : 1.0f, ty = g_state.projSet ? sp[3] : 0.0f;
    const float c0 = 0.5f * (lo0 + hi0), c1 = 0.5f * (lo1 + hi1), c2 = 0.5f * (lo2 + hi2);
    const float h0 = 0.5f * (hi0 - lo0), h1 = 0.5f * (hi1 - lo1), h2 = 0.5f * (hi2 - lo2);
    const float vx = m[0] * c0 + m[1] * c1 + m[2] * c2 + m[3];
    const float rx = std::fabs(m[0]) * h0 + std::fabs(m[1]) * h1 + std::fabs(m[2]) * h2;
    const float X = sx * vx + tx, R = std::fabs(sx) * rx;
    const float sclx = std::fabs(sx) * (std::fabs(m[0] * c0) + std::fabs(m[1] * c1) + std::fabs(m[2] * c2) +
                                        std::fabs(m[3]) + rx) + std::fabs(tx) + 1.0f;
    if (X - R - 1.0f > 1e-3f * sclx || -X - R - 1.0f > 1e-3f * sclx) return true;  // x - w, -x - w
    const float vy = m[4] * c0 + m[5] * c1 + m[6] * c2 + m[7];
    const float ry = std::fabs(m[4]) * h0 + std::fabs(m[5]) * h1 + std::fabs(m[6]) * h2;
    const float Y = sy * vy + ty, Ry = std::fabs(sy) * ry;
    const float scly = std::fabs(sy) * (std::fabs(m[4] * c0) + std::fabs(m[5] * c1) + std::fabs(m[6] * c2) +
                                        std::fabs(m[7]) + ry) + std::fabs(ty) + 1.0f;
    if (Y - Ry - 1.0f > 1e-3f * scly || -Y - Ry - 1.0f > 1e-3f * scly) return true;  // y - w, -y - w
    return false;  // (false on NaN)
  }
  // perspective: x_clip = p0*vx + p1*vz, y_clip = p2*vy + p3*vz, w = -vz
  const float p0 = sp[0], p1 = sp[1], p2 = sp[2], p3 = sp[3];
  const float *r0 = &m[0], *r1 = &m[4], *r2 = &m[8];
  for (int k = 0; k < 4; ++k) {
    const float *r = k < 2 ? r0 : r1;
    const float al = k == 0 ? p0 : k == 1 ? -p0 : k == 2 ? p2 : -p2;
    const float be = k == 0 ? p1 + 1.0f : k == 1 ? 1.0f - p1 : k == 2 ? p3 + 1.0f : 1.0f - p3;
    if (test(al * r[0] + be * r2[0], al * r[1] + be * r2[1], al * r[2] + be * r2[2], al * r[3] + be * r2[3]))
      return true;
  }
  return false;
}

static inline bool PCullTest(const float bb[6], int mi) {  // r45p: Culled + counters
  const bool c = Culled(bb, mi);
  ++s_p_cull_tested;
  s_p_cull_hits += c;
  return c;
}

// ---- per draw ----------------------------------------------------------------
static void Emit(const GXCommand &cmd, uint8_t prim) {
  const auto &v = cmd.verts();  // (a reference: read only when needed)
  if (s_draws.size() >= 65535u * 4 || s_rw - s_batch_r0 > kBatchCapUnits) ::nwii::runtime::gx::FlushBatch();
  ecache::Entry *ce = ecache::Get(cmd);
  const PackHdr *ph = ce ? nullptr : s_pack_hint;
  const uint32_t n = ce ? ce->x_n : ph ? ph->n : (uint32_t)v.size();
  Info local;
  if (ce && !ce->x_init) {
    ce->xi = Analyze(v);
    ::nwii::runtime::gx::SetBoundingBox(*ce, v);  // r36r
    ce->x_init = true;
  }
  // r36r NWII_R_CULL (see Culled above): before anything else of the draw.
  // One position matrix, in range: the GPU path transforms by it, and so does
  // the CPU path (EmitRange) of a draw with colour-sourced texgen.
  if (ce && !s_cull_done && ce->x_bb_ok && ce->xi.ok && ce->xi.hp && ce->xi.npos == 1 && n &&
      (ce->xi.pos[0] & 0x3F) * 4 + 12 <= 256 && CullOn() && !s_p_cull_off &&
      PCullTest(ce->x_bb, ce->xi.pos[0] & 0x3F)) {
    ++st_culled;
    return;
  }
  // r36r NWII_R_CULL: the same for a draw packed on the worker (not a display list).
  if (ph && ph->bb_ok && ph->xi.ok && ph->xi.hp && ph->xi.npos == 1 && n &&
      (ph->xi.pos[0] & 0x3F) * 4 + 12 <= 256 && CullOn() && !s_p_cull_off &&
      PCullTest(ph->bb, ph->xi.pos[0] & 0x3F)) {
    ++st_culled;
    ++st_culled_packed;
    return;
  }
  // r45p NWII_P_DLMERGE: the parts' arena slots (unused while the fused draw replaces them; a part
  // drawn alone again gets a new one) go back to the arena, a frame later as any freed slot.
  if (ce && s_emit_parts && !ce->p_freed) {
    ce->p_freed = true;
    if (const DecodedVerts *fd = cmd.shared_vertices.get())
      for (const auto &pd : fd->part_dv)
        if (pd && pd->emit_cache) {
          ecache::Entry *pe = static_cast<ecache::Entry *>(pd->emit_cache.get());
          if (pe->x_slot >= 0) {
            ArenaFree((uint32_t)pe->x_slot, pe->x_cls);
            pe->x_slot = -1;
            ++st_p_slots_freed;
          }
        }
  }
  // r45p NWII_P_DLMERGE: a fused draw kept whole: its parts one by one (same test, same matrix)
  if (ce && s_emit_parts && !s_mdi && ce->x_bb_ok && ce->xi.ok && ce->xi.hp && ce->xi.npos == 1 &&
      (ce->xi.pos[0] & 0x3F) * 4 + 12 <= 256 && CullOn() && !s_p_cull_off) {
    const std::vector<uint16_t> &parts = *s_emit_parts;
    if (!ce->p_init) {
      ce->p_init = true;
      ce->p_bb.assign(parts.size() * 6, 0.0f);
      ce->p_ok.assign(parts.size(), 0);
      size_t at = 0;
      for (size_t j = 0; j < parts.size() && at + parts[j] <= v.size(); at += parts[j], ++j) {
        float *b = &ce->p_bb[j * 6];
        bool ok = parts[j] > 0;
        for (size_t i = at; ok && i < at + parts[j]; ++i)
          for (int q = 0; q < 3; ++q) {
            const float x = v[i].pos[q];
            if (!(x == x) || std::isinf(x)) { ok = false; break; }
            if (i == at) { b[q] = b[3 + q] = x; }
            else { b[q] = std::min(b[q], x); b[3 + q] = std::max(b[3 + q], x); }
          }
        ce->p_ok[j] = ok;
      }
    }
    static std::vector<uint8_t> s_skip;
    s_skip.assign(parts.size(), 0);
    bool any = false;
    for (size_t j = 0; j < parts.size(); ++j)
      if (ce->p_ok[j] && PCullTest(&ce->p_bb[j * 6], ce->xi.pos[0] & 0x3F)) { s_skip[j] = 1; any = true; }
    if (any) s_emit_part_skip = &s_skip;
  }
  TexGenCfg cfg;
  PrepTexGen(cfg);
  if (!ce && !ph) local = Analyze(v);
  const Info *in = ce ? &ce->xi : ph ? &ph->xi : &local;
  bool gpu = in->ok;
  // Position/normal transform: all of the draw's matrix indices in range, or
  // none (a vertex out of range is not transformed on the CPU either).
  int pin = 0, nin = 0;
  for (int r = 0; gpu && r < in->npos; ++r) {
    pin += (in->pos[r] & 0x3F) * 4 + 12 <= 256;
    nin += 3 * (in->pos[r] & 31) + 9 <= 96;
  }
  const bool pxf = in->hp && pin > 0, nxf = in->hn && nin > 0;
  if (gpu && ((pxf && pin != in->npos) || (nxf && nin != in->npos))) gpu = false;
  if (!gpu) ++st_cpu_static;
  for (int t = 0; gpu && t < cfg.n; ++t)
    if (cfg.c[t].row == 2) { gpu = false; ++st_cpu_row2; }  // colour-sourced texgen
  uint32_t de[4];
  if (gpu) {
    ++st_gpu;
    const uint32_t units = (n * in->stride + 3) / 4;
    int64_t vb = -1;
    if (ce) {
      ++st_cached;
      if (ce->x_slot < 0) {
        uint8_t cls = 0;
        const int64_t s = ArenaAlloc(units, cls);
        if (s >= 0) {
          WriteVerts(v, n, *in, (uint32_t *)(s_map + (size_t)s * 16));
          ce->x_slot = s;
          ce->x_cls = cls;
          ++st_cached_new;
          if (rph::On()) rph::s_bytes += (uint64_t)n * in->stride * 4;
        }
      }
      vb = ce->x_slot;
    }
    if (vb < 0 && ph) {
      // R14 R5: packed on the worker: one copy into the ring.
      vb = (int64_t)Alloc(units);
      std::memcpy(s_map + (size_t)vb * 16, s_pack_words, (size_t)n * in->stride * 4);
      st_sverts += n;
      ++st_packed;
      if (rph::On()) rph::s_bytes += (uint64_t)n * in->stride * 4;
    }
    if (vb < 0) {
      static std::vector<uint32_t> tmp;
      if (tmp.size() < (size_t)units * 4) tmp.resize((size_t)units * 4);
      WriteVerts(v, n, *in, tmp.data());
      vb = (int64_t)Alloc(units);
      std::memcpy(s_map + (size_t)vb * 16, tmp.data(), (size_t)n * in->stride * 4);
      st_sverts += n;
      if (rph::On()) rph::s_bytes += (uint64_t)n * in->stride * 4;
    }
    // Position (+ normal) rows: one matrix (kept while unchanged) or a block
    // of one entry per index, in rank order.
    uint32_t pal = 0;
    if (pxf || nxf) {
      if (in->npos == 1) {
        pal = PalPosNrm(in->pos[0] & 0x3F, pxf, nxf);
      } else {
        const uint32_t es = nxf ? 6 : 3;
        pal = (uint32_t)Alloc(es * in->npos);
        float *dst = (float *)(s_map + (size_t)pal * 16);
        for (int r = 0; r < in->npos; ++r) {
          float buf[24] = {};
          const int mi = in->pos[r];
          if (pxf) std::memcpy(buf, &g_state.posMatrices[(mi & 0x3F) * 4], 48);
          if (nxf)
            for (int k = 0; k < 3; ++k)
              std::memcpy(&buf[12 + 4 * k], &g_state.normalMatrices[3 * (mi & 31) + 3 * k], 12);
          std::memcpy(dst + (size_t)r * es * 4, buf, es * 16);
        }
        ++st_pal;
      }
    }
    // Texgen record (4 units): count, coord flags; per texgen its texture
    // matrix rows (one entry per index, in rank order) and post matrix rows;
    // per texgen a mask of the ranks whose texture matrix exists.
    uint32_t rec[16] = {};
    // r36r NWII_R_TGMEMO: key = texgen config + texture matrix index of each
    // texgen (only draws with at most one index per texgen are memoised).
    bool tgm_ok = TgMemoOn();
    uint32_t tgk[9] = {};
    uint64_t dep_pos = 0, dep_post = 0;
    if (tgm_ok) {
      tgk[0] = (uint32_t)cfg.n;
      for (int t = 0; t < cfg.n && tgm_ok; ++t) {
        const auto &c = cfg.c[t];
        const int pi = c.pm ? (int)((c.pm - g_state.postMatrices) / 4) : 255;
        tgk[1 + 2 * t] = (uint32_t)c.row | (c.stq ? 32u : 0u) | (c.abc1 ? 64u : 0u) | (c.mtx ? 128u : 0u) |
                         (c.post ? 256u : 0u) | (c.post_norm ? 512u : 0u) | ((uint32_t)pi << 16);
        if (c.mtx) {
          const int nt = in->ntex[t];
          if (nt > 1) { tgm_ok = false; break; }
          const int mi = nt ? in->tex[t][0] : 255;
          tgk[2 + 2 * t] = (uint32_t)nt | ((uint32_t)mi << 8);
          if (nt && mi != 60 && mi * 4 + 12 <= 256) dep_pos |= 7ull << mi;  // rows mi..mi+2 (mi <= 61)
          if (c.post && c.pm) {
            if (pi > 61) { tgm_ok = false; break; }
            dep_post |= 7ull << pi;
          }
        }
      }
    }
    const bool tgm_hit = tgm_ok && s_tgm.valid && s_tgm.epoch == s_epoch &&
                         !(s_rows_pos_w & s_tgm.dep_pos) && !(s_rows_post_w & s_tgm.dep_post) &&
                         !std::memcmp(tgk, s_tgm.key, sizeof tgk);
    if (tgm_hit) {
      std::memcpy(rec, s_tgm.rec, sizeof rec);
      ++st_tgm_hit;
    } else {
    if (tgm_ok) ++st_tgm_miss;
    rec[0] = (uint32_t)cfg.n;
    for (int t = 0; t < cfg.n; ++t) {
      const auto &c = cfg.c[t];
      uint32_t cf = (uint32_t)c.row | (c.stq ? 32u : 0u) | (c.abc1 ? 64u : 0u) | (c.mtx ? 128u : 0u);
      uint32_t tm = 0, pm = 0, hmask = 0;
      if (c.mtx) {
        const int nt = in->ntex[t];
        const float *mm[16];
        for (int r = 0; r < nt; ++r) {
          const int mi = in->tex[t][r];
          const float *m = (mi != 60 && mi * 4 + 12 <= 256) ? &g_state.posMatrices[mi * 4] : nullptr;
          if (m && !(m[0] || m[1] || m[2] || m[4] || m[5] || m[6])) m = nullptr;  // as TexGen4
          mm[r] = m;
          if (m) hmask |= 1u << r;
        }
        if (nt == 1) {
          if (mm[0]) tm = Pal3(s_pal_tex[in->tex[t][0]], mm[0]);
        } else if (hmask) {
          tm = (uint32_t)Alloc(3 * nt);
          float *dst = (float *)(s_map + (size_t)tm * 16);
          for (int r = 0; r < nt; ++r) {
            static const float kZero[12] = {};
            std::memcpy(dst + 12 * r, mm[r] ? mm[r] : kZero, 48);
          }
          ++st_pal;
        }
        if (c.post) {
          cf |= 256u | (c.post_norm ? 512u : 0u);
          if (c.pm) {
            cf |= 2048u;
            pm = Pal3(s_pal_post[(c.pm - g_state.postMatrices) / 4], c.pm);
          }
        }
      }
      rec[1 + t / 2] |= cf << (16 * (t & 1));
      rec[4 + 2 * t] = tm;
      rec[5 + 2 * t] = pm;
      rec[12 + t] = hmask;
    }
    if (tgm_ok) {
      s_tgm.valid = true;
      s_tgm.epoch = s_epoch;
      s_tgm.dep_pos = dep_pos;
      s_tgm.dep_post = dep_post;
      std::memcpy(s_tgm.key, tgk, sizeof tgk);
      std::memcpy(s_tgm.rec, rec, sizeof rec);
      s_rows_pos_w &= ~dep_pos;  // rows written from now on invalidate it
      s_rows_post_w &= ~dep_post;
    }
    }  // r36r: not a memo hit
    if (s_tg_epoch != s_epoch || std::memcmp(rec, s_tg_last, sizeof rec)) {
      s_tg_unit = (uint32_t)Alloc(4);
      std::memcpy(s_map + (size_t)s_tg_unit * 16, rec, sizeof rec);
      std::memcpy(s_tg_last, rec, sizeof rec);
      s_tg_epoch = s_epoch;
      ++st_tg;
    }
    de[0] = (uint32_t)vb * 4;
    de[1] = in->stride | (in->hn ? 32u : 0u) | (in->hc ? 64u : 0u) | (pxf ? 256u : 0u) |
            (nxf ? 512u : 0u) | (in->ranks ? 1024u : 0u) | ((uint32_t)in->tmask << 16);
    de[2] = pal;
    de[3] = s_tg_unit;
    if (s_chk_frame) CheckDraw(v, n, de, cfg);
  } else {
    // The CPU path's own vertices, passed through by the shader.
    ++st_cpu;
    st_cverts += n;
    static std::vector<BatchVertex> tmp;
    if (tmp.size() < n) tmp.resize(std::max<size_t>(n, 2 * tmp.size()));
    EmitRange(v, n, tmp.data(), cfg);
    static_assert(sizeof(BatchVertex) == 72, "BatchVertex is 18 words");
    const uint64_t u = Alloc((n * 18 + 3) / 4);
    std::memcpy(s_map + u * 16, tmp.data(), (size_t)n * sizeof(BatchVertex));
    if (rph::On()) rph::s_bytes += (uint64_t)n * sizeof(BatchVertex);
    de[0] = (uint32_t)u * 4;
    de[1] = 18u | 128u;
    de[2] = de[3] = 0;
  }
  const uint32_t slot = (uint32_t)(s_draws.size() / 4);
  s_draws.insert(s_draws.end(), de, de + 4);
  if (s_mdi) {
    // R14 R5: one indirect sub-draw; base instance = slot (+ the batch's
    // first entry at FlushBatch).
    if (ce && (ce->x_islot < 0 || ce->x_iprim != prim)) {
      static std::vector<uint32_t> tmp;
      tmp.clear();
      s_batch_idx.swap(tmp);
      EmitIndices(prim, 0, n);
      s_batch_idx.swap(tmp);
      if (ce->x_islot >= 0) {
        ArenaFree((uint32_t)ce->x_islot, ce->x_icls);
        ce->x_islot = -1;
      }
      ce->x_icount = (uint32_t)tmp.size();
      ce->x_iprim = prim;
      if (!tmp.empty()) {
        uint8_t cls = 0;
        const int64_t s2 = ArenaAlloc((uint32_t)((tmp.size() + 3) / 4), cls);
        if (s2 >= 0) {
          std::memcpy(s_map + (size_t)s2 * 16, tmp.data(), tmp.size() * 4);
          ce->x_islot = s2;
          ce->x_icls = cls;
        }
      }
    }
    uint32_t c[5];
    if (ce && ce->x_islot >= 0) {
      if (!ce->x_icount) return;
      c[0] = ce->x_icount;
      c[2] = (uint32_t)ce->x_islot * 4;  // in indices (4 bytes) from the buffer start
      ++st_mdi_static;
    } else {
      const size_t at = s_batch_idx.size();
      EmitIndices(prim, 0, n);
      if (s_batch_idx.size() == at) return;
      c[0] = (uint32_t)(s_batch_idx.size() - at);
      c[2] = kRingIdx | (uint32_t)at;
      ++st_mdi_ring;
    }
    c[1] = 1;
    c[3] = 0;
    c[4] = slot;
    s_icmds.insert(s_icmds.end(), c, c + 5);
    return;
  }
  EmitIndices(prim, slot << 16, n);
}

static void FlushBatchMdi() {
  if (s_icmds.empty()) {
    s_draws.clear();
    s_batch_idx.clear();
    s_batch_r0 = s_rw;
    return;
  }
  const int ph0 = rph::Set(rph::kUpload);
  const uint32_t nd = (uint32_t)(s_draws.size() / 4);
  const uint64_t du = Alloc(nd);
  std::memcpy(s_map + du * 16, s_draws.data(), (size_t)nd * 16);
  uint64_t iu = 0;
  if (!s_batch_idx.empty()) {
    const uint64_t ib = (uint64_t)s_batch_idx.size() * 4;
    iu = Alloc((uint32_t)((ib + 15) / 16));
    std::memcpy(s_map + iu * 16, s_batch_idx.data(), ib);
  }
  const uint32_t ncmd = (uint32_t)(s_icmds.size() / 5);
  for (uint32_t k = 0; k < ncmd; ++k) {
    uint32_t *c = &s_icmds[(size_t)k * 5];
    if (c[2] & kRingIdx) c[2] = (uint32_t)(iu * 4) + (c[2] & ~kRingIdx);
    c[4] += BaseOf(du);
  }
  const uint64_t cu = Alloc((uint32_t)((s_icmds.size() * 4 + 15) / 16));
  std::memcpy(s_map + cu * 16, s_icmds.data(), s_icmds.size() * 4);
  if (rph::On()) {
    ++rph::s_flushes;
    rph::s_idx += s_batch_idx.size();
    rph::s_bytes += (uint64_t)nd * 16 + s_batch_idx.size() * 4 + s_icmds.size() * 4;
  }
  CBindVertexArray(s_vao);
  static GLuint s_bound_ind = 0;
  if (s_bound_ind != s_buf) {
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_buf);
    s_bound_ind = s_buf;
  }
  rph::Set(rph::kGlDraw);
  s_mdi_draw(GL_TRIANGLES, GL_UNSIGNED_INT, (const void *)(uintptr_t)(cu * 16), (GLsizei)ncmd, 0);
  ++g_prof_gl_draws;
  ++s_cap_cur.gl_draws;
  ++st_flushes;
  s_draws.clear();
  s_batch_idx.clear();
  s_icmds.clear();
  s_batch.clear();
  s_batch_r0 = s_rw;
  rph::Set(ph0);
}

static void FlushBatch() {
  if (s_mdi) {
    FlushBatchMdi();
    return;
  }
  if (s_batch_idx.empty()) {
    s_draws.clear();
    s_batch_r0 = s_rw;
    return;
  }
  const int ph0 = rph::Set(rph::kUpload);
  const uint32_t nd = (uint32_t)(s_draws.size() / 4);
  const uint64_t du = Alloc(nd);
  std::memcpy(s_map + du * 16, s_draws.data(), (size_t)nd * 16);
  const uint64_t ib = (uint64_t)s_batch_idx.size() * 4;
  const uint64_t iu = Alloc((uint32_t)((ib + 15) / 16));
  std::memcpy(s_map + iu * 16, s_batch_idx.data(), ib);
  if (rph::On()) {
    ++rph::s_flushes;
    rph::s_idx += s_batch_idx.size();
    rph::s_bytes += (uint64_t)nd * 16 + ib;
  }
  if (s_r2_xdubo) {
    // r39r2 NWII_R2_XDUBO: 16 bytes in the ring, 256-aligned, bound as block 1
    const uint64_t ub = Alloc(32);
    const size_t off = ((size_t)ub * 16 + 255) & ~(size_t)255;
    const int32_t v[4] = {(int32_t)du, 0, 0, 0};
    std::memcpy(s_map + off, v, sizeof v);
    glBindBufferRange(GL_UNIFORM_BUFFER, 1, s_buf, (GLintptr)off, 16);
  } else if (!s_baseinst) {
    GLShader &vsh = sso::s_on ? sso::Vsh(true) : *s_active;
    if (vsh.uXDraw >= 0) {
      if (sso::s_on) sso::pU1i(vsh.id, vsh.uXDraw, (GLint)du);
      else glUniform1i(vsh.uXDraw, (GLint)du);
    }
  }
  CBindVertexArray(s_vao);
  rph::Set(rph::kGlDraw);
  if (s_baseinst)
    s_draw_bi(GL_TRIANGLES, (GLsizei)s_batch_idx.size(), GL_UNSIGNED_INT,
              (const void *)(uintptr_t)(iu * 16), 1, BaseOf(du));
  else
    glDrawElements(GL_TRIANGLES, (GLsizei)s_batch_idx.size(), GL_UNSIGNED_INT,
                   (const void *)(uintptr_t)(iu * 16));
  ++g_prof_gl_draws;
  ++s_cap_cur.gl_draws;
  ++st_flushes;
  s_draws.clear();
  s_batch_idx.clear();
  s_batch.clear();
  s_batch_r0 = s_rw;
  rph::Set(ph0);
}

static void BeginFrame() {
  ++s_epoch;
  while (!s_inflight->empty() && Signaled(s_inflight->front().fence)) PopOldest(false);
  while (s_inflight->size() > 6) PopOldest(true);
  Recycle();
  s_draws.clear();
  s_batch_r0 = s_rw;
  CActiveUnit(12);
  glBindTexture(GL_TEXTURE_BUFFER, s_texw);
  CActiveUnit(13);
  glBindTexture(GL_TEXTURE_BUFFER, s_texq);
  const int ce = CheckEvery();
  static uint64_t nframes = 0;
  s_chk_frame = ce && (++nframes % (uint64_t)ce) == 0;
}

static void EndFrame() {
  s_inflight->push_back({s_frame, glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), s_frame_r0});
  ++s_frame;
  s_frame_r0 = s_rw;
  if (++st_frames < 600) return;
  const double f = (double)st_frames;
  printf("[GPUXF] per frame: draws gpu=%.0f (display-list %.0f, new slots %.1f) cpu=%.0f "
         "(not uniform %.0f, colour texgen %.0f) | verts ring=%.0f cpu=%.0f | palette=%.0f "
         "texgen=%.0f ringKB=%.0f gl=%.0f packed=%.0f mdi static/ring=%.0f/%.0f | arena %u/%u units, "
         "full %llu, waits %llu, finishes %llu, culled %.0f (packed %.0f) tgmemo %.0f/%.0f",
         st_gpu / f, st_cached / f, st_cached_new / f, st_cpu / f, st_cpu_static / f,
         st_cpu_row2 / f, st_sverts / f, st_cverts / f, st_pal / f, st_tg / f,
         st_units * 16.0 / 1024.0 / f, st_flushes / f, st_packed / f, st_mdi_static / f,
         st_mdi_ring / f, s_bump, kArenaUnits,
         (unsigned long long)st_arena_full, (unsigned long long)st_waits,
         (unsigned long long)st_finishes, st_culled / f, st_culled_packed / f, st_tgm_hit / f,
         (st_tgm_hit + st_tgm_miss) / f);
  if (CheckEvery())
    printf(" | check: draws=%llu verts=%llu bad_verts=%llu (pos %llu nrm %llu tex %llu col %llu)",
           (unsigned long long)chk_draws, (unsigned long long)chk_verts,
           (unsigned long long)chk_bad_verts, (unsigned long long)chk_bad[0],
           (unsigned long long)chk_bad[1], (unsigned long long)chk_bad[2],
           (unsigned long long)chk_bad[3]);
  printf("\n");
  std::fflush(stdout);
  st_frames = st_gpu = st_cached = st_cached_new = st_cpu = st_cpu_static = st_cpu_row2 = 0;
  st_sverts = st_cverts = st_pal = st_tg = st_units = st_flushes = st_packed = 0;
  st_mdi_static = st_mdi_ring = 0;
  st_culled = st_culled_packed = 0;
  st_tgm_hit = st_tgm_miss = 0;
}
}  // namespace xf

// NWII_GPUTIME=1: GPU time of each Render() batch (GL_TIME_ELAPSED queries,
// read back a few frames later), printed every 300 batches as
// "[GPUTIME] scale=Nx avg=.. max=.. ms". Measures what an internal
// resolution costs the GPU (also on the Switch: debug.toml NWII_GPUTIME = "1"
// with fps_log). The read-back is a glGet: only on when asked for.
namespace {
struct GpuTimer {
  static constexpr int kN = 8;
  GLuint q[kN] = {};
  bool busy[kN] = {};
  int next = 0, open = -1;
  double sum = 0, mx = 0;
  int n = 0;
  static bool On() {
    static const bool on = NWII_ENV("NWII_GPUTIME") != nullptr;
    return on;
  }
  void Begin() {
    if (!q[0]) glGenQueries(kN, q);
    Collect(false);
    if (busy[next]) Collect(true);  // ring full: wait for the oldest
    open = next;
    glBeginQuery(GL_TIME_ELAPSED, q[open]);
  }
  void End() {
    if (open < 0) return;
    glEndQuery(GL_TIME_ELAPSED);
    busy[open] = true;
    next = (open + 1) % kN;
    open = -1;
  }
  void Collect(bool wait) {
    for (int k = 0; k < kN; ++k) {
      const int i = (next + k) % kN;  // oldest first
      if (!busy[i]) continue;
      GLint avail = 0;
      if (!wait) {
        glGetQueryObjectiv(q[i], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) return;
      }
      GLuint64 ns = 0;
      glGetQueryObjectui64v(q[i], GL_QUERY_RESULT, &ns);
      busy[i] = false;
      const double ms = ns / 1e6;
      sum += ms;
      mx = std::max(mx, ms);
      if (++n == 300) {
        printf("[GPUTIME] scale=%gx avg=%.3f max=%.3f ms per render (%d renders)\n",
               (double)s_scale, sum / n, mx, n);
        std::fflush(stdout);
        sum = mx = 0;
        n = 0;
      }
      if (wait) return;
    }
  }
};
GpuTimer g_gpu_timer;

// r16res, NWII_RES_GPUPROF=1: where the GPU time of a frame goes, by segment.
// A frame is draw segment 0, copy 0, draw segment 1, copy 1, ... (EFB copies
// split it); GL_TIMESTAMP queries at every boundary, read back a few frames
// later (never waits). Every 300 frames, per-frame average of each segment
// (label: draw#k with its GX draw count, or the copy kind, size and scale).
struct ResGpuProf {
  static bool On() {
    static const bool on = NWII_ENV("NWII_RES_GPUPROF") != nullptr;
    return on;
  }
  struct Mark { GLuint q; std::string label; uint64_t draws = 0; };
  struct FrameRec { std::vector<Mark> marks; };
  std::deque<FrameRec> pending;
  std::vector<GLuint> pool;
  FrameRec cur;
  bool open = false;
  int seg = 0;
  uint64_t draws0 = 0;
  std::unordered_map<std::string, double> acc, acc_draws;
  std::vector<std::string> order;
  int frames = 0;
  GLuint Q() {
    if (pool.empty()) {
      GLuint q[64];
      glGenQueries(64, q);
      pool.assign(q, q + 64);
    }
    GLuint q = pool.back();
    pool.pop_back();
    return q;
  }
  void Add(const std::string &label) {
    if (!open) return;
    GLuint q = Q();
    glQueryCounter(q, GL_TIMESTAMP);
    cur.marks.push_back({q, label, 0});
  }
  void BeginFrame() {
    Collect();
    open = pending.size() < 8;
    cur.marks.clear();
    seg = 0;
    draws0 = g_prof_gx_draws;
    Add("draw#0");
  }
  // Before an EFB copy: closes the draw segment (its label gets the draw count).
  void Copy(const char *kind, int w, int h, int pw, int ph, bool half) {
    if (!open) return;
    char b[96];
    cur.marks.back().draws = g_prof_gx_draws - draws0;
    std::snprintf(b, sizeof b, "copy#%d %s %dx%d%s -> GL %dx%d", seg, kind, w, h,
                  half ? " half" : "", pw, ph);
    Add(b);
  }
  void AfterCopy() {
    if (!open) return;
    draws0 = g_prof_gx_draws;
    Add("draw#" + std::to_string(++seg));
  }
  void EndFrame() {
    if (!open) return;
    cur.marks.back().draws = g_prof_gx_draws - draws0;
    Add("end");
    pending.push_back(std::move(cur));
    cur = FrameRec{};
    open = false;
  }
  void Collect() {
    while (!pending.empty()) {
      FrameRec &f = pending.front();
      GLint avail = 0;
      glGetQueryObjectiv(f.marks.back().q, GL_QUERY_RESULT_AVAILABLE, &avail);
      if (!avail) return;
      std::vector<GLuint64> t(f.marks.size());
      for (size_t i = 0; i < f.marks.size(); ++i) {
        glGetQueryObjectui64v(f.marks[i].q, GL_QUERY_RESULT, &t[i]);
        pool.push_back(f.marks[i].q);
      }
      for (size_t i = 0; i + 1 < f.marks.size(); ++i) {
        auto it = acc.find(f.marks[i].label);
        if (it == acc.end()) {
          order.push_back(f.marks[i].label);
          it = acc.emplace(f.marks[i].label, 0.0).first;
        }
        it->second += (double)(t[i + 1] - t[i]) / 1e6;
        acc_draws[f.marks[i].label] += (double)f.marks[i].draws;
      }
      double total = (double)(t.back() - t.front()) / 1e6;
      acc["total"] += total;
      pending.pop_front();
      if (++frames == 300) Report();
    }
  }
  void Report() {
    printf("[RESGPU] scale %gx, GPU ms per frame over %d frames: total %.3f\n",
           (double)s_scale, frames, acc["total"] / frames);
    for (const auto &l : order) {
      const double ms = acc[l] / frames;
      const double d = acc_draws[l] / frames;
      if (ms >= 0.005) {
        if (d > 0) printf("[RESGPU]   %8.3f  %s (%.0f GX draws)\n", ms, l.c_str(), d);
        else printf("[RESGPU]   %8.3f  %s\n", ms, l.c_str());
      }
    }
    std::fflush(stdout);
    acc_draws.clear();
    acc.clear();
    order.clear();
    frames = 0;
  }
};
ResGpuProf g_res_gpu;
}  // namespace

const std::vector<GXCommand *> *g_render_draws = nullptr;
// R14 R5: the frame's draws' emit-cache entries (parallel to g_render_draws).
const std::vector<const void *> *g_render_entries = nullptr;
// R14 R5: the frame's packed non-display-list draws (see xf::PackHdr).
const std::vector<uint32_t> *g_render_pack = nullptr;
// r36r NWII_R_PREFIX: register writes of dropped frames, run before the frame (gx.cpp).
const std::vector<GXCommand> *g_render_prefix = nullptr;
// r40d NWII_D_DROPFAST: first command of the frame to render (gx.cpp; the ones
// before it are the dropped part of the batch, whose kept writes are in the prefix).
size_t g_render_start = 0;
// r40d NWII_D_MERGE_CHECK (gx.cpp): this frame with every merged FIFO draw split back into its parts.
const std::vector<GXCommand> *g_d_merge_alt = nullptr;

// R14 R5, GPU XF: prefetch from the worker's entry list, without the
// dependent loads of the old prefetch (command -> DecodedVerts -> entry, each
// a likely cache miss on its own: ~28 % of RenderOnce on the PC's LP-E cores).
// A draw with an entry and an arena slot reads only the entry's tail (x_init,
// xi, x_slot, x_n); one without (not a display list, or new) reads its vertices.
static inline void PrefetchDrawAheadXF(const std::vector<GXCommand *> &draws,
                                       const std::vector<const void *> &ents, size_t d) {
  const size_t nd = ents.size();
  if (d + 6 < nd) {
    const void *e = ents[d + 6];
    if (e && !(reinterpret_cast<uintptr_t>(e) & 1)) {
      const auto *ce = static_cast<const ecache::Entry *>(e);
      __builtin_prefetch(&ce->x_init);
      __builtin_prefetch(reinterpret_cast<const char *>(&ce->xi) + 64);
      __builtin_prefetch(&ce->x_n);
      __builtin_prefetch(&ce->x_bb[5]);  // r36r NWII_R_CULL
    }
  }
  if (d + 4 < nd) {
    const uintptr_t t = reinterpret_cast<uintptr_t>(ents[d + 4]);
    if ((t & 1) && g_render_pack) {
      // packed: its words, about to be copied into the ring
      const size_t off = t >> 1;
      if (off < g_render_pack->size()) {
        const char *w = reinterpret_cast<const char *>(g_render_pack->data() + off);
        for (int o = 0; o < 512; o += 64) __builtin_prefetch(w + o);
      }
    }
  }
  // A draw without an entry and not packed reads its vertices (Analyze,
  // WriteVerts). One with an entry reads them only the first time (no arena
  // slot yet): not prefetched, so that the entry is not read here.
  if (d + 2 < nd && !ents[d + 2]) {
    const GXCommand &c = *draws[d + 2];
    const auto &vv = c.verts();
    const char *b = (const char *)vv.data();
    const size_t bytes = std::min<size_t>(vv.size() * sizeof(VertexData), 1536);
    for (size_t o = 0; o < bytes; o += 64) __builtin_prefetch(b + o);
  }
}

// R7 perf: the world map draws ~11k display-list draws per frame, each
// reading its own heap-allocated vertex array (~7 vertices, ~1 KB) through a
// shared_ptr: shared_ptr -> vector header -> data, two dependent cache misses
// per draw before the first vertex, which on a core without L3 cost about
// as much as transforming the vertices. Prefetch both a few draws ahead.
static inline void PrefetchDrawAhead(const std::vector<GXCommand *> &draws,
                                     size_t d) {
  if (d + 6 < draws.size()) {
    if (const auto *sv = draws[d + 6]->shared_vertices.get())
      __builtin_prefetch(sv);
  }
  // R9: a draw with an emit-cache entry (see ecache::) likely copies its
  // stored output instead of reading its vertices: prefetch the entry, then
  // the output.
  if (d + 4 < draws.size()) {
    if (const auto *sv = draws[d + 4]->shared_vertices.get())
      if (const void *e = sv->emit_cache.get()) {
        if (xf::Active()) {
          // R14 R5: the fields GPU XF reads (x_init, xi, x_slot: the entry's
          // last ~110 bytes), not its head.
          const auto *ce = static_cast<const ecache::Entry *>(e);
          __builtin_prefetch(&ce->x_init);
          __builtin_prefetch(reinterpret_cast<const char *>(&ce->xi) + 64);
          __builtin_prefetch(&ce->x_slot);
        } else {
          __builtin_prefetch(e);
        }
      }
  }
  if (d + 2 < draws.size()) {
    const GXCommand &c = *draws[d + 2];
    const char *b = nullptr;
    size_t bytes = 0;
    const auto *ce = c.shared_vertices
                         ? static_cast<const ecache::Entry *>(c.shared_vertices->emit_cache.get())
                         : nullptr;
    // R14 R5: GPU XF reads only the entry's layout and arena slot (xi, x_slot)
    // of a display-list draw whose raw vertices are already in the arena: its
    // vertices are not read at all (the old prefetch below pulled ~1.5 KB of
    // them, 24 cache lines, for each of the ~11 000 draws of a world-map frame).
    if (ce && xf::Active() && ce->x_init && ce->x_slot >= 0) return;
    if (ce && ce->slot >= 0) {
      b = (const char *)ce->in_tg.data();  // the hit test reads these first
      bytes = std::min<size_t>(ce->in_tg.size() * 4, 512);
    } else if (ce && !ce->out.empty()) {
      b = (const char *)ce->out.data();
      bytes = std::min<size_t>(ce->out.size() * sizeof(BatchVertex), 1536);
    } else {
      const auto &vv = c.verts();
      b = (const char *)vv.data();
      bytes = std::min<size_t>(vv.size() * sizeof(VertexData), 1536);
    }
    for (size_t o = 0; o < bytes; o += 64) __builtin_prefetch(b + o);
  }
}

// R14 R5: the debug/test switches read on every GX command or draw, read once.
// Thread-safe (std::call_once): the render worker may call it too.
static std::once_flag s_flags_once;
static void InitFlags() {
  std::call_once(s_flags_once, [] {
    rph::InitLevel();
    s_drawstats_on = NWII_ENV("NWII_DRAWSTATS") != nullptr;
    {
      const char *e = NWII_ENV("NWII_DRAWTRACE");
      s_dt_on = e && std::sscanf(e, "%ld-%ld", &s_dt_from, &s_dt_to) == 2;
    }
    {
      const char *e = NWII_ENV("NWII_ECACHE");
      ecache::s_on = !(e && e[0] == '0');
    }
    ecache::s_check_on = NWII_ENV("NWII_ECACHE_CHECK") != nullptr;
    {  // r36r (r13)
      const char *dl = NWII_ENV("NWII_DRAWLOG");
      s_dl_on = dl != nullptr;
      s_dl_from = dl ? std::atol(dl) : 0;
      s_dl_n = NWII_ENV("NWII_DRAWLOG_N") ? std::atol(NWII_ENV("NWII_DRAWLOG_N")) : 40;
      s_xflog_n = NWII_ENV("NWII_XFLOG") ? std::atol(NWII_ENV("NWII_XFLOG")) : 0;
    }
    s_emit4_on = NWII_ENV("NWII_NOEMIT4") == nullptr;
  });
}

// R14 R5: called for every draw of a frame right after it is decoded, on the
// render worker (or, without one, on the render thread inside the drain),
// before the frame is handed to the render thread. A display-list draw's
// emit-cache entry is made here, with what the renderer needs from its
// vertices worked out once (texgen/position input masks; GPU XF layout and
// matrix indices: xf::Analyze, a pure function of the decoded vertices, which
// never change). The render thread then reads only the entry, never the
// ~1-2 KB of decoded vertices, for every frame that draws it. Thread safety: an
// entry is written here only while it does not exist yet (emit_cache null); a
// packet reaches the render thread only after this ran for all its draws (the
// hand-over mutex orders it), and the render thread never writes emit_cache
// itself unless the entry is missing (counted: "made late"), so the two
// threads never write the same field. Entries die on the worker as before.
// r36r NWII_R_CULL: the raw position bounding box of a cached draw (see xf::Culled).
static bool BBoxOf(const xf::Info &xi, const std::vector<VertexData> &v, float out[6]) {
  if (!xi.ok || !xi.hp || v.empty()) return false;
  float b[6] = {v[0].pos[0], v[0].pos[1], v[0].pos[2], v[0].pos[0], v[0].pos[1], v[0].pos[2]};
  for (const VertexData &x : v) {
    for (int k = 0; k < 3; ++k) {
      if (!(x.pos[k] == x.pos[k]) || std::isinf(x.pos[k])) return false;  // NaN/inf: never culled
      b[k] = std::min(b[k], x.pos[k]);
      b[3 + k] = std::max(b[3 + k], x.pos[k]);
    }
  }
  std::memcpy(out, b, sizeof b);
  return true;
}
static void SetBoundingBox(ecache::Entry &e, const std::vector<VertexData> &v) {
  e.x_bb_ok = BBoxOf(e.xi, v, e.x_bb);
}

// r40d NWII_D_FASTPREP=1: xf::Analyze and BBoxOf in ONE pass over the decoded
// vertices (~200 bytes each, ~6 500 per frame of FIFO draws in agua_pinguino)
// instead of two; the same Info (also when Analyze gives up half way) and the
// same box. NWII_D_FASTPREP_CHECK=1 packs every draw the old way too and compares.
static bool FastPrepOn() {
  static const bool on = [] { const char *e = NWII_ENV("NWII_D_FASTPREP"); return e && e[0] && e[0] != '0'; }();
  return on;
}
static bool FastPrepCheckOn() {
  static const bool on = NWII_ENV("NWII_D_FASTPREP_CHECK") != nullptr;
  return on;
}
static xf::Info AnalyzeBBox(const std::vector<VertexData> &v, bool &bb_ok, float bb[6]) {
  xf::Info in;
  bb_ok = false;
  if (v.empty()) return in;
  const VertexData &a = v[0];
  in.hp = a.has_pos;
  in.hn = a.has_norm;
  in.hc = a.has_color;
  in.tmask = xf::TexMask(a);
  float b[6] = {a.pos[0], a.pos[1], a.pos[2], a.pos[0], a.pos[1], a.pos[2]};
  bool finite = true;
  for (const VertexData &x : v) {
    if (x.pre_xf || x.has_pos != in.hp || x.has_norm != in.hn || x.has_color != in.hc ||
        xf::TexMask(x) != in.tmask || xf::Rank(in.pos, in.npos, x.posMtxIdx) < 0)
      return in;
    for (int t = 0; t < 4; ++t)
      if (xf::Rank(in.tex[t], in.ntex[t], x.texMtxIdx[t]) < 0) return in;
    if (finite)
      for (int k = 0; k < 3; ++k) {
        if (!(x.pos[k] == x.pos[k]) || std::isinf(x.pos[k])) { finite = false; break; }
        b[k] = std::min(b[k], x.pos[k]);
        b[3 + k] = std::max(b[3 + k], x.pos[k]);
      }
  }
  in.ranks = in.npos > 1 || in.ntex[0] > 1 || in.ntex[1] > 1 || in.ntex[2] > 1 || in.ntex[3] > 1;
  in.stride = (uint8_t)(3 + (in.hn ? 3 : 0) + (in.hc ? 1 : 0) + 2 * __builtin_popcount(in.tmask) +
                        (in.ranks ? 1 : 0));
  in.ok = true;
  if (in.hp && finite) {
    std::memcpy(bb, b, sizeof b);
    bb_ok = true;
  }
  return in;
}

const void *GX_PrepDrawForRender(const GXCommand &cmd, std::vector<uint32_t> &pack) {
  static thread_local bool t_flags = false;
  if (!t_flags) { t_flags = true; InitFlags(); }
  if (!cmd.shared_vertices) {
    // R14 R5: any other draw (vertices decoded for this frame only, ~30 000
    // vertices per frame in busy levels): GPU XF's Analyze() and WriteVerts()
    // here, the render thread only copies the words into its ring. Returned
    // as a tagged offset into `pack` ((offset << 1) | 1).
    if (!xf::s_on_w.load(std::memory_order_relaxed)) return nullptr;
    const auto &v = cmd.vertices;
    const uint32_t n = (uint32_t)v.size();
    if (!n || (cmd.prim_type & 0xF8) >= 0xA8) return nullptr;
    xf::PackHdr h;
    if (FastPrepOn()) {  // r40d
      h.xi = AnalyzeBBox(v, h.bb_ok, h.bb);
      if (FastPrepCheckOn()) {
        static uint64_t nchk = 0, bad = 0;
        xf::PackHdr o;
        o.xi = xf::Analyze(v);
        o.bb_ok = BBoxOf(o.xi, v, o.bb);
        ++nchk;
        auto info_eq = [](const xf::Info &x, const xf::Info &y) {  // (pos/tex past the counts are not set)
          if (x.ok != y.ok || x.hp != y.hp || x.hn != y.hn || x.hc != y.hc || x.ranks != y.ranks ||
              x.tmask != y.tmask || x.stride != y.stride || x.npos != y.npos ||
              std::memcmp(x.ntex, y.ntex, 4) || std::memcmp(x.pos, y.pos, x.npos))
            return false;
          for (int t = 0; t < 4; ++t)
            if (std::memcmp(x.tex[t], y.tex[t], x.ntex[t])) return false;
          return true;
        };
        const bool same = info_eq(o.xi, h.xi) && o.bb_ok == h.bb_ok &&
                          (!o.bb_ok || std::memcmp(o.bb, h.bb, sizeof o.bb) == 0);
        if (!same) ++bad;
        if (nchk % 200000 == 0 || (!same && bad <= 10))
          printf("[D40PREP_CHECK] draws=%llu mismatches=%llu\n", (unsigned long long)nchk, (unsigned long long)bad);
      }
    } else {
      h.xi = xf::Analyze(v);
      h.bb_ok = BBoxOf(h.xi, v, h.bb);  // r36r
    }
    h.n = n;
    h.words = h.xi.ok ? n * h.xi.stride : 0;
    const size_t off = pack.size();
    pack.resize(off + xf::kPackHdrWords + h.words);
    std::memcpy(pack.data() + off, &h, sizeof h);
    if (h.words) xf::WriteVerts(v, n, h.xi, pack.data() + off + xf::kPackHdrWords);
    return reinterpret_cast<const void *>((uintptr_t)((off << 1) | 1));
  }
  if (!ecache::On()) return nullptr;
  const DecodedVerts &dv = *cmd.shared_vertices;
  if (dv.emit_cache) return dv.emit_cache.get();
  auto e = MakeHost<ecache::Entry>();
  ecache::InitDeps(*e, dv);
  e->xi = xf::Analyze(dv);
  SetBoundingBox(*e, dv);  // r36r
  e->x_init = true;
  dv.emit_cache = e;
  ecache::s_entries_prep.fetch_add(1, std::memory_order_relaxed);
  return dv.emit_cache.get();
}

class RendererGL : public IRenderer {
public:
  RendererGL() = default;
  ~RendererGL() override = default;

  void Initialize(void *window) override { m_window = window; }

  // NWII_R9_FRAMECHECK=<n>: every n-th Render() that copies to the XFB is
  // drawn three times from the same commands and the same GX state: R9 paths,
  // then the r8 paths (no vbuf, no emit cache), then R9 again, and the XFB
  // images of the last two are compared pixel by pixel ("[FRAMECHECK] ...";
  // the third image is the one presented). Only for tests. The first pass is
  // a warm-up: a frame that samples an EFB copy (or an animated tile the
  // copies are stitched into) before making it again would see the previous
  // frame's copy in a single pass but this frame's in a repeated one, so only
  // repeated passes are compared ("pass1" counts how often the first one
  // differs too, which includes that effect).
  // r40d NWII_D_MERGE_CHECK: the frame is drawn as usual (presented pass first, as
  // FRAMECHECK does, so the compared passes are both repeats), then from the split copy and
  // again merged, both without the worker's draw lists (same emit path), and the XFB images
  // of the last two are compared pixel by pixel ("[D40MERGE_CHECK]").
  void MergeCheck(const std::vector<GXCommand> &commands) {
    static GXState *saved = new GXState;
    std::memcpy((void *)saved, (const void *)&g_state, sizeof(GXState));
    const long copies0 = s_xfb_copies;
    RenderOnce(commands);
    if (s_xfb_copies == copies0 || !s_xfb_w) return;
    const long copies1 = s_xfb_copies;
    static std::vector<uint8_t> a, b;
    const auto *dr = g_render_draws;
    const auto *en = g_render_entries;
    const size_t st = g_render_start;
    auto pass = [&](const std::vector<GXCommand> &cmds, size_t start, std::vector<uint8_t> &out) {
      std::memcpy((void *)&g_state, (const void *)saved, sizeof(GXState));
      s_xfb_copies = copies0;
      g_render_draws = nullptr;
      g_render_entries = nullptr;
      g_render_start = start;
      RenderOnce(cmds);
      ReadXfb(out);
    };
    pass(*g_d_merge_alt, 0, b);
    pass(commands, st, a);
    g_render_draws = dr;
    g_render_entries = en;
    g_render_start = st;
    s_xfb_copies = copies1;
    static uint64_t checks = 0, same = 0;
    size_t diff = 0;
    if (a.size() != b.size()) diff = std::max(a.size(), b.size()) / 4;
    else
      for (size_t i = 0; i < a.size(); i += 4)
        if (std::memcmp(&a[i], &b[i], 4)) ++diff;
    ++checks;
    if (!diff) ++same;
    static int logged = 0;
    if ((diff && logged++ < 16) || checks % 20 == 0)
      printf("[D40MERGE_CHECK] checks=%llu identical=%llu | xfb %ld: %zu pixels differ\n",
             (unsigned long long)checks, (unsigned long long)same, s_xfb_copies, diff);
  }
  void Render(const std::vector<GXCommand> &commands) override {
    if (g_d_merge_alt && s_xfb_fbo) {  // r40d
      MergeCheck(commands);
      return;
    }
    static const long every = [] {
      const char *e = NWII_ENV("NWII_R9_FRAMECHECK");
      return e ? std::max(1L, std::atol(e)) : 0L;
    }();
    static long n = 0;
    if (!every || (++n % every) != 0 || !s_xfb_fbo) {
      RenderOnce(commands);
      return;
    }
    static GXState *saved = new GXState;
    std::memcpy((void *)saved, (const void *)&g_state, sizeof(GXState));
    const long copies0 = s_xfb_copies;
    RenderOnce(commands);
    if (s_xfb_copies == copies0 || !s_xfb_w) return;  // no XFB copy: nothing to compare
    static std::vector<uint8_t> p1, a, b;
    ReadXfb(p1);
    const long copies1 = s_xfb_copies;
    auto again = [&](bool r8) {
      std::memcpy((void *)&g_state, (const void *)saved, sizeof(GXState));
      s_xfb_copies = copies0;
      const bool vb = vbuf::s_on;
      if (r8) vbuf::s_on = false;
      s_r8_pass = r8;
      nwii::runtime::hle::g_r2_off_pass = r8;  // r39r2: the r8 pass also takes the old copy/clear paths
      RenderOnce(commands);
      s_r8_pass = false;
      nwii::runtime::hle::g_r2_off_pass = false;
      vbuf::s_on = vb;
      s_xfb_copies = copies1;
    };
    again(true);
    ReadXfb(b);
    again(false);
    ReadXfb(a);
    static uint64_t checks = 0, same = 0, p1diff = 0;
    ++checks;
    auto count = [](const std::vector<uint8_t> &x, const std::vector<uint8_t> &y, int &maxd) {
      size_t diff = 0;
      maxd = 0;
      if (x.size() != y.size()) return std::max(x.size(), y.size()) / 4;
      for (size_t i = 0; i < x.size(); i += 4) {
        int d = 0;
        for (int c = 0; c < 4; ++c) d = std::max(d, std::abs((int)x[i + c] - (int)y[i + c]));
        if (d) { ++diff; maxd = std::max(maxd, d); }
      }
      return diff;
    };
    int maxd = 0, maxd1 = 0;
    const size_t diff = count(a, b, maxd);
    if (count(p1, b, maxd1)) ++p1diff;
    if (!diff) ++same;
    // NWII_R9_FRAMECHECK_DUMP=<prefix>: the first 6 differing frames as
    // <prefix>_<xfb>_{r9,r8}.ppm (bottom-up rows flipped, RGB).
    static const char *dump = NWII_ENV("NWII_R9_FRAMECHECK_DUMP");
    static int dumped = 0;
    if (diff && dump && a.size() == b.size() && dumped < 6) {
      ++dumped;
      for (int k = 0; k < 2; ++k) {
        char path[512];
        std::snprintf(path, sizeof path, "%s_%06ld_%s.ppm", dump, s_xfb_copies, k ? "r8" : "r9");
        if (FILE *f = std::fopen(path, "wb")) {
          const auto &px = k ? b : a;
          std::fprintf(f, "P6\n%d %d\n255\n", s_xfb_w, s_xfb_h);
          for (int y = s_xfb_h - 1; y >= 0; --y)
            for (int x = 0; x < s_xfb_w; ++x) std::fwrite(&px[((size_t)y * s_xfb_w + x) * 4], 1, 3, f);
          std::fclose(f);
        }
      }
    }
    static int logged = 0;
    if ((diff && logged++ < 16) || checks % 20 == 0)
      printf("[FRAMECHECK] checks=%llu identical=%llu (pass1 differs %llu) | xfb %ld %dx%d "
             "scale %gx: %zu pixels differ (max %d)\n",
             (unsigned long long)checks, (unsigned long long)same, (unsigned long long)p1diff,
             s_xfb_copies, s_xfb_w, s_xfb_h, (double)s_scale, diff, maxd);
  }

  static void ReadXfb(std::vector<uint8_t> &px) {
    px.resize((size_t)s_xfb_w * s_xfb_h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_xfb_fbo);
    glReadPixels(0, 0, s_xfb_w, s_xfb_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
  }

  void RenderOnce(const std::vector<GXCommand> &commands) {
    static bool s_flags = false;
    if (!s_flags) {
      s_flags = true; InitFlags(); R2CopyTraceOn(); s_r2ks_on = NWII_ENV("NWII_R2_KEYSTAT") != nullptr;
      s_p_runstat = NWII_ENV("NWII_P_RUNSTAT") != nullptr;  // r45p (diagnóstico)
      s_p_stalelog = NWII_ENV("NWII_P_STALELOG") != nullptr;  // r45p (diagnóstico)
      const char *tm = NWII_ENV("NWII_R2_TEXMEMO");
      s_r2_texmemo_on = tm && tm[0] == '1';
      if (s_r2_texmemo_on) printf("[R2] NWII_R2_TEXMEMO=1: texture lookups remembered between copies\n");
    }
    ++s_r2_tm_epoch;  // r39r2 NWII_R2_TEXMEMO
    if (s_r2_texmemo_on && (s_r2ct_frame % 600) == 0 && s_r2_tm_hits + s_r2_tm_miss) {
      printf("[R2TM] texture lookups: remembered %llu, looked up %llu\n", (unsigned long long)s_r2_tm_hits,
             (unsigned long long)s_r2_tm_miss);
      s_r2_tm_hits = s_r2_tm_miss = 0;
    }
    R2KeyStatFrame();
    ++s_r2ct_frame;
    rph::Set(rph::kFrame);
    // The frontend binds the EFB before every drain; read it (and its
    // colour texture) once.
    if (!s_efb_fbo) {
      GLint fbo = 0, tex = 0;
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
      glGetFramebufferAttachmentParameteriv(
          GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &tex);
      s_efb_fbo = (GLuint)fbo;
      s_efb_tex = (GLuint)tex;
      nwii::runtime::hle::TextureCache::get().set_restore_read_fbo(s_efb_fbo);
    }
    xf::PCullFrame();  // r45p NWII_P_CULLSKIP
    // The frontend touches GL between frames: forget the cached state.
    GLInvalidate();
    s_depth_range[0] = s_depth_range[1] = -1.0;
    s_state_dirty = true;
    s_batch_open = false;
    CEnable(kEnScissor, GL_SCISSOR_TEST, false);
    // GX clamps depth instead of clipping at the near/far planes; layouts sit
    // exactly on (or a hair past) the near plane and would vanish otherwise.
    glEnable(GL_DEPTH_CLAMP);
    static const bool s_legacy_clear = NWII_ENV("NWII_FLATZ") != nullptr;
    if (s_legacy_clear) {
      glDepthMask(GL_TRUE);
      glClear(GL_DEPTH_BUFFER_BIT);
    }
    s_compiles_this_frame = 0; // reset the per-frame shader compile budget
    // R11: the vertex path (GPU XF or classic) first: the programs, the
    // separable vertex program and the vertex buffers depend on it.
    static bool s_paths = false;
    if (!s_paths) {
      s_paths = true;
      xf::Init();
      shcache::Init();
      GLInvalidate();
    }
    PrewarmPrograms();
    vbuf::Init();
    // R11 worker: arena slots (vbuf and GPU XF) of entries freed on the render
    // worker, retired here before the frame's Recycle().
    vbuf::TakeDeferred();
    if (xf::Active()) {
      xf::BeginFrame();
    } else if (vbuf::s_on) {
      vbuf::Poll();
      vbuf::Recycle();
    }
    // Capture record of this batch ([RPROF] counters may be reset between
    // batches, so they are read as deltas inside Render only).
    s_cap_cur = CapStats{};
    const auto cap_t0 = std::chrono::steady_clock::now();
    if (GpuTimer::On()) g_gpu_timer.Begin();
    if (ResGpuProf::On()) g_res_gpu.BeginFrame();
    const uint64_t cap_up0 = g_prof_tex_uploads, cap_sh0 = g_prof_shader_compiles;
    if (g_state.pe_clear_pending) {
      g_state.pe_clear_pending = false;
      unsigned char cc[4];
      ::GX_GetClearColor(cc);
      glClearColor(cc[0] / 255.f, cc[1] / 255.f, cc[2] / 255.f, cc[3] / 255.f);
      glClearDepth((g_state.bp[0x51] & 0xFFFFFF) / 16777215.0);
      glDepthMask(GL_TRUE);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // NWII_NOBATCH=1: the old behaviour, a batch ends at every non-draw
    // command (for A/B measurements).
    static const bool s_nobatch = NWII_ENV("NWII_NOBATCH") != nullptr;
    static const bool s_noprefetch = NWII_ENV("NWII_NOPREFETCH") != nullptr;
    const std::vector<GXCommand *> *draws = s_noprefetch ? nullptr : g_render_draws;
    // R14 R5: the worker's per-draw entries (GPU XF; not in the r8 pass of
    // NWII_R9_FRAMECHECK, where the emit cache is off).
    const std::vector<const void *> *ents =
        (draws && g_render_entries && g_render_entries->size() == draws->size() &&
         xf::Active() && ecache::On())
            ? g_render_entries : nullptr;
    size_t draw_ord = 0;  // index of the next draw in *draws
    // r36r NWII_R_PREFIX (gx.cpp): the register writes of the frames dropped
    // before this one (no draws) come in their own list and run first, exactly
    // where they would sit at the start of `commands`.
    const std::vector<GXCommand> *const pre =
        (g_render_prefix && !g_render_prefix->empty()) ? g_render_prefix : nullptr;
    // r36r: early cull (NWII_R_CULL; off with the draw log debug switch).
    static const bool s_early_cull = xf::CullOn() && NWII_ENV("NWII_DRAWLOG") == nullptr;
    static const bool s_cull_state = [] {
      const char *e = NWII_ENV("NWII_R_CULL");
      return e && e[0] == '2';
    }();
    for (int pass = pre ? 0 : 1; pass < 2; ++pass) {
    const GXCommand *const cmd0 = pass == 0 ? pre->data() : commands.data();
    const size_t ncmd = pass == 0 ? pre->size() : commands.size();
    for (size_t ci = pass == 0 ? 0 : std::min(g_render_start, ncmd); ci < ncmd; ++ci) {  // r40d: start
      const GXCommand &cmd = cmd0[ci];
      // r45p NWII_P_RUNSTAT=1 (diagnóstico): qué hay entre un draw y el siguiente
      if (__builtin_expect(s_p_runstat, 0)) PRunStatCmd(cmd);
      // R14 R5: the command array is walked once, in order, 136 bytes a
      // command (~45 000 per world-map frame, ~6 MB: the first load of each was
      // the top miss of RenderOnce). Fetch 8 commands ahead.
      if (ci + 8 < ncmd) {
        const char *pf = reinterpret_cast<const char *>(cmd0 + ci + 8);
        __builtin_prefetch(pf);
        __builtin_prefetch(pf + 72);
      }
      if (rph::On())
        rph::Set(cmd.type == GXCommandType::DrawPrimitive ? rph::kState : rph::kRegs);
      if (cmd.type == GXCommandType::DrawPrimitive) {
        if (!s_r2_pend.empty()) { R2FlushCopies(); s_state_dirty = true; }  // r39r2 NWII_R2_COPYMRT
        if (s_r2_efb_off) R2EfbBack();  // r39r2 NWII_R2_LAZYEFB
      }
      if (s_r2ct_every && R2CopyTraceOn()) {  // r39r2 debug
        if (cmd.type == GXCommandType::DrawPrimitive) R2TraceDraw();
        else if (cmd.type == GXCommandType::BPRegister && (uint8_t)cmd.reg == 0x52)
          printf("[R2CT] C src=%u,%u %ux%u dest=%08X clear=%d xfb=%d\n", g_state.efbSrcX, g_state.efbSrcY,
                 g_state.efbW, g_state.efbH, g_state.efbCopyDest, (cmd.val >> 11) & 1, (cmd.val >> 14) & 1);
      }
      if (s_nobatch && cmd.type != GXCommandType::DrawPrimitive && s_batch_open) {
        FlushBatch();
        s_batch_open = false;
      }
      if (cmd.type == GXCommandType::XFRegister) {
        int addr = cmd.reg;
        bool touched_proj = false;
        // NWII_XFLOG=<n>: print the first n distinct XF writes (address,
        // count, first word) to 0x1000-0x10FF.
        const long s_xflog = s_xflog_n;  // r36r (r13): read once by InitFlags
        if (s_xflog && addr >= 0x1000 && !cmd.payload.empty()) {
          static long xn = 0;
          static std::unordered_map<uint64_t, int> seen;
          uint32_t w0;
          std::memcpy(&w0, &cmd.payload[0], 4);
          const uint64_t key = ((uint64_t)addr << 40) ^
                               ((uint64_t)cmd.payload.size() << 32) ^ w0;
          if (seen.emplace(key, 1).second && xn++ < s_xflog) {
            printf("[XFLOG] %04X n=%zu:", addr, cmd.payload.size());
            for (size_t i = 0; i < cmd.payload.size() && i < 8; ++i) {
              uint32_t w;
              std::memcpy(&w, &cmd.payload[i], 4);
              printf(" %08X", w);
            }
            printf("\n");
          }
        }
        // R7 perf: plain matrix loads (position/texture, normal and post
        // matrix memory: no XF register, no dirty tracking) are one copy.
        const size_t np = cmd.payload.size();
        float *mtx_dst = nullptr;
        if (addr >= 0 && addr + np <= 0x100)
          mtx_dst = &g_state.posMatrices[addr];
        else if (addr >= 0x400 && addr + np <= 0x460)
          mtx_dst = &g_state.normalMatrices[addr - 0x400];
        else if (addr >= 0x500 && addr + np <= 0x600)
          mtx_dst = &g_state.postMatrices[addr - 0x500];
        if (mtx_dst) {
          if (np) {
            std::memcpy(mtx_dst, cmd.payload.data(), np * sizeof(float));
            // r36r NWII_R_TGMEMO: rows (4 floats) written
            if (addr < 0x100 || addr >= 0x500) {
              const int a = addr < 0x100 ? addr : addr - 0x500;
              const int r0 = a >> 2, r1 = (a + (int)np - 1) >> 2;
              const uint64_t m = (r1 - r0 >= 63) ? ~0ull : (((2ull << (r1 - r0)) - 1) << r0);
              if (addr < 0x100) xf::s_rows_pos_w |= m;
              else xf::s_rows_post_w |= m;
            }
          }
          continue;
        }
        for (size_t i = 0; i < cmd.payload.size(); ++i) {
          int current_addr = addr + (int)i;
          uint32_t w;
          std::memcpy(&w, &cmd.payload[i], 4);
          if (current_addr >= 0x1000 && current_addr < 0x1000 + 256) {
            uint32_t &x = g_state.xf[current_addr - 0x1000];
            // Channel colours/control (0x0A-0x11) and the viewport
            // (0x1A-0x1F) are GL state; texgen and matrix indices are
            // consumed on the CPU at emit time.
            if (x != w && ((current_addr >= 0x100A && current_addr <= 0x1011) ||
                           (current_addr >= 0x101A && current_addr <= 0x101F)))
              s_state_dirty = true;
            x = w;
          }
          if (current_addr >= 0x0000 && current_addr <= 0x00FF) {
            g_state.posMatrices[current_addr] = cmd.payload[i];
            xf::s_rows_pos_w |= 1ull << (current_addr >> 2);  // r36r
          } else if (current_addr >= 0x0400 && current_addr <= 0x045F) {
            g_state.normalMatrices[current_addr - 0x0400] = cmd.payload[i];
          } else if (current_addr >= 0x0500 && current_addr <= 0x05FF) {
            g_state.postMatrices[current_addr - 0x0500] = cmd.payload[i];
            xf::s_rows_post_w |= 1ull << ((current_addr - 0x0500) >> 2);  // r36r
          } else if (current_addr >= 0x0600 && current_addr <= 0x067F) {
            int li = (current_addr - 0x0600) / 16;
            float &l = g_state.lights[li][(current_addr - 0x0600) % 16];
            if (std::memcmp(&l, &w, 4))
              s_state_dirty = true;
            l = cmd.payload[i];
          } else if (current_addr >= 0x1020 && current_addr <= 0x1026) {
            float &pr = g_state.projection[current_addr - 0x1020];
            if (std::memcmp(&pr, &w, 4))
              s_state_dirty = true;
            pr = cmd.payload[i];
            if (current_addr == 0x1026) {
              std::memcpy(&g_state.projType, &cmd.payload[i], 4);
            }
            touched_proj = true;
          }
        }
        if (touched_proj && !g_state.projSet) {
          g_state.projSet = true;
          s_state_dirty = true;
        }
      } else if (cmd.type == GXCommandType::BPRegister) {
        const uint8_t reg = (uint8_t)cmd.reg;
        if (reg == 0x52 || reg == 0x64 || reg == 0x65) ++s_r2_tm_epoch;  // r39r2 NWII_R2_TEXMEMO
        if (reg == 0x52) {
          rph::Set(rph::kCopy);
          // EFB copies and clears read/write the framebuffer: everything
          // drawn so far must be in it.
          if (s_batch_open) {
            FlushBatch();
            s_batch_open = false;
          }
        }
        const uint32_t old = g_state.bp[reg];
        uint32_t old_tev[4] = {0, 0, 0, 0};
        const bool tev_reg = reg >= 0xE0 && reg <= 0xE7;
        if (tev_reg) {
          const int idx = (reg - 0xE0) / 2, half = (reg - 0xE0) & 1;
          old_tev[0] = g_state.tevColorReg[idx][half];
          old_tev[1] = g_state.tevKonstReg[idx][half];
        }
        ApplyBPRegister(reg, cmd.val);
        if (tev_reg) {
          const int idx = (reg - 0xE0) / 2, half = (reg - 0xE0) & 1;
          if (old_tev[0] != g_state.tevColorReg[idx][half] ||
              old_tev[1] != g_state.tevKonstReg[idx][half])
            s_state_dirty = true;
        } else if ((old != g_state.bp[reg] && BpAffectsDraw(reg)) ||
                   reg == 0x65 /* TLUT load: palette content */) {
          s_state_dirty = true;
        }
        if (reg == 0x52) {
          if (s_cap_cur.ncopies < 24) {
            CapCopy &cc = s_cap_cur.copies[s_cap_cur.ncopies];
            cc.dest = (cmd.val & 0x4000) ? g_state.xfbAddr : g_state.efbCopyDest;
            cc.w = g_state.efbW;
            cc.h = g_state.efbH;
            cc.fmt = (uint8_t)CopyRealFormat(cmd.val);
            cc.flags = ((cmd.val & 0x4000) ? 1 : 0) | (((cmd.val >> 9) & 1) ? 2 : 0) |
                       (cmd.stale_copy ? 4 : 0) | (((cmd.val >> 11) & 1) ? 8 : 0);
          }
          ++s_cap_cur.ncopies;
          if ((cmd.val & 0x4000) && cmd.stale_copy) ++s_cap_cur.stale_frames;
          if (DrawTraceOn())
            printf("[DT] f=%ld COPY %s%s dest=%08X src=(%u,%u) %ux%u clear=%d fmt=%u half=%d\n",
                   s_xfb_copies + 1, (cmd.val & 0x4000) ? "XFB" : "TEX",
                   cmd.stale_copy ? "(stale)" : "", g_state.efbCopyDest,
                   g_state.efbSrcX, g_state.efbSrcY, g_state.efbW, g_state.efbH,
                   (cmd.val >> 11) & 1, CopyRealFormat(cmd.val), (cmd.val >> 9) & 1);
          if (NWII_ENV("NWII_DRAWLOG"))
            printf("[COPY] %s%s dest=%08X src=(%u,%u) %ux%u clear=%d fmt=%u "
                   "half=%d int=%d pf=%u stride=%u\n",
                   (cmd.val & 0x4000) ? "XFB" : "TEX",
                   cmd.stale_copy ? "(stale)" : "", g_state.efbCopyDest,
                   g_state.efbSrcX, g_state.efbSrcY, g_state.efbW, g_state.efbH,
                   (cmd.val >> 11) & 1, CopyRealFormat(cmd.val),
                   (cmd.val >> 9) & 1, ((cmd.val >> 15) & (cmd.val >> 16)) & 1,
                   g_state.bp[0x43] & 7, (g_state.bp[0x4D] & 0x3FF) << 5);
          if (ResGpuProf::On() && !cmd.stale_copy) {
            const bool hf = (cmd.val >> 9) & 1, xfb = cmd.val & 0x4000;
            const int cw = (int)g_state.efbW, ch = (int)g_state.efbH;
            const bool nat = !xfb && s_scale != 1.0f &&
                             nwii::runtime::hle::TextureCache::get().efb_copy_stitched(
                                 g_state.efbCopyDest);
            const int gw = nat ? cw : SE(g_state.efbSrcX + cw) - SE(g_state.efbSrcX),
                      gh = nat ? ch : SE(g_state.efbSrcY + ch) - SE(g_state.efbSrcY);
            g_res_gpu.Copy(xfb ? "XFB" : (cmd.val & 0x800) ? "TEX+clear" : "TEX", cw, ch,
                           hf && !xfb ? gw / 2 : gw, hf && !xfb ? gh / 2 : gh, hf);
          }
          uint64_t ct52 = rph::T();
          // r39r2 NWII_R2_COPYMRT: queue a plain copy (its clear goes with it)
          const bool r2q = !(cmd.val & 0x4000) && !cmd.stale_copy && R2QueueCopy(cmd.val);
          if (!r2q) R2FlushCopies();
          if (r2q) {
            rph::C(rph::cDraw, ct52);
          } else if (cmd.val & 0x4000) {
            if (cmd.stale_copy)
              ++s_xfb_copies;  // keeps the dump numbering in guest frames
            else {
              CopyEfbToXfb();
              rph::C(rph::cXfb, ct52);
              if (rph::On()) ++rph::s_nxfb;
            }
          } else if (!cmd.stale_copy) {
            // EFB copy WITHOUT bit14 = copy to a texture in guest RAM, kept
            // as a GL texture keyed by the destination address
            // (render-to-texture: light maps, animated tiles, previews).
            CopyEfbToTexture(cmd.val);
          }
          // r45p NWII_P_STALELOG=1: copies to a texture of at least 128x128 (made, queued or stale)
          if (__builtin_expect(s_p_stalelog, 0) && !(cmd.val & 0x4000) && !s_r8_pass) {
            if (g_state.efbW * g_state.efbH >= 128u * 128u)
              printf("[P45S] copia %s dest=%08X %ux%u src=%u,%u xfb=%ld\n",
                     cmd.stale_copy ? "STALE (fotograma descartado: solo el borrado)" : (r2q ? "encolada" : "hecha"),
                     g_state.efbCopyDest, g_state.efbW, g_state.efbH, g_state.efbSrcX, g_state.efbSrcY, s_xfb_copies);
          }
          if ((cmd.val & 0x800) && !r2q) {
            ct52 = rph::T();
            ClearEfbCopySource();
            rph::C(rph::cClear, ct52);
            if (rph::On()) ++rph::s_nclear;
          }
          rph::s_after = rph::On();
          if (DrawTraceOn()) PixWatch("copy");
          if (ResGpuProf::On()) R2EfbBack();  // r39r2 (debug profiler)
          if (ResGpuProf::On() && !cmd.stale_copy) g_res_gpu.AfterCopy();
          // Copies and clears changed GL state behind the cache.
          // r39r2 NWII_R2_COPYST: they kept the cache up to date instead.
          if (!R2CopySt() || s_r2_full_inval || ResGpuProf::On() || DrawTraceOn() ||
              NWII_ENV("NWII_COPYDUMP"))
            GLInvalidate();
          s_r2_full_inval = false;
          s_state_dirty = true;
        }
      } else if (cmd.type == GXCommandType::DrawPrimitive) {
        if (draws) {
          // The list must be this frame's draws, in order; else stop using it.
          if (draw_ord < draws->size() && (*draws)[draw_ord] == &cmd) {
            if (ents) {
              const void *e = (*ents)[draw_ord];
              const uintptr_t t = reinterpret_cast<uintptr_t>(e);
              if (t & 1) {
                // A packed draw (tagged offset into g_render_pack).
                ecache::s_hint = nullptr;
                const size_t off = t >> 1;
                if (g_render_pack && off + xf::kPackHdrWords <= g_render_pack->size()) {
                  xf::s_pack_hint = reinterpret_cast<const xf::PackHdr *>(g_render_pack->data() + off);
                  xf::s_pack_words = g_render_pack->data() + off + xf::kPackHdrWords;
                }
              } else {
                ecache::s_hint = const_cast<ecache::Entry *>(static_cast<const ecache::Entry *>(e));
              }
              ecache::s_hint_ok = true;
              PrefetchDrawAheadXF(*draws, *ents, draw_ord++);
            } else {
              PrefetchDrawAhead(*draws, draw_ord++);
            }
          } else {
            draws = nullptr;
            ents = nullptr;
          }
        }
        // r36r NWII_R_CULL (r9): a draw whose state is already set up (clean
        // state, open batch) and that the cull test rejects skips DrawPrimitive
        // altogether (it would only count it and return from xf::Emit).
        // NWII_R_CULL=2 (r10): also when the state is not set up yet: the
        // rejected draw does not set it up either (no texture lookup, no batch
        // key, no GL state): the next drawn draw does, and joins the open batch
        // when its state equals the batch's (same GL draws, fewer of them).
        if (s_early_cull && !xf::s_p_cull_off && ecache::s_hint_ok && ecache::s_hint &&
            (s_cull_state || (s_batch_open && !s_state_dirty && !s_skip_draw)) && xf::Active() &&
            !DrawTraceOn() && !DrawStatsOn() && (cmd.prim_type & 0xF8) < 0xA8) {
          const ecache::Entry *ce = ecache::s_hint;
          if (ce->x_init && ce->x_bb_ok && ce->xi.ok && ce->xi.hp && ce->xi.npos == 1 && ce->x_n &&
              (ce->xi.pos[0] & 0x3F) * 4 + 12 <= 256) {
            if (xf::PCullTest(ce->x_bb, ce->xi.pos[0] & 0x3F)) {
              ++g_prof_gx_draws;
              ++s_cap_cur.gx_draws;
              if (rph::On()) { ++rph::s_draws; rph::s_verts += ce->x_n; }
              ++xf::st_culled;
              ecache::s_hint_ok = false;
              xf::s_pack_hint = nullptr;
              continue;
            }
            xf::s_cull_done = true;  // r12: kept; xf::Emit does not test it again
          }
        }
        DrawPrimitive(cmd);
        xf::s_cull_done = false;
        ecache::s_hint_ok = false;
        xf::s_pack_hint = nullptr;
      }
    }
    }  // r36r: pass (prefix, frame)
    rph::Set(rph::kFrame);
    R2FlushCopies();  // r39r2 NWII_R2_COPYMRT
    if (PRunStatOn()) PRunStatFrame();  // r45p
    R2EfbBack();  // r39r2 NWII_R2_LAZYEFB
    if (s_batch_open) {
      FlushBatch();
      s_batch_open = false;
    }
    for (int i = 0; i < 8; ++i)
      glBindSampler(i, 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, s_efb_w, s_efb_h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glDisable(GL_COLOR_LOGIC_OP);
    CDepthRange(0.0, 1.0);
    if (DrawStatsOn())
      DrawStatsFrame();
    PixWatchEnd();
    if (GpuTimer::On()) g_gpu_timer.End();
    if (ResGpuProf::On()) g_res_gpu.EndFrame();
    DumpPresentedXfb();
    nwii::runtime::hle::TextureCache::get().end_frame();
    ecache::EndFrame();
    if (xf::Active()) xf::EndFrame();
    else if (vbuf::s_on) vbuf::EndFrame();
    s_cap_cur.xfb = s_xfb_copies;
    s_cap_cur.uploads = (uint32_t)(g_prof_tex_uploads - cap_up0);
    s_cap_cur.new_shaders = (uint32_t)(g_prof_shader_compiles - cap_sh0);
    s_cap_cur.render_ms = std::chrono::duration<float, std::milli>(
                              std::chrono::steady_clock::now() - cap_t0).count();
    s_cap_last = s_cap_cur;
    rph::Set(-1);
    rph::Report();
  }

  void Present() override {}

private:
  void *m_window = nullptr;
};

std::unique_ptr<IRenderer> CreateRendererGL() {
  return std::make_unique<RendererGL>();
}

} // namespace nwii::runtime::gx

uint64_t nwii_stat_hash0() { return nwii::runtime::gx::g_stat_hash0; }
uint64_t nwii_stat_shaders() { return nwii::runtime::gx::g_stat_shaders; }

// R8 memory: GL programs, state keys and shader-cache bytes held by the
// renderer, plus its batch buffers (host thread).
void GX_RendererMemStats(size_t &programs, size_t &state_keys, size_t &cache_bytes,
                         size_t &batch_bytes) {
  namespace g = nwii::runtime::gx;
  programs = g::s_programs.size();
  state_keys = g::s_shader_cache.size();
  cache_bytes = 0;
  for (const auto &kv : g::shcache::s_entries)
    cache_bytes += sizeof(kv) + 32 + kv.second.bin.capacity() + kv.second.fs.capacity();
  batch_bytes = g::s_batch.capacity() * sizeof(g::BatchVertex) +
                g::s_batch_idx.capacity() * sizeof(uint32_t);
}

namespace nwii::runtime::gx {}

// The EFB rectangle the game last copied to the XFB, in GL framebuffer
// coordinates (x0, y0, x1, y1), for the frontend to present. Falls back to
// the whole EFB before the first copy.
void GX_GetPresentRect(int *x0, int *y0, int *x1, int *y1) {
  using nwii::runtime::gx::g_state;
  int w = g_state.xfbW ? g_state.xfbW : 640;
  int h = g_state.xfbH ? g_state.xfbH : 480;
  int sx = g_state.xfbW ? g_state.xfbSrcX : 0;
  int sy = g_state.xfbW ? g_state.xfbSrcY : 0;
  using nwii::runtime::gx::SE;
  *x0 = SE(sx);
  *x1 = SE(sx + w);
  *y1 = nwii::runtime::gx::s_efb_h - SE(sy);
  *y0 = nwii::runtime::gx::s_efb_h - SE(sy + h);
}

// ---- Internal resolution: EFB storage -------------------------------------
// The frontend owns the EFB framebuffer object; its colour texture and depth
// renderbuffer are (re)allocated here at 640s x 528s. Called once at start
// (creates everything) and between frames when the scale changes (window
// resize, Switch dock/undock): the old contents are scaled into the new
// storage so the frame on screen does not flash.
static GLuint s_efb_depth = 0;
static void AllocEfbStorage(GLuint tex, GLuint rbo, int w, int h) {
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindRenderbuffer(GL_RENDERBUFFER, rbo);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
}

void GX_CreateEfb(float scale, unsigned *fbo, unsigned *tex) {
  using namespace nwii::runtime::gx;
  scale = std::min(4.0f, std::max(1.0f, scale));
  s_viewport_f = (PFNViewportIndexedf)GX_GLProc("glViewportIndexedf");
  if (NWII_ENV("NWII_NO_FLOAT_VIEWPORT")) s_viewport_f = nullptr;
  SetScale(scale);
  GLuint f = 0, t = 0;
  glGenFramebuffers(1, &f);
  glBindFramebuffer(GL_FRAMEBUFFER, f);
  glGenTextures(1, &t);
  glGenRenderbuffers(1, &s_efb_depth);
  AllocEfbStorage(t, s_efb_depth, s_efb_w, s_efb_h);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                            s_efb_depth);
  s_efb_fbo = f;
  s_efb_tex = t;
  nwii::runtime::hle::TextureCache::get().set_restore_read_fbo(f);
  *fbo = f;
  *tex = t;
}

float GX_GetEfbScale() { return nwii::runtime::gx::s_scale; }
void GX_GetEfbSize(int *w, int *h) {
  *w = nwii::runtime::gx::s_efb_w;
  *h = nwii::runtime::gx::s_efb_h;
}
// Guest height of the presented XFB (0 before the first copy): "720p" and
// auto scales are relative to it.
int GX_GetXfbLines() { return nwii::runtime::gx::s_xfb_lh; }

// New storage at `scale` for the EFB made by GX_CreateEfb; the texture name
// changes (returned in *tex), the framebuffer object stays. Leaves the EFB
// bound as GL_FRAMEBUFFER.
bool GX_ResizeEfb(float scale, unsigned *tex) {
  using namespace nwii::runtime::gx;
  scale = std::min(4.0f, std::max(1.0f, scale));
  if (!s_efb_fbo || scale == s_scale) return false;
  const float old_scale = s_scale;
  const int ow = s_efb_w, oh = s_efb_h;
  SetScale(scale);
  const int nw = s_efb_w, nh = s_efb_h;
  GLuint t = 0, rbo = 0, tmp = 0;
  glGenTextures(1, &t);
  glGenRenderbuffers(1, &rbo);
  AllocEfbStorage(t, rbo, nw, nh);
  glGenFramebuffers(1, &tmp);
  glBindFramebuffer(GL_FRAMEBUFFER, tmp);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmp);
  glBlitFramebuffer(0, 0, ow, oh, 0, 0, nw, nh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glBlitFramebuffer(0, 0, ow, oh, 0, 0, nw, nh, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo);
  glDeleteFramebuffers(1, &tmp);
  glDeleteTextures(1, &s_efb_tex);
  glDeleteRenderbuffers(1, &s_efb_depth);
  s_efb_tex = t;
  s_efb_depth = rbo;
  printf("[GX] internal resolution %gx -> %gx (EFB %dx%d%s)\n", (double)old_scale,
         (double)scale, nw, nh, !s_iscale && s_viewport_f ? ", float viewport" : "");
  std::fflush(stdout);
  GLInvalidate();  // the viewport cache may hold either kind of viewport
  *tex = t;
  return true;
}

// Present blit of the frontend: XFB (or EFB) rectangle -> window rectangle,
// bilinear. When a scaled frame is more than twice the window size, it is
// first halved (exact 2x2 box with bilinear) until it is not, so 3x/4x on a
// small window is supersampled instead of skipping texels. Native scale (or
// a source at most 2x the window) is the single blit it always was. Leaves
// `src` bound for reading and the window (0) for drawing.
void GX_PresentBlit(unsigned src, int sx0, int sy0, int sx1, int sy1, int dx0,
                    int dy0, int dx1, int dy1) {
  using namespace nwii::runtime::gx;
  int sw = sx1 - sx0, sh = sy1 - sy0;
  const int dw = std::max(1, dx1 - dx0), dh = std::max(1, dy1 - dy0);
  if (s_scale > 1.0f && (sw > 2 * dw || sh > 2 * dh)) {
    static GLuint fbo[2] = {0, 0}, tex[2] = {0, 0};
    static int tw[2] = {0, 0}, th[2] = {0, 0};
    if (!fbo[0]) {
      glGenFramebuffers(2, fbo);
      glGenTextures(2, tex);
    }
    GLuint rd = src;
    int rx = sx0, ry = sy0;
    for (int k = 0; sw > 2 * dw || sh > 2 * dh; k ^= 1) {
      const int nw = sw > 2 * dw ? (sw + 1) / 2 : sw, nh = sh > 2 * dh ? (sh + 1) / 2 : sh;
      if (tw[k] < nw || th[k] < nh) {
        tw[k] = std::max(tw[k], nw);
        th[k] = std::max(th[k], nh);
        glActiveTexture(GL_TEXTURE0 + nwii::runtime::hle::TextureCache::kScratchUnit);
        glBindTexture(GL_TEXTURE_2D, tex[k]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw[k], th[k], 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[k]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               tex[k], 0);
      }
      glBindFramebuffer(GL_READ_FRAMEBUFFER, rd);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[k]);
      glBlitFramebuffer(rx, ry, rx + sw, ry + sh, 0, 0, nw, nh, GL_COLOR_BUFFER_BIT,
                        GL_LINEAR);
      rd = fbo[k];
      rx = ry = 0;
      sw = nw;
      sh = nh;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rd);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, sw, sh, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    return;
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT,
                    GL_LINEAR);
}

// The last EFB->XFB copy: its framebuffer and size. False before the first.
bool GX_GetXfbFramebuffer(unsigned *fbo, int *w, int *h) {
  using namespace nwii::runtime::gx;
  if (!s_xfb_fbo || !s_xfb_w)
    return false;
  *fbo = s_xfb_fbo;
  *w = s_xfb_w;
  *h = s_xfb_h;
  return true;
}

// ---- Capture tool ------------------------------------------------------------
// F12 (frontend): save the last NWII_CAPTURE_FRAMES presented frames (default
// 120, about 2 s) as PNG plus a per-frame state log; F11: one full-size
// screenshot. Frames are kept on the GPU in a ring (one 2D array texture, one
// scaled blit per presented frame, no read-back) at 1/NWII_CAPTURE_SCALE size
// (default 2: 320x228). Files go to NWII_CAPTURE_DIR, default "capturas" next
// to the NAND directory (~/nsmbw-port/capturas), in a folder per capture
// named by date and time. NWII_CAPTURE_FRAMES=0 turns the ring off (F11
// still works). Writing happens on a background thread.
#include <ctime>
#include <memory>
#include <thread>
#include <sys/stat.h>
namespace {
using nwii::runtime::gx::CapStats;
using nwii::runtime::gx::CapCopy;

struct CapSlot {
  CapStats st;
  double t;            // seconds since the first recorded frame
  uint32_t unshown;    // renders since the previous present that were never shown
  int w, h;            // stored size
};
struct CapRing {
  bool init = false, on = false;
  int n = 0, scale = 2, w = 0, h = 0;
  GLuint tex = 0, fbo = 0;
  std::vector<CapSlot> slots;
  long head = 0;       // frames recorded so far
  std::chrono::steady_clock::time_point t0;
};
CapRing g_cap;

uint32_t Crc32(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  static uint32_t tab[256];
  static bool made = false;
  if (!made) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      tab[i] = v;
    }
    made = true;
  }
  for (size_t i = 0; i < n; ++i) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}

// RGB rows (top first) -> PNG with stored (uncompressed) deflate blocks: no
// zlib needed, any viewer opens it.
bool WritePng(const std::string &path, const uint8_t *rgb, int w, int h) {
  std::vector<uint8_t> raw;
  raw.reserve((size_t)h * (w * 3 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  for (size_t off = 0; off < raw.size() || off == 0;) {
    const size_t len = std::min<size_t>(65535, raw.size() - off);
    const bool last = off + len >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(len & 0xFF); z.push_back(len >> 8);
    z.push_back(~len & 0xFF); z.push_back((~len >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + len);
    off += len;
    if (last) break;
  }
  const uint32_t ad = (b << 16) | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back((ad >> s) & 0xFF);
  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  auto chunk = [&](const char *type, const uint8_t *d, size_t n) {
    uint8_t hdr[8] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n,
                      (uint8_t)type[0], (uint8_t)type[1], (uint8_t)type[2], (uint8_t)type[3]};
    std::fwrite(hdr, 1, 8, f);
    if (n) std::fwrite(d, 1, n, f);
    uint32_t c = Crc32(hdr + 4, 4);
    c = Crc32(d, n, c) ^ 0xFFFFFFFFu;
    uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    std::fwrite(cb, 1, 4, f);
  };
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::fwrite(sig, 1, 8, f);
  const uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                            (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                            8, 2, 0, 0, 0};
  chunk("IHDR", ihdr, 13);
  chunk("IDAT", z.data(), z.size());
  chunk("IEND", nullptr, 0);
  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok;
}

std::string CaptureDir() {
  std::string base;
  if (const char *e = NWII_ENV("NWII_CAPTURE_DIR")) {
    base = e;
  } else {
    std::string nand = nwii::runtime::Config::get().nand_dir;
    while (nand.size() > 1 && nand.back() == '/') nand.pop_back();
    const size_t sl = nand.find_last_of('/');
    base = (sl == std::string::npos ? std::string(".") : nand.substr(0, sl)) + "/capturas";
  }
  ::mkdir(base.c_str(), 0755);
  char stamp[64];
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);
  std::string dir = base + "/" + stamp;
  for (int k = 2; ::mkdir(dir.c_str(), 0755) != 0 && k < 100; ++k)
    dir = base + "/" + stamp + "_" + std::to_string(k);
  return dir;
}

void FormatStats(std::string &o, const CapSlot &s) {
  char b[512];
  const CapStats &st = s.st;
  std::snprintf(b, sizeof b,
                "xfb=%ld t=%.3fs render_ms=%.2f gx_draws=%u gl_draws=%u efb_copies=%u "
                "tex_uploads=%u tex_bind_fixes=%u new_shaders=%u programs=%u "
                "stale_frames=%u unshown=%u",
                st.xfb, s.t, st.render_ms, st.gx_draws, st.gl_draws, st.ncopies,
                st.uploads, st.bind_saves, st.new_shaders, st.nprog, st.stale_frames,
                s.unshown);
  o += b;
  o += "\n    copies:";
  for (uint32_t i = 0; i < std::min<uint32_t>(st.ncopies, 24); ++i) {
    const CapCopy &c = st.copies[i];
    std::snprintf(b, sizeof b, " %s%08X/%ux%u/f%u%s%s%s", (c.flags & 1) ? "XFB@" : "",
                  c.dest, c.w, c.h, c.fmt, (c.flags & 2) ? "/half" : "",
                  (c.flags & 8) ? "/clr" : "", (c.flags & 4) ? "/STALE" : "");
    o += b;
  }
  if (st.ncopies > 24) o += " ...";
  o += "\n    programs:";
  for (uint32_t i = 0; i < st.nprog; ++i) {
    std::snprintf(b, sizeof b, " %u", st.prog[i]);
    o += b;
  }
  o += "\n";
}

std::string GlInfo() {
  std::string s;
  for (GLenum e : {GL_VENDOR, GL_RENDERER, GL_VERSION})
    if (const GLubyte *v = glGetString(e)) s += (const char *)v, s += " | ";
  return s;
}

void CapInit() {
  g_cap.init = true;
#ifdef __SWITCH__
  long n = 0;  // no keyboard; opt in with NWII_CAPTURE_FRAMES
#else
  long n = 120;
#endif
  if (const char *e = NWII_ENV("NWII_CAPTURE_FRAMES")) n = std::atol(e);
  if (const char *e = NWII_ENV("NWII_CAPTURE_SCALE")) g_cap.scale = std::max(1, std::atoi(e));
  g_cap.n = (int)std::min<long>(std::max<long>(n, 0), 600);
  g_cap.on = g_cap.n > 0;
  g_cap.t0 = std::chrono::steady_clock::now();
}
} // namespace

namespace nwii::runtime::gx {
// Called by the frontend after presenting: record the frame it just showed.
// `fbo`/rect: the presented source (the XFB framebuffer, GL orientation).
void CaptureRecord(unsigned fbo, int x0, int y0, int x1, int y1, uint32_t unshown) {
  if (!g_cap.init) CapInit();
  if (!g_cap.on || x1 <= x0 || y1 <= y0) return;
  // Ring frames stay at native/NWII_CAPTURE_SCALE size at any internal
  // resolution (the scaled source is just blitted down further).
  const float div = g_cap.scale * nwii::runtime::gx::s_scale;
  const int w = std::max(1, (int)std::lround((x1 - x0) / div)),
            h = std::max(1, (int)std::lround((y1 - y0) / div));
  if (!g_cap.tex || w != g_cap.w || h != g_cap.h) {
    if (!g_cap.tex) {
      glGenTextures(1, &g_cap.tex);
      glGenFramebuffers(1, &g_cap.fbo);
    }
    glActiveTexture(GL_TEXTURE0 + nwii::runtime::hle::TextureCache::kScratchUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_cap.tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, w, h, g_cap.n, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(GL_TEXTURE0);
    g_cap.w = w;
    g_cap.h = h;
    g_cap.slots.assign(g_cap.n, CapSlot{});
    g_cap.head = 0;
  }
  const int layer = (int)(g_cap.head % g_cap.n);
  // No glGet (it would stall Mesa's GL thread every frame): the frontend calls
  // this right after its present blit, with `fbo` as read and the window as
  // draw framebuffer, and gets exactly that back.
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_cap.fbo);
  glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, g_cap.tex, 0, layer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glBlitFramebuffer(x0, y0, x1, y1, 0, 0, w, h, GL_COLOR_BUFFER_BIT,
                    div > 1.0f ? GL_LINEAR : GL_NEAREST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  CapSlot &s = g_cap.slots[layer];
  s.st = s_cap_last;
  s.t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_cap.t0).count();
  s.unshown = unshown;
  s.w = w;
  s.h = h;
  ++g_cap.head;
}

// kind 1: the ring (F12); kind 2: one full-size screenshot of `fbo`/rect (F11).
// Returns the folder written to (files finish on a background thread).
std::string CaptureSave(int kind, unsigned fbo, int x0, int y0, int x1, int y1) {
  if (!g_cap.init) CapInit();
  struct Img { std::string name; int w, h; std::vector<uint8_t> rgb; };
  auto imgs = std::make_shared<std::vector<Img>>();
  auto log = std::make_shared<std::string>();
  const std::string dir = CaptureDir();
  char b[256];
  const std::time_t now = std::time(nullptr);
  std::snprintf(b, sizeof b, "NSMBW recomp capture (%s), %s", kind == 1 ? "F12 ring" : "F11 screenshot",
                std::ctime(&now));
  *log += b;
  *log += "GL: " + GlInfo() + "\n";
  auto rgba_to_rgb_flip = [](const std::vector<uint8_t> &px, int w, int h) {
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        std::memcpy(&rgb[((size_t)y * w + x) * 3], &px[((size_t)(h - 1 - y) * w + x) * 4], 3);
    return rgb;
  };
  GLint prev_read = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
  if (kind == 2) {
    int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0) {
      std::vector<uint8_t> px((size_t)w * h * 4);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
      glReadPixels(x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      // Native size unless NWII_DUMP_SCALED=1 (like NWII_XFBDUMP).
      if (const float sc = nwii::runtime::gx::s_scale; sc > 1.0f && !DumpScaled()) {
        const int nw = (int)std::lround(w / sc), nh = (int)std::lround(h / sc);
        BoxDownsample(px, w, h, nw, nh);
        w = nw;
        h = nh;
      }
      std::snprintf(b, sizeof b, "screenshot_xfb%06ld.png", s_cap_last.xfb);
      imgs->push_back({b, w, h, rgba_to_rgb_flip(px, w, h)});
      CapSlot s{};
      s.t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_cap.t0).count();
      s.st = s_cap_last;
      *log += "frame: ";
      FormatStats(*log, s);
    }
  } else if (g_cap.on && g_cap.head > 0) {
    const long n = std::min<long>(g_cap.head, g_cap.n);
    *log += "frames (oldest first; one line per presented frame, xfb = guest frame number):\n";
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_cap.fbo);
    for (long k = 0; k < n; ++k) {
      const long abs = g_cap.head - n + k;
      const int layer = (int)(abs % g_cap.n);
      const CapSlot &s = g_cap.slots[layer];
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, g_cap.tex, 0, layer);
      std::vector<uint8_t> px((size_t)s.w * s.h * 4);
      glReadPixels(0, 0, s.w, s.h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      std::snprintf(b, sizeof b, "f%03ld_xfb%06ld.png", k, s.st.xfb);
      imgs->push_back({b, s.w, s.h, rgba_to_rgb_flip(px, s.w, s.h)});
      std::snprintf(b, sizeof b, "%03ld ", k);
      *log += b;
      FormatStats(*log, s);
    }
  } else {
    *log += "ring is off (NWII_CAPTURE_FRAMES=0) or empty\n";
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
  *log += "programs: GL program id -> source hash\n";
  for (const auto &kv : s_programs) {
    std::snprintf(b, sizeof b, "  %u %016llx\n", kv.second.id, (unsigned long long)kv.first);
    *log += b;
  }
  std::thread([dir, imgs, log] {
    for (const Img &im : *imgs) WritePng(dir + "/" + im.name, im.rgb.data(), im.w, im.h);
    if (FILE *f = std::fopen((dir + "/log.txt").c_str(), "w")) {
      std::fwrite(log->data(), 1, log->size(), f);
      std::fclose(f);
    }
    std::printf("[CAPTURE] %zu image(s) + log.txt written to %s\n", imgs->size(), dir.c_str());
    std::fflush(stdout);
  }).detach();
  return dir;
}
} // namespace nwii::runtime::gx

// ---- r10: on-screen performance overlay ----------------------------------
// config.toml [switch] fps_overlay = true (or debug.toml, or NWII_FPS_OVERLAY=1
// on PC). Drawn on the default framebuffer right before every present, with
// scissored clears only (no shader, no geometry, no texture), so it costs the
// GPU a few clears and the CPU almost nothing:
//   - fps of the last ~0.5 s as 7-segment digits: green >= 58, yellow >= 45,
//     orange >= 28, red below;
//   - the time between the last 60 presents as bars (0-50 ms), with marks at
//     16.7 ms (60 fps) and 33.3 ms (30 fps);
//   - Switch: two bars with the share of a core the render thread (blue) and
//     the game CPU thread (orange) used over the last ~0.5 s.
#ifdef __SWITCH__
namespace nwii::runtime::nx { void overlay_thread_load(float *render, float *game); }
#endif
namespace nwii::runtime::gx {
void DrawPerfOverlay() {
  static const bool on = NWII_ENV("NWII_FPS_OVERLAY") != nullptr;
  if (!on) return;
  using clk = std::chrono::steady_clock;
  constexpr int kHist = 60;
  static float hist[kHist] = {};
  static int hpos = 0, hcount = 0, win_frames = 0, fps_shown = 0;
  static clk::time_point last{}, win_start{};
  static float load_r = -1.f, load_g = -1.f;
  const clk::time_point now = clk::now();
  if (last != clk::time_point{}) {
    hist[hpos] = std::chrono::duration<float, std::milli>(now - last).count();
    hpos = (hpos + 1) % kHist;
    hcount = std::min(hcount + 1, kHist);
  } else {
    win_start = now;
  }
  last = now;
  ++win_frames;
  const float win_s = std::chrono::duration<float>(now - win_start).count();
  if (win_s >= 0.5f) {
    fps_shown = (int)std::lround(win_frames / win_s);
    win_frames = 0;
    win_start = now;
#ifdef __SWITCH__
    nwii::runtime::nx::overlay_thread_load(&load_r, &load_g);
#endif
  }

  SDL_Window *win = GX_HostWindow();
  if (!win) return;
  int w = 0, h = 0;
  GX_HostDrawable(win, &w, &h);
  if (w <= 0 || h <= 0) return;
  GLint fbo = 0, sc[4];
  GLfloat cc[4];
  GLboolean cm[4];
  const GLboolean sc_on = glIsEnabled(GL_SCISSOR_TEST);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
  glGetIntegerv(GL_SCISSOR_BOX, sc);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
  glGetBooleanv(GL_COLOR_WRITEMASK, cm);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glEnable(GL_SCISSOR_TEST);
  struct C { float r, g, b; };
  auto rect = [](int x, int y, int rw, int rh, C c) {
    if (rw <= 0 || rh <= 0) return;
    glScissor(x, y, rw, rh);
    glClearColor(c.r, c.g, c.b, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
  };
  const C kGreen{0.20f, 0.85f, 0.30f}, kYellow{0.95f, 0.85f, 0.20f}, kOrange{0.95f, 0.55f, 0.15f},
      kRed{0.95f, 0.20f, 0.18f}, kPanel{0.04f, 0.04f, 0.05f}, kTrack{0.18f, 0.18f, 0.20f},
      kMark{0.55f, 0.55f, 0.58f}, kBlue{0.30f, 0.55f, 1.00f}, kGame{1.00f, 0.60f, 0.20f};
  auto fps_color = [&](int f) { return f >= 58 ? kGreen : f >= 45 ? kYellow : f >= 28 ? kOrange : kRed; };
  auto ms_color = [&](float ms) { return ms <= 17.5f ? kGreen : ms <= 25.f ? kYellow : ms <= 34.f ? kOrange : kRed; };

  const int u = std::max(2, h / 240);  // 3 px at 720p, 4 at 1080p
  const int graph_w = kHist * u, digits_h = 10 * u, graph_h = 16 * u;
  const bool loads = load_r >= 0.f && load_g >= 0.f;
  const int panel_w = graph_w + 4 * u;
  const int panel_h = 2 * u + digits_h + 2 * u + graph_h + 2 * u + (loads ? 7 * u : 0);
  const int x0 = 2 * u, top = h - 2 * u;
  rect(x0, top - panel_h, panel_w, panel_h, kPanel);

  // fps digits (7 segments), up to 3.
  static const uint8_t kSeg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  const int dw = 6 * u, dh = digits_h, t = u;
  const int dy = top - 2 * u - dh;
  const int f = std::min(fps_shown, 999);
  const std::string txt = std::to_string(f);
  const C fc = fps_color(f);
  int dx = x0 + 2 * u;
  for (char ch : txt) {
    const uint8_t m = kSeg[ch - '0'];
    const int x = dx, y = dy, half = dh / 2;
    if (m & 0x01) rect(x + t, y + dh - t, dw - 2 * t, t, fc);           // a
    if (m & 0x02) rect(x + dw - t, y + half, t, half - t, fc);          // b
    if (m & 0x04) rect(x + dw - t, y + t, t, half - t, fc);             // c
    if (m & 0x08) rect(x + t, y, dw - 2 * t, t, fc);                    // d
    if (m & 0x10) rect(x, y + t, t, half - t, fc);                      // e
    if (m & 0x20) rect(x, y + half, t, half - t, fc);                   // f
    if (m & 0x40) rect(x + t, y + half - t / 2, dw - 2 * t, t, fc);     // g
    dx += dw + 2 * u;
  }

  // Frame-time bars, oldest on the left; 0-50 ms over graph_h.
  const int gx = x0 + 2 * u, gy = dy - 2 * u - graph_h;
  rect(gx, gy, graph_w, graph_h, kTrack);
  auto ms_y = [&](float ms) { return (int)(std::min(ms, 50.f) / 50.f * graph_h); };
  for (int i = 0; i < hcount; ++i) {
    const float ms = hist[(hpos - hcount + i + kHist) % kHist];
    rect(gx + (kHist - hcount + i) * u, gy, std::max(1, u - 1), std::max(1, ms_y(ms)), ms_color(ms));
  }
  rect(gx, gy + ms_y(16.7f), graph_w, 1, kMark);
  rect(gx, gy + ms_y(33.3f), graph_w, 1, kMark);

  // Thread load bars (Switch).
  if (loads) {
    const int by = gy - 2 * u - 2 * u;
    rect(gx, by, graph_w, 2 * u, kTrack);
    rect(gx, by, (int)(graph_w * std::min(load_r, 1.f)), 2 * u, kBlue);
    rect(gx, by - 3 * u, graph_w, 2 * u, kTrack);
    rect(gx, by - 3 * u, (int)(graph_w * std::min(load_g, 1.f)), 2 * u, kGame);
  }

  glScissor(sc[0], sc[1], sc[2], sc[3]);
  if (!sc_on) glDisable(GL_SCISSOR_TEST);
  glClearColor(cc[0], cc[1], cc[2], cc[3]);
  glColorMask(cm[0], cm[1], cm[2], cm[3]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)fbo);
}
} // namespace nwii::runtime::gx
