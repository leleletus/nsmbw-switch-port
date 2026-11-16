#include <optional>
#include "runtime/nwii_env.h"
#include <iostream>
#include <vector>
#include "runtime/gx/fifo_parser.h"
#include "runtime/gx_state.h"
#include "runtime/cpu_context.h"
#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <map>
#include <unordered_map>
#include <algorithm>
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>  // R22 R7: DLContentHash
#elif defined(__x86_64__)
#include <immintrin.h>
#endif

// LOAD_INDX data copied by the guest thread when the game issued the command
// (gx.cpp): false = no copy, read guest RAM now as before.
namespace nwii::runtime {
bool GX_IdxTakeMain(int arr, uint32_t addr, int num, float *out);
void GX_IdxBeginDL(uint32_t addr, uint32_t size);
bool GX_IdxReadDL(int arr, uint32_t addr, int num, float *out);
void GX_IdxEndDL();
}

namespace nwii::runtime { extern bool g_early_gx_sync; }
namespace nwii::runtime {
std::shared_ptr<const std::vector<uint8_t>> GX_DL_FetchShared(uint32_t addr, uint32_t size);
}

namespace nwii::runtime {
    extern MMU* g_mmu;
}

namespace nwii::runtime::gx {

extern GXState g_state;

// ---- R11 render worker: parser-owned register state -------------------------
// The parser used to keep the CP state (VAT, vertex arrays, matrix indices) in
// g_state and to "seed" g_state.bp / the TEV register files with every BP write
// it parsed, while the renderer reads g_state (shader hash: CP 0x50/0x60; BP
// registers not yet re-applied in the frame) and writes it (BP applies). With
// the parse of frame N+1 on another thread than the render of frame N, those
// writes cannot go to g_state directly. The parser keeps its own copy (s_ps)
// and records its seeds (s_seed); TakeRenderState hands both over and
// ApplyRenderState (render thread) stores them in g_state right before the
// frame is rendered -- exactly the values today's single-threaded drain left
// there, since the drain parsed everything before rendering.
namespace {
struct ParseRegs {
    uint32_t cp[256] = {};
    VATSlot vat[8] = {};
    uint32_t arrayBase[16] = {};
    uint32_t arrayStride[16] = {};
    uint8_t defPosMtxIdx = 0;
    uint8_t defTexMtxIdx[8] = {};
};
ParseRegs s_ps;
struct SeedDelta {
    uint32_t bp[256];
    uint64_t mask[4] = {};
    uint32_t konst[4][2], color[4][2];
    uint8_t kmask = 0, cmask = 0;  // bit idx*2+half
};
SeedDelta s_seed;
// Same effect on g_state as `g_state.bp[reg] = val; StoreTevColorWrite(reg,
// val);` once applied.
inline void SeedWrite(uint8_t reg, uint32_t val) {
    s_seed.bp[reg] = val;
    s_seed.mask[reg >> 6] |= 1ull << (reg & 63);
    if (reg < 0xE0 || reg > 0xE7) return;
    const int idx = (reg - 0xE0) / 2, half = (reg - 0xE0) & 1;
    if ((val >> 23) & 1) {
        s_seed.konst[idx][half] = val;
        s_seed.kmask |= 1u << (idx * 2 + half);
    } else {
        s_seed.color[idx][half] = val;
        s_seed.cmask |= 1u << (idx * 2 + half);
    }
}
}  // namespace

void FifoParser::TakeRenderState(ParseRenderState& out) {
    std::memcpy(out.cp, s_ps.cp, sizeof out.cp);
    for (int i = 0; i < 8; ++i) out.vat[i] = s_ps.vat[i];
    std::memcpy(out.arrayBase, s_ps.arrayBase, sizeof out.arrayBase);
    std::memcpy(out.arrayStride, s_ps.arrayStride, sizeof out.arrayStride);
    out.defPosMtxIdx = s_ps.defPosMtxIdx;
    std::memcpy(out.defTexMtxIdx, s_ps.defTexMtxIdx, sizeof out.defTexMtxIdx);
    for (int w = 0; w < 4; ++w) {
        out.seed_mask[w] = s_seed.mask[w];
        for (uint64_t m = s_seed.mask[w]; m; m &= m - 1) {
            const int r = w * 64 + __builtin_ctzll(m);
            out.seed_bp[r] = s_seed.bp[r];
        }
        s_seed.mask[w] = 0;
    }
    out.seed_kmask = s_seed.kmask;
    out.seed_cmask = s_seed.cmask;
    std::memcpy(out.seed_konst, s_seed.konst, sizeof out.seed_konst);
    std::memcpy(out.seed_color, s_seed.color, sizeof out.seed_color);
    s_seed.kmask = s_seed.cmask = 0;
}

void FifoParser::ApplyRenderState(const ParseRenderState& s) {
    std::memcpy(g_state.cp, s.cp, sizeof g_state.cp);
    for (int i = 0; i < 8; ++i) g_state.vat[i] = s.vat[i];
    std::memcpy(g_state.arrayBase, s.arrayBase, sizeof g_state.arrayBase);
    std::memcpy(g_state.arrayStride, s.arrayStride, sizeof g_state.arrayStride);
    g_state.defPosMtxIdx = s.defPosMtxIdx;
    std::memcpy(g_state.defTexMtxIdx, s.defTexMtxIdx, sizeof g_state.defTexMtxIdx);
    for (int w = 0; w < 4; ++w)
        for (uint64_t m = s.seed_mask[w]; m; m &= m - 1) {
            const int r = w * 64 + __builtin_ctzll(m);
            g_state.bp[r] = s.seed_bp[r];
        }
    for (int k = 0; k < 8; ++k) {
        if (s.seed_kmask & (1u << k)) g_state.tevKonstReg[k / 2][k & 1] = s.seed_konst[k / 2][k & 1];
        if (s.seed_cmask & (1u << k)) g_state.tevColorReg[k / 2][k & 1] = s.seed_color[k / 2][k & 1];
    }
}

// NWII_SKINDBG state (declared early so ParseCP/ParseBP in the anonymous
// namespace can reference it).
// Parse-time shadow of the XF matrix memory: matrix loads (direct 0x10 and
// indexed 0x20-0x38) are applied here as the stream is parsed, so each draw
// can snapshot the matrix that is current at its point in the stream — before
// the renderer applies later loads over the same slot.
// R7: set by every CP register write; DLStateHash() recomputes only then.
static bool g_dlstate_dirty = true;
static float g_parse_posmtx[256] = {0};
static float g_parse_nrmmtx[96] = {0};

static void ParseApplyMatrixWrite(uint32_t xf_addr, const FloatPayload& payload) {
    for (size_t i = 0; i < payload.size(); ++i) {
        uint32_t a = xf_addr + (uint32_t)i;
        if (a < 256) g_parse_posmtx[a] = payload[i];
        else if (a >= 0x400 && a < 0x400 + 96) g_parse_nrmmtx[a - 0x400] = payload[i];
    }
}

static bool g_skindbg = false;
static uint64_t g_skin_loaded_rows = 0;
static uint64_t g_skin_used_rows = 0;
static uint64_t g_skin_used_missing = 0;
static uint64_t g_skin_draws_total = 0, g_skin_draws_pmidx = 0, g_skin_defnz = 0, g_skin_snapped = 0;

namespace {
    inline bool gx_trace() {
        static bool t = NWII_ENV("NWII_GXTRACE") != nullptr;
        return t;
    }

    inline uint32_t Read24(const std::vector<uint8_t>& fifo, size_t offset) {
        return (fifo[offset] << 16) | (fifo[offset+1] << 8) | fifo[offset+2];
    }

    inline uint32_t Read32(const std::vector<uint8_t>& fifo, size_t offset) {
        return (fifo[offset] << 24) | (fifo[offset+1] << 16) | (fifo[offset+2] << 8) | fifo[offset+3];
    }

    void ApplyBPRegisterImpl(uint8_t reg, uint32_t val);

    void StoreTevColorWrite(uint8_t reg, uint32_t val) {
        if (reg < 0xE0 || reg > 0xE7) return;
        int idx = (reg - 0xE0) / 2, half = (reg - 0xE0) & 1;
        if ((val >> 23) & 1)
            g_state.tevKonstReg[idx][half] = val;
        else
            g_state.tevColorReg[idx][half] = val;
    }

    void ParseBP(uint8_t reg, uint32_t val);
    void ParseCP(uint8_t reg, uint32_t val);

    // XF 0x1018/0x1019 mirror CP 0x30/0x40 (matrix index A/B); G3D sets its
    // per-material texture matrices through XF, so the parser's matrix-index
    // state must follow (Dolphin SetTexMatrixChangedA/B).
    void ParseXFMatrixIndex(const GXCommand& c) {
        for (size_t i = 0; i < c.payload.size(); ++i) {
            const uint32_t a = (uint32_t)c.reg + (uint32_t)i;
            if (a != 0x1018 && a != 0x1019) continue;
            uint32_t w;
            std::memcpy(&w, &c.payload[i], 4);
            const uint8_t cp = a == 0x1018 ? 0x30 : 0x40;
            s_ps.cp[cp] = w;
            g_dlstate_dirty = true;
            ParseCP(cp, w);
        }
    }

    // BP 0xFE (SS_MASK) limits the next BP write to the bits it sets (Dolphin
    // bpmem semantics); G3D uses it for swap tables, KSEL and CMODE0. The merge
    // happens in stream order against a parser-private copy: g_state.bp is
    // rewritten at render time and skipped under NWII_NOSEED.
    uint32_t s_parse_bp[256] = {};
    uint32_t s_bp_mask = 0xFFFFFF;

    uint32_t ParseBPMasked(uint8_t reg, uint32_t raw) {
        if (reg == 0xFE) {
            s_bp_mask = raw & 0xFFFFFF;
            s_parse_bp[0xFE] = raw;
            return raw;
        }
        uint32_t merged = (s_parse_bp[reg] & ~s_bp_mask) | (raw & s_bp_mask);
        s_bp_mask = 0xFFFFFF;
        s_parse_bp[reg] = merged;
        ParseBP(reg, merged);
        return merged;
    }

    void ParseBP(uint8_t reg, uint32_t val) {

        
        
        // gx.cpp already signalled these when the CPU wrote them.
        if (nwii::runtime::g_early_gx_sync) {
        } else if (reg == 0x45 && (val & 0xF) == 2) {
            nwii::runtime::hw::pe_signal_finish();
        } else if (reg == 0x47) {
            nwii::runtime::hw::pe_signal_token(val & 0xFFFF, false);
        } else if (reg == 0x48) {
            nwii::runtime::hw::pe_signal_token(val & 0xFFFF, true);
        }

        
        // from the wrong state is cached for the rest of the run. Dolphin's

        // time. NWII_NOSEED=1 takes the Dolphin behaviour.
        static const bool no_seed = NWII_ENV("NWII_NOSEED") != nullptr;
        if (!no_seed) SeedWrite(reg, val);  // R11: applied with the frame
    }

