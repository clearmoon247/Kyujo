#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
# Builds the Windows program against the headers and libraries of the REAL llvm-mingw package (the one Setup.ps1 downloads), not against our own
# mingw-w64 sysroot. The two differ: our sysroot declared strndup(), the package's headers do not, so host_trace.c compiled here and failed on the
# first real Windows build (2026-11-12). Run it after any change to src/platform, src/render or tools/native.
#   tests/native/setup/build_against_package.sh <llvm-mingw-20240619-ucrt-x86_64.zip> <decomp> <work dir>
set -e
ZIP=$1; DECOMP=$2; W=$3
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$HERE/../../..
rm -rf "$W"; mkdir -p "$W"
python3 -I "$HERE/toolchain_rules.py" "$ZIP" --extract "$W/tc" > "$W/unpack.log"
# the Windows clang finds its compiler-rt next to its own headers; the Linux clang used here looks next to the sysroot, so put a copy there
cp "$W/tc/lib/clang/18/lib/windows/libclang_rt.builtins-i386.a" "$W/tc/i686-w64-mingw32/lib/libclang_rt.builtins-i686.a"
python3 -I "$ROOT/tools/native/build_native.py" --decomp "$DECOMP" --profile win32 --sysroot "$W/tc/i686-w64-mingw32" --out "$W/out"
python3 -I "$ROOT/tools/native/pe_check.py" "$W/out/kyujo.exe" --gui --befix --fragdata
echo "built against the llvm-mingw package: OK"
