#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
# Builds a small cross-compilation "sysroot" (C library headers + libraries) so that clang can produce, and for Linux also run,
# 32-bit and 64-bit x86 programs on a machine that has clang/LLVM but no cross C library.
#
#   build_sysroots.sh <work-dir> musl-i386      32-bit Linux, static, musl C library   (runs on a 64-bit Linux kernel)
#   build_sysroots.sh <work-dir> mingw-i686     32-bit Windows, mingw-w64, UCRT        (needs Windows 10 or later to run)
#   build_sysroots.sh <work-dir> mingw-x86_64   64-bit Windows, mingw-w64, UCRT
#
# Result: <work-dir>/sysroot/<target>/{include,lib}. tools/toolchain/README.md explains how the build scripts use it.
#
# Everything is built from source revisions pinned below (exact commit hashes), so the same inputs give the same sysroot.
# Requirements: clang >= 18, lld, llvm-ar, llvm-ranlib, llvm-dlltool, git, make, python3. Network access to github.com (git only).
#
# Why not the distribution's packages: the development sandbox this project is built in cannot install packages, and the
# project's own build must not depend on whatever happens to be installed. Users who already have a MinGW or musl toolchain
# do not need this script.
set -euo pipefail

WORK="${1:?usage: build_sysroots.sh <work-dir> <musl-i386|mingw-i686|mingw-x86_64>}"
TARGET="${2:?usage: build_sysroots.sh <work-dir> <musl-i386|mingw-i686|mingw-x86_64>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-$(nproc)}"

# ---- Pinned source revisions -------------------------------------------------------------------------------------------
# LLVM 18.1.3 matches the clang 18.1.3 used to develop this; only compiler-rt/lib/builtins is fetched from it.
LLVM_URL=https://github.com/llvm/llvm-project;        LLVM_TAG=llvmorg-18.1.3
# mingw-w64 master as of 2026-10-24 (version macro __MINGW64_VERSION_MAJOR 15).
MINGW_URL=https://github.com/mingw-w64/mingw-w64;      MINGW_REV=7ce22f6637cc93fd2eb37c8a645908c92bb80bbd
# musl 1.2.6 plus later commits, from the kraj/musl mirror of the official repository.
MUSL_URL=https://github.com/kraj/musl;                 MUSL_REV=bd54dc3e8049b9c7db092905e743cdebbb8864f3

SRC="$WORK/src"; BUILD="$WORK/build"; SYSROOT="$WORK/sysroot/$TARGET"
mkdir -p "$SRC" "$BUILD" "$SYSROOT"

# Fetches exactly one commit of a repository (no history) into $SRC/<name> and checks it out.
fetch_rev() {   # name url rev-or-tag
    local name="$1" url="$2" rev="$3"
    if [ ! -d "$SRC/$name/.git" ]; then git init -q "$SRC/$name"; git -C "$SRC/$name" remote add origin "$url"; fi
    git -C "$SRC/$name" fetch -q --depth 1 origin "$rev"
    git -C "$SRC/$name" checkout -q FETCH_HEAD
}

# Only the compiler-rt builtins directory of LLVM is needed (a few hundred files, not the whole repository).
fetch_builtins() {
    if [ ! -d "$SRC/llvm-project/.git" ]; then
        git clone -q --depth 1 --filter=blob:none --sparse --branch "$LLVM_TAG" "$LLVM_URL" "$SRC/llvm-project"
        git -C "$SRC/llvm-project" sparse-checkout set compiler-rt/lib/builtins
    fi
    BUILTINS_SRC="$SRC/llvm-project/compiler-rt/lib/builtins"
}

case "$TARGET" in
musl-i386)
    fetch_rev musl "$MUSL_URL" "$MUSL_REV"
    fetch_builtins
    mkdir -p "$BUILD/musl-i386"; cd "$BUILD/musl-i386"
    CLANG_TARGET=i386-linux-musl
    # Step 1: headers only (the builtins below include <limits.h> etc. and must see musl's, not the host's).
    CC="clang --target=$CLANG_TARGET" AR=llvm-ar RANLIB=llvm-ranlib \
        "$SRC/musl/configure" --target=i386 --prefix="$SYSROOT" --disable-shared >/dev/null
    make install-headers >/dev/null
    # Step 2: the 64-bit integer / float helper routines (musl itself needs them; GCC would supply libgcc here).
    python3 -I "$HERE/build_builtins.py" "$BUILTINS_SRC" "$CLANG_TARGET" "$SYSROOT/lib/libclang_rt.builtins-i386.a" \
        --sysroot-include "$SYSROOT/include"
    # Step 3: the C library itself, told where the helper routines are.
    make distclean >/dev/null 2>&1 || true
    CC="clang --target=$CLANG_TARGET" AR=llvm-ar RANLIB=llvm-ranlib LIBCC="$SYSROOT/lib/libclang_rt.builtins-i386.a" \
        "$SRC/musl/configure" --target=i386 --prefix="$SYSROOT" --disable-shared >/dev/null
    make -j"$JOBS" >/dev/null
    make install >/dev/null
    ;;
mingw-i686|mingw-x86_64)
    fetch_rev mingw-w64 "$MINGW_URL" "$MINGW_REV"
    fetch_builtins
    if [ "$TARGET" = mingw-i686 ]; then
        HOST=i686-w64-mingw32;   CLANG_TARGET=i686-w64-windows-gnu;   LIBFLAGS="--enable-lib32 --disable-lib64"; RT=libclang_rt.builtins-i686.a
    else
        HOST=x86_64-w64-mingw32; CLANG_TARGET=x86_64-w64-windows-gnu; LIBFLAGS="--disable-lib32 --enable-lib64"; RT=libclang_rt.builtins-x86_64.a
    fi
    RESDIR="$(clang --print-resource-dir)/include"
    # Windows 10 (0x0A00) is the oldest Windows this project targets; UCRT is part of Windows 10 so no C runtime is shipped.
    mkdir -p "$BUILD/$TARGET-headers"; cd "$BUILD/$TARGET-headers"
    "$SRC/mingw-w64/mingw-w64-headers/configure" --host="$HOST" --prefix="$SYSROOT" --enable-sdk=all \
        --with-default-win32-winnt=0x0A00 --with-default-msvcrt=ucrt >/dev/null
    make install >/dev/null
    python3 -I "$HERE/build_builtins.py" "$BUILTINS_SRC" "$CLANG_TARGET" "$SYSROOT/lib/$RT" \
        --sysroot-include "$SYSROOT/include" --windows
    mkdir -p "$BUILD/$TARGET-crt"; cd "$BUILD/$TARGET-crt"
    # The C compiler is told not to look at the host's /usr/include and to use only clang's headers and the ones just installed.
    CC="clang --target=$CLANG_TARGET -nostdinc -isystem $RESDIR -isystem $SYSROOT/include -fuse-ld=lld" \
    AR=llvm-ar RANLIB=llvm-ranlib DLLTOOL=llvm-dlltool AS="clang --target=$CLANG_TARGET" \
        "$SRC/mingw-w64/mingw-w64-crt/configure" --host="$HOST" --prefix="$SYSROOT" --with-default-msvcrt=ucrt \
        $LIBFLAGS --disable-libarm32 --disable-libarm64 >/dev/null
    make -j"$JOBS" >/dev/null
    make install >/dev/null
    ;;
*)
    echo "unknown target '$TARGET'" >&2; exit 2 ;;
esac

echo "sysroot ready: $SYSROOT"
