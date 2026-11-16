#include "recompiler/recompiler.h"
#include "ppc/instruction.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <algorithm>

namespace nwii {
namespace recomp {

Recompiler::Recompiler(const analyzer::Analyzer &analyzer,
                       const SymbolTable *symbols,
                       const RecompilerConfig &config)
    : analyzer_(analyzer), symbols_(symbols), config_(config) {}

static const std::unordered_set<std::string> &hle_handlers() {
  // Native handlers that actually exist in nWiiRuntime. Binding is by symbol
  // name, so any game whose symbols the analyzer can name gets the whole seam
  // with no per-game configuration; [hle_hooks] in the TOML stays available for
  // overrides and for games where a symbol is missed.
  //
  // Deliberately conservative: only handlers that have been run and checked in
  // a real game are listed. Binding DVDReadAsyncPrio automatically loaded the
  // game's DLL as zeros and gave a black screen, so disc, audio, IOS and the
  // remaining VI entry points stay out until each is verified on its own. They
  // can still be forced per game through [hle_hooks].
  //
  // GXLoadTexMtxImm and GXCallDisplayList are deliberately absent: both are
  // implemented but broke rendering in Mario Party 7, so they stay opt-in via
  // the TOML until that is understood.
  //
  // The GX state setters are NOT seamed: the SDK keeps a shadow of GEN_MODE,
  // CMODE0, matrix index A etc. in __GXData and its dirty flush rewrites the
  // registers from that shadow, so a seam that writes the register but not
  // the shadow gets overwritten with stale values (NSMBW: LYT draws lost
  // their TEV stages and culling, fades drew opaque). The native SDK code is
  // exact; the FIFO parser sees every register it writes.
  static const std::unordered_set<std::string> s = {
      "OSAllocFromHeap",
      "OSCreateHeap",
      "OSFreeToHeap",
      "OSInitAlloc",
      "OSReport",
      "PADInit",
      "PADRead",
      "VIWaitForRetrace",
  };
  return s;
}

// Symbols out of the signature database can carry the library they came from,
// e.g. "GXSetCullMode\tgx.a GXGeometry.o" -- match on the bare name.
static std::string bare_symbol(const std::string &name) {
  size_t cut = name.find_first_of(" \t");
  if (cut == std::string::npos) {
    // By this point the name has usually been sanitised into an identifier,
    // so the separator that followed the function name is now "__".
    cut = name.find("__", 1);
  }
  std::string out = cut == std::string::npos ? name : name.substr(0, cut);
  // Symbols also arrive carrying their own address, e.g. GXSetZMode_800C0CC0.
  if (out.size() > 9) {
    const size_t us = out.size() - 9;
    if (out[us] == '_' &&
        out.find_first_not_of("0123456789ABCDEFabcdef", us + 1) ==
            std::string::npos)
      out.resize(us);
  }
  return out;
}

static bool is_hle_function(const std::string &name) {
  return hle_handlers().count(bare_symbol(name)) != 0;
}


std::vector<std::string> Recompiler::generate_cpp(uint32_t entry_point) {
  std::vector<std::string> generated_files;

  std::cout << "[Recompiler] Starting C++ generation. Total functions to emit: "
            << analyzer_.get_functions().size() << "\n";

  auto emit_headers = [](std::ostream &out) {
    out << "#include \"runtime/cpu_context.h\"\n";
    out << "#include \"runtime/config.h\"\n";
    out << "#include <stdint.h>\n";
    out << "#include <bit>\n";
    out << "#include <thread>\n";
    out << "#include <chrono>\n";

    out << "#include <cmath>\n";
    out << "#include <iostream>\n";
    out << "#include <cstdlib>\n\n";
    out << "using namespace nwii::runtime;\n\n";
  };

  if (config_.split_output) {
    
    std::string header_path = config_.output_dir + "/functions.h";
    std::ofstream hout(header_path);
    hout << "#pragma once\n";
    hout << "#include \"runtime/cpu_context.h\"\n\n";
    hout << "extern \"C\" {\n";
    hout << "void OSReport(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Open(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_OpenAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Close(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Read(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Ioctl(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_IoctlAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Ioctlv(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_IoctlvAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void iosAlloc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void iosFree(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIConfigure(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIConfigurePan(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void PADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VISetBlack(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIGetNextField(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIWaitForRetrace(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSDisableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSEnableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSRestoreInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetTime(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSTicksToMilliseconds(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInitFifoBase(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInitFifoPtrs(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXGetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCopyClear(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXBegin(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXEnd(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetVtxDesc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetVtxAttrFmt(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadPosMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXCallDisplayList(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadNrmMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadTexMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCurrentMtx(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Write(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Seek(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_CloseAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_ReadAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_WriteAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_SeekAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXAcquireVoice(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXFreeVoice(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceState(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceMix(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceAdpcm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceSrc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceOffsets(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDOpen(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVD_Callback(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDReadAsyncPrio(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDClose(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDGetDriveStatus(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDReadPrio(nwii::runtime::CPUContext& ctx);\n";
    hout << "void PADRead(nwii::runtime::CPUContext& ctx);\n";
    hout << "void WPADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void WPADRead(nwii::runtime::CPUContext& ctx);\n";
    hout << "void KPADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSInitAlloc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSCreateHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSAllocFromHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSFreeToHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetCurrentThread(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSleepThread(nwii::runtime::CPUContext& ctx);\n";
    // [hle_hooks] targets are runtime handlers the generated code calls by name.
    for (const auto &[addr, name] : config_.hle_hooks)
      hout << "void " << name << "(nwii::runtime::CPUContext& ctx);\n";
    hout << "}\n";

    std::set<std::string> emitted_names;
    std::vector<std::string> all_func_names;
    // The symbol name before any uniquifying suffix. The seam matches on this;
    // the wrapper itself must never carry it, or it would collide with the
    // native handler's own symbol at link time.
    std::vector<std::string> all_base_names;

    hout << "extern \"C\" {\n";
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name;
      if (symbols_ && symbols_->has_symbol(start_addr)) {
        func_name = symbols_->get_symbol(start_addr);
      } else {
        std::stringstream ss;
        ss << "func_" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(8) << start_addr;
        func_name = ss.str();
      }
      all_base_names.push_back(bare_symbol(func_name));
      if (emitted_names.count(func_name)) {
        std::stringstream ss;
        ss << func_name << "_" << std::hex << std::uppercase << start_addr;
        func_name = ss.str();
      }
      emitted_names.insert(func_name);
      all_func_names.push_back(func_name);

      hout << "void " << func_name << "(nwii::runtime::CPUContext& ctx);\n";
    }
    hout << "}\n";
    hout.close();

    int file_idx = 0;
    int func_idx = 0;
    int count_in_current_file = 0;
    std::ofstream out;

    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      if (count_in_current_file == 0) {
        if (out.is_open()) {
          out.close();
          std::cout << "[Recompiler] Generated " << generated_files.back()
                    << "\n";
        }
        std::stringstream ss;
        ss << "output_" << file_idx++ << ".cpp";
        std::string fname = ss.str();
        generated_files.push_back(fname);
        out.open(config_.output_dir + "/" + fname);
        emit_headers(out);
        out << "#include \"functions.h\"\n\n";
      }

      std::string func_name = all_func_names[func_idx];
      const std::string &base_name = all_base_names[func_idx++];
      if (!func.hle_hook_name.empty() || is_hle_function(base_name)) {
        std::string target =
            !func.hle_hook_name.empty() ? func.hle_hook_name : base_name;
        if (target != func_name) {
          out << "void " << func_name << "(nwii::runtime::CPUContext& ctx) {\n";
          out << "    " << target << "(ctx);\n";
          out << "    ctx.pc = ctx.lr;\n";
          out << "}\n\n";
        }
      } else {
        emit_function(out, func, func_name);
      }

      count_in_current_file += func.instructions.size();
      if (count_in_current_file >= config_.instructions_per_file) {
        count_in_current_file = 0;
      }
    }
    if (out.is_open()) {
      out.close();
      std::cout << "[Recompiler] Generated " << generated_files.back() << "\n";
    }

    std::string main_name = "main_output.cpp";
    generated_files.push_back(main_name);
    out.open(config_.output_dir + "/" + main_name);
    emit_headers(out);
    out << "#if defined(__GNUC__) || defined(__clang__)\n";
    out << "#pragma GCC optimize (\"O0\")\n";
    out << "#endif\n";
    out << "#if defined(__clang__)\n";
    out << "#pragma clang optimize off\n";
    out << "#endif\n\n";
    out << "#include \"functions.h\"\n\n";

    std::string entry_func;
    if (symbols_ && symbols_->has_symbol(entry_point)) {
      entry_func = symbols_->get_symbol(entry_point);
    } else {
      std::stringstream ss;
      ss << "func_" << std::hex << std::uppercase << std::setfill('0')
         << std::setw(8) << entry_point;
      entry_func = ss.str();
    }

    out << "#include <algorithm>\n";
    out << "// --- Function Bounds Table ---\n";
    out << "struct FuncBound { uint32_t start; uint32_t end; "
           "void(*func)(CPUContext&); };\n";
    out << "static const FuncBound g_func_bounds[] = {\n";

    func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      bool is_hle = !func.hle_hook_name.empty() ||
                    is_hle_function(all_base_names[func_idx]);
      ++func_idx;
      if (!is_hle) {
        out << "    {0x" << std::hex << std::uppercase << start_addr << ", 0x"
            << func.end_address << ", " << func_name << "},\n";
      }
    }
    out << "    {0, 0, nullptr}\n";
    out << "};\n\n";

    out << "// --- Entry Point Wrapper ---\n";
    out << "extern \"C\" void run_game(nwii::runtime::CPUContext& ctx) {\n";
    out << "    static uint32_t pc_history[10] = {0};\n";
    out << "    static int pc_history_idx = 0;\n";
    out << "    if (ctx.pc == 0) ctx.pc = 0x" << std::hex << std::uppercase
        << entry_point << std::dec << ";\n";
    out << "    while (ctx.is_running) {\n"
           "      pc_history[pc_history_idx] = ctx.pc;\n"
           "      pc_history_idx = (pc_history_idx + 1) % 10;\n"
           "      if (ctx.pc == 0) {\n"
           "          static bool warned_zero = false;\n"
           "          if (!warned_zero) {\n"
           "              warned_zero = true;\n"
           "              std::cout << \"[run_game] PC=0 (branch to NULL), last PCs:\";\n"
           "              for (int i = 0; i < 10; ++i)\n"
           "                  std::cout << \" 0x\" << std::hex << pc_history[(pc_history_idx + 9 - i) % 10];\n"
           "              std::cout << \" lr=0x\" << ctx.lr << std::dec << std::endl;\n"
           "          }\n"
           "          if ((ctx.msr & 0x8000) == 0) ctx.msr |= 0x8000; // Force "
           "EE=1 in idle\n"
           "          process_pending_callbacks(ctx);\n"
           "          if (ctx.pc == 0) {\n"
           "              "
           "std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
           "          }\n"
           "          continue;\n"
           "      }\n";
    out << "      if (_setjmp(ctx.exception_jmp_buf) == 0) {\n";
    out << "        process_pending_callbacks(ctx);\n";
    out << "        if (ctx.pc == 0xFFFFFFFC) {\n";
    out << "            if (!ctx.backup_stack.empty()) {\n";
    out << "                auto& bk = ctx.backup_stack.top();\n";
    out << "                ctx.gpr = bk.gpr; ctx.fpr = bk.fpr; ctx.ps1 = "
           "bk.ps1;\n";
    out << "                ctx.cr = bk.cr; ctx.lr = bk.lr; ctx.ctr = "
           "bk.ctr;\n";
    out << "                ctx.xer = bk.xer; ctx.pc = bk.pc;\n"
           "                ctx.srr0 = bk.srr0; ctx.srr1 = bk.srr1;\n"
           "                ctx.msr = bk.msr; ctx.fpscr = bk.fpscr;\n"
           "                ctx.gqr = bk.gqr; ctx.sprg = bk.sprg;\n";
    out << "                ctx.backup_stack.pop();\n";
    out << "            }\n";
    out << "            ctx.callback_depth--;\n";
    out << "            if (ctx.callback_depth <= 0) {\n";
    out << "                ctx.in_callback = false;\n";
    out << "                ctx.callback_depth = 0;\n";
    out << "            }\n";
    out << "            continue;\n";
    out << "        }\n";
    out << "        if ((ctx.pc & 0xF0000000) == 0xC0000000) ctx.pc = (ctx.pc "
           "& 0x0FFFFFFF) | 0x80000000;\n";
    out << "        else if ((ctx.pc & 0xF0000000) == 0xD0000000) ctx.pc = "
           "(ctx.pc & 0x0FFFFFFF) | 0x90000000;\n";
    out << "        else if (ctx.pc < 0x80000000) ctx.pc |= 0x80000000;\n";
    out << "        uint32_t target = ctx.pc;\n";
    out << "        switch (target) {\n";