    void ApplyBPRegisterImpl(uint8_t reg, uint32_t val) {
        g_state.bp[reg] = val;
        StoreTevColorWrite(reg, val);
        if (reg == 0x00) {
            g_state.numTexGens = (val & 0xF);
            g_state.numChans = ((val >> 4) & 0x7);
            g_state.numTevStages = ((val >> 10) & 0xF) + 1;
            g_state.cullMode = (val >> 14) & 3;
        } else if (reg >= 0x28 && reg <= 0x2F) {

            
            int stage = (reg - 0x28) * 2;
            for (int half = 0; half < 2; ++half) {
                uint32_t f = val >> (half * 12);
                auto& s = g_state.tevStages[stage + half];
                bool enabled = (f >> 6) & 1;
                s.texMap = enabled ? (uint8_t)(f & 0x7) : 0xFF;
                s.texCoord = (f >> 3) & 0x7;
                s.colorChan = (f >> 7) & 0x7;
            }
        } else if (reg >= 0xC0 && reg <= 0xDF) {
            
            // Layout per Dolphin BPMemory TevStageCombiner — the inputs are

            
            int stage = (reg - 0xC0) / 2;
            auto& s = g_state.tevStages[stage];
            if (((reg - 0xC0) & 1) == 0) { 
                s.colorInD    = val & 0xF;
                s.colorInC    = (val >> 4) & 0xF;
                s.colorInB    = (val >> 8) & 0xF;
                s.colorInA    = (val >> 12) & 0xF;
                s.colorBias   = (val >> 16) & 0x3;
                s.colorOp     = (val >> 18) & 0x1;
                s.colorClamp  = (val >> 19) & 0x1;
                s.colorScale  = (val >> 20) & 0x3;
                s.colorRegId  = (val >> 22) & 0x3;
            } else {                       
                s.alphaInD    = (val >> 4) & 0x7;
                s.alphaInC    = (val >> 7) & 0x7;
                s.alphaInB    = (val >> 10) & 0x7;
                s.alphaInA    = (val >> 13) & 0x7;
                s.alphaBias   = (val >> 16) & 0x3;
                s.alphaOp     = (val >> 18) & 0x1;
                s.alphaClamp  = (val >> 19) & 0x1;
                s.alphaScale  = (val >> 20) & 0x3;
                s.alphaRegId  = (val >> 22) & 0x3;
            }
        } else if ((reg >= 0x88 && reg <= 0x8B) || (reg >= 0xA8 && reg <= 0xAB)) {
            
            // 10-19, format bits 20-23 (Dolphin BPMemory TexImage0).
            int idx = (reg >= 0x88 && reg <= 0x8B) ? (reg - 0x88) : (reg - 0xA8 + 4);
            if (idx < (int)g_state.texStages.size()) {
                g_state.texStages[idx].width  = ((val >>  0) & 0x3FF) + 1;
                g_state.texStages[idx].height = ((val >> 10) & 0x3FF) + 1;
                g_state.texStages[idx].format = (val >> 20) & 0xF;
            }
        } else if ((reg >= 0x94 && reg <= 0x97) || (reg >= 0xB4 && reg <= 0xB7)) {
            
            int idx = (reg >= 0x94 && reg <= 0x97) ? (reg - 0x94) : (reg - 0xB4 + 4);
            if (idx < (int)g_state.texStages.size()) {
                g_state.texStages[idx].base_addr = (val & 0xFFFFFF) << 5;
            }
        } else if ((reg >= 0x98 && reg <= 0x9B) || (reg >= 0xB8 && reg <= 0xBB)) {
            // TX_SETTLUT for maps 0-3 / 4-7 (0x9C-0x9F and 0xBC-0xBF are not
            // TLUT registers; Dolphin bpmem).
            int idx = (reg <= 0x9B) ? (reg - 0x98) : (reg - 0xB8 + 4);
            if (idx < (int)g_state.texStages.size()) {
                g_state.texStages[idx].tlut_offset = (val & 0x3FF) << 9;
                g_state.texStages[idx].tlut_format = (val >> 10) & 0x3;
            }
        } else if (reg == 0x64) {

            // (Dolphin names Wind Waker and Double Dash; MP7 does it too, which

            uint32_t addr = (val & 0xFFFFFF) << 5;
            if (nwii::runtime::Config::get().platform ==
                nwii::runtime::Platform::GameCube)
                addr &= 0x01FFFFFF;
            g_state.tlutSrcAddr = addr;
        } else if (reg == 0x65) {

            uint32_t dst = (val & 0x3FF) << 9;
            uint32_t bytes = ((val >> 10) & 0x7FF) * 32;
            if (nwii::runtime::g_mmu && dst + bytes <= sizeof(g_state.tlutMem)) {
                if (const uint8_t* p =
                        nwii::runtime::g_mmu->get_ptr(g_state.tlutSrcAddr)) {
                    std::memcpy(&g_state.tlutMem[dst], p, bytes);
                } else {
                    for (uint32_t i = 0; i < bytes; i++)
                        g_state.tlutMem[dst + i] =
                            nwii::runtime::g_mmu->read8(g_state.tlutSrcAddr + i);
                }
            }
        } else if (reg == 0x40) {
            g_state.zMode.enable  = (val >> 0) & 1;
            g_state.zMode.func    = (val >> 1) & 7;
            g_state.zMode.update  = (val >> 4) & 1;
        } else if (reg == 0xF3) {

            g_state.alphaTest.ref0  = val & 0xFF;
            g_state.alphaTest.ref1  = (val >> 8) & 0xFF;
            g_state.alphaTest.comp0 = (val >> 16) & 0x7;
            g_state.alphaTest.comp1 = (val >> 19) & 0x7;
            g_state.alphaTest.logic = (val >> 22) & 0x3;
        } else if (reg == 0x4F) {
            g_state.clearAR = val & 0xFFFF; 
        } else if (reg == 0x50) {
            g_state.clearGB = val & 0xFFFF; 
        } else if (reg == 0x49) { 
            g_state.efbSrcX = val & 0x3FF;
            g_state.efbSrcY = (val >> 10) & 0x3FF;
        } else if (reg == 0x4A) { 
            g_state.efbW = (val & 0x3FF) + 1;
            g_state.efbH = ((val >> 10) & 0x3FF) + 1;
        } else if (reg == 0x4B) { 
            g_state.efbCopyDest = (val & 0xFFFFFF) << 5;
        } else if (reg == 0x4D) { 
            g_state.efbCopyStride = (val & 0x3FF) << 5;
        } else if (reg == 0x52) { 

            if (gx_trace()) {
                printf("[GXTRACE] BP 0x52 PE_COPY_EXECUTE val=0x%08X (bit14=%d bit11=%d)\n",
                       val, (val & 0x4000) != 0, (val & 0x800) != 0);
            }

            
            if (val & 0x4000) {
                g_state.xfbAddr = g_state.efbCopyDest;
                g_state.xfbW = g_state.efbW;
                g_state.xfbH = g_state.efbH;
                g_state.xfbSrcX = g_state.efbSrcX;
                g_state.xfbSrcY = g_state.efbSrcY;
                g_state.xfbStride = g_state.efbCopyStride
                                        ? g_state.efbCopyStride
                                        : (uint32_t)g_state.efbW * 2;
                g_state.frame_ready = true;
            }
            if (val & 0x800) {
                g_state.pe_clear_pending = true;
            }
            // An EFB copy does not signal PE finish (Dolphin BPWritten: only
            // BP 0x45 does). Raising it here fired a PE interrupt from the
            // host thread for every copy the renderer drew -- 20+ per frame
            // once the title's light maps render -- at times unrelated to
            // the guest. NWII_COPY_FINISH=1 restores it.
            static const bool copy_finish = NWII_ENV("NWII_COPY_FINISH") != nullptr;
            if (copy_finish)
                nwii::runtime::hw::pe_signal_finish();
        }
    }

    

    
    void ParseCP(uint8_t reg, uint32_t val) {
        // R22 R7: DLStateHash() covers CP 0x30-0xBF; a write of the same value
        // (most of a display list's replayed CP writes) leaves it as it is.
        if (s_ps.cp[reg] != val) {
            s_ps.cp[reg] = val;
            g_dlstate_dirty = true;
        }
        if (g_skindbg && (reg == 0x50 || reg == 0x30 || reg == 0x60)) {
            static uint32_t last50=0xFFFFFFFF, last30=0xFFFFFFFF, last60=0xFFFFFFFF;
            uint32_t* lp = reg==0x50?&last50:(reg==0x30?&last30:&last60);
            if (val != *lp) {
                *lp = val;
                printf("[CPTR] reg=0x%02X val=0x%08X (0x50.bit0/posMatIdx=%d)\n",
                       reg, val, reg==0x50 ? (val & 1) : -1);
            }
        }
        if (reg >= 0xA0 && reg <= 0xAF) {
            s_ps.arrayBase[reg - 0xA0] = val & 0x3FFFFFFF;
        } else if (reg >= 0xB0 && reg <= 0xBF) {
            s_ps.arrayStride[reg - 0xB0] = val & 0xFF;
        } else if (reg == 0x30) {
            // Matrix index A: pos/normal (bits 0-5), tex0-3 (6-29).
            s_ps.defPosMtxIdx = val & 0x3F;
            for (int t = 0; t < 4; t++)
                s_ps.defTexMtxIdx[t] = (val >> (6 + t * 6)) & 0x3F;
            static const bool tmdbg = NWII_ENV("NWII_TEXMTXDBG") != nullptr;
            if (tmdbg) {
                static uint32_t seen = 0;
                for (int t = 0; t < 4; t++) seen |= (1u << s_ps.defTexMtxIdx[t]);
                static auto t0 = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    printf("[TEXMTX30] tex0-3 indices:");
                    for (int i = 0; i < 32; i++) if (seen & (1u << i)) printf(" %d", i);
                    printf(" numTexGens=%d\n", g_state.numTexGens);
                    seen = 0;
                }
            }
        } else if (reg == 0x40) {
            // Matrix index B: tex4-7.
            for (int t = 0; t < 4; t++)
                s_ps.defTexMtxIdx[4 + t] = (val >> (t * 6)) & 0x3F;
            static const bool tmdbg = NWII_ENV("NWII_TEXMTXDBG") != nullptr;
            if (tmdbg) {
                static uint32_t seen = 0;
                for (int t = 0; t < 8; t++) seen |= (1u << s_ps.defTexMtxIdx[t]);
                static auto t0 = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    printf("[TEXMTX] indices seen:");
                    for (int i = 0; i < 32; i++) if (seen & (1u << i)) printf(" %d", i);
                    printf("\n");
                    seen = 0;
                }
            }
        } else if (reg == 0x50) {

            
            
            for (int i = 0; i < 8; i++) {
                s_ps.vat[i].posMatIdx  = (val >> 0) & 1;
                for (int t = 0; t < 8; t++)
                    s_ps.vat[i].texMatIdx[t] = (val >> (1 + t)) & 1;
                s_ps.vat[i].posMask    = (VtxAttrMask)((val >> 9)  & 3);
                s_ps.vat[i].nrmMask    = (VtxAttrMask)((val >> 11) & 3);
                s_ps.vat[i].clrMask[0] = (VtxAttrMask)((val >> 13) & 3);
                s_ps.vat[i].clrMask[1] = (VtxAttrMask)((val >> 15) & 3);
            }
        } else if (reg == 0x60) {
            
            for (int i = 0; i < 8; i++)
                for (int t = 0; t < 8; t++)
                    s_ps.vat[i].texMask[t] = (VtxAttrMask)((val >> (t * 2)) & 3);
        } else if (reg >= 0x70 && reg <= 0x77) {
            
            int i = reg - 0x70;
            s_ps.vat[i].posElements  = (val >> 0)  & 1;
            s_ps.vat[i].posType      = (VtxAttrType)((val >> 1) & 7);
            s_ps.vat[i].posShift     = (val >> 4)  & 0x1F;
            s_ps.vat[i].nrmElements  = (val >> 9)  & 1;
            s_ps.vat[i].nrmType      = (VtxAttrType)((val >> 10) & 7);
            s_ps.vat[i].clrElements[0] = (val >> 13) & 1;
            s_ps.vat[i].clrType[0]   = (VtxAttrType)((val >> 14) & 7);
            s_ps.vat[i].clrElements[1] = (val >> 17) & 1;
            s_ps.vat[i].clrType[1]   = (VtxAttrType)((val >> 18) & 7);
            s_ps.vat[i].texElements[0] = (val >> 21) & 1;
            s_ps.vat[i].texType[0]   = (VtxAttrType)((val >> 22) & 7);
            s_ps.vat[i].texShift[0]  = (val >> 25) & 0x1F;
            // Dolphin UVAT_group0 bit 31: NormalIndex3 (NBT by three indices).
            s_ps.vat[i].nrmIndex3    = ((val >> 31) & 1) != 0;
        } else if (reg >= 0x80 && reg <= 0x87) {
            
            int i = reg - 0x80;
            s_ps.vat[i].texElements[1] = (val >> 0)  & 1;
            s_ps.vat[i].texType[1]   = (VtxAttrType)((val >> 1) & 7);
            s_ps.vat[i].texShift[1]  = (val >> 4)  & 0x1F;
            s_ps.vat[i].texElements[2] = (val >> 9)  & 1;
            s_ps.vat[i].texType[2]   = (VtxAttrType)((val >> 10) & 7);
            s_ps.vat[i].texShift[2]  = (val >> 13) & 0x1F;
            s_ps.vat[i].texElements[3] = (val >> 18) & 1;
            s_ps.vat[i].texType[3]   = (VtxAttrType)((val >> 19) & 7);
            s_ps.vat[i].texShift[3]  = (val >> 22) & 0x1F;
            s_ps.vat[i].texElements[4] = (val >> 27) & 1;
            s_ps.vat[i].texType[4]   = (VtxAttrType)((val >> 28) & 7);
        } else if (reg >= 0x90 && reg <= 0x97) {
            
            int i = reg - 0x90;
            s_ps.vat[i].texShift[4]  = (val >> 0)  & 0x1F;
            s_ps.vat[i].texElements[5] = (val >> 5)  & 1;
            s_ps.vat[i].texType[5]   = (VtxAttrType)((val >> 6) & 7);
            s_ps.vat[i].texShift[5]  = (val >> 9)  & 0x1F;
            s_ps.vat[i].texElements[6] = (val >> 14) & 1;
            s_ps.vat[i].texType[6]   = (VtxAttrType)((val >> 15) & 7);
            s_ps.vat[i].texShift[6]  = (val >> 18) & 0x1F;
            s_ps.vat[i].texElements[7] = (val >> 23) & 1;
            s_ps.vat[i].texType[7]   = (VtxAttrType)((val >> 24) & 7);
            s_ps.vat[i].texShift[7]  = (val >> 27) & 0x1F;
        }
    }

    inline int TypeSize(VtxAttrType type) {
        switch (type) {
            case VtxAttrType::U8: case VtxAttrType::S8: return 1;
            case VtxAttrType::U16: case VtxAttrType::S16: return 2;
            default: return 4; 
        }
    }

    // r40d NWII_D_FASTDECODE=1: the same values without a float division per
    // component: x / 2^s == x * 2^-s exactly (a power of two, results never
    // denormal: |x| >= 1 or 0), and colour channels from tables filled with
    // the very expressions below (c / 255.0f ...). NWII_D_FASTDECODE_CHECK=1
    // decodes every draw again the old way and compares.
    thread_local bool g_fastdec = false;  // r40d: per thread (NWII_D_RENDERPREP decodes on the render thread too)
    struct FastTabs {
        float inv_pow2[32];
        float c255[256], c63[64], c31[32], c15[16];
        FastTabs() {
            for (int s = 0; s < 32; ++s) inv_pow2[s] = 1.0f / (float)(1 << s);
            for (int i = 0; i < 256; ++i) c255[i] = i / 255.0f;
            for (int i = 0; i < 64; ++i) c63[i] = i / 63.0f;
            for (int i = 0; i < 32; ++i) c31[i] = i / 31.0f;
            for (int i = 0; i < 16; ++i) c15[i] = i / 15.0f;
        }
    };
    const FastTabs s_fast_tabs;  // (filled at load; no guard on every use)
    inline const FastTabs& Tabs() { return s_fast_tabs; }
    inline float DecodeScalar(const uint8_t* p, VtxAttrType type, uint8_t shift) {
        if (g_fastdec) {
            const float inv = Tabs().inv_pow2[shift & 31];
            switch (type) {
                case VtxAttrType::U8:  return (float)p[0] * inv;
                case VtxAttrType::S8:  return (float)(int8_t)p[0] * inv;
                case VtxAttrType::U16: return (float)(uint16_t)((p[0] << 8) | p[1]) * inv;
                case VtxAttrType::S16: return (float)(int16_t)((p[0] << 8) | p[1]) * inv;
                default: {
                    uint32_t v = ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
                    float f; std::memcpy(&f, &v, 4); return f;
                }
            }
        }
        switch (type) {
            case VtxAttrType::U8:  return (float)p[0] / (float)(1 << shift);
            case VtxAttrType::S8:  return (float)(int8_t)p[0] / (float)(1 << shift);
            case VtxAttrType::U16: return (float)(uint16_t)((p[0] << 8) | p[1]) / (float)(1 << shift);
            case VtxAttrType::S16: return (float)(int16_t)((p[0] << 8) | p[1]) / (float)(1 << shift);
            default: {
                uint32_t v = ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
                float f; std::memcpy(&f, &v, 4); return f;
            }
        }
    }

    

    
    // Guest memory read by a decode's indexed attributes (decoded-vertex
    // cache validation): per attribute slot, the lowest and highest byte.
    struct RangeTrack {
        uint32_t lo[16], hi[16];
        bool used[16];
    };
    thread_local RangeTrack* g_track = nullptr;  // r40d: per thread (was shared; see g_fastdec)
    inline void TrackRead(int slot, uint32_t addr, uint32_t bytes) {
        if (!g_track || slot < 0) return;
        RangeTrack& t = *g_track;
        if (!t.used[slot]) {
            t.used[slot] = true;
            t.lo[slot] = addr;
            t.hi[slot] = addr + bytes;
        } else {
            t.lo[slot] = std::min(t.lo[slot], addr);
            t.hi[slot] = std::max(t.hi[slot], addr + bytes);
        }
    }

    bool ReadVectorAttr(const std::vector<uint8_t>& fifo, size_t& fifo_offset, size_t fifo_size,
                        VtxAttrMask mask, VtxAttrType type, uint8_t shift,
                        uint32_t array_base, uint32_t array_stride, int ncomp, float* out,
                        int track = -1) {
        for (int i = 0; i < ncomp; i++) out[i] = 0.0f;
        if (mask == VtxAttrMask::None) return true;

        int sz = TypeSize(type);
        if (mask == VtxAttrMask::Direct) {
            if (fifo_offset + (size_t)sz * ncomp > fifo_size) return false;
            for (int i = 0; i < ncomp; i++)
                out[i] = DecodeScalar(&fifo[fifo_offset + (size_t)sz * i], type, shift);
            fifo_offset += (size_t)sz * ncomp;
            return true;
        }

        uint32_t index = 0;
        if (mask == VtxAttrMask::Index8) {
            if (fifo_offset + 1 > fifo_size) return false;
            index = fifo[fifo_offset++];
        } else { 
            if (fifo_offset + 2 > fifo_size) return false;
            index = (fifo[fifo_offset] << 8) | fifo[fifo_offset + 1];
            fifo_offset += 2;
        }
        if (!nwii::runtime::g_mmu) return true;
        uint32_t base = array_base + index * array_stride;
        TrackRead(track, base, (uint32_t)(sz * ncomp));

        const uint8_t* p = nwii::runtime::g_mmu->get_ptr(base);
        if (!p) return true;
        float inv = g_fastdec ? Tabs().inv_pow2[shift & 31] : 1.0f / (float)(1 << shift);  // r40d (same value)
        for (int i = 0; i < ncomp; i++) {
            const uint8_t* q = p + (size_t)sz * i;
            switch (type) {
                case VtxAttrType::U8:  out[i] = (float)q[0] * inv; break;
                case VtxAttrType::S8:  out[i] = (float)(int8_t)q[0] * inv; break;
                case VtxAttrType::U16: out[i] = (float)(uint16_t)((q[0] << 8) | q[1]) * inv; break;
                case VtxAttrType::S16: out[i] = (float)(int16_t)((q[0] << 8) | q[1]) * inv; break;
                default: {
                    uint32_t w = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                                 ((uint32_t)q[2] << 8) | q[3];
                    std::memcpy(&out[i], &w, 4);
                    break;
                }
            }
        }
        return true;
    }

    
    
    int ColorBytes(VtxAttrType clrType) {
        switch ((int)clrType) {
            case 0: case 3: return 2;
            case 1: case 4: return 3;
            default:        return 4; 
        }
    }

    void DecodeColor(const uint8_t* p, VtxAttrType clrType, float out[4]) {
        if (g_fastdec) {  // r40d: same values from tables
            const FastTabs& T = Tabs();
            switch ((int)clrType) {
                case 0: {
                    uint16_t c = (p[0] << 8) | p[1];
                    out[0] = T.c31[(c >> 11) & 0x1F]; out[1] = T.c63[(c >> 5) & 0x3F];
                    out[2] = T.c31[c & 0x1F]; out[3] = 1.0f;
                    return;
                }
                case 1: case 2:
                    out[0] = T.c255[p[0]]; out[1] = T.c255[p[1]]; out[2] = T.c255[p[2]]; out[3] = 1.0f;
                    return;
                case 3: {
                    uint16_t c = (p[0] << 8) | p[1];
                    out[0] = T.c15[(c >> 12) & 0xF]; out[1] = T.c15[(c >> 8) & 0xF];
                    out[2] = T.c15[(c >> 4) & 0xF]; out[3] = T.c15[c & 0xF];
                    return;
                }
                case 4: {
                    uint32_t c = (p[0] << 16) | (p[1] << 8) | p[2];
                    out[0] = T.c63[(c >> 18) & 0x3F]; out[1] = T.c63[(c >> 12) & 0x3F];
                    out[2] = T.c63[(c >> 6) & 0x3F]; out[3] = T.c63[c & 0x3F];
                    return;
                }
                default:
                    out[0] = T.c255[p[0]]; out[1] = T.c255[p[1]]; out[2] = T.c255[p[2]]; out[3] = T.c255[p[3]];
                    return;
            }
        }
        switch ((int)clrType) {
            case 0: { 
                uint16_t c = (p[0] << 8) | p[1];
                out[0] = ((c >> 11) & 0x1F) / 31.0f;
                out[1] = ((c >> 5) & 0x3F) / 63.0f;
                out[2] = (c & 0x1F) / 31.0f;
                out[3] = 1.0f;
                break;
            }
            case 1: 
            case 2: 
                out[0] = p[0] / 255.0f;
                out[1] = p[1] / 255.0f;
                out[2] = p[2] / 255.0f;
                out[3] = 1.0f;
                break;
            case 3: { 
                uint16_t c = (p[0] << 8) | p[1];
                out[0] = ((c >> 12) & 0xF) / 15.0f;
                out[1] = ((c >> 8) & 0xF) / 15.0f;
                out[2] = ((c >> 4) & 0xF) / 15.0f;
                out[3] = (c & 0xF) / 15.0f;
                break;
            }
            case 4: { 
                uint32_t c = (p[0] << 16) | (p[1] << 8) | p[2];
                out[0] = ((c >> 18) & 0x3F) / 63.0f;
                out[1] = ((c >> 12) & 0x3F) / 63.0f;
                out[2] = ((c >> 6) & 0x3F) / 63.0f;
                out[3] = (c & 0x3F) / 63.0f;
                break;
            }
            default: 
                out[0] = p[0] / 255.0f;
                out[1] = p[1] / 255.0f;
                out[2] = p[2] / 255.0f;
                out[3] = p[3] / 255.0f;
                break;
        }
    }

    
    bool ReadColorAttr(const std::vector<uint8_t>& fifo, size_t& fifo_offset, size_t fifo_size,
                       VtxAttrMask mask, VtxAttrType clrType,
                       uint32_t array_base, uint32_t array_stride, float out[4],
                       int track = -1) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        if (mask == VtxAttrMask::None) return true;

        int nbytes = ColorBytes(clrType);
        if (mask == VtxAttrMask::Direct) {
            if (fifo_offset + (size_t)nbytes > fifo_size) return false;
            DecodeColor(&fifo[fifo_offset], clrType, out);
            fifo_offset += nbytes;
            return true;
        }

        uint32_t index = 0;
        if (mask == VtxAttrMask::Index8) {
            if (fifo_offset + 1 > fifo_size) return false;
            index = fifo[fifo_offset++];
        } else {
            if (fifo_offset + 2 > fifo_size) return false;
            index = (fifo[fifo_offset] << 8) | fifo[fifo_offset + 1];
            fifo_offset += 2;
        }
        if (!nwii::runtime::g_mmu) return true;
        uint32_t base = array_base + index * array_stride;
        TrackRead(track, base, (uint32_t)nbytes);
        const uint8_t* p = nwii::runtime::g_mmu->get_ptr(base);
        if (!p) return true;
        DecodeColor(p, clrType, out);
        return true;
    }
}


