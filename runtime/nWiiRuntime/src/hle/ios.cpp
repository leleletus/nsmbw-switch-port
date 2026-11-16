#include "../core/s2.h"  // r34s2
// r34s2: compilacion con ThreadSanitizer (tools/build_s2.sh TSAN=1). La entrega de interrupciones hace
// longjmp a un setjmp del codigo generado (sin instrumentar), que el interceptor de TSan no conoce: se
// llama al longjmp de libc y las funciones del camino no se instrumentan (pila sombra equilibrada).
#include <csetjmp>
#if defined(__SANITIZE_THREAD__)
#include <dlfcn.h>
#define NWII_NO_TSAN __attribute__((no_sanitize("thread")))
[[noreturn]] NWII_NO_TSAN static void nwii_real_longjmp(jmp_buf &b) {
  static void (*real)(jmp_buf, int) = (void (*)(jmp_buf, int))dlsym(dlopen("libc.so.6", RTLD_NOW), "longjmp");
  real(b, 1);
  __builtin_unreachable();
}
#define NWII_LONGJMP(b) nwii_real_longjmp(b)
#else
#define NWII_NO_TSAN
#define NWII_LONGJMP(b) longjmp(b, 1)
#endif
#include "runtime/nwii_env.h"

void vtl_note(uint32_t kind, uint32_t arg);  // r37v (gx.cpp): NWII_V_TL
void vtl_idle_turn(bool at_end);             // r37v (gx.cpp)
#include "runtime/platform/platform.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include "runtime/hw/hw.h"
#include "runtime/event_scheduler.h"
#include "runtime/os_thread.h"
#include "runtime/virtual_disc.h"

namespace nwii { namespace runtime { void poll_ee_kick(CPUContext &ctx); } }  // [r33-g7] definida abajo
using namespace nwii::runtime;

namespace nwii::runtime {
    std::string read_guest_string(CPUContext &ctx, uint32_t addr, int max_len = 256);
    bool valid_callback(uint32_t cb);
}

#include "runtime/ios_device.h"
#include "runtime/ios_kernel.h"
#include "runtime/devices.h"
#include <iostream>
#include <vector>

extern "C" void run_game(nwii::runtime::CPUContext &ctx);

static uint32_t phys_to_virt(uint32_t addr) {
  if (addr == 0)
    return 0;
  if (addr >= 0x80000000 && addr < 0x94000000)
    return addr;
  if (addr >= 0x00800000 && addr < 0x04000000)
    return addr | 0x90000000;
  if (addr < 0x00800000)
    return addr | 0x80000000;
  if (addr >= 0x10000000 && addr < 0x14000000)
    return addr | 0x90000000;
  return addr;
}

std::string nwii::runtime::read_guest_string(CPUContext &ctx, uint32_t addr,
                                     int max_len) {
  std::string path;
  if (addr == 0) {
    std::cout << "[HLE IOS] read_guest_string: null address! pc=0x" << std::hex
              << ctx.pc << " lr=0x" << ctx.lr << std::dec << std::endl;
    return path;
  }
  uint32_t vaddr = phys_to_virt(addr);
  if (vaddr >= 0x80000000 && vaddr < 0x94000000) {
    for (int i = 0; i < max_len; ++i) {
      char c = ctx.mmu.read8(vaddr + i);
      if (c == '\0')
        break;
      if (c >= 32 && c <= 126)
        path += c;
    }
    if (path.empty()) {
      std::cout << "[HLE IOS] read_guest_string: string at 0x" << std::hex
                << addr << " (vaddr 0x" << vaddr
                << ") is empty (first char is 0x" << (int)ctx.mmu.read8(vaddr)
                << ") pc=0x" << ctx.pc << " lr=0x" << ctx.lr << std::dec
                << std::endl;
    }
  } else {
    std::cout << "[HLE IOS] read_guest_string: invalid translated address 0x"
              << std::hex << vaddr << " (orig: 0x" << addr << ") pc=0x"
              << ctx.pc << " lr=0x" << ctx.lr << std::dec << std::endl;
  }
  return path;
}

static constexpr int32_t IPC_EIO = -8;


static constexpr uint32_t IPC_POOL_BASE = 0x93EE0000;
static constexpr uint32_t IPC_SLOT_SIZE = 128; 
static constexpr uint32_t IPC_POOL_SLOTS = 64;
static uint32_t ipc_pool_next = 0; 




static uint32_t ios_guest_heap_ptr = 0x93E40000;
static uint32_t ios_ipc_heap_ptr = 0x93E00000;

static uint32_t ios_guest_alloc(CPUContext &ctx, uint32_t size, uint32_t align,
                                int heap_id = 0) {
  if (align < 32)
    align = 32;
  
  uint32_t &heap =
      (heap_id == 0 && size <= 128) ? ios_ipc_heap_ptr : ios_guest_heap_ptr;
  heap = (heap + align - 1) & ~(align - 1);
  uint32_t ptr = heap;
  heap += (size + align - 1) & ~(align - 1);
  for (uint32_t i = 0; i < size; ++i)
    ctx.mmu.write8(ptr + i, 0);
  return ptr;
}

static uint32_t ipc_pool_alloc(CPUContext &ctx) {
  uint32_t slot = ipc_pool_next;
  ipc_pool_next = (ipc_pool_next + 1) % IPC_POOL_SLOTS;
  uint32_t ptr = IPC_POOL_BASE + slot * IPC_SLOT_SIZE;
  
  for (uint32_t i = 0; i < IPC_SLOT_SIZE; ++i)
    ctx.mmu.write8(ptr + i, 0);
  return ptr;
}


bool nwii::runtime::valid_callback(uint32_t cb) {
  return cb != 0 && cb != 0xFFFFFFFFu && cb >= 0x80000000u && cb < 0x82000000u;
}

static std::shared_ptr<std::ifstream> g_disc_file;
static bool g_disc_is_wbfs = false;
static uint32_t g_wbfs_block_size = 0;
static std::vector<uint16_t> g_wbfs_bat;

static bool init_wbfs(const std::filesystem::path &path) {
  auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!file->is_open())
    return false;

  char magic[4];
  file->read(magic, 4);
  if (magic[0] != 'W' || magic[1] != 'B' || magic[2] != 'F' ||
      magic[3] != 'S') {
    return false;
  }

  uint8_t shift;
  file->read(reinterpret_cast<char *>(&shift), 1);
  g_wbfs_block_size = 1u << (shift + 17);

  file->seekg(0, std::ios::end);
  size_t file_size = file->tellg();
  file->seekg(0x100, std::ios::beg);

  size_t num_entries = (file_size - 0x100) / 2;
  if (num_entries > 65536)
    num_entries = 65536;

  g_wbfs_bat.resize(num_entries);
  for (size_t i = 0; i < num_entries; ++i) {
    uint16_t entry;
    file->read(reinterpret_cast<char *>(&entry), 2);
    g_wbfs_bat[i] = ((entry >> 8) & 0xFF) | ((entry << 8) & 0xFF00);
  }

  g_disc_file = file;
  g_disc_is_wbfs = true;
  std::cout << "[HLE IOS] Opened WBFS: " << path
            << " block_size=" << g_wbfs_block_size << std::endl;
  return true;
}

static void init_iso(const std::filesystem::path &path) {
  g_disc_file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (g_disc_file->is_open()) {
    g_disc_is_wbfs = false;
    std::cout << "[HLE IOS] Opened ISO: " << path << std::endl;
  }
}

static std::shared_ptr<std::ifstream> get_disc_file() {
  if (!g_disc_file) {
    auto &cfg = Config::get();
    std::filesystem::path game_dir(cfg.game_dir);
    std::filesystem::path p;

    p = game_dir / "disc.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir / "game.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir.parent_path() / "game.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir.parent_path() / "disc.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }

    if (std::filesystem::exists(game_dir)) {
      for (const auto &entry : std::filesystem::directory_iterator(game_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".wbfs") {
          if (init_wbfs(entry.path()))
            return g_disc_file;
        }
      }
    }
    if (std::filesystem::exists(game_dir.parent_path())) {
      for (const auto &entry :
           std::filesystem::directory_iterator(game_dir.parent_path())) {
        if (entry.is_regular_file() && entry.path().extension() == ".wbfs") {
          if (init_wbfs(entry.path()))
            return g_disc_file;
        }
      }
    }
  }
  return g_disc_file;
}

static void read_disc_data(uint8_t *dst, uint64_t offset, uint32_t length) {
  
  auto &vd = VirtualDisc::get();
  if (!vd.valid())
    vd.init(Config::get().game_dir);
  if (vd.valid() && vd.read(offset, length, dst))
    return;

  auto file = get_disc_file();
  if (!file || !file->is_open()) {
    std::memset(dst, 0, length);
    return;
  }

  if (!g_disc_is_wbfs) {
    file->seekg(offset);
    file->read(reinterpret_cast<char *>(dst), length);
  } else {
    uint32_t done = 0;
    while (done < length) {
      uint64_t block_idx = (offset + done) / g_wbfs_block_size;
      uint32_t block_off = (offset + done) % g_wbfs_block_size;
      uint32_t chunk =
          std::min<uint32_t>(length - done, g_wbfs_block_size - block_off);

      if (block_idx < g_wbfs_bat.size() && g_wbfs_bat[block_idx] != 0) {
        uint64_t phys =
            (uint64_t)g_wbfs_bat[block_idx] * g_wbfs_block_size + block_off;
        file->seekg(phys);
        file->read(reinterpret_cast<char *>(dst + done), chunk);
      } else {
        std::memset(dst + done, 0, chunk);
      }
      done += chunk;
    }
  }
}

namespace nwii {
namespace runtime {

struct PendingCallback {
  uint32_t callback;
  uint32_t arg1;
  uint32_t arg2;
};

static std::vector<PendingCallback> g_pending_callbacks;



} 
} 

