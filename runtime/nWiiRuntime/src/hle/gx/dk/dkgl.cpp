// r15dk: dkgl, el subconjunto de GL del runtime sobre deko3d (ver dkgl.h y r9/r15dk/DISENO_DEKO3D.md §3).
//
// Modelo: las funciones gl* solo cambian el estado que guarda dkgl (como un driver). En cada draw/clear se
// emite a un DkCmdBuf lo que cambió. Un "frame" de dkgl va de SwapBuffers a SwapBuffers: su número (seq) marca
// la memoria (anillo de comandos, anillo de datos, borrados diferidos) que no se puede reutilizar hasta que su
// valla (DkFence) se haya cumplido. Convenciones de GL en el dispositivo (origen abajo a la izquierda, z en
// [-1,1]): medido en la consola con dk3test, la fila 0 de una imagen en memoria es la de ABAJO, igual que en
// GL, así que rectángulos de viewport, scissor, blits, copias y lecturas pasan tal cual.
#ifdef __SWITCH__
#include "dkgl.h"
#include "dk_glsl.h"
#include "uam_api.h"

#include <switch.h>
#include <deko3d.h>
#include <glad/glad.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_PROGRAM_SEPARABLE
#define GL_PROGRAM_SEPARABLE 0x8258
#endif
#ifndef GL_VERTEX_SHADER_BIT
#define GL_VERTEX_SHADER_BIT 0x00000001
#define GL_FRAGMENT_SHADER_BIT 0x00000002
#endif
#ifndef GL_NUM_PROGRAM_BINARY_FORMATS
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif

namespace dkgl {

static bool s_active = false;

static void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::printf("[DK] ");
  std::vprintf(fmt, ap);
  std::printf("\n");
  va_end(ap);
  std::fflush(stdout);
}

static void DebugCb(void*, const char* ctx, DkResult res, const char* msg) {
  Log("deko3d: %s: resultado %d: %s", ctx ? ctx : "?", (int)res, msg ? msg : "");
}

// ============================================================ memoria
struct Alloc {
  DkMemBlock blk = nullptr;
  uint32_t off = 0, size = 0;
  uint8_t* cpu = nullptr;
  DkGpuAddr gpu = 0;
  int heap = -1, bi = -1;
  explicit operator bool() const { return blk != nullptr; }
};

static DkDevice s_dev;
static DkQueue s_q;
static DkCmdBuf s_cmd;

// Bloques grandes subdivididos (primer hueco, con fusión al liberar).
struct Heap {
  uint32_t flags = 0, block = 0;
  int id = 0;
  struct Blk {
    DkMemBlock mem;
    uint32_t size;
    std::map<uint32_t, uint32_t> free;  // offset -> tamaño
  };
  std::vector<Blk> blks;
  Alloc Allocate(uint32_t size, uint32_t align) {
    size = (size + 255) & ~255u;
    if (align < 256) align = 256;
    for (int pass = 0; pass < 2; ++pass) {
      for (size_t b = 0; b < blks.size(); ++b) {
        auto& fl = blks[b].free;
        for (auto it = fl.begin(); it != fl.end(); ++it) {
          const uint32_t start = (it->first + align - 1) & ~(align - 1);
          const uint32_t end = it->first + it->second;
          if (start + size > end) continue;
          const uint32_t o = it->first, l = it->second;
          fl.erase(it);
          if (start > o) fl[o] = start - o;
          if (start + size < o + l) fl[start + size] = o + l - (start + size);
          Alloc a;
          a.blk = blks[b].mem;
          a.off = start;
          a.size = size;
          a.cpu = (flags & DkMemBlockFlags_CpuAccessMask) ? (uint8_t*)dkMemBlockGetCpuAddr(a.blk) + start : nullptr;
          a.gpu = dkMemBlockGetGpuAddr(a.blk) + start;
          a.heap = id;
          a.bi = (int)b;
          return a;
        }
      }
      // otro bloque
      const uint32_t bs = std::max(block, (size + align + 0xFFFFu) & ~0xFFFFu);
      DkMemBlockMaker m;
      dkMemBlockMakerDefaults(&m, s_dev, bs);
      m.flags = flags;
      Blk nb;
      nb.mem = dkMemBlockCreate(&m);
      nb.size = bs;
      if (!nb.mem) { Log("sin memoria (bloque de %u KB)", bs >> 10); return Alloc(); }
      nb.free[0] = (flags & DkMemBlockFlags_Code) ? bs - DK_SHADER_CODE_UNUSABLE_SIZE : bs;
      blks.push_back(std::move(nb));
    }
    return Alloc();
  }
  void Free(const Alloc& a) {
    if (!a || a.bi < 0) return;
    auto& fl = blks[a.bi].free;
    uint32_t o = a.off, l = a.size;
    auto next = fl.lower_bound(o);
    if (next != fl.end() && o + l == next->first) { l += next->second; next = fl.erase(next); }
    if (next != fl.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == o) { o = prev->first; l += prev->second; fl.erase(prev); }
    }
    fl[o] = l;
  }
};
enum { kHeapData, kHeapImage, kHeapCode, kNumHeaps };
static Heap s_heaps[kNumHeaps];

// ============================================================ frames, vallas, borrados diferidos
static uint64_t s_seq = 1;        // frame que se está grabando
static uint64_t s_done = 0;       // último frame terminado por la GPU
struct Inflight { uint64_t seq; DkFence* fence; };
static std::deque<Inflight> s_inflight;
struct Deferred { uint64_t seq; Alloc a; int desc = -1; };
static std::deque<Deferred> s_deferred;
static void FreeDesc(int d);

static void PollFences(bool wait_oldest) {
  while (!s_inflight.empty()) {
    auto& f = s_inflight.front();
    const DkResult r = dkFenceWait(f.fence, wait_oldest ? -1 : 0);
    if (r != DkResult_Success) break;
    s_done = std::max(s_done, f.seq);
    delete f.fence;
    s_inflight.pop_front();
    wait_oldest = false;
  }
  while (!s_deferred.empty() && s_deferred.front().seq <= s_done) {
    auto& d = s_deferred.front();
    if (d.a) s_heaps[d.a.heap].Free(d.a);
    if (d.desc >= 0) FreeDesc(d.desc);
    s_deferred.pop_front();
  }
}
static void WaitSeq(uint64_t seq) {
  while (s_done < seq && !s_inflight.empty()) PollFences(true);
}
static void FreeLater(const Alloc& a) { if (a) s_deferred.push_back({s_seq, a, -1}); }

// Memoria en trozos reutilizados por frame: comandos y datos de un solo uso (subidas, glBufferData pequeños).
struct ChunkRing {
  uint32_t flags = 0, chunk = 0;
  struct C { Alloc a; uint64_t seq = 0; };
  std::vector<C> chunks;
  size_t cur = (size_t)-1;
  uint32_t used = 0;
  bool Next() {
    for (size_t k = 1; k <= chunks.size(); ++k) {
      const size_t i = (cur + k) % chunks.size();
      if (chunks[i].seq == s_seq) continue;  // en uso por este frame
      if (chunks[i].seq > s_done) WaitSeq(chunks[i].seq);
      cur = i;
      chunks[i].seq = s_seq;
      used = 0;
      return true;
    }
    C c;
    c.a = s_heaps[kHeapData].Allocate(chunk, 256);
    if (!c.a) return false;
    c.seq = s_seq;
    chunks.push_back(c);
    cur = chunks.size() - 1;
    used = 0;
    return true;
  }
  Alloc Take(uint32_t size, uint32_t align) {
    if (cur == (size_t)-1 || chunks[cur].seq != s_seq) { if (!Next()) return Alloc(); }
    uint32_t o = (used + align - 1) & ~(align - 1);
    if (o + size > chunk) {
      if (!Next()) return Alloc();
      o = 0;
    }
    used = o + size;
    Alloc a = chunks[cur].a;
    a.off += o;
    a.cpu += o;
    a.gpu += o;
    a.size = size;
    a.bi = -1;  // no se libera suelto
    return a;
  }
};
static ChunkRing s_cmdring, s_dataring;
// estadísticas por ventana de 600 frames ([DK] frames ...)
static uint64_t s_st_draw_ticks = 0, s_st_push_bytes = 0, s_st_barriers = 0, s_st_rt = 0, s_st_submit_ticks = 0, s_st_acquire_ticks = 0;

static void CmdAddMem(void*, DkCmdBuf cmd, size_t need) {
  if (need > s_cmdring.chunk) Log("bloque de comandos de %zu B: demasiado grande", need);
  // el trozo actual puede tener comandos de varios frames: se marca con el último que escribió en él
  if (s_cmdring.cur != (size_t)-1) s_cmdring.chunks[s_cmdring.cur].seq = s_seq;
  s_cmdring.Next();
  Alloc a = s_cmdring.chunks[s_cmdring.cur].a;
  s_cmdring.used = s_cmdring.chunk;
  dkCmdBufAddMemory(cmd, a.blk, a.off, s_cmdring.chunk);
}

