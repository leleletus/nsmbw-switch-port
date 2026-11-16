#!/usr/bin/env bash
# tanda 1: k1 (sin tablas), k2 (sin tablas + partición, con partial-training), k2n (k2 sin partial-training)
cd /home/djvemo/nsmbw-port/r9/r42k
KFLAGS='-fno-jump-tables' tools/build_k.sh k1
KFLAGS='-fno-jump-tables -freorder-blocks-and-partition' tools/build_k.sh k2
KFLAGS='-fno-profile-partial-training -fno-jump-tables -freorder-blocks-and-partition' tools/build_k.sh k2n