    std::set<uint32_t> emitted_cases;

    func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      bool is_hle = !func.hle_hook_name.empty() ||
                    is_hle_function(all_base_names[func_idx]);
      ++func_idx;

      if (emitted_cases.insert(start_addr).second) {
        if (is_hle) {
          std::string hle_target =
              !func.hle_hook_name.empty() ? func.hle_hook_name : func_name;
          out << "            case 0x" << std::hex << std::uppercase
              << start_addr << std::dec << ": " << hle_target
              << "(ctx); ctx.pc = ctx.lr; break;\n";
        } else {
          out << "            case 0x" << std::hex << std::uppercase
              << start_addr << std::dec << ": " << func_name
              << "(ctx); break;\n";
        }
      }

      if (!is_hle) {
        for (const auto &inst : func.instructions) {
          ppc::Instruction ppc_inst(inst.opcode);
          if (ppc_inst.is_branch_link() || ppc_inst.opcode() == 17) {
            uint32_t ret_addr = inst.address + 4;
            if (emitted_cases.insert(ret_addr).second) {
              out << "            case 0x" << std::hex << std::uppercase
                  << ret_addr << std::dec << ": " << func_name
                  << "(ctx); break;\n";
            }
          }
          if (ppc_inst.opcode() == 16 || ppc_inst.opcode() == 18) {
            uint32_t target = ppc_inst.branch_target(inst.address);
            bool is_local =
                std::find_if(func.instructions.begin(), func.instructions.end(),
                             [target](const auto &i) {
                               return i.address == target;
                             }) != func.instructions.end();
            if (target <= inst.address && is_local) {
              if (emitted_cases.insert(inst.address).second) {
                out << "            case 0x" << std::hex << std::uppercase
                    << inst.address << std::dec << ": " << func_name
                    << "(ctx); break;\n";
              }
            }
          }
        }
        for (uint32_t jump_target : func.jump_table_targets) {
          if (emitted_cases.insert(jump_target).second) {
            out << "            case 0x" << std::hex << std::uppercase
                << jump_target << std::dec << ": " << func_name
                << "(ctx); break;\n";
          }
        }
      }
    }
    out << "            default: {\n";
    out << "                bool found = false;\n";
    // g_func_bounds is sorted by start: binary search instead of a linear
    // scan (39k entries; it ran for every mid-function resume address).
    out << "                const FuncBound *fb_end = g_func_bounds + "
           "(sizeof(g_func_bounds) / sizeof(g_func_bounds[0]) - 1);\n";
    out << "                const FuncBound *fb = std::upper_bound(g_func_bounds, "
           "fb_end, target, [](uint32_t t, const FuncBound &b) { return t < "
           "b.start; });\n";
    out << "                if (fb != g_func_bounds && target < (--fb)->end) {\n";
    out << "                    ctx.pc = target;\n";
    out << "                    fb->func(ctx);\n";
    out << "                    found = true;\n";
    out << "                }\n";
    out << "                if (!found) {\n";
    out << "                    // Not in any recompiled function: this is\n";
    out << "                    // runtime-generated code (copied to low mem,\n";
    out << "                    // trampolines). Interpret it from memory.\n";
    out << "                    nwii::runtime::interpret_step(ctx);\n";
    out << "                }\n";
    out << "            }\n";
    out << "        }\n";
    out << "      } else {\n";
    out << "          continue;\n";
    out << "      }\n";
    out << "    }\n";
    out << "    std::cout << \"[DEBUG] PC History before 0x0:\\n\";\n";
    out << "    for (int i=0; i<10; ++i) {\n";
    out << "        std::cout << \"  0x\" << std::hex << "
           "pc_history[(pc_history_idx + i) % 10] << \"\\n\";\n";
    out << "    }\n";
    out << "}\n";
    out.close();
    std::cout << "[Recompiler] Generated " << main_name << "\n";
  } else {
    
    std::string fname = "output.cpp";
    generated_files.push_back(fname);
    std::ofstream out(config_.output_dir + "/" + fname);
    emit_headers(out);

    std::set<std::string> emitted_names;
    std::vector<std::string> all_func_names;
    std::vector<std::string> all_base_names;

    out << "// --- Forward Declarations ---\n";
    out << "extern \"C\" {\n";
    out << "void OSReport(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Open(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_OpenAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Close(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Read(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Ioctl(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_IoctlAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Ioctlv(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_IoctlvAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void iosAlloc(nwii::runtime::CPUContext& ctx);\n";
    out << "void iosFree(nwii::runtime::CPUContext& ctx);\n";
    out << "void VIInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void PADInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void VISetBlack(nwii::runtime::CPUContext& ctx);\n";
    out << "void VIGetNextField(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSDisableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSEnableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSRestoreInterrupts(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetTime(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSTicksToMilliseconds(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSSetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSSetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXInitFifoBase(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXInitFifoPtrs(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXGetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCopyClear(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXBegin(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXEnd(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetVtxDesc(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetVtxAttrFmt(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXLoadPosMtxImm(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXCallDisplayList(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXLoadNrmMtxImm(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXLoadTexMtxImm(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCurrentMtx(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Write(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Seek(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_CloseAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_ReadAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_WriteAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_SeekAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXAcquireVoice(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXFreeVoice(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceState(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceMix(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceAdpcm(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceSrc(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceOffsets(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDOpen(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDReadAsyncPrio(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDClose(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDGetDriveStatus(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDReadPrio(nwii::runtime::CPUContext& ctx);\n";
    out << "void PADRead(nwii::runtime::CPUContext& ctx);\n";
    out << "void WPADInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void WPADRead(nwii::runtime::CPUContext& ctx);\n";
    out << "void KPADInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSInitAlloc(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSCreateHeap(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSAllocFromHeap(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSFreeToHeap(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetCurrentThread(CPUContext& ctx);\n";
    out << "void OSSleepThread(CPUContext& ctx);\n";
    for (const auto &[addr, name] : config_.hle_hooks)
      out << "void " << name << "(nwii::runtime::CPUContext& ctx);\n";
    out << "}\n";
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name;
      if (symbols_ && symbols_->has_symbol(start_addr)) {
        func_name = symbols_->get_symbol(start_addr);
      } else {
        std::stringstream ss;
        ss << "func_" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(8) << start_addr;
        func_name = ss.str();
      }
      all_base_names.push_back(bare_symbol(func_name));
      if (emitted_names.count(func_name)) {
        std::stringstream ss;
        ss << func_name << "_" << std::hex << std::uppercase << start_addr;
        func_name = ss.str();
      }
      emitted_names.insert(func_name);
      all_func_names.push_back(func_name);
      out << "void " << func_name << "(CPUContext& ctx);\n";
    }

    out << "\n// --- Function Bodies ---\n";
    int func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      const std::string &base_name = all_base_names[func_idx++];
      if (!func.hle_hook_name.empty() || is_hle_function(base_name)) {
        out << "// [HLE Hook] Skipping generation for " << func_name << "\n\n";
      } else {
        emit_function(out, func, func_name);
      }
    }

    std::string entry_func;
    if (symbols_ && symbols_->has_symbol(entry_point)) {
      entry_func = symbols_->get_symbol(entry_point);
    } else {
      std::stringstream ss;
      ss << "func_" << std::hex << std::uppercase << std::setfill('0')
         << std::setw(8) << entry_point;
      entry_func = ss.str();
    }

    out << "\n// --- Function Bounds Table ---\n";
    out << "struct FuncBound { uint32_t start; uint32_t end; "
           "void(*func)(CPUContext&); };\n";
    out << "static const FuncBound g_func_bounds[] = {\n";

    func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      bool is_hle = !func.hle_hook_name.empty() ||
                    is_hle_function(all_base_names[func_idx]);
      ++func_idx;
      if (!is_hle) {
        out << "    {0x" << std::hex << std::uppercase << start_addr << ", 0x"
            << func.end_address << ", " << func_name << "},\n";
      }
    }
    out << "    {0, 0, nullptr}\n";
    out << "};\n\n";

    out << "// --- Entry Point Wrapper ---\n";
    out << "extern \"C\" void run_game(nwii::runtime::CPUContext& ctx) {\n";
    out << "    static uint32_t pc_history[10] = {0};\n";
    out << "    static int pc_history_idx = 0;\n";
    out << "    if (ctx.pc == 0) ctx.pc = 0x" << std::hex << std::uppercase
        << entry_point << std::dec << ";\n";
    out << "    while (ctx.is_running) {\n"
           "      pc_history[pc_history_idx] = ctx.pc;\n"
           "      pc_history_idx = (pc_history_idx + 1) % 10;\n"
           "      if (ctx.pc == 0) {\n"
           "          static bool warned_zero = false;\n"
           "          if (!warned_zero) {\n"
           "              warned_zero = true;\n"
           "              std::cout << \"[run_game] PC=0 (branch to NULL), last PCs:\";\n"
           "              for (int i = 0; i < 10; ++i)\n"
           "                  std::cout << \" 0x\" << std::hex << pc_history[(pc_history_idx + 9 - i) % 10];\n"
           "              std::cout << \" lr=0x\" << ctx.lr << std::dec << std::endl;\n"
           "          }\n"
           "          if ((ctx.msr & 0x8000) == 0) ctx.msr |= 0x8000; // Force "
           "EE=1 in idle\n"
           "          process_pending_callbacks(ctx);\n"
           "          if (ctx.pc == 0) {\n"
           "              "
           "std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
           "          }\n"
           "          continue;\n"
           "      }\n";
    out << "      try {\n";
    out << "        process_pending_callbacks(ctx);\n";
    out << "        if (ctx.pc == 0xFFFFFFFC) {\n";
    out << "            if (!ctx.backup_stack.empty()) {\n";
    out << "                auto& bk = ctx.backup_stack.top();\n";
    out << "                ctx.gpr = bk.gpr; ctx.fpr = bk.fpr; ctx.ps1 = "
           "bk.ps1;\n";
    out << "                ctx.cr = bk.cr; ctx.lr = bk.lr; ctx.ctr = "
           "bk.ctr;\n";
    out << "                ctx.xer = bk.xer; ctx.pc = bk.pc;\n"
           "                ctx.srr0 = bk.srr0; ctx.srr1 = bk.srr1;\n"
           "                ctx.msr = bk.msr; ctx.fpscr = bk.fpscr;\n"
           "                ctx.gqr = bk.gqr; ctx.sprg = bk.sprg;\n";
    out << "                ctx.backup_stack.pop();\n";
    out << "            }\n";
    out << "            ctx.callback_depth--;\n";
    out << "            if (ctx.callback_depth <= 0) {\n";
    out << "                ctx.in_callback = false;\n";
    out << "                ctx.callback_depth = 0;\n";
    out << "            }\n";
    out << "            continue;\n";
    out << "        }\n";
    out << "        if ((ctx.pc & 0xF0000000) == 0xC0000000) ctx.pc = (ctx.pc "
           "& 0x0FFFFFFF) | 0x80000000;\n";
    out << "        else if ((ctx.pc & 0xF0000000) == 0xD0000000) ctx.pc = "
           "(ctx.pc & 0x0FFFFFFF) | 0x90000000;\n";
    out << "        else if (ctx.pc < 0x80000000) ctx.pc |= 0x80000000;\n";
    out << "        uint32_t target = ctx.pc;\n";
    out << "        switch (target) {\n";

    std::set<uint32_t> emitted_cases;

    func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      bool is_hle = !func.hle_hook_name.empty() ||
                    is_hle_function(all_base_names[func_idx]);
      ++func_idx;

      if (emitted_cases.insert(start_addr).second) {
        if (is_hle) {
          std::string hle_target =
              !func.hle_hook_name.empty() ? func.hle_hook_name : func_name;
          out << "            case 0x" << std::hex << std::uppercase
              << start_addr << std::dec << ": " << hle_target
              << "(ctx); ctx.pc = ctx.lr; break;\n";
        } else {
          out << "            case 0x" << std::hex << std::uppercase
              << start_addr << std::dec << ": " << func_name
              << "(ctx); break;\n";
        }
      }

      if (!is_hle) {
        for (const auto &inst : func.instructions) {
          ppc::Instruction ppc_inst(inst.opcode);
          if (ppc_inst.is_branch_link() || ppc_inst.opcode() == 17) {
            uint32_t ret_addr = inst.address + 4;
            if (emitted_cases.insert(ret_addr).second) {
              out << "            case 0x" << std::hex << std::uppercase
                  << ret_addr << std::dec << ": " << func_name
                  << "(ctx); break;\n";
            }
          }
          if (ppc_inst.opcode() == 16 || ppc_inst.opcode() == 18) {
            uint32_t target = ppc_inst.branch_target(inst.address);
            bool is_local =
                std::find_if(func.instructions.begin(), func.instructions.end(),
                             [target](const auto &i) {
                               return i.address == target;
                             }) != func.instructions.end();
            if (target <= inst.address && is_local) {
              if (emitted_cases.insert(inst.address).second) {
                out << "            case 0x" << std::hex << std::uppercase
                    << inst.address << std::dec << ": " << func_name
                    << "(ctx); break;\n";
              }
            }
          }
        }
        for (uint32_t jump_target : func.jump_table_targets) {
          if (emitted_cases.insert(jump_target).second) {
            out << "            case 0x" << std::hex << std::uppercase
                << jump_target << std::dec << ": " << func_name
                << "(ctx); break;\n";
          }
        }
      }
    }
    out << "            default: {\n";
    out << "                std::cerr << \"UNKNOWN DISPATCH TO 0x\" << "
           "std::hex << target << \"\\n\";\n";
    out << "                bool found = false;\n";
    out << "                for (const auto& fb : g_func_bounds) {\n";
    out << "                    if (fb.start == 0) break;\n";
    out << "                    if (target >= fb.start && target < fb.end) {\n";
    out << "                        std::cerr << \"  -> Fallback dispatch to "
           "function 0x\" << std::hex << fb.start << \"\\n\";\n";
    out << "                        ctx.pc = target;\n";
    out << "                        fb.func(ctx);\n";
    out << "                        found = true;\n";
    out << "                        break;\n";
    out << "                    }\n";
    out << "                }\n";
    out << "                if (!found) std::exit(1);\n";
    out << "            }\n";
    out << "        }\n";
    out << "    }\n";
    out << "    std::cout << \"[DEBUG] PC History before 0x0:\\n\";\n";
    out << "    for (int i=0; i<10; ++i) {\n";
    out << "        std::cout << \"  0x\" << std::hex << "
           "pc_history[(pc_history_idx + i) % 10] << \"\\n\";\n";
    out << "    }\n";
    out << "}\n";
  }

  return generated_files;
}

