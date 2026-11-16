// IOS /dev/fs backed by a host directory (Config::nand_dir), following
// Dolphin's IOS::HLE::FS semantics: the NAND tree lives at <nand_dir>/<path>,
// files persist across runs, /tmp is wiped at boot, and every FS ioctl the
// SDK's ISFS/NAND libraries use is served (unknown ones are logged).
#include <cstdlib>
#include "runtime/devices.h"
#include "runtime/config.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace nwii::runtime::devices {

namespace fs = std::filesystem;

// IOS FS result codes.
constexpr int32_t FS_EINVAL = -101;
constexpr int32_t FS_EACCESS = -102;
constexpr int32_t FS_EEXIST = -105;
constexpr int32_t FS_ENOENT = -106;
constexpr int32_t FS_ENFILE = -109;
constexpr int32_t FS_ENOTEMPTY = -115;

// /dev/fs ioctls.
enum : uint32_t {
    ISFS_FORMAT = 0x01,
    ISFS_GET_STATS = 0x02,
    ISFS_CREATE_DIR = 0x03,
    ISFS_READ_DIR = 0x04,       // ioctlv
    ISFS_SET_ATTR = 0x05,
    ISFS_GET_ATTR = 0x06,
    ISFS_DELETE = 0x07,
    ISFS_RENAME = 0x08,
    ISFS_CREATE_FILE = 0x09,
    ISFS_SET_FILE_VER_CTRL = 0x0A,
    ISFS_GET_FILE_STATS = 0x0B, // on a file fd
    ISFS_GET_USAGE = 0x0C,      // ioctlv
    ISFS_SHUTDOWN = 0x0D,
};

constexpr uint32_t NAND_PATH_MAX = 64;
constexpr uint32_t CLUSTER_SIZE = 0x4000;
// ISFS attribute block: owner u32, group u16, path[64], 3 mode bytes, attr.
constexpr uint32_t ATTR_SIZE = 0x4C;
constexpr uint32_t ATTR_PATH = 0x06;
constexpr uint32_t ATTR_OWNER_MODE = 0x46;

// IOS hands out physical addresses; the MMU wants effective ones.
static uint32_t to_virt(uint32_t addr) {
    if (addr < 0x01800000 || (addr >= 0x10000000 && addr < 0x14000000))
        return addr | 0x80000000;
    return addr;
}

static std::string read_path(CPUContext& ctx, uint32_t addr) {
    std::string s;
    addr = to_virt(addr);
    for (uint32_t i = 0; i < NAND_PATH_MAX; ++i) {
        char c = (char)ctx.mmu.read8(addr + i);
        if (!c) break;
        s += c;
    }
    return s;
}

static void copy_out(CPUContext& ctx, uint32_t addr, const void* src, size_t n) {
    addr = to_virt(addr);
    const uint8_t* p = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) ctx.mmu.write8(addr + (uint32_t)i, p[i]);
}

static void copy_in(CPUContext& ctx, uint32_t addr, void* dst, size_t n) {
    addr = to_virt(addr);
    uint8_t* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; ++i) p[i] = ctx.mmu.read8(addr + (uint32_t)i);
}

// --- system files a fresh console carries -----------------------------------

