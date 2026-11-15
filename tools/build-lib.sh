#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
#
# Shared by build-android.sh and build-switch.sh (it is sourced, not run). It fetches and builds, once, the tools those builds need:
# every source is taken at ONE pinned commit (git checks the commit, so what you get is exactly what was tested), and the two
# downloaded files are checked against a fixed SHA-256. Everything goes into $KYUJO_WORK (default: .work in the repository).
#
# Settings (all optional):
#   KYUJO_WORK=<folder>       where the tools are kept (default: .work)
#   KYUJO_DECOMP=<folder|zip> a copy of the decompilation you already have (default: the ext/decomp submodule)
#   KYUJO_JOBS=<n>            parallel jobs (default: from your CPU count and free memory)
#   KYUJO_YES=1               answer "yes" to the questions (licence, installing devkitPro)

# ---- pinned versions ---------------------------------------------------------------------------------------------------
DECOMP_URL=https://github.com/team-hippodrome/pokestadium; DECOMP_REV=8efced606b389f8ff2793ad52beddf60aba213ab   # same commit as ext/decomp
WABT_URL=https://github.com/WebAssembly/wabt;            WABT_REV=98e3a78739833015c703a749725aade0dc053211
BINARYEN_URL=https://github.com/WebAssembly/binaryen;    BINARYEN_REV=95a214dc48f2ed57772f4389b905b1c009633b44
WASI_URL=https://github.com/WebAssembly/wasi-libc;       WASI_REV=ce2ee9fb7e39c20ae266d294dd2d0afd1bf1f8e2
LLVM_URL=https://github.com/llvm/llvm-project;           LLVM_TAG=llvmorg-18.1.3            # only compiler-rt's builtins are used
SDL_URL=https://github.com/libsdl-org/SDL;               SDL_REV=5d249570393f7a37e037abf22cd6012a4cc56a71   # release-2.32.10
CMDTOOLS_URL=https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
CMDTOOLS_SHA256=2d2d50857e4eb553af5a6dc3ad507a17adf43d115264b1afc116f95c92e5e258
R8_URL=https://maven.google.com/com/android/tools/r8/8.13.19/r8-8.13.19.jar
R8_SHA256=7712edbae6a71f35937ffc1bd5f4c202919ef2a5e58d3c3544015a868a3cf457
ANDROID_PLATFORM_PKG="platforms;android-34"; ANDROID_BUILD_TOOLS_PKG="build-tools;34.0.0"; ANDROID_NDK_VERSION=27.3.13750724

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${KYUJO_WORK:-$REPO/.work}"
TOOLS="$WORK/tools"
BUILD="$REPO/build"
mkdir -p "$TOOLS" "$BUILD"
LOG="$WORK/logs"; mkdir -p "$LOG"
HINT_PKGS="git clang lld llvm cmake ninja-build make python3 python3-pil unzip curl default-jdk-headless"

# ---- small helpers -----------------------------------------------------------------------------------------------------
if [ "$(id -u)" = "0" ] && [ -z "${KYUJO_ALLOW_ROOT:-}" ]; then
    echo >&2; echo "Kyujo build stopped: do not run this with sudo (or as root)." >&2
    echo "   Run it as your normal user:  ./$(basename "$0")   The script asks for sudo itself, once, only when it has to install something." >&2
    exit 1
fi
STEP_N=0
step() { STEP_N=$((STEP_N + 1)); echo; echo "== [$STEP_N/$STEP_TOTAL] $*"; }
note() { echo "   $*"; }
fail() { echo; echo "Kyujo build stopped: $*" >&2; exit 1; }
yes_or_stop() {     # question; KYUJO_YES=1 answers yes
    [ "${KYUJO_YES:-}" = "1" ] && return 0
    [ -t 0 ] || fail "$1 (not asked: no keyboard. Run again with KYUJO_YES=1 to answer yes.)"
    read -r -p "   $1 [y/N] " a; case "$a" in y|Y|yes|YES) return 0 ;; *) fail "stopped, as you answered no" ;; esac
}
# Runs a command quietly into a log file; on failure shows the end of the log.
quiet() {           # logname command...
    local name="$1"; shift
    if ! "$@" > "$LOG/$name.log" 2>&1; then
        echo "   the command failed: $*" >&2; tail -n 25 "$LOG/$name.log" >&2
        fail "$name failed; the whole log is $LOG/$name.log"
    fi
}

jobs_default() {
    local n mem
    n="$(nproc 2>/dev/null || echo 2)"
    mem="$(awk '/MemAvailable/ {print int($2/1024/1024)}' /proc/meminfo 2>/dev/null || echo 4)"   # GB
    local by_mem=$(( mem / 2 )); [ "$by_mem" -lt 1 ] && by_mem=1
    [ "$by_mem" -lt "$n" ] && n="$by_mem"
    echo "$n"
}
JOBS="${KYUJO_JOBS:-$(jobs_default)}"

