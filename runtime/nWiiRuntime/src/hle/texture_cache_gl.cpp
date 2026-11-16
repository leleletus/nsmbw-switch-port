#include "runtime/nwii_env.h"
#include "runtime/texture_cache_gl.h"
#include <cstdio>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <map>
#include <chrono>
#include <vector>

// GX texture decoding, kept bit-exact with Dolphin's TextureDecoder:




namespace nwii::runtime::hle {

using gx::g_state;

} // namespace nwii::runtime::hle
// [RPROF]: EFB copies stitched into larger textures (animated tiles).
uint64_t g_prof_stitches = 0;
uint64_t g_prof_tex_uploads = 0;  // [RPROF]: textures decoded + uploaded
namespace nwii::runtime::hle {

// r39r2 NWII_R2_COPYFBO=1: one framebuffer object per texture that EFB copies
// are drawn into or read from (copy destinations, their native-size
// downsamples, stitch sources), instead of re-attaching the texture to one
// shared framebuffer for every copy (Mesa 20.1: a new renderbuffer wrapper, a
// new pipe surface and a completeness check each time). Every texture this
// cache deletes drops its framebuffer first (r2_del_tex), so a reused texture
// name never finds a stale one.
static std::unordered_map<GLuint, GLuint> s_r2_fbo;
bool g_r2_off_pass = false;  // r39r2: set by the renderer during the r8 pass of NWII_R9_FRAMECHECK
bool r2_copyfbo_on() {
    if (g_r2_off_pass) return false;
    static const bool on = [] {
        const char* e = std::getenv("NWII_R2_COPYFBO");
        const bool v = e && *e == '1';
        if (v) printf("[R2] NWII_R2_COPYFBO=1: one framebuffer per EFB-copy texture\n");
        return v;
    }();
    return on;
}
// Binds (to `target`) the framebuffer whose colour 0 is `tex`, made on first use.
GLuint r2_tex_fbo(GLuint tex, GLenum target) {
    auto it = s_r2_fbo.find(tex);
    if (it != s_r2_fbo.end()) {
        glBindFramebuffer(target, it->second);
        return it->second;
    }
    GLuint f = 0;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(target, f);
    glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    s_r2_fbo.emplace(tex, f);
    return f;
}
// r39r2 NWII_R2_LAZYNAT=1: the native-size downsample of a scaled EFB copy is
// only read when the copy is stitched into a RAM-decoded texture; the renderer
// used to make it after every scaled copy (~27 a frame in the desert, never
// stitched). With the switch it records here what the blit would be and the
// blit runs only if a stitch reads that downsample, from the same scaled
// texture, which nothing changed meanwhile (a new copy into that texture
// first forgets the record, see r2_native_forget_src).
struct R2LazyNat { GLuint src; uint32_t pw, ph, dw, dh; };
static std::unordered_map<GLuint, R2LazyNat> s_r2_lazy;  // native texture -> pending blit
bool r2_lazynat_on() {
    if (g_r2_off_pass) return false;
    static const bool on = [] {
        const char* e = std::getenv("NWII_R2_LAZYNAT");
        const bool v = e && *e == '1';
        if (v) printf("[R2] NWII_R2_LAZYNAT=1: native downsample of scaled EFB copies on demand\n");
        return v;
    }();
    return on;
}
GLuint r2_tex_fbo(GLuint tex, GLenum target);
// The renderer made a scaled copy into `src` whose downsample would go to `nat`: record it
// (a later copy into the same pair replaces the record, as it replaced the downsample).
void r2_native_defer(GLuint nat, GLuint src, uint32_t pw, uint32_t ph, uint32_t dw, uint32_t dh) {
    for (auto it = s_r2_lazy.begin(); it != s_r2_lazy.end();)
        if (it->second.src == src && it->first != nat) it = s_r2_lazy.erase(it); else ++it;
    s_r2_lazy[nat] = R2LazyNat{src, pw, ph, dw, dh};
}
// The renderer made the downsample into `nat` itself: nothing pending for it.
void r2_native_made(GLuint nat) {
    if (!s_r2_lazy.empty()) s_r2_lazy.erase(nat);
}
// Runs the pending blit into `nat` (if any): the same glBlitFramebuffer the renderer made
// right after the copy (scissor off, colour mask full), then the EFB back as both targets.
static void r2_native_resolve(GLuint nat, GLuint restore_fbo) {
    if (s_r2_lazy.empty()) return;
    auto it = s_r2_lazy.find(nat);
    if (it == s_r2_lazy.end()) return;
    const R2LazyNat z = it->second;
    s_r2_lazy.erase(it);
    const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean cm[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    const bool full = cm[0] && cm[1] && cm[2] && cm[3];
    if (sc) glDisable(GL_SCISSOR_TEST);
    if (!full) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    r2_tex_fbo(z.src, GL_READ_FRAMEBUFFER);
    r2_tex_fbo(nat, GL_DRAW_FRAMEBUFFER);
    glBlitFramebuffer(0, 0, z.pw, z.ph, 0, 0, z.dw, z.dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, restore_fbo);
    if (!full) glColorMask(cm[0], cm[1], cm[2], cm[3]);
    if (sc) glEnable(GL_SCISSOR_TEST);
}
// r39r2 NWII_R2_COPYMRT: framebuffers with several copy textures attached (one
// per draw buffer), keyed by the texture list; a deleted texture deletes every
// such framebuffer that holds it.
static std::map<std::vector<GLuint>, GLuint> s_r2_mrt;
GLuint r2_mrt_fbo(const GLuint* tex, int n) {
    std::vector<GLuint> key(tex, tex + n);
    auto it = s_r2_mrt.find(key);
    if (it != s_r2_mrt.end()) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, it->second);
        return it->second;
    }
    GLuint f = 0;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f);
    GLenum bufs[8];
    for (int i = 0; i < n; ++i) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, tex[i], 0);
        bufs[i] = GL_COLOR_ATTACHMENT0 + i;
    }
    glDrawBuffers(n, bufs);
    if (s_r2_mrt.size() > 512) {  // bounded: forget them all (rare)
        for (auto& kv : s_r2_mrt) glDeleteFramebuffers(1, &kv.second);
        s_r2_mrt.clear();
    }
    s_r2_mrt.emplace(std::move(key), f);
    return f;
}
static void r2_del_tex(GLsizei n, const GLuint* t) {
    if (!s_r2_mrt.empty())
        for (auto it = s_r2_mrt.begin(); it != s_r2_mrt.end();) {
            bool hit = false;
            for (GLsizei i = 0; i < n && !hit; ++i)
                for (GLuint x : it->first) if (x == t[i]) { hit = true; break; }
            if (hit) { glDeleteFramebuffers(1, &it->second); it = s_r2_mrt.erase(it); } else ++it;
        }
    if (!s_r2_lazy.empty())
        for (GLsizei i = 0; i < n; ++i)
            for (auto it = s_r2_lazy.begin(); it != s_r2_lazy.end();)
                if (it->first == t[i] || it->second.src == t[i]) it = s_r2_lazy.erase(it); else ++it;
    if (!s_r2_fbo.empty())
        for (GLsizei i = 0; i < n; ++i) {
            auto it = s_r2_fbo.find(t[i]);
            if (it == s_r2_fbo.end()) continue;
            glDeleteFramebuffers(1, &it->second);  // never bound here (copies rebind the EFB)
            s_r2_fbo.erase(it);
        }
    glDeleteTextures(n, t);
}

