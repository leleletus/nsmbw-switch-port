#pragma once

#include <glad/glad.h>
#include "runtime/cpu_context.h"
#include "runtime/gx_state.h"

#include <unordered_map>
#include <map>
#include <vector>
#include <cstdint>
#include <cstdlib>

struct Color {
    uint8_t r, g, b, a;
};

struct Image {
    void *data;
    int width;
    int height;
    int mipmaps;
    int format;
};

inline Image GenImageColor(int width, int height, Color color) {
    Image img;
    img.width = width;
    img.height = height;
    img.data = malloc(width * height * sizeof(Color));
    for (int i=0; i<width*height; i++) ((Color*)img.data)[i] = color;
    return img;
}
inline void UnloadImage(Image img) { free(img.data); }

#define BLANK Color{0,0,0,0}
#define MAGENTA Color{255,0,255,255}

namespace nwii::runtime::hle {

enum class TextureFormat {
    I4 = 0, I8 = 1, IA4 = 2, IA8 = 3, RGB565 = 4, RGB5A3 = 5,
    RGBA8 = 6, C4 = 8, C8 = 9, C14X2 = 10, CMPR = 14
};

// Identity of a decoded texture: everything but the content hash. One GL
// texture per identity; a content change re-uploads into it.
struct TextureKey {
    uint32_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t tlut_offset;
    uint32_t tlut_format;

    bool operator==(const TextureKey& other) const {
        return addr == other.addr && width == other.width &&
               height == other.height && format == other.format &&
               tlut_offset == other.tlut_offset &&
               tlut_format == other.tlut_format;
    }
};

struct TextureKeyHash {
    std::size_t operator()(const TextureKey& k) const {
        std::size_t h = k.addr;
        h = h * 31 + k.width;
        h = h * 31 + k.height;
        h = h * 31 + k.format;
        h = h * 31 + k.tlut_offset;
        h = h * 31 + k.tlut_format;
        return h;
    }
};

class TextureCache {
public:
    static TextureCache& get();

    // GL texture for a texture map. EFB copies made to the same address and
    // size win over RAM (guest RAM never receives the copy).
    GLuint get_texture(CPUContext& ctx, const gx::TexStage& stage);

    // EFB copy-to-texture. acquire_efb_copy returns the GL texture (w x h,
    // RGBA8) the renderer must fill for a copy to guest address `addr`;
    // efb_copy_done then records it and stitches it into any decoded texture
    // whose RAM range contains it (NSMBW animates tiles by copying 32x32
    // frames into its 1024x1024 tileset). `copy_fmt` is the real EFB copy
    // format (0-15), `stride` the destination row pitch in bytes (BP 0x4D).
    // Internal resolution: the GL texture is gl_w x gl_h (w x h when 0; about
    // w * scale x h * scale, r16res: any scale); w, h stay the guest (texel)
    // size the cache matches texture maps against.
    GLuint acquire_efb_copy(uint32_t addr, uint32_t w, uint32_t h, uint32_t gl_w = 0,
                            uint32_t gl_h = 0);
    // Scaled copies only: a w x h texture the renderer fills with the copy
    // downsampled to native size, right after the scaled copy. Stitching into
    // a RAM-decoded (native-size) texture copies from it, so stitches never
    // need a scaling blit (which would depend on the renderer's GL state).
    GLuint efb_copy_native(uint32_t addr);
    // r16res: the copy at `addr` has been stitched into a RAM texture (so
    // the renderer makes the next one at native size directly).
    bool efb_copy_stitched(uint32_t addr) const;
    void efb_copy_done(CPUContext* ctx, uint32_t addr, uint32_t w, uint32_t h,
                       uint32_t copy_fmt, uint32_t stride);

    // Called once per rendered frame: per-frame hash memo and eviction.
    void end_frame();

