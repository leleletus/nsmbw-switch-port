#!/usr/bin/env bash
# build_g6.sh <nx|pc> <export_dir> <obj_dir>    (run it under ~/nsmbw-port/scripts/lim; JOBS default 2)
# Compiles every output_*.cpp of <export_dir> (the generated code of a G6 variant) into <obj_dir>/output_N.o
# with exactly the compile command of r9/r14g5's build (Switch: nx/build, PC: export/build), only the source,
# the object and the export include dir changed. Incremental (ninja; a changed .cpp recompiles).
# main_output.cpp / fastdispatch.cpp / functions.h are not touched by G5/G6: the links reuse r14g5's objects.
set -euo pipefail
W=$1; E=$(realpath "$2"); O=$(realpath -m "$3")
G5=/home/djvemo/nsmbw-port/r9/r14g5
mkdir -p "$O"
if [[ -n ${TMPL:-} ]]; then
  # [r30g6c3] template saved beforehand (other machine without r14g5's build dirs): tools/templates/<nx|pc>.cmd
  tmpl=$(cat "$TMPL"); tmpl=${tmpl//@EXPORT@/$E}
  if [[ $W == nx ]]; then RUNDIR=$G5/nx/build; else RUNDIR=$G5/export/build; fi
  mkdir -p "$RUNDIR"
elif [[ $W == nx ]]; then
  all=$(cd $G5/nx/build && ninja -t commands nsmbw.elf); tmpl=$(grep -m1 -- "-c $G5/export/output_3.cpp\$" <<<"$all")
  ob=CMakeFiles/nsmbw.dir$G5/export/output_3.o
  tmpl=${tmpl// -MD -MT $ob -MF $ob.d/}
  tmpl=${tmpl/-o $ob/-o \$out}
  tmpl=${tmpl/-c $G5\/export\/output_3.cpp/-c \$in}
  tmpl=${tmpl/-I$G5\/export /-I$E }
  RUNDIR=$G5/nx/build
else
  all=$(cd $G5/export/build && ninja -t commands NSMBW); tmpl=$(grep -m1 -- "-c $G5/export/output_3.cpp\$" <<<"$all")
  ob=CMakeFiles/NSMBW.dir/output_3.cpp.o
  tmpl=${tmpl// -MD -MT $ob -MF $ob.d/}
  tmpl=${tmpl/-o $ob/-o \$out}
  tmpl=${tmpl/-c $G5\/export\/output_3.cpp/-I$E -c \$in}
  RUNDIR=$G5/export/build
fi
tmpl=${tmpl/ccache /}
# [r33g7] EXTRA_FLAGS="-DG7_NOIC": flags de mas (delante de -c)
[[ -n ${EXTRA_FLAGS:-} ]] && tmpl=${tmpl/ -c \$in/ $EXTRA_FLAGS -c \$in}
[[ $tmpl == *'$in'* && $tmpl == *'$out'* ]] || { echo "build_g6: could not make the compile template" >&2; exit 2; }
{
  echo "rule cc"
  echo "  command = cd $RUNDIR && nice -n 5 $tmpl"
  echo "  description = $W \$in"
  for f in "$E"/output_*.cpp; do
    b=$(basename "$f" .cpp)
    echo "build $O/$b.o: cc $f"
  done
} > "$O/build.ninja"
echo "$tmpl" > "$O/compile.cmd"
[[ -n ${DRY:-} ]] && { cat "$O/compile.cmd"; exit 0; }
ninja -f "$O/build.ninja" -j"${JOBS:-2}"
ls "$O"/output_*.o | wc -l