TextureCache& TextureCache::get() {
    static TextureCache instance;
    return instance;
}

// Every bind the cache makes goes to its own unit. Binding on whatever unit
// happened to be active (as it used to) replaced a texture of the batch the
// renderer had built but not drawn yet: that batch was then drawn with the
// texture just uploaded -- e.g. the coin-sparkle star stretched over the
// full-screen quad of the next layer, for one frame, whenever a new effect
// texture was decoded.
void TextureCache::bind_scratch(GLuint tex) {
    // NWII_TEXBIND_OLD=1: old behaviour (bind on the active unit), only to
    // reproduce the glitch in A/B tests.
    static const bool old_bind = NWII_ENV("NWII_TEXBIND_OLD") != nullptr;
    if (!old_bind) glActiveTexture(GL_TEXTURE0 + kScratchUnit);
    glBindTexture(GL_TEXTURE_2D, tex);
    ++gl_touches_;
}

void TextureCache::index_erase(const std::pair<const TextureKey, Entry>* kv) {
    auto r = by_addr_.equal_range(kv->first.addr);
    for (auto it = r.first; it != r.second; ++it)
        if (it->second == kv) {
            by_addr_.erase(it);
            return;
        }
}

void TextureCache::clear() {
    for (auto& pair : cache)
        r2_del_tex(1, &pair.second.tex);
    cache.clear();
    by_addr_.clear();
    // EFB copies survive: the renderer refills them every copy.
}