void ApplyBPRegister(uint8_t reg, uint32_t val) { ApplyBPRegisterImpl(reg, val); }

static void ParseStream(const std::vector<uint8_t>& fifo, size_t& offset, std::vector<GXCommand>& commands, int depth);
static bool PreXf();
// r40d NWII_D_LIGHTPARSE (see the header): s_doom_xfb = XFB copies below which the depth-0
// stream is doomed; g_doomed = the command being parsed now is (set by ParseStream).
static uint32_t s_doom_xfb = 0;
static bool g_doomed = false;
static uint64_t s_light_skipped = 0;
static uint64_t s_light_ns = 0, s_light_frames = 0;  // r40d: parse time of the doomed part (diagnostic)
static std::chrono::steady_clock::time_point s_parse_t0;
// r40d NWII_D_EAGERDECODE (see ParseStream) / _CHECK.
static bool EagerDecodeOn() {
    static const bool on = [] { const char* e = NWII_ENV("NWII_D_EAGERDECODE"); return e && e[0] && e[0] != '0'; }();
    return on;
}
static bool EagerCheckOn() {
    static const bool on = NWII_ENV("NWII_D_EAGERDECODE_CHECK") != nullptr;
    return on;
}
static uint64_t s_eager_draws = 0;
// r40d NWII_D_MERGE=1: a FIFO draw that comes right after another FIFO draw (no command between
// them: the render state is the same), with the same primitive (quads or triangles: lists, so
// the concatenation is the same primitives in the same order) and the same snapshot (VAT, arrays,
// default matrices; no parse-time transform), is appended to it (bytes and count) instead of
// becoming a draw of its own. The renderer then sees one draw with the same vertices in the same
// order. Kept apart: display-list draws (the cache owns them), draws already decoded, a previous
// draw whose count is not a whole number of primitives, more than kMergeMaxVerts vertices.
static bool MergeOn() {
    static const bool on = [] { const char* e = NWII_ENV("NWII_D_MERGE"); return e && e[0] && e[0] != '0'; }();
    return on;
}
bool FifoParser::MergeCheckOn() {
    static const bool on = NWII_ENV("NWII_D_MERGE_CHECK") != nullptr;
    return on;
}
static uint64_t s_merged = 0;
uint64_t FifoParser::Merged() { return s_merged; }
static constexpr uint32_t kMergeMaxVerts = 4096;
static bool AllDirect(const VATSlot& v) {
    auto ix = [](VtxAttrMask m) { return m == VtxAttrMask::Index8 || m == VtxAttrMask::Index16; };
    if (ix(v.posMask) || ix(v.nrmMask) || ix(v.clrMask[0]) || ix(v.clrMask[1])) return false;
    for (int t = 0; t < 8; ++t)
        if (ix(v.texMask[t])) return false;
    return true;
}
static bool VertsSame(const std::vector<VertexData>& a, const std::vector<VertexData>& b);
static void DecodeDrawImpl(GXCommand& c);





// Display lists are static geometry the game CALLs every frame; re-parsing
// them dominated the 3D-scene drain (measured 1.3s per 2s). Cache the parsed
// commands keyed by address+content, guarded by a hash of the parser state
// that gets baked into draw snapshots (VAT, arrays, matrix indices, channel
// control) so a state change forces a re-parse. LOAD_INDX payloads are the
// one thing read from RAM at parse time — on a cache hit they are re-read so
// skinning animation stays live. NWII_NODLCACHE=1 disables.
// R22 R7: per draw of a cached list, its decoded vertices and prepared emit-cache
// entry, valid while the guest memory its indexed attributes read is unchanged
// (same test as DecodeDraw's decode cache: RangesHash of the same ranges). A
// replay then writes the draw's command straight from this compact table (no
// DrawRaw, no range vector, no reference count touched; ~12 000 draws per frame
// in the castle cutscene, each of those a cache miss on the Switch) and notes its
// entry, so the frame-end pass does not visit the draw at all. The command points
// at the vertices without owning them (aliasing HostPtr, no control block): the
// table owns them, and a replaced or evicted one is retired (s_grave) until no
// frame that could point at it is alive (ReleaseRetired). NWII_DLMEMO=0: off.
struct DLDrawMemo {
    HostPtr<const DecodedVerts> dv;  // null: no memo (decoded as before)
    const void* entry = nullptr;     // GX_PrepDrawForRender(dv) (valid with has_entry)
    uint64_t hash = 0;               // RangesHash of the ranges when dv was made
    uint32_t r0 = 0, rn = 0;         // its ranges in DLCacheEntry::ranges
    bool has_entry = false;
    // The draw command's own fields (its position in cmds and its header): a
    // valid memo writes the command without reading the cached one.
    uint8_t prim_type = 0;
    bool stale_copy = false;
    uint16_t reg = 0, length = 0;
    uint32_t ci = 0, val = 0, bp_raw = 0;
};
// r45p NWII_P_DLMERGE=1: a run of memo draws [k0, k1) of a list, consecutive in `cmds` (no command
// between them: same render state, same matrices), same primitive (quads, triangles, strips or
// fans), replayed as ONE draw whose vertices are theirs concatenated (DecodedVerts::parts = their
// counts) with its own emit-cache entry: the renderer does the per-draw work (state test, cull
// test, GPU-XF entry, palette, texgen record) once per run instead of once per draw, and still
// makes the indices of each part as before. Made when every part has a valid memo; made again when
// a part's vertices change (Retire, as the memos); not used for a frame in which a part is not valid
// (the parts then go one by one, as without the switch).
struct DLFuseGroup {
    uint32_t k0 = 0, k1 = 0;               // memo range
    HostPtr<const DecodedVerts> dv;         // the fused vertices (owning)
    const void* entry = nullptr;            // GX_PrepDrawForRender(dv)
    std::vector<const DecodedVerts*> src;   // the parts' memo vertices it was made from
    bool bad = false;                       // never fused (flags differ / too many vertices)
};
struct DLCacheEntry {
    uint64_t state_hash;
    uint64_t content_hash;
    std::vector<GXCommand> cmds;
    uint64_t last_use = 0;   // g_gx_drain_epoch of the last hit/insert
    std::vector<DLDrawMemo> memo;  // R22 R7: per draw of cmds, in order
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    // R22 R7: the 1 KB chunks all memo ranges touch (sorted, unique) and their
    // hash when every memo was last found valid: while it matches, no memo's
    // memory changed and the per-draw tests are skipped (one test per list).
    std::vector<uint32_t> chunks;
    uint64_t chunks_hash = 0;
    bool chunks_ok = false;
    bool memo_init = false;
    std::vector<DLFuseGroup> fgroups;  // r45p NWII_P_DLMERGE
    std::vector<int32_t> fg_of;        // r45p: memo index -> its group if it starts one, else -1
    bool fg_init = false;
};
extern uint64_t g_gx_drain_epoch;
static std::unordered_map<uint64_t, DLCacheEntry> s_dl_cache;
static uint64_t s_frame_seq = 1;  // R22 R7: see FifoParser::SetFrameSeq
static std::vector<std::pair<uint64_t, HostPtr<const DecodedVerts>>> s_grave;
static inline void Retire(HostPtr<const DecodedVerts>& p) {
    // Commands parsed now may end up in this frame or (the tail after its end)
    // the next one: both may point at it.
    if (p) s_grave.emplace_back(s_frame_seq + 1, std::move(p));
    p.reset();
}
static void RetireEntry(DLCacheEntry& e) {
    for (DLDrawMemo& m : e.memo) Retire(m.dv);
    for (DLFuseGroup& g : e.fgroups) Retire(g.dv);  // r45p
}
void FifoParser::SetFrameSeq(uint64_t frame_seq) { s_frame_seq = frame_seq; }
void FifoParser::ReleaseRetired(uint64_t min_live_seq) {
    if (s_grave.empty() || s_grave.front().first >= min_live_seq) return;
    size_t k = 0;
    while (k < s_grave.size() && s_grave[k].first < min_live_seq) ++k;
    s_grave.erase(s_grave.begin(), s_grave.begin() + k);  // (in tag order)
}
// r40d NWII_D_RENDERPREP: a draw the render thread may decode and pack itself: a FIFO draw (not
// from a display list: its DrawRaw belongs to this command alone, no count shared with the caches)
// whose attributes are all direct (its decode reads only its own bytes and snapshot, no guest
// memory), not decoded yet, no parse-time transform.
bool FifoParser::RenderPrepOk(const GXCommand& c) {
    return c.type == GXCommandType::DrawPrimitive && c.raw && !c.raw->in_dl && c.vertices.empty() &&
           !c.shared_vertices && !PreXf() && AllDirect(c.raw->vat);
}
bool FifoParser::DLMemoOn() {
    static const bool on = [] { const char* e = NWII_ENV("NWII_DLMEMO"); return !(e && e[0] == '0'); }();
    return on;
}
const void* const FifoParser::kEntryUnknown = reinterpret_cast<const void*>(uintptr_t(2));
static FifoParser::Index* s_pidx = nullptr;  // R22 R7: Parse()'s index (depth-0 commands)
static inline void NoteCmd(const GXCommand& c, uint32_t i) {
    if (c.type == GXCommandType::DrawPrimitive) {
        s_pidx->draws.push_back(i);
        s_pidx->ents.push_back(FifoParser::kEntryUnknown);
    } else if (c.type == GXCommandType::BPRegister && c.reg == 0x52 && (c.val & 0x4000)) {
        s_pidx->xfb_copies.push_back(i);
    }
}
uint64_t g_prof_dl_hits = 0;
uint64_t g_prof_dl_hit_us = 0, g_prof_dl_hit_cmds = 0, g_prof_dl_miss_us = 0;  // R22 R7 ([WPROF])

// R8 memory: heap bytes a parsed command owns (its vectors and, for a draw,
// its DrawRaw with the decoded vertices kept for the decode cache). Shared
// DrawRaws are counted for every command that holds them; in the DL cache
// each one has a single owner, so the sum is right there.
size_t GXCommandHeapBytes(const GXCommand& c) {
    size_t b = c.payload.capacity() * sizeof(float) +
               c.vertices.capacity() * sizeof(VertexData);
    if (const DrawRaw* r = c.raw.get()) {
        b += sizeof(DrawRaw) + 16 + r->bytes.capacity() +
             (r->snap_pos_bank.capacity() + r->snap_nrm_bank.capacity()) * sizeof(float) +
             r->ranges.capacity() * sizeof(r->ranges[0]);
        if (r->decoded) b += 16 + r->decoded->capacity() * sizeof(VertexData);
    } else if (c.shared_vertices) {
        b += 16 + c.shared_vertices->capacity() * sizeof(VertexData);
    }
    return b;
}