bool Recompiler::generate_cmake_project(uint32_t entry_point) {
  if (!std::filesystem::exists(config_.output_dir)) {
    std::filesystem::create_directories(config_.output_dir);
  }

  std::vector<std::string> generated_files = generate_cpp(entry_point);
  if (generated_files.empty()) {
    return false;
  }

  // Copy the runtime sources, and only the sources.
  //
  // A plain recursive copy takes the source tree's own `build` directory with
  // it — CMake caches, object files, fetched dependencies and their `.git`
  // packs. Those are read-only inside git object stores, so the copy dies on
  // the first pack file and the whole export fails. It also happens to be the
  // last step, so everything generated up to that point was silently thrown
  // away, including the SDK symbol report.
  std::string runtime_dest = config_.output_dir + "/nWiiRuntime";
  try {
    namespace fs = std::filesystem;
    const fs::path src = config_.runtime_source_dir;
    const auto opts = fs::copy_options::overwrite_existing;

    auto skip = [](const fs::path &rel) {
      for (const auto &part : rel)
      {
        const std::string s = part.string();
        if (s == "build" || s == ".git" || s == ".cache" || s == ".DS_Store")
          return true;
      }
      return false;
    };

    fs::create_directories(runtime_dest);
    for (auto it = fs::recursive_directory_iterator(src);
         it != fs::recursive_directory_iterator(); ++it)
    {
      const fs::path rel = fs::relative(it->path(), src);
      if (skip(rel))
      {
        if (it->is_directory()) it.disable_recursion_pending();
        continue;
      }
      const fs::path dest = fs::path(runtime_dest) / rel;
      if (it->is_directory())
        fs::create_directories(dest);
      else if (it->is_regular_file())
        fs::copy_file(it->path(), dest, opts);
    }
  } catch (const std::exception &e) {
    std::cerr << "Failed to copy runtime from " << config_.runtime_source_dir
              << ": " << e.what() << "\n";
    return false;
  }

  std::string cmake_path = config_.output_dir + "/CMakeLists.txt";
  std::ofstream out(cmake_path);
  if (!out.is_open())
    return false;

  out << "cmake_minimum_required(VERSION 3.20)\n";

  
  out << "project(" << config_.project_name << " LANGUAGES C CXX)\n\n";
  out << "set(CMAKE_POLICY_VERSION_MINIMUM 3.5)\n";
  out << "set(CMAKE_CXX_STANDARD 20)\n";
  out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";

  
  
  out << "include(FetchContent)\n";
  out << "FetchContent_Declare(\n";
  out << "    tomlplusplus\n";
  out << "    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git\n";
  out << "    GIT_TAG        v3.4.0\n";
  out << ")\n";
  out << "FetchContent_MakeAvailable(tomlplusplus)\n\n";
  out << "FetchContent_Declare(\n";
  out << "    SDL2\n";
  out << "    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git\n";
  out << "    GIT_TAG        release-2.28.5\n";
  out << ")\n";
  out << "set(SDL2_DISABLE_INSTALL ON CACHE BOOL \"\" FORCE)\n";
  out << "FetchContent_MakeAvailable(SDL2)\n\n";

  

  out << "if(APPLE)\n";
  out << "    set(_sdl_gl_src "
         "\"${sdl2_SOURCE_DIR}/src/video/cocoa/SDL_cocoaopengl.m\")\n";
  out << "    if(EXISTS \"${_sdl_gl_src}\")\n";
  out << "        file(READ \"${_sdl_gl_src}\" _sdl_gl_txt)\n";
  out << "        string(REPLACE \"attr[i++] = "
         "NSOpenGLPFAAllowOfflineRenderers;\"\n";
  out << "                       \"/* NWii: breaks pixel format on dual-GPU "
         "Macs */\"\n";
  out << "                       _sdl_gl_patched \"${_sdl_gl_txt}\")\n";
  out << "        if(NOT _sdl_gl_txt STREQUAL _sdl_gl_patched)\n";
  out << "            file(WRITE \"${_sdl_gl_src}\" \"${_sdl_gl_patched}\")\n";
  out << "        endif()\n";
  out << "    endif()\n";
  out << "endif()\n\n";

  out << "add_subdirectory(nWiiRuntime)\n\n";

  out << "add_executable(" << config_.project_name
      << " nWiiRuntime/src/core/main.cpp";
  for (const auto &file : generated_files) {
    out << " " << file;
  }
  out << ")\n";

  out << "target_include_directories(" << config_.project_name
      << " PRIVATE nWiiRuntime/src nWiiRuntime/include)\n";

  
  out << "target_link_libraries(" << config_.project_name
      << " PRIVATE nwiiruntime)\n";

  return true;
}

std::string Recompiler::generate_function_cpp(const analyzer::Function &func) {
  std::stringstream ss;

  std::string func_name;
  if (symbols_ && symbols_->has_symbol(func.start_address)) {
    func_name = symbols_->get_symbol(func.start_address);
  } else {
    std::stringstream ss_name;
    ss_name << "func_" << std::hex << std::uppercase << std::setfill('0')
            << std::setw(8) << func.start_address;
    func_name = ss_name.str();
  }

  emit_function(ss, func, func_name);
  return ss.str();
}

// The RVL SDK's SelectThread waits for a runnable thread with
//   lwz rX, RunQueueBits(r13); cmpwi rX, 0; beq -8
// i.e. a backward conditional branch over a 2-instruction loop that loads an
// r13-relative word and compares it with zero. Only an interrupt can end it.
static bool is_idle_spin(const analyzer::Function &func,
                         const analyzer::Instruction &branch, uint32_t target) {
  if (target != branch.address - 8)
    return false;
  const analyzer::Instruction *ld = nullptr, *cmp = nullptr;
  for (const auto &i : func.instructions) {
    if (i.address == target) ld = &i;
    if (i.address == target + 4) cmp = &i;
  }
  if (!ld || !cmp)
    return false;
  ppc::Instruction l(ld->opcode), c(cmp->opcode);
  return l.opcode() == 32 && l.ra() == 13 &&               // lwz rX, d(r13)
         c.opcode() == 11 && c.ra() == l.rd() && c.simm() == 0 &&  // cmpwi rX, 0
         ((c.rd() >> 2) == ((branch.opcode >> 18) & 7));  // same cr field
}

// RVL SDK OSSaveContext: "stmw r13,0x34(r3); mfspr r0,GQR1; ..."
static bool is_os_save_context(const analyzer::Function &func) {
  const auto &in = func.instructions;
  return in.size() >= 2 && in[0].address == func.start_address &&
         in[0].opcode == 0xBDA30034u && in[1].opcode == 0x7C11E2A6u;
}

// RVL SDK OSLoadContext: tests OS_CONTEXT_STATE_EXC with lhz r4,0x1A2(r3),
// restores r13-r31 with lmw r13,0x34(r3) (r5-r31 for an exception-saved
// context) and resumes the context with rfi.
static bool is_os_load_context(const analyzer::Function &func) {
  bool state = false, lmw = false, rfi = false;
  for (const auto &i : func.instructions) {
    state |= i.opcode == 0xA08301A2u;
    lmw |= i.opcode == 0xB9A30034u;
    rfi |= i.opcode == 0x4C000064u;
  }
  return state && lmw && rfi && func.instructions.size() < 64;
}

void Recompiler::emit_function(std::ostream &out,
                               const analyzer::Function &func,
                               const std::string &func_name) {
  out << "void " << func_name << "(CPUContext& ctx) {\n";

  
  
  auto hook = config_.hle_hooks.find(func.start_address);
  if (hook != config_.hle_hooks.end()) {
    out << "    " << hook->second << "(ctx);\n";
    out << "    ctx.pc = ctx.lr; return;\n";
    out << "}\n\n";
    return;
  }

  

  
  
  bool all_zero = !func.instructions.empty();
  for (const auto &inst : func.instructions) {
    if (inst.opcode != 0) {
      all_zero = false;
      break;
    }
  }
  bool os_low_mem = (func.start_address & 0x3FFFFFFF) < 0x3100;
  if (all_zero || os_low_mem) {
    out << "    nwii::runtime::interpret_step(ctx);\n";
    out << "}\n\n";
    return;
  }

  
  out << "    if (nwii::runtime::g_trace_calls && ctx.pc == 0x" << std::hex
      << std::uppercase << func.start_address << std::dec
      << ") nwii::runtime::trace_call(0x" << std::hex << std::uppercase
      << func.start_address << std::dec << ", ctx);\n";

  std::set<uint32_t> valid_addrs;
  for (const auto &inst : func.instructions) {
    valid_addrs.insert(inst.address);
  }

  if (valid_addrs.size() > 1) {
    out << "    if (ctx.pc != 0x" << std::hex << std::uppercase
        << func.start_address << std::dec << ") {\n";
    out << "        switch (ctx.pc) {\n";
    for (uint32_t addr : valid_addrs) {
      if (addr != func.start_address) {
        out << "            case 0x" << std::hex << std::uppercase << addr
            << std::dec << ": goto loc_" << std::hex << std::uppercase << addr
            << std::dec << ";\n";
      }
    }
    out << "            case 0x0: return; // idle: no runnable thread\n";

    
    
    out << "            default: nwii::runtime::interpret_step(ctx); return;\n";
    out << "        }\n";
    out << "    }\n";

  }
  // Per-thread FP registers: the eager stand-in for the SDK's lazy FPU
  // switch (MSR[FP] is not emulated), see fpu_on_save_context in
  // cpu_context.h.
  if (is_os_save_context(func))
    out << "    nwii::runtime::fpu_on_save_context(ctx);\n";
  else if (is_os_load_context(func))
    out << "    nwii::runtime::fpu_on_load_context(ctx);\n";
  // Retired instructions are counted per straight-line run, not per
  // instruction: a run starts at the entry, at every local branch target and
  // after any instruction that can leave the function (call, return, poll).
  // Every instruction keeps its label, so mid-function entry still works (a
  // run entered in the middle is just not counted).
  const auto &insts = func.instructions;
  std::vector<std::string> code(insts.size());
  for (size_t i = 0; i < insts.size(); ++i) {
    std::ostringstream s;
    emit_instruction(s, insts[i], func);
    code[i] = s.str();
  }
  std::unordered_set<uint32_t> run_start;
  if (!insts.empty())
    run_start.insert(insts[0].address);
  for (size_t i = 0; i < insts.size(); ++i) {
    const std::string &c = code[i];
    size_t body = c.find('\n');
    for (size_t p = c.find("goto loc_", body); p != std::string::npos;
         p = c.find("goto loc_", p + 9))
      run_start.insert((uint32_t)std::stoul(c.substr(p + 9, 8), nullptr, 16));
    if (i + 1 < insts.size() && (c.find("return", body) != std::string::npos ||
                                 c.find("(ctx)", body) != std::string::npos))
      run_start.insert(insts[i + 1].address);
  }
  for (size_t i = 0; i < insts.size(); ++i) {
    size_t eol = code[i].find('\n') + 1;
    out << code[i].substr(0, eol);
    if (run_start.count(insts[i].address)) {
      size_t n = 1;
      while (i + n < insts.size() && !run_start.count(insts[i + n].address))
        ++n;
      out << "    ctx.inst_count += " << n << ";\n";
    }
    out << code[i].substr(eol);
  }

  out << "}\n\n";
}

