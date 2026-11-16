#!/usr/bin/env bash
# link_g6.sh <nx|pc> <obj_dir> <name>    (run it under ~/nsmbw-port/scripts/lim)
# Links m1 (r9/r18m) with the generated-code objects of a G6 variant (tools/build_g6.sh) instead of
# G5's: the same link line r18m used (Switch: nx/link_m1_dk3drel.cmd = the bytes of nsmbw_m1.nro, GL by
# default, libdeko3d release; PC: r18m's pc build), only output_*.o swapped. Runtime, main.o, glad,
# deko3d/uam, main_output.o and fastdispatch.o are m1's.
#   nx -> bin/nsmbw_<name>.nro/.elf (+ _input_test.nro: scripted input, tests only, never the game)
#   pc -> bin/NSMBW_<name> + bin/NSMBW_<name>_input_test
set -euo pipefail
W=$1; O=$(realpath "$2"); N=$3
G=/home/djvemo/nsmbw-port/r9/r20g6; M=/home/djvemo/nsmbw-port/r9/r18m; G5=/home/djvemo/nsmbw-port/r9/r14g5
DKP=/opt/devkitpro
mkdir -p $G/bin
if [[ $W == nx ]]; then
  cmd=$(cat $M/nx/link_m1_dk3drel.cmd)
  n=0
  for f in "$O"/output_*.o; do
    b=$(basename "$f")
    old=CMakeFiles/nsmbw.dir$G5/export/$b
    [[ $b == output_24.o ]] && old=$M/nx/build/mod_output_24.o
    [[ $cmd == *" $old "* ]] || { echo "link_g6: $old not in the link line" >&2; exit 3; }
    cmd=${cmd/ $old / $f }
    n=$((n + 1))
  done
  [[ $n == 87 ]] || { echo "link_g6: $n objects, expected 87" >&2; exit 3; }
  [[ $cmd != *"$G5/export/output_"* && $cmd != *mod_output_24* ]] || { echo "link_g6: an old object is left" >&2; exit 3; }
  echo "$cmd" > $G/nx_link_$N.cmd
  D=$G/nx_link/$N; mkdir -p "$D/game" "$D/test"
  (cd $G5/nx/build && eval "${cmd/-o nsmbw.elf/-o $D/game/nsmbw.elf}")
  $DKP/tools/bin/nacptool --create "NSMBW PRUEBA G6 $N" "nsmbw-port" "$N" "$D/game/nsmbw.nacp" > /dev/null
  $DKP/tools/bin/elf2nro "$D/game/nsmbw.elf" "$G/bin/nsmbw_$N.nro" --icon=$DKP/libnx/default_icon.jpg --nacp="$D/game/nsmbw.nacp" > /dev/null
  cp -f "$D/game/nsmbw.elf" "$G/bin/nsmbw_$N.elf"
  tcmd=${cmd/-o nsmbw.elf/-o $D/test/nsmbw.elf}
  tcmd=${tcmd/ -fPIE / -fPIE $M/nx/build/input_script_nx.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
  (cd $G5/nx/build && eval "$tcmd")
  $DKP/tools/bin/elf2nro "$D/test/nsmbw.elf" "$G/bin/nsmbw_${N}_input_test.nro" --icon=$DKP/libnx/default_icon.jpg --nacp="$D/game/nsmbw.nacp" > /dev/null
  ls -la "$G/bin/nsmbw_$N.nro" "$G/bin/nsmbw_$N.elf" "$G/bin/nsmbw_${N}_input_test.nro"
else
  RB=$G5/export/build B=$M/pc_build
  all=$(cd "$RB" && ninja -t commands NSMBW); cmd=$(tail -1 <<<"$all" | sed "s/^: && //; s/ && :$//")
  n=0
  for f in "$O"/output_*.o; do
    b=$(basename "$f" .o)
    old=CMakeFiles/NSMBW.dir/$b.cpp.o
    [[ $cmd == *" $old "* ]] || { echo "link_g6: $old not in the PC link line" >&2; exit 3; }
    cmd=${cmd/ $old / $f }
    n=$((n + 1))
  done
  [[ $n == 87 ]] || { echo "link_g6: $n PC objects, expected 87" >&2; exit 3; }
  cmd=${cmd/-Wl,--dependency-file=CMakeFiles\/NSMBW.dir\/link.d/}
  cmd=${cmd/CMakeFiles\/NSMBW.dir\/nWiiRuntime\/src\/core\/main.cpp.o/$B/CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o}
  cmd=${cmd/nWiiRuntime\/libnwiiruntime.a/$B/nWiiRuntime/libnwiiruntime.a}
  echo "$cmd" > $G/pc_link_$N.cmd
  (cd "$RB" && eval "${cmd/ -o NSMBW / -o $G/bin/NSMBW_$N }")
  (cd "$RB" && eval "${cmd/ -o NSMBW / -o $G/bin/NSMBW_${N}_input_test $B/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
  ls -la "$G/bin/NSMBW_$N" "$G/bin/NSMBW_${N}_input_test"
fi