static void di_read_internal(nwii::runtime::MMU *mmu, uint32_t inbuf, uint32_t outbuf,
                             uint32_t callback, uint32_t userdata,
                             bool is_async, uint32_t out_size = 0) {
  
  uint32_t cmd = 0, transferSize = 0, addr = 0, length = 0;
  uint64_t offset = 0;

  uint32_t ipc_ptr = phys_to_virt(inbuf);

  if (ipc_ptr >= 0x80000000 && ipc_ptr < 0x94000000) {
    uint32_t raw_cmd = mmu->read32(ipc_ptr + 0x00);
    cmd = raw_cmd >> 24; 
    if (cmd == 1) {
      cmd = 0x12; 
    } else if (cmd == 0) {
      
      if (raw_cmd == 0x20)
        cmd = 0x20;
    }

    transferSize = mmu->read32(ipc_ptr + 0x04);
    addr = mmu->read32(ipc_ptr + 0x08);

    uint32_t val10 = mmu->read32(ipc_ptr + 0x10);
    uint32_t val14 = mmu->read32(ipc_ptr + 0x14);

    if (nwii::runtime::Config::get().platform ==
        nwii::runtime::Platform::GameCube) {
      offset = ((uint64_t)val14) << 2; 
    } else {
      
      offset = (((uint64_t)(val10 & 0xFFFF) << 32) | val14) << 2;
    }

    length = mmu->read32(ipc_ptr + 0x18);
  }
  
  if (addr == 0 && outbuf != 0) {
      addr = outbuf;
  }

  std::cout << "[HLE IOS] DI_Read" << (is_async ? "Async" : "") << ": inbuf=0x"
            << std::hex << inbuf << " cmd=0x" << cmd
            << " transferSize=" << std::dec << transferSize << " addr=0x"
            << std::hex << addr << " cb=0x" << callback << " offset=0x"
            << offset << " length=" << std::dec << length << " outbuf=0x" << std::hex << outbuf << std::endl;

  bool use_defaults =
      (cmd != 0x20 && cmd != 0x12 && cmd != 0x95 && cmd != 0x71 && cmd != 0x72 && cmd != 0x96 && cmd != 0xe3) || (offset > 0x1FFFFFFFFFFULL);

  if (use_defaults) {
    std::cout << "[HLE IOS] DI_Read: Using defaults (invalid structure)"
              << std::endl;
    cmd = 0x20;
    transferSize = 2;
    offset = 0;
    length = 0x440;
  }

  uint32_t buf_ptr = phys_to_virt(addr);
  uint32_t total_length = transferSize * 2048;
  if (length > 0 && length < total_length)
    total_length = length;
  if (total_length == 0 && length > 0)
    total_length = length;
  if (total_length == 0)
    total_length = 0x440;
  
  if (out_size > 0 && total_length > out_size)
    total_length = out_size;

  std::cout << "[HLE IOS] DI_Read final: offset=0x" << std::hex
            << (uint32_t)offset << " total_length=" << std::dec << total_length
            << " buf_ptr=0x" << std::hex << buf_ptr << std::dec << std::endl;

  auto file = get_disc_file();
  bool success = false;
  uint32_t bytes_read = 0;
  bool have_media = (file && file->is_open()) || VirtualDisc::get().valid();

  if (have_media && buf_ptr != 0 && total_length > 0) {
    std::vector<uint8_t> temp(total_length);
    read_disc_data(temp.data(), (uint32_t)offset, total_length);
    bytes_read = total_length;
    for (uint32_t i = 0; i < bytes_read; i++)
      mmu->write8(buf_ptr + i, temp[i]);
    std::cout << "[DI] Read " << bytes_read << " bytes" << std::endl;
    success = true;
  } else if (buf_ptr != 0) {
    if (offset == 0) {
      const std::string& gid = Config::get().game_id;
      for (uint32_t i = 0; i < total_length; i++) mmu->write8(buf_ptr + i, 0);
      for (int i = 0; i < (int)gid.size() && i < 4; i++)
        mmu->write8(buf_ptr + i, (uint8_t)gid[i]);
      mmu->write32(buf_ptr + 0x18, 0x5D1C9EA3);
      std::cout << "[DI] Faked disc header (" << gid << ")" << std::endl;
      success = true;
      bytes_read = total_length;
    } else {
      
      std::cout << "[DI] No disc file, zeroing out read buffer of size " << total_length << std::endl;
      for (uint32_t i = 0; i < total_length; i++) mmu->write8(buf_ptr + i, 0);
      success = true;
      bytes_read = total_length;
    }
  }
}

void di_read_internal_shared(nwii::runtime::MMU* mmu, uint32_t inbuf, uint32_t outbuf,
                              uint32_t callback, uint32_t userdata, bool is_async,
                              uint32_t out_size) {
    di_read_internal(mmu, inbuf, outbuf, callback, userdata, is_async, out_size);
}

extern "C" {
void IOS_Open(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_open(ctx);
}

void IOS_OpenAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_open_async(ctx);
}

void IOS_Close(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_close(ctx);
}

void IOS_Read(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_read(ctx);
}

void IOS_Write(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_write(ctx);
}

void IOS_Ioctl(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctl(ctx);
}

void IOS_Ioctlv(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctlv(ctx);
}

void iosAlloc(CPUContext &ctx) {
  uint32_t heap_id = ctx.gpr[3];
  uint32_t size = ctx.gpr[4];
  uint32_t align = ctx.gpr[5];
  uint32_t ptr = ios_guest_alloc(ctx, size, align, heap_id);
  std::cout << "[HLE IOS] iosAlloc: heap=" << heap_id << " size=" << size
            << " align=" << align << " -> ptr=0x" << std::hex << ptr << " LR=0x" << ctx.lr << std::dec
            << std::endl;
  ctx.gpr[3] = ptr;
  ctx.pc = ctx.lr;
}


void iosFree(CPUContext &ctx) {
  uint32_t heap_id = ctx.gpr[3];
  uint32_t ptr = ctx.gpr[4];
  std::cout << "[HLE IOS] iosFree: heap=" << heap_id << " ptr=0x" << std::hex
            << ptr << " LR=0x" << ctx.lr << std::dec << std::endl;
  ctx.gpr[3] = 0; 
  ctx.pc = ctx.lr;
}

void IOS_IoctlAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctl_async(ctx);
}

void IOS_IoctlvAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctlv_async(ctx);
}

void IOS_Seek(CPUContext &ctx) {
  if (Config::get().platform == Platform::GameCube) {
    ctx.gpr[3] = -1;
    ctx.pc = ctx.lr;
    return;
  }
  int32_t fd = (int32_t)ctx.gpr[3];
  int32_t offset = (int32_t)ctx.gpr[4];
  int32_t whence = (int32_t)ctx.gpr[5];

  int32_t result = IOSKernel::get().seek(ctx, fd, offset, whence);
  ctx.gpr[3] = result;
  ctx.pc = ctx.lr;
}

void IOS_CloseAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_close_async(ctx);
}

void IOS_ReadAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_read_async(ctx);
}

void IOS_WriteAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_write_async(ctx);
}

void IOS_SeekAsync(CPUContext &ctx) {
  if (Config::get().platform == Platform::GameCube) {
    ctx.gpr[3] = -1;
    ctx.pc = ctx.lr;
    return;
  }
  int32_t fd = (int32_t)ctx.gpr[3];
  int32_t offset = (int32_t)ctx.gpr[4];
  int32_t whence = (int32_t)ctx.gpr[5];
  uint32_t callback = ctx.gpr[6];
  uint32_t userdata = ctx.gpr[7];

  int32_t result = IOSKernel::get().seek(ctx, fd, offset, whence);

  if (valid_callback(callback)) {
    ctx.queue_callback(callback, result, userdata);
  }

  ctx.gpr[3] = IPC_OK;
  ctx.pc = ctx.lr;
}

} 

namespace nwii {
namespace runtime {
extern MMU *g_mmu;
}
} 

extern "C" int32_t handle_ios_ipc(CPUContext& ctx, uint32_t request_addr) {
  if (!nwii::runtime::g_mmu)
    return 0;

  

  uint32_t virt_addr = request_addr;
  if (virt_addr < 0x01800000) {
    virt_addr |= 0x80000000;
  } else if (virt_addr >= 0x10000000 && virt_addr < 0x14000000) {
    virt_addr = (virt_addr & 0x03FFFFFF) | 0x90000000;
  }

  // IOS IPC request layout (Dolphin IOS::HLE):
  
  uint32_t cmd = nwii::runtime::g_mmu->read32(virt_addr);
  int32_t result = IPC_OK;

  if (cmd == 1) { 
    
    uint32_t path_addr = nwii::runtime::g_mmu->read32(virt_addr + 0x0C);
    uint32_t mode = nwii::runtime::g_mmu->read32(virt_addr + 0x10);
    
    if (path_addr < 0x01800000)
      path_addr |= 0x80000000;
    else if (path_addr >= 0x10000000 && path_addr < 0x14000000)
      path_addr = (path_addr & 0x03FFFFFF) | 0x90000000;

    std::string path;
    for (int i = 0; i < 64; ++i) {
      char c = (char)ctx.mmu.read8(path_addr + i);
      if (c == '\0') break;
      path += c;
    }
    result = IOSKernel::get().open(ctx, path, mode);
    std::cout << "[HW IPC] Open path='" << path << "' -> fd=" << result << "\n";
  } else if (cmd == 2) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    result = IOSKernel::get().close(ctx, fd);
  } else if (cmd == 3) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t buf = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t len = ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().read(ctx, fd, buf, len);
  } else if (cmd == 4) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t buf = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t len = ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().write(ctx, fd, buf, len);
  } else if (cmd == 5) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    int32_t offset = (int32_t)ctx.mmu.read32(virt_addr + 0x0C);
    int32_t whence = (int32_t)ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().seek(ctx, fd, offset, whence);
  } else if (cmd == 6) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t ioctl_cmd = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t in_buf = ctx.mmu.read32(virt_addr + 0x10);
    uint32_t in_size = ctx.mmu.read32(virt_addr + 0x14);
    uint32_t out_buf = ctx.mmu.read32(virt_addr + 0x18);
    uint32_t out_size = ctx.mmu.read32(virt_addr + 0x1C);

    std::cout << "[HW IPC] Ioctl fd=" << fd << " cmd=0x" << std::hex << ioctl_cmd << std::dec << "\n";

    IpcRequest req{};
    req.fd = fd;
    req.req_addr = virt_addr;
    req.ioctl_cmd = ioctl_cmd;
    req.in_buf = in_buf;
    req.in_size = in_size;
    req.out_buf = out_buf;
    req.out_size = out_size;
    result = IOSKernel::get().ioctl(ctx, req);
  } else if (cmd == 7) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t ioctl_cmd = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t arg_in = ctx.mmu.read32(virt_addr + 0x10);
    uint32_t arg_out = ctx.mmu.read32(virt_addr + 0x14);
    uint32_t arg_array = ctx.mmu.read32(virt_addr + 0x18);

    IpcRequest req{};
    req.cmd = cmd;
    req.fd = fd;
    req.req_addr = virt_addr;
    req.ioctl_cmd = ioctl_cmd;
    req.arg_cnt_in = arg_in;
    req.arg_cnt_out = arg_out;

    std::cout << "[HW IPC] Parsed IOCTLV fd=" << fd << " cmd=" << std::hex << ioctl_cmd << std::dec 
              << " in=" << arg_in << " out=" << arg_out << "\n";

    for (uint32_t i = 0; i < arg_in + arg_out; i++) {
        IoctlvVector vec;
        vec.addr = ctx.mmu.read32(arg_array + i * 8);
        vec.len = ctx.mmu.read32(arg_array + i * 8 + 4);
        req.ioctlv_vecs.push_back(vec);
    }

    result = IOSKernel::get().ioctlv(ctx, req);
  }

  std::cout << "[HW IPC] cmd=" << cmd << " virt=0x" << std::hex << virt_addr
            << " -> result=" << (int32_t)result << std::dec << std::endl;

  if (result != -0x70000001) { 
      nwii::runtime::g_mmu->write32(virt_addr + 4, (uint32_t)(int32_t)result);
  }
  return result;
}

