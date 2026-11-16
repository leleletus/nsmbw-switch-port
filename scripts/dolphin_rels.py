#!/usr/bin/env python3
"""Print the REL modules linked in a running Dolphin (Wii) and their bases.

Reads emulated MEM1 straight from Dolphin's shared-memory fd and walks the
OS module list at 0x800030C8. Output is ready to paste into [[rel_aot]].

Usage: dolphin_rels.py <dolphin_pid> <dir_with_rels>
"""
import os
import struct
import sys

MEM1_SIZE = 0x01800000
MODULE_LIST_HEAD = 0x800030C8


def find_ram_fd(pid):
    fd_dir = f"/proc/{pid}/fd"
    best = None
    for fd in os.listdir(fd_dir):
        try:
            target = os.readlink(f"{fd_dir}/{fd}")
        except OSError:
            continue
        if "dolphin" in target.lower() and ("memfd" in target or "/dev/shm" in target):
            size = os.stat(f"{fd_dir}/{fd}").st_size
            if best is None or size > best[1]:
                best = (f"{fd_dir}/{fd}", size)
    if not best:
        sys.exit("no Dolphin shared-memory fd found (is the game running?)")
    return best[0]


def main():
    pid, rel_dir = sys.argv[1], sys.argv[2]
    with open(find_ram_fd(pid), "rb") as f:
        mem1 = f.read(MEM1_SIZE)

    def u32(addr):
        off = addr & 0x3FFFFFFF
        if off + 4 > len(mem1):
            raise ValueError(f"{addr:#x} outside MEM1")
        return struct.unpack(">I", mem1[off:off + 4])[0]

    ids = {}
    for name in sorted(os.listdir(rel_dir)):
        if name.endswith(".rel"):
            with open(os.path.join(rel_dir, name), "rb") as rf:
                ids[struct.unpack(">I", rf.read(4))[0]] = name

    mod = u32(MODULE_LIST_HEAD)
    while mod:
        mod_id = u32(mod + 0x00)
        nsec = u32(mod + 0x0C)
        secinfo = u32(mod + 0x10)
        bss_size = u32(mod + 0x20)
        bss = 0
        for i in range(nsec):
            off, size = u32(secinfo + i * 8), u32(secinfo + i * 8 + 4)
            if size == bss_size and off and not (off & 1):
                bss = off
        print("[[rel_aot]]")
        print(f'path = "{os.path.abspath(os.path.join(rel_dir, ids.get(mod_id, f"unknown_id_{mod_id}.rel")))}"')
        print(f"module_base = 0x{mod:08X}")
        print(f"bss_base = 0x{bss:08X}  # bss size 0x{bss_size:X}\n")
        mod = u32(mod + 0x04)


if __name__ == "__main__":
    main()