need() {
    local missing=""
    for t in "$@"; do command -v "$t" >/dev/null 2>&1 || missing="$missing $t"; done
    [ -z "$missing" ] || fail "these programs are not installed:$missing
   On Debian / Ubuntu / WSL, one command installs everything the build needs:
       sudo apt install $HINT_PKGS"
}
check_clang() {
    local ver; ver="$(clang --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)"
    [ "${ver:-0}" -ge 18 ] || fail "clang $ver is too old; clang 18 or newer is needed (sudo apt install clang lld llvm)"
    echo 'int main(void){return 0;}' > "$LOG/wasm-probe.c"
    clang --target=wasm32-wasi -c -nostdinc -ffreestanding "$LOG/wasm-probe.c" -o "$LOG/wasm-probe.o" 2>/dev/null \
        || fail "this clang cannot produce WebAssembly code. Install a clang with the wasm32 target (the Debian/Ubuntu clang has it): sudo apt install clang lld llvm"
    command -v wasm-ld >/dev/null 2>&1 || command -v wasm-ld-"$ver" >/dev/null 2>&1 || fail "wasm-ld is missing: sudo apt install lld"
}
check_python_pil() { python3 -c "import PIL" 2>/dev/null || fail "Python's Pillow is missing: sudo apt install python3-pil   (or: pip install pillow)"; }

# Exactly one commit of a repository, no history, into <dir>. Stamp file: done when the right commit is already there.
fetch_rev() {       # dir url rev
    local dir="$1" url="$2" rev="$3"
    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$rev" ]; then return 0; fi
    rm -rf "$dir"; mkdir -p "$dir"
    git init -q "$dir"; git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$rev" || fail "could not download $url (needs internet access to github.com)"
    git -C "$dir" checkout -q FETCH_HEAD
    [ "$(git -C "$dir" rev-parse HEAD)" = "$rev" ] || fail "$url did not give the expected commit $rev"
}
download_checked() {   # url file sha256
    local url="$1" file="$2" want="$3"
    if [ -f "$file" ] && [ "$(sha256sum "$file" | cut -d' ' -f1)" = "$want" ]; then return 0; fi
    mkdir -p "$(dirname "$file")"
    curl -fL --retry 3 -sS -o "$file.part" "$url" || fail "could not download $url"
    [ "$(sha256sum "$file.part" | cut -d' ' -f1)" = "$want" ] || { rm -f "$file.part"; fail "the file from $url is not the expected one (SHA-256 differs); it was not used"; }
    mv "$file.part" "$file"
}

# ---- the decompilation -------------------------------------------------------------------------------------------------
ensure_decomp() {
    DECOMP="${KYUJO_DECOMP:-}"
    if [ -z "$DECOMP" ]; then
        if [ -e "$REPO/.git" ]; then      # a git clone: the decompilation is the submodule ext/decomp
            if [ ! -e "$REPO/ext/decomp/.git" ]; then
                git -C "$REPO" submodule update --init ext/decomp || fail "could not download the decompilation (needs internet access to github.com)"
            fi
            DECOMP="$REPO/ext/decomp"
        else                              # a downloaded zip of the source has no git data: fetch the same commit directly
            note "downloading the decompilation (about 45 MB, once)"
            fetch_rev "$TOOLS/src/decomp" "$DECOMP_URL" "$DECOMP_REV"
            DECOMP="$TOOLS/src/decomp"
        fi
    fi
    note "using $DECOMP (every file the build reads is checked against port/decomp-manifest.txt)"
}