    // Read framebuffer to restore after stitching (the EFB). Stitching binds
    // textures on the active unit.
    void set_restore_read_fbo(GLuint fbo) { restore_read_fbo_ = fbo; }

    void clear();

    // R8 memory: GL textures the cache holds and their RGBA8 bytes (on the
    // Switch, GL texture storage comes out of the same newlib heap).
    struct MemStats {
        size_t tex = 0, tex_bytes = 0, efb = 0, efb_bytes = 0, retired = 0;
    };
    MemStats mem_stats() const;

    // Texture unit the cache binds on for uploads and stitches. The renderer
    // samples on units 0-7 only, so a batch it has built but not yet drawn
    // keeps its bindings. gl_touches() changes whenever the cache switched the
    // active unit (the renderer's state cache must forget it).
    static constexpr int kScratchUnit = 15;
    uint64_t gl_touches() const { return gl_touches_; }

private:
    uint64_t gl_touches_ = 0;
    void bind_scratch(GLuint tex);
    TextureCache() = default;

    struct Entry {
        GLuint tex = 0;
        uint32_t hash = 0;        // texel + TLUT hash of the uploaded content
        uint32_t data_hash = 0;   // texel part, memoised for one frame
        uint64_t data_frame = ~0ull;
        uint64_t last_used = 0;
        uint32_t size = 0;        // bytes of texel data in guest RAM
    };
    struct EfbCopy {
        GLuint tex = 0;
        uint32_t w = 0, h = 0;    // guest texels
        uint32_t gl_w = 0, gl_h = 0;  // size of `tex` (w, h times the scale)
        GLuint native = 0;        // w x h downsample when tex is scaled
        bool scaled() const { return gl_w != w || gl_h != h; }
        GLuint stitch_src() const { return scaled() ? native : tex; }
        uint32_t fmt = 0, stride = 0;
        uint32_t bytes = 0;       // encoded size in guest RAM
        uint32_t ram_hash = 0;    // RAM content when the copy was made
        uint64_t frame = 0;
        // R14 R5: hash_ram of the copy's range, memoised for one frame (as
        // the texel hash of the decoded textures): a copy made in an earlier
        // frame was re-hashed at every lookup (each batch-key rebuild).
        uint32_t seen_hash = 0;
        uint64_t seen_frame = ~0ull;
        mutable bool stitched = false;  // r16res: stitch_one used it
    };

    std::unordered_map<TextureKey, Entry, TextureKeyHash> cache;
    // R14 R5: the cache entries by guest address (element pointers of `cache`
    // are stable until erased), so an EFB copy finds the larger textures it
    // lands inside without walking the whole cache; max_size_: the largest
    // Entry::size so far (a texture starting more than that below the copy
    // cannot contain it).
    std::multimap<uint32_t, const std::pair<const TextureKey, Entry>*> by_addr_;
    uint32_t max_size_ = 0;
    void index_erase(const std::pair<const TextureKey, Entry>* kv);
    std::map<uint32_t, EfbCopy> efb_copies;   // keyed by physical address
    uint64_t frame_ = 1;
    GLuint restore_read_fbo_ = 0;
    std::vector<GLuint> retired_;  // replaced textures, deleted at frame end

    // Copy every EFB copy that lies inside [base, base+size) into `tex`.
    void stitch_into(GLuint tex, const gx::TexStage& stage, uint32_t base,
                     uint32_t size);
    bool stitch_one(GLuint dst, uint32_t base, uint32_t width, uint32_t height,
                    uint32_t format, const EfbCopy& c, uint32_t addr);

    Image decode_texture(CPUContext& ctx, const gx::TexStage& stage);

    void decode_i4(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_i8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_ia4(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_ia8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_rgb565(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_rgb5a3(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_rgba8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_cmpr(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_paletted(CPUContext& ctx, const gx::TexStage& stage, Color* out);
};

uint32_t texture_data_size(uint32_t width, uint32_t height, uint32_t format);

} 