namespace nwii {
namespace runtime {

void init_ipc_client(CPUContext &ctx) {
  IOSKernel::get().init();
  nwii::runtime::devices::register_all();
}

bool handle_syscall(CPUContext &ctx) {
  uint32_t syscall_id = ctx.gpr[0];
  uint32_t old_pc = ctx.pc; 

  switch (syscall_id) {
  case 0x61:
    IOS_Open(ctx);
    break;
  case 0x62:
    IOS_Close(ctx);
    break;
  case 0x63:
    IOS_Read(ctx);
    break;
  case 0x64:
    IOS_Write(ctx);
    break;
  case 0x65:
    IOS_Seek(ctx);
    break;
  case 0x66:
    IOS_Ioctl(ctx);
    break;
  case 0x67:
    IOS_Ioctlv(ctx);
    break;
  case 0x68:
    IOS_OpenAsync(ctx);
    break;
  case 0x69:
    IOS_CloseAsync(ctx);
    break;
  case 0x6A:
    IOS_ReadAsync(ctx);
    break;
  case 0x6B:
    IOS_IoctlAsync(ctx);
    break;
  case 0x6C:
    IOS_IoctlvAsync(ctx);
    break;
  default:

    
    if (process_pending_callbacks(ctx))
      return true;
    break;
  }

  ctx.pc = old_pc; 
  return false;
}

} 
} 

#include "runtime/cpu_context.h"

// process_pending_callbacks runs at every backward branch of recompiled code;
// read debug switches once instead of scanning the environment each time.
static const bool g_env_sample = NWII_ENV("NWII_SAMPLE") != nullptr;

namespace nwii::runtime {



bool g_ipc_reply_pending = false;

int g_ipc_interrupt_delay = 0;


static constexpr uint32_t CALLBACK_STACK_TOP_DEFAULT = 0x816F0000;

static inline uint32_t get_callback_stack_top() {
    uint32_t v = Config::get().game_profile.callback_stack_top;
    return v ? v : CALLBACK_STACK_TOP_DEFAULT;
}

void hle_drive_thread_queue(CPUContext& ctx) {
    
}



// OSContext is 0x2C8 bytes; leave room for it plus a stack frame below it.
static constexpr uint32_t OS_CONTEXT_FRAME = 0x300;

// OSClearContext: mode and state halfwords; drops FPU ownership if held.
static uint32_t hle_clear_context(CPUContext &ctx, uint32_t c) {
  ctx.mmu.write16(c + 0x1A0, 0);
  ctx.mmu.write16(c + 0x1A2, 0);
  if (ctx.mmu.read32(0x800000D8) == c)
    ctx.mmu.write32(0x800000D8, 0);
  return c;
}

// OSSetCurrentContext: virtual pointer at 0xD4, physical at 0xC0.
static void hle_set_current_context(CPUContext &ctx, uint32_t c) {
  ctx.mmu.write32(0x800000D4, c);
  ctx.mmu.write32(0x800000C0, c & 0x3FFFFFFF);
}

// __OSDispatchInterrupt runs every handler between OSDisableScheduler and
// OSEnableScheduler; handlers and IOS callbacks dispatched natively do the
// same through the game's Reschedule counter ([hle] sched_lock_offset), so
// a thread woken inside them only runs once they return (see __OSReschedule
// on the 0xFFFFFFFC path).
static uint32_t g_dispatch_sched_lock = 0;

static void hle_lock_scheduler_for_dispatch(CPUContext &ctx) {
  uint32_t off = Config::get().game_profile.sched_lock_offset;
  uint32_t r13 = ctx.gpr[13];
  if (off == 0 || r13 < 0x80200000u || r13 >= 0x81800000u)
    return;
  g_dispatch_sched_lock = r13 - off;
  ctx.mmu.write32(g_dispatch_sched_lock,
                  ctx.mmu.read32(g_dispatch_sched_lock) + 1);
}

static void hle_unlock_scheduler_after_dispatch(CPUContext &ctx) {
  if (!g_dispatch_sched_lock)
    return;
  ctx.mmu.write32(g_dispatch_sched_lock,
                  ctx.mmu.read32(g_dispatch_sched_lock) - 1);
  g_dispatch_sched_lock = 0;
}

// __OSReschedule has no signature in the database, but its shape is fixed:
//   lwz r0,RunQueueHint(r13); cmpwi r0,0; beqlr; li r3,0; b SelectThread
// and SelectThread compares OSGetCurrentContext() with *0x800000E4.
static uint32_t hle_find_os_reschedule(CPUContext &ctx) {
  for (uint32_t a = 0x80004000; a < 0x80400000; a += 4) {
    if ((ctx.mmu.read32(a) & 0xFFFF0000) != 0x800D0000 ||
        ctx.mmu.read32(a + 4) != 0x2C000000 ||
        ctx.mmu.read32(a + 8) != 0x4D820020 ||
        ctx.mmu.read32(a + 12) != 0x38600000)
      continue;
    uint32_t b = ctx.mmu.read32(a + 16);
    if ((b & 0xFC000003) != 0x48000000)
      continue;
    int32_t off = (int32_t)((b & 0x03FFFFFC) << 6) >> 6;
    uint32_t target = a + 16 + off;
    for (uint32_t i = 0; i < 32; ++i) {
      if (ctx.mmu.read32(target + i * 4) == 0x80A400E4) {  // lwz r5,0xE4(r4)
        std::cout << "[HLE] __OSReschedule at 0x" << std::hex << a
                  << " (SelectThread 0x" << target << ")" << std::dec << "\n";
        return a;
      }
    }
  }
  std::cout << "[HLE] __OSReschedule not found; callbacks will not reschedule\n";
  return 0;
}

static void hle_save_context_to_guest(CPUContext &ctx, uint32_t c) {
  if (g_env_sample) {
      static int n = 0;
      if (n++ < 40)
          std::cout << "[CtxSave] #" << n << " ctx=0x" << std::hex << c
                    << " srr0<=0x" << ctx.pc << " msr=0x" << ctx.msr
                    << " lr=0x" << ctx.lr << std::dec << "\n";
  }
  for (int i = 0; i < 32; i++)
    ctx.mmu.write32(c + i * 4, ctx.gpr[i]);

  

  ctx.mmu.write16(c + 0x1A2, ctx.mmu.read16(c + 0x1A2) | 0x0002);
  uint32_t cr_val = 0;
  for (int i = 0; i < 8; i++) {
    cr_val |= (ctx.cr[i].lt ? 8u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].gt ? 4u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].eq ? 2u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].so ? 1u : 0) << (28 - i * 4);
  }
  ctx.mmu.write32(c + 0x80, cr_val);
  ctx.mmu.write32(c + 0x84, ctx.lr);
  ctx.mmu.write32(c + 0x88, ctx.ctr);
  ctx.mmu.write32(c + 0x8C, ctx.xer);
  for (int i = 0; i < 32; i++) {
    uint64_t d;
    std::memcpy(&d, &ctx.fpr[i], 8);
    ctx.mmu.write64(c + 0x90 + i * 8, d);
  }
  ctx.mmu.write32(c + 0x194, ctx.fpscr);
  ctx.mmu.write32(c + 0x198, ctx.pc);  
  ctx.mmu.write32(c + 0x19C, ctx.msr); 
  for (int i = 0; i < 8; i++)
    ctx.mmu.write32(c + 0x1A4 + i * 4, ctx.gqr[i]);
  for (int i = 0; i < 32; i++) {
    uint64_t d;
    std::memcpy(&d, &ctx.ps1[i], 8);
    ctx.mmu.write64(c + 0x1C8 + i * 8, d);
  }
}