// Envía lo grabado. signal_frame: cierra el frame con su valla.
static uint64_t s_submitted_cmds = 0;
static void Submit(bool signal_frame) {
  // Las escrituras de la CPU (memoria sin caché de CPU: anillo y arena de GPU XF, índices, descriptores) tienen
  // que haber llegado a la RAM antes de que la GPU lea esta lista.
  __asm__ volatile("dsb sy" ::: "memory");
  if (signal_frame) {
    DkFence* f = new DkFence();
    dkCmdBufSignalFence(s_cmd, f, true);
    s_inflight.push_back({s_seq, f});
  }
  const uint64_t t0 = armGetSystemTick();
  DkCmdList l = dkCmdBufFinishList(s_cmd);
  dkQueueSubmitCommands(s_q, l);
  dkQueueFlush(s_q);
  s_st_submit_ticks += armGetSystemTick() - t0;
  ++s_submitted_cmds;
  // Al empezar la lista siguiente: esperar a que la GPU acabe lo anterior (WaitForIdle) e invalidar la caché de
  // texturas y la L2. La invalidación que deko3d añade tras cada flush no espera: los últimos draws de la lista
  // anterior podían volver a cargar en la L2 líneas de memoria que la CPU ya estaba reescribiendo para el frame
  // siguiente (huecos reutilizados de la arena de GPU XF junto a datos vivos, el anillo). Con eso la GPU leía
  // vértices viejos: "explosiones" de vértices en la demo submarina (consola 8-nov 19:41 y 22:27). Es lo que hace
  // Mesa/nvc0 con los buffers persistentes coherentes (invalida por draw).
  dkCmdBufBarrier(s_cmd, DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
  if (dkQueueIsInErrorState(s_q)) {
    static int n = 0;
    if (n++ < 4) Log("LA COLA DE deko3d ESTÁ EN ERROR (fallo de GPU)");
  }
}
static void Finish() {
  Submit(false);
  dkQueueWaitIdle(s_q);
}

// ============================================================ descriptores
constexpr int kMaxImages = 8192, kMaxSamplers = 512;
static Alloc s_imgdesc, s_smpdesc;
static std::vector<int> s_freedesc;
static int s_nextdesc = 1;  // 0 = textura negra de reserva
static int AllocDesc() {
  if (!s_freedesc.empty()) { int d = s_freedesc.back(); s_freedesc.pop_back(); return d; }
  if (s_nextdesc >= kMaxImages) { Log("sin descriptores de imagen"); return 0; }
  return s_nextdesc++;
}
static void FreeDesc(int d) { if (d > 0) s_freedesc.push_back(d); }
static void WriteImageDesc(int slot, const DkImageView* v) {
  DkImageDescriptor d;
  dkImageDescriptorInitialize(&d, v, false, false);
  std::memcpy(s_imgdesc.cpu + slot * sizeof(d), &d, sizeof(d));
}

struct SamplerKey {
  GLint minf, magf, ws, wt;
  bool operator<(const SamplerKey& o) const { return std::memcmp(this, &o, sizeof *this) < 0; }
};
static std::map<SamplerKey, int> s_samplers;
static DkWrapMode Wrap(GLint w) {
  return w == GL_REPEAT ? DkWrapMode_Repeat : w == GL_MIRRORED_REPEAT ? DkWrapMode_MirroredRepeat : DkWrapMode_ClampToEdge;
}
static int SamplerSlot(const SamplerKey& k) {
  auto it = s_samplers.find(k);
  if (it != s_samplers.end()) return it->second;
  const int slot = (int)s_samplers.size();
  if (slot >= kMaxSamplers) return 0;
  DkSampler s;
  dkSamplerDefaults(&s);
  s.magFilter = k.magf == GL_LINEAR ? DkFilter_Linear : DkFilter_Nearest;
  s.minFilter = (k.minf == GL_LINEAR || k.minf == GL_LINEAR_MIPMAP_NEAREST || k.minf == GL_LINEAR_MIPMAP_LINEAR)
                    ? DkFilter_Linear : DkFilter_Nearest;
  s.mipFilter = DkMipFilter_None;  // el runtime no usa mipmaps
  s.wrapMode[0] = Wrap(k.ws);
  s.wrapMode[1] = Wrap(k.wt);
  s.wrapMode[2] = DkWrapMode_ClampToEdge;
  DkSamplerDescriptor d;
  dkSamplerDescriptorInitialize(&d, &s);
  std::memcpy(s_smpdesc.cpu + slot * sizeof(d), &d, sizeof(d));
  s_samplers[k] = slot;
  return slot;
}

// ============================================================ objetos GL
template <class T>
struct Table {
  std::vector<std::unique_ptr<T>> v{1};  // 0 = ninguno
  std::vector<GLuint> freeids;
  GLuint Gen() {
    if (!freeids.empty()) { GLuint id = freeids.back(); freeids.pop_back(); v[id].reset(new T()); return id; }
    v.emplace_back(new T());
    return (GLuint)v.size() - 1;
  }
  T* Get(GLuint id) { return id && id < v.size() ? v[id].get() : nullptr; }
  void Del(GLuint id) { if (id && id < v.size() && v[id]) { v[id].reset(); freeids.push_back(id); } }
};

struct Buf {
  Alloc mem;
  uint32_t size = 0;
  bool heap = false;  // memoria propia (se libera diferida); si no, del anillo de datos
};
struct Tex {
  GLenum target = 0;
  uint32_t w = 0, h = 0;
  DkImageFormat fmt = DkImageFormat_None;
  Alloc mem;
  DkImageLayout layout;
  DkImage img;
  DkImageView view;
  int desc = -1;
  bool ok = false;
  GLint minf = GL_NEAREST_MIPMAP_LINEAR, magf = GL_LINEAR, ws = GL_REPEAT, wt = GL_REPEAT;
};
struct Rbo { Tex t; };
struct Fbo { GLuint color = 0, depth_rbo = 0, depth_tex = 0; };
struct Sampler { GLint minf = GL_NEAREST_MIPMAP_LINEAR, magf = GL_LINEAR, ws = GL_REPEAT, wt = GL_REPEAT; };
struct Attrib { bool en = false; GLint size = 4; GLenum type = GL_FLOAT; bool norm = false, integer = false; GLsizei stride = 0; uintptr_t off = 0; GLuint buf = 0; GLuint divisor = 0; };
struct Vao { Attrib a[16]; GLuint ebo = 0; };

struct Code {  // un DKSH cargado (compartido por todos los programas con el mismo texto convertido)
  Alloc mem;
  DkShader sh;
  dkglsl::Reflection refl;
  bool ok = false;
};
struct Shd {
  GLenum type = 0;
  std::string src;
  Code* code = nullptr;
  bool compiled = false;
  std::string log;
};
struct StageProg {
  Code* code = nullptr;
  std::vector<uint8_t> ubo;
  Alloc ubo_mem;
  uint32_t ubo_size = 0;  // alineado a 256
  uint32_t dlo = ~0u, dhi = 0;  // rango sucio
  uint8_t unit[32] = {};  // binding de sampler -> unidad de textura
};
struct Loc { std::string name; int u[2] = {-1, -1}; int smp[2] = {-1, -1}; };
struct Prog {
  std::vector<GLuint> shaders;
  bool separable = false, linked = false;
  std::string log;
  StageProg st[2];  // 0 VS, 1 FS
  std::vector<Loc> locs;
};
struct Pipe { GLuint vs = 0, fs = 0; };
struct Sync { DkFence fence; bool submitted = false; uint64_t seq = 0; };

static Table<Buf> s_bufs;
static Table<Tex> s_texs;
static Table<Rbo> s_rbos;
static Table<Fbo> s_fbos;
static Table<Sampler> s_smps;
static Table<Vao> s_vaos;
static Table<Shd> s_shds;
static Table<Prog> s_progs;  // compartido con los shaders en GL; aquí tablas separadas con ids altos
static Table<Pipe> s_pipes;

// ids de shaders y programas comparten espacio en GL: los programas van por encima de este
constexpr GLuint kProgBase = 0x100000;

// ============================================================ estado GL
struct State {
  GLuint array_buf = 0, pack_buf = 0, texbuf_bind = 0, vao = 0;
  GLuint draw_fbo = 0, read_fbo = 0, rbo = 0;
  GLuint prog = 0, pipe = 0;
  int active_unit = 0;
  GLuint tex2d[32] = {}, texbuf[32] = {}, smp[32] = {};
  bool blend = false, depth_test = false, cull = false, scissor = false, logic = false, depth_clamp = false,
       raster_discard = false;
  GLenum blend_eq_rgb = GL_FUNC_ADD, blend_eq_a = GL_FUNC_ADD;
  GLenum bsrc_rgb = GL_ONE, bdst_rgb = GL_ZERO, bsrc_a = GL_ONE, bdst_a = GL_ZERO;
  GLenum depth_func = GL_LESS;
  bool depth_mask = true;
  bool cmask[4] = {true, true, true, true};
  GLenum logic_op = GL_COPY;
  GLenum cull_face = GL_BACK, front_face = GL_CCW;
  GLint vp[4] = {0, 0, 0, 0};
  float vpf[4] = {0, 0, 0, 0};  // viewport en float (glViewportIndexedf, resolución fraccionaria del frente RES)
  double dr[2] = {0.0, 1.0};
  GLint sc[4] = {0, 0, 0, 0};
  float clear_color[4] = {0, 0, 0, 0};
  double clear_depth = 1.0;
  GLint pack_align = 4, unpack_align = 4;
  GLenum error = GL_NO_ERROR;
};
static State g;
static Vao s_default_vao;

// ============================================================ swapchain (framebuffer 0)
// tamaño del swapchain: 1920x1080 si se arranca en sobremesa, 1280x720 en portátil (NWII_DK_FB=720|1080 lo fuerza).
// Al acoplar/desacoplar después no cambia: el compositor escala la capa.
static int kFbW = 1280, kFbH = 720;
constexpr int kFbN = 3;
static Tex s_fb[kFbN];
static Tex s_fbdepth;
static DkSwapchain s_swap = nullptr;
static int s_slot = -1;
static int s_swap_interval = 1;
static Tex s_black;  // descriptor 0: 1x1 negro (texturas sin contenido)
static Alloc s_zero;  // 256 B a cero: "buffer" de los atributos desactivados

// ============================================================ caché DKSH
static std::unordered_map<uint64_t, Code*> s_code;
static std::unordered_map<uint64_t, std::vector<uint8_t>> s_disk;
static FILE* s_diskf = nullptr;
static uint64_t Fnv(const void* p, size_t n, uint64_t h = 14695981039346656037ull) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
static void DiskInit() {
  const char* sc = std::getenv("NWII_SHADERCACHE");
  std::string path = sc ? sc : "shadercache.bin";
  const size_t sl = path.find_last_of('/');
  path = (sl == std::string::npos ? std::string() : path.substr(0, sl + 1)) + "dkshcache.bin";
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char magic[4];
    size_t good = 0;
    if (std::fread(magic, 1, 4, f) == 4 && !std::memcmp(magic, "DKS1", 4)) {
      good = 4;
      for (;;) {
        uint64_t key;
        uint32_t len;
        if (std::fread(&key, 8, 1, f) != 1 || std::fread(&len, 4, 1, f) != 1 || len > (4u << 20)) break;
        std::vector<uint8_t> d(len);
        if (std::fread(d.data(), 1, len, f) != len) break;
        s_disk[key] = std::move(d);
        good += 12 + len;
      }
    }
    std::fseek(f, 0, SEEK_END);
    const long total = std::ftell(f);
    std::fclose(f);
    if ((long)good != total) {
      // un registro roto al final (HOME a mitad de escritura): se reescribe el fichero con los buenos
      if (FILE* w = std::fopen(path.c_str(), "wb")) {
        std::fwrite("DKS1", 1, 4, w);
        for (const auto& kv : s_disk) {
          const uint32_t len = (uint32_t)kv.second.size();
          std::fwrite(&kv.first, 8, 1, w);
          std::fwrite(&len, 4, 1, w);
          std::fwrite(kv.second.data(), 1, len, w);
        }
        std::fclose(w);
      }
    }
  }
  s_diskf = std::fopen(path.c_str(), "ab");
  if (s_diskf && std::ftell(s_diskf) == 0) std::fwrite("DKS1", 1, 4, s_diskf);
  Log("caché DKSH %s: %zu shaders", path.c_str(), s_disk.size());
}

struct Job { std::string glsl; int stage; std::vector<uint8_t> out; bool ok; uint64_t us; };
static void JobThread(void* p) {
  Job* j = (Job*)p;
  const uint64_t t0 = armTicksToNs(armGetSystemTick());
  j->ok = uamlib::Compile((uamlib::Stage)j->stage, j->glsl.c_str(), j->out);
  j->us = (armTicksToNs(armGetSystemTick()) - t0) / 1000;
}
// GLSL de GL -> Code (convertido, compilado con uam o sacado de la caché, cargado en la memoria de código)
static uint64_t s_compile_us = 0;
static int s_compiles = 0;
static Code* GetCode(const std::string& src, GLenum type, std::string& log) {
  const dkglsl::Stage st = type == GL_VERTEX_SHADER ? dkglsl::Vertex : dkglsl::Fragment;
  dkglsl::Reflection refl;
  const std::string conv = dkglsl::Convert(src, st, refl);
  const uint64_t key = dkglsl::CacheKey(conv);
  auto it = s_code.find(key);
  if (it != s_code.end()) return it->second;
  if (!refl.error.empty()) Log("conversión GLSL: %s", refl.error.c_str());
  std::vector<uint8_t> dksh;
  auto dit = s_disk.find(key);
  if (dit != s_disk.end()) {
    dksh = dit->second;
  } else {
    Job j{conv, (int)st, {}, false, 0};
    Thread t;
    if (R_FAILED(threadCreate(&t, JobThread, &j, nullptr, 8 << 20, 0x2C, -2))) { log = "threadCreate"; return nullptr; }
    threadStart(&t);
    threadWaitForExit(&t);
    threadClose(&t);
    if (!j.ok) {
      log = "uam no compiló el shader";
      Log("uam FALLÓ (%s):\n%s", type == GL_VERTEX_SHADER ? "VS" : "FS", conv.c_str());
      return nullptr;
    }
    s_compile_us += j.us;
    ++s_compiles;
    Log("compilado %s con uam en %.1f ms (%zu B)", type == GL_VERTEX_SHADER ? "VS" : "FS", j.us / 1000.0, j.out.size());
    dksh = std::move(j.out);
    if (s_diskf) {
      const uint32_t len = (uint32_t)dksh.size();
      std::vector<uint8_t> rec(12 + len);
      std::memcpy(rec.data(), &key, 8);
      std::memcpy(rec.data() + 8, &len, 4);
      std::memcpy(rec.data() + 12, dksh.data(), len);
      std::fwrite(rec.data(), 1, rec.size(), s_diskf);
      std::fflush(s_diskf);
      fsync(fileno(s_diskf));
    }
    s_disk[key] = dksh;
  }
  Code* c = new Code();
  c->refl = refl;
  c->mem = s_heaps[kHeapCode].Allocate((uint32_t)dksh.size(), DK_SHADER_CODE_ALIGNMENT);
  if (!c->mem) { log = "sin memoria de código"; return nullptr; }
  std::memcpy(c->mem.cpu, dksh.data(), dksh.size());
  DkShaderMaker m;
  dkShaderMakerDefaults(&m, c->mem.blk, c->mem.off);
  dkShaderInitialize(&c->sh, &m);
  c->ok = dkShaderIsValid(&c->sh);
  if (!c->ok) { log = "DKSH no válido"; return nullptr; }
  s_code[key] = c;
  return c;
}