// 5/6/4/3-bit channel expansion, matching Dolphin's ConvertNTo8 tables.
static inline uint8_t c5to8(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t c6to8(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static inline uint8_t c4to8(uint32_t v) { return (uint8_t)((v << 4) | v); }
static inline uint8_t c3to8(uint32_t v) { return (uint8_t)((v << 5) | (v << 2) | (v >> 1)); }

uint32_t texture_data_size(uint32_t width, uint32_t height, uint32_t format) {
    uint32_t tw, th, bpp;
    switch ((TextureFormat)format) {
        case TextureFormat::I4:     tw = 8; th = 8; bpp = 4;  break;
        case TextureFormat::I8:     tw = 8; th = 4; bpp = 8;  break;
        case TextureFormat::IA4:    tw = 8; th = 4; bpp = 8;  break;
        case TextureFormat::IA8:    tw = 4; th = 4; bpp = 16; break;
        case TextureFormat::RGB565: tw = 4; th = 4; bpp = 16; break;
        case TextureFormat::RGB5A3: tw = 4; th = 4; bpp = 16; break;
        case TextureFormat::RGBA8:  tw = 4; th = 4; bpp = 32; break;
        case TextureFormat::C4:     tw = 8; th = 8; bpp = 4;  break;
        case TextureFormat::C8:     tw = 8; th = 4; bpp = 8;  break;
        case TextureFormat::C14X2:  tw = 4; th = 4; bpp = 16; break;
        case TextureFormat::CMPR:   tw = 8; th = 8; bpp = 4;  break;
        default:                    tw = 4; th = 4; bpp = 16; break;
    }
    uint32_t wt = (width + tw - 1) / tw;
    uint32_t ht = (height + th - 1) / th;
    return wt * ht * tw * th * bpp / 8;
}

// Block geometry of a GX texture format: block width/height in texels and
// bytes per block. RGBA8 blocks are 64 bytes (AR and GB halves).
static void block_dims(uint32_t format, uint32_t& bw, uint32_t& bh, uint32_t& bytes) {
    switch ((TextureFormat)format) {
        case TextureFormat::I4: case TextureFormat::C4: case TextureFormat::CMPR:
            bw = 8; bh = 8; bytes = 32; break;
        case TextureFormat::I8: case TextureFormat::IA4: case TextureFormat::C8:
            bw = 8; bh = 4; bytes = 32; break;
        case TextureFormat::RGBA8:
            bw = 4; bh = 4; bytes = 64; break;
        default:
            bw = 4; bh = 4; bytes = 32; break;
    }
}

// Texture format an EFB copy format encodes into (Dolphin EFBCopyFormat ->
// TextureFormat): R4 -> I4, R8/A8/G8/B8 -> I8, RA4 -> IA4, RA8/RG8/GB8 -> IA8.
static uint32_t copy_tex_format(uint32_t copy_fmt) {
    switch (copy_fmt) {
        case 0: return 0;                          // R4
        case 1: case 7: case 8: case 9: case 10:   // R8, A8, R8, G8, B8
            return 1;
        case 2: return 2;                          // RA4
        case 3: case 11: case 12: return 3;        // RA8, RG8, GB8
        case 4: return 4;
        case 5: return 5;
        case 6: return 6;
        default: return 6;
    }
}

// R14 R5 counters ([TCSTAT] every 600 frames): hash_ram calls and sampled
// words, by caller; EFB copies and the cache entries they examined.
static uint64_t st_hash_tex = 0, st_hash_efb_get = 0, st_hash_efb_done = 0, st_hash_words = 0,
                st_efb_done = 0, st_efb_scan = 0, st_frames = 0;
// r31t (frente T): cuantas veces por fotograma se crea o redimensiona la textura de una
// copia EFB (cada una es un bloque nuevo del driver: ioctl al nucleo + esperar a la GPU al
// soltar el viejo), y por que.
static uint64_t st_efb_new = 0, st_efb_resize = 0, st_efb_drop = 0, st_nat_new = 0, st_nat_retire = 0,
                st_efb_var_hit = 0;
// NWII_T_EFBVAR (por defecto encendido; =0 apaga): una copia EFB cuyo tamano en pixeles GL cambia (a escala fraccionaria el
// redondeo depende de la posicion del rectangulo copiado) guarda una textura por tamano en
// vez de reasignar la misma. El contenido y el tamano de la textura que se usa son los de
// siempre; solo se evita crear y destruir memoria de la GPU en cada copia.
struct EfbVar { GLuint tex, native; uint32_t gl_w, gl_h, w, h; };
static std::unordered_map<uint32_t, std::vector<EfbVar>> s_efb_var;
static bool efbvar_on() {
    // Encendido por defecto desde t1 (medido en consola: T06 vs T02/T07); NWII_T_EFBVAR=0 lo apaga.
    static const bool on = [] { const char* e = std::getenv("NWII_T_EFBVAR"); return !(e && *e == '0'); }();
    return on;
}
static void efbvar_retire(uint32_t addr, std::vector<GLuint>& retired) {
    auto it = s_efb_var.find(addr);
    if (it == s_efb_var.end()) return;
    for (const EfbVar& v : it->second) {
        retired.push_back(v.tex);
        if (v.native) retired.push_back(v.native);
    }
    s_efb_var.erase(it);
}

// Content hash of guest RAM [addr, addr+size): one word every 32 bytes (one
// every size/4096 for large ranges). Only ever compared with another value of
// this same function. R14 R5: four FNV-1a lanes over consecutive samples
// (same words as before) instead of one serial chain, so the multiplies of
// four samples overlap (the serial chain was ~5 cycles per word on the A57);
// the value differs from the r13 one, which nothing stored keeps.
// r39r2: with NWII_RPHASE, time spent in hash_ram by caller (0 tex, 1 efb_get, 2 efb_done),
// in [TCSTAT3] every 600 frames.
static uint64_t st_r2_hash_t[3] = {0, 0, 0};
static inline uint64_t r2_now() {
#if defined(__SWITCH__)
    uint64_t v; asm volatile("mrs %0, cntpct_el0" : "=r"(v)); return v;
#else
    return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}
static inline bool r2_ht_on() { static const bool on = std::getenv("NWII_RPHASE") != nullptr; return on; }
static uint32_t hash_ram_(CPUContext* ctx, uint32_t addr, uint32_t size);
static inline uint32_t hash_ram(CPUContext* ctx, uint32_t addr, uint32_t size, int who) {
    if (!r2_ht_on()) return hash_ram_(ctx, addr, size);
    const uint64_t t0 = r2_now();
    const uint32_t h = hash_ram_(ctx, addr, size);
    st_r2_hash_t[who] += r2_now() - t0;
    return h;
}
static uint32_t hash_ram_(CPUContext* ctx, uint32_t addr, uint32_t size);
// r39r2 NWII_R2_PREHASH=1: the RAM hashes a frame needs (texel data of the
// textures it samples, destinations of its EFB copies) are computed in the
// render thread's idle time before the frame (after the swap, while no new
// frame waits: ProcessGXFifo -> r2_prehash_idle), for the ranges the previous
// frame used; the frame then takes them instead of hashing inline. Same
// function, same ranges; the RAM is read a little earlier (the render thread
// reads guest RAM at no fixed point of the guest's timeline either way).
struct R2Pre { uint32_t hash; uint64_t frame; };
static std::unordered_map<uint64_t, R2Pre> s_r2_pre;           // (addr << 32 | size) -> hash for frame
static std::vector<std::pair<uint32_t, uint32_t>> s_r2_todo;  // ranges to hash for frame s_r2_todo_frame
static uint64_t s_r2_todo_frame = 0;
static size_t s_r2_todo_i = 0;
static CPUContext* s_r2_ctx = nullptr;
static uint64_t st_r2_pre_hit = 0, st_r2_pre_done = 0, st_r2_pre_chk = 0, st_r2_pre_bad = 0;
static bool r2_prehash_on() {
    static const bool on = [] {
        const char* e = std::getenv("NWII_R2_PREHASH");
        const bool v = e && *e == '1';
        if (v) printf("[R2] NWII_R2_PREHASH=1: RAM hashes of the next frame in the render thread's idle time\n");
        return v;
    }();
    return on;
}
static inline bool r2_pre_take(uint32_t addr, uint32_t size, uint64_t frame, uint32_t& h) {
    if (s_r2_pre.empty()) return false;
    auto it = s_r2_pre.find(((uint64_t)addr << 32) | size);
    if (it == s_r2_pre.end() || it->second.frame != frame) return false;
    h = it->second.hash;
    ++st_r2_pre_hit;
    // NWII_R2_PREHASH_CHECK=1: hash inline too and count differences (RAM written
    // between the idle hash and this use, or a wrong range).
    static const bool chk = std::getenv("NWII_R2_PREHASH_CHECK") != nullptr;
    if (chk && s_r2_ctx) {
        ++st_r2_pre_chk;
        if (hash_ram_(s_r2_ctx, addr, size) != h) ++st_r2_pre_bad;
    }
    return true;
}
// Called by the render thread while it has nothing to draw; stops as soon as
// more() says a frame is waiting. Returns true when all is done.
bool r2_prehash_pending() { return s_r2_todo_i < s_r2_todo.size(); }
bool r2_prehash_idle(bool (*more)()) {
    if (!s_r2_ctx || s_r2_todo_i >= s_r2_todo.size()) return true;
    while (s_r2_todo_i < s_r2_todo.size()) {
        const auto r = s_r2_todo[s_r2_todo_i++];
        s_r2_pre[((uint64_t)r.first << 32) | r.second] = R2Pre{hash_ram_(s_r2_ctx, r.first, r.second), s_r2_todo_frame};
        ++st_r2_pre_done;
        if (more()) return false;  // (one hash is at most ~0.1 ms on the Switch)
    }
    return true;
}
static uint32_t hash_ram_(CPUContext* ctx, uint32_t addr, uint32_t size) {
    uint32_t hash = 2166136261u;
    if (!ctx || !size) return hash;
    const uint8_t* p = ctx->mmu.get_ptr(addr);
    if (!p) return hash;
    const uint32_t stride = size > 32 * 4096 ? (size / 4096) & ~3u : 32;
    uint32_t h0 = 2166136261u, h1 = 0x01000193u, h2 = 0x9E3779B9u, h3 = 0x85EBCA6Bu;
    uint32_t off = 0;
    uint64_t words = 0;
    // r39r2 NWII_R2_HASHPF=1: prefetch the samples 4 iterations ahead (same
    // words, same order, same value). On the Switch each sample of a big range
    // is its own cache line (DRAM latency): ~1 ms a frame in the desert.
    static const bool s_pf = [] { const char* e = std::getenv("NWII_R2_HASHPF"); return e && *e == '1'; }();
    if (s_pf) {
        const uint32_t ahead = 16 * stride;
        for (; off + 3 * stride + 4 <= size; off += 4 * stride) {
            const uint8_t* q = p + off + ahead;
            __builtin_prefetch(q);
            __builtin_prefetch(q + stride);
            __builtin_prefetch(q + 2 * stride);
            __builtin_prefetch(q + 3 * stride);
            uint32_t w0, w1, w2, w3;
            std::memcpy(&w0, p + off, 4);
            std::memcpy(&w1, p + off + stride, 4);
            std::memcpy(&w2, p + off + 2 * stride, 4);
            std::memcpy(&w3, p + off + 3 * stride, 4);
            h0 = (h0 ^ w0) * 16777619u;
            h1 = (h1 ^ w1) * 16777619u;
            h2 = (h2 ^ w2) * 16777619u;
            h3 = (h3 ^ w3) * 16777619u;
            words += 4;
        }
    }
    for (; off + 3 * stride + 4 <= size; off += 4 * stride) {
        uint32_t w0, w1, w2, w3;
        std::memcpy(&w0, p + off, 4);
        std::memcpy(&w1, p + off + stride, 4);
        std::memcpy(&w2, p + off + 2 * stride, 4);
        std::memcpy(&w3, p + off + 3 * stride, 4);
        h0 = (h0 ^ w0) * 16777619u;
        h1 = (h1 ^ w1) * 16777619u;
        h2 = (h2 ^ w2) * 16777619u;
        h3 = (h3 ^ w3) * 16777619u;
        words += 4;
    }
    for (; off + 4 <= size; off += stride) {
        uint32_t w;
        std::memcpy(&w, p + off, 4);
        h0 = (h0 ^ w) * 16777619u;
        ++words;
    }
    st_hash_words += words;
    hash = (hash ^ h0) * 16777619u;
    hash = (hash ^ h1) * 16777619u;
    hash = (hash ^ h2) * 16777619u;
    hash = (hash ^ h3) * 16777619u;
    return hash;
}

GLuint TextureCache::get_texture(CPUContext& ctx, const gx::TexStage& stage) {
    const uint32_t addr = stage.base_addr & 0x1FFFFFFF;
    const uint32_t size = texture_data_size(stage.width, stage.height, stage.format);

    // An EFB copy to exactly this address and size is the texture -- unless
    // the CPU has since put new data there (Dolphin compares the RAM hash).
    auto ec = efb_copies.find(addr);
    if (ec != efb_copies.end() && ec->second.w == stage.width &&
        ec->second.h == stage.height) {
        if (ec->second.frame == frame_)
            return ec->second.tex;
        // R14 R5: memoised for the frame, as the texel hash below.
        EfbCopy& cp = ec->second;
        if (cp.seen_frame != frame_) {
            cp.seen_hash = hash_ram(&ctx, addr, cp.bytes, 1);
            cp.seen_frame = frame_;
            ++st_hash_efb_get;
        }
        if (cp.seen_hash == cp.ram_hash)
            return ec->second.tex;
        // A batch not yet drawn may still sample it: delete at frame end.
        retired_.push_back(ec->second.tex);
        if (ec->second.native) retired_.push_back(ec->second.native);
        efbvar_retire(addr, retired_);
        ++st_efb_drop;
        efb_copies.erase(ec);
    }

    TextureKey key = {addr, stage.width, stage.height, stage.format,
                      stage.tlut_offset, stage.tlut_format};
    auto ins = cache.try_emplace(key);
    Entry& e = ins.first->second;
    if (ins.second) by_addr_.emplace(addr, &*ins.first);

    // Texel hash, memoised for the frame (the 8 maps are looked up per batch).
    if (e.data_frame != frame_) {
        uint32_t ph;
        if (r2_prehash_on() && r2_pre_take(addr, size, frame_, ph)) e.data_hash = ph;  // r39r2
        else e.data_hash = hash_ram(&ctx, addr, size, 0);
        s_r2_ctx = &ctx;
        e.data_frame = frame_;
        ++st_hash_tex;
    }
    uint32_t hash = e.data_hash;

    // Paletted formats: the palette is part of the content (Dolphin XORs a
    // TLUT hash in). TLUT loads happen between draws, so it is not memoised.
    uint32_t palette_entries = 0;
    if (stage.format == 8) palette_entries = 16;
    else if (stage.format == 9) palette_entries = 256;
    else if (stage.format == 10) palette_entries = 16384;
    if (palette_entries) {
        uint32_t tl_bytes = palette_entries * 2;
        uint32_t tl_off = stage.tlut_offset;
        if (tl_off < sizeof(gx::g_state.tlutMem)) {
            uint32_t avail = (uint32_t)sizeof(gx::g_state.tlutMem) - tl_off;
            if (tl_bytes > avail) tl_bytes = avail;
            const uint8_t* tp = gx::g_state.tlutMem + tl_off;
            uint32_t i = 0;
            for (; i + 4 <= tl_bytes; i += 4) {
                uint32_t w;
                std::memcpy(&w, tp + i, 4);
                hash = (hash ^ w) * 16777619u;
            }
            for (; i < tl_bytes; ++i)
                hash = (hash ^ tp[i]) * 16777619u;
        }
    }

    e.last_used = frame_;
    if (e.tex && e.hash == hash)
        return e.tex;

    // New or changed content. A changed texture gets a fresh GL object: the
    // renderer may still have a pending batch that samples the old content
    // (palette swaps between draws); the old one is deleted at frame end.
    ++g_prof_tex_uploads;
    Image img = decode_texture(ctx, stage);
    if (e.tex)
        retired_.push_back(e.tex);
    glGenTextures(1, &e.tex);
    bind_scratch(e.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, stage.width, stage.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, img.data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    UnloadImage(img);
    e.hash = hash;
    e.size = size;
    if (size > max_size_) max_size_ = size;
    stitch_into(e.tex, stage, addr, size);
    return e.tex;
}

TextureCache::MemStats TextureCache::mem_stats() const {
    MemStats m;
    for (const auto& kv : cache)
        if (kv.second.tex) {
            ++m.tex;
            m.tex_bytes += (size_t)kv.first.width * kv.first.height * 4;
        }
    for (const auto& kv : efb_copies) {
        const EfbCopy& c = kv.second;
        if (c.tex) { ++m.efb; m.efb_bytes += (size_t)c.gl_w * c.gl_h * 4; }
        if (c.native) m.efb_bytes += (size_t)c.w * c.h * 4;
    }
    m.retired = retired_.size();
    return m;
}

void TextureCache::end_frame() {
    if (r2_prehash_on()) {  // r39r2: the ranges this frame hashed, to hash for the next one
        s_r2_todo.clear();
        s_r2_todo_i = 0;
        for (const auto& kv : cache)
            if (kv.second.data_frame == frame_ && kv.second.size)
                s_r2_todo.emplace_back(kv.first.addr, kv.second.size);
        for (const auto& kv : efb_copies)
            if (kv.second.frame == frame_ && kv.second.bytes)
                s_r2_todo.emplace_back(kv.first, kv.second.bytes);
        s_r2_todo_frame = frame_ + 1;
        if (s_r2_pre.size() > 4096) s_r2_pre.clear();
    }
    ++frame_;
    if (++st_frames == 600) {
        const double f = (double)st_frames;
        printf("[TCSTAT] per frame: hash_ram tex=%.1f efb_get=%.1f efb_done=%.1f words=%.0f | "
               "efb copies=%.1f scanned=%.1f | entries=%zu efb=%zu max_size=%u\n",
               st_hash_tex / f, st_hash_efb_get / f, st_hash_efb_done / f, st_hash_words / f,
               st_efb_done / f, st_efb_scan / f, cache.size(), efb_copies.size(), max_size_);
        printf("[TCSTAT2] por fotograma: copia EFB nueva=%.2f redimensionada=%.2f (reusada por tamano=%.2f) "
               "descartada por RAM cambiada=%.2f | nativa nueva=%.2f retirada=%.2f | NWII_T_EFBVAR=%d\n",
               st_efb_new / f, st_efb_resize / f, st_efb_var_hit / f, st_efb_drop / f, st_nat_new / f,
               st_nat_retire / f, (int)efbvar_on());
        if (r2_prehash_on())
            printf("[R2PRE] per frame: prehashed=%.1f used=%.1f | check: %llu compared, %llu differ\n",
                   st_r2_pre_done / f, st_r2_pre_hit / f, (unsigned long long)st_r2_pre_chk,
                   (unsigned long long)st_r2_pre_bad);
        st_r2_pre_done = st_r2_pre_hit = 0;
        if (r2_ht_on()) {
#if defined(__SWITCH__)
            const double us = 1.0 / 19.2;
#else
            const double us = 1e-3;
#endif
            printf("[TCSTAT3] hash_ram us/frame: tex=%.0f efb_get=%.0f efb_done=%.0f\n", st_r2_hash_t[0] * us / f,
                   st_r2_hash_t[1] * us / f, st_r2_hash_t[2] * us / f);
            st_r2_hash_t[0] = st_r2_hash_t[1] = st_r2_hash_t[2] = 0;
        }
        st_efb_new = st_efb_resize = st_efb_drop = st_nat_new = st_nat_retire = st_efb_var_hit = 0;
        st_hash_tex = st_hash_efb_get = st_hash_efb_done = st_hash_words = 0;
        st_efb_done = st_efb_scan = st_frames = 0;
    }
    if (!retired_.empty()) {
        r2_del_tex((GLsizei)retired_.size(), retired_.data());
        retired_.clear();
    }
    // Evict textures unused for 10 s worth of frames once the cache is big.
    if (cache.size() > 512) {
        for (auto it = cache.begin(); it != cache.end();) {
            if (it->second.last_used + 600 < frame_) {
                r2_del_tex(1, &it->second.tex);
                index_erase(&*it);
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (efb_copies.size() > 256) {
        for (auto it = efb_copies.begin(); it != efb_copies.end();) {
            if (it->second.frame + 600 < frame_) {
                r2_del_tex(1, &it->second.tex);
                if (it->second.native) r2_del_tex(1, &it->second.native);
                efbvar_retire(it->first, retired_);
                it = efb_copies.erase(it);
            } else {
                ++it;
            }
        }
    }
}

GLuint TextureCache::acquire_efb_copy(uint32_t addr, uint32_t w, uint32_t h, uint32_t gl_w,
                                      uint32_t gl_h) {
    addr &= 0x1FFFFFFF;
    EfbCopy& c = efb_copies[addr];
    const uint32_t gw = gl_w ? gl_w : w, gh = gl_h ? gl_h : h;
    if (c.tex && (c.gl_w != gw || c.gl_h != gh || c.w != w || c.h != h) && efbvar_on()) {
        // r31t: esta direccion recibe copias de varios tamanos (en el desierto, dos por
        // fotograma). Se aparta la textura de este tamano (con su reduccion nativa) y se
        // recupera la del tamano pedido si ya existe.
        std::vector<EfbVar>& v = s_efb_var[addr];
        v.push_back({c.tex, c.native, c.gl_w, c.gl_h, c.w, c.h});
        c.tex = 0;
        c.native = 0;
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].gl_w == gw && v[i].gl_h == gh && v[i].w == w && v[i].h == h) {
                c.tex = v[i].tex;
                c.native = v[i].native;
                c.gl_w = gw;
                c.gl_h = gh;
                c.w = w;
                c.h = h;
                v.erase(v.begin() + (long)i);
                ++st_efb_var_hit;
                // como al crearlas de nuevo: filtros en NEAREST
                bind_scratch(c.tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                if (c.native) {
                    bind_scratch(c.native);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                }
                break;
            }
        if (v.size() > 8) {  // demasiados tamanos distintos: fuera el mas viejo
            retired_.push_back(v.front().tex);
            if (v.front().native) retired_.push_back(v.front().native);
            v.erase(v.begin());
        }
    }
    if (!c.tex || c.gl_w != gw || c.gl_h != gh) {
        if (!c.tex) {
            glGenTextures(1, &c.tex);
            ++st_efb_new;
        } else {
            ++st_efb_resize;
        }
        bind_scratch(c.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, gw, gh, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        c.gl_w = gw;
        c.gl_h = gh;
    }
    if (c.native && (c.w != w || c.h != h)) {
        retired_.push_back(c.native);  // wrong size: a new one on demand
        ++st_nat_retire;
        c.native = 0;
    }
    c.w = w;
    c.h = h;
    return c.tex;
}

GLuint TextureCache::efb_copy_native(uint32_t addr) {
    auto it = efb_copies.find(addr & 0x1FFFFFFF);
    if (it == efb_copies.end() || !it->second.tex) return 0;
    EfbCopy& c = it->second;
    if (!c.native) {
        ++st_nat_new;
        glGenTextures(1, &c.native);
        bind_scratch(c.native);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c.w, c.h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    return c.native;
}

bool TextureCache::efb_copy_stitched(uint32_t addr) const {
    auto it = efb_copies.find(addr & 0x1FFFFFFF);
    return it != efb_copies.end() && it->second.stitched;
}

void TextureCache::efb_copy_done(CPUContext* ctx, uint32_t addr, uint32_t w,
                                 uint32_t h, uint32_t copy_fmt, uint32_t stride) {
    addr &= 0x1FFFFFFF;
    auto it = efb_copies.find(addr);
    if (it == efb_copies.end()) return;
    EfbCopy& c = it->second;
    c.fmt = copy_fmt;
    uint32_t bw, bh, bpb;
    block_dims(copy_tex_format(copy_fmt), bw, bh, bpb);
    const uint32_t row_bytes = ((w + bw - 1) / bw) * bpb;
    c.stride = stride ? stride : row_bytes;
    c.bytes = ((h + bh - 1) / bh - 1) * c.stride + row_bytes;
    uint32_t ph;
    if (r2_prehash_on() && r2_pre_take(addr, c.bytes, frame_, ph)) c.ram_hash = ph;  // r39r2
    else c.ram_hash = hash_ram(ctx, addr, c.bytes, 2);
    c.frame = frame_;
    ++st_hash_efb_done;
    ++st_efb_done;

    // Partial update of a larger texture already decoded from RAM.
    // R14 R5: only the entries starting in [addr - max_size_, addr] can
    // contain addr (was: every entry of the cache, ~420 on the world map, per
    // copy). Same entries, same test; they are visited in address order
    // instead of hash order (each stitch writes its own texture).
    const uint32_t lo = addr > max_size_ ? addr - max_size_ : 0;
    for (auto it = by_addr_.lower_bound(lo); it != by_addr_.end() && it->first <= addr; ++it) {
        const TextureKey& k = it->second->first;
        const Entry& e = it->second->second;
        ++st_efb_scan;
        if (!e.tex || k.addr == addr || addr < k.addr || addr >= k.addr + e.size)
            continue;
        stitch_one(e.tex, k.addr, k.width, k.height, k.format, c, addr);
    }
}

void TextureCache::stitch_into(GLuint tex, const gx::TexStage& stage,
                               uint32_t base, uint32_t size) {
    if (efb_copies.empty()) return;
    for (auto it = efb_copies.lower_bound(base);
         it != efb_copies.end() && it->first < base + size; ++it) {
        if (it->first == base && it->second.w == stage.width &&
            it->second.h == stage.height)
            continue;
        stitch_one(tex, base, stage.width, stage.height, stage.format,
                   it->second, it->first);
    }
}

// Place EFB copy `c` (made to guest address `addr`) inside texture `dst`
// (base, width x height, GX format) by block position.
bool TextureCache::stitch_one(GLuint dst, uint32_t base, uint32_t width,
                              uint32_t height, uint32_t format, const EfbCopy& c,
                              uint32_t addr) {
    uint32_t bw, bh, bpb;
    block_dims(format, bw, bh, bpb);
    uint32_t cbw, cbh, cbpb;
    block_dims(copy_tex_format(c.fmt), cbw, cbh, cbpb);
    if (bw != cbw || bh != cbh || bpb != cbpb) return false;
    const uint32_t bpr = (width + bw - 1) / bw;
    if (c.stride != bpr * bpb) return false;
    const uint32_t off = addr - base;
    if (off % bpb) return false;
    const uint32_t block = off / bpb;
    const uint32_t x = (block % bpr) * bw, y = (block / bpr) * bh;
    if (x >= width || y >= height) return false;
    const uint32_t cw = std::min(c.w, width - x), ch = std::min(c.h, height - y);

    // glCopyTexSubImage2D from a read framebuffer holding the copy: unlike a
    // blit it ignores the scissor test, and it leaves the draw framebuffer
    // alone. No glGet*: each one would stall Mesa's GL thread.
    static GLuint s_rd = 0;
    if (!s_rd) glGenFramebuffers(1, &s_rd);
    // (A scaled copy is read from its native-size downsample.)
    const GLuint src = c.stitch_src();
    if (src) r2_native_resolve(src, restore_read_fbo_);  // r39r2 NWII_R2_LAZYNAT
    if (r2_copyfbo_on()) {  // r39r2
        if (!src) return false;
        r2_tex_fbo(src, GL_READ_FRAMEBUFFER);
    } else {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_rd);
    if (!src) return false;
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    }
    bind_scratch(dst);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 0, 0, cw, ch);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, restore_read_fbo_);
    c.stitched = true;
    ++g_prof_stitches;
    return true;
}

Image TextureCache::decode_texture(CPUContext& ctx, const gx::TexStage& stage) {
    uint32_t width = stage.width, height = stage.height;
    Image img = GenImageColor(width, height, BLANK);
    Color* pixels = (Color*)img.data;
    uint32_t addr = stage.base_addr;

    switch ((TextureFormat)stage.format) {
        case TextureFormat::I4:     decode_i4(ctx, addr, width, height, pixels); break;
        case TextureFormat::I8:     decode_i8(ctx, addr, width, height, pixels); break;
        case TextureFormat::IA4:    decode_ia4(ctx, addr, width, height, pixels); break;
        case TextureFormat::IA8:    decode_ia8(ctx, addr, width, height, pixels); break;
        case TextureFormat::RGB565: decode_rgb565(ctx, addr, width, height, pixels); break;
        case TextureFormat::RGB5A3: decode_rgb5a3(ctx, addr, width, height, pixels); break;
        case TextureFormat::RGBA8:  decode_rgba8(ctx, addr, width, height, pixels); break;
        case TextureFormat::C4:
        case TextureFormat::C8:
        case TextureFormat::C14X2:  decode_paletted(ctx, stage, pixels); break;
        case TextureFormat::CMPR:   decode_cmpr(ctx, addr, width, height, pixels); break;
        default:
            std::cout << "[TextureCache] Unknown texture format: "
                      << (int)stage.format << std::endl;
            for (uint32_t i = 0; i < width * height; i++) pixels[i] = MAGENTA;
            break;
    }

    return img;
}

static uint32_t get_tile_address(uint32_t x, uint32_t y, uint32_t bw, uint32_t bh, uint32_t bpp, uint32_t width) {
    uint32_t tiles_x = (width + bw - 1) / bw;
    uint32_t tx = x / bw;
    uint32_t ty = y / bh;
    uint32_t in_tile_x = x % bw;
    uint32_t in_tile_y = y % bh;

    uint32_t tile_idx = ty * tiles_x + tx;
    uint32_t tile_offset = tile_idx * bw * bh * bpp / 8;
    uint32_t in_tile_offset = (in_tile_y * bw + in_tile_x) * bpp / 8;

    return tile_offset + in_tile_offset;
}

void TextureCache::decode_i4(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x+=2) {
            uint32_t offset = get_tile_address(x, y, 8, 8, 4, width);
            uint8_t b = ctx.mmu.read8(addr + offset);
            uint8_t i1 = c4to8(b >> 4);
            uint8_t i2 = c4to8(b & 0x0F);
            out[y * width + x] = {i1, i1, i1, i1};
            if (x+1 < width) out[y * width + x + 1] = {i2, i2, i2, i2};
        }
    }
}

void TextureCache::decode_i8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t offset = get_tile_address(x, y, 8, 4, 8, width);
            uint8_t i = ctx.mmu.read8(addr + offset);
            out[y * width + x] = {i, i, i, i};
        }
    }
}

