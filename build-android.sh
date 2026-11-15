#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
#
# Builds the Kyujo Android app. One command:   ./build-android.sh
#
# It fetches and builds the tools it needs (once, into .work), compiles the game, and writes build/Kyujo-Android.apk.
# The first run takes 30 to 90 minutes and downloads about 2 GB; later runs only rebuild the game (about 20 minutes).
# Your ROM is not needed to build; you choose it in the app.
#
# Needs a Linux PC (or WSL) and, on Debian / Ubuntu, this one-time install:
#   sudo apt install git clang lld llvm cmake ninja-build make python3 python3-pil unzip curl default-jdk-headless
# Settings (all optional): KYUJO_DECOMP=<decompilation folder or zip>  KYUJO_WORK=<tool folder>  KYUJO_JOBS=<n>  KYUJO_YES=1 (accept the Android licence)
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/tools/build-lib.sh"
STEP_TOTAL=5

step "Checking the tools"
need git clang llvm-ar llvm-nm llvm-ranlib cmake ninja make python3 java javac keytool unzip curl
check_clang; check_python_pil
note "all found; using $JOBS parallel jobs"

step "Getting the decompilation (the game's source code, by Team Hippodrome)"
ensure_decomp

step "Preparing the WebAssembly tools (first time only)"
ensure_wasi_sysroot; ensure_wabt; ensure_binaryen

step "Preparing the Android tools (first time only)"
ensure_android

step "Building the app"
build_wasm android --define HOST_SHELL_PICKER
note "building the app (about 15 minutes)"
python3 "$REPO/tools/android/build_apk.py" --sdk "$ANDROID_SDK" --ndk "$ANDROID_NDK" --sdl "$SDL_SRC" --wasm "$BUILD/android-opt.wasm" --wabt "$WABT" \
    --out "$BUILD/android" --abis arm64-v8a x86_64 --jobs "$JOBS" --d8-jar "$R8_JAR" > "$LOG/apk.log" 2>&1 \
    || { tail -n 25 "$LOG/apk.log" >&2; fail "building the app failed; the whole log is $LOG/apk.log"; }
cp "$BUILD/android/kyujo-android.apk" "$BUILD/Kyujo-Android.apk"

cat <<DONE

Done: build/Kyujo-Android.apk
  Copy it to your phone and open it (allow "install unknown apps" for the app you open it from, if Android asks), or with a cable:
      adb install build/Kyujo-Android.apk
  Then open Kyujo, tap "Choose the Pokemon Stadium ROM", pick your game file, and tap Play.
This app contains the game's code: it is for you; do not share it (see NOTICE.md).
DONE