# ---- WebAssembly tools: the C library, wasm2c, wasm-opt ---------------------------------------------------------------
ensure_wasi_sysroot() {
    WASI_SYSROOT="$TOOLS/wasi-sysroot"
    [ -f "$WASI_SYSROOT/.done-$WASI_REV" ] && { note "the WebAssembly C library: already done"; return 0; }
    note "making the WebAssembly C library (a few minutes, once)"
    fetch_rev "$TOOLS/src/wasi-libc" "$WASI_URL" "$WASI_REV"
    rm -rf "$WASI_SYSROOT" "$TOOLS/build/wasi-libc"
    quiet wasi-libc-configure cmake -S "$TOOLS/src/wasi-libc" -B "$TOOLS/build/wasi-libc" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_AR="$(command -v llvm-ar)" -DCMAKE_NM="$(command -v llvm-nm)" -DCMAKE_RANLIB="$(command -v llvm-ranlib)" \
        -DTARGET_TRIPLE=wasm32-wasi -DBUILD_TESTS=OFF -DSETJMP=ON -DCMAKE_INSTALL_PREFIX="$WASI_SYSROOT"
    quiet wasi-libc-build cmake --build "$TOOLS/build/wasi-libc" --parallel "$JOBS"
    quiet wasi-libc-install cmake --install "$TOOLS/build/wasi-libc"
    # the compiler support routines (64-bit and long-double helpers) that WebAssembly code needs, from LLVM's own sources
    if [ ! -d "$TOOLS/src/llvm-project/.git" ]; then
        quiet llvm-fetch git clone -q --depth 1 --filter=blob:none --sparse --branch "$LLVM_TAG" "$LLVM_URL" "$TOOLS/src/llvm-project"
        git -C "$TOOLS/src/llvm-project" sparse-checkout set compiler-rt/lib/builtins
    fi
    quiet builtins python3 -I "$REPO/tools/toolchain/build_builtins.py" "$TOOLS/src/llvm-project/compiler-rt/lib/builtins" wasm32-wasi \
        "$WASI_SYSROOT/lib/wasm32-wasi/libclang_rt.builtins-wasm32.a" --sysroot-include "$WASI_SYSROOT/include/wasm32-wasi"
    touch "$WASI_SYSROOT/.done-$WASI_REV"
}

ensure_wabt() {
    WABT="$TOOLS/src/wabt"
    if [ -x "$WABT/build/wasm2c" ] && [ -x "$WABT/build/wasm-validate" ] && [ "$(git -C "$WABT" rev-parse HEAD 2>/dev/null)" = "$WABT_REV" ]; then
        note "wasm2c: already done"; return 0
    fi
    note "building wasm2c (the WebAssembly-to-C translator, a few minutes, once)"
    fetch_rev "$WABT" "$WABT_URL" "$WABT_REV"
    quiet wabt-submodules git -C "$WABT" submodule update --init --depth 1 third_party/picosha2 third_party/simde
    quiet wabt-configure cmake -S "$WABT" -B "$WABT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DWITH_EXCEPTIONS=OFF
    quiet wabt-build cmake --build "$WABT/build" --parallel "$JOBS" --target wasm2c wasm-validate
}

ensure_binaryen() {
    BINARYEN="$TOOLS/src/binaryen"
    if [ -x "$BINARYEN/build/bin/wasm-opt" ] && [ "$(git -C "$BINARYEN" rev-parse HEAD 2>/dev/null)" = "$BINARYEN_REV" ]; then
        note "wasm-opt: already done"; return 0
    fi
    note "building wasm-opt (the WebAssembly optimiser; this is the longest tool, 5 to 20 minutes, once)"
    fetch_rev "$BINARYEN" "$BINARYEN_URL" "$BINARYEN_REV"
    quiet binaryen-configure cmake -S "$BINARYEN" -B "$BINARYEN/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
    quiet binaryen-build cmake --build "$BINARYEN/build" --parallel "$JOBS" --target wasm-opt
}

# ---- the game as WebAssembly, then optimised ---------------------------------------------------------------------------
build_wasm() {      # name [extra build_native.py arguments...]    -> $BUILD/<name>-opt.wasm
    local name="$1"; shift
    note "compiling the game to WebAssembly (about 5 minutes)"
    mkdir -p "$BUILD/$name"
    python3 -I "$REPO/tools/native/build_native.py" --decomp "$DECOMP" --profile wasm32 --sysroot "$WASI_SYSROOT" --out "$BUILD/$name" \
        --opt O0 --jobs "$JOBS" "$@" > "$LOG/wasm-$name.log" 2>&1 || { tail -n 25 "$LOG/wasm-$name.log" >&2; fail "compiling the game failed; the whole log is $LOG/wasm-$name.log"; }
    note "optimising it (a few minutes)"
    python3 -I "$REPO/tools/native/optimize_wasm.py" --in "$BUILD/$name/kyujo.wasm" --out "$BUILD/$name-opt.wasm" \
        --wasm-opt "$BINARYEN/build/bin/wasm-opt" --wabt "$WABT" --function-map "$BUILD/$name-fnmap.txt" > "$LOG/optimise-$name.log" 2>&1 \
        || { tail -n 25 "$LOG/optimise-$name.log" >&2; fail "optimising failed; the whole log is $LOG/optimise-$name.log"; }
}