void TextureCache::decode_ia4(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t offset = get_tile_address(x, y, 8, 4, 8, width);
            uint8_t b = ctx.mmu.read8(addr + offset);
            uint8_t a = c4to8(b >> 4);
            uint8_t i = c4to8(b & 0x0F);
            out[y * width + x] = {i, i, i, a};
        }
    }
}

void TextureCache::decode_ia8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t offset = get_tile_address(x, y, 4, 4, 16, width);
            uint8_t a = ctx.mmu.read8(addr + offset);
            uint8_t i = ctx.mmu.read8(addr + offset + 1);
            out[y * width + x] = {i, i, i, a};
        }
    }
}

static inline Color unpack_rgb565(uint16_t c) {
    return { c5to8((c >> 11) & 0x1F), c6to8((c >> 5) & 0x3F), c5to8(c & 0x1F), 255 };
}

static inline Color unpack_rgb5a3(uint16_t c) {
    if (c & 0x8000) { 
        return { c5to8((c >> 10) & 0x1F), c5to8((c >> 5) & 0x1F), c5to8(c & 0x1F), 255 };
    }
    return { c4to8((c >> 8) & 0xF), c4to8((c >> 4) & 0xF), c4to8(c & 0xF),
             c3to8((c >> 12) & 0x7) };
}

