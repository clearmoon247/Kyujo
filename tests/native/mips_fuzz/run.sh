#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
# Differential test of the MIPS interpreters on random programs (a few hundred thousand): the module's reference interpreter, its per-instruction
# interpreter, its block interpreter and the shell's native block interpreter (shell/core/mips_native.c) must give the same hash for every program.
# The memory the interpreted code uses must lie below 16 MiB, so the harnesses are linked at low addresses (x86-64 Linux, gcc).
set -e
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../../.." && pwd); OUT=${OUT:-/tmp/mips_fuzz}; N=${1:-100000}
mkdir -p "$OUT"
LF="-Wl,-Ttext-segment=0x100000 -Wl,--section-start=.dat=0x800000 -Wl,--section-start=.pool=0x801000 -Wl,--section-start=.stk=0xA00000"
gcc -O1 -no-pie -fno-pie -I"$REPO/src/platform" -I"$REPO/shell/core" -I"$HERE" $LF -o "$OUT/fuzz_wasm_side" "$HERE/fuzz_wasm_side.c"
gcc -O2 -no-pie -fno-pie -I"$REPO/src/platform" -I"$REPO/shell/core" -I"$HERE" $LF -o "$OUT/fuzz_native_side" "$HERE/fuzz_native_side.c"
a=$(KYUJO_MIPS_REF=1 "$OUT/fuzz_wasm_side" $N 2>/dev/null | md5sum)
b=$(KYUJO_MIPS_BLOCKS=0 "$OUT/fuzz_wasm_side" $N 2>/dev/null | md5sum)
c=$("$OUT/fuzz_wasm_side" $N 2>/dev/null | md5sum)
d=$("$OUT/fuzz_native_side" $N 2>/dev/null | md5sum)
echo "reference $a"; echo "per-instruction $b"; echo "blocks $c"; echo "native blocks $d"
[ "$a" = "$b" ] && [ "$a" = "$c" ] && [ "$a" = "$d" ] && echo "ALL EQUAL ($N programs)" || { echo "DIFFERENT"; exit 1; }