// ============================================================ imágenes
static bool MakeImage(Tex& t, uint32_t w, uint32_t h, DkImageFormat fmt, uint32_t flags) {
  if (t.mem) FreeLater(t.mem);
  if (t.desc > 0) s_deferred.push_back({s_seq, Alloc(), t.desc});
  t.mem = Alloc();
  t.desc = -1;
  t.ok = false;
  t.w = w; t.h = h; t.fmt = fmt;
  DkImageLayoutMaker m;
  dkImageLayoutMakerDefaults(&m, s_dev);
  m.flags = flags;
  m.format = fmt;
  m.dimensions[0] = w;
  m.dimensions[1] = h;
  dkImageLayoutInitialize(&t.layout, &m);
  t.mem = s_heaps[kHeapImage].Allocate((uint32_t)dkImageLayoutGetSize(&t.layout), dkImageLayoutGetAlignment(&t.layout));
  if (!t.mem) return false;
  dkImageInitialize(&t.img, &t.layout, t.mem.blk, t.mem.off);
  dkImageViewDefaults(&t.view, &t.img);
  t.desc = AllocDesc();
  WriteImageDesc(t.desc, &t.view);
  t.ok = true;
  return true;
}

static bool s_xfer_barrier = false;  // una copia/subida pendiente de hacerse visible a los shaders
// Barreras (para medir en la consola; por defecto las seguras):
//   NWII_DK_XFER_BARRIER = full (defecto: DkBarrier_Full + invalidar imagen y L2 antes del draw que sigue a una
//                          subida/copia/blit) | prim (WaitForIdle + invalidar imagen) | inval (solo invalidar imagen)
//   NWII_DK_RT_BARRIER   = frag (defecto: FragmentBarrier + invalidar imagen al cambiar de destino) | none
static int s_xfer_mode = 0, s_rt_mode = 0;
static void ReadBarrierModes() {
  const char* x = std::getenv("NWII_DK_XFER_BARRIER");
  s_xfer_mode = !x ? 0 : !std::strcmp(x, "prim") ? 1 : !std::strcmp(x, "inval") ? 2 : 0;
  const char* r = std::getenv("NWII_DK_RT_BARRIER");
  s_rt_mode = r && !std::strcmp(r, "none") ? 1 : 0;
  if (x || r) Log("barreras: transferencias %s, cambio de destino %s", s_xfer_mode == 0 ? "full" : s_xfer_mode == 1 ? "prim" : "inval",
                  s_rt_mode ? "none" : "frag");
}
static void Upload(Tex& t, int x, int y, int w, int h, GLenum format, const void* data, bool fresh = false) {
  if (!data || w <= 0 || h <= 0) return;
  // una imagen que ya existía puede estar leyéndose en draws anteriores de este frame
  if (!fresh) dkCmdBufBarrier(s_cmd, DkBarrier_Fragments, 0);
  const uint32_t bpp = 4;
  const uint32_t row = (uint32_t)w * bpp;
  Alloc st = s_dataring.Take(row * h, 256);
  if (!st) {
    st = s_heaps[kHeapData].Allocate(row * h, 256);
    FreeLater(st);
  }
  if (format == GL_RGBA || format == GL_RGBA8) {
    const uint32_t src_row = (row + g.unpack_align - 1) & ~(uint32_t)(g.unpack_align - 1);
    if (src_row == row) std::memcpy(st.cpu, data, (size_t)row * h);
    else for (int r = 0; r < h; ++r) std::memcpy(st.cpu + r * row, (const uint8_t*)data + r * src_row, row);
  } else {
    Log("glTex*Image: formato 0x%x no soportado", format);
    return;
  }
  DkCopyBuf src = {st.gpu, 0, 0};
  DkImageRect r = {(uint32_t)x, (uint32_t)y, 0, (uint32_t)w, (uint32_t)h, 1};
  dkCmdBufCopyBufferToImage(s_cmd, &src, &t.view, &r, 0);
  s_xfer_barrier = true;
}

// ============================================================ estado aplicado al grabar
struct Bound {
  const DkImageView* rt_color = nullptr;
  const DkImageView* rt_depth = nullptr;
  bool rt_valid = false;
  Code* vs = nullptr;
  Code* fs = nullptr;
  StageProg* sp[2] = {nullptr, nullptr};
  DkResHandle tex[2][32];
  bool tex_valid[2][32] = {};
  DkRasterizerState rs;
  DkColorState cs;
  DkColorWriteState cw;
  DkBlendState bs;
  DkDepthStencilState ds;
  DkViewport vp;
  DkScissor sc;
  bool states_valid = false;
  uint32_t vtx_hash = 0;
  bool vtx_valid = false;
};
static Bound b;

static Tex* FboColor(GLuint fbo) {
  if (!fbo) {
    if (s_slot < 0) {
      const uint64_t t0 = armGetSystemTick();
      s_slot = dkQueueAcquireImage(s_q, s_swap);
      s_st_acquire_ticks += armGetSystemTick() - t0;
    }
    return &s_fb[s_slot];
  }
  Fbo* f = s_fbos.Get(fbo);
  return f ? s_texs.Get(f->color) : nullptr;
}
static Tex* FboDepth(GLuint fbo) {
  if (!fbo) return &s_fbdepth;
  Fbo* f = s_fbos.Get(fbo);
  if (!f) return nullptr;
  if (f->depth_rbo) { Rbo* r = s_rbos.Get(f->depth_rbo); return r && r->t.ok ? &r->t : nullptr; }
  if (f->depth_tex) return s_texs.Get(f->depth_tex);
  return nullptr;
}

static void BindTargets() {
  Tex* c = FboColor(g.draw_fbo);
  Tex* d = FboDepth(g.draw_fbo);
  const DkImageView* cv = c && c->ok ? &c->view : nullptr;
  const DkImageView* dv = d && d->ok ? &d->view : nullptr;
  if (b.rt_valid && cv == b.rt_color && dv == b.rt_depth) return;
  // lo dibujado en el destino anterior puede leerse como textura a partir de ahora
  if (!s_rt_mode) { dkCmdBufBarrier(s_cmd, DkBarrier_Fragments, DkInvalidateFlags_Image); ++s_st_barriers; }
  ++s_st_rt;
  dkCmdBufBindRenderTargets(s_cmd, cv ? &cv : nullptr, cv ? 1 : 0, dv);
  b.rt_color = cv;
  b.rt_depth = dv;
  b.rt_valid = true;
}

static DkBlendFactor BF(GLenum f) {
  switch (f) {
    case GL_ZERO: return DkBlendFactor_Zero;
    case GL_ONE: return DkBlendFactor_One;
    case GL_SRC_COLOR: return DkBlendFactor_SrcColor;
    case GL_ONE_MINUS_SRC_COLOR: return DkBlendFactor_InvSrcColor;
    case GL_SRC_ALPHA: return DkBlendFactor_SrcAlpha;
    case GL_ONE_MINUS_SRC_ALPHA: return DkBlendFactor_InvSrcAlpha;
    case GL_DST_ALPHA: return DkBlendFactor_DstAlpha;
    case GL_ONE_MINUS_DST_ALPHA: return DkBlendFactor_InvDstAlpha;
    case GL_DST_COLOR: return DkBlendFactor_DstColor;
    case GL_ONE_MINUS_DST_COLOR: return DkBlendFactor_InvDstColor;
    case GL_SRC_ALPHA_SATURATE: return DkBlendFactor_SrcAlphaSaturate;
    case GL_SRC1_COLOR: return DkBlendFactor_Src1Color;
    case GL_ONE_MINUS_SRC1_COLOR: return DkBlendFactor_InvSrc1Color;
    case GL_SRC1_ALPHA: return DkBlendFactor_Src1Alpha;
    case GL_ONE_MINUS_SRC1_ALPHA: return DkBlendFactor_InvSrc1Alpha;
    default: return DkBlendFactor_One;
  }
}
static DkBlendOp BO(GLenum e) {
  switch (e) {
    case GL_FUNC_SUBTRACT: return DkBlendOp_Sub;
    case GL_FUNC_REVERSE_SUBTRACT: return DkBlendOp_RevSub;
    case GL_MIN: return DkBlendOp_Min;
    case GL_MAX: return DkBlendOp_Max;
    default: return DkBlendOp_Add;
  }
}
static DkCompareOp CO(GLenum f) { return (DkCompareOp)(f - GL_NEVER + 1); }  // mismo orden

// deko3d 0.5.0 (y master) #29: dkCmdBufBindBlendStates escribe en FuncAlphaDst el factor destino de COLOR
// (confirmado en la consola: dk3test T4). Tras cada bind se reescribe ese método a mano: IndependentBlend[i]
// está en 0x780 + 8*i, FuncAlphaDst = +6 (engine_3d.def); cabecera Maxwell "increasing", 1 palabra, subcanal
// 3D = 0; el valor es getBlendFactorSetting() = factor & 0x1f (sin factores de color constante).
static void BindBlendFixed(DkCmdBuf cmd, uint32_t id, const DkBlendState& bs) {
  dkCmdBufBindBlendState(cmd, id, &bs);
  const uint32_t method = 0x780 + 8 * id + 6;
  const uint32_t w[2] = {method | (1u << 16) | (1u << 29), (uint32_t)bs.dstAlphaBlendFactor & 0x1f};
  dkCmdBufReplayCmds(cmd, w, 2);
}

static void ApplyFixedState() {
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.rasterizerEnable = !g.raster_discard;
  rs.depthClampEnable = g.depth_clamp;
  rs.cullMode = !g.cull ? DkFace_None : g.cull_face == GL_FRONT ? DkFace_Front : g.cull_face == GL_BACK ? DkFace_Back : DkFace_FrontAndBack;
  rs.frontFace = g.front_face == GL_CW ? DkFrontFace_CW : DkFrontFace_CCW;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  if (g.logic) cs.logicOp = (DkLogicOp)(g.logic_op - GL_CLEAR);
  else if (g.blend) dkColorStateSetBlendEnable(&cs, 0, true);
  DkColorWriteState cw;
  dkColorWriteStateDefaults(&cw);
  dkColorWriteStateSetMask(&cw, 0, (g.cmask[0] ? DkColorMask_R : 0) | (g.cmask[1] ? DkColorMask_G : 0) |
                                       (g.cmask[2] ? DkColorMask_B : 0) | (g.cmask[3] ? DkColorMask_A : 0));
  DkBlendState bs;
  dkBlendStateDefaults(&bs);
  dkBlendStateSetOps(&bs, BO(g.blend_eq_rgb), BO(g.blend_eq_a));
  dkBlendStateSetFactors(&bs, BF(g.bsrc_rgb), BF(g.bdst_rgb), BF(g.bsrc_a), BF(g.bdst_a));
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = g.depth_test;
  ds.depthWriteEnable = g.depth_test && g.depth_mask;
  ds.depthCompareOp = CO(g.depth_func);
  DkViewport vp = {g.vpf[0], g.vpf[1], g.vpf[2], g.vpf[3], (float)g.dr[0], (float)g.dr[1]};
  DkScissor sc = g.scissor ? DkScissor{(uint32_t)std::max(0, g.sc[0]), (uint32_t)std::max(0, g.sc[1]),
                                        (uint32_t)std::max(0, g.sc[2]), (uint32_t)std::max(0, g.sc[3])}
                           : DkScissor{0, 0, 16384, 16384};
  const bool all = !b.states_valid;
  if (all || std::memcmp(&rs, &b.rs, sizeof rs)) { dkCmdBufBindRasterizerState(s_cmd, &rs); b.rs = rs; }
  if (all || std::memcmp(&cs, &b.cs, sizeof cs)) { dkCmdBufBindColorState(s_cmd, &cs); b.cs = cs; }
  if (all || std::memcmp(&cw, &b.cw, sizeof cw)) { dkCmdBufBindColorWriteState(s_cmd, &cw); b.cw = cw; }
  if (all || std::memcmp(&bs, &b.bs, sizeof bs)) { BindBlendFixed(s_cmd, 0, bs); b.bs = bs; }
  if (all || std::memcmp(&ds, &b.ds, sizeof ds)) { dkCmdBufBindDepthStencilState(s_cmd, &ds); b.ds = ds; }
  if (all || std::memcmp(&vp, &b.vp, sizeof vp)) { dkCmdBufSetViewports(s_cmd, 0, &vp, 1); b.vp = vp; }
  if (all || std::memcmp(&sc, &b.sc, sizeof sc)) { dkCmdBufSetScissors(s_cmd, 0, &sc, 1); b.sc = sc; }
  b.states_valid = true;
}

