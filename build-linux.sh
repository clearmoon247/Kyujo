#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
#
# Builds Kyujo for Linux / Batocera on this computer. One command:   ./build-linux.sh
#
# What it does: checks the tools, fetches the decompilation (it is not part of this repository), makes the small C library the
# program needs (once), builds the program, and packs it, with the start script for Batocera's "Ports" menu, into
# build/Kyujo-Linux.zip. Your ROM is not needed to build; you add it when you play.
#
# Needs: a Linux PC (or WSL) with clang 18 or newer, lld, llvm (ar, ranlib, dlltool, readobj), git, make, python3. On Debian/Ubuntu:
#   sudo apt install clang lld llvm git make python3
# Settings (all optional):  KYUJO_DECOMP=<folder or zip of the decompilation>   KYUJO_WORK=<folder for the C library, default .work>
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. tools/build-lib.sh     # shared helpers (also fetches the decompilation)
OUT="$PWD/build/linux32"

fail() { echo; echo "Kyujo build stopped: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "'$1' is not installed. On Debian/Ubuntu: sudo apt install clang lld llvm git make python3"; }

echo "== 1/4  Checking the tools"
for t in clang ld.lld llvm-ar llvm-ranlib llvm-dlltool llvm-readobj git make python3; do need "$t"; done
ver="$(clang --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)"
[ "${ver:-0}" -ge 18 ] || fail "clang $ver is too old; clang 18 or newer is needed"
echo "   clang $ver, all tools found"

echo "== 2/4  Getting the decompilation (the game's source code, by Team Hippodrome)"
ensure_decomp

echo "== 3/4  Preparing the C library (first time only, a few minutes)"
if [ ! -d "$WORK/sysroot/musl-i386/lib" ]; then
    mkdir -p "$WORK"
    bash tools/toolchain/build_sysroots.sh "$WORK" musl-i386 || fail "the C library could not be built (needs internet access to github.com)"
else
    echo "   already done ($WORK)"
fi

echo "== 4/4  Building Kyujo (a few minutes)"
python3 tools/native/build_native.py --decomp "$DECOMP" --profile linux32 --sysroot "$WORK/sysroot/musl-i386" --out "$OUT" \
    || fail "the build failed; the message above says why"
python3 tools/native/package_linux.py --program "$OUT/kyujo" --probe "$OUT/kyujo_probe" --out build/Kyujo-Linux.zip

cat <<DONE

Done: build/Kyujo-Linux.zip
  * Batocera: unpack it, copy "Kyujo.sh" and the folder "kyujo" into roms/ports, copy your ROM into the folder "kyujo",
    then update the game list and start Kyujo from the Ports menu.
  * Other Linux: unpack it and run   ./kyujo/kyujo --rom /path/to/your-rom.z64
This program contains the game's code: it is for you; do not share it (see NOTICE.md).
DONE
