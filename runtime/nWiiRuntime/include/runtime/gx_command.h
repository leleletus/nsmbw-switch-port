#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <utility>

namespace nwii::runtime::gx {

// R9 render: reference counting for objects only the host thread (FIFO
// drain + renderer) ever touches: the per-draw DrawRaw and the decoded
// vertices, copied with every cached display-list draw of every frame (~11 000
// on the world map, several count updates each). std::shared_ptr makes each
// update an atomic read-modify-write (LDXR/STXR loops on the Switch);
// libstdc++'s single-threaded policy is plain integer arithmetic, same
// ownership semantics otherwise.
#if defined(__GLIBCXX__)
template <class T> using HostPtr = std::__shared_ptr<T, __gnu_cxx::_S_single>;
template <class T, class... A> inline HostPtr<T> MakeHost(A&&... a) {
    return std::__make_shared<T, __gnu_cxx::_S_single>(std::forward<A>(a)...);
}
#else
template <class T> using HostPtr = std::shared_ptr<T>;
template <class T, class... A> inline HostPtr<T> MakeHost(A&&... a) {
    return std::make_shared<T>(std::forward<A>(a)...);
}
#endif

// R9 render: the float payload of an XF command (matrix rows, lights,
// projection, viewport). Most are one 3x4 matrix or less, loaded for nearly
// every draw (~11 000 per frame on the world map): with std::vector each one
// was a heap allocation when parsed or copied from the display-list cache and
// a free when the frame's commands were destroyed. Up to kInline floats live
// inside the command; larger loads go to the heap. Same contents and the
// std::vector members the runtime uses (size/empty/data/[]/begin/end/
// resize, which zero-fills like vector, capacity).
class FloatPayload {
public:
    static constexpr uint32_t kInline = 12;
    FloatPayload() = default;
    FloatPayload(const FloatPayload& o) { assign(o.data(), o.n_); }
    FloatPayload(FloatPayload&& o) noexcept { take(o); }
    FloatPayload& operator=(const FloatPayload& o) {
        if (this != &o) assign(o.data(), o.n_);
        return *this;
    }
    FloatPayload& operator=(FloatPayload&& o) noexcept {
        if (this != &o) {
            release();
            take(o);
        }
        return *this;
    }
    ~FloatPayload() { release(); }

    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    size_t capacity() const { return cap_ > kInline ? cap_ : 0; }  // heap floats
    float* data() { return cap_ > kInline ? u_.heap : u_.inl; }
    const float* data() const { return cap_ > kInline ? u_.heap : u_.inl; }
    float& operator[](size_t i) { return data()[i]; }
    const float& operator[](size_t i) const { return data()[i]; }
    float* begin() { return data(); }
    float* end() { return data() + n_; }
    const float* begin() const { return data(); }
    const float* end() const { return data() + n_; }
    void resize(size_t n) {
        reserve(n);
        float* d = data();
        for (size_t i = n_; i < n; ++i) d[i] = 0.0f;
        n_ = (uint32_t)n;
    }

private:
    void reserve(size_t n) {
        if (n <= (cap_ > kInline ? cap_ : kInline)) return;
        float* h = new float[n];
        const float* d = data();
        for (uint32_t i = 0; i < n_; ++i) h[i] = d[i];
        release_heap();
        u_.heap = h;
        cap_ = (uint32_t)n;
    }
    void assign(const float* src, uint32_t n) {
        if (n > (cap_ > kInline ? cap_ : kInline)) {
            release_heap();
            u_.heap = new float[n];
            cap_ = n;
        }
        float* d = data();
        for (uint32_t i = 0; i < n; ++i) d[i] = src[i];
        n_ = n;
    }
    void take(FloatPayload& o) {
        n_ = o.n_;
        cap_ = o.cap_;
        if (o.cap_ > kInline) {
            u_.heap = o.u_.heap;
        } else {
            for (uint32_t i = 0; i < o.n_; ++i) u_.inl[i] = o.u_.inl[i];
        }
        o.n_ = 0;
        o.cap_ = kInline;
    }
    void release_heap() {
        if (cap_ > kInline) delete[] u_.heap;
        cap_ = kInline;
    }
    void release() {
        release_heap();
        n_ = 0;
    }