# ---- Android tools -----------------------------------------------------------------------------------------------------
ensure_android() {
    ANDROID_SDK="$TOOLS/android-sdk"
    ANDROID_NDK="$ANDROID_SDK/ndk/$ANDROID_NDK_VERSION"
    SDL_SRC="$TOOLS/src/SDL"
    R8_JAR="$TOOLS/r8-8.13.19.jar"
    fetch_rev "$SDL_SRC" "$SDL_URL" "$SDL_REV"
    download_checked "$R8_URL" "$R8_JAR" "$R8_SHA256"
    if [ -f "$ANDROID_NDK/build/cmake/android.toolchain.cmake" ] && [ -d "$ANDROID_SDK/platforms/android-34" ] && [ -d "$ANDROID_SDK/build-tools/34.0.0" ]; then
        note "Android SDK and NDK: already done"; return 0
    fi
    echo "   The Android SDK and NDK are Google's, under their licence (https://developer.android.com/studio/terms)."
    yes_or_stop "Download them (about 1.5 GB) and accept that licence?"
    note "downloading the Android command-line tools"
    download_checked "$CMDTOOLS_URL" "$TOOLS/cmdline-tools.zip" "$CMDTOOLS_SHA256"
    rm -rf "$ANDROID_SDK/cmdline-tools"; mkdir -p "$ANDROID_SDK/cmdline-tools"
    quiet unzip-cmdtools unzip -q -o "$TOOLS/cmdline-tools.zip" -d "$ANDROID_SDK/cmdline-tools"
    mv "$ANDROID_SDK/cmdline-tools/cmdline-tools" "$ANDROID_SDK/cmdline-tools/latest"
    local sdkm="$ANDROID_SDK/cmdline-tools/latest/bin/sdkmanager"
    note "downloading the platform, build tools and NDK (a few minutes)"
    yes | "$sdkm" --sdk_root="$ANDROID_SDK" --licenses > "$LOG/sdk-licenses.log" 2>&1 || true
    quiet sdkmanager "$sdkm" --sdk_root="$ANDROID_SDK" "$ANDROID_PLATFORM_PKG" "$ANDROID_BUILD_TOOLS_PKG" "ndk;$ANDROID_NDK_VERSION"
    [ -f "$ANDROID_NDK/build/cmake/android.toolchain.cmake" ] || fail "the Android NDK did not install; see $LOG/sdkmanager.log"
}

# ---- devkitPro (Nintendo Switch) ---------------------------------------------------------------------------------------
dkp_ok() {          # folder -> 0 if it holds everything the Switch build needs
    local d="$1"
    [ -x "$d/devkitA64/bin/aarch64-none-elf-gcc" ] && [ -f "$d/libnx/switch.specs" ] && [ -x "$d/tools/bin/elf2nro" ] && [ -x "$d/tools/bin/nacptool" ] \
        && [ -f "$d/portlibs/switch/include/SDL2/SDL.h" ] && [ -f "$d/portlibs/switch/lib/libSDL2.a" ] && [ -f "$d/portlibs/switch/lib/libEGL.a" ] \
        && [ -f "$d/portlibs/switch/lib/libGLESv2.a" ] && [ -f "$d/portlibs/switch/lib/libglapi.a" ] && [ -f "$d/portlibs/switch/lib/libdrm_nouveau.a" ]
}
DKP_PACKAGES="switch-dev switch-sdl2 switch-mesa switch-libdrm_nouveau"
ensure_devkitpro() {
    DKP="${DEVKITPRO:-/opt/devkitpro}"
    if dkp_ok "$DKP"; then note "devkitPro: found in $DKP"; return 0; fi
    echo "   devkitPro (the Switch homebrew toolchain) is not installed here, or is missing a package."
    if ! command -v dkp-pacman >/dev/null 2>&1; then
        echo "   It is installed system-wide into /opt/devkitpro with devkitPro's own installer (Debian / Ubuntu / WSL), which needs 'sudo' once."
        yes_or_stop "Install devkitPro now?"
        command -v wget >/dev/null 2>&1 || fail "wget is missing: sudo apt install wget"
        quiet dkp-installer-download wget -O "$WORK/install-devkitpro-pacman" https://apt.devkitpro.org/install-devkitpro-pacman
        chmod +x "$WORK/install-devkitpro-pacman"
        sudo "$WORK/install-devkitpro-pacman" || fail "devkitPro's installer failed; its instructions are at https://devkitpro.org/wiki/devkitPro_pacman"
    else
        yes_or_stop "Install the missing devkitPro packages ($DKP_PACKAGES) with sudo?"
    fi
    sudo dkp-pacman -S --needed --noconfirm $DKP_PACKAGES || fail "could not install $DKP_PACKAGES with dkp-pacman"
    DKP=/opt/devkitpro
    dkp_ok "$DKP" || fail "devkitPro is installed but something is still missing in $DKP (SDL2, EGL, GLESv2, glapi, libdrm_nouveau, libnx, elf2nro). Run: dkp-pacman -Ss switch- | less"
}
