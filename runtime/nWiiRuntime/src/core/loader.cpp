#include "loader/loader.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include "runtime/config.h"

namespace nwii {
namespace loader {

bool Executable::load_dol(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << path << std::endl;
        return false;
    }

    DolHeader header;
    if (!file.read(reinterpret_cast<char*>(&header), sizeof(DolHeader))) {
        std::cerr << "Failed to read DOL header" << std::endl;
        return false;
    }

    entry_point = header.entry_point;

    for (int i = 0; i < 7; ++i) {
        if (header.text_offsets[i] == 0 || header.text_sizes[i] == 0) continue;

        Section sec;
        sec.address = header.text_addresses[i];
        sec.size = header.text_sizes[i];
        sec.is_text = true;
        sec.is_bss = false;
        sec.data.resize(sec.size);

        file.seekg(header.text_offsets[i], std::ios::beg);
        file.read(reinterpret_cast<char*>(sec.data.data()), sec.size);

        sections.push_back(std::move(sec));
    }

    for (int i = 0; i < 11; ++i) {
        if (header.data_offsets[i] == 0 || header.data_sizes[i] == 0) continue;

        Section sec;
        sec.address = header.data_addresses[i];
        sec.size = header.data_sizes[i];
        sec.is_text = false;
        sec.is_bss = false;
        sec.data.resize(sec.size);

        file.seekg(header.data_offsets[i], std::ios::beg);
        file.read(reinterpret_cast<char*>(sec.data.data()), sec.size);

        sections.push_back(std::move(sec));
    }

    if (header.bss_size > 0) {
        Section bss;
        bss.address = header.bss_address;
        bss.size = header.bss_size;
        bss.is_text = false;
        bss.is_bss = true;
        
        bss.data.clear(); 
        
        sections.push_back(std::move(bss));
    }