    union U {
        float inl[kInline];
        float* heap;
    } u_;
    uint32_t n_ = 0;
    uint32_t cap_ = kInline;  // > kInline: u_.heap holds cap_ floats
};

enum class GXCommandType : uint8_t {  // R22 R7: one byte (GXCommand 136 -> 128 bytes)
    BPRegister,
    CPRegister,
    XFRegister,
    DrawPrimitive
};

struct VertexData {
    // Value-initialised: GX_POS_XY and one-component texcoords leave the
    // last component unset, and it still enters the 3x4 transforms.
    float pos[3] = {0, 0, 0};
    float norm[3] = {0, 0, 0};
    float color[4] = {1, 1, 1, 1};
    float tex[8][2] = {};
    // Untransformed position/normal: texgen reads the raw vertex inputs.
    float raw_pos[3] = {0, 0, 0};
    float raw_norm[3] = {0, 0, 1};
    bool has_raw_norm = false;

    bool has_pos = false;
    bool has_norm = false;
    // Position/normal already transformed to view space at decode time (using
    // the per-draw matrix snapshot); the renderer must not transform again.
    bool pre_xf = false;
    bool has_color = false;
    bool has_tex[8] = {false};

    uint8_t posMtxIdx = 0;
    uint8_t texMtxIdx[8] = {0};
};

// Decoded vertices of a display-list draw, shared between the decode cache
// and every frame that draws them (they never change once built).
// emit_cache: the renderer's per-draw emit cache entry (R9 render; opaque
// here, freed together with the vertices).
struct DecodedVerts : std::vector<VertexData> {
    explicit DecodedVerts(std::vector<VertexData>&& v) : std::vector<VertexData>(std::move(v)) {}
    mutable HostPtr<void> emit_cache;
    // r45p NWII_P_DLMERGE: a display-list draw made of several draws of the list that follow one
    // another with no command between them (same primitive): their vertex counts, in order (the
    // renderer makes the indices part by part: the same primitives in the same order). Empty: one draw.
    std::vector<uint16_t> parts;
    // r45p: the parts' own decoded vertices (owning: they live as long as this), so the renderer can
    // give back their GPU-XF arena slots while the fused draw replaces them.
    std::vector<HostPtr<const DecodedVerts>> part_dv;
};

// Deferred draw payload: raw vertex bytes plus the parse-time state needed to
// decode them later. Most draws are frame-skipped and never decoded, so the
// parser only snapshots; FifoParser::DecodeDraw() decodes survivors.
struct DrawRaw;

// R22 R7: fields ordered so the header packs into 16 bytes and a command is
// exactly 128 bytes (two cache lines; was 136): ~27 000 per castle frame are
// written, read back and destroyed on the render worker.
struct GXCommand {
    GXCommandType type;
    uint8_t prim_type;
    // BP 0x52 (EFB copy) of a frame whose draws frame-skip dropped: the
    // renderer performs only its clear, so the copy cannot overwrite a
    // texture (or the XFB) with an EFB that never received those draws.
    bool stale_copy = false;

    uint16_t reg;
    uint16_t length;
    uint32_t val;
    // BP writes: the value as written, before the BP 0xFE mask merge, so a
    // cached display list can be re-merged against the current registers.
    uint32_t bp_raw = 0;

    std::vector<VertexData> vertices;
    // Decoded vertices shared with the display-list cache (a cached draw
    // whose vertex arrays did not change since its last decode).
    HostPtr<const DecodedVerts> shared_vertices;
    HostPtr<DrawRaw> raw;

    const std::vector<VertexData>& verts() const {
        return shared_vertices ? *shared_vertices : vertices;
    }

    FloatPayload payload;
};
static_assert(sizeof(void*) != 8 || sizeof(GXCommand) == 128, "GXCommand layout (R22 R7)");

} 