static bool hle_load_context_from_guest(CPUContext &ctx) {
  uint32_t c = ctx.mmu.read32(0x800000D4);
  if (c == 0)
    return false;
  for (int i = 0; i < 32; i++)
    ctx.gpr[i] = ctx.mmu.read32(c + i * 4);
  uint32_t cr_val = ctx.mmu.read32(c + 0x80);
  for (int i = 0; i < 8; i++) {
    uint32_t nib = (cr_val >> (28 - i * 4)) & 0xF;
    ctx.cr[i].lt = (nib & 8) != 0;
    ctx.cr[i].gt = (nib & 4) != 0;
    ctx.cr[i].eq = (nib & 2) != 0;
    ctx.cr[i].so = (nib & 1) != 0;
  }
  ctx.lr = ctx.mmu.read32(c + 0x84);
  ctx.ctr = ctx.mmu.read32(c + 0x88);
  ctx.xer = ctx.mmu.read32(c + 0x8C);
  for (int i = 0; i < 32; i++) {
    uint64_t d = ctx.mmu.read64(c + 0x90 + i * 8);
    std::memcpy(&ctx.fpr[i], &d, 8);
  }
  ctx.fpscr = ctx.mmu.read32(c + 0x194);
  for (int i = 0; i < 8; i++)
    ctx.gqr[i] = ctx.mmu.read32(c + 0x1A4 + i * 4);
  for (int i = 0; i < 32; i++) {
    uint64_t d = ctx.mmu.read64(c + 0x1C8 + i * 8);
    std::memcpy(&ctx.ps1[i], &d, 8);
  }
  ctx.pc = ctx.mmu.read32(c + 0x198);  
  ctx.msr = ctx.mmu.read32(c + 0x19C); 
  poll_ee_kick(ctx);  // [r33-g7]

  

  
  ctx.mmu.write16(c + 0x1A2, ctx.mmu.read16(c + 0x1A2) & ~0x0002);
  return true;
}

int g_trace_after_dec = 0;

// R6 perf: process_pending_callbacks runs at every backward branch of the
// recompiled code (~6% of all samples under a Switch-like CPU profile, mostly
// the prologue of this large function). It is split in three:
//   process_pending_callbacks  small fast path for the common case (no debug
//                              switch, no pending VI/IPC/callback work)
//   ppc_full                   the original body up to the event scheduler
//   ppc_tail                   the original body from the decrementer check on
// The fast path performs exactly the side effects the full body would (DI
// tick, timebase read + scheduler advance, decrementer check) and enters
// ppc_tail at the same point, so behaviour is unchanged.
bool g_vtl_on_ios = false;  // r37v: NWII_V_TL (vtl_init, gx.cpp)
// r37v (NWII_V_WAKE): cuenta de entregas al invitado (interrupcion externa, DEC, callback); idle_wait
// no duerme en la vuelta en la que ha habido una (ver os_hle_stubs.cpp).
std::atomic<uint32_t> g_v_dispatch_seq{0};
// r37v (frente V), NWII_V_LATE=<ms> (1..12): retrazo VI elastico. Si el retrazo llega con el hilo del juego
// ocupado (no ha pasado por el bucle ocioso en los ultimos 300 us: aun no ha terminado su fotograma), la
// interrupcion VI se retiene hasta que el juego entra en el bucle ocioso (idle_wait: ya espera al retrazo) o
// hasta <ms> despues, lo que llegue antes; los retrazos siguientes siguen en su sitio (60 por segundo, el
// reloj invitado y el audio no cambian). Un fotograma que acaba un poco tarde ya no espera al retrazo
// siguiente entero (16,7 ms): el juego encadena y recupera en los fotogramas siguientes.
extern CPUContext *g_ctx_ptr;  // main.cpp
int64_t g_v_idle_ns = 0;     // ultima entrada/salida de idle_wait del hilo del juego (ns, steady_clock)
bool g_v_vi_held = false;    // hay un retrazo retenido
static int64_t s_v_held_ns = 0;
uint64_t g_v_held_n = 0, g_v_held_idle_n = 0, g_v_held_to_n = 0, g_v_held_drop_n = 0, g_v_held_us = 0;
static inline int64_t v_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
static uint64_t v_late_ticks(const CPUContext &c) {
  static const uint64_t t = [&c] {
    const char *e = std::getenv("NWII_V_LATE");
    if (!(e && e[0] && e[0] != '0')) return (uint64_t)0;
    double ms = std::atof(e);
    if (ms < 0.5) ms = 0.5;
    if (ms > 12) ms = 12;
    std::printf("[V] retrazo elastico: NWII_V_LATE=%.1f ms\n", ms);
    return (uint64_t)(c.tb_freq * ms / 1000.0);
  }();
  return t;
}
static inline bool v_vi_ok(CPUContext &c) {
  return (nwii::runtime::hw::pi_intmr & 0x100) && !c.in_callback && !(nwii::runtime::hw::pi_intsr & 0x100) &&
         (c.dispatch_saved_ctx == 0 || c.mmu.read32(0x800000E4) == 0);
}
// why: 1 = el juego entra en el bucle ocioso, 2 = se acabo el plazo
void v_deliver_held(CPUContext &c, int why) {
  if (!g_v_vi_held) return;
  g_v_vi_held = false;
  const int64_t us = (v_now_ns() - s_v_held_ns) / 1000;
  g_v_held_us += (uint64_t)us;
  (why == 1 ? g_v_held_idle_n : g_v_held_to_n)++;
  if (v_vi_ok(c)) {
    nwii::runtime::hw::vi_trigger_interrupt();
  } else {
    ++g_v_held_drop_n;  // como sin retencion: un retrazo que no se puede lanzar se pierde
  }
  if (__builtin_expect(g_vtl_on_ios, 0)) ::vtl_note(15, (uint32_t)us | (uint32_t)why << 28);
}

static bool g_vi_registered = false;
// r21s: core/force_demo.cpp (aquí ya estamos dentro de nwii::runtime)
extern bool g_force_demo_on;
void force_demo_init();
void force_demo_tick(CPUContext &c);
static bool g_hb_due = false;  // R9 guestloop: ppc_full prints the [Heartbeat] block
namespace hw { extern int g_di_interrupt_delay; }
NWII_NO_TSAN static bool ppc_tail(CPUContext &ctx, bool dec_exp);