// SYSCONF in the SCv0 layout the SC library parses (as Dolphin writes it):
// "SCv0", u16 count, u16 offsets[count + 1], then entries of
// (type << 5 | name_len - 1), name, payload; "SCed" closes the 16 KiB file.
static std::vector<uint8_t> make_sysconf(int language) {
    enum { BIGARRAY = 1, SMALLARRAY = 2, BYTE = 3, SHORT = 4, LONG = 5 };
    struct Item { const char* name; int type; std::vector<uint8_t> data; };
    auto u32be = [](uint32_t v) {
        return std::vector<uint8_t>{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    };
    std::vector<uint8_t> sadr(0x1007, 0);
    sadr[0] = 49;  // country: United States, matching the USA disc
    std::vector<uint8_t> nick(0x16, 0);
    nick[1] = 'W'; nick[3] = 'i'; nick[5] = 'i';  // UTF-16BE "Wii"
    const std::vector<Item> items = {
        {"IPL.LNG", BYTE, {uint8_t(language)}},
        {"IPL.CD", BYTE, {1}},
        {"IPL.CD2", BYTE, {1}},
        {"IPL.AR", BYTE, {1}},     // 16:9
        {"IPL.E60", BYTE, {1}},
        {"IPL.PGS", BYTE, {0}},
        {"IPL.SSV", BYTE, {1}},
        {"IPL.SND", BYTE, {1}},    // stereo
        {"IPL.DH", BYTE, {0}},
        {"IPL.EULA", BYTE, {1}},
        {"IPL.CB", LONG, u32be(0)},
        {"IPL.IDL", SMALLARRAY, {0x00, 0x01}},
        {"IPL.NIK", SMALLARRAY, nick},
        {"IPL.PC", SMALLARRAY, std::vector<uint8_t>(0x4A, 0)},
        {"IPL.SADR", BIGARRAY, sadr},
        {"BT.SENS", LONG, u32be(3)},
        {"BT.BAR", BYTE, {1}},
        {"BT.SPKV", BYTE, {0x58}},
        {"BT.MOT", BYTE, {1}},
        {"BT.DINF", BIGARRAY, std::vector<uint8_t>(0x461, 0)},
        {"BT.CDIF", BIGARRAY, std::vector<uint8_t>(0x205, 0)},
    };
    std::vector<uint8_t> sc(0x4000, 0);
    std::memcpy(sc.data(), "SCv0", 4);
    const uint16_t count = (uint16_t)items.size();
    sc[4] = count >> 8; sc[5] = count & 0xFF;
    size_t off = 6 + 2 * (count + 1);
    for (uint16_t i = 0; i < count; ++i) {
        const Item& it = items[i];
        sc[6 + 2 * i] = off >> 8; sc[7 + 2 * i] = off & 0xFF;
        size_t name_len = std::strlen(it.name);
        sc[off++] = uint8_t((it.type << 5) | (name_len - 1));
        std::memcpy(&sc[off], it.name, name_len);
        off += name_len;
        if (it.type == BIGARRAY) {
            uint16_t n = uint16_t(it.data.size() - 1);
            sc[off++] = n >> 8; sc[off++] = n & 0xFF;
        } else if (it.type == SMALLARRAY) {
            sc[off++] = uint8_t(it.data.size() - 1);
        }
        std::memcpy(&sc[off], it.data.data(), it.data.size());
        off += it.data.size();
    }
    sc[6 + 2 * count] = off >> 8; sc[7 + 2 * count] = off & 0xFF;
    std::memcpy(&sc[0x4000 - 4], "SCed", 4);
    return sc;
}

static std::vector<uint8_t> make_setting_txt() {
    const char* txt =
        "AREA=USA\r\n"
        "MODEL=RVL-001(USA)\r\n"
        "DVD=0\r\n"
        "MPCH=0x7FFE\r\n"
        "CODE=LU\r\n"
        "SERNO=123456789\r\n"
        "VIDEO=NTSC\r\n"
        "GAME=US\r\n";
    return std::vector<uint8_t>(txt, txt + std::strlen(txt));
}

class FSDevice : public IDevice {
public:
    const char* get_name() const override { return "/dev/fs"; }
    bool matches_path(const std::string& path) const override {
        if (path == "/dev/fs") return true;
        if (path.find("/dev/") == 0) return false;
        return path.find("/") == 0;
    }

    int32_t open(CPUContext& ctx, const std::string& path, uint32_t mode) override {
        init();
        if (path == "/dev/fs") return 0;
        fs::path host;
        if (!host_path(path, host)) return FS_EINVAL;
        std::error_code ec;
        if (!fs::exists(host, ec)) {
            std::cout << "[FS] open " << path << ": not found\n";
            return FS_ENOENT;
        }
        if (fs::is_directory(host, ec)) return FS_EACCESS;
        for (size_t i = 1; i < m_handles.size(); ++i) {
            if (!m_handles[i].open) {
                m_handles[i] = {host, path, 0, mode, true};
                std::cout << "[FS] open " << path << " mode=" << mode
                          << " -> h" << i << "\n";
                return (int32_t)i;
            }
        }
        return FS_ENFILE;
    }

    int32_t close(CPUContext& ctx, uint32_t fd) override {
        if (Handle* h = handle(fd)) h->open = false;
        return IPC_OK;
    }

    int32_t read(CPUContext& ctx, uint32_t fd, uint32_t buf, uint32_t len) override {
        Handle* h = handle(fd);
        if (!h) return FS_EINVAL;
        if (!(h->mode & 1)) return FS_EACCESS;
        std::ifstream f(h->host, std::ios::binary);
        if (!f) return FS_ENOENT;
        f.seekg(0, std::ios::end);
        uint32_t size = (uint32_t)f.tellg();
        uint32_t n = h->pos < size ? std::min(len, size - h->pos) : 0;
        std::vector<uint8_t> data(n);
        f.seekg(h->pos);
        f.read(reinterpret_cast<char*>(data.data()), n);
        copy_out(ctx, buf, data.data(), n);
        h->pos += n;
        return (int32_t)n;
    }

    int32_t write(CPUContext& ctx, uint32_t fd, uint32_t buf, uint32_t len) override {
        Handle* h = handle(fd);
        if (!h) return FS_EINVAL;
        if (!(h->mode & 2)) return FS_EACCESS;
        std::vector<uint8_t> data(len);
        copy_in(ctx, buf, data.data(), len);
        std::fstream f(h->host, std::ios::binary | std::ios::in | std::ios::out);
        if (!f) return FS_ENOENT;
        f.seekp(h->pos);
        f.write(reinterpret_cast<const char*>(data.data()), len);
        h->pos += len;
        return (int32_t)len;
    }

    int32_t seek(CPUContext& ctx, uint32_t fd, int32_t offset, int32_t whence) override {
        Handle* h = handle(fd);
        if (!h) return FS_EINVAL;
        std::error_code ec;
        int64_t size = (int64_t)fs::file_size(h->host, ec);
        int64_t base = whence == 0 ? 0 : whence == 1 ? h->pos : whence == 2 ? size : -1;
        int64_t pos = base + offset;
        if (base < 0 || pos < 0 || pos > size) return FS_EINVAL;
        h->pos = (uint32_t)pos;
        return (int32_t)pos;
    }

    int32_t ioctl(CPUContext& ctx, const IpcRequest& req) override {
        init();
        switch (req.ioctl_cmd) {
        case ISFS_FORMAT:
        case ISFS_SET_FILE_VER_CTRL:
        case ISFS_SHUTDOWN:
            return IPC_OK;
        case ISFS_GET_STATS: {
            // cluster size, free/used/bad/reserved clusters, free/used inodes
            const uint32_t stats[7] = {CLUSTER_SIZE, 0x5DEC, 0x1DD4, 0x10, 0x02F0, 0x146B, 0x0394};
            for (int i = 0; i < 7 && (uint32_t)i * 4 < req.out_size; ++i)
                ctx.mmu.write32(to_virt(req.out_buf) + i * 4, stats[i]);
            return IPC_OK;
        }
        case ISFS_CREATE_DIR:
        case ISFS_CREATE_FILE:
            return create(ctx, req.in_buf, req.ioctl_cmd == ISFS_CREATE_DIR);
        case ISFS_SET_ATTR: {
            fs::path host;
            std::string path = read_path(ctx, req.in_buf + ATTR_PATH);
            if (!host_path(path, host)) return FS_EINVAL;
            std::error_code ec;
            return fs::exists(host, ec) ? IPC_OK : FS_ENOENT;
        }
        case ISFS_GET_ATTR:
            return get_attr(ctx, read_path(ctx, req.in_buf), req.out_buf);
        case ISFS_DELETE: {
            std::string path = read_path(ctx, req.in_buf);
            fs::path host;
            if (!host_path(path, host) || path == "/") return FS_EINVAL;
            std::error_code ec;
            if (!fs::exists(host, ec)) return FS_ENOENT;
            fs::remove_all(host, ec);
            std::cout << "[FS] delete " << path << "\n";
            return ec ? FS_EACCESS : IPC_OK;
        }
        case ISFS_RENAME: {
            std::string from = read_path(ctx, req.in_buf);
            std::string to = read_path(ctx, req.in_buf + NAND_PATH_MAX);
            fs::path hf, ht;
            if (!host_path(from, hf) || !host_path(to, ht)) return FS_EINVAL;
            std::error_code ec;
            if (!fs::exists(hf, ec)) return FS_ENOENT;
            if (!fs::is_directory(ht.parent_path(), ec)) return FS_ENOENT;
            if (fs::exists(ht, ec)) fs::remove_all(ht, ec);
            fs::rename(hf, ht, ec);
            std::cout << "[FS] rename " << from << " -> " << to << "\n";
            return ec ? FS_EACCESS : IPC_OK;
        }
        case ISFS_GET_FILE_STATS: {
            Handle* h = handle(req.internal_fd);
            if (!h) return FS_EINVAL;
            std::error_code ec;
            uint32_t size = (uint32_t)fs::file_size(h->host, ec);
            ctx.mmu.write32(to_virt(req.out_buf), size);
            ctx.mmu.write32(to_virt(req.out_buf) + 4, h->pos);
            return IPC_OK;
        }
        default:
            std::cout << "[FS] unhandled ioctl 0x" << std::hex << req.ioctl_cmd
                      << std::dec << " in=" << req.in_size << " out=" << req.out_size << "\n";
            return FS_EINVAL;
        }
    }

    int32_t ioctlv(CPUContext& ctx, const IpcRequest& req) override {
        init();
        const auto& v = req.ioctlv_vecs;
        switch (req.ioctl_cmd) {
        case ISFS_READ_DIR: {
            if (req.arg_cnt_in < 1 || req.arg_cnt_out < 1) return FS_EINVAL;
            std::string path = read_path(ctx, v[0].addr);
            fs::path host;
            if (!host_path(path, host)) return FS_EINVAL;
            std::error_code ec;
            if (!fs::exists(host, ec)) return FS_ENOENT;
            if (!fs::is_directory(host, ec)) return FS_EINVAL;
            std::vector<std::string> names;
            for (const auto& e : fs::directory_iterator(host, ec))
                names.push_back(e.path().filename().string());
            std::sort(names.begin(), names.end());
            if (req.arg_cnt_in == 1) {
                // count only: out0 = u32 count
                ctx.mmu.write32(to_virt(v[1].addr), (uint32_t)names.size());
            } else {
                // in1 = max entries; out0 = NUL-separated names, out1 = count
                uint32_t max = ctx.mmu.read32(to_virt(v[1].addr));
                uint32_t n = std::min<uint32_t>(max, (uint32_t)names.size());
                uint32_t dst = v[2].addr, room = v[2].len;
                for (uint32_t i = 0; i < n; ++i) {
                    uint32_t len = (uint32_t)names[i].size() + 1;
                    if (len > room) { n = i; break; }
                    copy_out(ctx, dst, names[i].c_str(), len);
                    dst += len;
                    room -= len;
                }
                ctx.mmu.write32(to_virt(v[3].addr), n);
            }
            std::cout << "[FS] readdir " << path << " -> " << names.size() << " entries\n";
            return IPC_OK;
        }
        case ISFS_GET_USAGE: {
            if (req.arg_cnt_in < 1 || req.arg_cnt_out < 2) return FS_EINVAL;
            std::string path = read_path(ctx, v[0].addr);
            fs::path host;
            if (!host_path(path, host)) return FS_EINVAL;
            std::error_code ec;
            if (!fs::is_directory(host, ec)) return FS_ENOENT;
            uint32_t clusters = 0, inodes = 1;
            for (const auto& e : fs::recursive_directory_iterator(host, ec)) {
                ++inodes;
                if (e.is_regular_file(ec))
                    clusters += (uint32_t)((e.file_size(ec) + CLUSTER_SIZE - 1) / CLUSTER_SIZE);
            }
            ctx.mmu.write32(to_virt(v[1].addr), clusters);
            ctx.mmu.write32(to_virt(v[2].addr), inodes);
            return IPC_OK;
        }
        default:
            std::cout << "[FS] unhandled ioctlv 0x" << std::hex << req.ioctl_cmd
                      << std::dec << " in=" << req.arg_cnt_in << " out=" << req.arg_cnt_out << "\n";
            return FS_EINVAL;
        }
    }

private:
    struct Handle {
        fs::path host;
        std::string nand_path;
        uint32_t pos = 0;
        uint32_t mode = 0;
        bool open = false;
    };
    std::vector<Handle> m_handles = std::vector<Handle>(16);
    fs::path m_root;
    bool m_init = false;

    Handle* handle(uint32_t fd) {
        return (fd > 0 && fd < m_handles.size() && m_handles[fd].open) ? &m_handles[fd] : nullptr;
    }

    // NAND path -> host path; rejects anything that is not an absolute NAND
    // path or that could step outside the NAND root.
    bool host_path(const std::string& path, fs::path& out) const {
        if (path.empty() || path[0] != '/' || path.size() >= NAND_PATH_MAX) return false;
        if (path.find("..") != std::string::npos) return false;
        out = m_root / path.substr(1);
        return true;
    }

    void write_file(const std::string& nand_path, const std::vector<uint8_t>& data) {
        fs::path host;
        host_path(nand_path, host);
        std::error_code ec;
        fs::create_directories(host.parent_path(), ec);
        std::ofstream(host, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    void init() {
        if (m_init) return;
        m_init = true;
        const Config& cfg = Config::get();
        m_root = cfg.nand_dir;
        std::error_code ec;
        // r21s: NWII_NO_SAVE=1 (y siempre con NWII_FORCE_LEVEL): ninguna escritura llega a la partida
        // de verdad. Al arrancar, la NAND real (save/) se copia a <nand_dir>_prueba (rehecha en cada
        // arranque) y el juego lee y escribe ahí: cree que guardó bien, y la real no se toca.
        {
            const char* ns = std::getenv("NWII_NO_SAVE");
            const char* fl = std::getenv("NWII_FORCE_LEVEL");
            if ((ns && ns[0] && ns[0] != '0') || (fl && fl[0])) {
                fs::path real = m_root, copy = m_root;
                while (!copy.empty() && !copy.has_filename()) copy = copy.parent_path();
                copy += "_prueba";
                fs::remove_all(copy, ec);
                ec.clear();
                fs::create_directories(copy, ec);
                // copia a mano (directorios + archivos por flujo): sirve igual en la SD de la Switch
                size_t nfiles = 0, nerr = 0;
                if (fs::is_directory(real, ec)) {
                    std::string base = real.string();
                    while (base.size() > 1 && base.back() == '/') base.pop_back();
                    for (fs::recursive_directory_iterator it(real, ec), end; !ec && it != end; it.increment(ec)) {
                        // ruta relativa por prefijo (sin fs::relative: en la SD de la Switch no hay realpath)
                        std::string ps = it->path().string();
                        if (ps.compare(0, base.size(), base) != 0) { ++nerr; continue; }
                        ps.erase(0, base.size());
                        while (!ps.empty() && ps.front() == '/') ps.erase(ps.begin());
                        if (ps.empty()) continue;
                        const fs::path rel = ps;
                        std::error_code e2;
                        if (it->is_directory(e2)) {
                            fs::create_directories(copy / rel, e2);
                        } else {
                            std::ifstream in(it->path(), std::ios::binary);
                            std::ofstream out(copy / rel, std::ios::binary | std::ios::trunc);
                            if (fs::file_size(it->path(), e2) > 0) out << in.rdbuf();
                            if (in.bad() || !out) ++nerr; else ++nfiles;
                        }
                    }
                }
                std::cout << "[FS] NWII_NO_SAVE: la partida no se escribe; NAND de prueba " << copy.string()
                          << " (copia de " << real.string() << ": " << nfiles << " archivos, " << nerr
                          << " errores)\n";
                m_root = copy;
                ec.clear();
            }
        }
        fs::create_directories(m_root, ec);

        // IOS wipes /tmp at boot.
        fs::remove_all(m_root / "tmp", ec);
        for (const char* d : {"tmp", "shared2/sys", "shared2/menu", "import", "meta", "sys",
                              "ticket/00010000", "title/00000001/00000002/data"})
            fs::create_directories(m_root / d, ec);

        // The installed title's own directories: /title/00010000/<id>/data.
        char tid[9] = {0};
        for (int i = 0; i < 4; ++i)
            std::snprintf(tid + i * 2, 3, "%02x",
                          i < (int)cfg.game_id.size() ? (unsigned char)cfg.game_id[i] : 0);
        fs::create_directories(m_root / "title/00010000" / tid / "data", ec);
        fs::create_directories(m_root / "title/00010000" / tid / "content", ec);

        // SYSCONF is regenerated each boot so [system] language applies;
        // the others are only seeded when missing.
        write_file("/shared2/sys/SYSCONF", make_sysconf(cfg.system_language));
        if (!fs::exists(m_root / "title/00000001/00000002/data/setting.txt", ec))
            write_file("/title/00000001/00000002/data/setting.txt", make_setting_txt());
        if (!fs::exists(m_root / "title/00000001/00000002/data/play_rec.dat", ec))
            write_file("/title/00000001/00000002/data/play_rec.dat", std::vector<uint8_t>(0x20, 0));
        if (!fs::exists(m_root / "shared2/test2/dvderror.dat", ec))
            write_file("/shared2/test2/dvderror.dat", std::vector<uint8_t>(4, 0));

        std::cout << "[FS] NAND root " << m_root.string() << " (language "
                  << cfg.system_language << ", title " << tid << ")\n";
    }

    // ISFS_CreateDir / ISFS_CreateFile from an attribute block.
    int32_t create(CPUContext& ctx, uint32_t attr, bool dir) {
        std::string path = read_path(ctx, attr + ATTR_PATH);
        fs::path host;
        if (!host_path(path, host) || path == "/") return FS_EINVAL;
        std::error_code ec;
        if (fs::exists(host, ec)) return FS_EEXIST;
        if (!fs::is_directory(host.parent_path(), ec)) return FS_ENOENT;
        if (dir) fs::create_directory(host, ec);
        else std::ofstream(host, std::ios::binary);
        std::cout << "[FS] create " << (dir ? "dir " : "file ") << path << "\n";
        return ec ? FS_EACCESS : IPC_OK;
    }

    int32_t get_attr(CPUContext& ctx, const std::string& path, uint32_t out) {
        fs::path host;
        if (!host_path(path, host)) return FS_EINVAL;
        std::error_code ec;
        if (!fs::exists(host, ec)) return FS_ENOENT;
        // Everything belongs to the running title, read/write for owner and group.
        uint8_t attr[ATTR_SIZE] = {0};
        attr[0] = 0x00; attr[1] = 0x00; attr[2] = 0x10; attr[3] = 0x00;  // uid 0x1000
        const std::string& id = Config::get().game_id;
        attr[4] = id.size() > 4 ? id[4] : '0';  // gid = maker code
        attr[5] = id.size() > 5 ? id[5] : '1';
        std::memcpy(attr + ATTR_PATH, path.c_str(), std::min<size_t>(path.size(), NAND_PATH_MAX - 1));
        attr[ATTR_OWNER_MODE] = 3;
        attr[ATTR_OWNER_MODE + 1] = 3;
        attr[ATTR_OWNER_MODE + 2] = 1;
        copy_out(ctx, out, attr, ATTR_SIZE);
        return IPC_OK;
    }
};

std::unique_ptr<IDevice> create_fs_device() {
    return std::make_unique<FSDevice>();
}

}
