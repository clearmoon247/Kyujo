#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
# Builds the native program for both targets and checks what can be checked on a machine that cannot run Windows programs.
#
#   check_build.sh <decomp-dir> <sysroot-work-dir> <out-dir>
#
# <sysroot-work-dir> is the <work> directory given to tools/toolchain/build_sysroots.sh (it must contain sysroot/musl-i386 and
# sysroot/mingw-i686).
#
# Checks, and what each proves:
#   linux32  built, then RUN twice without the game's data: with no ROM, and with a file that is not the ROM. Both must be refused
#            (exit status 2). The scheduler self-test is then built and run (exit 0 and "PASSED").
#   win32    built, then inspected only: PE machine type, windows subsystem (the game: no console window; the test programs stay console), imports restricted to Windows 10 system DLLs,
#            and the absolute segment constant D_1002480 (0x01002480) must exist unchanged in the image and must NOT be covered by
#            a base relocation (otherwise Windows would move it when loading at another address).
#            NOT run: there is no Windows or Wine here. Run the .exe on Windows; it should print the same lines as linux32
#            and exit with status 70. The scheduler self-test is built for Windows too (tests/native/sched_test_*.c); on Windows
#            running sched.exe should print "SCHEDULER SELF-TEST PASSED" and exit with status 0.
set -euo pipefail

DECOMP="${1:?usage: check_build.sh <decomp-dir> <sysroot-work-dir> <out-dir>}"
WORK="${2:?}"
OUT="${3:?}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== linux32 ==="
python3 -I "$HERE/build_native.py" --decomp "$DECOMP" --profile linux32 --sysroot "$WORK/sysroot/musl-i386" --out "$OUT/linux32" | tail -3
# The program needs the player's ROM, so the automatic checks here are the refusals (no ROM, wrong ROM). Nothing is claimed about
# how far the game gets with a real ROM: that is reported separately (see docs/m3-status.md) and is not a pass/fail check.
set +e
"$OUT/linux32/kyujo" 2> "$OUT/linux32/run_norom.txt"; STATUS=$?
set -e
cat "$OUT/linux32/run_norom.txt"
[ "$STATUS" -eq 2 ] && grep -q "no ROM given" "$OUT/linux32/run_norom.txt" || { echo "FAIL: running without a ROM must exit 2 and say so (got $STATUS)"; exit 1; }
head -c 4096 /dev/urandom > "$OUT/linux32/not_a_rom.bin"
set +e
"$OUT/linux32/kyujo" --rom "$OUT/linux32/not_a_rom.bin" 2> "$OUT/linux32/run_badrom.txt"; STATUS=$?
set -e
cat "$OUT/linux32/run_badrom.txt"
[ "$STATUS" -eq 2 ] && ! grep -q "ROM accepted" "$OUT/linux32/run_badrom.txt" || { echo "FAIL: a wrong ROM must be refused with exit 2 (got $STATUS)"; exit 1; }
echo "linux32 OK: refuses to start without the right ROM"

echo "--- linux32 scheduler self-test (runs real libultra threads/messages/timer on the replacement layer)"
python3 -I "$HERE/build_test.py" --decomp "$DECOMP" --build-dir "$OUT/linux32" --profile linux32 --sysroot "$WORK/sysroot/musl-i386" --test sched \
    2>&1 | tee "$OUT/linux32/sched_test.txt" | tail -3
grep -q "SCHEDULER SELF-TEST PASSED" "$OUT/linux32/sched_test.txt" || { echo "FAIL: scheduler self-test"; exit 1; }

echo "=== win32 (static checks only) ==="
python3 -I "$HERE/build_native.py" --decomp "$DECOMP" --profile win32 --sysroot "$WORK/sysroot/mingw-i686" --out "$OUT/win32" | tail -3
python3 -I "$HERE/pe_check.py" "$OUT/win32/kyujo.exe" --gui --constant 0x01002480 --fragdata
python3 -I "$HERE/build_test.py" --decomp "$DECOMP" --build-dir "$OUT/win32" --profile win32 --sysroot "$WORK/sysroot/mingw-i686" --test sched --no-run | tail -1
python3 -I "$HERE/pe_check.py" "$OUT/win32/tests/sched/sched.exe"
echo "win32 built and statically checked; NOT run here"