static StageProg* CurStage(int s) {
  if (g.prog) {
    Prog* p = s_progs.Get(g.prog - kProgBase);
    return p && p->linked && p->st[s].code ? &p->st[s] : nullptr;
  }
  if (g.pipe) {
    Pipe* pp = s_pipes.Get(g.pipe);
    if (!pp) return nullptr;
    Prog* p = s_progs.Get((s ? pp->fs : pp->vs) - kProgBase);
    return p && p->linked && p->st[s].code ? &p->st[s] : nullptr;
  }
  return nullptr;
}

static DkResHandle TexHandle(Tex* t, GLuint smp_id) {
  if (!t || !t->ok) return dkMakeTextureHandle(0, SamplerSlot({GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE}));
  SamplerKey k{t->minf, t->magf, t->ws, t->wt};
  if (Sampler* s = s_smps.Get(smp_id)) k = {s->minf, s->magf, s->ws, s->wt};
  return dkMakeTextureHandle((uint32_t)t->desc, (uint32_t)SamplerSlot(k));
}

static bool PrepareDraw() {
  // NWII_DK_DRAW_INVALIDATE=1 (diagnóstico): como Mesa/nvc0 con buffers coherentes, WaitForIdle + invalidar la caché
  // de texturas y la L2 antes de CADA draw (lento). Si con esto desaparece un fallo de imagen, es de coherencia.
  static const bool s_draw_inval = [] { const char* e = std::getenv("NWII_DK_DRAW_INVALIDATE"); return e && *e == '1'; }();
  if (s_draw_inval) dkCmdBufBarrier(s_cmd, DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
  BindTargets();
  if (s_xfer_barrier) {
    if (s_xfer_mode == 0) dkCmdBufBarrier(s_cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    else if (s_xfer_mode == 1) dkCmdBufBarrier(s_cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
    else dkCmdBufBarrier(s_cmd, DkBarrier_None, DkInvalidateFlags_Image);
    s_xfer_barrier = false;
    ++s_st_barriers;
  }
  ApplyFixedState();
  StageProg* sp[2] = {CurStage(0), CurStage(1)};
  if (!sp[0] || !sp[1]) {
    static int n = 0;
    if (n++ < 8) Log("draw sin programa completo (vs %p fs %p): se salta", (void*)sp[0], (void*)sp[1]);
    return false;
  }
  if (sp[0]->code != b.vs || sp[1]->code != b.fs) {
    DkShader const* shs[2] = {&sp[0]->code->sh, &sp[1]->code->sh};
    dkCmdBufBindShaders(s_cmd, DkStageFlag_GraphicsMask, shs, 2);
    b.vs = sp[0]->code;
    b.fs = sp[1]->code;
  }
  for (int s = 0; s < 2; ++s) {
    const DkStage stage = s ? DkStage_Fragment : DkStage_Vertex;
    StageProg& p = *sp[s];
    if (p.ubo_size) {
      if (b.sp[s] != &p) dkCmdBufBindUniformBuffer(s_cmd, stage, 0, p.ubo_mem.gpu, p.ubo_size);
      if (p.dlo < p.dhi) {
        const uint32_t lo = p.dlo & ~3u, hi = std::min<uint32_t>((p.dhi + 3) & ~3u, (uint32_t)p.ubo.size());
        dkCmdBufPushConstants(s_cmd, p.ubo_mem.gpu, p.ubo_size, lo, hi - lo, p.ubo.data() + lo);
        s_st_push_bytes += hi - lo;
        p.dlo = ~0u;
        p.dhi = 0;
      }
    }
    b.sp[s] = &p;
    for (const auto& smp : p.code->refl.samplers) {
      const int unit = p.unit[smp.binding] & 31;
      const DkResHandle h = smp.buffer ? TexHandle(s_texs.Get(g.texbuf[unit]), 0)
                                       : TexHandle(s_texs.Get(g.tex2d[unit]), g.smp[unit]);
      if (!b.tex_valid[s][smp.binding] || b.tex[s][smp.binding] != h) {
        dkCmdBufBindTexture(s_cmd, stage, smp.binding, h);
        b.tex[s][smp.binding] = h;
        b.tex_valid[s][smp.binding] = true;
      }
    }
  }
  // vértices: un binding por atributo activo (dirección = buffer + offset)
  Vao* v = g.vao ? s_vaos.Get(g.vao) : &s_default_vao;
  if (!v) v = &s_default_vao;
  DkVtxAttribState at[16];
  DkVtxBufferState vb[16];
  DkBufExtents ext[16];
  int n = 0;
  for (int i = 0; i < 16; ++i) if (v->a[i].en) n = i + 1;
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; ++i) {
    std::memset(&at[i], 0, sizeof at[i]);
    const Attrib& a = v->a[i];
    Buf* bf = a.en ? s_bufs.Get(a.buf) : nullptr;
    at[i].bufferId = i;
    if (!bf || !bf->mem) {
      at[i].isFixed = 1;
      at[i].size = DkVtxAttribSize_4x32;
      at[i].type = DkVtxAttribType_Float;
      vb[i] = {0, 0};
      ext[i] = {s_zero.gpu, 256};
    } else {
      at[i].offset = 0;
      const int c = std::clamp(a.size, 1, 4);
      if (a.type == GL_FLOAT) {
        static const DkVtxAttribSize s32[4] = {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32};
        at[i].size = s32[c - 1];
        at[i].type = DkVtxAttribType_Float;
      } else if (a.type == GL_UNSIGNED_BYTE || a.type == GL_BYTE) {
        static const DkVtxAttribSize s8[4] = {DkVtxAttribSize_1x8, DkVtxAttribSize_2x8, DkVtxAttribSize_3x8, DkVtxAttribSize_4x8};
        at[i].size = s8[c - 1];
        at[i].type = a.type == GL_UNSIGNED_BYTE ? (a.norm ? DkVtxAttribType_Unorm : DkVtxAttribType_Uscaled)
                                                : (a.norm ? DkVtxAttribType_Snorm : DkVtxAttribType_Sscaled);
        if (a.integer) at[i].type = a.type == GL_UNSIGNED_BYTE ? DkVtxAttribType_Uint : DkVtxAttribType_Sint;
      } else if (a.integer && (a.type == GL_UNSIGNED_INT || a.type == GL_INT)) {
        static const DkVtxAttribSize s32[4] = {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32};
        at[i].size = s32[c - 1];
        at[i].type = a.type == GL_UNSIGNED_INT ? DkVtxAttribType_Uint : DkVtxAttribType_Sint;
      } else if (a.type == GL_UNSIGNED_SHORT || a.type == GL_SHORT) {
        static const DkVtxAttribSize s16[4] = {DkVtxAttribSize_1x16, DkVtxAttribSize_2x16, DkVtxAttribSize_3x16, DkVtxAttribSize_4x16};
        at[i].size = s16[c - 1];
        at[i].type = a.type == GL_UNSIGNED_SHORT ? (a.norm ? DkVtxAttribType_Unorm : DkVtxAttribType_Uscaled)
                                                 : (a.norm ? DkVtxAttribType_Snorm : DkVtxAttribType_Sscaled);
      } else {
        at[i].size = DkVtxAttribSize_4x32;
        at[i].type = DkVtxAttribType_Float;
      }
      const uint32_t comp = (a.type == GL_FLOAT || a.type == GL_UNSIGNED_INT || a.type == GL_INT) ? 4
                            : (a.type == GL_UNSIGNED_SHORT || a.type == GL_SHORT) ? 2 : 1;
      vb[i] = {(uint32_t)(a.stride ? a.stride : c * comp), a.divisor};
      ext[i] = {bf->mem.gpu + a.off, (uint32_t)(bf->size > a.off ? bf->size - a.off : 0)};
    }
    uint32_t w[4];
    std::memcpy(&w[0], &at[i], 4);
    w[1] = vb[i].stride;
    w[2] = (uint32_t)ext[i].addr;
    w[3] = (uint32_t)(ext[i].addr >> 32) ^ ext[i].size;
    h = (uint32_t)Fnv(w, 16, h);
  }
  h ^= (uint32_t)n * 0x9E3779B9u;
  if (!b.vtx_valid || h != b.vtx_hash) {
    dkCmdBufBindVtxAttribState(s_cmd, at, n);
    dkCmdBufBindVtxBufferState(s_cmd, vb, n);
    if (n) dkCmdBufBindVtxBuffers(s_cmd, 0, ext, n);
    b.vtx_hash = h;
    b.vtx_valid = true;
  }
  return true;
}

static DkPrimitive Prim(GLenum m) {
  switch (m) {
    case GL_POINTS: return DkPrimitive_Points;
    case GL_LINES: return DkPrimitive_Lines;
    case GL_LINE_STRIP: return DkPrimitive_LineStrip;
    case GL_TRIANGLE_STRIP: return DkPrimitive_TriangleStrip;
    case GL_TRIANGLE_FAN: return DkPrimitive_TriangleFan;
    default: return DkPrimitive_Triangles;
  }
}

// ============================================================ funciones GL
static void ActiveTexture(GLenum t) { g.active_unit = (int)(t - GL_TEXTURE0) & 31; }
static GLuint CreateShader(GLenum type) { GLuint id = s_shds.Gen(); s_shds.Get(id)->type = type; return id; }
static void ShaderSource(GLuint id, GLsizei n, const GLchar* const* str, const GLint* len) {
  Shd* s = s_shds.Get(id);
  if (!s) return;
  s->src.clear();
  for (GLsizei i = 0; i < n; ++i) s->src += len && len[i] >= 0 ? std::string(str[i], len[i]) : std::string(str[i]);
}
static void CompileShader(GLuint id) {
  Shd* s = s_shds.Get(id);
  if (!s) return;
  s->code = GetCode(s->src, s->type, s->log);
  s->compiled = s->code != nullptr;
}
static void GetShaderiv(GLuint id, GLenum pn, GLint* v) {
  Shd* s = s_shds.Get(id);
  if (pn == GL_COMPILE_STATUS) *v = s && s->compiled;
  else if (pn == GL_INFO_LOG_LENGTH) *v = s ? (GLint)s->log.size() + 1 : 0;
  else if (pn == GL_SHADER_TYPE) *v = s ? (GLint)s->type : 0;
  else *v = 0;
}
static void GetShaderInfoLog(GLuint id, GLsizei max, GLsizei* len, GLchar* log) {
  Shd* s = s_shds.Get(id);
  const std::string l = s ? s->log : "";
  const GLsizei n = std::min<GLsizei>(max > 0 ? max - 1 : 0, (GLsizei)l.size());
  if (max > 0) { std::memcpy(log, l.data(), n); log[n] = 0; }
  if (len) *len = n;
}
static void DeleteShader(GLuint id) { s_shds.Del(id); }
static GLuint CreateProgram() { return s_progs.Gen() + kProgBase; }
static Prog* P(GLuint id) { return id >= kProgBase ? s_progs.Get(id - kProgBase) : nullptr; }
static void AttachShader(GLuint p, GLuint s) { if (Prog* pr = P(p)) pr->shaders.push_back(s); }
static void DetachShader(GLuint p, GLuint s) {
  if (Prog* pr = P(p)) pr->shaders.erase(std::remove(pr->shaders.begin(), pr->shaders.end(), s), pr->shaders.end());
}
static void ProgramParameteri(GLuint p, GLenum pn, GLint v) { if (Prog* pr = P(p)) if (pn == GL_PROGRAM_SEPARABLE) pr->separable = v; }
static int LocIndex(Prog* p, const std::string& name) {
  for (size_t i = 0; i < p->locs.size(); ++i) if (p->locs[i].name == name) return (int)i;
  Loc l;
  l.name = name;
  p->locs.push_back(l);
  return (int)p->locs.size() - 1;
}
static void LinkProgram(GLuint id) {
  Prog* p = P(id);
  if (!p) return;
  p->linked = false;
  p->locs.clear();
  for (int s = 0; s < 2; ++s) { p->st[s] = StageProg(); }
  bool ok = true;
  for (GLuint sid : p->shaders) {
    Shd* s = s_shds.Get(sid);
    if (!s) continue;
    if (!s->compiled) { ok = false; p->log = s->log; continue; }
    const int st = s->type == GL_VERTEX_SHADER ? 0 : 1;
    StageProg& sp = p->st[st];
    sp.code = s->code;
    const auto& refl = s->code->refl;
    sp.ubo.assign(refl.ubo_size, 0);
    sp.ubo_size = (refl.ubo_size + 255) & ~255u;
    if (sp.ubo_size) {
      sp.ubo_mem = s_heaps[kHeapData].Allocate(sp.ubo_size, 256);
      sp.dlo = 0;
      sp.dhi = refl.ubo_size;
    }
    for (size_t u = 0; u < refl.uniforms.size(); ++u) p->locs[LocIndex(p, refl.uniforms[u].name)].u[st] = (int)u;
    for (const auto& smp : refl.samplers) p->locs[LocIndex(p, smp.name)].smp[st] = smp.binding;
  }
  p->linked = ok;
}
static void GetProgramiv(GLuint id, GLenum pn, GLint* v) {
  Prog* p = P(id);
  if (pn == GL_LINK_STATUS) *v = p && p->linked;
  else if (pn == GL_INFO_LOG_LENGTH) *v = p ? (GLint)p->log.size() + 1 : 0;
  else *v = 0;
}
static void GetProgramInfoLog(GLuint id, GLsizei max, GLsizei* len, GLchar* log) {
  Prog* p = P(id);
  const std::string l = p ? p->log : "";
  const GLsizei n = std::min<GLsizei>(max > 0 ? max - 1 : 0, (GLsizei)l.size());
  if (max > 0) { std::memcpy(log, l.data(), n); log[n] = 0; }
  if (len) *len = n;
}
static void DeleteProgram(GLuint id) {
  Prog* p = P(id);
  if (!p) return;
  for (auto& s : p->st) if (s.ubo_mem) FreeLater(s.ubo_mem);
  if (g.prog == id) g.prog = 0;
  for (int s = 0; s < 2; ++s) b.sp[s] = nullptr;
  s_progs.Del(id - kProgBase);
}
static void UseProgram(GLuint id) { g.prog = id; }
static GLint GetUniformLocation(GLuint id, const GLchar* name) {
  Prog* p = P(id);
  if (!p || !p->linked) return -1;
  std::string n = name;
  int elem = 0;
  const size_t br = n.find('[');
  if (br != std::string::npos) { elem = std::atoi(n.c_str() + br + 1); n = n.substr(0, br); }
  for (size_t i = 0; i < p->locs.size(); ++i)
    if (p->locs[i].name == n) return (GLint)(i * 64 + elem);
  return -1;
}
// escribe 'count' elementos (cada uno 'bytes' bytes) a partir de la location
static void SetU(GLuint prog, GLint loc, GLsizei count, const void* data, uint32_t bytes, bool is_int) {
  Prog* p = P(prog);
  if (!p || loc < 0) return;
  const size_t li = (size_t)loc / 64;
  const int elem = loc % 64;
  if (li >= p->locs.size()) return;
  const Loc& l = p->locs[li];
  for (int s = 0; s < 2; ++s) {
    StageProg& sp = p->st[s];
    if (l.smp[s] >= 0 && is_int) {  // sampler: unidad de textura
      sp.unit[l.smp[s] & 31] = (uint8_t)(*(const GLint*)data);
      continue;
    }
    if (l.u[s] < 0 || !sp.code) continue;
    const dkglsl::Uniform& u = sp.code->refl.uniforms[l.u[s]];
    for (GLsizei e = 0; e < count && elem + e < u.count; ++e) {
      const uint32_t off = u.offset + (uint32_t)(elem + e) * u.stride;
      const uint32_t n = std::min<uint32_t>(bytes, u.cols > 1 ? u.cols * 16u : u.comps * 4u);
      if (off + n > sp.ubo.size()) break;
      const uint8_t* src = (const uint8_t*)data + (size_t)e * bytes;
      if (!std::memcmp(sp.ubo.data() + off, src, n)) continue;  // igual que lo que ya tiene la GPU (o pendiente)
      std::memcpy(sp.ubo.data() + off, src, n);
      sp.dlo = std::min(sp.dlo, off);
      sp.dhi = std::max(sp.dhi, off + n);
    }
  }
}
static GLuint CurProgForUniform() {
  if (g.prog) return g.prog;
  return 0;
}
static void Uniform1i(GLint l, GLint v) { SetU(CurProgForUniform(), l, 1, &v, 4, true); }
static void Uniform1f(GLint l, GLfloat v) { SetU(CurProgForUniform(), l, 1, &v, 4, false); }
static void Uniform1ui(GLint l, GLuint v) { SetU(CurProgForUniform(), l, 1, &v, 4, false); }
static void Uniform1iv(GLint l, GLsizei n, const GLint* v) { SetU(CurProgForUniform(), l, n, v, 4, true); }
static void Uniform2fv(GLint l, GLsizei n, const GLfloat* v) { SetU(CurProgForUniform(), l, n, v, 8, false); }
static void Uniform3i(GLint l, GLint x, GLint y, GLint z) { GLint v[3] = {x, y, z}; SetU(CurProgForUniform(), l, 1, v, 12, false); }
static void Uniform4fv(GLint l, GLsizei n, const GLfloat* v) { SetU(CurProgForUniform(), l, n, v, 16, false); }
static void Uniform4iv(GLint l, GLsizei n, const GLint* v) { SetU(CurProgForUniform(), l, n, v, 16, false); }
static void UniformMatrix4fv(GLint l, GLsizei n, GLboolean tr, const GLfloat* v) {
  if (tr) Log("glUniformMatrix4fv con transpose: no soportado");
  SetU(CurProgForUniform(), l, n, v, 64, false);
}
static void ProgramUniform1i(GLuint p, GLint l, GLint v) { SetU(p, l, 1, &v, 4, true); }
static void ProgramUniform1ui(GLuint p, GLint l, GLuint v) { SetU(p, l, 1, &v, 4, false); }
static void ProgramUniform1f(GLuint p, GLint l, GLfloat v) { SetU(p, l, 1, &v, 4, false); }
static void ProgramUniform1iv(GLuint p, GLint l, GLsizei n, const GLint* v) { SetU(p, l, n, v, 4, true); }
static void ProgramUniform2fv(GLuint p, GLint l, GLsizei n, const GLfloat* v) { SetU(p, l, n, v, 8, false); }
static void ProgramUniform4fv(GLuint p, GLint l, GLsizei n, const GLfloat* v) { SetU(p, l, n, v, 16, false); }
static void ProgramUniform4iv(GLuint p, GLint l, GLsizei n, const GLint* v) { SetU(p, l, n, v, 16, false); }
static void ProgramUniformMatrix4fv(GLuint p, GLint l, GLsizei n, GLboolean, const GLfloat* v) { SetU(p, l, n, v, 64, false); }

static void GenProgramPipelines(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = s_pipes.Gen(); }
static void DeleteProgramPipelines(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; ++i) s_pipes.Del(ids[i]); }
static void BindProgramPipeline(GLuint id) { g.pipe = id; }
static void UseProgramStages(GLuint pp, GLbitfield bits, GLuint prog) {
  Pipe* p = s_pipes.Get(pp);
  if (!p) return;
  if (bits & GL_VERTEX_SHADER_BIT) p->vs = prog;
  if (bits & GL_FRAGMENT_SHADER_BIT) p->fs = prog;
}

// ---- buffers
static GLuint* BindPoint(GLenum t) {
  static GLuint dummy;
  switch (t) {
    case GL_ARRAY_BUFFER: return &g.array_buf;
    case GL_ELEMENT_ARRAY_BUFFER: { Vao* v = g.vao ? s_vaos.Get(g.vao) : &s_default_vao; return v ? &v->ebo : &dummy; }
    case GL_PIXEL_PACK_BUFFER: return &g.pack_buf;
    case GL_TEXTURE_BUFFER: return &g.texbuf_bind;
    default: return &dummy;
  }
}
static void GenBuffers(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = s_bufs.Gen(); }
static void DeleteBuffers(GLsizei n, const GLuint* ids) {
  for (GLsizei i = 0; i < n; ++i) {
    Buf* bf = s_bufs.Get(ids[i]);
    if (bf && bf->heap) FreeLater(bf->mem);
    s_bufs.Del(ids[i]);
  }
  b.vtx_valid = false;
}
static void BindBuffer(GLenum t, GLuint id) { *BindPoint(t) = id; }
static void BindBufferBase(GLenum, GLuint, GLuint) {}
static void BufAlloc(Buf* bf, uint32_t size, bool persistent) {  // persistent=false: GL_STREAM_DRAW
  if (bf->heap) FreeLater(bf->mem);
  bf->mem = Alloc();
  bf->heap = false;
  bf->size = size;
  if (!persistent && size <= (1u << 20)) {
    bf->mem = s_dataring.Take(std::max<uint32_t>(size, 4), 256);
  }
  if (!bf->mem) {
    bf->mem = s_heaps[kHeapData].Allocate(std::max<uint32_t>(size, 256), 256);
    bf->heap = true;
  }
  b.vtx_valid = false;
}
static void BufferData(GLenum t, GLsizeiptr size, const void* data, GLenum usage) {
  Buf* bf = s_bufs.Get(*BindPoint(t));
  if (!bf) return;
  // solo GL_STREAM_DRAW va al anillo de datos (vale un frame); lo demás tiene memoria propia
  BufAlloc(bf, (uint32_t)size, usage != GL_STREAM_DRAW);
  if (data && bf->mem) std::memcpy(bf->mem.cpu, data, (size_t)size);
}
static void BufferStorage(GLenum t, GLsizeiptr size, const void* data, GLbitfield) {
  Buf* bf = s_bufs.Get(*BindPoint(t));
  if (!bf) return;
  BufAlloc(bf, (uint32_t)size, true);
  if (data && bf->mem) std::memcpy(bf->mem.cpu, data, (size_t)size);
}
static void BufferSubData(GLenum t, GLintptr off, GLsizeiptr size, const void* data) {
  Buf* bf = s_bufs.Get(*BindPoint(t));
  if (bf && bf->mem && off + size <= bf->size) std::memcpy(bf->mem.cpu + off, data, (size_t)size);
}
static void* MapBufferRange(GLenum t, GLintptr off, GLsizeiptr, GLbitfield access) {
  Buf* bf = s_bufs.Get(*BindPoint(t));
  if (!bf || !bf->mem) return nullptr;
  if (t == GL_PIXEL_PACK_BUFFER && (access & GL_MAP_READ_BIT)) Finish();  // lectura de un PBO
  return bf->mem.cpu + off;
}
static GLboolean UnmapBuffer(GLenum) { return GL_TRUE; }
static void GetBufferSubData(GLenum t, GLintptr off, GLsizeiptr size, void* data) {
  Buf* bf = s_bufs.Get(*BindPoint(t));
  Finish();
  if (bf && bf->mem) std::memcpy(data, bf->mem.cpu + off, (size_t)size);
}

// ---- VAO
static void GenVertexArrays(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = s_vaos.Gen(); }
static void DeleteVertexArrays(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; ++i) s_vaos.Del(ids[i]); if (n) b.vtx_valid = false; }
static void BindVertexArray(GLuint id) { g.vao = id; }
static Vao* CurVao() { Vao* v = g.vao ? s_vaos.Get(g.vao) : &s_default_vao; return v ? v : &s_default_vao; }
static void EnableVertexAttribArray(GLuint i) { if (i < 16) CurVao()->a[i].en = true; }
static void DisableVertexAttribArray(GLuint i) { if (i < 16) CurVao()->a[i].en = false; }
static void VertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr) {
  if (i >= 16) return;
  Attrib& a = CurVao()->a[i];
  a.size = size; a.type = type; a.norm = norm; a.integer = false; a.stride = stride; a.off = (uintptr_t)ptr; a.buf = g.array_buf;
}
static void VertexAttribIPointer(GLuint i, GLint size, GLenum type, GLsizei stride, const void* ptr) {
  if (i >= 16) return;
  Attrib& a = CurVao()->a[i];
  a.size = size; a.type = type; a.norm = false; a.integer = true; a.stride = stride; a.off = (uintptr_t)ptr; a.buf = g.array_buf;
}
static void VertexAttribDivisor(GLuint i, GLuint d) { if (i < 16) CurVao()->a[i].divisor = d; }

