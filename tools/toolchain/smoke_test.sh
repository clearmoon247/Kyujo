#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
# Proves that a sysroot made by build_sysroots.sh can really produce a program, and checks the result as far as this machine can.
#
#   smoke_test.sh <sysroot-dir> <musl-i386|mingw-i686|mingw-x86_64> [out-dir]
#
# Each test program does a few things whose answers are known in advance (integer maths, 64-bit division through the compiler
# runtime library, printf, a sizeof check). It prints "SMOKE OK" only if all of them come out right.
#
#   musl-i386     a 32-bit static Linux program is built AND RUN here (a 64-bit Linux kernel runs 32-bit programs).
#   mingw-*       a Windows .exe is built and inspected with llvm-readobj (machine type, subsystem, imported DLLs, entry point),
#                 but NOT run: this machine cannot run Windows programs. Run the .exe on Windows to complete the check; it should
#                 print SMOKE OK and exit with status 0.
# Exit status: 0 when every check that could be done here passed.
set -euo pipefail

SYSROOT="${1:?usage: smoke_test.sh <sysroot-dir> <musl-i386|mingw-i686|mingw-x86_64> [out-dir]}"
TARGET="${2:?usage: smoke_test.sh <sysroot-dir> <musl-i386|mingw-i686|mingw-x86_64> [out-dir]}"
OUT="${3:-$(mktemp -d)}"
mkdir -p "$OUT"
RESDIR="$(clang --print-resource-dir)/include"

cat > "$OUT/smoke.c" <<'EOF'
/* Known-answer test for a freshly built toolchain. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* volatile: stop the compiler from folding these at compile time, so the run-time helper routines really are called. */
volatile uint64_t big = 0x123456789ABCDEF0ULL;
volatile uint64_t div = 1000003ULL;
volatile double   dbl = 2.5;

int main(void) {
    int bad = 0;
    uint64_t q = big / div, r = big % div;                       /* 64-bit divide/modulo: compiler-rt (__udivdi3 etc. on 32-bit) */
    if (q * div + r != big) { puts("FAIL: 64-bit divide"); bad++; }
    if (q != 0x123456789ABCDEF0ULL / 1000003ULL) { puts("FAIL: 64-bit quotient"); bad++; }
    if ((int)(dbl * 4.0) != 10) { puts("FAIL: double maths"); bad++; }
    char buf[32];
    snprintf(buf, sizeof buf, "%d-%s-%llu", 42, "ok", (unsigned long long)7);
    if (strcmp(buf, "42-ok-7") != 0) { puts("FAIL: snprintf"); bad++; }
    if (sizeof(void *) != (SMOKE_POINTER_BYTES)) { puts("FAIL: pointer size"); bad++; }
    if (bad == 0) puts("SMOKE OK");
    return bad;
}
EOF

case "$TARGET" in
musl-i386)
    CT=i386-linux-musl; PB=4
    clang --target=$CT -nostdinc -isystem "$RESDIR" -isystem "$SYSROOT/include" -DSMOKE_POINTER_BYTES=$PB -c "$OUT/smoke.c" -o "$OUT/smoke.o"
    # musl's start-up objects come first and last; the compiler runtime provides the 64-bit helpers.
    clang --target=$CT -fuse-ld=lld -nostdlib -static "$SYSROOT/lib/crt1.o" "$SYSROOT/lib/crti.o" "$OUT/smoke.o" \
        -L"$SYSROOT/lib" -lc "$SYSROOT/lib/libclang_rt.builtins-i386.a" "$SYSROOT/lib/crtn.o" -o "$OUT/smoke"
    llvm-readobj --file-headers "$OUT/smoke" | grep -E "Class:|DataEncoding:|Machine:|Type:"
    echo "--- running (32-bit static Linux program)"
    "$OUT/smoke" | tee "$OUT/run.txt"
    grep -qx "SMOKE OK" "$OUT/run.txt"
    ;;
mingw-i686|mingw-x86_64)
    if [ "$TARGET" = mingw-i686 ]; then CT=i686-w64-windows-gnu; PB=4; WANT=IMAGE_FILE_MACHINE_I386; RT=libclang_rt.builtins-i686.a
    else CT=x86_64-w64-windows-gnu; PB=8; WANT=IMAGE_FILE_MACHINE_AMD64; RT=libclang_rt.builtins-x86_64.a; fi
    clang --target=$CT -nostdinc -isystem "$RESDIR" -isystem "$SYSROOT/include" -DSMOKE_POINTER_BYTES=$PB -c "$OUT/smoke.c" -o "$OUT/smoke.o"
    # lld-link in MinGW mode: -lmingw32 etc. come from the sysroot; the C runtime start-up (crt2.o) is listed explicitly.
    clang --target=$CT -fuse-ld=lld -nostdlib -static-libgcc "$SYSROOT/lib/crt2.o" "$SYSROOT/lib/crtbegin.o" "$OUT/smoke.o" \
        -L"$SYSROOT/lib" -lmingw32 "$SYSROOT/lib/$RT" -lmingwex -lmsvcrt -lkernel32 -ladvapi32 -o "$OUT/smoke.exe" 2>"$OUT/link.log" || {
        echo "link failed:"; cat "$OUT/link.log"; exit 1; }
    llvm-readobj --file-headers "$OUT/smoke.exe" | grep -E "Machine:|Subsystem|MajorOperatingSystemVersion|AddressOfEntryPoint|ImageBase"
    llvm-readobj --file-headers "$OUT/smoke.exe" | grep -q "Machine: $WANT" || { echo "wrong machine type"; exit 1; }
    echo "--- imported DLLs (must all exist on a clean Windows 10/11):"
    llvm-readobj --coff-imports "$OUT/smoke.exe" | grep "Name:" | sort -u
    echo "built $OUT/smoke.exe — NOT run here. Run it on Windows: it should print SMOKE OK and exit 0."
    ;;
*) echo "unknown target '$TARGET'" >&2; exit 2 ;;
esac