NWII_NO_TSAN static __attribute__((noinline)) bool ppc_full(CPUContext &ctx) {

  
  if (g_env_sample) {
      static uint32_t last_ee = 0, edges = 0;
      uint32_t ee = ctx.msr & 0x8000;
      if (ee && !last_ee && edges < 8)
          std::cout << "[EEup] #" << ++edges << " pc=0x" << std::hex << ctx.pc
                    << " lr=0x" << ctx.lr << std::dec << "\n";
      last_ee = ee;
  }

  
  if (g_trace_after_dec > 0) {
      static int skip = 20000; 
      if (skip > 0) { skip--; }
      else {
          g_trace_after_dec--;
          std::cout << "[T] 0x" << std::hex << ctx.pc << " lr=0x" << ctx.lr
                    << " r1=0x" << ctx.gpr[1] << " r3=0x" << ctx.gpr[3]
                    << " r29=0x" << ctx.gpr[29] << " sr0=0x" << ctx.srr0
                    << std::dec << "\n";
      }
  }

  {
    static uint32_t trace_pc = []() -> uint32_t {
      const char *e = NWII_ENV("NWII_LOOPTRACE");
      return e ? (uint32_t)std::strtoul(e, nullptr, 16) : 0;
    }();
    static uint64_t hits = 0;
    if (trace_pc && ctx.pc == trace_pc) {
      if ((hits++ % 200000) == 0)
        std::cout << "[Loop 0x" << std::hex << trace_pc << "] #" << std::dec
                  << hits << " r3=0x" << std::hex << ctx.gpr[3] << " r4=0x"
                  << ctx.gpr[4] << " r5=0x" << ctx.gpr[5] << " r30=0x"
                  << ctx.gpr[30] << " r31=0x" << ctx.gpr[31] << " lr=0x"
                  << ctx.lr << std::dec << "\n";
    }
  }

  {
    static bool sample = g_env_sample;
    static uint64_t n = 0;
    if (sample && (++n % 400000) == 0)
      std::cout << "[Sample] pc=0x" << std::hex << ctx.pc << " lr=0x" << ctx.lr
                << " r3=0x" << ctx.gpr[3] << std::dec << "\n";
  }

  

  
  if (ctx.pc == 0xFFFFFFFC && !ctx.in_callback) {
    hle_unlock_scheduler_after_dispatch(ctx);

    

    
    
    static uint32_t g_resched_addr = [&ctx]() -> uint32_t {
      const char *e = NWII_ENV("NWII_RESCHED");
      return e ? (uint32_t)std::strtoul(e, nullptr, 16)
               : hle_find_os_reschedule(ctx);
    }();
    if (ctx.ext_resched_pending && g_resched_addr) {
      ctx.ext_resched_pending = false;
      // SelectThread only switches when the current context is the current
      // thread's own; put the interrupted context back before rescheduling.
      if (ctx.dispatch_saved_ctx)
        hle_set_current_context(ctx, ctx.dispatch_saved_ctx);
      if (g_env_sample) {
        static int n = 0;
        if (n++ < 12) {
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          std::cout << "[Resched] cur=0x" << std::hex << cur
                    << " prio=" << std::dec << ctx.mmu.read32(cur + 0x2D0)
                    << " r1=0x" << std::hex << ctx.gpr[1] << std::dec << "\n";
        }
      }
      ctx.gpr[3] = 0;
      ctx.lr = 0xFFFFFFFC;
      ctx.pc = g_resched_addr;
      return true;
    }
    ctx.ext_resched_pending = false;
    if (ctx.dispatch_saved_ctx) {
      uint32_t current_thread = ctx.mmu.read32(0x800000E4);
      if (current_thread != 0 && current_thread != ctx.dispatch_saved_ctx) {
        hle_set_current_context(ctx, current_thread);
      } else {
        hle_set_current_context(ctx, ctx.dispatch_saved_ctx);
      }
      ctx.dispatch_saved_ctx = 0;
    }
    if (hle_load_context_from_guest(ctx)) {
      if (__builtin_expect(s2::g_enabled, 0)) s2::game_sync(ctx);  // r34s2: EE / hilo pueden haber cambiado

      

      
      ctx.dec_written_tb = ctx.read_timebase();
      static uint32_t restore_count = 0;
      restore_count++;
      if (ctx.pc == 0 || (restore_count % 64) == 1) {
          std::cout << "[HLE PI] Context restore #" << std::dec << restore_count
                    << " pc=0x" << std::hex << ctx.pc << " r1=0x" << ctx.gpr[1]
                    << " msr=0x" << ctx.msr << std::dec << "\n";
      }
      return true;
    }
  }

  if (g_hb_due) {
      std::cout << "[Heartbeat] PC: 0x" << std::hex << ctx.pc << " LR: 0x" << ctx.lr
                << " intmr=0x" << nwii::runtime::hw::pi_intmr.load()
                << " intsr=0x" << nwii::runtime::hw::pi_intsr.load()
                << " msr=0x" << ctx.msr
                << " dec_h=0x" << ctx.mmu.read32(0x80003020) << "\n";
      {
          std::cout << "  [ExcTable]";
          for (int i = 0; i < 16; i++) {
              uint32_t h = ctx.mmu.read32(0x80003000 + i * 4);
              if (h) std::cout << " " << i << ":0x" << std::hex << h << std::dec;
          }
          std::cout << "\n";
      }

      
      
      auto dump_thread = [&](uint32_t th, const char *tag) {
          uint16_t state = ctx.mmu.read16(th + 0x2C8);
          uint32_t prio = ctx.mmu.read32(th + 0x2D0);
          uint32_t wqueue = ctx.mmu.read32(th + 0x2DC);
          uint32_t srr0 = ctx.mmu.read32(th + 0x198);
          uint32_t slr = ctx.mmu.read32(th + 0x84);
          std::cout << "  [" << tag << "] 0x" << std::hex << th << " state=" << state
                    << " prio=" << std::dec << prio << " waitq=0x" << std::hex
                    << wqueue << " srr0=0x" << srr0 << " lr=0x" << slr;
          
          uint32_t sp = ctx.mmu.read32(th + 4);
          for (int f = 0; f < 6 && sp != 0 && sp < 0x94000000; ++f) {
              uint32_t ret = ctx.mmu.read32(sp + 4);
              if (ret) std::cout << " <-0x" << std::hex << ret;
              sp = ctx.mmu.read32(sp);
          }
          std::cout << std::dec << "\n";
      };
      
      {
          static uint32_t peek_addr = 0, peek_words = 8;
          static bool peek_parsed = false;
          if (!peek_parsed) {
              peek_parsed = true;
              if (const char *env = NWII_ENV("NWII_PEEK")) {
                  peek_addr = (uint32_t)std::strtoul(env, nullptr, 16);
                  if (const char *c = std::strchr(env, ','))
                      peek_words = (uint32_t)std::strtoul(c + 1, nullptr, 10);
              }
          }
          if (peek_addr) {
              std::cout << "  [Peek 0x" << std::hex << peek_addr << "]";
              for (uint32_t i = 0; i < peek_words; i++)
                  std::cout << " " << ctx.mmu.read32(peek_addr + i * 4);
              std::cout << std::dec << "\n";
          }
      }
      uint32_t cur = ctx.mmu.read32(0x800000E4);
      if (cur) dump_thread(cur, "CurThread");
      uint32_t th = ctx.mmu.read32(0x800000DC);
      int guard = 0;
      while (th != 0 && guard++ < 16) {
          if (th != cur) dump_thread(th, "Thread");
          th = ctx.mmu.read32(th + 0x2FC);
      }
  }

  
  
  if (ctx.pc == 0) {
      ctx.note_idle();  // no runnable thread (overload governor, cpu_context.h)
      static uint64_t idle_hits = 0;
      if (++idle_hits == 200000) {
          std::cout << "[IDLE] no runnable thread. intmr=0x" << std::hex
                    << nwii::runtime::hw::pi_intmr << " intsr=0x"
                    << nwii::runtime::hw::pi_intsr << " dec_armed="
                    << std::dec << ctx.dec_irq_pending << "\n";
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          uint32_t th = ctx.mmu.read32(0x800000DC);
          int guard = 0;
          while (th != 0 && guard++ < 24) {
              uint16_t state = ctx.mmu.read16(th + 0x2C8);
              uint32_t prio = ctx.mmu.read32(th + 0x2D0);
              uint32_t wq = ctx.mmu.read32(th + 0x2DC);
              uint32_t srr0 = ctx.mmu.read32(th + 0x198);
              std::cout << "  [IdleThread] 0x" << std::hex << th
                        << (th == cur ? "*" : " ") << " state=" << std::dec
                        << state << " prio=" << prio << " waitq=0x" << std::hex
                        << wq << " srr0=0x" << srr0 << std::dec << "\n";
              th = ctx.mmu.read32(th + 0x2FC);
          }
      }
  }

  if (g_ipc_interrupt_delay > 0) {
      g_ipc_interrupt_delay--;
      if (g_ipc_interrupt_delay == 0) {
          nwii::runtime::hw::trigger_pi_interrupt(0x00004000);
          if (g_env_sample)
              std::cout << "[IPC] irq raised intmr=0x" << std::hex
                        << nwii::runtime::hw::pi_intmr << " msr=0x" << ctx.msr
                        << " pc=0x" << ctx.pc << std::dec << "\n";
      }
  }

  
  nwii::runtime::hw::di_tick();

  if (ctx.vblank_pending) {
      if (!ctx.in_callback && (ctx.msr & 0x8000)) {
          ctx.vblank_pending = false;
          nwii::runtime::hw::vi_trigger_interrupt();

          
          
          hle_drive_thread_queue(ctx);
      }
  }

  if (ctx.in_callback && ctx.pc == 0xFFFFFFFC) {
    
    ctx.in_callback = false;
  }

  if (ctx.in_callback) {
    if (g_env_sample && (nwii::runtime::hw::pi_intsr & 0x4000)) {
        static uint64_t n = 0;
        if ((n++ % 100000) == 0)
            std::cout << "[IPC] irq pending but in_callback pc=0x" << std::hex
                      << ctx.pc << " lr=0x" << ctx.lr << " depth="
                      << std::dec << ctx.callback_depth << "\n";
    }
    return false;
  }

  // Periodically raise VI (PI_INTSR bit 8 = 0x100, Dolphin INT_CAUSE_VI)

  

  

  

  

  
  
  {
      if (!g_vi_registered) {
          g_vi_registered = true;
          nwii::runtime::force_demo_init();  // r21s: NWII_FORCE_DEMO (solo si está puesto)

          
          uint64_t vi_period = ctx.tb_freq / 60;
          if (vi_period == 0) vi_period = 500000;
          nwii::runtime::EventScheduler::get().schedule_recurring(
              vi_period, [](CPUContext& c, uint64_t) {
                  if (__builtin_expect(nwii::runtime::g_force_demo_on, 0)) nwii::runtime::force_demo_tick(c);

                  
                  
                  const bool vi_ok = (nwii::runtime::hw::pi_intmr & 0x100) && !c.in_callback &&
                      !(nwii::runtime::hw::pi_intsr & 0x100) &&
                      (c.dispatch_saved_ctx == 0 || c.mmu.read32(0x800000E4) == 0);
                  if (vi_ok) {
                      // r37v NWII_V_LATE: retener si el juego aun no ha acabado su fotograma
                      const uint64_t late = v_late_ticks(c);
                      if (late && &c == nwii::runtime::g_ctx_ptr && !g_v_vi_held &&
                          v_now_ns() - g_v_idle_ns > 300000) {
                          g_v_vi_held = true;
                          s_v_held_ns = v_now_ns();
                          ++g_v_held_n;
                          nwii::runtime::EventScheduler::get().schedule_after(
                              late, [](CPUContext &c2, uint64_t) { v_deliver_held(c2, 2); });
                      } else {
                          nwii::runtime::hw::vi_trigger_interrupt();
                      }
                  }
                  {
                      extern bool g_vtl_on_ios;  // r37v: NWII_V_TL (ios.cpp)
                      if (__builtin_expect(g_vtl_on_ios, 0)) {
                          ::vtl_note(1, (vi_ok ? 1u : 0u) | ((nwii::runtime::hw::pi_intmr & 0x100) ? 0u : 2u) |
                                          (c.in_callback ? 4u : 0u) |
                                          ((nwii::runtime::hw::pi_intsr & 0x100) ? 8u : 0u) |
                                          (c.dispatch_saved_ctx != 0 ? 16u : 0u) |
                                          (&c == nwii::runtime::s2::snd_ctx() ? 32u : 0u) |
                                          (g_v_vi_held ? 64u : 0u));
                      }
                  }
              });
      }
      nwii::runtime::EventScheduler::get().advance(ctx, ctx.read_timebase());
  }

  return ppc_tail(ctx, ctx.dec_expired());
}