// ---- texturas
static void GenTextures(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = s_texs.Gen(); }
static void DeleteTextures(GLsizei n, const GLuint* ids) {
  for (GLsizei i = 0; i < n; ++i) {
    Tex* t = s_texs.Get(ids[i]);
    if (!t) continue;
    if (t->mem && t->target != GL_TEXTURE_BUFFER) FreeLater(t->mem);
    if (t->desc > 0) s_deferred.push_back({s_seq, Alloc(), t->desc});
    s_texs.Del(ids[i]);
  }
  b.rt_valid = false;
  std::memset(b.tex_valid, 0, sizeof b.tex_valid);
}
static void BindTexture(GLenum target, GLuint id) {
  if (Tex* t = s_texs.Get(id)) if (!t->target) t->target = target;
  if (target == GL_TEXTURE_BUFFER) g.texbuf[g.active_unit] = id;
  else g.tex2d[g.active_unit] = id;
}
static Tex* BoundTex(GLenum target) {
  return s_texs.Get(target == GL_TEXTURE_BUFFER ? g.texbuf[g.active_unit] : g.tex2d[g.active_unit]);
}
static void TexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint, GLenum fmt, GLenum type, const void* data) {
  Tex* t = BoundTex(target);
  if (!t || level != 0) return;
  const bool u32 = ifmt == GL_RGBA32UI;  // destino de la emulación de transform feedback (renderer_gl.cpp)
  if (!u32 && type != GL_UNSIGNED_BYTE) Log("glTexImage2D: tipo 0x%x no soportado", type);
  if (!u32 && ifmt != GL_RGBA8 && ifmt != GL_RGBA) Log("glTexImage2D: formato interno 0x%x tratado como RGBA8", ifmt);
  const DkImageFormat df = u32 ? DkImageFormat_RGBA32_Uint : DkImageFormat_RGBA8_Unorm;
  bool fresh = false;
  if (!t->ok || t->w != (uint32_t)w || t->h != (uint32_t)h || t->fmt != df)
    fresh = MakeImage(*t, (uint32_t)std::max(1, w), (uint32_t)std::max(1, h), df,
              u32 ? DkImageFlags_UsageRender : DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine);
  if (u32) { b.rt_valid = false; std::memset(b.tex_valid, 0, sizeof b.tex_valid); return; }
  b.rt_valid = false;
  std::memset(b.tex_valid, 0, sizeof b.tex_valid);
  Upload(*t, 0, 0, w, h, fmt, data, fresh);
}
static void TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum, const void* data) {
  Tex* t = BoundTex(target);
  if (t && t->ok && level == 0) Upload(*t, x, y, w, h, fmt, data);
}
static void TexImage3D(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) {
  static bool once = false;
  if (!once) { once = true; Log("glTexImage3D no soportado (captura F12 de PC)"); }
}
static void TexParameteri(GLenum target, GLenum pn, GLint v) {
  Tex* t = BoundTex(target);
  if (!t) return;
  if (pn == GL_TEXTURE_MIN_FILTER) t->minf = v;
  else if (pn == GL_TEXTURE_MAG_FILTER) t->magf = v;
  else if (pn == GL_TEXTURE_WRAP_S) t->ws = v;
  else if (pn == GL_TEXTURE_WRAP_T) t->wt = v;
  std::memset(b.tex_valid, 0, sizeof b.tex_valid);
}
static void TexBuffer(GLenum, GLenum ifmt, GLuint buf) {
  Tex* t = BoundTex(GL_TEXTURE_BUFFER);
  Buf* bf = s_bufs.Get(buf);
  if (!t || !bf || !bf->mem) return;
  const DkImageFormat fmt = ifmt == GL_RGBA32UI ? DkImageFormat_RGBA32_Uint : DkImageFormat_R32_Uint;
  const uint32_t bpt = ifmt == GL_RGBA32UI ? 16 : 4;
  if (t->desc > 0) s_deferred.push_back({s_seq, Alloc(), t->desc});
  t->target = GL_TEXTURE_BUFFER;
  t->w = bf->size / bpt;
  t->h = 1;
  t->fmt = fmt;
  DkImageLayoutMaker m;
  dkImageLayoutMakerDefaults(&m, s_dev);
  m.type = DkImageType_Buffer;
  m.format = fmt;
  m.dimensions[0] = t->w;
  dkImageLayoutInitialize(&t->layout, &m);
  dkImageInitialize(&t->img, &t->layout, bf->mem.blk, bf->mem.off);
  dkImageViewDefaults(&t->view, &t->img);
  t->mem = bf->mem;  // no es suya: no se libera con la textura
  t->desc = AllocDesc();
  WriteImageDesc(t->desc, &t->view);
  t->ok = true;
  std::memset(b.tex_valid, 0, sizeof b.tex_valid);
}
static void GenSamplers(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = s_smps.Gen(); }
static void SamplerParameteri(GLuint id, GLenum pn, GLint v) {
  Sampler* s = s_smps.Get(id);
  if (!s) return;
  if (pn == GL_TEXTURE_MIN_FILTER) s->minf = v;
  else if (pn == GL_TEXTURE_MAG_FILTER) s->magf = v;
  else if (pn == GL_TEXTURE_WRAP_S) s->ws = v;
  else if (pn == GL_TEXTURE_WRAP_T) s->wt = v;
}
static void BindSampler(GLuint unit, GLuint id) { if (unit < 32) g.smp[unit] = id; }
static void PixelStorei(GLenum pn, GLint v) {
  if (pn == GL_PACK_ALIGNMENT) g.pack_align = v;
  else if (pn == GL_UNPACK_ALIGNMENT) g.unpack_align = v;
}

