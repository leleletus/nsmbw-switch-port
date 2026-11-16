#!/usr/bin/env bash
# pc_verify.sh <binary_input_test> <name> <secs> [extra env...]
#   PC run (offscreen, silent, private NAND mp) that goes title -> file select -> World 1 map -> 1-1
#   with the scripted input of prof.sh (lvl11), NWII_AXFX_VERIFY=40 by default; output pc/runs/<name>/.
#   Extra env: e.g. NWII_AXFX_VERIFY=20 NWII_AXFX_FAST=0. Runs inside nsmbw.slice (run.sh -> limc).
set -u
C=$(cd "$(dirname "$0")/.." && pwd)
bin=$1 name=$2 secs=$3; shift 3
export NWII_INPUT_PHASES='3>5' NWII_INPUT_SCRIPT_1='0-100000:100:20:4' \
       NWII_INPUT_SCRIPT_2='0-10000000:100:120:4;60-10000000:4:120:12' NWII_AXFX_VERIFY=${NWII_AXFX_VERIFY:-40}
for kv in "$@"; do export "$kv"; done
EVERY=${EVERY:-1200} PROFILE=${PROFILE:-free} RES=${RES:-1} "$C/pc/run.sh" "$bin" "$name" "$secs" mp '0-100000:100:20:4'
grep -h '\[R9\] axfx' "$C/pc/runs/$name/run.log" | tail -3
grep -h '\[R9\] axfx verify' "$C/pc/runs/$name/run.log" | grep -v ' ok=' | head -20
grep -m3 '\[SCENE\]' "$C/pc/runs/$name/run.log" | tail -2