// R8 memory: entries, cached commands and heap bytes of the display-list
// cache (host thread, like every other s_dl_cache access).
void DLCacheMemStats(size_t& entries, size_t& cmds, size_t& bytes) {
    entries = s_dl_cache.size();
    cmds = 0;
    bytes = s_dl_cache.bucket_count() * sizeof(void*);
    for (const auto& kv : s_dl_cache) {
        const DLCacheEntry& e = kv.second;
        cmds += e.cmds.size();
        bytes += sizeof(kv) + 16 + e.cmds.capacity() * sizeof(GXCommand) +
                 e.memo.capacity() * sizeof(DLDrawMemo) + e.ranges.capacity() * 8;
        for (const GXCommand& c : e.cmds) bytes += GXCommandHeapBytes(c);
    }
}
// NWII_SKINDBG report (state declared earlier for ParseCP visibility).
static void skin_report() {
    static auto t0 = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() < 2) return;
    t0 = now;
    printf("[SKIN] loaded rows:");
    for (int i = 0; i < 64; i++) if (g_skin_loaded_rows & (1ull<<i)) printf(" %d", i);
    printf(" | used posMtxIdx:");
    for (int i = 0; i < 64; i++) if (g_skin_used_rows & (1ull<<i)) printf(" %d", i);
    printf(" | used-but-unloaded:");
    for (int i = 0; i < 64; i++) if (g_skin_used_missing & (1ull<<i)) printf(" %d", i);
    printf(" | draws=%llu perVtxIdx=%llu defNonZero=%llu snapped=%llu\n",
           (unsigned long long)g_skin_draws_total,
           (unsigned long long)g_skin_draws_pmidx,
           (unsigned long long)g_skin_defnz,
           (unsigned long long)g_skin_snapped);
    g_skin_loaded_rows = g_skin_used_rows = g_skin_used_missing = 0;
    g_skin_draws_total = g_skin_draws_pmidx = g_skin_defnz = g_skin_snapped = 0;
}

static uint64_t DLStateHash() {
    // R7 perf: recomputed only after a CP register write (~900 display-list
    // calls per frame on the world map, each hashed the 144 registers).
    static uint64_t s_hash = 0;
    if (!g_dlstate_dirty) return s_hash;
    g_dlstate_dirty = false;
    uint64_t h = 14695981039346656037ull;
    for (int r = 0x30; r <= 0xBF; ++r) { h ^= s_ps.cp[r]; h *= 1099511628211ull; }
    s_hash = h;
    return h;
}

// Whole-content hash of a display list (64-bit words, FNV-style mix): a
// sampled hash missed in-place edits of small lists (colour animation DLs).
// R22 R7: the same job with the CRC32C instruction (ARMv8 crc / x86 SSE4.2):
// four independent lanes over 32-byte blocks, ~8 bytes per cycle instead of
// ~2 (the castle cutscene hashes ~715 KB of display lists per frame, plus the
// 1 KB chunks of RangesHash). A change is missed with probability ~2^-32 per
// lane it touches. NWII_DLHASH=fnv: the multiply hash below.
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#define NWII_HAVE_CRC_HASH 1
#define NWII_CRC_ATTR
static inline uint32_t Crc64(uint32_t c, uint64_t w) { return __crc32cd(c, w); }
static inline uint32_t Crc8(uint32_t c, uint8_t b) { return __crc32cb(c, b); }
static inline bool CrcSupported() { return true; }
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define NWII_HAVE_CRC_HASH 1
#define NWII_CRC_ATTR __attribute__((target("sse4.2")))
NWII_CRC_ATTR static inline uint32_t Crc64(uint32_t c, uint64_t w) { return (uint32_t)_mm_crc32_u64(c, w); }
NWII_CRC_ATTR static inline uint32_t Crc8(uint32_t c, uint8_t b) { return _mm_crc32_u8(c, b); }
static inline bool CrcSupported() { static const bool ok = __builtin_cpu_supports("sse4.2"); return ok; }
#endif
#ifdef NWII_HAVE_CRC_HASH
NWII_CRC_ATTR static uint64_t DLContentHashCrc(const uint8_t* p, uint32_t size) {
    uint32_t c0 = 0x9E3779B9u ^ size, c1 = 0x85EBCA6Bu, c2 = 0xC2B2AE35u, c3 = 0x27D4EB2Fu;
    uint32_t i = 0;
    for (; i + 32 <= size; i += 32) {
        uint64_t w0, w1, w2, w3;
        std::memcpy(&w0, p + i, 8);
        std::memcpy(&w1, p + i + 8, 8);
        std::memcpy(&w2, p + i + 16, 8);
        std::memcpy(&w3, p + i + 24, 8);
        c0 = Crc64(c0, w0);
        c1 = Crc64(c1, w1);
        c2 = Crc64(c2, w2);
        c3 = Crc64(c3, w3);
    }
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        c0 = Crc64(c0, w);
    }
    for (; i < size; ++i) c1 = Crc8(c1, p[i]);
    uint64_t h = ((uint64_t)c0 << 32 | c1) ^ (((uint64_t)c2 << 32 | c3) * 0x9E3779B97F4A7C15ull);
    h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 29;
    return h ^ size;
}
#endif
static uint64_t DLContentHashFnv(const uint8_t* p, uint32_t size);
static uint64_t DLContentHash(const uint8_t* p, uint32_t size) {
#ifdef NWII_HAVE_CRC_HASH
    static const bool crc = [] {
        const char* e = NWII_ENV("NWII_DLHASH");
        return CrcSupported() && !(e && std::strcmp(e, "fnv") == 0);
    }();
    if (crc) return DLContentHashCrc(p, size);
#endif
    return DLContentHashFnv(p, size);
}
static uint64_t DLContentHashFnv(const uint8_t* p, uint32_t size) {
    // Four independent lanes over 32-byte blocks (R5 perf): the single
    // dependent multiply chain ran at ~1.5 GB/s and the party hashes ~45 MB/s
    // of display lists plus every indexed vertex array chunk.
    uint64_t h0 = 14695981039346656037ull ^ size, h1 = 0x9E3779B97F4A7C15ull,
             h2 = 0xC2B2AE3D27D4EB4Full, h3 = 0x165667B19E3779F9ull;
    uint32_t i = 0;
    for (; i + 32 <= size; i += 32) {
        uint64_t w0, w1, w2, w3;
        std::memcpy(&w0, p + i, 8);
        std::memcpy(&w1, p + i + 8, 8);
        std::memcpy(&w2, p + i + 16, 8);
        std::memcpy(&w3, p + i + 24, 8);
        h0 = (h0 ^ w0) * 1099511628211ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ w1) * 1099511628211ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ w2) * 1099511628211ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ w3) * 1099511628211ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0;
    h = (h ^ h1) * 1099511628211ull; h ^= h >> 29;
    h = (h ^ h2) * 1099511628211ull; h ^= h >> 29;
    h = (h ^ h3) * 1099511628211ull; h ^= h >> 29;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static bool RangesHashSpan(const std::pair<uint32_t, uint32_t>* ranges, size_t n, uint64_t& out);
static void ReplayMemo(DLCacheEntry& e, std::vector<GXCommand>& commands);
}  // namespace nwii::runtime::gx
namespace nwii::runtime::gx {
const void *GX_PrepDrawForRender(const GXCommand &cmd, std::vector<uint32_t> &pack);  // renderer_gl.cpp
}
namespace nwii::runtime::gx {

// Returns true if it noted what it appended in s_pidx itself (R22 R7).
static bool ExpandDisplayList(uint32_t addr, uint32_t size,
                              std::vector<GXCommand>& commands, int depth) {
    uint32_t phys = addr & 0x1FFFFFFF;
    // Lists may live in MEM1 or (Wii) MEM2; NW4R G3D keeps its model lists in
    // MEM2, so a MEM1-only bound silently drops every 3D model.
    const bool in_mem1 = phys + size <= 0x01800000;
    const bool in_mem2 = phys >= 0x10000000 && phys + size <= 0x14000000;
    if (!nwii::runtime::g_mmu || size == 0 || size > 0x400000 ||
        !(in_mem1 || in_mem2) || depth >= 4)
        return false;
    g_prof_dl_calls++;
    g_prof_dl_bytes += size;
    auto dl0 = std::chrono::steady_clock::now();

    static const bool nocache = NWII_ENV("NWII_NODLCACHE") != nullptr;
    const uint8_t* src = nwii::runtime::g_mmu->get_ptr(0x80000000u | phys);
    // Prefer the copy taken when the game actually made the call -- guest RAM
    // may already hold the next frame's list by the time we get here.
    std::shared_ptr<const std::vector<uint8_t>> snap =
        nwii::runtime::GX_DL_FetchShared(addr, size);
    if (snap) src = snap->data();

    if (src && !nocache && depth == 0) {
        // Sampled content hash: stride 512 plus the tail, cheap even for
        // multi-MB lists. DLs are effectively immutable once written.
        const uint64_t ch = DLContentHash(src, size);
        uint64_t sh = DLStateHash();
        // The parser state is part of the key: one list called under two
        // vertex formats keeps both parses instead of evicting each other.
        uint64_t key = (((uint64_t)phys << 23) ^ size) * 0x9E3779B97F4A7C15ull ^ sh;
        auto it = s_dl_cache.find(key);
        if (it != s_dl_cache.end() && it->second.state_hash == sh &&
            it->second.content_hash == ch) {
            g_prof_dl_hits++;
            it->second.last_use = g_gx_drain_epoch;
            if (s_pidx && depth == 0 && FifoParser::DLMemoOn() && !PreXf()) {
                // R22 R7: replay through the draw memo (notes what it appends).
                const size_t n0 = commands.size();
                ReplayMemo(it->second, commands);
                auto dl1 = std::chrono::steady_clock::now();
                g_prof_dl_us += std::chrono::duration_cast<std::chrono::microseconds>(dl1 - dl0).count();
                g_prof_dl_hit_us += std::chrono::duration_cast<std::chrono::nanoseconds>(dl1 - dl0).count();
                g_prof_dl_hit_cmds += commands.size() - n0;
                return true;
            }
            // Geometric growth: an exact reserve reallocated (and moved) the
            // whole frame's command vector on every display-list call once
            // it ran out of room (R7 perf).
            if (commands.capacity() < commands.size() + it->second.cmds.size())
                commands.reserve(std::max(commands.size() + it->second.cmds.size(),
                                          2 * commands.capacity()));
            const std::vector<GXCommand>& cached = it->second.cmds;
            const size_t ncached = cached.size();
            for (size_t ci = 0; ci < ncached; ++ci) {
                const GXCommand& c0 = cached[ci];
                // R7 perf: copying a draw bumps its DrawRaw's reference
                // count, one cache miss per draw: prefetch it a few ahead.
                if (ci + 6 < ncached)
                    if (const DrawRaw* r6 = cached[ci + 6].raw.get())
                        __builtin_prefetch((const char*)r6 - 8, 1);
                if (g_doomed && depth == 0 && c0.type == GXCommandType::DrawPrimitive) {  // r40d
                    ++s_light_skipped;
                    continue;
                }
                commands.push_back(c0);
                GXCommand& c = commands.back();
                // Replay parser-state side effects so main-stream commands
                // after the CALL still see the DL's CP/BP writes.
                if (c.type == GXCommandType::CPRegister)
                    ParseCP((uint8_t)c.reg, c.val);
                else if (c.type == GXCommandType::BPRegister)
                    c.val = ParseBPMasked((uint8_t)c.reg, c.bp_raw);
                else if (c.type == GXCommandType::XFRegister &&
                         (c.val & 0x80000000u)) {
                    // LOAD_INDX marker: re-read the (possibly animated)
                    // matrix data from guest RAM at today's array base.
                    int array = 12 + ((c.val >> 24) & 0x3);
                    int num = (c.val >> 16) & 0xFF;
                    uint32_t index = c.val & 0xFFFF;
                    uint32_t base = s_ps.arrayBase[array] +
                                    index * s_ps.arrayStride[array];
                    if (num > (int)c.payload.size() ||
                        !nwii::runtime::GX_IdxReadDL(array - 12, base, num, c.payload.data()))
                        for (int i = 0; i < num && i < (int)c.payload.size(); i++)
                            c.payload[i] =
                                nwii::runtime::g_mmu->read_f32(base + i * 4);
                    if (c.reg < 256 || (c.reg >= 0x400 && c.reg < 0x460))
                        ParseApplyMatrixWrite(c.reg, c.payload);
                } else if (c.type == GXCommandType::XFRegister &&
                           c.reg <= 0x1019 &&
                           c.reg + c.payload.size() > 0x1018) {
                    ParseXFMatrixIndex(c);
                } else if (c.type == GXCommandType::XFRegister &&
                           (c.reg < 256 || (c.reg >= 0x400 && c.reg < 0x460))) {
                    // Direct matrix write cached in the DL: feed the parse
                    // shadow so re-snapshots below stay live.
                    ParseApplyMatrixWrite(c.reg, c.payload);
                } else if (c.type == GXCommandType::DrawPrimitive && PreXf() &&
                           c.raw && c.raw->have_snap_mtx) {
                    // (have_snap_mtx is only ever set with PreXf(); testing
                    // it first skips a DrawRaw cache miss per draw. R7 perf)
                    // Re-snapshot the matrix from the current parse shadow so a
                    // cached skinned draw animates instead of freezing at the
                    // pose from when it was first parsed. (raw is shared_ptr;
                    // copy before mutating so the cache entry stays pristine.)
                    c.raw = MakeHost<DrawRaw>(*c.raw);
                    uint32_t pb = (uint32_t)c.raw->defPosMtxIdx * 4;
                    uint32_t nb = 3u * (c.raw->defPosMtxIdx & 31);
                    if (pb + 12 <= 256 && nb + 9 <= 96) {
                        std::memcpy(c.raw->snap_pos_mtx, &g_parse_posmtx[pb], 12 * sizeof(float));
                        std::memcpy(c.raw->snap_nrm_mtx, &g_parse_nrmmtx[nb], 9 * sizeof(float));
                    }
                }
            }
            auto dl1 = std::chrono::steady_clock::now();
            const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(dl1 - dl0).count();
            g_prof_dl_us += us;
            g_prof_dl_hit_us += std::chrono::duration_cast<std::chrono::nanoseconds>(dl1 - dl0).count();  // (ns)  // R22 R7
            g_prof_dl_hit_cmds += ncached;
            return false;
        }
        // Miss: parse into a local vector so it can be stored.
        std::vector<uint8_t> dl(src, src + size);
        size_t off = 0;
        std::vector<GXCommand> local;
        ParseStream(dl, off, local, depth + 1);
        // Evict by age, not all at once: the old "clear at 512" emptied the
        // cache every frame in Peach's party (~650 lists per frame), so it
        // never hit and every list was re-parsed and re-decoded each frame.
        // (R22 R7: the epoch now advances twice per frame, see gx.cpp: 16 = 8 frames.)
        if (s_dl_cache.size() >= 2048) {
            for (auto e = s_dl_cache.begin(); e != s_dl_cache.end();) {
                if (e->second.last_use + 16 < g_gx_drain_epoch) {
                    RetireEntry(e->second);
                    e = s_dl_cache.erase(e);
                } else {
                    e = std::next(e);
                }
            }
            if (s_dl_cache.size() >= 4096) {
                for (auto& kv : s_dl_cache) RetireEntry(kv.second);
                s_dl_cache.clear();
            }
        }
        {
            DLCacheEntry& ne = s_dl_cache[key];
            RetireEntry(ne);  // (an entry under the same key whose content or state changed)
            ne = DLCacheEntry{sh, ch, local, g_gx_drain_epoch, {}, {}, {}, 0, false, false};
        }
        if (g_doomed && depth == 0) {  // r40d NWII_D_LIGHTPARSE: the cache keeps the draws
            for (const GXCommand& c : local) {
                if (c.type == GXCommandType::DrawPrimitive) { ++s_light_skipped; continue; }
                commands.push_back(c);
            }
        } else {
            commands.insert(commands.end(), local.begin(), local.end());
        }
        auto dl1 = std::chrono::steady_clock::now();
        const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(dl1 - dl0).count();
        g_prof_dl_us += us;
        g_prof_dl_miss_us += std::chrono::duration_cast<std::chrono::nanoseconds>(dl1 - dl0).count();  // (ns)  // R22 R7
        return false;
    }

    std::vector<uint8_t> dl(size);
    if (src) {
        std::memcpy(dl.data(), src, size);
    } else {
        for (uint32_t i = 0; i < size; i++)
            dl[i] = nwii::runtime::g_mmu->read8(addr + i);
    }
    size_t off = 0;
    ParseStream(dl, off, commands, depth + 1);
    auto dl1 = std::chrono::steady_clock::now();
    g_prof_dl_us += std::chrono::duration_cast<std::chrono::microseconds>(dl1 - dl0).count();
    return false;
}

// Drain-path profiling counters, reported by ProcessGXFifo's [GXPROF] line.
// The 3D-scene drain still burns seconds somewhere after deferred decode;
// these split the cost by cause instead of guessing.
uint64_t g_prof_draws = 0;        // draws snapshotted
uint64_t g_prof_draw_bytes = 0;   // raw vertex bytes copied
uint64_t g_prof_dl_calls = 0;     // display-list expansions
uint64_t g_prof_dl_bytes = 0;     // display-list bytes copied+parsed
uint64_t g_prof_cmds = 0;         // commands emitted
uint64_t g_prof_unknown = 0;
uint64_t g_prof_dl_us = 0;      // time inside display-list expansion
uint64_t g_prof_snap_us = 0;    // time snapshotting draws
uint64_t g_prof_dl_loadindx = 0; // LOAD_INDX commands seen inside display lists      // unknown-opcode single-byte skips

namespace {
    // Byte width of one attribute in the vertex stream.
    inline size_t AttrBytes(VtxAttrMask mask, VtxAttrType type, int ncomp,
                            bool is_color) {
        switch (mask) {
        case VtxAttrMask::None:    return 0;
        case VtxAttrMask::Index8:  return 1;
        case VtxAttrMask::Index16: return 2;
        default:
            return is_color ? (size_t)ColorBytes(type)
                            : (size_t)TypeSize(type) * ncomp;
        }
    }