// ---- framebuffers
static void GenFramebuffers(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = s_fbos.Gen(); }
static void DeleteFramebuffers(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; ++i) s_fbos.Del(ids[i]); b.rt_valid = false; }
static void BindFramebuffer(GLenum t, GLuint id) {
  if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) g.draw_fbo = id;
  if (t == GL_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER) g.read_fbo = id;
}
static Fbo* TargetFbo(GLenum t) { return s_fbos.Get(t == GL_READ_FRAMEBUFFER ? g.read_fbo : g.draw_fbo); }
static void FramebufferTexture2D(GLenum t, GLenum att, GLenum, GLuint tex, GLint) {
  Fbo* f = TargetFbo(t);
  if (!f) return;
  if (att == GL_COLOR_ATTACHMENT0) f->color = tex;
  else if (att == GL_DEPTH_ATTACHMENT || att == GL_DEPTH_STENCIL_ATTACHMENT) { f->depth_tex = tex; f->depth_rbo = 0; }
  b.rt_valid = false;
}
static void FramebufferTextureLayer(GLenum, GLenum, GLuint, GLint, GLint) {}
static void GenRenderbuffers(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = s_rbos.Gen(); }
static void DeleteRenderbuffers(GLsizei n, const GLuint* ids) {
  for (GLsizei i = 0; i < n; ++i) {
    Rbo* r = s_rbos.Get(ids[i]);
    if (r && r->t.mem) FreeLater(r->t.mem);
    if (r && r->t.desc > 0) s_deferred.push_back({s_seq, Alloc(), r->t.desc});
    s_rbos.Del(ids[i]);
  }
  b.rt_valid = false;
}
static void BindRenderbuffer(GLenum, GLuint id) { g.rbo = id; }
static void RenderbufferStorage(GLenum, GLenum, GLsizei w, GLsizei h) {
  Rbo* r = s_rbos.Get(g.rbo);
  if (!r) return;
  MakeImage(r->t, (uint32_t)w, (uint32_t)h, DkImageFormat_Z24S8, DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine);
  b.rt_valid = false;
}
static void FramebufferRenderbuffer(GLenum t, GLenum att, GLenum, GLuint rbo) {
  Fbo* f = TargetFbo(t);
  if (!f) return;
  if (att == GL_DEPTH_ATTACHMENT || att == GL_DEPTH_STENCIL_ATTACHMENT) { f->depth_rbo = rbo; f->depth_tex = 0; }
  b.rt_valid = false;
}
static GLenum CheckFramebufferStatus(GLenum) { return GL_FRAMEBUFFER_COMPLETE; }
static void GetFramebufferAttachmentParameteriv(GLenum t, GLenum att, GLenum pn, GLint* v) {
  *v = 0;
  const GLuint id = (t == GL_READ_FRAMEBUFFER) ? g.read_fbo : g.draw_fbo;
  Fbo* f = s_fbos.Get(id);
  if (f && pn == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME && att == GL_COLOR_ATTACHMENT0) *v = (GLint)f->color;
}

static void Transfer(bool before) {
  if (before) dkCmdBufBarrier(s_cmd, DkBarrier_Fragments, 0);
  else s_xfer_barrier = true;
  if (!before) { b.rt_valid = false; }
}

static void BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                            GLbitfield mask, GLenum filter) {
  uint32_t flags = filter == GL_LINEAR ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest;
  if ((sx1 < sx0) != (dx1 < dx0)) flags |= DkBlitFlag_FlipX;
  if ((sy1 < sy0) != (dy1 < dy0)) flags |= DkBlitFlag_FlipY;
  auto rect = [](GLint a0, GLint b0, GLint a1, GLint b1) {
    return DkImageRect{(uint32_t)std::min(a0, a1), (uint32_t)std::min(b0, b1), 0, (uint32_t)std::abs(a1 - a0),
                       (uint32_t)std::abs(b1 - b0), 1};
  };
  DkImageRect sr = rect(sx0, sy0, sx1, sy1), dr = rect(dx0, dy0, dx1, dy1);
  if (!sr.width || !sr.height || !dr.width || !dr.height) return;
  Transfer(true);
  if (mask & GL_COLOR_BUFFER_BIT) {
    Tex* s = FboColor(g.read_fbo);
    Tex* d = FboColor(g.draw_fbo);
    if (s && d && s->ok && d->ok) {
      if (sr.width == dr.width && sr.height == dr.height && !(flags & (DkBlitFlag_FlipX | DkBlitFlag_FlipY)))
        dkCmdBufCopyImage(s_cmd, &s->view, &sr, &d->view, &dr, 0);
      else
        dkCmdBufBlitImage(s_cmd, &s->view, &sr, &d->view, &dr, flags, 0);
    }
  }
  if (mask & GL_DEPTH_BUFFER_BIT) {
    Tex* s = FboDepth(g.read_fbo);
    Tex* d = FboDepth(g.draw_fbo);
    if (s && d && s->ok && d->ok) {
      if (sr.width == dr.width && sr.height == dr.height) dkCmdBufCopyImage(s_cmd, &s->view, &sr, &d->view, &dr, 0);
      // con escala (cambio de resolución interna): el motor 2D copia Z24S8 como píxeles de 32 bits, sin filtrar
      else dkCmdBufBlitImage(s_cmd, &s->view, &sr, &d->view, &dr, (flags & ~DkBlitFlag_Filter_Mask) | DkBlitFlag_FilterNearest, 0);
    }
  }
  Transfer(false);
}
static void CopyTexSubImage2D(GLenum target, GLint, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
  Tex* d = BoundTex(target);
  Tex* s = FboColor(g.read_fbo);
  if (!d || !s || !d->ok || !s->ok || w <= 0 || h <= 0) return;
  Transfer(true);
  DkImageRect sr = {(uint32_t)x, (uint32_t)y, 0, (uint32_t)w, (uint32_t)h, 1};
  DkImageRect dr = {(uint32_t)xo, (uint32_t)yo, 0, (uint32_t)w, (uint32_t)h, 1};
  dkCmdBufCopyImage(s_cmd, &s->view, &sr, &d->view, &dr, 0);
  Transfer(false);
}
static void ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum, void* out) {
  Tex* s = FboColor(g.read_fbo);
  if (!s || !s->ok || w <= 0 || h <= 0) return;
  const uint32_t sbpp = s->fmt == DkImageFormat_RGBA32_Uint ? 16 : 4;
  const uint32_t row = (uint32_t)w * sbpp;
  Alloc rb = s_heaps[kHeapData].Allocate(row * h, 256);
  if (!rb) return;
  Transfer(true);
  DkCopyBuf dst = {rb.gpu, 0, 0};
  DkImageRect r = {(uint32_t)x, (uint32_t)y, 0, (uint32_t)w, (uint32_t)h, 1};
  dkCmdBufCopyImageToBuffer(s_cmd, &s->view, &r, &dst, 0);
  Transfer(false);
  Finish();
  uint8_t* dstp = (uint8_t*)out;
  if (Buf* pb = s_bufs.Get(g.pack_buf)) dstp = pb->mem ? pb->mem.cpu + (uintptr_t)out : nullptr;
  if (dstp) {
    const uint32_t bpp = sbpp == 16 ? 16 : fmt == GL_RGB ? 3 : 4;
    const uint32_t orow = ((uint32_t)w * bpp + g.pack_align - 1) & ~(uint32_t)(g.pack_align - 1);
    for (int r2 = 0; r2 < h; ++r2) {
      const uint8_t* sp = rb.cpu + (size_t)r2 * row;
      uint8_t* dp = dstp + (size_t)r2 * orow;
      if (bpp == 4 || bpp == 16) std::memcpy(dp, sp, row);
      else for (int i = 0; i < w; ++i) { dp[i * 3] = sp[i * 4]; dp[i * 3 + 1] = sp[i * 4 + 1]; dp[i * 3 + 2] = sp[i * 4 + 2]; }
    }
  }
  s_heaps[kHeapData].Free(rb);
}

