// AX DSP microcode HLE.
//
// The game's AX library sends one command list per 3 ms audio frame through
// the DSP mailbox (0xBABE<size>, then the list address). Instead of running
// the DSP microcode we interpret that list on the CPU: walk the voice
// parameter blocks, decode PCM8/PCM16/DSP-ADPCM through an emulated DSP
// accelerator, resample, apply envelope and filters, mix into the main and aux
// buses, and write the 32 kHz stereo frame back to RAM, where AI DMA picks it
// up. Semantics follow Dolphin's AXWii HLE for the 0x4cc52064 microcode
// layout (the one NSMBW ships).
//
// R8 (threads): the list can run on its own host thread (ax_async.cpp), like
// the real DSP runs beside the CPU; the mixer works on host pointers and the
// per-sample volume loops are vectorized. Output is bit-exact with the r7b
// scalar mixer, kept as ax::ref (ax_hle_ref.cpp) for NWII_AX_VERIFY=1.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nwii::runtime::ax {

// Runs one command list of `size_words` 16-bit words at `addr`.
// Returns true if the list produced an output frame (OUTPUT command).
bool run_command_list(uint32_t addr, uint16_t size_words);
// The same in two steps: copy the list out of guest memory (on the CPU thread,
// at the mail write: the AX library starts on the next list in the buffer
// right after the mail), then run the copy (on the AX thread).
constexpr uint32_t kMaxListWords = 512;
uint32_t load_command_list(uint32_t addr, uint16_t size_words, uint16_t *out);
bool run_loaded_list(const uint16_t *list, uint32_t n);
// Words of the last list up to and including its END command.
uint32_t last_list_words_used();

// Mixer state that carries over between lists (buses, volume ramps,
// compressor), as raw bytes: compared against ref:: by NWII_AX_VERIFY.
void state_bytes(std::vector<uint8_t> &out);

// Debug hooks (nullptr in normal play). Every byte range of guest memory the
// mixer writes / reads is appended as (host pointer, length).
using RangeLog = std::vector<std::pair<uint8_t *, uint32_t>>;
extern RangeLog *g_write_log;
extern RangeLog *g_read_log;   // reads of inputs only (list, aux, samples, PBs)

namespace ref {
// The r7b scalar mixer. Reads guest memory through `g_overlay` and writes only
// to it (never to guest memory).
extern std::unordered_map<const uint8_t *, uint8_t> *g_overlay;
bool run_command_list(uint32_t addr, uint16_t size_words);
void state_bytes(std::vector<uint8_t> &out);
}  // namespace ref

// ---- ax_async.cpp: where the list runs ------------------------------------
// NWII_AX_THREAD=0 runs the list on the CPU thread at the mail write, as r7b.
// Default (2): the list runs on the AX thread and the DSP's "frame done" mail
// (DSP_SYNC) goes out when it is mixed. See ax_async.cpp for the rules.
bool async_enabled();
// Mode 2: DSP_SYNC waits for the mix (up to DSP_YIELD) instead of going out at 0.1 ms.
bool sync_slides();
// Starts processing (or, inline mode, processes) the list. Called on the CPU
// thread from the DSP mailbox write.
void submit(uint32_t addr, uint16_t size_words);
// r34s2: mixes the list right now on the calling thread whatever NWII_AX_THREAD says (the sound CPU:
// the mixer then reads the voice blocks exactly at the mail, never beside the SoundThread).
bool mix_inline(uint32_t addr, uint16_t size_words);
// True once the list submitted last has finished (always true inline).
bool done();
// Waits for the list submitted last; returns its run_command_list() result.
bool finish();
// Stats for the [FPS] line: lists/s, mixer us/list, CPU-thread wait us/s.
void stats_line(char *buf, int size);

}  // namespace nwii::runtime::ax