NWII_NO_TSAN static bool ppc_tail(CPUContext &ctx, bool dec_exp) {
  if (dec_exp && (ctx.msr & 0x8000) && !ctx.in_callback) {
      ctx.dec_irq_pending = false; 
      if (g_env_sample) {
          static uint32_t df = 0;
          if (df++ < 12)
              std::cout << "[DECfire] #" << df << " pc=0x" << std::hex << ctx.pc
                        << " lr=0x" << ctx.lr << " inst=" << std::dec
                        << ctx.inst_count << "\n";
          extern int g_trace_after_dec;
          if (df == 1) g_trace_after_dec = 300;
      }

      

      
      // [r35m4] quitado del todo el parche viejo "DEC 64 veces en el mismo pc -> forzar alarmas por debajo de
      // r13": escribia la hora en datos del juego que parecian un OSAlarm (r9/r34s2/LEEME.md §9). Nunca saltaba
      // sin S2 en los registros revisados; con S2 corrompia las texturas de luz.

      
      static uint32_t dcount = 0;
      if ((dcount++ % 500) == 0) {
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          uint32_t th = ctx.mmu.read32(0x800000DC);
          std::cout << "[Sched] cur=0x" << std::hex << cur << std::dec << "\n";
          int guard = 0;
          while (th != 0 && guard++ < 24) {
              uint16_t state = ctx.mmu.read16(th + 0x2C8);
              uint32_t prio = ctx.mmu.read32(th + 0x2D0);
              uint32_t wq = ctx.mmu.read32(th + 0x2DC);
              uint32_t srr0 = ctx.mmu.read32(th + 0x198);
              std::cout << "  th=0x" << std::hex << th << (th==cur?"*":" ")
                        << " state=" << std::dec << state << " prio=" << prio
                        << " waitq=0x" << std::hex << wq << " srr0=0x" << srr0
                        << std::dec << "\n";

              

              if (wq >= 0x80000000u && wq < 0x81800000u &&
                  g_env_sample) {
                  std::cout << "    wq[-2..5]:" << std::hex;
                  for (int i = -2; i < 6; ++i)
                      std::cout << " " << ctx.mmu.read32(wq + i * 4);
                  std::cout << std::dec << "\n";
              }
              th = ctx.mmu.read32(th + 0x2FC);
          }
      }
      uint32_t dec_handler = ctx.mmu.read32(0x80003000 + 8 * 4);
      uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
      if (dec_handler != 0 && dec_handler != 0xFFFFFFFF && current_ctx != 0) {
          hle_save_context_to_guest(ctx, current_ctx);
          ctx.dispatch_saved_ctx = current_ctx;
          ctx.srr0 = ctx.pc;
          ctx.srr1 = ctx.msr;
          ctx.msr &= ~0x8000;
          ctx.gpr[3] = 8;           
          ctx.gpr[4] = current_ctx; 

          
          
          ctx.lr = 0xFFFFFFFC;
          ctx.pc = dec_handler;
          { g_v_dispatch_seq.fetch_add(1, std::memory_order_relaxed); if (__builtin_expect(s2::g_enabled, 0)) s2::game_irq_enter(ctx); NWII_LONGJMP(ctx.exception_jmp_buf); }
      }
  }

  uint32_t active_ints = nwii::runtime::hw::pi_intsr & nwii::runtime::hw::pi_intmr;

  
  if (active_ints != 0 && (ctx.msr & 0x8000)) {

      

      
      
      bool sched_disabled = false;
      uint32_t r13 = ctx.gpr[13];
      uint32_t lock_off = nwii::runtime::Config::get().game_profile.sched_lock_offset;
      if (lock_off != 0 && r13 >= 0x80200000u && r13 < 0x81800000u) {
          uint32_t lock_addr = r13 - lock_off;
          if (lock_addr >= 0x80000000u && lock_addr < 0x82000000u) {
              uint32_t lock_val = ctx.mmu.read32(lock_addr);

              sched_disabled = (lock_val > 0 && lock_val < 256);
          }
      }
      bool handler_in_flight = (ctx.dispatch_saved_ctx != 0);

      

      
      bool use_new_path =
          (nwii::runtime::Config::get().game_profile.ext_interrupt_dispatch != 0);

      

      
      bool idle = (ctx.mmu.read32(0x800000E4) == 0);

      

      

      if (use_new_path && (handler_in_flight || (!idle && sched_disabled))) {
          
          if (g_env_sample) {
              static uint64_t defer_count = 0;
              if ((++defer_count % 500000) == 0)
                  std::cout << "[HLE PI] int deferred: sched_disabled="
                            << sched_disabled << " in_flight=" << handler_in_flight
                            << " pc=0x" << std::hex << ctx.pc << std::dec << "\n";
          }
      } else {

          
          static uint32_t s_dispatch_addr = 0xFFFFFFFF; 
          if (s_dispatch_addr == 0xFFFFFFFF) {
              
              uint32_t from_cfg = nwii::runtime::Config::get().game_profile.ext_interrupt_dispatch;
              if (from_cfg != 0) {
                  s_dispatch_addr = from_cfg;
                  std::cout << "[HLE PI] __OSDispatchInterrupt from config: 0x"
                            << std::hex << s_dispatch_addr << std::dec << "\n";
              } else {

                  
                  s_dispatch_addr = 0;
              }
          }

          if (s_dispatch_addr != 0) {

              

              

              

              

              ctx.srr0 = ctx.pc;
              ctx.srr1 = ctx.msr;

              
              
              uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
              if (current_ctx == 0) {
                  current_ctx = ctx.mmu.read32(0x800000D0);
              }
              if (current_ctx == 0) {
                  std::cout << "[HLE PI] FATAL: current_ctx is 0! pc=0x" << std::hex << ctx.pc << " intmr=0x" << nwii::runtime::hw::pi_intmr << " intsr=0x" << nwii::runtime::hw::pi_intsr << std::dec << "\n";
              }

              
              
              if (current_ctx != 0) {
                  hle_save_context_to_guest(ctx, current_ctx);
              }

              
              ctx.gpr[3] = 0x04;
              
              ctx.gpr[4] = current_ctx;

              
              ctx.msr &= ~0x8000u; 

              
              
              ctx.lr = 0xFFFFFFFC;
              ctx.pc = s_dispatch_addr;

              if (g_env_sample) {
                  static uint32_t dc = 0;
                  if ((dc++ % 1000) == 0)
                      std::cout << "[HLE PI] -> __OSDispatchInterrupt #" << std::dec
                                << dc << " pc=0x" << std::hex << ctx.pc
                                << " cur=0x" << ctx.mmu.read32(0x800000E4)
                                << std::dec << "\n";
              }

              { g_v_dispatch_seq.fetch_add(1, std::memory_order_relaxed); if (__builtin_expect(s2::g_enabled, 0)) s2::game_irq_enter(ctx); NWII_LONGJMP(ctx.exception_jmp_buf); }

          } else {

              
              int os_intr = -1;
              uint32_t bit_to_clear = 0;
              
              if      (active_ints & 0x00004000) { os_intr = 27; bit_to_clear = 0x00004000; } 
              else if (active_ints & 0x00000100) { os_intr = 24; bit_to_clear = 0x00000100; } 
              else if (active_ints & 0x00000004) { os_intr = 21; bit_to_clear = 0x00000004; } 
              else if (active_ints & 0x00000008) { os_intr = 20; bit_to_clear = 0x00000008; } 
              else if (active_ints & 0x00000010) { os_intr =  9; bit_to_clear = 0x00000010; } 

              else if (active_ints & 0x00000040) { os_intr = nwii::runtime::hw::dsp_pending_os_interrupt(); bit_to_clear = 0x00000040; } 
              else if (active_ints & 0x00000020) { os_intr =  8; bit_to_clear = 0x00000020; } 
              else if (active_ints & 0x00000400) { os_intr = 19; bit_to_clear = 0x00000400; } 
              else if (active_ints & 0x00000200) { os_intr = 18; bit_to_clear = 0x00000200; } 
              else if (active_ints & 0x00000800) { os_intr = 17; bit_to_clear = 0x00000800; } 

              if (os_intr != -1) {
                  uint32_t handler = ctx.mmu.read32(0x80003040 + os_intr * 4);
                  if (__builtin_expect(g_vtl_on_ios, 0)) {  // r37v
                      ::vtl_note(2, (uint32_t)os_intr | (&ctx == nwii::runtime::s2::snd_ctx() ? 0x100u : 0u));
                  }

                  if (handler != 0 && handler != 0xFFFFFFFF) {
                      if (g_env_sample && os_intr == 24) {
                          static int vn = 0;
                          if (vn++ < 5)
                              std::cout << "[VIdisp] #" << vn << " handler=0x"
                                        << std::hex << handler << " pc=0x"
                                        << ctx.pc << std::dec << "\n";
                      }
                      
                      static const bool ipc_log = NWII_ENV("NWII_IPCLOG") != nullptr;
                      if (ipc_log && os_intr == 27)
                          std::cout << "[IPCLOG] irq27 ppcctrl=0x" << std::hex
                                    << ctx.mmu.read32(0xCD000004) << " armmsg=0x"
                                    << ctx.mmu.read32(0xCD000008) << " pc=0x"
                                    << ctx.pc << std::dec << "\n";
                      static uint32_t vi_dispatch_count = 0;
                      if (os_intr != 24 && (vi_dispatch_count++ % 256) == 0) {
                          std::cout << "[HLE PI] Dispatching interrupt "
                                    << std::dec << os_intr << " to handler 0x"
                                    << std::hex << handler << " r1=0x"
                                    << ctx.gpr[1] << " pc=0x" << ctx.pc
                                    << std::dec << std::endl;
                      }
                      if (g_env_sample) {
                          static uint64_t census[32] = {0};
                          static uint64_t total = 0;
                          if (os_intr >= 0 && os_intr < 32) census[os_intr]++;
                          if ((++total % 2000) == 0) {
                              std::cout << "[IntCensus]";
                              for (int i = 0; i < 32; ++i)
                                  if (census[i])
                                      std::cout << " " << std::dec << i
                                                << ":" << census[i];
                              std::cout << "\n";
                          }
                      }
                      uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
                      if (current_ctx == 0) {
                          
                          if (g_env_sample) {
                              static int n = 0;
                              if (n++ < 4)
                                  std::cout << "[HLE PI] int " << std::dec
                                            << os_intr << " deferred: no current context\n";
                          }
                      } else {
                          
                          nwii::runtime::hw::pi_intsr &= ~bit_to_clear;
                          hle_save_context_to_guest(ctx, current_ctx);
                          ctx.dispatch_saved_ctx = current_ctx;
                          ctx.srr0 = ctx.pc;
                          ctx.srr1 = ctx.msr;
                          ctx.msr &= ~0x8000u; 
                          ctx.gpr[3] = os_intr;
                          ctx.gpr[4] = current_ctx;
                          ctx.lr = 0xFFFFFFFC;
                          ctx.pc = handler;
                          ctx.ext_resched_pending = true;
                          hle_lock_scheduler_for_dispatch(ctx);
                          { g_v_dispatch_seq.fetch_add(1, std::memory_order_relaxed); if (__builtin_expect(s2::g_enabled, 0)) s2::game_irq_enter(ctx); NWII_LONGJMP(ctx.exception_jmp_buf); }
                      }

                      ctx.msr &= ~0x8000u;
                      CPUContext::BackupState bk;
                      bk.gpr = ctx.gpr;
                      bk.fpr = ctx.fpr;
                      bk.ps1 = ctx.ps1;
                      bk.cr  = ctx.cr;
                      bk.lr  = ctx.lr;
                      bk.ctr = ctx.ctr;
                      bk.xer = ctx.xer;
                      bk.pc  = ctx.pc;
                      bk.srr0 = ctx.srr0;
                      bk.srr1 = ctx.srr1;
                      bk.msr = ctx.msr | 0x8000;
                      bk.fpscr = ctx.fpscr;
                      bk.gqr = ctx.gqr;
                      bk.sprg = ctx.sprg;
                      ctx.backup_stack.push(bk);

                      ctx.in_callback = true;
                      ctx.poll_kick();  // R9: ppc_full on every poll in a callback
                      ctx.callback_depth++;
                      ctx.srr0 = ctx.pc;
                      ctx.srr1 = ctx.msr | 0x8000;
                      ctx.gpr[3] = os_intr;
                      ctx.gpr[4] = 0;
                      ctx.lr = 0xFFFFFFFC;
                      ctx.pc = handler;
                      { g_v_dispatch_seq.fetch_add(1, std::memory_order_relaxed); if (__builtin_expect(s2::g_enabled, 0)) s2::game_irq_enter(ctx); NWII_LONGJMP(ctx.exception_jmp_buf); }
                  } else {
                      
                      static uint32_t last_null_pc = 0;
                      static int null_warn_count = 0;
                      if (ctx.pc != last_null_pc) {
                          null_warn_count = 0;
                          last_null_pc = ctx.pc;
                      }
                      if (null_warn_count++ < 3)
                          std::cout << "[HLE PI] Handler for interrupt "
                                    << std::dec << os_intr
                                    << " is NULL (OS reinit in progress, deferring)."
                                    << std::endl;
                  }
              }
          } 
      } 
  } else if (nwii::runtime::hw::pi_intsr != 0) {
      
  }

  if ((ctx.msr & 0x8000) == 0)
    return false;

  if (ctx.pending_cb_count.load(std::memory_order_relaxed) == 0)
    return false;

  CallbackInfo cb;
  {
    std::lock_guard<std::mutex> lock(ctx.cb_mutex);
    if (ctx.pending_callbacks.empty())
      return false;
    cb = ctx.pending_callbacks.front();
    ctx.pending_callbacks.pop();
    ctx.pending_cb_count.fetch_sub(1, std::memory_order_relaxed);
  }

  if (!s2::g_enabled)
  std::cout << "[HLE Callback] Dispatching cb=0x" << std::hex << cb.cb_addr
            << " arg1=0x" << cb.arg1 << " arg2=0x" << cb.arg2 << " from PC=0x"
            << ctx.pc << " LR=0x" << ctx.lr << " r1=0x" << ctx.gpr[1]
            << std::dec << std::endl;

  uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
  if (current_ctx != 0) {

    hle_save_context_to_guest(ctx, current_ctx);
    ctx.dispatch_saved_ctx = current_ctx;

    ctx.srr0 = ctx.pc;
    ctx.srr1 = ctx.msr;
    ctx.msr &= ~0x8000; 

    // Run the callback the way the SDK's IPC reply handler does: on a cleared
    // temporary OSContext made current, so a thread woken from inside it
    // (OSWakeupThread -> SelectThread) cannot switch threads mid-callback.
    // The switch happens afterwards through __OSReschedule, once the
    // interrupted context is current again (see the 0xFFFFFFFC return path).
    uint32_t tmp_ctx = (ctx.gpr[1] - OS_CONTEXT_FRAME) & ~0x1Fu;
    hle_set_current_context(ctx, hle_clear_context(ctx, tmp_ctx));
    ctx.gpr[1] = tmp_ctx - 0x10;
    ctx.mmu.write32(ctx.gpr[1], 0);
    ctx.ext_resched_pending = true;
    hle_lock_scheduler_for_dispatch(ctx);

    ctx.gpr[3] = cb.arg1;
    ctx.gpr[4] = cb.arg2;

    ctx.lr = 0xFFFFFFFC;
    ctx.pc = cb.cb_addr;
    { g_v_dispatch_seq.fetch_add(1, std::memory_order_relaxed); if (__builtin_expect(s2::g_enabled, 0)) s2::game_irq_enter(ctx); NWII_LONGJMP(ctx.exception_jmp_buf); }
  }

  CPUContext::BackupState bk;
  bk.gpr = ctx.gpr;
  bk.fpr = ctx.fpr;
  bk.ps1 = ctx.ps1;
  bk.cr  = ctx.cr;
  bk.lr  = ctx.lr;
  bk.ctr = ctx.ctr;
  bk.xer = ctx.xer;
  bk.pc  = ctx.pc;
  bk.srr0 = ctx.srr0;
  bk.srr1 = ctx.srr1;
  bk.msr = ctx.msr;
  bk.fpscr = ctx.fpscr;
  bk.gqr = ctx.gqr;
  bk.sprg = ctx.sprg;
  ctx.backup_stack.push(bk);

  ctx.in_callback = true;
  ctx.poll_kick();  // R9: ppc_full on every poll in a callback
  ctx.callback_depth++;

  ctx.gpr[3] = cb.arg1;
  ctx.gpr[4] = cb.arg2;

  uint32_t cb_stack_top = get_callback_stack_top() - (ctx.callback_depth - 1) * 4096;
  ctx.gpr[1] = cb_stack_top - 16;
  ctx.mmu.write32(ctx.gpr[1], 0);
  ctx.mmu.write32(ctx.gpr[1] + 4, 0xFFFFFFFC);

  ctx.lr = 0xFFFFFFFC;
  ctx.pc = cb.cb_addr;
  { g_v_dispatch_seq.fetch_add(1, std::memory_order_relaxed); if (__builtin_expect(s2::g_enabled, 0)) s2::game_irq_enter(ctx); NWII_LONGJMP(ctx.exception_jmp_buf); }
}