void TextureCache::decode_rgb565(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t offset = get_tile_address(x, y, 4, 4, 16, width);
            out[y * width + x] = unpack_rgb565(ctx.mmu.read16(addr + offset));
        }
    }
}

void TextureCache::decode_rgb5a3(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t offset = get_tile_address(x, y, 4, 4, 16, width);
            out[y * width + x] = unpack_rgb5a3(ctx.mmu.read16(addr + offset));
        }
    }
}

void TextureCache::decode_rgba8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    uint32_t tiles_x = (width + 3) / 4;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t tx = x / 4;
            uint32_t ty = y / 4;
            uint32_t tile_idx = ty * tiles_x + tx;
            uint32_t tile_offset = tile_idx * 64; 

            uint32_t in_tile_x = x % 4;
            uint32_t in_tile_y = y % 4;
            uint32_t in_tile_offset = (in_tile_y * 4 + in_tile_x) * 2;

            uint16_t ar = ctx.mmu.read16(addr + tile_offset + in_tile_offset);
            uint16_t gb = ctx.mmu.read16(addr + tile_offset + 32 + in_tile_offset);

            out[y * width + x] = {
                (uint8_t)(ar & 0xFF),
                (uint8_t)(gb >> 8),
                (uint8_t)(gb & 0xFF),
                (uint8_t)(ar >> 8)
            };
        }
    }
}