// ---- estado fijo
static bool* Cap(GLenum c) {
  switch (c) {
    case GL_BLEND: return &g.blend;
    case GL_DEPTH_TEST: return &g.depth_test;
    case GL_CULL_FACE: return &g.cull;
    case GL_SCISSOR_TEST: return &g.scissor;
    case GL_COLOR_LOGIC_OP: return &g.logic;
    case GL_DEPTH_CLAMP: return &g.depth_clamp;
    case GL_RASTERIZER_DISCARD: return &g.raster_discard;
    default: return nullptr;
  }
}
static void Enable(GLenum c) { if (bool* p = Cap(c)) *p = true; }
static void Disable(GLenum c) { if (bool* p = Cap(c)) *p = false; }
static GLboolean IsEnabled(GLenum c) { bool* p = Cap(c); return p && *p; }
static void BlendEquationSeparate(GLenum r, GLenum a) { g.blend_eq_rgb = r; g.blend_eq_a = a; }
static void BlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) { g.bsrc_rgb = sr; g.bdst_rgb = dr; g.bsrc_a = sa; g.bdst_a = da; }
static void DepthFunc(GLenum f) { g.depth_func = f; }
static void DepthMask(GLboolean m) { g.depth_mask = m; }
static void DepthRange(GLdouble n, GLdouble f) { g.dr[0] = n; g.dr[1] = f; }
static void ColorMask(GLboolean r, GLboolean gg, GLboolean bb, GLboolean a) { g.cmask[0] = r; g.cmask[1] = gg; g.cmask[2] = bb; g.cmask[3] = a; }
static void LogicOp(GLenum op) { g.logic_op = op; }
static void CullFace(GLenum f) { g.cull_face = f; }
static void FrontFace(GLenum f) { g.front_face = f; }
static void Viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  g.vp[0] = x; g.vp[1] = y; g.vp[2] = w; g.vp[3] = h;
  g.vpf[0] = (float)x; g.vpf[1] = (float)y; g.vpf[2] = (float)w; g.vpf[3] = (float)h;
}
static void ViewportIndexedf(GLuint i, GLfloat x, GLfloat y, GLfloat w, GLfloat h) {
  if (i) return;
  g.vpf[0] = x; g.vpf[1] = y; g.vpf[2] = w; g.vpf[3] = h;
  g.vp[0] = (GLint)x; g.vp[1] = (GLint)y; g.vp[2] = (GLint)w; g.vp[3] = (GLint)h;
}
static void Scissor(GLint x, GLint y, GLsizei w, GLsizei h) { g.sc[0] = x; g.sc[1] = y; g.sc[2] = w; g.sc[3] = h; }
static void ClearColor(GLfloat r, GLfloat gg, GLfloat bb, GLfloat a) { g.clear_color[0] = r; g.clear_color[1] = gg; g.clear_color[2] = bb; g.clear_color[3] = a; }
static void ClearDepth(GLdouble d) { g.clear_depth = d; }
static void Clear(GLbitfield mask) {
  BindTargets();
  ApplyFixedState();  // el borrado respeta el scissor (ClearBufferFlags::Scissor en deko3d)
  if ((mask & GL_COLOR_BUFFER_BIT) && b.rt_color) {
    const uint32_t cm = (g.cmask[0] ? DkColorMask_R : 0) | (g.cmask[1] ? DkColorMask_G : 0) |
                        (g.cmask[2] ? DkColorMask_B : 0) | (g.cmask[3] ? DkColorMask_A : 0);
    dkCmdBufClearColor(s_cmd, 0, cm, g.clear_color);
  }
  if ((mask & GL_DEPTH_BUFFER_BIT) && g.depth_mask && b.rt_depth)
    dkCmdBufClearDepthStencil(s_cmd, true, (float)g.clear_depth, 0, 0);
}

// ---- draws
static uint64_t s_draws = 0;
struct DrawTimer {
  uint64_t t0 = armGetSystemTick();
  ~DrawTimer() { s_st_draw_ticks += armGetSystemTick() - t0; }
};
static void DrawArrays(GLenum mode, GLint first, GLsizei count) {
  DrawTimer dt;
  if (count <= 0 || !PrepareDraw()) return;
  dkCmdBufDraw(s_cmd, Prim(mode), (uint32_t)count, 1, (uint32_t)first, 0);
  ++s_draws;
}
static void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* off, GLint base) {
  DrawTimer dt;
  if (count <= 0 || !PrepareDraw()) return;
  Buf* ib = s_bufs.Get(CurVao()->ebo);
  if (!ib || !ib->mem) return;
  const DkIdxFormat f = type == GL_UNSIGNED_INT ? DkIdxFormat_Uint32 : type == GL_UNSIGNED_SHORT ? DkIdxFormat_Uint16 : DkIdxFormat_Uint8;
  dkCmdBufBindIdxBuffer(s_cmd, f, ib->mem.gpu + (uintptr_t)off);
  dkCmdBufDrawIndexed(s_cmd, Prim(mode), (uint32_t)count, 1, 0, base, 0);
  ++s_draws;
}
static void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* off) { DrawElementsBaseVertex(mode, count, type, off, 0); }
static void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* off, GLsizei inst, GLuint base) {
  if (count <= 0 || inst <= 0 || !PrepareDraw()) return;
  Buf* ib = s_bufs.Get(CurVao()->ebo);
  if (!ib || !ib->mem) return;
  const DkIdxFormat f = type == GL_UNSIGNED_INT ? DkIdxFormat_Uint32 : type == GL_UNSIGNED_SHORT ? DkIdxFormat_Uint16 : DkIdxFormat_Uint8;
  dkCmdBufBindIdxBuffer(s_cmd, f, ib->mem.gpu + (uintptr_t)off);
  dkCmdBufDrawIndexed(s_cmd, Prim(mode), (uint32_t)count, (uint32_t)inst, 0, 0, base);
  ++s_draws;
}
static void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei inst, GLuint base) {
  if (count <= 0 || inst <= 0 || !PrepareDraw()) return;
  dkCmdBufDraw(s_cmd, Prim(mode), (uint32_t)count, (uint32_t)inst, (uint32_t)first, base);
  ++s_draws;
}

// ---- sincronización
static GLsync FenceSync(GLenum, GLbitfield) {
  Sync* s = new Sync();
  dkCmdBufSignalFence(s_cmd, &s->fence, true);
  s->seq = s_seq;
  return (GLsync)s;
}
static uint64_t s_last_submit_count = 0;
static GLenum ClientWaitSync(GLsync sy, GLbitfield, GLuint64 timeout) {
  Sync* s = (Sync*)sy;
  if (!s) return GL_WAIT_FAILED;
  if (!s->submitted) { Submit(false); s->submitted = true; }
  const DkResult r = dkFenceWait(&s->fence, timeout >= (GLuint64)INT64_MAX ? -1 : (int64_t)timeout);
  return r == DkResult_Success ? GL_CONDITION_SATISFIED : GL_TIMEOUT_EXPIRED;
}
static void DeleteSync(GLsync sy) {
  Sync* s = (Sync*)sy;
  if (!s) return;
  if (!s->submitted) { Submit(false); s->submitted = true; }
  dkFenceWait(&s->fence, -1);  // la valla no puede moverse ni desaparecer pendiente
  delete s;
}
static void GlFinish() { Finish(); }
static void Flush() { Submit(false); }

// ---- consultas
static GLenum GetError() { GLenum e = g.error; g.error = GL_NO_ERROR; return e; }
static const char* const kExt[] = {"GL_ARB_separate_shader_objects", "GL_ARB_buffer_storage", "GL_ARB_gpu_shader5",
                                   "GL_ARB_blend_func_extended", "GL_ARB_texture_buffer_object"};
static const GLubyte* GetString(GLenum n) {
  switch (n) {
    case GL_VENDOR: return (const GLubyte*)"r15dk";
    case GL_RENDERER: return (const GLubyte*)"dkgl (deko3d 0.5.0 + uam) NVIDIA Tegra X1";
    case GL_VERSION: return (const GLubyte*)"4.3 dkgl";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte*)"4.60 (uam)";
    default: return (const GLubyte*)"";
  }
}
static const GLubyte* GetStringi(GLenum n, GLuint i) {
  if (n == GL_EXTENSIONS && i < sizeof(kExt) / sizeof(kExt[0])) return (const GLubyte*)kExt[i];
  return nullptr;
}
static void GetIntegerv(GLenum pn, GLint* v) {
  switch (pn) {
    case GL_MAJOR_VERSION: *v = 4; break;
    case GL_MINOR_VERSION: *v = 3; break;
    case GL_NUM_EXTENSIONS: *v = (GLint)(sizeof(kExt) / sizeof(kExt[0])); break;
    case GL_DRAW_FRAMEBUFFER_BINDING: *v = (GLint)g.draw_fbo; break;
    case GL_READ_FRAMEBUFFER_BINDING: *v = (GLint)g.read_fbo; break;
    case GL_CURRENT_PROGRAM: *v = (GLint)g.prog; break;
    case GL_VIEWPORT: std::memcpy(v, g.vp, 16); break;
    case GL_SCISSOR_BOX: std::memcpy(v, g.sc, 16); break;
    case GL_PACK_ALIGNMENT: *v = g.pack_align; break;
    case GL_UNPACK_ALIGNMENT: *v = g.unpack_align; break;
    case GL_MAX_TEXTURE_BUFFER_SIZE: *v = 1 << 27; break;
    case GL_MAX_TEXTURE_SIZE: *v = 16384; break;
    case GL_NUM_PROGRAM_BINARY_FORMATS: *v = 0; break;
    case GL_ACTIVE_TEXTURE: *v = GL_TEXTURE0 + g.active_unit; break;
    case GL_VERTEX_ARRAY_BINDING: *v = (GLint)g.vao; break;
    case GL_ARRAY_BUFFER_BINDING: *v = (GLint)g.array_buf; break;
    case GL_TEXTURE_BINDING_2D: *v = (GLint)g.tex2d[g.active_unit]; break;
    default: *v = 0; break;
  }
}
static void GetFloatv(GLenum pn, GLfloat* v) {
  if (pn == GL_COLOR_CLEAR_VALUE) std::memcpy(v, g.clear_color, 16);
  else *v = 0;
}
static void GetBooleanv(GLenum pn, GLboolean* v) {
  if (pn == GL_COLOR_WRITEMASK) for (int i = 0; i < 4; ++i) v[i] = g.cmask[i];
  else *v = 0;
}
// consultas de tiempo: no disponibles (resultado 0, siempre "disponible")
static void GenQueries(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = (GLuint)(i + 1); }
static void BeginQuery(GLenum, GLuint) {}
static void EndQuery(GLenum) {}
static void QueryCounter(GLuint, GLenum) {}  // GL_TIMESTAMP (frente RES): sin datos, resultado 0
static void GetQueryObjectiv(GLuint, GLenum pn, GLint* v) { *v = pn == GL_QUERY_RESULT_AVAILABLE ? 1 : 0; }
static void GetQueryObjectui64v(GLuint, GLenum, GLuint64* v) { *v = 0; }
static void TransformFeedbackVaryings(GLuint, GLsizei, const GLchar* const*, GLenum) {}
static void BeginTransformFeedback(GLenum) {}
static void EndTransformFeedback() {}