// ---- R9 guestloop: the per-back-edge poll ---------------------------------
// The inline part (cpu_context.h, process_pending_callbacks) decides most
// polls with a countdown and, with MSR[EE] set, three flag tests. This slow
// part takes the same decisions as the r8 poll did:
//   - the rare conditions (debug switches, VI not registered yet, IPC/DI
//     interrupt delays counted in polls, virtual-clock VI, the legacy callback
//     path, the dispatcher's 0xFFFFFFFC / 0 cases, the heartbeat) go to
//     ppc_full on every poll while they last (the countdown is kicked);
//   - otherwise: refresh the guest clock from the host clock when the
//     countdown ran out (every kPollTbEvery polls, as often as r8 did),
//     move the scheduler clock, fire due events, deliver a decrementer /
//     external interrupt / queued callback when MSR[EE] allows it.
// The scheduler's next due time is cached (one compare) and
// EventScheduler::get() has no initialization guard; PI_INTSR/PI_INTMR are
// read with relaxed loads (seq_cst loads are acquire loads on AArch64).
// NWII_POLL_STATS=1 (PC: environment, Switch: debug.toml) counts slow polls,
// clock refreshes and ppc_full / ppc_tail entries for the fps_log lines.
static bool g_poll_force = true;   // ppc_full on every poll (until it says otherwise)
static bool g_tb_wall = false;     // CPUContext::wall_clock_tb(), read by ppc_full
static bool g_poll_stats = false;
static bool g_poll_check = false;  // NWII_POLL_CHECK (below)
static uint64_t g_hb_next = 1ull << 30;
std::atomic<uint64_t> g_poll_slow_n{0}, g_poll_full_n{0}, g_poll_tail_n{0}, g_poll_tb_n{0};

static void poll_update_force() {
  static const bool s_debug = g_env_sample || NWII_ENV("NWII_LOOPTRACE") != nullptr ||
                              NWII_ENV("NWII_TB_NOCACHE") != nullptr;
  static const bool s_stats = NWII_ENV("NWII_POLL_STATS") != nullptr;
  static const bool s_check = NWII_ENV("NWII_POLL_CHECK") != nullptr;
  g_tb_wall = CPUContext::wall_clock_tb();
  g_poll_stats = s_stats;
  g_poll_check = s_check;
  g_poll_force = s_debug || g_trace_after_dec > 0 || !g_vi_registered;
}