void TextureCache::decode_paletted(CPUContext& ctx, const gx::TexStage& stage, Color* out) {

    uint32_t width = stage.width, height = stage.height, addr = stage.base_addr;
    auto lookup = [&](uint32_t index) -> Color {
        uint32_t off = stage.tlut_offset + index * 2;
        if (off + 1 >= sizeof(g_state.tlutMem)) return MAGENTA;
        uint16_t entry = (g_state.tlutMem[off] << 8) | g_state.tlutMem[off + 1];
        switch (stage.tlut_format) {
            case 0: { 
                uint8_t a = entry >> 8, i = entry & 0xFF;
                return {i, i, i, a};
            }
            case 1: return unpack_rgb565(entry);
            default: return unpack_rgb5a3(entry);
        }
    };

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t index;
            switch ((TextureFormat)stage.format) {
                case TextureFormat::C4: {
                    uint32_t offset = get_tile_address(x & ~1u, y, 8, 8, 4, width);
                    uint8_t b = ctx.mmu.read8(addr + offset);
                    index = (x & 1) ? (b & 0x0F) : (b >> 4);
                    break;
                }
                case TextureFormat::C8: {
                    uint32_t offset = get_tile_address(x, y, 8, 4, 8, width);
                    index = ctx.mmu.read8(addr + offset);
                    break;
                }
                default: { 
                    uint32_t offset = get_tile_address(x, y, 4, 4, 16, width);
                    index = ctx.mmu.read16(addr + offset) & 0x3FFF;
                    break;
                }
            }
            out[y * width + x] = lookup(index);
        }
    }
}