void Recompiler::emit_instruction(std::ostream &out,
                                  const analyzer::Instruction &inst,
                                  const analyzer::Function &func) {
  ppc::Instruction ppc_inst(inst.opcode);

  out << "loc_" << std::hex << std::uppercase << inst.address << std::dec
      << ": ;\n";
  out << "    // 0x" << std::hex << std::setfill('0') << std::setw(8)
      << inst.address << ": ";
  out << std::setfill('0') << std::setw(8) << inst.opcode << std::dec << "\n";

  if (ppc_inst.opcode() == 14) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();

    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = " << simm << "; // li r" << rD << ", "
          << simm << "\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rA << "] + " << simm
          << "; // addi r" << rD << ", r" << rA << ", " << simm << "\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 444) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();
    if (rS == rB) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "]; // mr r" << rA
          << ", r" << rS << "\n";
    } else {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] | ctx.gpr["
          << rB << "]; // or r" << rA << ", r" << rS << ", r" << rB << "\n";
    }

    
    if (ppc_inst.value() & 1) {
      out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
      out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
      out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 339) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
    uint32_t spr_top = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t spr = (spr_top << 5) | spr_bottom;
    if (spr == 8) {
      out << "    ctx.gpr[" << rD << "] = ctx.lr; // mflr r" << rD << "\n";
    } else if (spr == 9) {
      out << "    ctx.gpr[" << rD << "] = ctx.ctr; // mfctr r" << rD << "\n";
    } else if (spr == 26) {
      out << "    ctx.gpr[" << rD << "] = ctx.srr0; // mfsrr0 r" << rD << "\n";
    } else if (spr == 27) {
      out << "    ctx.gpr[" << rD << "] = ctx.srr1; // mfsrr1 r" << rD << "\n";
    } else if (spr == 22) {
      out << "    ctx.gpr[" << rD << "] = ctx.read_dec(); // mfdec r" << rD << "\n";
    } else if (spr >= 912 && spr <= 919) {
      out << "    ctx.gpr[" << rD << "] = ctx.gqr[" << (spr - 912)
          << "]; // mfgqr" << (spr - 912) << " r" << rD << "\n";
    } else if (spr == 1) {
      out << "    ctx.gpr[" << rD << "] = ctx.xer; // mfxer r" << rD << "\n";
    } else if (spr == 920) {
      out << "    ctx.gpr[" << rD << "] = ctx.hid2; // mfspr HID2\n";
    } else if (spr == 922) {
      out << "    ctx.gpr[" << rD << "] = ctx.dma_u; // mfspr DMA_U\n";
    } else if (spr == 923) {
      out << "    ctx.gpr[" << rD << "] = ctx.dma_l; // mfspr DMA_L\n";
    } else if (spr >= 920 && spr <= 924) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr HID " << spr << "\n";
    } else if (spr == 1008) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr HID0 (stub)\n";
    } else if (spr == 287) {
      out << "    ctx.gpr[" << rD
          << "] = (nwii::runtime::Config::get().platform == "
             "nwii::runtime::Platform::GameCube) ? 0x00083214 : 0x00087102; // "
             "mfspr PVR\n";
    } else if (spr == 1017) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr L2CR\n";
    } else {
      out << "    std::cerr << \"[WARN] mfspr r\" << " << rD
          << " << \" spr=\" << " << spr << " << \" (stub=0)\\n\";\n";
      out << "    ctx.gpr[" << rD << "] = 0; // unknown spr stub\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 467) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
    uint32_t spr_top = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t spr = (spr_top << 5) | spr_bottom;
    if (spr == 8) {
      out << "    ctx.lr = ctx.gpr[" << rS << "]; // mtlr r" << rS << "\n";
    } else if (spr == 9) {
      out << "    ctx.ctr = ctx.gpr[" << rS << "]; // mtctr r" << rS << "\n";
    } else if (spr == 26) {
      out << "    ctx.srr0 = ctx.gpr[" << rS << "]; // mtsrr0 r" << rS << "\n";
    } else if (spr == 27) {
      out << "    ctx.srr1 = ctx.gpr[" << rS << "]; // mtsrr1 r" << rS << "\n";
    } else if (spr == 22) {
      out << "    ctx.write_dec(ctx.gpr[" << rS << "]); // mtdec r" << rS << "\n";
    } else if (spr >= 912 && spr <= 919) {
      out << "    ctx.gqr[" << (spr - 912) << "] = ctx.gpr[" << rS
          << "]; // mtgqr" << (spr - 912) << " r" << rS << "\n";
    } else if (spr == 1) {
      out << "    ctx.xer = ctx.gpr[" << rS << "]; // mtxer r" << rS << "\n";
    } else if (spr == 920) {
      out << "    ctx.write_hid2(ctx.gpr[" << rS << "]); // mtspr HID2\n";
    } else if (spr == 922) {
      out << "    ctx.dma_u = ctx.gpr[" << rS << "]; // mtspr DMA_U\n";
    } else if (spr == 923) {
      // Locked-cache DMA (LCLoadBlocks/LCStoreBlocks): nw4r g3d builds the
      // model matrices in the locked cache and stores them to RAM this way.
      out << "    ctx.write_dmal(ctx.gpr[" << rS << "]); // mtspr DMA_L\n";
    } else {
      out << "    // unhandled mtspr " << spr
          << "\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 0) {
    
    uint32_t crD = ppc_inst.rd() >> 2; 
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();

    out << "    ctx.cr[" << crD << "].lt = ((int32_t)ctx.gpr[" << rA
        << "] < (int32_t)ctx.gpr[" << rB << "]);\n";
    out << "    ctx.cr[" << crD << "].gt = ((int32_t)ctx.gpr[" << rA
        << "] > (int32_t)ctx.gpr[" << rB << "]);\n";
    out << "    ctx.cr[" << crD << "].eq = ((int32_t)ctx.gpr[" << rA
        << "] == (int32_t)ctx.gpr[" << rB << "]);\n";
    out << "    ctx.cr[" << crD << "].so = (ctx.xer >> 31) & 1;\n";
  } else if (ppc_inst.opcode() == 32) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(" << simm
          << "); // lwz r" << rD << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
          << "] + " << simm << "); // lwz r" << rD << ", " << simm << "(r" << rA
          << ")\n";
    }
  } else if (ppc_inst.opcode() == 33) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
        << "]); // lwzu r" << rD << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 34) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(" << simm
          << "); // lbz r" << rD << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
          << "] + " << simm << "); // lbz r" << rD << ", " << simm << "(r" << rA
          << ")\n";
    }
  } else if (ppc_inst.opcode() == 35) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
        << "]); // lbzu r" << rD << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 36) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.mmu.write32(" << simm << ", ctx.gpr[" << rS
          << "]); // stw r" << rS << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.mmu.write32(ctx.gpr[" << rA << "] + " << simm
          << ", ctx.gpr[" << rS << "]); // stw r" << rS << ", " << simm << "(r"
          << rA << ")\n";
    }
  } else if (ppc_inst.opcode() == 37) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t ea = ctx.gpr[" << rA << "] + " << simm << ";\n";
    out << "        ctx.mmu.write32(ea, ctx.gpr[" << rS << "]);\n";
    out << "        ctx.gpr[" << rA << "] = ea;\n";
    out << "    } // stwu r" << rS << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 38) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.mmu.write8(" << simm << ", (uint8_t)ctx.gpr[" << rS
          << "]); // stb r" << rS << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + " << simm
          << ", (uint8_t)ctx.gpr[" << rS << "]); // stb r" << rS << ", " << simm
          << "(r" << rA << ")\n";
    }
  } else if (ppc_inst.opcode() == 39) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t ea = ctx.gpr[" << rA << "] + " << simm << ";\n";
    out << "        ctx.mmu.write8(ea, (uint8_t)ctx.gpr[" << rS << "]);\n";
    out << "        ctx.gpr[" << rA << "] = ea;\n";
    out << "    } // stbu r" << rS << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 16) {
    
    uint32_t bo = ppc_inst.bo();
    uint32_t bi = ppc_inst.bi();
    uint32_t cr_idx = bi / 4;
    uint32_t cr_bit = bi % 4;

    std::string cond_field;
    if (cr_bit == 0)
      cond_field = "lt";
    else if (cr_bit == 1)
      cond_field = "gt";
    else if (cr_bit == 2)
      cond_field = "eq";
    else
      cond_field = "so";

    int16_t bd = ppc_inst.simm() & 0xFFFC; 
    int32_t target = bd;                   
    if ((inst.opcode & 2) == 2) {          
      target = bd;                         
      out << "    // Absolute branches not fully supported in simple string "
             "emit\n";
    } else {
      target = inst.address + bd;
    }

    std::stringstream ss;
    ss << "loc_" << std::hex << std::uppercase << target;
    std::string target_lbl = ss.str();

    bool is_local =
        std::find_if(func.instructions.begin(), func.instructions.end(),
                     [target](const auto &i) { return i.address == target; }) !=
        func.instructions.end();

    if (target <= inst.address && is_local) {

      out << "    ctx.pc = 0x" << std::hex << std::uppercase << inst.address
          << std::dec << ";\n";
      out << "    if (process_pending_callbacks(ctx)) return;\n";
    }

    std::string ext_name;
    if (!is_local) {
      if (symbols_ && symbols_->has_symbol(target)) {
        ext_name = symbols_->get_symbol(target);
        if (ext_name.find("loc_") == 0)
          ext_name.replace(0, 4, "func_");
      } else {
        std::stringstream ss_ext;
        ss_ext << "func_" << std::hex << std::uppercase << std::setfill('0')
               << std::setw(8) << target;
        ext_name = ss_ext.str();
      }
    }

    bool bit0 = (bo & 0x10) != 0;
    bool bit1 = (bo & 0x08) != 0;
    bool bit2 = (bo & 0x04) != 0;
    bool bit3 = (bo & 0x02) != 0;

    bool dec_ctr = !bit2;
    bool test_ctr = !bit2;
    bool test_cr = !bit0;

    std::vector<std::string> conditions;
    if (test_ctr) {
      if (bit3)
        conditions.push_back("ctx.ctr == 0");
      else
        conditions.push_back("ctx.ctr != 0");
    }
    if (test_cr) {
      if (bit1)
        conditions.push_back("ctx.cr[" + std::to_string(cr_idx) + "]." +
                             cond_field);
      else
        conditions.push_back("!ctx.cr[" + std::to_string(cr_idx) + "]." +
                             cond_field);
    }

    std::string cond_expr;
    if (conditions.empty())
      cond_expr = "true";
    else if (conditions.size() == 1)
      cond_expr = conditions[0];
    else
      cond_expr = conditions[0] + " && " + conditions[1];

    if (dec_ctr)
      out << "    ctx.ctr--;\n";

    if (ppc_inst.lk())
      out << "    ctx.lr = 0x" << std::hex << std::uppercase
          << (inst.address + 4) << std::dec << "; // save return address\n";

    if (is_local && is_idle_spin(func, inst, target)) {
      out << "    if (" << cond_expr << ") { nwii::runtime::idle_wait(ctx); goto "
          << target_lbl << "; }\n";
    } else if (is_local) {
      if (cond_expr == "true")
        out << "    goto " << target_lbl << ";\n";
      else
        out << "    if (" << cond_expr << ") goto " << target_lbl << ";\n";
    } else {
      bool is_mapped = true;
      if (!symbols_ || !symbols_->has_symbol(target)) {
        if (analyzer_.get_functions().find(target) ==
            analyzer_.get_functions().end()) {
          is_mapped = false;
        }
      }
      if (is_mapped) {
        if (cond_expr == "true") {
          out << "    ctx.pc = 0x" << std::hex << std::uppercase << target
              << std::dec << "; if (nwii::runtime::native_stack_ok()) "
              << ext_name << "(ctx); return;\n";
        } else {
          out << "    if (" << cond_expr << ") { ctx.pc = 0x" << std::hex
              << std::uppercase << target << std::dec
              << "; if (nwii::runtime::native_stack_ok()) " << ext_name
              << "(ctx); return; }\n";
        }
      } else {
        if (cond_expr == "true") {
          out << "    ctx.pc = 0x" << std::hex << target << std::dec
              << "; return;\n";
        } else {
          out << "    if (" << cond_expr << ") { ctx.pc = 0x" << std::hex
              << target << std::dec << "; return; }\n";
        }
      }
    }
  } else if (ppc_inst.opcode() == 18) {
    
    uint32_t target = ppc_inst.branch_target(inst.address);
    std::stringstream ss;
    std::string target_name;

    bool is_local =
        std::find_if(func.instructions.begin(), func.instructions.end(),
                     [target](const auto &i) { return i.address == target; }) !=
        func.instructions.end();

    if (target <= inst.address && is_local) {

      out << "    ctx.pc = 0x" << std::hex << std::uppercase << inst.address
          << std::dec << ";\n";
      out << "    if (process_pending_callbacks(ctx)) return;\n";
    }

    if (symbols_ && symbols_->has_symbol(target)) {
      target_name = symbols_->get_symbol(target);
      if (target_name.find("loc_") == 0)
        target_name.replace(0, 4, "func_");
    } else {
      std::stringstream ss_name;
      ss_name << "func_" << std::hex << std::uppercase << std::setfill('0')
              << std::setw(8) << target;
      target_name = ss_name.str();
    }

    if (ppc_inst.is_branch_link()) {
      out << "    ctx.lr = 0x" << std::hex << std::uppercase
          << (inst.address + 4) << std::dec << "; // save return address\n";
      if (is_local) {
        out << "    goto loc_" << std::hex << std::uppercase
            << std::setfill('0') << std::setw(8) << target << ";\n";
      } else {
        bool is_mapped = true;
        if (!symbols_ || !symbols_->has_symbol(target)) {
          if (analyzer_.get_functions().find(target) ==
              analyzer_.get_functions().end()) {
            is_mapped = false;
          }
        }
        if (is_mapped) {
          bool target_is_hle = (symbols_ && symbols_->has_symbol(target) &&
                                is_hle_function(symbols_->get_symbol(target)));
          out << "    ctx.pc = 0x" << std::hex << std::uppercase << target
              << std::dec << "; ";
          out << target_name << "(ctx);\n";
          out << "    if (ctx.pc != 0x" << std::hex << std::uppercase
              << (inst.address + 4) << std::dec << ") return;\n";
        } else {
          out << "    ctx.pc = 0x" << std::hex << target << std::dec
              << "; return; // bl to unmapped target\n";
        }
      }
    } else {
      if (is_local) {
        out << "    goto loc_" << std::hex << std::uppercase
            << std::setfill('0') << std::setw(8) << target << ";\n";
      } else {
        bool is_mapped = true;
        if (!symbols_ || !symbols_->has_symbol(target)) {
          if (analyzer_.get_functions().find(target) ==
              analyzer_.get_functions().end()) {
            is_mapped = false;
          }
        }
        if (is_mapped) {
          // Tail call: run the target natively like a bc to another function
          // does, instead of unwinding to run_game and re-entering every
          // caller frame through the dispatcher.
          out << "    ctx.pc = 0x" << std::hex << std::uppercase << target
              << std::dec << "; if (nwii::runtime::native_stack_ok()) "
              << target_name << "(ctx); return;\n";
        } else {
          out << "    ctx.pc = 0x" << std::hex << target << std::dec
              << "; return;\n";
        }
      }
    }
  } else if (ppc_inst.opcode() == 19) {
    uint32_t xo = ppc_inst.extended_opcode();
    if (xo == 150) { 
      out << "    // isync\n";
    } else if (xo == 50) { 
        out << "    {\n";
        out << "        uint32_t ee_was = ctx.msr & 0x8000;\n";
        out << "        ctx.msr = ctx.srr1; // rfi: restore MSR from SRR1\n";
        out << "        ctx.pc = ctx.srr0;  // rfi: jump to SRR0\n";
        out << "        ctx.dispatch_saved_ctx = 0; // exit ISR\n";
        out << "        if (!ee_was && (ctx.msr & 0x8000)) {\n";
        out << "            if (process_pending_callbacks(ctx)) return;\n";
        out << "        }\n";
        out << "    }\n";
        out << "    return;\n";
    } else if (xo == 16 || xo == 528) { 
      uint32_t bo = ppc_inst.bo();
      uint32_t bi = ppc_inst.bi();
      uint32_t cr_idx = bi / 4;
      uint32_t cr_bit = bi % 4;

      std::string cond_field;
      if (cr_bit == 0)
        cond_field = "lt";
      else if (cr_bit == 1)
        cond_field = "gt";
      else if (cr_bit == 2)
        cond_field = "eq";
      else
        cond_field = "so";

      std::string target_reg = (xo == 16) ? "ctx.lr" : "ctx.ctr";

      bool bit0 = (bo & 0x10) != 0;
      bool bit1 = (bo & 0x08) != 0;
      bool bit2 = (bo & 0x04) != 0;
      bool bit3 = (bo & 0x02) != 0;

      bool dec_ctr = !bit2;
      bool test_ctr = !bit2;
      bool test_cr = !bit0;

      std::vector<std::string> conditions;
      if (test_ctr) {
        if (bit3)
          conditions.push_back("ctx.ctr == 0");
        else
          conditions.push_back("ctx.ctr != 0");
      }
      if (test_cr) {
        if (bit1)
          conditions.push_back("ctx.cr[" + std::to_string(cr_idx) + "]." +
                               cond_field);
        else
          conditions.push_back("!ctx.cr[" + std::to_string(cr_idx) + "]." +
                               cond_field);
      }

      std::string cond_expr;
      if (conditions.empty())
        cond_expr = "true";
      else if (conditions.size() == 1)
        cond_expr = conditions[0];
      else
        cond_expr = conditions[0] + " && " + conditions[1];

      if (dec_ctr)
        out << "    ctx.ctr--;\n";

      if (ppc_inst.lk() && target_reg == "ctx.lr") {
        out << "    {\n";
        out << "        uint32_t temp_lr = ctx.lr;\n";
        out << "        ctx.lr = 0x" << std::hex << std::uppercase
            << (inst.address + 4) << std::dec << "; // save return address\n";
        if (cond_expr == "true") {
          out << "        ctx.pc = temp_lr; return;\n";
        } else {
          out << "        if (" << cond_expr
              << ") { ctx.pc = temp_lr; return; }\n";
        }
        out << "    }\n";
      } else {
        if (ppc_inst.lk()) {
          out << "    ctx.lr = 0x" << std::hex << std::uppercase
              << (inst.address + 4) << std::dec << "; // save return address\n";
        }

        
        bool null_guard = ppc_inst.lk() && target_reg == "ctx.ctr";
        if (cond_expr == "true") {
          if (null_guard)
            out << "    if (" << target_reg << ") { ctx.pc = " << target_reg
                << "; return; } else nwii::runtime::note_null_call(0x"
                << std::hex << std::uppercase << inst.address << std::dec
                << ", ctx.lr);\n";
          else
            out << "    ctx.pc = " << target_reg << "; return;\n";
        } else {
          if (null_guard)
            out << "    if ((" << cond_expr << ") && " << target_reg
                << ") { ctx.pc = " << target_reg << "; return; }\n";
          else
            out << "    if (" << cond_expr << ") { ctx.pc = " << target_reg
                << "; return; }\n";
        }
      }
    } else if (xo == 257 || xo == 129 || xo == 289 || xo == 225 || xo == 33 ||
               xo == 449 || xo == 417 || xo == 193) {
      uint32_t crbD = ppc_inst.rd();
      uint32_t crbA = ppc_inst.ra();
      uint32_t crbB = ppc_inst.rb();

      auto get_cr_bit_str = [](uint32_t bit_idx) {
        std::string field = "ctx.cr[" + std::to_string(bit_idx / 4) + "]";
        uint32_t bit = bit_idx % 4;
        if (bit == 0)
          return field + ".lt";
        if (bit == 1)
          return field + ".gt";
        if (bit == 2)
          return field + ".eq";
        return field + ".so";
      };

      std::string op;
      if (xo == 257)
        op = get_cr_bit_str(crbA) + " & " + get_cr_bit_str(crbB); 
      else if (xo == 129)
        op = get_cr_bit_str(crbA) + " & !" + get_cr_bit_str(crbB); 
      else if (xo == 289)
        op = get_cr_bit_str(crbA) + " == " + get_cr_bit_str(crbB); 
      else if (xo == 225)
        op = "!(" + get_cr_bit_str(crbA) + " & " + get_cr_bit_str(crbB) +
             ")"; 
      else if (xo == 33)
        op = "!(" + get_cr_bit_str(crbA) + " | " + get_cr_bit_str(crbB) +
             ")"; 
      else if (xo == 449)
        op = get_cr_bit_str(crbA) + " | " + get_cr_bit_str(crbB); 
      else if (xo == 417)
        op = get_cr_bit_str(crbA) + " | !" + get_cr_bit_str(crbB); 
      else if (xo == 193)
        op = get_cr_bit_str(crbA) + " ^ " + get_cr_bit_str(crbB); 

      out << "    " << get_cr_bit_str(crbD) << " = " << op << ";\n";
    } else if (xo == 0) {
      uint32_t crfD = ppc_inst.rd() >> 2;
      uint32_t crfS = ppc_inst.ra() >> 2;
      out << "    ctx.cr[" << crfD << "] = ctx.cr[" << crfS << "]; // mcrf\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED OPCODE 19 XO \" << " << xo
          << " << \" at 0x" << std::hex << inst.address << std::dec
          << "\\n\"; std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 48 || ppc_inst.opcode() == 49) {
    // lfs / lfsu: the single is widened into BOTH paired-single halves.
    uint32_t fD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    bool update = ppc_inst.opcode() == 49;
    std::string ea = (rA == 0 && !update)
                         ? std::to_string(simm)
                         : "ctx.gpr[" + std::to_string(rA) + "] + " + std::to_string(simm);
    if (update) {
      out << "    { uint32_t ea = " << ea << "; ctx.lfs(" << fD
          << ", ea); ctx.gpr[" << rA << "] = ea; } // lfsu\n";
    } else {
      out << "    ctx.lfs(" << fD << ", " << ea << "); // lfs\n";
    }
  } else if (ppc_inst.opcode() == 52 || ppc_inst.opcode() == 53) {
    // stfs / stfsu: Gekko selects the single bits (no rounding).
    uint32_t fS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    bool update = ppc_inst.opcode() == 53;
    std::string ea = (rA == 0 && !update)
                         ? std::to_string(simm)
                         : "ctx.gpr[" + std::to_string(rA) + "] + " + std::to_string(simm);
    if (update) {
      out << "    { uint32_t ea = " << ea << "; ctx.stfs(" << fS
          << ", ea); ctx.gpr[" << rA << "] = ea; } // stfsu\n";
    } else {
      out << "    ctx.stfs(" << fS << ", " << ea << "); // stfs f" << fS << "\n";
    }
  } else if (ppc_inst.opcode() == 59) {
    // Single-precision arithmetic: Dolphin semantics (cpu_context.h). The
    // result fills ps0 and ps1; products round frC to 25 bits; fused
    // multiply-adds round once. No NSMBW instruction here has Rc=1.
    uint32_t xo = (ppc_inst.value() >> 1) & 0x1F;
    uint32_t fD = ppc_inst.rd();
    uint32_t fA = ppc_inst.ra();
    uint32_t fB = ppc_inst.rb();
    uint32_t fC = ppc_inst.rc();
    auto args3 = [&](uint32_t a, uint32_t b) {
      return std::to_string(fD) + ", " + std::to_string(a) + ", " + std::to_string(b);
    };
    auto args4 = [&]() {
      return std::to_string(fD) + ", " + std::to_string(fA) + ", " +
             std::to_string(fC) + ", " + std::to_string(fB);
    };

    if (xo == 21) {
      out << "    ctx.fadds(" << args3(fA, fB) << ");\n";
    } else if (xo == 20) {
      out << "    ctx.fsubs(" << args3(fA, fB) << ");\n";
    } else if (xo == 25) {
      out << "    ctx.fmuls(" << args3(fA, fC) << ");\n";
    } else if (xo == 18) {
      out << "    ctx.fdivs(" << args3(fA, fB) << ");\n";
    } else if (xo == 22) {
      out << "    ctx.fill(" << fD << ", nwii::runtime::ppc_single(std::sqrt(ctx.fpr["
          << fB << "]))); // fsqrts (not on Gekko)\n";
    } else if (xo == 24) {
      out << "    ctx.fres(" << fD << ", " << fB << ");\n";
    } else if (xo == 29) {
      out << "    ctx.fmadds(" << args4() << ");\n";
    } else if (xo == 28) {
      out << "    ctx.fmsubs(" << args4() << ");\n";
    } else if (xo == 31) {
      out << "    ctx.fnmadds(" << args4() << ");\n";
    } else if (xo == 30) {
      out << "    ctx.fnmsubs(" << args4() << ");\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 59 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 63) {
    uint32_t xo = (ppc_inst.value() >> 1) & 0x3FF;
    uint32_t fD = ppc_inst.rd();
    uint32_t fB = ppc_inst.rb();
    uint32_t fA = ppc_inst.ra();
    uint32_t fC = (ppc_inst.value() >> 6) & 0x1F;
    // Double-precision ops and moves write ps0 only (Dolphin SetPS0).
    if (xo == 72) {
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fB << "]; // fmr\n";
    } else if (xo == 40) { 
      out << "    ctx.fpr[" << fD << "] = -ctx.fpr[" << fB << "]; // fneg\n";
    } else if (xo == 264) { 
      out << "    ctx.fpr[" << fD << "] = std::fabs(ctx.fpr[" << fB
          << "]); // fabs\n";
    } else if (xo == 136) { 
      out << "    ctx.fpr[" << fD << "] = -std::fabs(ctx.fpr[" << fB
          << "]); // fnabs\n";
    } else if (xo == 12) { 
      out << "    ctx.frsp(" << fD << ", " << fB << ");\n";
    } else if (xo == 15) { 
      out << "    ctx.fctiwz(" << fD << ", " << fB << ");\n";
    } else if (xo == 14) { 
      out << "    ctx.fctiw(" << fD << ", " << fB << ");\n";
    } else if (xo == 583) { 
      out << "    ctx.mffs(" << fD << ");\n";
    } else if (xo == 711) { 
      out << "    ctx.mtfsf(" << ((ppc_inst.value() >> 17) & 0xFF) << ", " << fB
          << ");\n";
    } else if (xo == 134) {
      out << "    ctx.mtfsfi(" << (fD >> 2) << ", " << ((ppc_inst.value() >> 12) & 0xF)
          << ");\n";
    } else if (xo == 38 || xo == 70) {
      out << "    ctx.mtfsb(" << fD << ", " << (xo == 38 ? "true" : "false")
          << "); // mtfsb" << (xo == 38 ? 1 : 0) << "\n";
    } else if ((xo & 0x1F) == 26) { 
      out << "    ctx.frsqrte(" << fD << ", " << fB << ");\n";
    } else if ((xo & 0x1F) == 24) { 
      out << "    ctx.fpr[" << fD << "] = 1.0 / ctx.fpr[" << fB
          << "]; // fre\n";
    } else if ((xo & 0x1F) == 18) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] / ctx.fpr["
          << fB << "]; // fdiv\n";
    } else if ((xo & 0x1F) == 20) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] - ctx.fpr["
          << fB << "]; // fsub\n";
    } else if ((xo & 0x1F) == 21) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] + ctx.fpr["
          << fB << "]; // fadd\n";
    } else if ((xo & 0x1F) == 25) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] * ctx.fpr["
          << fC << "]; // fmul\n";
    } else if ((xo & 0x1F) == 29) { 
      out << "    ctx.fmadd(" << fD << ", " << fA << ", " << fC << ", " << fB << ");\n";
    } else if ((xo & 0x1F) == 28) { 
      out << "    ctx.fmsub(" << fD << ", " << fA << ", " << fC << ", " << fB << ");\n";
    } else if ((xo & 0x1F) == 31) { 
      out << "    ctx.fnmadd(" << fD << ", " << fA << ", " << fC << ", " << fB << ");\n";
    } else if ((xo & 0x1F) == 30) { 
      out << "    ctx.fnmsub(" << fD << ", " << fA << ", " << fC << ", " << fB << ");\n";
    } else if ((xo & 0x1F) == 23) { 
      out << "    ctx.fpr[" << fD << "] = (ctx.fpr[" << fA
          << "] >= 0.0) ? ctx.fpr[" << fC
          << "] : ctx.fpr[" << fB << "]; // fsel\n";
    } else if (xo == 64) { 
      out << "    // mcrfs stub\n";
    } else if (xo == 0 || xo == 32) { 
      out << "    ctx.fcmp(" << (fD >> 2) << ", ctx.fpr[" << fA << "], ctx.fpr["
          << fB << "]); // fcmp" << (xo == 0 ? "u" : "o") << "\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 63 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 20) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t mb = (ppc_inst.value() >> 6) & 0x1F;
    uint32_t me = (ppc_inst.value() >> 1) & 0x1F;

    uint32_t mask = 0;
    if (mb <= me) {
      for (int i = mb; i <= me; ++i)
        mask |= (1 << (31 - i));
    } else {
      for (int i = 0; i <= me; ++i)
        mask |= (1 << (31 - i));
      for (int i = mb; i <= 31; ++i)
        mask |= (1 << (31 - i));
    }

    out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rA << "] & ~0x"
        << std::hex << std::uppercase << mask << std::dec
        << ") | (std::rotl(ctx.gpr[" << rS << "], " << sh << ") & 0x"
        << std::hex << std::uppercase << mask << std::dec << "); // rlwimi r"
        << rA << ", r" << rS << ", " << sh << ", " << mb << ", " << me << "\n";
    if (ppc_inst.value() & 1) {
      out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
      out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
      out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
    }
  } else if (ppc_inst.opcode() == 21) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t mb = (ppc_inst.value() >> 6) & 0x1F;
    uint32_t me = (ppc_inst.value() >> 1) & 0x1F;

    uint32_t mask = 0;
    if (mb <= me) {
      for (int i = mb; i <= me; ++i)
        mask |= (1 << (31 - i));
    } else {
      for (int i = 0; i <= me; ++i)
        mask |= (1 << (31 - i));
      for (int i = mb; i <= 31; ++i)
        mask |= (1 << (31 - i));
    }

    out << "    ctx.gpr[" << rA << "] = std::rotl(ctx.gpr[" << rS << "], " << sh
        << ") & 0x" << std::hex << std::uppercase << mask << std::dec
        << "; // rlwinm r" << rA << ", r" << rS << ", " << sh << ", " << mb
        << ", " << me << "\n";
    if (ppc_inst.value() & 1) {
      out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
      out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
      out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
    }
  } else if (ppc_inst.opcode() == 31) {
    uint32_t xo = ppc_inst.extended_opcode();
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();
    uint32_t rS = ppc_inst.rs();
    // X-form effective address: (rA|0) + rB.
    auto ea_x = [](uint32_t a, uint32_t b) {
      return (a ? "ctx.gpr[" + std::to_string(a) + "] + " : std::string()) +
             "ctx.gpr[" + std::to_string(b) + "]";
    };

    if (xo == 598 || xo == 854 || xo == 982 || xo == 54 || xo == 86 ||
        xo == 278 || xo == 246 || xo == 470) {
      
      out << "    // sync/cache instruction\n";
    } else if (xo == 0) { 
      uint32_t crfD = rD >> 2;
      out << "    ctx.cr[" << crfD << "].lt = ((int32_t)ctx.gpr[" << rA
          << "] < (int32_t)ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].gt = ((int32_t)ctx.gpr[" << rA
          << "] > (int32_t)ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].eq = (ctx.gpr[" << rA
          << "] == ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].so = (ctx.xer >> 31) & 1;\n";
    } else if (xo == 32) { 
      uint32_t crfD = rD >> 2;
      out << "    ctx.cr[" << crfD << "].lt = (ctx.gpr[" << rA << "] < ctx.gpr["
          << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].gt = (ctx.gpr[" << rA << "] > ctx.gpr["
          << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].eq = (ctx.gpr[" << rA
          << "] == ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].so = (ctx.xer >> 31) & 1;\n";
    } else if (xo == 11) { 
      out << "    ctx.gpr[" << rD << "] = (uint32_t)(((uint64_t)ctx.gpr[" << rA
          << "] * (uint64_t)ctx.gpr[" << rB << "]) >> 32); // mulhwu\n";
      if (ppc_inst.value() & 1) { 
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD << "] < 0);\n";
        out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD << "] > 0);\n";
        out << "    ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 75) { 
      out << "    ctx.gpr[" << rD
          << "] = (uint32_t)(((int64_t)(int32_t)ctx.gpr[" << rA
          << "] * (int64_t)(int32_t)ctx.gpr[" << rB << "]) >> 32); // mulhw\n";
      if (ppc_inst.value() & 1) { 
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD << "] < 0);\n";
        out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD << "] > 0);\n";
        out << "    ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 87) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rB
            << "]); // lbzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lbzx\n";
    } else if (xo == 215) {
      if (rA == 0)
        out << "    ctx.mmu.write8(ctx.gpr[" << rB << "], (uint8_t)ctx.gpr["
            << rS << "]); // stbx\n";
      else
        out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], (uint8_t)ctx.gpr[" << rS << "]); // stbx\n";
    } else if (xo == 279) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read16(ctx.gpr[" << rB
            << "]); // lhzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read16(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lhzx\n";
    } else if (xo == 371) { 
      uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
      uint32_t spr_top    = (ppc_inst.value() >> 11) & 0x1F;
      uint32_t spr = (spr_top << 5) | spr_bottom;
      if (spr == 269) { 
        out << "    ctx.gpr[" << rD << "] = (uint32_t)(ctx.read_timebase() >> 32); // mftbu\n";
      } else { 
        out << "    ctx.gpr[" << rD << "] = (uint32_t)(ctx.read_timebase() & 0xFFFFFFFF); // mftb\n";
      }

      
      
    } else if (xo == 407) {
      if (rA == 0)
        out << "    ctx.mmu.write16(ctx.gpr[" << rB << "], (uint16_t)ctx.gpr["
            << rS << "]); // sthx\n";
      else
        out << "    ctx.mmu.write16(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], (uint16_t)ctx.gpr[" << rS << "]); // sthx\n";
    } else if (xo == 23) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rB
            << "]); // lwzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lwzx\n";
    } else if (xo == 151) {
      if (rA == 0)
        out << "    ctx.mmu.write32(ctx.gpr[" << rB << "], ctx.gpr[" << rS
            << "]); // stwx\n";
      else
        out << "    ctx.mmu.write32(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], ctx.gpr[" << rS << "]); // stwx\n";
    } else if (xo == 40) {
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rB << "] - ctx.gpr["
          << rA << "]; // subf\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 235) {
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rA << "] * ctx.gpr["
          << rB << "]; // mullw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 491) {
      out << "    ctx.gpr[" << rD << "] = nwii::runtime::ppc_divw(ctx.gpr[" << rA
          << "], ctx.gpr[" << rB << "]); // divw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 459) {
      out << "    ctx.gpr[" << rD << "] = (ctx.gpr[" << rB << "] != 0) ? ctx.gpr["
          << rA << "] / ctx.gpr[" << rB << "] : 0u; // divwu\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 954) {
      out << "    ctx.gpr[" << rA << "] = (uint32_t)(int32_t)(int8_t)ctx.gpr["
          << rS << "]; // extsb\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 922) {
      out << "    ctx.gpr[" << rA << "] = (uint32_t)(int32_t)(int16_t)ctx.gpr["
          << rS << "]; // extsh\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 28) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] & ctx.gpr["
          << rB << "]; // and\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 60) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] & ~ctx.gpr["
          << rB << "]; // andc\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 316) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] ^ ctx.gpr["
          << rB << "]; // xor\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 24) {
      out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rB
          << "] & 0x20) ? 0 : (ctx.gpr[" << rS << "] << (ctx.gpr[" << rB
          << "] & 0x1F)); // slw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 536) {
      out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rB
          << "] & 0x20) ? 0 : (ctx.gpr[" << rS << "] >> (ctx.gpr[" << rB
          << "] & 0x1F)); // srw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 792) {
      // sraw rA, rS, rB: shift amount is rB[26:31]; sets XER[CA] when a
      // negative value shifts out 1 bits (srawi/sraw + addze divisions).
      out << "    {\n";
      out << "        int32_t sv = (int32_t)ctx.gpr[" << rS << "];\n";
      out << "        uint32_t sh = ctx.gpr[" << rB << "] & 0x3F;\n";
      out << "        uint32_t ca;\n";
      out << "        if (sh >= 32) { ctx.gpr[" << rA
          << "] = (sv < 0) ? 0xFFFFFFFFu : 0u; ca = (sv < 0) ? 1u : 0u; }\n";
      out << "        else { ctx.gpr[" << rA << "] = (uint32_t)(sv >> sh);\n";
      out << "            ca = (sv < 0 && ((uint32_t)sv & ((1u << sh) - 1u)) != 0) ? 1u : 0u; }\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | (ca << 29);\n";
      if (ppc_inst.value() & 1)
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      out << "    } // sraw\n";
    } else if (xo == 26) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS
          << "] == 0 ? 32 : std::countl_zero(ctx.gpr[" << rS
          << "]); // cntlzw\n";
      if (ppc_inst.value() & 1) { 
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
        out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
        out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 10) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + (uint64_t)ctx.gpr[" << rB << "];\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 8) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + (uint64_t)ctx.gpr[" << rB << "] + 1;\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 104 || xo == 616) { 
      out << "    ctx.gpr[" << rD << "] = 0u - ctx.gpr[" << rA
          << "]; // neg\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 266) { 
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rA << "] + ctx.gpr["
          << rB << "];\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 138) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + (uint64_t)ctx.gpr[" << rB
          << "] + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 136) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + (uint64_t)ctx.gpr[" << rB
          << "] + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 202) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 200) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 234) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + 0xFFFFFFFFULL + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 232) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + 0xFFFFFFFFULL + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 83) { 
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.msr;\n";
    } else if (xo == 146) { 

      

      
      out << "    {\n";
      out << "        uint32_t ee_was = ctx.msr & 0x8000;\n";
      out << "        ctx.msr = ctx.gpr[" << ppc_inst.rs() << "];\n";
      out << "        if (!ee_was && (ctx.msr & 0x8000)) {\n";
      out << "            ctx.pc = 0x" << std::hex << std::uppercase
          << (inst.address + 4) << std::dec << ";\n";
      out << "            if (process_pending_callbacks(ctx)) return;\n";
      out << "        }\n";
      out << "    }\n";
    } else if (xo == 124) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] | ctx.gpr["
          << rB << "]); // nor\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 476) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] & ctx.gpr["
          << rB << "]); // nand\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 284) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] ^ ctx.gpr["
          << rB << "]); // eqv\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 412) { 
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] | ~ctx.gpr["
          << rB << "]; // orc\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 20) { 
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(" << ea_x(rA, rB)
          << "); // lwarx\n";
    } else if (xo == 150) { 
      out << "    ctx.mmu.write32(" << ea_x(rA, rB)
          << ", ctx.gpr[" << ppc_inst.rs() << "]); // stwcx\n";
      out << "    ctx.cr[0].eq = true; ctx.cr[0].lt = false; ctx.cr[0].gt = "
             "false; // always succeed\n";
    } else if (xo == 119) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read8(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lbzux\n";
    } else if (xo == 311) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read16(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lhzux\n";
    } else if (xo == 343) { 
      out << "    ctx.gpr[" << rD
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(" << ea_x(rA, rB)
          << "); // lhax\n";
    } else if (xo == 375) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lhaux\n";
    } else if (xo == 247) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write8(ea, (uint8_t)ctx.gpr[" << ppc_inst.rs()
          << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // stbux\n";
    } else if (xo == 439) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write16(ea, (uint16_t)ctx.gpr[" << ppc_inst.rs()
          << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // sthux\n";
    } else if (xo == 824) { 

      

      uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
      out << "    {\n";
      out << "        int32_t sv = (int32_t)ctx.gpr[" << rD << "];\n";
      out << "        ctx.gpr[" << rA << "] = (uint32_t)(sv >> " << sh << ");\n";
      out << "        uint32_t ca = (sv < 0 && (uint32_t)(sv & "
          << (sh == 0 ? 0u : ((1u << sh) - 1)) << "u) != 0) ? 1 : 0;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | (ca << 29);\n";
      if (ppc_inst.value() & 1)
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      out << "    } // srawi\n";
    } else if (xo == 1003) { // divwo (OE=1)
      out << "    ctx.gpr[" << rD << "] = nwii::runtime::ppc_divw(ctx.gpr[" << rA
          << "], ctx.gpr[" << rB << "]); // divwo\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 971) { // divwuo
      out << "    ctx.gpr[" << rD << "] = (ctx.gpr[" << rB
          << "] != 0) ? ctx.gpr[" << rA << "] / ctx.gpr[" << rB
          << "] : 0u; // divwuo\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 535) { 
      out << "    ctx.lfs(" << ppc_inst.rd() << ", " << ea_x(rA, rB) << "); // lfsx\n";
    } else if (xo == 567) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ctx.lfs(" << ppc_inst.rd() << ", ea); ctx.gpr[" << rA
          << "] = ea; } // lfsux\n";
    } else if (xo == 599) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    ctx.fpr[" << frD2 << "] = ctx.mmu.read_f64(" << ea_x(rA, rB)
          << "); // lfdx\n";
    } else if (xo == 631) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.fpr[" << frD2 << "] = ctx.mmu.read_f64(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lfdux\n";
    } else if (xo == 663) { 
      out << "    ctx.stfs(" << ppc_inst.rs() << ", " << ea_x(rA, rB) << "); // stfsx\n";
    } else if (xo == 695) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ctx.stfs(" << ppc_inst.rs() << ", ea); ctx.gpr[" << rA
          << "] = ea; } // stfsux\n";
    } else if (xo == 727) { 
      out << "    ctx.mmu.write_f64(" << ea_x(rA, rB) << ", ctx.fpr["
          << ppc_inst.rs() << "]); // stfdx\n";
    } else if (xo == 759) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ctx.mmu.write_f64(ea, ctx.fpr[" << ppc_inst.rs()
          << "]); ctx.gpr[" << rA << "] = ea; } // stfdux\n";
    } else if (xo == 983) { 
      // stfiwx stores the low word of the raw FPR image (an fctiw* result).
      out << "    ctx.stfiwx(" << ppc_inst.rs() << ", " << ea_x(rA, rB) << ");\n";
    } else if (xo == 790) { 
      out << "    { uint16_t v = ctx.mmu.read16(" << ea_x(rA, rB)
          << "); ctx.gpr[" << rD
          << "] = ((v&0xFF)<<8)|((v>>8)&0xFF); } // lhbrx\n";
    } else if (xo == 534) { 
      out << "    { uint32_t v = ctx.mmu.read32(" << ea_x(rA, rB)
          << "); ctx.gpr[" << rD
          << "] = __builtin_bswap32(v); } // lwbrx\n";
    } else if (xo == 662) { 
      out << "    ctx.mmu.write32(" << ea_x(rA, rB)
          << ", __builtin_bswap32(ctx.gpr[" << ppc_inst.rs()
          << "])); // stwbrx\n";
    } else if (xo == 1014) { 
      out << "    ctx.dcbz(" << ea_x(rA, rB) << "); // dcbz\n";
    } else if (xo == 55) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read32(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lwzux\n";
    } else if (xo == 183) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write32(ea, ctx.gpr[" << ppc_inst.rs() << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // stwux\n";
    } else if (xo == 918) { 
      out << "    ctx.mmu.write16(" << ea_x(rA, rB)
          << ", ((ctx.gpr[" << ppc_inst.rs() << "] & 0xFF) << 8) | ((ctx.gpr["
          << ppc_inst.rs() << "] >> 8) & 0xFF)); // sthbrx\n";
    } else if (xo == 595 || xo == 210) { 
      out << "    // mfsr/mtsr stub\n";
    } else if (xo == 19) { 
      out << "    {\n";
      out << "        uint32_t cr_val = 0;\n";
      out << "        for (int i = 0; i < 8; ++i) {\n";
      out << "            cr_val |= (ctx.cr[i].lt ? 8 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].gt ? 4 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].eq ? 2 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].so ? 1 : 0) << (28 - i * 4);\n";
      out << "        }\n";
      out << "        ctx.gpr[" << rD << "] = cr_val;\n";
      out << "    }\n";
    } else if (xo == 144) { 
      uint32_t crm = (ppc_inst.value() >> 12) & 0xFF;
      out << "    {\n";
      out << "        uint32_t cr_val = ctx.gpr[" << ppc_inst.rs() << "];\n";
      out << "        uint32_t mask = 0x" << std::hex << crm << std::dec
          << ";\n";
      out << "        for (int i = 0; i < 8; ++i) {\n";
      out << "            if (mask & (1 << (7 - i))) {\n";
      out << "                uint32_t field = (cr_val >> (28 - i * 4)) & "
             "0xF;\n";
      out << "                ctx.cr[i].lt = (field & 8) != 0;\n";
      out << "                ctx.cr[i].gt = (field & 4) != 0;\n";
      out << "                ctx.cr[i].eq = (field & 2) != 0;\n";
      out << "                ctx.cr[i].so = (field & 1) != 0;\n";
      out << "            }\n";
      out << "        }\n";
      out << "    }\n";
    } else if (xo == 566) { 
      out << "    // tlbsync (no-op)\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 31 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 24) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] | " << ppc_inst.uimm() << "; // ori\n";
  } else if (ppc_inst.opcode() == 25) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] | " << (ppc_inst.uimm() << 16) << "; // oris\n";
  } else if (ppc_inst.opcode() == 26) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] ^ " << ppc_inst.uimm() << "; // xori\n";
  } else if (ppc_inst.opcode() == 27) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] ^ " << (ppc_inst.uimm() << 16) << "; // xoris\n";
  } else if (ppc_inst.opcode() == 28) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << ppc_inst.rs() << "] & "
        << ppc_inst.uimm() << "; // andi.\n";
    out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
    out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
    out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
  } else if (ppc_inst.opcode() == 29) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << ppc_inst.rs()
        << "] & " << (ppc_inst.uimm() << 16) << "u; // andis.\n";
    out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
    out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
    out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
    out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
  } else if (ppc_inst.opcode() == 7) { 
    out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.gpr[" << ppc_inst.ra()
        << "] * (uint32_t)" << ppc_inst.simm() << "; // mulli\n";
  } else if (ppc_inst.opcode() == 8) { 
    // subfic: rD = ~rA + SIMM + 1, XER[CA] = carry out (subfic + subfze
    // negates 64-bit values in the OS/runtime).
    out << "    {\n";
    out << "        uint64_t res = (uint64_t)(uint32_t)~ctx.gpr[" << ppc_inst.ra()
        << "] + (uint64_t)(uint32_t)" << ppc_inst.simm() << " + 1;\n";
    out << "        ctx.gpr[" << ppc_inst.rd() << "] = (uint32_t)res;\n";
    out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | ((uint32_t)(res >> 32) & 1u) << 29;\n";
    out << "    } // subfic\n";
  } else if (ppc_inst.opcode() == 12) { 
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int32_t simm = ppc_inst.simm();
    out << "    {\n";
    // The immediate is sign-extended to 32 bits, then added as an unsigned
    // 32-bit value: `+ simm` with a negative int would sign-extend to 64
    // bits and invert the carry (oracle FINDINGS D1, OSUnlockMutex).
    out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
        << "] + (uint64_t)(uint32_t)" << simm << ";\n";
    out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
    out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
           "29); // CA\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 13) { 
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int32_t simm = ppc_inst.simm();
    out << "    {\n";
    // The immediate is sign-extended to 32 bits, then added as an unsigned
    // 32-bit value: `+ simm` with a negative int would sign-extend to 64
    // bits and invert the carry (oracle FINDINGS D1, OSUnlockMutex).
    out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
        << "] + (uint64_t)(uint32_t)" << simm << ";\n";
    out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
    out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
           "29); // CA\n";
    out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD << "] < 0);\n";
    out << "        ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD << "] > 0);\n";
    out << "        ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
    out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 15) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = " << (ppc_inst.simm() << 16) << "; // lis\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.gpr[" << rA << "] + "
          << (ppc_inst.simm() << 16) << "; // addis\n";
  } else if (ppc_inst.opcode() == 17) {
    // sc. The RVL/GC SDK only uses it in PPCSync, DCFlushRange and
    // DCStoreRange, and the OS system-call vector (__OSSystemCallVector)
    // just pulses HID0[ABE] and rfi's back (Dolphin runs that handler): no
    // IOS work, IOS goes over IPC. The old handle_syscall call had two bugs:
    // ctx.pc still held the last poll address -- the bdnz of the DC*Range
    // loop -- so an interrupt taken inside it resumed the finished loop with
    // CTR = 0, i.e. 2^32 iterations sweeping the whole address space (40-80 s
    // freezes in the title demos, r4 = 0xE lines, LR 0x8025A8C0); and r0
    // (numNodes in g3d's CalcWorld flush) was taken for an IOS call number.
    // Now: an interrupt check that resumes after the sc.
    out << "    // sc (system call: sync + interrupt check)\n";
    out << "    ctx.pc = 0x" << std::hex << std::uppercase << (inst.address + 4)
        << std::dec << ";\n";
    out << "    if (process_pending_callbacks(ctx)) return;\n";
  } else if (ppc_inst.opcode() == 4) { 
    uint32_t xo = (inst.opcode >> 1) & 0x1F;
    uint32_t xo_10 = (inst.opcode >> 1) & 0x3FF;
    uint32_t frD = ppc_inst.rd();
    uint32_t frA = ppc_inst.ra();
    uint32_t frB = ppc_inst.rb();
    uint32_t frC = ppc_inst.rc();

    if (xo == 21) {
      out << "    ctx.ps_add(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 20) {
      out << "    ctx.ps_sub(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 25) {
      out << "    ctx.ps_mul(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 29) {
      out << "    ctx.ps_madd(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 28) {
      out << "    ctx.ps_msub(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 31) {
      out << "    ctx.ps_nmadd(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 30) {
      out << "    ctx.ps_nmsub(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 18) {
      out << "    ctx.ps_div(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 10) {
      out << "    ctx.ps_sum0(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 11) {
      out << "    ctx.ps_sum1(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 12) {
      out << "    ctx.ps_muls0(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 13) {
      out << "    ctx.ps_muls1(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 14) {
      out << "    ctx.ps_madds0(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 15) {
      out << "    ctx.ps_madds1(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 23) {
      out << "    ctx.ps_sel(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo_10 == 72) {
      out << "    ctx.ps_mr(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 40) {
      out << "    ctx.ps_neg(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 264) {
      out << "    ctx.ps_abs(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 136) {
      out << "    ctx.ps_nabs(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 528) {
      out << "    ctx.ps_merge00(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 560) {
      out << "    ctx.ps_merge01(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 592) {
      out << "    ctx.ps_merge10(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 624) {
      out << "    ctx.ps_merge11(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 0) {
      out << "    ctx.ps_cmpu0(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 32) {
      out << "    ctx.ps_cmpo0(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 64) {
      out << "    ctx.ps_cmpu1(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 96) {
      out << "    ctx.ps_cmpo1(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo == 24) {
      out << "    ctx.ps_res(" << frD << ", " << frB << ");\n";
    } else if (xo == 26) {
      out << "    ctx.ps_rsqrte(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 1014) {
      // dcbz_l: zero one locked-cache line (LCEnable clears the LC this way).
      out << "    ctx.dcbz(" << (frA ? "ctx.gpr[" + std::to_string(frA) + "] + " : std::string())
          << "ctx.gpr[" << frB << "]); // dcbz_l\n";
    } else if (uint32_t xo6 = (inst.opcode >> 1) & 0x3F;
               xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) {
      // psq_lx / psq_stx / psq_lux / psq_stux: indexed quantized load/store,
      // W in bit 10 and GQR index in bits 7-9 (the d-form puts them higher).
      uint32_t rA = (inst.opcode >> 16) & 0x1F;
      uint32_t rB = (inst.opcode >> 11) & 0x1F;
      uint32_t W = (inst.opcode >> 10) & 0x1;
      uint32_t I = (inst.opcode >> 7) & 0x7;
      bool store = (xo6 & 1) != 0;
      bool update = xo6 >= 38;
      std::string ea = (rA == 0 && !update)
                           ? "ctx.gpr[" + std::to_string(rB) + "]"
                           : "ctx.gpr[" + std::to_string(rA) + "] + ctx.gpr[" +
                                 std::to_string(rB) + "]";
      out << "    {\n        uint32_t ea = " << ea << ";\n";
      out << "        ctx." << (store ? "psq_store(" : "psq_load(") << frD
          << ", ea, " << W << ", " << I << "); // psq_" << (store ? "st" : "l")
          << (update ? "ux" : "x") << "\n";
      if (update)
        out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    }\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 4 (ps_*) XO \" << " << xo
          << " << \" XO_10 \" << " << xo_10
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
      out << "    ctx.fpr[" << frD << "] = 0.0;\n";
      out << "    ctx.ps1[" << frD << "] = 0.0;\n";
    }
  } else if (ppc_inst.opcode() == 10) { 
    uint32_t crD = ppc_inst.rd() >> 2;
    out << "    ctx.cr[" << crD << "].lt = (ctx.gpr[" << ppc_inst.ra() << "] < "
        << ppc_inst.uimm() << ");\n";
    out << "    ctx.cr[" << crD << "].gt = (ctx.gpr[" << ppc_inst.ra() << "] > "
        << ppc_inst.uimm() << ");\n";
    out << "    ctx.cr[" << crD << "].eq = (ctx.gpr[" << ppc_inst.ra()
        << "] == " << ppc_inst.uimm() << ");\n";
    out << "    ctx.cr[" << crD << "].so = (ctx.xer >> 31) & 1;\n";
  } else if (ppc_inst.opcode() == 11) { 
    uint32_t crD = ppc_inst.rd() >> 2;
    out << "    ctx.cr[" << crD << "].lt = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] < " << ppc_inst.simm() << ");\n";
    out << "    ctx.cr[" << crD << "].gt = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] > " << ppc_inst.simm() << ");\n";
    out << "    ctx.cr[" << crD << "].eq = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] == " << ppc_inst.simm() << ");\n";
    out << "    ctx.cr[" << crD << "].so = (ctx.xer >> 31) & 1;\n";
  } else if (ppc_inst.opcode() == 34) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read8("
          << ppc_inst.simm() << "); // lbz\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read8(ctx.gpr["
          << rA << "] + " << ppc_inst.simm() << "); // lbz\n";
  } else if (ppc_inst.opcode() == 38) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write8(" << ppc_inst.simm() << ", (uint8_t)ctx.gpr["
          << ppc_inst.rs() << "]); // stb\n";
    else
      out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
          << ", (uint8_t)ctx.gpr[" << ppc_inst.rs() << "]); // stb\n";
  } else if (ppc_inst.opcode() == 40) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16("
          << ppc_inst.simm() << "); // lhz\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16(ctx.gpr["
          << rA << "] + " << ppc_inst.simm() << "); // lhz\n";
  } else if (ppc_inst.opcode() == 41) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16(ctx.gpr["
        << rA << "]); // lhzu\n";
  } else if (ppc_inst.opcode() == 42) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16("
          << ppc_inst.simm() << "); // lha\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ctx.gpr[" << rA
          << "] + " << ppc_inst.simm() << "); // lha\n";
  } else if (ppc_inst.opcode() == 43) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << ppc_inst.rd()
        << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ctx.gpr[" << rA
        << "]); // lhau\n";
  } else if (ppc_inst.opcode() == 44) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write16(" << ppc_inst.simm() << ", (uint16_t)ctx.gpr["
          << ppc_inst.rs() << "]); // sth\n";
    else
      out << "    ctx.mmu.write16(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
          << ", (uint16_t)ctx.gpr[" << ppc_inst.rs() << "]); // sth\n";
  } else if (ppc_inst.opcode() == 45) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.mmu.write16(ctx.gpr[" << rA << "], (uint16_t)ctx.gpr["
        << ppc_inst.rs() << "]); // sthu\n";
  } else if (ppc_inst.opcode() == 46) { 
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t addr = "
        << (rA == 0 ? std::string("0") : "ctx.gpr[" + std::to_string(rA) + "]")
        << " + " << simm << ";\n";
    out << "        for (int i = " << rD << "; i <= 31; ++i) {\n";
    out << "            ctx.gpr[i] = ctx.mmu.read32(addr);\n";
    out << "            addr += 4;\n";
    out << "        }\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 47) { 
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t addr = "
        << (rA == 0 ? std::string("0") : "ctx.gpr[" + std::to_string(rA) + "]")
        << " + " << simm << ";\n";
    out << "        for (int i = " << rS << "; i <= 31; ++i) {\n";
    out << "            ctx.mmu.write32(addr, ctx.gpr[i]);\n";
    out << "            addr += 4;\n";
    out << "        }\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 50) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64("
          << ppc_inst.simm() << "); // lfd\n";
    else
      out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64(ctx.gpr["
          << rA << "] + " << ppc_inst.simm() << "); // lfd\n";
  } else if (ppc_inst.opcode() == 51) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64(ctx.gpr["
        << rA << "] + " << ppc_inst.simm() << "); // lfdu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + "
        << ppc_inst.simm() << ";\n";
  } else if (ppc_inst.opcode() == 54) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write_f64(" << ppc_inst.simm() << ", ctx.fpr["
          << ppc_inst.rs() << "]); // stfd\n";
    else
      out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
          << ", ctx.fpr[" << ppc_inst.rs() << "]); // stfd\n";
  } else if (ppc_inst.opcode() == 55) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
        << ", ctx.fpr[" << ppc_inst.rs() << "]); // stfdu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + "
        << ppc_inst.simm() << ";\n";
  } else if (ppc_inst.opcode() == 56) { 
    uint32_t frD = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    if (rA == 0)
      out << "    ctx.psq_load(" << frD << ", " << d << ", " << W << ", " << I
          << "); // psq_l\n";
    else
      out << "    ctx.psq_load(" << frD << ", ctx.gpr[" << rA << "] + " << d
          << ", " << W << ", " << I << "); // psq_l\n";
  } else if (ppc_inst.opcode() == 57) { 
    uint32_t frD = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    out << "    ctx.psq_load(" << frD << ", ctx.gpr[" << rA << "] + " << d
        << ", " << W << ", " << I << "); // psq_lu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << d << ";\n";
  } else if (ppc_inst.opcode() == 60) { 
    uint32_t frS = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    if (rA == 0)
      out << "    ctx.psq_store(" << frS << ", " << d << ", " << W << ", " << I
          << "); // psq_st\n";
    else
      out << "    ctx.psq_store(" << frS << ", ctx.gpr[" << rA << "] + " << d
          << ", " << W << ", " << I << "); // psq_st\n";
  } else if (ppc_inst.opcode() == 61) { 
    uint32_t frS = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    out << "    ctx.psq_store(" << frS << ", ctx.gpr[" << rA << "] + " << d
        << ", " << W << ", " << I << "); // psq_stu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << d << ";\n";
  } else {
    out << "    nwii::runtime::micro_interpret(ctx, 0x" << std::hex
        << inst.opcode << ", 0x" << inst.address << std::dec << ");\n";
    out << "    if (ctx.pc != 0x" << std::hex << (inst.address + 4) << std::dec
        << ") return;\n";
  }
}

} 
} 