static inline void poll_count(std::atomic<uint64_t> &c) {
  if (__builtin_expect(g_poll_stats, 0))
    c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

extern CPUContext *g_ctx_ptr;  // main.cpp
void poll_kick_global() {
  if (CPUContext *c = g_ctx_ptr) c->poll_kick();
}

NWII_NO_TSAN static NWII_NOINLINE bool ppc_full_entry(CPUContext &ctx) {
  poll_count(g_poll_full_n);
  // Every poll takes this path while the condition lasts; ppc_full may not
  // return (it longjmps into an interrupt handler).
  ctx.poll_kick();
  if (ctx.inst_count >= g_hb_next) {
    // [Heartbeat] (r8: when a poll saw inst_count % 20000000 == 0, about every
    // 0.9 G guest instructions on average): every 2^30 instructions.
    g_hb_due = true;
    g_hb_next = (ctx.inst_count | ((1ull << 30) - 1)) + 1;
  }
  const bool r = ppc_full(ctx);
  g_hb_due = false;
  poll_update_force();
  return r;
}

// R16w: age limit of the guest's GX write-gather buffer (gx.cpp).
extern uint64_t g_wgp_age_next;
void GX_WGPIPE_AgeFlush(uint64_t now, uint64_t tb_freq);

// Same as CPUContext::read_timebase() on its refresh path (wall clock).
static NWII_NOINLINE void poll_tb_refresh(CPUContext &ctx) {
  poll_count(g_poll_tb_n);
  const uint64_t now = ctx.wall_timebase_now();
  const uint64_t tb = ctx.guest_timebase_from_wall(now);
  if (tb > ctx.tb_cached) ctx.tb_cached = tb;
  if (__builtin_expect(now >= g_wgp_age_next, 0)) GX_WGPIPE_AgeFlush(now, ctx.tb_freq);
}

// ---- NWII_POLL_CHECK=1 (test only): is the inline skip ever wrong? ---------
// Every poll comes here (the countdown is pinned to kCheckForce); the real
// countdown is kept in s_vleft, kicks included. For each poll the inline
// part would have skipped, this checks that the full logic would not have
// done anything either at the same guest time (no ppc_full condition, no
// event due, no decrementer / interrupt / callback to deliver with EE set),
// and then skips it as the inline part would. Mismatches are counted and the
// first ones logged ([POLL] check). The heartbeat (log only) is not counted.
static constexpr int32_t kCheckForce = -(1 << 29);
static int32_t s_vleft = 0;
static uint64_t s_chk_polls = 0, s_chk_skips = 0, s_chk_bad = 0;

static NWII_NOINLINE int32_t poll_check_entry(CPUContext &ctx, int32_t left, bool &skip) {
  ++s_chk_polls;
  if (left == kCheckForce - 1) s_vleft -= 1;        // no kick since the last poll
  else if (left == 0) s_vleft = 0;                  // tb_invalidate
  else s_vleft = CPUContext::kPollKick - 1;         // kicked
  ctx.poll_left.store(kCheckForce, std::memory_order_relaxed);
  const bool ee = ctx.msr & 0x8000;
  const bool ints = (hw::pi_intsr.load(std::memory_order_relaxed) &
                     hw::pi_intmr.load(std::memory_order_relaxed)) != 0;
  const bool cb = ctx.pending_cb_count.load(std::memory_order_relaxed) != 0;
  skip = ((uint32_t)(ctx.pc + 4u) > 4u) && s_vleft > 0 && !(ee && (ctx.poll_dec_due | ints | cb));
  if (skip) {
    ++s_chk_skips;
    const uint64_t tb = g_tb_wall ? ctx.tb_cached : ctx.inst_count;
    const bool force = g_poll_force || ((g_ipc_interrupt_delay | hw::g_di_interrupt_delay) != 0) ||
                       ctx.vblank_pending.load(std::memory_order_relaxed) || ctx.in_callback;
    const bool event = tb >= EventScheduler::get().next_due();
    const bool dec = ctx.dec_irq_pending && (tb - ctx.dec_written_tb) >= ctx.dec_value;
    if (force || event || (ee && (dec || ints || cb))) {
      if (s_chk_bad++ < 20)
        std::printf("[POLL] check: skipped poll would act: force=%d event=%d dec=%d ee=%d pc=0x%08x "
                    "vleft=%d tb=%llu next_due=%llu\n", (int)force, (int)event, (int)dec, (int)ee,
                    ctx.pc, s_vleft, (unsigned long long)tb,
                    (unsigned long long)EventScheduler::get().next_due());
    }
  }
  if ((s_chk_polls & ((1u << 26) - 1)) == 0)
    std::printf("[POLL] check: %llu polls, %llu skipped by the inline path, %llu mismatches\n",
                (unsigned long long)s_chk_polls, (unsigned long long)s_chk_skips,
                (unsigned long long)s_chk_bad);
  return s_vleft;
}

// ---- [r33-g7] sondeo compacto -----------------------------------------------------------------
// El codigo generado G7 (g7::poll_due) solo cuenta atras: ya no mira en cada sondeo
// "MSR[EE] && (poll_dec_due | PI_INTSR & PI_INTMR | pending_cb_count)". Para que el sondeo lento
// corra en los mismos sondeos que antes, todo lo que puede volver cierta esa condicion fuerza el
// sondeo siguiente (poll_kick): trigger_pi_interrupt (hw.h), escribir PI_INTMR (hw_pi.cpp),
// queue_callback (cpu_context.h), poner MSR[EE] (poll_ee_kick: mtmsr/rfi del interprete, carga de
// contexto; los mtmsr del codigo generado ya hacen un sondeo completo al encender EE y run_game
// sondea con la comprobacion completa en cada vuelta) y este camino lento cuando sale con la
// condicion aun cierta (poll_persist_exit). Los sitios con el sondeo antiguo (run_game, mtmsr,
// funciones sin G5) siguen mirando las banderas ellos mismos: los avisos de mas no les cambian nada.
// Peor caso si un aviso de otro hilo se pisa con la cuenta atras (carga-resta-almacen no atomico):
// la interrupcion se entrega en el siguiente sondeo lento natural (<= NWII_POLL_EVERY sondeos, ~5 us).
void poll_ee_kick(CPUContext &ctx) {
  if ((ctx.msr & 0x8000) &&
      (ctx.poll_dec_due |
       ((hw::pi_intsr.load(std::memory_order_relaxed) & hw::pi_intmr.load(std::memory_order_relaxed)) != 0) |
       (ctx.pending_cb_count.load(std::memory_order_relaxed) != 0)))
    ctx.poll_kick();
}
// Condicion aun cierta al salir del sondeo lento sin entregar nada (p. ej. manejador a NULL durante un
// reinicio del SO): el sondeo antiguo volvia al camino lento en CADA sondeo, con la cuenta atras
// siguiendo su curso (reloj cada kPollTbEvery). Aqui: se guarda la cuenta atras en s_g7_shadow, se
// fuerza el sondeo siguiente y, al entrar forzado por nosotros, se sigue desde la copia.
static int32_t s_g7_shadow = 0;
static bool s_g7_rekick = false;
static inline void poll_persist_exit(CPUContext &ctx) {
  if (__builtin_expect((ctx.msr & 0x8000) &&
                       (ctx.poll_dec_due |
                        ((hw::pi_intsr.load(std::memory_order_relaxed) & hw::pi_intmr.load(std::memory_order_relaxed)) != 0) |
                        (ctx.pending_cb_count.load(std::memory_order_relaxed) != 0)), 0)) {
    const int32_t cur = ctx.poll_left.load(std::memory_order_relaxed);
    if (cur > 0) {  // (<= 0: ya esta forzado por otro aviso)
      s_g7_shadow = cur;
      s_g7_rekick = true;
      ctx.poll_kick();
    }
  }
}
NWII_NO_TSAN static bool process_pending_callbacks_slow_inner(CPUContext &ctx);
NWII_NO_TSAN bool process_pending_callbacks_slow(CPUContext &ctx) {
  if (__builtin_expect(s2::g_enabled, 0)) {  // r34s2
    if (&ctx != g_ctx_ptr) return s2::snd_poll(ctx);  // contexto de la CPU de sonido: su propio sondeo (sin s_g7_*)
    s2::game_poll(ctx);
  }
  const bool r = process_pending_callbacks_slow_inner(ctx);  // [r33-g7]
  if (!g_poll_check) poll_persist_exit(ctx);
  return r;
}

NWII_NO_TSAN static bool process_pending_callbacks_slow_inner(CPUContext &ctx) {
  poll_count(g_poll_slow_n);
  int32_t left = ctx.poll_left.load(std::memory_order_relaxed);
  if (__builtin_expect(s_g7_rekick, 0)) {
    s_g7_rekick = false;
    if (left == CPUContext::kPollKick - 1) {  // forzado por poll_persist_exit y nada mas desde entonces
      left = s_g7_shadow - 1;                 // la cuenta atras sigue como con el sondeo antiguo
      ctx.poll_left.store(left, std::memory_order_relaxed);
    }
  }
  if (__builtin_expect(g_poll_check, 0)) {
    bool skip;
    left = poll_check_entry(ctx, left, skip);
    if (skip) return false;
  }
  bool refresh = false;
  if (left <= 0) {
    // 0: the countdown ran out (or tb_invalidate); < 0: kicked.
    refresh = left == 0;
    // r31t (frente T): NWII_POLL_EVERY=N -> el sondeo lento (reloj + agenda) cada N
    // sondeos en vez de cada kPollTbEvery (16). Quien levanta una interrupcion o
    // encola un evento ya fuerza el sondeo siguiente (poll_kick), asi que solo
    // cambia la finura con que el hilo del juego mira el reloj.
    static const int32_t s_every = [] {
      const char *e = NWII_ENV("NWII_POLL_EVERY");
      const int v = e ? std::atoi(e) : 0;
      return v >= 1 && v <= 4096 ? (int32_t)v : CPUContext::kPollTbEvery;
    }();
    const int32_t k = g_tb_wall ? s_every : 1;
    if (__builtin_expect(g_poll_check, 0)) s_vleft = k;
    else ctx.poll_left.store(k, std::memory_order_relaxed);
  }
  if (__builtin_expect(g_poll_force | ((uint32_t)(ctx.pc + 4u) <= 4u) |  // 0xFFFFFFFC / 0
                           (ctx.inst_count >= g_hb_next) |                 // [Heartbeat]
                           ((g_ipc_interrupt_delay | hw::g_di_interrupt_delay) != 0) |
                           ctx.vblank_pending.load(std::memory_order_relaxed) |
                           ctx.in_callback,
                       0))
    return ppc_full_entry(ctx);
  uint64_t tb;
  if (__builtin_expect(g_tb_wall, 1)) {
    if (refresh) poll_tb_refresh(ctx);
    tb = ctx.tb_cached;
  } else {
    tb = ctx.inst_count;
  }
  EventScheduler::get().advance(ctx, tb);
  const bool dec_exp = ctx.dec_irq_pending && (tb - ctx.dec_written_tb) >= ctx.dec_value;
  ctx.poll_dec_due = dec_exp;
  if (!(ctx.msr & 0x8000)) return false;
  if (!dec_exp &&
      (hw::pi_intsr.load(std::memory_order_relaxed) & hw::pi_intmr.load(std::memory_order_relaxed)) == 0 &&
      ctx.pending_cb_count.load(std::memory_order_relaxed) == 0)
    return false;
  poll_count(g_poll_tail_n);
  return ppc_tail(ctx, dec_exp);
}

// ", slow polls ../s (full .., tail .., clock ..)" for the fps lines (NWII_POLL_STATS).
void poll_stats_report(char *buf, size_t n, double dt) {
  static uint64_t p0 = 0, f0 = 0, t0 = 0, c0 = 0;
  if (n == 0) return;
  buf[0] = 0;
  if (!g_poll_stats) return;
  const uint64_t p = g_poll_slow_n.load(std::memory_order_relaxed), f = g_poll_full_n.load(std::memory_order_relaxed),
                 t = g_poll_tail_n.load(std::memory_order_relaxed), c = g_poll_tb_n.load(std::memory_order_relaxed);
  const double k = dt > 0 ? 1.0 / dt : 0.0;
  std::snprintf(buf, n, ", slow polls %.0fk/s (full %.0f/s, tail %.0f/s, clock %.0fk/s)",
                (p - p0) * k / 1000, (f - f0) * k, (t - t0) * k, (c - c0) * k / 1000);
  p0 = p; f0 = f; t0 = t; c0 = c;
}

} 