// ---- funciones que glad carga pero el runtime no usa
static void Unimplemented() {
  static int n = 0;
  if (n++ < 16) Log("llamada a una función GL no implementada en dkgl (ver los avisos de carga)");
}

// ============================================================ tabla de nombres
struct Fn { const char* name; void* fn; };
#define F(n) {"gl" #n, (void*)&n}
static const Fn kFns[] = {
    F(ActiveTexture), F(AttachShader), F(BeginQuery), F(BeginTransformFeedback), F(BindBuffer), F(BindBufferBase),
    F(BindFramebuffer), F(BindProgramPipeline), F(BindRenderbuffer), F(BindSampler), F(BindTexture),
    F(BindVertexArray), F(BlendEquationSeparate), F(BlendFuncSeparate), F(BlitFramebuffer), F(BufferData),
    F(BufferStorage), F(BufferSubData), F(CheckFramebufferStatus), F(Clear), F(ClearColor), F(ClearDepth),
    F(ClientWaitSync), F(ColorMask), F(CompileShader), F(CopyTexSubImage2D), F(CreateProgram), F(CreateShader),
    F(CullFace), F(DeleteBuffers), F(DeleteFramebuffers), F(DeleteProgram), F(DeleteProgramPipelines),
    F(DeleteRenderbuffers), F(DeleteShader), F(DeleteSync), F(DeleteTextures), F(DeleteVertexArrays), F(DepthFunc),
    F(DepthMask), F(DepthRange), F(DetachShader), F(Disable), F(DisableVertexAttribArray), F(DrawArrays),
    F(DrawElements), F(DrawElementsBaseVertex), F(Enable), F(EnableVertexAttribArray), F(EndQuery),
    F(EndTransformFeedback), F(FenceSync), F(Flush), F(FramebufferRenderbuffer), F(FramebufferTexture2D),
    F(FramebufferTextureLayer), F(FrontFace), F(GenBuffers), F(GenFramebuffers), F(GenProgramPipelines),
    F(GenQueries), F(QueryCounter), F(GenRenderbuffers), F(GenSamplers), F(GenTextures), F(GenVertexArrays), F(GetBooleanv),
    F(GetBufferSubData), F(GetError), F(GetFloatv), F(GetFramebufferAttachmentParameteriv), F(GetIntegerv),
    F(GetProgramInfoLog), F(GetProgramiv), F(GetQueryObjectiv), F(GetQueryObjectui64v), F(GetShaderInfoLog),
    F(GetShaderiv), F(GetString), F(GetStringi), F(GetUniformLocation), F(IsEnabled), F(LinkProgram), F(LogicOp),
    F(MapBufferRange), F(PixelStorei), F(ProgramParameteri), F(ProgramUniform1i), F(ProgramUniform1ui),
    F(ProgramUniform1f), F(ProgramUniform1iv), F(ProgramUniform2fv), F(ProgramUniform4fv), F(ProgramUniform4iv),
    F(ProgramUniformMatrix4fv), F(ReadPixels), F(RenderbufferStorage), F(SamplerParameteri), F(Scissor),
    F(ShaderSource), F(TexBuffer), F(TexImage2D), F(TexImage3D), F(TexParameteri), F(TexSubImage2D),
    F(TransformFeedbackVaryings), F(Uniform1i), F(Uniform1f), F(Uniform1ui), F(Uniform1iv), F(Uniform2fv),
    F(Uniform3i), F(Uniform4fv), F(Uniform4iv), F(UniformMatrix4fv), F(UnmapBuffer), F(UseProgram),
    F(UseProgramStages), F(ViewportIndexedf), F(VertexAttribPointer), F(VertexAttribIPointer), F(VertexAttribDivisor), F(Viewport),
    F(DrawElementsInstancedBaseInstance), F(DrawArraysInstancedBaseInstance),
    {"glFinish", (void*)&GlFinish},
};
#undef F

void* GetProcAddress(const char* name) {
  for (const auto& f : kFns)
    if (!std::strcmp(f.name, name)) return f.fn;
  // glGetProgramBinary/glProgramBinary: nullptr (la caché de binarios GL se apaga; dkgl tiene la suya de DKSH)
  if (!std::strcmp(name, "glGetProgramBinary") || !std::strcmp(name, "glProgramBinary")) return nullptr;
  // sin multidraw en deko3d: el renderer lo da por no disponible
  if (!std::strcmp(name, "glMultiDrawElementsIndirect")) return nullptr;
  static int n = 0;
  if (n++ < 400) { /* glad pide todas las de GL 3.3: solo se avisa si se llaman */ }
  return (void*)&Unimplemented;
}

// ============================================================ inicio, presentación
bool Active() { return s_active; }

static void InitHeaps() {
  s_heaps[kHeapData] = Heap{DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 64u << 20, kHeapData, {}};
  s_heaps[kHeapImage] = Heap{DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 64u << 20, kHeapImage, {}};
  s_heaps[kHeapCode] = Heap{DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, 16u << 20, kHeapCode, {}};
  s_cmdring.chunk = 1u << 20;
  s_dataring.chunk = 2u << 20;
}

bool Init() {
  if (s_active) return true;
  DkDeviceMaker dm;
  dkDeviceMakerDefaults(&dm);
  dm.cbDebug = DebugCb;
  dm.flags = DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginLowerLeft;
  s_dev = dkDeviceCreate(&dm);
  if (!s_dev) { Log("dkDeviceCreate falló"); return false; }
  InitHeaps();
  s_imgdesc = s_heaps[kHeapData].Allocate(kMaxImages * sizeof(DkImageDescriptor), DK_IMAGE_DESCRIPTOR_ALIGNMENT);
  s_smpdesc = s_heaps[kHeapData].Allocate(kMaxSamplers * sizeof(DkSamplerDescriptor), DK_SAMPLER_DESCRIPTOR_ALIGNMENT);
  std::memset(s_imgdesc.cpu, 0, kMaxImages * sizeof(DkImageDescriptor));
  s_zero = s_heaps[kHeapData].Allocate(256, 256);
  std::memset(s_zero.cpu, 0, 256);
  DkCmdBufMaker cm;
  dkCmdBufMakerDefaults(&cm, s_dev);
  cm.cbAddMem = CmdAddMem;
  s_cmd = dkCmdBufCreate(&cm);
  CmdAddMem(nullptr, s_cmd, 0);
  DkQueueMaker qm;
  dkQueueMakerDefaults(&qm, s_dev);
  qm.flags = DkQueueFlags_Graphics;
  qm.commandMemorySize = 4u << 20;
  qm.flushThreshold = 1u << 20;
  s_q = dkQueueCreate(&qm);
  if (!s_q) { Log("dkQueueCreate falló"); return false; }
  {
    const char* e = std::getenv("NWII_DK_FB");
    const bool big = e ? std::atoi(e) == 1080 : appletGetOperationMode() == AppletOperationMode_Console;
    kFbW = big ? 1920 : 1280;
    kFbH = big ? 1080 : 720;
  }
  DkImage const* imgs[kFbN];
  for (int i = 0; i < kFbN; ++i) {
    MakeImage(s_fb[i], kFbW, kFbH, DkImageFormat_RGBA8_Unorm,
              DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression | DkImageFlags_Usage2DEngine);
    imgs[i] = &s_fb[i].img;
  }
  MakeImage(s_fbdepth, kFbW, kFbH, DkImageFormat_Z24S8, DkImageFlags_UsageRender);
  // deko3d trata cualquier error como fatal (applet de error): se comprueba antes que la ventana nativa está
  // libre (nadie le ha configurado buffers: ni EGL ni la consola de texto). dkSwapchainCreate hace lo mismo.
  if (R_FAILED(nwindowSetDimensions(nwindowGetDefault(), kFbW, kFbH))) {
    Log("la ventana nativa ya está en uso (EGL/SDL/consola): no se puede usar deko3d");
    return false;
  }
  DkSwapchainMaker sm;
  dkSwapchainMakerDefaults(&sm, s_dev, nwindowGetDefault(), imgs, kFbN);
  s_swap = dkSwapchainCreate(&sm);
  if (!s_swap) { Log("dkSwapchainCreate falló (¿ventana ocupada por EGL?)"); return false; }
  dkSwapchainSetSwapInterval(s_swap, s_swap_interval);
  // descriptor 0 = 1x1 negro opaco (texturas sin contenido, como una textura GL incompleta)
  {
    s_black.mem = s_heaps[kHeapImage].Allocate(0x10000, 0x10000);
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, s_dev);
    m.format = DkImageFormat_RGBA8_Unorm;
    m.dimensions[0] = 1;
    m.dimensions[1] = 1;
    dkImageLayoutInitialize(&s_black.layout, &m);
    dkImageInitialize(&s_black.img, &s_black.layout, s_black.mem.blk, s_black.mem.off);
    dkImageViewDefaults(&s_black.view, &s_black.img);
    s_black.ok = true;
    WriteImageDesc(0, &s_black.view);
    const uint32_t px = 0xFF000000u;
    Upload(s_black, 0, 0, 1, 1, GL_RGBA, &px, true);
  }
  dkCmdBufSetPointSize(s_cmd, 1.0f);
  dkCmdBufBindImageDescriptorSet(s_cmd, s_imgdesc.gpu, kMaxImages);
  dkCmdBufBindSamplerDescriptorSet(s_cmd, s_smpdesc.gpu, kMaxSamplers);
  DiskInit();
  ReadBarrierModes();
  Finish();
  s_active = true;
  Log("deko3d activo: dispositivo, cola, swapchain %dx%d x%d, memoria (datos 64 MB, imágenes 64 MB, código 16 MB)", kFbW, kFbH, kFbN);
  return true;
}

void SwapBuffers() {
  if (!s_active) return;
  if (s_slot < 0) {
    // nada dibujado en el framebuffer 0 este frame: se presenta en negro
    GLuint save = g.draw_fbo;
    g.draw_fbo = 0;
    BindTargets();
    const float black[4] = {0, 0, 0, 1};
    dkCmdBufClearColor(s_cmd, 0, DkColorMask_RGBA, black);
    g.draw_fbo = save;
  }
  // el swapchain se lee con el motor de presentación: todo lo dibujado tiene que haber terminado
  dkCmdBufBarrier(s_cmd, DkBarrier_Fragments, 0);
  Submit(true);
  dkQueuePresentImage(s_q, s_swap, s_slot);
  s_slot = -1;
  b.rt_valid = false;  // la imagen del framebuffer 0 cambia
  ++s_seq;
  // como mucho 2 frames en vuelo
  PollFences(false);
  while (s_inflight.size() > 2) PollFences(true);
  // el estado de deko3d se mantiene entre listas de la misma cola; los descriptores también
  static uint64_t frames = 0;
  if (++frames % 600 == 0) {
    auto us = [](uint64_t t) { return armTicksToNs(t) / 1000.0 / 600.0; };
    Log("frames %llu, por frame: draws %.0f (%.0f us en dkgl), push %.1f KB, barreras %.1f, cambios de destino %.1f, "
        "envío %.0f us, adquirir imagen %.0f us; uam %d compilaciones (%.0f ms de media)",
        (unsigned long long)frames, s_draws / 600.0, us(s_st_draw_ticks), s_st_push_bytes / 600.0 / 1024.0,
        s_st_barriers / 600.0, s_st_rt / 600.0, us(s_st_submit_ticks), us(s_st_acquire_ticks), s_compiles,
        s_compiles ? s_compile_us / 1000.0 / s_compiles : 0.0);
    s_draws = s_st_draw_ticks = s_st_push_bytes = s_st_barriers = s_st_rt = s_st_submit_ticks = s_st_acquire_ticks = 0;
  }
}

void GetDrawableSize(int* w, int* h) { *w = kFbW; *h = kFbH; }
void SetSwapInterval(int i) { s_swap_interval = i; if (s_swap) dkSwapchainSetSwapInterval(s_swap, i); }
void Shutdown() {
  if (!s_active) return;
  Finish();
  dkQueueWaitIdle(s_q);
  dkSwapchainDestroy(s_swap);
  dkQueueDestroy(s_q);
  dkCmdBufDestroy(s_cmd);
  if (s_diskf) std::fclose(s_diskf);
  s_active = false;
  // los bloques de memoria los libera el sistema al salir
}

}  // namespace dkgl
#endif  // __SWITCH__
