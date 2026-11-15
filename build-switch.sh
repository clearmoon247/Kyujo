#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
#
# Builds Kyujo for the Nintendo Switch (homebrew). One command:   ./build-switch.sh
#
# It fetches and builds the tools it needs (once, into .work), compiles the game, and writes build/kyujo.nro.
# The first run takes 30 to 90 minutes; later runs only rebuild the game (about 30 minutes).
# Your ROM is not needed to build; you copy it to the console, or choose it in the game's menu.
#
# Needs a Linux PC (or WSL) and, on Debian / Ubuntu, this one-time install:
#   sudo apt install git clang lld llvm cmake ninja-build make python3 python3-pil unzip curl wget
# devkitPro (the Switch toolchain) is installed by the script if it is missing; that asks for 'sudo' once.
# Settings (all optional): KYUJO_DECOMP=<decompilation folder or zip>  KYUJO_WORK=<tool folder>  KYUJO_JOBS=<n>  KYUJO_YES=1
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/tools/build-lib.sh"
STEP_TOTAL=5

step "Checking the tools"
need git clang llvm-ar llvm-nm llvm-ranlib cmake ninja make python3 curl
check_clang
note "all found; using $JOBS parallel jobs"

step "Getting the decompilation (the game's source code, by Team Hippodrome)"
ensure_decomp

step "Preparing the WebAssembly tools (first time only)"
ensure_wasi_sysroot; ensure_wabt; ensure_binaryen

step "Preparing devkitPro (the Switch toolchain)"
ensure_devkitpro

step "Building the Switch application"
build_wasm switch
note "translating to C and building the application (about 25 minutes; this is the slow part)"
python3 -I "$REPO/tools/native/build_shell.py" --wasm "$BUILD/switch-opt.wasm" --wabt "$WABT" --out "$BUILD/switch" --front sdl --target switch \
    --devkitpro "$DKP" --gpu --jobs "$JOBS" > "$LOG/switch.log" 2>&1 \
    || { tail -n 25 "$LOG/switch.log" >&2; fail "building the application failed; the whole log is $LOG/switch.log"; }
cp "$BUILD/switch/kyujo.nro" "$BUILD/kyujo.nro"

cat <<DONE

Done: build/kyujo.nro
  Copy it to the SD card of your Switch as   switch/kyujo/kyujo.nro   and start Kyujo from the Homebrew Menu.
  Put your game file next to it (switch/kyujo/), or choose it in the game's menu.
This program contains the game's code: it is for you; do not share it (see NOTICE.md).
DONE
