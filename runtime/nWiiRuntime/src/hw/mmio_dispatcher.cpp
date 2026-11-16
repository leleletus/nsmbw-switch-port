#include "runtime/hw/mmio_dispatcher.h"
#include <iostream>
#include <algorithm>

namespace nwii::runtime {

namespace {
// Window index for the page lookup table, or -1 outside the three windows.
inline int lut_window(uint32_t addr) {
    switch (addr >> 16) {
    case 0xCC00: return 0;
    case 0xCD00: return 1;
    case 0x0D00: return 2;
    default: return -1;
    }
}
constexpr uint32_t kWindowBase[3] = {0xCC000000u, 0xCD000000u, 0x0D000000u};
}  // namespace

void MMIODispatcher::rebuild_lut() {
    for (int w = 0; w < 3; ++w) {
        for (uint32_t p = 0; p < 256; ++p) {
            const uint32_t lo = kWindowBase[w] | (p << 8), hi = lo + 0xFF;
            int16_t entry = kLutScan;
            for (size_t i = 0; i < m_regions.size() && i < 0x7FFF; ++i) {
                const MMIORegion& r = m_regions[i];
                if (r.end_addr < lo || r.start_addr > hi)
                    continue;  // does not touch this page
                if (r.start_addr <= lo && r.end_addr >= hi)
                    entry = (int16_t)i;
                break;  // first region touching the page decides
            }
            m_lut[w][p] = entry;
        }
    }
}

const MMIORegion* MMIODispatcher::find(uint32_t addr) const {
    const int w = lut_window(addr);
    if (w >= 0) {
        const int16_t i = m_lut[w][(addr >> 8) & 0xFF];
        if (i != kLutScan)
            return &m_regions[(size_t)i];
    }
    for (const auto& region : m_regions)
        if (addr >= region.start_addr && addr <= region.end_addr)
            return &region;
    return nullptr;
}

void MMIODispatcher::register_region(uint32_t start, uint32_t end, MMIORegionReadCallback read_cb, MMIORegionWriteCallback write_cb) {
    m_regions.push_back({start, end, std::move(read_cb), std::move(write_cb), nullptr, nullptr});
    rebuild_lut();
}

void MMIODispatcher::register_region(uint32_t start, uint32_t end, MMIORegionReadCallback read_cb, MMIORegionWriteCallback write_cb,
                                     MMIORegionRead16Callback read16_cb, MMIORegionWrite16Callback write16_cb) {
    m_regions.push_back({start, end, std::move(read_cb), std::move(write_cb),
                         std::move(read16_cb), std::move(write16_cb)});
    rebuild_lut();
}

uint32_t MMIODispatcher::read32(uint32_t addr) {
    if (const MMIORegion* region = find(addr)) {
        if (region->read_cb) return region->read_cb(addr);
        return 0;
    }
    std::cout << "[MMIO] Unhandled read32 at 0x" << std::hex << addr << std::dec << std::endl;
    return 0;
}

void MMIODispatcher::write32(uint32_t addr, uint32_t val) {
    if (const MMIORegion* region = find(addr)) {
        if (region->write_cb)
            region->write_cb(addr, val);
        return;
    }
    std::cout << "[MMIO] Unhandled write32 at 0x" << std::hex << addr << " val=0x" << val << std::dec << std::endl;
}

uint16_t MMIODispatcher::read16(uint32_t addr) {
    if (const MMIORegion* region = find(addr)) {
        if (region->read16_cb) return region->read16_cb(addr);
        if (region->read_cb) return (uint16_t)region->read_cb(addr);
        return 0;
    }
    std::cout << "[MMIO] Unhandled read16 at 0x" << std::hex << addr << std::dec << std::endl;
    return 0;
}

void MMIODispatcher::write16(uint32_t addr, uint16_t val) {
    if (const MMIORegion* region = find(addr)) {
        if (region->write16_cb) region->write16_cb(addr, val);
        else if (region->write_cb) region->write_cb(addr, val);
        return;
    }
    std::cout << "[MMIO] Unhandled write16 at 0x" << std::hex << addr << " val=0x" << val << std::dec << std::endl;
}

void MMIODispatcher::clear() {
    m_regions.clear();
    rebuild_lut();
}

}
