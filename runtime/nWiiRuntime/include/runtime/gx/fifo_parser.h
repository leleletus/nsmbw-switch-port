#pragma once
#include <vector>
#include <cstdint>
#include "runtime/gx_command.h"
#include "runtime/gx_state.h"

namespace nwii::runtime::gx {

void ApplyBPRegister(uint8_t reg, uint32_t val);

// Drain profiling counters (defined in fifo_parser.cpp, reported by gx.cpp).
extern uint64_t g_prof_draws, g_prof_draw_bytes, g_prof_dl_calls,
    g_prof_dl_bytes, g_prof_cmds, g_prof_unknown, g_prof_dl_us, g_prof_snap_us, g_prof_dl_loadindx, g_prof_dl_hits,
    g_prof_decode_hits;
// R22 R7: display-list cache hits (replay) vs misses, for [WPROF].
extern uint64_t g_prof_dl_hit_us, g_prof_dl_hit_cmds, g_prof_dl_miss_us;


// Parse-time snapshot of everything needed to decode a draw's raw vertex
// bytes later (after frame-skip decides the draw survives).
struct DrawRaw {
    VATSlot vat;
    uint32_t arrayBase[16];
    uint32_t arrayStride[16];
    uint8_t defPosMtxIdx;
    uint8_t defTexMtxIdx[8];
    bool need_normal;   // lighting enabled at snapshot time
    // Position/normal matrix captured at parse time for this draw. GX skins by
    // swapping the matrix in a slot between draws; the renderer applies loads
    // late, so a live lookup gives the wrong (last-written) matrix. Snapshot
    // makes the transform use the matrix that was current for THIS draw.
    bool have_snap_mtx = false;
    float snap_pos_mtx[12];
    float snap_nrm_mtx[9];
    // Skinned draws pick a matrix per vertex, so one matrix is not enough --
    // the whole bank has to be frozen at this draw's stream position. Empty for
    // ordinary draws, so nothing is paid for them.
    std::vector<float> snap_pos_bank;   // 256 floats = 64 slots of 3x4
    std::vector<float> snap_nrm_bank;   // 96 floats  = 32 slots of 3x3
    uint16_t count;
    std::vector<uint8_t> bytes;
    // Display-list draws come back every frame as the same DrawRaw (the DL
    // cache shares it). Their decode only depends on the raw bytes, this
    // snapshot and the guest memory the indexed attributes read, so the
    // decoded vertices are kept with a hash of those memory ranges and reused
    // while it matches (NWII_NODECODECACHE=1 disables).
    bool in_dl = false;
    bool no_decode_cache = false;
    HostPtr<const DecodedVerts> decoded;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  // guest addr, bytes
    uint64_t ranges_hash = 0;
    // r40d NWII_D_MERGE_CHECK: vertex counts of the FIFO draws merged into this one (test only).
    std::vector<uint16_t> merge_counts;
};

// R11 render worker: the parser's register state that the renderer reads from
// g_state (CP registers, VAT, arrays, matrix indices) and the BP "seed" writes
// of the commands parsed since the last hand-over. Taken on the parsing thread
// together with a frame, applied to g_state on the render thread just before
// that frame is rendered (see gx.cpp).
struct ParseRenderState {
    uint32_t cp[256];
    VATSlot vat[8];
    uint32_t arrayBase[16];
    uint32_t arrayStride[16];
    uint8_t defPosMtxIdx;
    uint8_t defTexMtxIdx[8];
    uint32_t seed_bp[256];      // valid where seed_mask is set
    uint64_t seed_mask[4];
    uint32_t seed_konst[4][2];  // TEV konst / colour register files (BP 0xE0-0xE7)
    uint32_t seed_color[4][2];
    uint8_t seed_kmask, seed_cmask;  // bit idx*2+half
};

class FifoParser {
public:
    // Parsing thread: copy the parser state and move the pending seeds out.
    static void TakeRenderState(ParseRenderState& out);
    // Render thread: store a taken state in g_state.
    static void ApplyRenderState(const ParseRenderState& s);


    static void Parse(std::vector<uint8_t>& fifo, std::vector<GXCommand>& commands);

    // R22 R7: what Parse() appended, noted while the commands are written (the
    // caller then does not walk the frame again to find them): the positions in
    // `commands` of the draws, per draw its prepared entry (GX_PrepDrawForRender's
    // result, made while replaying a display list) or kEntryUnknown (decode and
    // prepare it as before), and the positions of the XFB copies (BP 0x52 with
    // bit 0x4000, the frame ends).
    struct Index {
        std::vector<uint32_t> draws;
        std::vector<const void*> ents;
        std::vector<uint32_t> xfb_copies;
        void clear() { draws.clear(); ents.clear(); xfb_copies.clear(); }
    };
    static const void* const kEntryUnknown;
    static void Parse(std::vector<uint8_t>& fifo, std::vector<GXCommand>& commands, Index* idx);
    // R22 R7: decoded vertices that commands may still point at without owning
    // them (display-list draw memo, see fifo_parser.cpp) are freed only once no
    // frame that could hold such a pointer is alive: frame_seq = the sequence
    // number the frame being parsed now will get; ReleaseRetired(min_live) frees
    // what was retired while parsing frames < min_live - 1.
    static void SetFrameSeq(uint64_t frame_seq);
    // r40d NWII_D_PARSESTOP: with an index, Parse() stops right after the n-th XFB copy it notes (0: no
    // limit); the bytes after it stay in `fifo` for the next call. StoppedAtXfb(): the last Parse() did.
    static void SetStopAfterXfb(uint32_t n);
    static bool StoppedAtXfb();
    // r40d NWII_D_LIGHTPARSE: with an index, while Parse() has noted fewer than n XFB copies the
    // frames being parsed are known to be dropped: their draws are not written (immediate draws
    // skipped, display-list replays write only their register commands); every other command and
    // all parser state are as in a full parse. 0: off. LightSkipped(): draws skipped so far.
    static void SetDoomedXfb(uint32_t n);
    static bool RenderPrepOk(const GXCommand& c);  // r40d NWII_D_RENDERPREP (see fifo_parser.cpp)
    static bool MergeCheckOn();                     // r40d NWII_D_MERGE_CHECK
    static void DLMergeStats(uint64_t& groups, uint64_t& parts, uint64_t& made, uint64_t& bad);  // r45p NWII_P_DLMERGE (and resets)
    static uint64_t Merged();                       // r40d: FIFO draws merged into the previous one so far
    static uint64_t LightSkipped();
    static void LightTime(uint64_t& ns, uint64_t& frames);  // parse time spent in doomed frames, their count
    static void ReleaseRetired(uint64_t min_live_seq);
    static bool DLMemoOn();

    // Decode a deferred draw's raw bytes into cmd.vertices using the state
    // snapshotted at parse time. No-op if already decoded or not a draw.
    static void DecodeDraw(GXCommand& cmd);
};

} 