    return true;
}

bool Executable::load_rel(const std::string& path, uint32_t module_base,
                          uint32_t bss_base) {
    return load_rels({{path, module_base, bss_base}}) == 1;
}

int Executable::load_rels(const std::vector<RelSpec>& rels) {
    struct Module {
        const RelSpec* spec;
        std::vector<uint8_t> img;
        uint32_t id = 0;
        std::vector<uint32_t> sec_fileoff, sec_size, sec_addr;
        std::vector<bool> sec_exec;
    };
    auto be32 = [](const std::vector<uint8_t>& img, size_t o) -> uint32_t {
        return ((uint32_t)img[o] << 24) | ((uint32_t)img[o + 1] << 16) |
               ((uint32_t)img[o + 2] << 8) | img[o + 3];
    };
    auto be16 = [](const std::vector<uint8_t>& img, size_t o) -> uint16_t {
        return (uint16_t)(((uint32_t)img[o] << 8) | img[o + 1]);
    };
    auto wr32 = [](std::vector<uint8_t>& img, size_t o, uint32_t v) {
        img[o] = v >> 24; img[o + 1] = v >> 16; img[o + 2] = v >> 8; img[o + 3] = v;
    };
    auto wr16 = [](std::vector<uint8_t>& img, size_t o, uint16_t v) {
        img[o] = v >> 8; img[o + 1] = v & 0xFF;
    };

    // Pass 1: read every module and place its sections, so imports between
    // the modules can resolve whichever order they appear in.
    std::vector<Module> mods;
    for (const auto& spec : rels) {
        std::ifstream file(spec.path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "[REL] cannot open " << spec.path << std::endl;
            continue;
        }
        Module m;
        m.spec = &spec;
        m.img.resize((size_t)file.tellg());
        file.seekg(0);
        file.read(reinterpret_cast<char*>(m.img.data()), m.img.size());
        m.id = be32(m.img, 0x00);
        uint32_t num_sec = be32(m.img, 0x0C), sec_off = be32(m.img, 0x10);
        m.sec_fileoff.resize(num_sec);
        m.sec_size.resize(num_sec);
        m.sec_addr.resize(num_sec);
        m.sec_exec.resize(num_sec);
        for (uint32_t i = 0; i < num_sec; ++i) {
            uint32_t raw = be32(m.img, sec_off + i * 8);
            m.sec_fileoff[i] = raw & ~3u;
            m.sec_exec[i] = (raw & 1) != 0;
            m.sec_size[i] = be32(m.img, sec_off + i * 8 + 4);
            m.sec_addr[i] = m.sec_fileoff[i] ? (spec.module_base + m.sec_fileoff[i]) : 0;
            if (m.sec_fileoff[i] == 0 && m.sec_size[i] > 0)
                m.sec_addr[i] = spec.bss_base;
        }
        mods.push_back(std::move(m));
    }
    auto find_module = [&](uint32_t id) -> const Module* {
        for (const auto& m : mods)
            if (m.id == id) return &m;
        return nullptr;
    };
    auto in_text = [&](uint32_t addr) {
        for (const auto& m : mods)
            for (size_t i = 0; i < m.sec_addr.size(); ++i)
                if (m.sec_exec[i] && addr >= m.sec_addr[i] &&
                    addr < m.sec_addr[i] + m.sec_size[i])
                    return true;
        for (const auto& sec : sections)
            if (sec.is_text && addr >= sec.address && addr < sec.address + sec.size)
                return true;
        return false;
    };

    // Pass 2: apply relocations, collect code roots, publish text sections.
    enum { R_ADDR32 = 1, R_ADDR24 = 2, R_ADDR16 = 4, R_ADDR16_LO = 5,
           R_ADDR16_HI = 6, R_ADDR16_HA = 7, R_REL24 = 10, R_REL14 = 11,
           R_NOP = 201, R_SECTION = 202, R_END = 203, R_MRKREF = 204 };
    std::vector<uint32_t> pointer_roots;
    int loaded = 0;
    for (auto& m : mods) {
        auto& img = m.img;
        const std::string& path = m.spec->path;
        uint32_t imp_off = be32(img, 0x28), imp_size = be32(img, 0x2C);
        for (uint32_t e = 0; e < imp_size / 8; ++e) {
            uint32_t mod_id = be32(img, imp_off + e * 8);
            uint32_t rel = be32(img, imp_off + e * 8 + 4);
            const Module* target = mod_id ? find_module(mod_id) : nullptr;
            if (mod_id && !target) {
                std::cerr << "[REL] " << path << ": imports from module "
                          << mod_id << " which is not loaded; skipped\n";
                continue;
            }
            uint32_t tgt_sec = 0, run = 0;
            size_t o = rel;
            while (o + 8 <= img.size()) {
                uint16_t roff = be16(img, o);
                uint8_t rtype = img[o + 2];
                uint8_t rsec = img[o + 3];
                uint32_t addend = be32(img, o + 4);
                o += 8;
                if (rtype == R_END) break;
                if (rtype == R_SECTION) { tgt_sec = rsec; run = 0; continue; }
                run += roff;
                if (rtype == R_NOP || rtype == R_MRKREF) continue;

                uint32_t patch_fo = m.sec_fileoff[tgt_sec] + run;
                uint32_t patch_addr = m.sec_addr[tgt_sec] + run;
                uint32_t value = target ? (target->sec_addr[rsec] + addend) : addend;
                switch (rtype) {
                case R_ADDR32:
                    wr32(img, patch_fo, value);
                    if (in_text(value)) pointer_roots.push_back(value);
                    break;
                case R_ADDR16:
                case R_ADDR16_LO: wr16(img, patch_fo, value & 0xFFFF); break;
                // Metrowerks/OSLink emit ADDR16_HI paired with a sign-extending
                // low (addi/lwz), and this game's OSLink applies the high-adjusted
                // (@ha) value: verified against the game's own relocated code
                // (ctor lis=0x8053 for 0x8052DA08, bss lis=0x8058). Applying plain
                // #hi left every lis/addi data pointer 0x10000 too low, so the REL
                // dereferenced its ctor table on the stack and jumped to garbage.
                case R_ADDR16_HI:
                case R_ADDR16_HA:
                    wr16(img, patch_fo, ((value >> 16) + ((value & 0x8000) ? 1 : 0)) & 0xFFFF);
                    break;
                case R_ADDR24: {
                    uint32_t insn = be32(img, patch_fo);
                    wr32(img, patch_fo, (insn & ~0x03FFFFFCu) | (value & 0x03FFFFFC));
                    break;
                }
                case R_REL24: {
                    uint32_t insn = be32(img, patch_fo);
                    uint32_t delta = value - patch_addr;
                    wr32(img, patch_fo, (insn & ~0x03FFFFFCu) | (delta & 0x03FFFFFC));
                    if (target) function_roots.push_back(value);
                    break;
                }
                case R_REL14: {
                    uint32_t insn = be32(img, patch_fo);
                    uint32_t delta = value - patch_addr;
                    wr32(img, patch_fo, (insn & ~0x0000FFFCu) | (delta & 0x0000FFFC));
                    break;
                }
                default: break;
                }
            }
        }

        // prolog / epilog / unresolved: section index bytes at 0x30..0x32,
        // offsets at 0x34..0x3C.
        for (int k = 0; k < 3; ++k) {
            uint8_t sec = img[0x30 + k];
            uint32_t off = be32(img, 0x34 + k * 4);
            if (sec && sec < m.sec_addr.size() && m.sec_exec[sec])
                function_roots.push_back(m.sec_addr[sec] + off);
        }

        int added = 0;
        for (size_t i = 0; i < m.sec_addr.size(); ++i) {
            if (!m.sec_exec[i] || m.sec_size[i] == 0 || m.sec_fileoff[i] == 0) continue;
            Section sec;
            sec.address = m.sec_addr[i];
            sec.size = m.sec_size[i];
            sec.is_text = true;
            sec.is_bss = false;
            sec.data.assign(img.begin() + m.sec_fileoff[i],
                            img.begin() + m.sec_fileoff[i] + m.sec_size[i]);
            sections.push_back(std::move(sec));
            ++added;
            std::cout << "[REL] " << path << " (id " << m.id << ") text sec" << i
                      << " @0x" << std::hex << m.sec_addr[i] << " size 0x"
                      << m.sec_size[i] << std::dec << "\n";
        }
        // Relocated data sections (.rodata/.data: switch jump tables, vtables),
        // published as an analysis-only view so the analyzer can resolve the
        // jump tables of REL functions. Without it every switch in a REL
        // ended in an unresolved bctr and ran in the interpreter. Marked
        // is_bss: the game loads this data itself through OSLink at run time,
        // so nothing may treat it as an image to preload, and the analyzer
        // keeps it out of its raw "every code pointer is a function" scan
        // (case labels are not functions); it reads REL data pointers as
        // weak roots instead (Analyzer::analyze).
        for (size_t i = 0; i < m.sec_addr.size(); ++i) {
            if (m.sec_exec[i] || m.sec_size[i] == 0 || m.sec_fileoff[i] == 0) continue;
            if ((size_t)m.sec_fileoff[i] + m.sec_size[i] > img.size()) continue;
            Section sec;
            sec.address = m.sec_addr[i];
            sec.size = m.sec_size[i];
            sec.is_text = false;
            sec.is_bss = true;
            sec.data.assign(img.begin() + m.sec_fileoff[i],
                            img.begin() + m.sec_fileoff[i] + m.sec_size[i]);
            sections.push_back(std::move(sec));
        }
        if (added) ++loaded;
    }

    // Data pointers into code are mostly function pointers (vtables, profile
    // tables, callbacks), but switch jump tables also point into .text at case
    // labels. Keep only targets shaped like a function entry.
    auto read_text = [&](uint32_t addr, uint32_t& out) {
        for (const auto& sec : sections)
            if (sec.is_text && addr >= sec.address && addr + 4 <= sec.address + sec.size) {
                size_t o = addr - sec.address;
                out = ((uint32_t)sec.data[o] << 24) | ((uint32_t)sec.data[o + 1] << 16) |
                      ((uint32_t)sec.data[o + 2] << 8) | sec.data[o + 3];
                return true;
            }
        return false;
    };
    size_t accepted = 0;
    for (uint32_t addr : pointer_roots) {
        uint32_t insn = 0, prev = 0;
        if (!read_text(addr, insn)) continue;
        bool prologue = (insn & 0xFFFF8000u) == 0x94218000u  // stwu r1,-X(r1)
                        || insn == 0x7C0802A6u;              // mflr r0
        bool after_end = read_text(addr - 4, prev) &&
                         (prev == 0x4E800020u || prev == 0);  // blr / padding
        if (prologue || after_end) {
            function_roots.push_back(addr);
            ++accepted;
        }
    }
    std::cout << "[REL] " << function_roots.size() << " code roots ("
              << accepted << " from data pointers)\n";
    return loaded;
}

bool Executable::load_unpacked_game(const std::string& path) {
    std::string dol_path = path;
    std::string boot_bin_path = "";

    if (std::filesystem::is_directory(path)) {
        dol_path = path + "/sys/main.dol";
        boot_bin_path = path + "/sys/boot.bin";
        if (!std::filesystem::exists(dol_path)) {
            dol_path = path + "/main.dol";
            boot_bin_path = path + "/boot.bin";
        }
        if (!std::filesystem::exists(dol_path)) {
            std::cerr << "Could not find main.dol in " << path << " or " << path << "/sys/" << std::endl;
            return false;
        }
    } else {
        
        if (!std::filesystem::exists(dol_path)) {
            std::cerr << "File does not exist: " << dol_path << std::endl;
            return false;
        }
    }

    if (!boot_bin_path.empty() && std::filesystem::exists(boot_bin_path)) {
        std::ifstream boot(boot_bin_path, std::ios::binary);
        if (boot.is_open()) {
            char game_id_buf[7] = {0};
            if (boot.read(game_id_buf, 6)) {
                nwii::runtime::Config::get().game_id = std::string(game_id_buf);
                std::cout << "[Loader] Parsed Game ID from boot.bin: " << nwii::runtime::Config::get().game_id << std::endl;
            }
        }
    }

    return load_dol(dol_path);
}

} 
} 