void TextureCache::decode_cmpr(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {

    
    uint32_t tiles_x = (width + 7) / 8;
    for (uint32_t y = 0; y < height; y+=8) {
        for (uint32_t x = 0; x < width; x+=8) {
            uint32_t tx = x / 8;
            uint32_t ty = y / 8;
            uint32_t tile_offset = (ty * tiles_x + tx) * 32;

            for (int by = 0; by < 2; by++) {
                for (int bx = 0; bx < 2; bx++) {
                    uint32_t block_offset = tile_offset + (by * 2 + bx) * 8;
                    uint16_t c0 = ctx.mmu.read16(addr + block_offset);
                    uint16_t c1 = ctx.mmu.read16(addr + block_offset + 2);
                    uint32_t indices = ctx.mmu.read32(addr + block_offset + 4);

                    Color pal[4];
                    pal[0] = unpack_rgb565(c0);
                    pal[1] = unpack_rgb565(c1);
                    if (c0 > c1) {
                        // GX blends 3/8 + 5/8 (Dolphin DXTBlend), not 1/3.
                        auto bl = [](uint8_t a, uint8_t b) { return (uint8_t)((a * 3 + b * 5) >> 3); };
                        pal[2] = { bl(pal[1].r, pal[0].r), bl(pal[1].g, pal[0].g), bl(pal[1].b, pal[0].b), 255 };
                        pal[3] = { bl(pal[0].r, pal[1].r), bl(pal[0].g, pal[1].g), bl(pal[0].b, pal[1].b), 255 };
                    } else {

                        pal[2] = { (uint8_t)((pal[0].r + pal[1].r)/2), (uint8_t)((pal[0].g + pal[1].g)/2), (uint8_t)((pal[0].b + pal[1].b)/2), 255 };
                        pal[3] = { pal[2].r, pal[2].g, pal[2].b, 0 };
                    }

                    for (int py = 0; py < 4; py++) {
                        for (int px = 0; px < 4; px++) {
                            int shift = 30 - 2 * (py * 4 + px); 
                            int idx = (indices >> shift) & 3;
                            uint32_t out_x = x + bx * 4 + px;
                            uint32_t out_y = y + by * 4 + py;
                            if (out_x < width && out_y < height) {
                                out[out_y * width + out_x] = pal[idx];
                            }
                        }
                    }
                }
            }
        }
    }
}

} 