    size_t VertexSize(const VATSlot& vat) {
        size_t sz = vat.posMatIdx ? 1 : 0;
        for (int t = 0; t < 8; t++)
            if (vat.texMatIdx[t]) sz++;
        sz += AttrBytes(vat.posMask, vat.posType, vat.posElements ? 3 : 2, false);
        {
            size_t nrm = AttrBytes(vat.nrmMask, vat.nrmType,
                                   vat.nrmElements ? 9 : 3, false);
            // Three indices, not one, when NBT normals come in by index.
            if (vat.nrmIndex3 && vat.nrmElements &&
                (vat.nrmMask == VtxAttrMask::Index8 ||
                 vat.nrmMask == VtxAttrMask::Index16))
                nrm *= 3;
            sz += nrm;
        }
        for (int ci = 0; ci < 2; ci++)
            sz += AttrBytes(vat.clrMask[ci], vat.clrType[ci], 1, true);
        for (int t = 0; t < 8; t++)
            sz += AttrBytes(vat.texMask[t], vat.texType[t],
                            vat.texElements[t] ? 2 : 1, false);
        return sz;
    }
} // namespace

// NWII_PREXF=1 restores the parse-time matrix snapshot (pre-transformed
// vertices). Off by default: the renderer applies XF matrix loads in stream
// order, and the snapshot froze banked (per-vertex PNMTXIDX) draws on DL
// cache hits (audit findings 13/23).
static bool PreXf() {
    static const bool on = NWII_ENV("NWII_PREXF") != nullptr;
    return on;
}

uint64_t g_prof_decode_hits = 0;

// Hash of the guest memory a cached draw's indexed attributes read; false if
// a range is not plain contiguous RAM (then the draw is not cached).
// Range content hashes are memoised for one drain (g_gx_drain_epoch) in 1 KB
// chunks: the thousands of draws of a scene read overlapping parts of a few
// vertex arrays, which were hashed again for every draw (RangesHash was the
// top host-thread symbol in perf on the world map).
uint64_t g_gx_drain_epoch = 0;
static bool RangesHash(const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
                       uint64_t& out) {
    return RangesHashSpan(ranges.data(), ranges.size(), out);
}
// Direct-mapped memo, one slot per 1 KB of MEM1/MEM2 (R5 perf; was an
// unordered_map cleared every drain, the #3 host symbol in Peach's
// party). A range's chunks are adjacent slots, so a lookup is a stream
// instead of a hash probe. A slot written in an older drain is stale.
// (R22 R7: ChunkHash split out of RangesHashSpan, unchanged.)
struct ChunkSlot { uint64_t epoch; uint64_t hash; };
constexpr uint32_t kChunk = 1024;
constexpr uint32_t kMem1Chunks = 0x01800000u / kChunk;   // 24 MB
constexpr uint32_t kMem2Chunks = 0x04000000u / kChunk;   // 64 MB
static ChunkSlot* const s_chunk_memo = new ChunkSlot[kMem1Chunks + kMem2Chunks]();
static inline bool ChunkHash(uint32_t c, uint64_t ep, uint64_t& ch) {
    const uint32_t phys = (c * kChunk) & 0x1FFFFFFFu;
    ChunkSlot* sl = nullptr;
    if (phys < 0x01800000u)
        sl = &s_chunk_memo[phys / kChunk];
    else if (phys >= 0x10000000u && phys < 0x14000000u)
        sl = &s_chunk_memo[kMem1Chunks + (phys - 0x10000000u) / kChunk];
    if (sl && sl->epoch == ep) {
        ch = sl->hash;
        return true;
    }
    const uint8_t* p = nwii::runtime::g_mmu->get_ptr(c * kChunk);
    const uint8_t* q = nwii::runtime::g_mmu->get_ptr(c * kChunk + kChunk - 1);
    if (!p || !q || q - p != (ptrdiff_t)kChunk - 1) return false;
    ch = DLContentHash(p, kChunk);
    if (sl) *sl = {ep, ch};
    return true;
}
static bool RangesHashSpan(const std::pair<uint32_t, uint32_t>* ranges, size_t n, uint64_t& out) {
    const uint64_t ep = g_gx_drain_epoch + 1;  // 0 = never written
    uint64_t h = 14695981039346656037ull;
    for (size_t ri = 0; ri < n; ++ri) {
        const auto& rg = ranges[ri];
        if (!rg.second) continue;
        const uint32_t c0 = rg.first / kChunk, c1 = (rg.first + rg.second - 1) / kChunk;
        for (uint32_t c = c0; c <= c1; ++c) {
            uint64_t ch;
            if (!ChunkHash(c, ep, ch)) return false;
            h = ch ^ (h * 1099511628211ull);
        }
        h ^= ((uint64_t)rg.first << 32) | rg.second;
    }
    out = h;
    return true;
}
// R22 R7: hash of a display list's chunk set (DLCacheEntry::chunks).
static bool ChunksHash(const std::vector<uint32_t>& chunks, uint64_t& out) {
    const uint64_t ep = g_gx_drain_epoch + 1;
    uint64_t h = 0x84222325CBF29CE4ull ^ chunks.size();
    for (uint32_t c : chunks) {
        uint64_t ch;
        if (!ChunkHash(c, ep, ch)) return false;
        h = (h ^ ch) * 1099511628211ull;
        h ^= h >> 29;
    }
    out = h;
    return true;
}

static bool FastDecodeOn() {
    static const bool on = [] { const char* e = NWII_ENV("NWII_D_FASTDECODE"); return e && e[0] && e[0] != '0'; }();
    return on;
}
static bool FastDecodeCheckOn() {
    static const bool on = NWII_ENV("NWII_D_FASTDECODE_CHECK") != nullptr;
    return on;
}
static bool VertsSame(const std::vector<VertexData>& a, const std::vector<VertexData>& b) {
    if (a.size() != b.size()) return false;
    auto feq = [](const float* x, const float* y, int k) { return std::memcmp(x, y, k * sizeof(float)) == 0; };
    for (size_t i = 0; i < a.size(); ++i) {
        const VertexData &x = a[i], &y = b[i];
        if (!(feq(x.pos, y.pos, 3) && feq(x.norm, y.norm, 3) && feq(x.color, y.color, 4) &&
              feq(&x.tex[0][0], &y.tex[0][0], 16) && feq(x.raw_pos, y.raw_pos, 3) &&
              feq(x.raw_norm, y.raw_norm, 3) && x.has_raw_norm == y.has_raw_norm && x.has_pos == y.has_pos &&
              x.has_norm == y.has_norm && x.pre_xf == y.pre_xf && x.has_color == y.has_color &&
              std::memcmp(x.has_tex, y.has_tex, 8) == 0 && x.posMtxIdx == y.posMtxIdx &&
              std::memcmp(x.texMtxIdx, y.texMtxIdx, 8) == 0))
            return false;
    }
    return true;
}
static void DecodeDrawImpl(GXCommand& c);
void FifoParser::DecodeDraw(GXCommand& c) {
    if (c.type != GXCommandType::DrawPrimitive || !c.raw || !c.vertices.empty() ||
        c.shared_vertices)
        return;
    if (!FastDecodeOn()) { DecodeDrawImpl(c); return; }
    // r40d NWII_D_FASTDECODE (see DecodeScalar).
    HostPtr<DrawRaw> keep;
    const bool chk = FastDecodeCheckOn();
    if (chk) keep = c.raw;  // (the decode drops c.raw)
    g_fastdec = true;
    DecodeDrawImpl(c);
    g_fastdec = false;
    if (chk && keep) {
        static uint64_t n = 0, bad = 0;
        GXCommand t;
        t.type = GXCommandType::DrawPrimitive;
        t.prim_type = c.prim_type;
        auto r2 = MakeHost<DrawRaw>(*keep);
        r2->decoded.reset();
        r2->no_decode_cache = true;
        t.raw = r2;
        DecodeDrawImpl(t);
        const std::vector<VertexData>& got = c.shared_vertices ? static_cast<const std::vector<VertexData>&>(*c.shared_vertices) : c.vertices;
        ++n;
        if (!VertsSame(got, t.vertices)) ++bad;
        if ((bad && bad <= 20 && !VertsSame(got, t.vertices)) || n % 200000 == 0)
            printf("[D40DEC_CHECK] draws=%llu mismatches=%llu\n", (unsigned long long)n, (unsigned long long)bad);
    }
}
static void DecodeDrawImpl(GXCommand& c) {
    DrawRaw& r = *c.raw;
    static const bool no_dcache = NWII_ENV("NWII_NODECODECACHE") != nullptr;
    const bool cacheable = r.in_dl && !r.no_decode_cache && !no_dcache && !PreXf() &&
                           r.count <= 8192 && nwii::runtime::g_mmu;
    if (cacheable && r.decoded) {
        uint64_t h;
        if (RangesHash(r.ranges, h) && h == r.ranges_hash) {
            c.shared_vertices = r.decoded;
            ++g_prof_decode_hits;
            c.raw.reset();
            return;
        }
        r.decoded.reset();
    }
    RangeTrack track;
    std::memset(&track, 0, sizeof(track));
    g_track = cacheable ? &track : nullptr;
    struct ClearTrack { ~ClearTrack() { g_track = nullptr; } } clear_track;
    const std::vector<uint8_t>& buf = r.bytes;
    const size_t buf_size = buf.size();
    size_t off = 0;
    c.vertices.reserve(r.count);
    for (uint32_t i = 0; i < r.count; i++) {
        // r40d NWII_D_FASTDECODE: the vertex is built in place (no 200-byte
        // copy per vertex; r.count was reserved, so no reallocation); a vertex
        // that fails to decode is taken back out, as it was never pushed.
        std::optional<VertexData> vtx_old;
        const bool inplace = g_fastdec;
        if (!inplace) vtx_old.emplace();
        VertexData& vtx = inplace ? c.vertices.emplace_back() : *vtx_old;
        struct PopIfFailed {
            std::vector<VertexData>& v;
            bool on, done = false;
            ~PopIfFailed() { if (on && !done) v.pop_back(); }
        } pop_if_failed{c.vertices, inplace};
        vtx.posMtxIdx = r.defPosMtxIdx;
        for (int t = 0; t < 8; t++)
            vtx.texMtxIdx[t] = r.defTexMtxIdx[t];
        if (r.vat.posMatIdx) {
            if (off + 1 > buf_size) return;
            vtx.posMtxIdx = buf[off++];
        }
        if (g_skindbg && vtx.posMtxIdx < 64) {
            g_skin_used_rows |= (1ull << vtx.posMtxIdx);
            if (!(g_skin_loaded_rows & (1ull << vtx.posMtxIdx)))
                g_skin_used_missing |= (1ull << vtx.posMtxIdx);
        }
        for (int t = 0; t < 8; t++) {
            if (r.vat.texMatIdx[t]) {
                if (off + 1 > buf_size) return;
                vtx.texMtxIdx[t] = buf[off++];
            }
        }
        vtx.has_pos = (r.vat.posMask != VtxAttrMask::None);
        if (!ReadVectorAttr(buf, off, buf_size, r.vat.posMask, r.vat.posType,
                            r.vat.posShift, r.arrayBase[0], r.arrayStride[0],
                            r.vat.posElements ? 3 : 2, vtx.pos, 0))
            return;
        vtx.raw_pos[0] = vtx.pos[0]; vtx.raw_pos[1] = vtx.pos[1];
        vtx.raw_pos[2] = r.vat.posElements ? vtx.pos[2] : 0.0f;
        // CLR1 and tex1-7 are parsed for stream position only (the renderer
        // uses clr0/tex0). Normals are decoded only for lit draws.
        // Three indices when NBT normals arrive by index: consume the extra two
        // so the stream position stays right even though only the normal is
        // used. Getting this wrong is invisible on this vertex and fatal on
        // every one after it.
        const bool nrm_idx3 =
            r.vat.nrmIndex3 && r.vat.nrmElements &&
            (r.vat.nrmMask == VtxAttrMask::Index8 ||
             r.vat.nrmMask == VtxAttrMask::Index16);
        const size_t nrm_idx_bytes =
            (r.vat.nrmMask == VtxAttrMask::Index16) ? 2 : 1;

        // Normals feed lighting (need_normal) and also normal-sourced texgen
        // (NW4R light/env maps), so they are always decoded when present.
        if (r.vat.nrmMask != VtxAttrMask::None) {
            // Lit or not is a render-time question (XF 0x100E/F are applied
            // in stream order by the renderer), so normals are always kept.
            vtx.has_norm = true;
            // Normals have a fixed fraction (Dolphin FracAdjust): U8 7,
            // S8 6, U16 15, S16 14 bits.
            uint8_t nfrac = 0;
            switch (r.vat.nrmType) {
                case VtxAttrType::U8:  nfrac = 7; break;
                case VtxAttrType::S8:  nfrac = 6; break;
                case VtxAttrType::U16: nfrac = 15; break;
                case VtxAttrType::S16: nfrac = 14; break;
                default: break;
            }
            float nbt[9];
            if (!ReadVectorAttr(buf, off, buf_size, r.vat.nrmMask, r.vat.nrmType,
                                nfrac, r.arrayBase[1], r.arrayStride[1],
                                r.vat.nrmElements ? 9 : 3, nbt, 1))
                return;
            if (nrm_idx3) off += 2 * nrm_idx_bytes;
            vtx.norm[0] = nbt[0]; vtx.norm[1] = nbt[1]; vtx.norm[2] = nbt[2];
            vtx.raw_norm[0] = nbt[0]; vtx.raw_norm[1] = nbt[1]; vtx.raw_norm[2] = nbt[2];
            vtx.has_raw_norm = true;
        }
        if (r.vat.clrMask[0] != VtxAttrMask::None) {
            float col[4];
            if (!ReadColorAttr(buf, off, buf_size, r.vat.clrMask[0],
                               r.vat.clrType[0], r.arrayBase[2],
                               r.arrayStride[2], col, 2))
                return;
            vtx.has_color = true;
            vtx.color[0] = col[0]; vtx.color[1] = col[1];
            vtx.color[2] = col[2]; vtx.color[3] = col[3];
        }
        off += AttrBytes(r.vat.clrMask[1], r.vat.clrType[1], 1, true);
        if (r.vat.texMask[0] != VtxAttrMask::None) {
            vtx.has_tex[0] = true;
            if (!ReadVectorAttr(buf, off, buf_size, r.vat.texMask[0],
                                r.vat.texType[0], r.vat.texShift[0],
                                r.arrayBase[4], r.arrayStride[4],
                                r.vat.texElements[0] ? 2 : 1, vtx.tex[0], 4))
                return;
        }
        for (int t = 1; t < 8; t++) {
            if (r.vat.texMask[t] == VtxAttrMask::None)
                continue;
            vtx.has_tex[t] = true;
            vtx.tex[t][1] = 0.0f;  // one-component coords leave T unset
            if (!ReadVectorAttr(buf, off, buf_size, r.vat.texMask[t],
                                r.vat.texType[t], r.vat.texShift[t],
                                r.arrayBase[4 + t], r.arrayStride[4 + t],
                                r.vat.texElements[t] ? 2 : 1, vtx.tex[t], 4 + t))
                return;
        }
        if (off > buf_size)
            return;

        // Transform by the per-draw matrix snapshot (GX 3x4). Doing it here,
        // with the matrix captured at this draw's stream position, fixes the
        // matrix-swap skinning scramble where a live render-time lookup would
        // use whatever matrix was written last.
        if (!PreXf()) {
            // Live, stream-ordered matrices in the renderer (Dolphin).
        } else if (!r.snap_pos_bank.empty() && vtx.has_pos) {
            const uint32_t pb = (uint32_t)vtx.posMtxIdx * 4;
            const uint32_t nb = 3u * (vtx.posMtxIdx & 31);
            if (pb + 12 <= 256 && nb + 9 <= 96) {
                const float* m = &r.snap_pos_bank[pb];
                float x = vtx.pos[0], y = vtx.pos[1], z = vtx.pos[2];
                vtx.pos[0] = m[0]*x + m[1]*y + m[2]*z + m[3];
                vtx.pos[1] = m[4]*x + m[5]*y + m[6]*z + m[7];
                vtx.pos[2] = m[8]*x + m[9]*y + m[10]*z + m[11];
                if (vtx.has_norm && !r.snap_nrm_bank.empty()) {
                    const float* n = &r.snap_nrm_bank[nb];
                    float a = vtx.norm[0], b = vtx.norm[1], cc = vtx.norm[2];
                    vtx.norm[0] = n[0]*a + n[1]*b + n[2]*cc;
                    vtx.norm[1] = n[3]*a + n[4]*b + n[5]*cc;
                    vtx.norm[2] = n[6]*a + n[7]*b + n[8]*cc;
                }
                vtx.pre_xf = true;
            }
        } else if (r.have_snap_mtx && vtx.has_pos) {
            const float* m = r.snap_pos_mtx;
            float x = vtx.pos[0], y = vtx.pos[1], z = vtx.pos[2];
            vtx.pos[0] = m[0]*x + m[1]*y + m[2]*z + m[3];
            vtx.pos[1] = m[4]*x + m[5]*y + m[6]*z + m[7];
            vtx.pos[2] = m[8]*x + m[9]*y + m[10]*z + m[11];
            if (vtx.has_norm) {
                const float* n = r.snap_nrm_mtx;
                float a = vtx.norm[0], b = vtx.norm[1], cc = vtx.norm[2];
                vtx.norm[0] = n[0]*a + n[1]*b + n[2]*cc;
                vtx.norm[1] = n[3]*a + n[4]*b + n[5]*cc;
                vtx.norm[2] = n[6]*a + n[7]*b + n[8]*cc;
            }
            vtx.pre_xf = true;
        }
        if (!inplace) c.vertices.push_back(vtx);
        pop_if_failed.done = true;
    }
    if (cacheable && c.vertices.size() == r.count) {
        r.ranges.clear();
        for (int k = 0; k < 16; ++k)
            if (track.used[k])
                r.ranges.push_back({track.lo[k], track.hi[k] - track.lo[k]});
        uint64_t h;
        if (RangesHash(r.ranges, h)) {
            r.ranges_hash = h;
            auto sp = MakeHost<DecodedVerts>(std::move(c.vertices));
            c.vertices.clear();
            r.decoded = sp;
            c.shared_vertices = std::move(sp);
        } else {
            r.no_decode_cache = true;
        }
    }
    c.raw.reset(); // decoded: drop the raw copy
}

// R22 R7: a display-list cache hit replayed through its draw memo (see
// DLDrawMemo). Same commands and parser side effects as the plain replay in
// ExpandDisplayList (PreXf off); each draw whose memo is valid is written from
// the memo, any other one is decoded right here (DecodeDraw, as the frame-end
// pass would) and its memo refreshed. Notes every appended command in s_pidx.
// NWII_DLMEMO_CHECK=1 (test): every draw written from its memo is decoded again
// from its raw bytes, without any cache, and compared with the memo's vertices.
static void MemoCheck(const GXCommand& c0, const DecodedVerts& dv) {
    static uint64_t n = 0, bad = 0;
    GXCommand t = c0;
    auto r2 = MakeHost<DrawRaw>(*c0.raw);
    r2->decoded.reset();
    r2->no_decode_cache = true;
    t.raw = r2;
    FifoParser::DecodeDraw(t);
    const std::vector<VertexData>& a = t.vertices;
    bool ok = a.size() == dv.size();
    auto feq = [](const float* x, const float* y, int k) { return std::memcmp(x, y, k * sizeof(float)) == 0; };
    for (size_t i = 0; ok && i < a.size(); ++i) {
        const VertexData &x = a[i], &y = dv[i];
        ok = feq(x.pos, y.pos, 3) && feq(x.norm, y.norm, 3) && feq(x.color, y.color, 4) &&
             feq(&x.tex[0][0], &y.tex[0][0], 16) && feq(x.raw_pos, y.raw_pos, 3) &&
             feq(x.raw_norm, y.raw_norm, 3) && x.has_raw_norm == y.has_raw_norm && x.has_pos == y.has_pos &&
             x.has_norm == y.has_norm && x.pre_xf == y.pre_xf && x.has_color == y.has_color &&
             std::memcmp(x.has_tex, y.has_tex, 8) == 0 && x.posMtxIdx == y.posMtxIdx &&
             std::memcmp(x.texMtxIdx, y.texMtxIdx, 8) == 0;
    }
    ++n;
    if (!ok) ++bad;
    if ((!ok && bad <= 20) || n % 200000 == 0)
        printf("[DLMEMO_CHECK] checked=%llu mismatches=%llu\n", (unsigned long long)n, (unsigned long long)bad);
}

// r45p NWII_P_DLMERGE (see DLFuseGroup)
static bool DLMergeOn() {
    static const bool on = [] {
        const char* v = NWII_ENV("NWII_P_DLMERGE");
        const bool o = v && v[0] == '1';
        if (o) std::printf("[P45DL] NWII_P_DLMERGE=1: draws seguidos de una lista sin ordenes en medio fundidos en uno\n");
        return o;
    }();
    return on;
}
static constexpr size_t kDLMergeMaxParts = 1024;
static constexpr size_t kDLMergeMaxVerts = 4096;
static uint64_t s_dlm_groups = 0, s_dlm_parts = 0, s_dlm_made = 0, s_dlm_bad = 0;
static inline bool SameVtxFlags(const VertexData& a, const VertexData& b) {
    return a.has_pos == b.has_pos && a.has_norm == b.has_norm && a.pre_xf == b.pre_xf &&
           a.has_color == b.has_color && std::memcmp(a.has_tex, b.has_tex, sizeof a.has_tex) == 0;
}
// Every part valid now (as the plain memo path tests it) and the fused vertices made of the parts'
// current ones (made again if a part changed). False: replay the parts one by one this time.
static bool RangesHashSpan(const std::pair<uint32_t, uint32_t>* ranges, size_t n, uint64_t& out);
static bool DLFuseReady(DLCacheEntry& e, DLFuseGroup& G, bool list_ok) {
    for (uint32_t j = G.k0; j < G.k1; ++j) {
        const DLDrawMemo& mj = e.memo[j];
        if (!mj.dv || !mj.has_entry || mj.dv->empty()) return false;
        if (!list_ok) {
            uint64_t h;
            if (!(RangesHashSpan(e.ranges.data() + mj.r0, mj.rn, h) && h == mj.hash)) return false;
        }
    }
    bool same = G.dv && G.src.size() == G.k1 - G.k0;
    for (uint32_t j = G.k0; same && j < G.k1; ++j) same = G.src[j - G.k0] == e.memo[j].dv.get();
    if (same) return true;
    size_t total = 0;
    const VertexData& f0 = (*e.memo[G.k0].dv)[0];
    for (uint32_t j = G.k0; j < G.k1; ++j) {
        const DecodedVerts& d = *e.memo[j].dv;
        total += d.size();
        if (d.size() > 0xFFFF || !d.parts.empty()) { G.bad = true; ++s_dlm_bad; return false; }
        for (const VertexData& vd : d)
            if (!SameVtxFlags(vd, f0)) { G.bad = true; ++s_dlm_bad; return false; }
    }
    if (total > kDLMergeMaxVerts) { G.bad = true; ++s_dlm_bad; return false; }
    std::vector<VertexData> all;
    all.reserve(total);
    std::vector<uint16_t> parts;
    parts.reserve(G.k1 - G.k0);
    G.src.clear();
    for (uint32_t j = G.k0; j < G.k1; ++j) {
        const DecodedVerts& d = *e.memo[j].dv;
        all.insert(all.end(), d.begin(), d.end());
        parts.push_back((uint16_t)d.size());
        G.src.push_back(&d);
    }
    auto nd = MakeHost<DecodedVerts>(std::move(all));
    nd->parts = std::move(parts);
    nd->part_dv.reserve(G.k1 - G.k0);
    for (uint32_t j = G.k0; j < G.k1; ++j) nd->part_dv.push_back(e.memo[j].dv);
    Retire(G.dv);
    G.dv = nd;
    GXCommand c;
    c.type = GXCommandType::DrawPrimitive;
    c.prim_type = e.memo[G.k0].prim_type;
    c.shared_vertices = nd;
    static std::vector<uint32_t> s_no_pack2;  // (display-list draws never write GPU XF words)
    G.entry = GX_PrepDrawForRender(c, s_no_pack2);
    ++s_dlm_made;
    return true;
}
void FifoParser::DLMergeStats(uint64_t& groups, uint64_t& parts, uint64_t& made, uint64_t& bad) {
    groups = s_dlm_groups; parts = s_dlm_parts; made = s_dlm_made; bad = s_dlm_bad;
    s_dlm_groups = s_dlm_parts = s_dlm_made = 0;
}
static void ReplayMemo(DLCacheEntry& e, std::vector<GXCommand>& commands) {
    const std::vector<GXCommand>& cached = e.cmds;
    const size_t ncached = cached.size();
    if (!e.memo_init || e.ranges.size() > (1u << 16)) {
        for (DLDrawMemo& m : e.memo) Retire(m.dv);
        e.memo.clear();
        for (DLFuseGroup& g : e.fgroups) Retire(g.dv);  // r45p
        e.fgroups.clear();
        e.fg_of.clear();
        e.fg_init = false;
        for (size_t ci = 0; ci < ncached; ++ci) {
            const GXCommand& c = cached[ci];
            if (c.type != GXCommandType::DrawPrimitive) continue;
            DLDrawMemo m;
            m.ci = (uint32_t)ci;
            m.prim_type = c.prim_type;
            m.stale_copy = c.stale_copy;
            m.reg = c.reg;
            m.length = c.length;
            m.val = c.val;
            m.bp_raw = c.bp_raw;
            e.memo.push_back(std::move(m));
        }
        e.ranges.clear();
        e.chunks.clear();
        e.chunks_ok = false;
        e.memo_init = true;
    }
    if (DLMergeOn() && !e.fg_init) {  // r45p: the runs of the list (from its commands only)
        e.fg_init = true;
        e.fg_of.assign(e.memo.size(), -1);
        const size_t nmm = e.memo.size();
        for (size_t j = 0; j < nmm;) {
            const uint8_t prim = e.memo[j].prim_type & 0xF8;
            size_t q = j + 1;
            if (prim == 0x80 || prim == 0x90 || prim == 0x98 || prim == 0xA0)
                while (q < nmm && e.memo[q].ci == e.memo[q - 1].ci + 1 && (e.memo[q].prim_type & 0xF8) == prim &&
                       q - j < kDLMergeMaxParts)
                    ++q;
            if (q - j >= 2) {
                e.fg_of[j] = (int32_t)e.fgroups.size();
                DLFuseGroup g;
                g.k0 = (uint32_t)j;
                g.k1 = (uint32_t)q;
                e.fgroups.push_back(std::move(g));
                j = q;
            } else {
                j = j + 1;
            }
        }
    }
    static const bool s_dlvalid = [] { const char* v = NWII_ENV("NWII_DLMEMO_LIST"); return !(v && v[0] == '0'); }();
    bool list_ok = false;  // every memo valid: the list's chunks did not change
    if (s_dlvalid && e.chunks_ok) {
        uint64_t hc;
        list_ok = ChunksHash(e.chunks, hc) && hc == e.chunks_hash;
    }
    bool all_valid = true, ranges_changed = false;
    if (commands.capacity() < commands.size() + ncached)
        commands.reserve(std::max(commands.size() + ncached, 2 * commands.capacity()));
    static std::vector<uint32_t> s_no_pack;  // (display-list draws never write GPU XF words)
    FifoParser::Index& ix = *s_pidx;
    size_t k = 0;  // next memo
    const size_t nm = e.memo.size();
    size_t next_draw = nm ? e.memo[0].ci : ncached;
    const bool doomed = g_doomed;  // r40d NWII_D_LIGHTPARSE (ReplayMemo runs at depth 0)
    for (size_t ci = 0; ci < ncached; ++ci) {
        if (ci == next_draw) {
            DLDrawMemo& m = e.memo[k++];
            next_draw = k < nm ? e.memo[k].ci : ncached;
            if (doomed) {  // a dropped frame's draw: not written, its memo left as it is
                ++s_light_skipped;
                continue;
            }
            if (e.fg_init && e.fg_of.size() == nm && e.fg_of[k - 1] >= 0) {  // r45p NWII_P_DLMERGE
                DLFuseGroup& G = e.fgroups[(size_t)e.fg_of[k - 1]];
                if (!G.bad && DLFuseReady(e, G, list_ok)) {
                    commands.emplace_back();
                    GXCommand& c = commands.back();
                    c.type = GXCommandType::DrawPrimitive;
                    c.reg = m.reg;
                    c.val = m.val;
                    c.bp_raw = m.bp_raw;
                    c.stale_copy = m.stale_copy;
                    c.length = m.length;
                    c.prim_type = m.prim_type;
                    c.shared_vertices = HostPtr<const DecodedVerts>(HostPtr<const DecodedVerts>(), G.dv.get());
                    g_prof_decode_hits += G.k1 - G.k0;
                    ix.draws.push_back((uint32_t)(commands.size() - 1));
                    ix.ents.push_back(G.entry);
                    ++s_dlm_groups;
                    s_dlm_parts += G.k1 - G.k0;
                    ci = e.memo[G.k1 - 1].ci;  // the parts are consecutive commands: skip them
                    k = G.k1;
                    next_draw = k < nm ? e.memo[k].ci : ncached;
                    continue;
                }
            }
            if (k + 2 < nm) {
                const DLDrawMemo& m2 = e.memo[k + 2];
                __builtin_prefetch(&m2);
                if (m2.rn && !list_ok) __builtin_prefetch(&e.ranges[m2.r0]);
            }
            uint64_t h;
            if (m.dv && m.has_entry &&
                (list_ok || (RangesHashSpan(e.ranges.data() + m.r0, m.rn, h) && h == m.hash))) {
                commands.emplace_back();
                GXCommand& c = commands.back();
                c.type = GXCommandType::DrawPrimitive;
                c.reg = m.reg;
                c.val = m.val;
                c.bp_raw = m.bp_raw;
                c.stale_copy = m.stale_copy;
                c.length = m.length;
                c.prim_type = m.prim_type;
                // Not owning (no control block): the memo keeps it alive.
                c.shared_vertices = HostPtr<const DecodedVerts>(HostPtr<const DecodedVerts>(), m.dv.get());
                ++g_prof_decode_hits;
                ix.draws.push_back((uint32_t)(commands.size() - 1));
                ix.ents.push_back(m.entry);
                static const bool s_mcheck = NWII_ENV("NWII_DLMEMO_CHECK") != nullptr;
                if (s_mcheck && cached[ci].raw) MemoCheck(cached[ci], *m.dv);
                continue;
            }
            const GXCommand& c0 = cached[ci];
            commands.push_back(c0);
            GXCommand& c = commands.back();
            const DrawRaw* r = c0.raw.get();  // (the cache entry keeps it)
            FifoParser::DecodeDraw(c);
            ix.draws.push_back((uint32_t)(commands.size() - 1));
            if (r && c.shared_vertices && r->decoded && c.shared_vertices.get() == r->decoded.get()) {
                if (m.dv.get() != r->decoded.get()) {
                    Retire(m.dv);
                    m.dv = r->decoded;
                }
                m.hash = r->ranges_hash;
                if (m.rn != r->ranges.size() || m.r0 + m.rn > e.ranges.size()) {
                    m.r0 = (uint32_t)e.ranges.size();
                    m.rn = (uint32_t)r->ranges.size();
                    e.ranges.insert(e.ranges.end(), r->ranges.begin(), r->ranges.end());
                    ranges_changed = true;
                } else if (!std::equal(r->ranges.begin(), r->ranges.end(), e.ranges.begin() + m.r0)) {
                    std::copy(r->ranges.begin(), r->ranges.end(), e.ranges.begin() + m.r0);
                    ranges_changed = true;
                }
                m.entry = GX_PrepDrawForRender(c, s_no_pack);
                m.has_entry = true;
                // Like the fast path: the frame's command does not own it.
                c.shared_vertices = HostPtr<const DecodedVerts>(HostPtr<const DecodedVerts>(), m.dv.get());
                ix.ents.push_back(m.entry);
            } else {
                Retire(m.dv);
                m.has_entry = false;
                all_valid = false;
                ix.ents.push_back(FifoParser::kEntryUnknown);
            }
            continue;
        }
        const GXCommand& c0 = cached[ci];
        if (ci + 4 < ncached) {
            const char* pf = reinterpret_cast<const char*>(&cached[ci + 4]);
            __builtin_prefetch(pf);
            __builtin_prefetch(pf + 64);
        }
        commands.push_back(c0);
        GXCommand& c = commands.back();
        // Parser side effects, exactly as the plain replay.
        if (c.type == GXCommandType::CPRegister) {
            ParseCP((uint8_t)c.reg, c.val);
        } else if (c.type == GXCommandType::BPRegister) {
            c.val = ParseBPMasked((uint8_t)c.reg, c.bp_raw);
            if (c.reg == 0x52 && (c.val & 0x4000)) ix.xfb_copies.push_back((uint32_t)(commands.size() - 1));
        } else if (c.type == GXCommandType::XFRegister && (c.val & 0x80000000u)) {
            int array = 12 + ((c.val >> 24) & 0x3);
            int num = (c.val >> 16) & 0xFF;
            uint32_t index = c.val & 0xFFFF;
            uint32_t base = s_ps.arrayBase[array] + index * s_ps.arrayStride[array];
            if (num > (int)c.payload.size() ||
                !nwii::runtime::GX_IdxReadDL(array - 12, base, num, c.payload.data()))
                for (int i = 0; i < num && i < (int)c.payload.size(); i++)
                    c.payload[i] = nwii::runtime::g_mmu->read_f32(base + i * 4);
            if (c.reg < 256 || (c.reg >= 0x400 && c.reg < 0x460))
                ParseApplyMatrixWrite(c.reg, c.payload);
        } else if (c.type == GXCommandType::XFRegister && c.reg <= 0x1019 &&
                   c.reg + c.payload.size() > 0x1018) {
            ParseXFMatrixIndex(c);
        } else if (c.type == GXCommandType::XFRegister &&
                   (c.reg < 256 || (c.reg >= 0x400 && c.reg < 0x460))) {
            ParseApplyMatrixWrite(c.reg, c.payload);
        }
    }
    if (doomed) {  // r40d: the memos were not checked: nothing is known about the list's chunks
        if (s_dlvalid && !list_ok) e.chunks_ok = false;
        return;
    }
    if (s_dlvalid && !list_ok) {
        // Every memo is valid now (checked or refreshed against this epoch's
        // chunk hashes): remember the list's chunks and their hash.
        e.chunks_ok = false;
        if (all_valid) {
            if (ranges_changed || e.chunks.empty()) {
                e.chunks.clear();
                for (const DLDrawMemo& m : e.memo)
                    for (uint32_t ri = m.r0; ri < m.r0 + m.rn; ++ri) {
                        const auto& rg = e.ranges[ri];
                        if (!rg.second) continue;
                        for (uint32_t c = rg.first / kChunk; c <= (rg.first + rg.second - 1) / kChunk; ++c)
                            e.chunks.push_back(c);
                    }
                std::sort(e.chunks.begin(), e.chunks.end());
                e.chunks.erase(std::unique(e.chunks.begin(), e.chunks.end()), e.chunks.end());
            }
            e.chunks_ok = ChunksHash(e.chunks, e.chunks_hash);
        }
    }
}

static uint32_t s_stop_xfb = 0;  // r40d NWII_D_PARSESTOP (see the header)
static bool s_stopped_xfb = false;
void FifoParser::SetStopAfterXfb(uint32_t n) { s_stop_xfb = n; }
void FifoParser::SetDoomedXfb(uint32_t n) { s_doom_xfb = n; }
uint64_t FifoParser::LightSkipped() { return s_light_skipped; }
void FifoParser::LightTime(uint64_t& ns, uint64_t& frames) { ns = s_light_ns; frames = s_light_frames; }
bool FifoParser::StoppedAtXfb() { return s_stopped_xfb; }
void FifoParser::Parse(std::vector<uint8_t>& fifo, std::vector<GXCommand>& commands, Index* idx) {
    s_stopped_xfb = false;
    if (s_doom_xfb) s_parse_t0 = std::chrono::steady_clock::now();
    s_pidx = idx;
    Parse(fifo, commands);
    s_pidx = nullptr;
    g_doomed = false;
}

void FifoParser::Parse(std::vector<uint8_t>& fifo, std::vector<GXCommand>& commands) {
    static bool skin_init = (g_skindbg = NWII_ENV("NWII_SKINDBG") != nullptr, true);
    (void)skin_init;
    if (g_skindbg) skin_report();
    size_t offset = 0;
    ParseStream(fifo, offset, commands, 0);

    
    if (offset > 0 && offset <= fifo.size())
        fifo.erase(fifo.begin(), fifo.begin() + offset);
}

static void ParseStream(const std::vector<uint8_t>& fifo, size_t& offset, std::vector<GXCommand>& commands, int depth) {
    const size_t fifo_size = fifo.size();
    // R22 R7: note the depth-0 commands for Parse()'s index as they are written.
    const bool note = depth == 0 && s_pidx;
    size_t noted = commands.size();
    struct FlushNotes {
        const bool on; size_t& noted; std::vector<GXCommand>& cmds;
        ~FlushNotes() { if (on) for (; noted < cmds.size(); ++noted) NoteCmd(cmds[noted], (uint32_t)noted); }
    } flush_notes{note, noted, commands};
    while (offset < fifo_size) {
        if (note) for (; noted < commands.size(); ++noted) NoteCmd(commands[noted], (uint32_t)noted);
        // r40d NWII_D_PARSESTOP: stop right after the frame end the caller asked for (a whole
        // command was parsed: the rest of the bytes start at a command boundary).
        if (note && s_stop_xfb && s_pidx->xfb_copies.size() >= s_stop_xfb) {
            s_stopped_xfb = true;
            break;
        }
        if (depth == 0) {  // r40d
            const bool was = g_doomed;
            g_doomed = note && s_doom_xfb && s_pidx->xfb_copies.size() < s_doom_xfb;
            if (was && !g_doomed) {
                s_light_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - s_parse_t0).count();
                s_light_frames += s_doom_xfb;
            }
        }
        uint8_t cmd = fifo[offset];

        if (cmd == 0x00) {
            
            offset++;
        } else if (cmd == 0x40) {
            
            if (offset + 9 > fifo_size) break;
            uint32_t dl_addr = Read32(fifo, offset + 1);
            uint32_t dl_size = Read32(fifo, offset + 5);
            // NWII_DLDBG: are we expanding the same lists over and over?
            static const bool dldbg = NWII_ENV("NWII_DLDBG") != nullptr;
            if (dldbg) {
                static std::map<uint64_t, uint32_t> hits;
                static auto t0 = std::chrono::steady_clock::now();
                hits[((uint64_t)dl_addr << 32) | dl_size]++;
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    std::vector<std::pair<uint32_t, uint64_t>> top;
                    for (auto &kv : hits) top.push_back({kv.second, kv.first});
                    std::sort(top.rbegin(), top.rend());
                    printf("[DLDBG] distinct=%zu top:", hits.size());
                    for (size_t i = 0; i < top.size() && i < 5; i++)
                        printf(" %08X/%uB x%u", (uint32_t)(top[i].second >> 32),
                               (uint32_t)top[i].second, top[i].first);
                    printf("\n");
                    hits.clear();
                }
            }
            // A CALL_DL inside a display list is not executed (Dolphin
            // OnDisplayList: "recursive display list detected").
            if (depth == 0) {
                nwii::runtime::GX_IdxBeginDL(dl_addr, dl_size);
                if (ExpandDisplayList(dl_addr, dl_size, commands, depth) && note)
                    noted = commands.size();  // R22 R7: the replay noted its commands
                nwii::runtime::GX_IdxEndDL();
            }
            offset += 9;
        } else if (cmd == 0x78) {
            // Seam RMW marker (not a real GP opcode). Native GX handlers own
            // only some bits of GEN_MODE / CMODE0 / CP 0x30; merging at call
            // time raced this parser and used a base value up to a frame
            // stale, clobbering the other fields (texgen counts, dither bits,
            // texture matrix indices). The merge happens here instead, at the
            // marker's position in the stream, so the base is always current.
            if (offset + 11 > fifo_size) break;
            uint8_t kind = fifo[offset + 1];
            uint8_t reg  = fifo[offset + 2];
            uint32_t mask = Read32(fifo, offset + 3);
            uint32_t val  = Read32(fifo, offset + 7);
            GXCommand c;
            c.reg = reg;
            if (kind == 0) {
                uint32_t merged = (s_parse_bp[reg] & ~mask) | (val & mask);
                s_parse_bp[reg] = merged;
                s_bp_mask = 0xFFFFFF;  // a real BP write consumes the mask
                c.bp_raw = merged;
                ParseBP(reg, merged);
                c.type = GXCommandType::BPRegister;
                c.val = merged;
            } else {
                uint32_t merged = (s_ps.cp[reg] & ~mask) | (val & mask);
                s_ps.cp[reg] = merged;
                g_dlstate_dirty = true;
                ParseCP(reg, merged);
                c.type = GXCommandType::CPRegister;
                c.val = merged;
            }
            commands.push_back(std::move(c));
            offset += 11;
        } else if (cmd == 0x48) {
            
            offset++;
        } else if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) {
            if (depth > 0) g_prof_dl_loadindx++;
            if (offset + 5 > fifo_size) break;
            uint32_t val = Read32(fifo, offset + 1);
            int array = 12 + (cmd - 0x20) / 8;
            uint32_t index = val >> 16;
            uint32_t xf_addr = val & 0xFFF;
            int num = ((val >> 12) & 0xF) + 1;
            if (nwii::runtime::g_mmu) {
                GXCommand c;
                c.type = GXCommandType::XFRegister;
                c.reg = xf_addr;
                c.length = num - 1;
                // LOAD_INDX marker for the DL cache: array/num/index packed so
                // a cache hit can re-read live matrix data.
                c.val = 0x80000000u | ((uint32_t)(array - 12) << 24) |
                        ((uint32_t)num << 16) | (index & 0xFFFF);
                c.payload.resize(num);
                uint32_t base = s_ps.arrayBase[array] +
                                index * s_ps.arrayStride[array];
                const bool snap =
                    depth == 0 ? nwii::runtime::GX_IdxTakeMain(array - 12, base, num, c.payload.data())
                               : nwii::runtime::GX_IdxReadDL(array - 12, base, num, c.payload.data());
                if (!snap)
                    for (int i = 0; i < num; i++)
                        c.payload[i] = nwii::runtime::g_mmu->read_f32(base + i * 4);
                if (gx_trace()) {
                    static int ln = 0;
                    if (ln++ < 24) {
                        printf("[GXTRACE] LOAD_INDX arr=%d idx=%u xf=0x%03X n=%d base=0x%X stride=%u [",
                               array, index, xf_addr, num, base,
                               s_ps.arrayStride[array]);
                        for (int i = 0; i < num && i < 8; i++)
                            printf("%.3f ", c.payload[i]);
                        printf("]\n");
                    }
                }
                if (g_skindbg && xf_addr < 0x100 && (xf_addr % 4) == 0)
                    g_skin_loaded_rows |= (1ull << (xf_addr / 4));
                if (xf_addr < 256 || (xf_addr >= 0x400 && xf_addr < 0x460))
                    ParseApplyMatrixWrite(xf_addr, c.payload);
                commands.push_back(std::move(c));
            }
            offset += 5;
        } else if (cmd == 0x61) {
            
            if (offset + 5 > fifo_size) break;
            uint8_t reg = fifo[offset + 1];
            uint32_t val = Read24(fifo, offset + 2);

            GXCommand c;
            c.type = GXCommandType::BPRegister;
            c.reg = reg;
            c.bp_raw = val;
            c.val = ParseBPMasked(reg, val);
            commands.push_back(std::move(c));

            offset += 5;
        } else if (cmd == 0x08) {
            
            if (offset + 6 > fifo_size) break;
            uint8_t reg = fifo[offset + 1];
            uint32_t val = Read32(fifo, offset + 2);
            ParseCP(reg, val);

            GXCommand c;
            c.type = GXCommandType::CPRegister;
            c.reg = reg;
            c.val = val;
            commands.push_back(std::move(c));

            offset += 6;
        } else if (cmd == 0x10) {
            
            if (offset + 5 > fifo_size) break;
            uint16_t length = (fifo[offset + 1] << 8) | fifo[offset + 2];
            uint32_t total_size = 5 + ((length + 1) * 4);
            if (offset + total_size > fifo_size) break;

            GXCommand c;
            c.type = GXCommandType::XFRegister;
            c.length = length;
            c.reg = (fifo[offset + 3] << 8) | fifo[offset + 4];

            if (gx_trace() && c.reg >= 0x1020 && c.reg <= 0x1027) {
                static int xf_print_proj = 0;
                if (xf_print_proj++ < 5)
                    printf("[GXTRACE] XF proj load: len=%d reg=0x%04X\n", length, c.reg);
            }
            if (gx_trace() && c.reg < 0x100) {
                static int xf_print_mtx = 0;
                if (xf_print_mtx++ < 24)
                    printf("[GXTRACE] XF direct mtx: reg=0x%03X len=%d\n", c.reg, length + 1);
            }

            // NWII_XFDBG: is the game actually animating the colour-channel
            // material/ambient registers? (0x100a-0x1011). Runs in the parser
            // so it works headless, where the renderer never runs.
            static const bool xfdbg = NWII_ENV("NWII_XFDBG") != nullptr;
            if (xfdbg && c.reg >= 0x1009 && c.reg <= 0x1011) {
                static uint32_t last[9] = {0};
                static uint32_t changes[9] = {0};
                static uint64_t writes[9] = {0};
                static auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i <= length; i++) {
                    int r = c.reg + i - 0x1009;
                    if (r < 0 || r > 8) continue;
                    uint32_t v = Read32(fifo, offset + 5 + i * 4);
                    writes[r]++;
                    if (v != last[r]) { changes[r]++; last[r] = v; }
                }
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 1) {
                    t0 = now;
                    printf("[XFDBG] chan/mat regs (reg: writes/changes last):");
                    static const char* nm[9] = {"numchan","amb0","amb1","mat0","mat1",
                                                "cc0","cc1","ac0","ac1"};
                    for (int r = 0; r < 9; r++)
                        printf(" %s:%llu/%u=%08X", nm[r],
                               (unsigned long long)writes[r], changes[r], last[r]);
                    printf("\n");
                }
            }

            int num_floats = length + 1;
            c.payload.resize(num_floats);
            for (int i = 0; i < num_floats; ++i) {
                uint32_t val = Read32(fifo, offset + 5 + (i * 4));
                std::memcpy(&c.payload[i], &val, 4);
            }

            if (c.reg < 256 || (c.reg >= 0x400 && c.reg < 0x460))
                ParseApplyMatrixWrite(c.reg, c.payload);
            if (c.reg <= 0x1019 && c.reg + c.payload.size() > 0x1018)
                ParseXFMatrixIndex(c);

            commands.push_back(std::move(c));

            offset += total_size;
        } else if (cmd >= 0x80 && cmd <= 0xBF) {
            
            if (offset + 3 > fifo_size) break;

            uint16_t vtx_count = (fifo[offset + 1] << 8) | fifo[offset + 2];
            uint8_t vat_idx = cmd & 0x07;

            VATSlot& vat = s_ps.vat[vat_idx];

            // Vertex size is constant for a draw (every attribute is either
            // direct with a fixed byte width or a fixed 1/2-byte index), so
            // the payload length is known without touching the vertices.
            // Decode is deferred: frame-skip drops most draws, and decoding
            // only survivors is what keeps parse cost off the drain path.
            size_t vsize = VertexSize(vat);
            size_t need = 3 + (size_t)vtx_count * vsize;
            if (offset + need > fifo_size) break; // incomplete: wait for more
            if (depth == 0 && g_doomed) {  // r40d NWII_D_LIGHTPARSE: a dropped frame's draw
                ++s_light_skipped;
                offset += need;
                continue;
            }
            if (depth == 0 && MergeOn() && !commands.empty() && !PreXf()) {  // r40d NWII_D_MERGE
                GXCommand& p = commands.back();
                const uint8_t prim = cmd & 0xF8;
                const uint32_t unit = prim == 0x80 ? 4u : 3u;
                if (p.type == GXCommandType::DrawPrimitive && p.prim_type == prim && (prim == 0x80 || prim == 0x90) &&
                    p.raw && !p.raw->in_dl && p.vertices.empty() && !p.shared_vertices) {
                    DrawRaw& r = *p.raw;
                    if (r.count % unit == 0 && (uint32_t)r.count + vtx_count <= kMergeMaxVerts &&
                        !r.have_snap_mtx && r.snap_pos_bank.empty() && !r.decoded &&
                        std::memcmp(&r.vat, &vat, sizeof vat) == 0 && r.defPosMtxIdx == s_ps.defPosMtxIdx &&
                        std::memcmp(r.defTexMtxIdx, s_ps.defTexMtxIdx, 8) == 0 &&
                        std::memcmp(r.arrayBase, s_ps.arrayBase, sizeof r.arrayBase) == 0 &&
                        std::memcmp(r.arrayStride, s_ps.arrayStride, sizeof r.arrayStride) == 0 &&
                        r.bytes.size() == (size_t)r.count * vsize) {
                        if (FifoParser::MergeCheckOn()) {
                            if (r.merge_counts.empty()) r.merge_counts.push_back(r.count);
                            r.merge_counts.push_back(vtx_count);
                        }
                        r.bytes.insert(r.bytes.end(), fifo.begin() + offset + 3, fifo.begin() + offset + need);
                        r.count = (uint16_t)(r.count + vtx_count);
                        ++s_merged;
                        g_prof_draws++;
                        g_prof_draw_bytes += need;
                        offset += need;
                        continue;
                    }
                }
            }
            if (depth == 0 && EagerDecodeOn() && !PreXf() && AllDirect(vat)) {
                // r40d NWII_D_EAGERDECODE=1: a FIFO draw whose attributes are all direct
                // decodes from its own bytes and the parser state of this moment only
                // (no guest memory, no matrix snapshot: PreXf off), exactly what the
                // deferred DecodeDraw would read from its DrawRaw: decode it now from a
                // reused scratch DrawRaw instead of allocating one (and its byte copy)
                // per draw. A draw whose decode stops short keeps a DrawRaw of its own.
                static DrawRaw* const scratch = new DrawRaw;
                DrawRaw& r = *scratch;
                r.vat = vat;
                r.defPosMtxIdx = s_ps.defPosMtxIdx;
                std::memcpy(r.defTexMtxIdx, s_ps.defTexMtxIdx, 8);
                r.need_normal = false;
                r.have_snap_mtx = false;
                r.snap_pos_bank.clear();
                r.snap_nrm_bank.clear();
                std::memcpy(r.arrayBase, s_ps.arrayBase, sizeof(r.arrayBase));
                std::memcpy(r.arrayStride, s_ps.arrayStride, sizeof(r.arrayStride));
                r.count = vtx_count;
                r.bytes.assign(fifo.begin() + offset + 3, fifo.begin() + offset + need);
                r.in_dl = false;
                r.no_decode_cache = false;
                r.decoded.reset();
                r.ranges.clear();
                r.ranges_hash = 0;
                GXCommand c;
                c.type = GXCommandType::DrawPrimitive;
                c.prim_type = cmd & 0xF8;
                c.raw = HostPtr<DrawRaw>(HostPtr<DrawRaw>(), scratch);  // not owning
                FifoParser::DecodeDraw(c);
                if (c.raw) c.raw = MakeHost<DrawRaw>(r);  // stopped short: as the deferred decode would
                if (EagerCheckOn()) {
                    static uint64_t n = 0, bad = 0;
                    GXCommand t;
                    t.type = GXCommandType::DrawPrimitive;
                    t.prim_type = c.prim_type;
                    t.raw = MakeHost<DrawRaw>(r);
                    const bool fd = g_fastdec;
                    g_fastdec = false;
                    DecodeDrawImpl(t);
                    g_fastdec = fd;
                    ++n;
                    if (!VertsSame(c.vertices, t.vertices) || (bool)c.raw != (bool)t.raw) ++bad;
                    if (n % 200000 == 0 || (bad && bad <= 10 && !VertsSame(c.vertices, t.vertices)))
                        printf("[D40EAGER_CHECK] draws=%llu mismatches=%llu\n", (unsigned long long)n,
                               (unsigned long long)bad);
                }
                g_prof_draws++;
                g_prof_draw_bytes += need;
                ++s_eager_draws;
                commands.push_back(std::move(c));
                offset += need;
                continue;
            }

            GXCommand c;
            c.type = GXCommandType::DrawPrimitive;
            c.prim_type = cmd & 0xF8;
            c.raw = MakeHost<DrawRaw>();
            c.raw->in_dl = depth > 0;
            c.raw->vat = vat;
            c.raw->defPosMtxIdx = s_ps.defPosMtxIdx;
            std::memcpy(c.raw->defTexMtxIdx, s_ps.defTexMtxIdx, 8);
            // Snapshot this draw's position/normal matrix from the parse-time
            // shadow, unless the draw carries a per-vertex matrix index (then
            // each vertex may use a different matrix — fall back to live).
            if (!PreXf()) {
                // no snapshot: live matrices
            } else if (vat.posMatIdx) {
                // Per-vertex matrix index: freeze the entire bank, because the
                // vertices in this one draw will index different slots and the
                // renderer's live copy is whatever the frame ended up with.
                bool any = false;
                for (int k = 0; k < 256 && !any; ++k) any = g_parse_posmtx[k] != 0.0f;
                if (any) {
                    c.raw->snap_pos_bank.assign(g_parse_posmtx, g_parse_posmtx + 256);
                    c.raw->snap_nrm_bank.assign(g_parse_nrmmtx, g_parse_nrmmtx + 96);
                }
            } else {
                uint32_t pb = (uint32_t)s_ps.defPosMtxIdx * 4;
                uint32_t nb = 3u * (s_ps.defPosMtxIdx & 31);
                if (pb + 12 <= 256 && nb + 9 <= 96) {
                    const float* pm = &g_parse_posmtx[pb];
                    // Only trust the snapshot if the slot was actually written
                    // (a zero matrix = never loaded in the parse shadow) — else
                    // fall back to the renderer's live matrix.
                    bool nonzero = false;
                    for (int k = 0; k < 12 && !nonzero; ++k) nonzero = pm[k] != 0.0f;
                    if (nonzero) {
                        std::memcpy(c.raw->snap_pos_mtx, pm, 12 * sizeof(float));
                        std::memcpy(c.raw->snap_nrm_mtx, &g_parse_nrmmtx[nb], 9 * sizeof(float));
                        c.raw->have_snap_mtx = true;
                    }
                }
            }
            if (g_skindbg) {
                g_skin_draws_total++;
                if (vat.posMatIdx) g_skin_draws_pmidx++;
                if (s_ps.defPosMtxIdx != 0) g_skin_defnz++;
                if (c.raw->have_snap_mtx) g_skin_snapped++;
            }
            // Normals only matter when a colour channel is lit (XF 0x100e/0x100f
            // bit1); decoding them otherwise is pure cost.
            // R11: was read from g_state.xf (render-thread state); nothing
            // reads need_normal (DecodeDraw always keeps normals).
            c.raw->need_normal = false;
            std::memcpy(c.raw->arrayBase, s_ps.arrayBase, sizeof(c.raw->arrayBase));
            std::memcpy(c.raw->arrayStride, s_ps.arrayStride, sizeof(c.raw->arrayStride));
            c.raw->count = vtx_count;
            c.raw->bytes.assign(fifo.begin() + offset + 3, fifo.begin() + offset + need);
            static const bool vatdbg = NWII_ENV("NWII_VATDBG") != nullptr;
            if (vatdbg) {
                // Compact VAT signature: does the computed vsize land the next
                // byte on a plausible opcode? If not, vsize is wrong.
                uint8_t nb = (offset + need < fifo_size) ? fifo[offset + need] : 0;
                bool good = (nb==0||nb==0x08||nb==0x10||nb==0x61||nb==0x40||nb==0x48||
                             (nb>=0x20&&nb<=0x38)||(nb>=0x80&&nb<=0xBF));
                uint32_t sig = (vat.posMatIdx?1:0) | ((uint32_t)vat.posType<<1) |
                    ((uint32_t)vat.posElements<<4) | ((uint32_t)vat.nrmMask<<5) |
                    ((uint32_t)vat.nrmType<<7) | ((uint32_t)vat.nrmElements<<10) |
                    ((uint32_t)vat.clrMask[0]<<11) | ((uint32_t)vat.clrType[0]<<13) |
                    ((uint32_t)vat.texMask[0]<<16) | ((uint32_t)vat.texType[0]<<18);
                static std::map<uint32_t,std::pair<uint64_t,uint64_t>> sigs; // sig->(count,badnext)
                auto& e = sigs[sig]; e.first++;
                if(!good){ e.second++;
                    // scan forward for the next plausible opcode to learn the
                    // TRUE payload size, vs our computed 'need'.
                    size_t scan=offset+3;
                    while(scan<fifo_size && scan<offset+3+(size_t)vtx_count*64){
                        uint8_t b=fifo[scan];
                        if(b==0||b==0x08||b==0x10||b==0x61||b==0x40||b==0x48||
                           (b>=0x20&&b<=0x38)||(b>=0x80&&b<=0xBF)) break;
                        scan++;
                    }
                    size_t actual_payload=scan-(offset+3);
                    static int shown=0;
                    if(shown++<30 && vtx_count>0)
                        printf("[SZERR] vtx_count=%u our_vsize=%zu our_need=%zu actual_payload=%zu actual_per_vtx=%.2f nextbyte=0x%02X\n",
                            vtx_count, VertexSize(vat), need-3, actual_payload,
                            (double)actual_payload/vtx_count, scan<fifo_size?fifo[scan]:0);
                }
                static auto t0=std::chrono::steady_clock::now();
                auto now=std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now-t0).count()>=2){
                    t0=now;
                    printf("[VAT] sig(posMI,posT,posE,nrmM,nrmT,nrmE,clrM,clrT,texM,texT)=count/badnext vsize:\n");
                    for(auto&kv:sigs){uint32_t g=kv.first;
                        printf("  (%d,%d,%d,%d,%d,%d,%d,%d,%d,%d)=%llu/%llu sz=%zu\n",
                        g&1,(g>>1)&7,(g>>4)&1,(g>>5)&3,(g>>7)&7,(g>>10)&1,(g>>11)&3,(g>>13)&7,(g>>16)&3,(g>>18)&7,
                        (unsigned long long)kv.second.first,(unsigned long long)kv.second.second,VertexSize(vat));}
                    sigs.clear();
                }
            }
            g_prof_draws++;
            g_prof_draw_bytes += need;

            commands.push_back(std::move(c));
            offset += need;
        } else {
            g_prof_unknown++;
            // NWII_DESYNC: histogram the byte the parser can't recognise, plus
            // the 3 bytes before it (where the previous command over/under-ran).
            static const bool desync = NWII_ENV("NWII_DESYNC") != nullptr;
            if (desync) {
                static uint64_t hist[256] = {0};
                static auto t0 = std::chrono::steady_clock::now();
                hist[fifo[offset]]++;
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    printf("[DESYNC] top unknown opcodes:");
                    for (int pass = 0; pass < 6; pass++) {
                        int bi = 0; uint64_t bv = 0;
                        for (int i = 0; i < 256; i++) if (hist[i] > bv) { bv = hist[i]; bi = i; }
                        if (!bv) break;
                        printf(" 0x%02X:%llu", bi, (unsigned long long)bv);
                        hist[bi] = 0;
                    }
                    printf("\n");
                    for (int i = 0; i < 256; i++) hist[i] = 0;
                }
            }
            offset++;
        }
    }
}

} 
