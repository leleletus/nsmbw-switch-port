#!/usr/bin/env bash
# tools/tu_peak.sh <source.cpp> <out.o> [extra flags...]: compile ONE generated TU with the Switch
# flags (+ extras) and print the peak RSS (VmHWM of cc1plus, sampled every 0.2 s) and the time.
# Run it through ~/nsmbw-port/scripts/lim. Waits until MemAvailable >= 7 GB before starting.
set -uo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
src=$1; out=$2; shift 2
while (( $(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo) < 7000 )); do sleep 10; done
G=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++
T=${TREE:-$R}
$G -DNWII_PLATFORM_SWITCH=1 -D_longjmp=longjmp -D_setjmp=setjmp -I$T/export -I$T/nWiiRuntime/src -I$T/nWiiRuntime/include \
  -isystem /opt/devkitpro/portlibs/switch/include -isystem /opt/devkitpro/libnx/include \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -ffunction-sections -fdata-sections \
  -D__SWITCH__ -O2 -DNDEBUG -DNWII_FASTMEM=1 -ffp-contract=off -fomit-frame-pointer -fvisibility=hidden \
  -std=gnu++20 -fPIE -D_REENTRANT "$@" -c "$src" -o "$out" 2> "$out.err" &
gpid=$!
t0=$(date +%s); peak=0
while kill -0 $gpid 2>/dev/null; do
  for p in $(pgrep -P $gpid cc1plus); do
    h=$(awk '/VmHWM/{print $2}' /proc/$p/status 2>/dev/null); [[ -n $h ]] && ((h > peak)) && peak=$h
  done
  sleep 0.2
done
wait $gpid; rc=$?
echo "$(basename "$src") [$*] rc=$rc peak_rss=$((peak / 1024)) MB time=$(( $(date +%s) - t0 )) s"
