<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Dan B. Cooper -->
# Documentation

## For players

| Page | What it covers |
|---|---|
| [`../README.md`](../README.md) | What Kyujo is, how to get it, FAQ, known issues |
| [`using-kyujo.md`](using-kyujo.md) | The menu, keys, gamepads, Game Boy games, saves, cheats |
| [`../BUILDING.md`](../BUILDING.md) | Building for each system |
| [`windows-setup.md`](windows-setup.md) | How the Windows setup works and what it checks |
| [`../android/README.md`](../android/README.md) | The Android app |
| [`batocera.md`](batocera.md) | Batocera in detail, with a test program for window, pad and sound |

## For developers

| Page | What it covers |
|---|---|
| [`architecture.md`](architecture.md) | Decisions, targets, design, open problems |
| [`native-port-feasibility.md`](native-port-feasibility.md) | What was measured on the decompilation, and how to reproduce it |
| [`port-patches.md`](port-patches.md), [`../port/patches/`](../port/patches/) | Every change to the decompilation's source, with the reason |
| [`../port/decomp-manifest.txt`](../port/decomp-manifest.txt) | The exact decompilation files (path, size, SHA-256) the build reads, and their commit |
| [`known-decomp-issues.md`](known-decomp-issues.md) | Bugs and layout assumptions found in the decompilation |
| [`build-windows-linux.md`](build-windows-linux.md) | Every step of the native Windows and Linux build, and what each check proves |
| [`switch-android-plan.md`](switch-android-plan.md) | The Switch and Android design, decisions and measurements (long) |
| [`spec/behaviour-spec.md`](spec/behaviour-spec.md) | What the port must do, written as requirements |
| [`rdp-notes.md`](rdp-notes.md), [`move-trace.md`](move-trace.md) | Renderer notes; the move animation trace |
| [`test-checklist.md`](test-checklist.md), [`windows-test-guide.md`](windows-test-guide.md) | What was tested on real hardware, and the checklists used |
| [`../RELEASING.md`](../RELEASING.md) | How a release is made |
| [`../CONTRIBUTING.md`](../CONTRIBUTING.md) | How to contribute |

## How it works, in short

The decompiled C of the game, the libultra it was linked against, and the assembly the decompilation has not converted yet are compiled for a normal PC. Memory keeps the console's big-endian byte order and addresses (a pointer is the physical address), which is why the game's memory accesses are converted at build time (`tools/native/be_memory.py`). The console's hardware (video, audio, serial/PIF, flash, the RSP and RDP) is written by hand in `src/ultra/`, `src/rsp/` and `src/render/`; the window, input, audio and saves of each system are in `src/platform/` and `shell/`.

The decompilation itself is **not modified**. Where the original only works because of how the console's linker laid out memory, a small *port patch* (`port/patches/`, each starting with a plain-words explanation) is applied to a copy at build time. The patch files hold only the lines we add; the original's lines are named by hash (`tools/native/patchapply.py --explain` shows a patch as a full diff on your own copy).

## Repository layout

| Path | What it is |
|---|---|
| `LICENSE`, `REUSE.toml`, `NOTICE.md`, `THIRD-PARTY.md` | The licence (GPL-3.0-only for our code), how files are marked, what it does and doesn't cover, and every third-party component |
| `build-linux.sh`, `build-appimage.sh`, `build-android.sh`, `build-switch.sh` | One-command builds (Linux / Batocera zip, Linux AppImage, Android APK, Switch NRO); they share `tools/build-lib.sh` (pinned tool versions, downloads checked by hash) |
| `dist/windows-setup/` | The Windows setup (`Setup.bat`, `setup/`): the release's download |
| `src/platform/` | Host side: start-up, threads and timers, window and input, audio, saves, Transfer Pak |
| `src/ultra/` | The console's hardware interfaces |
| `src/rsp/`, `src/render/` | RSP interpreter and the software renderer |
| `src/common/`, `tests/` | Portable C++17 helpers (settings, stick-as-D-pad, `launcher.cfg`) and tests |
| `shell/` | The Switch and Android front end (SDL), the WebAssembly host and the GPU renderer |
| `android/`, `tools/android/` | The Android app and its builder |
| `tools/native/` | Measure the decompilation and build the programs (`build_native.py`, `build_shell.py`, `check_build.sh`, `srctree.py`, `patchapply.py`, `mkpatch.py`, packagers, checkers) |
| `tools/toolchain/` | Builds the cross C libraries (musl, mingw-w64, compiler-rt) from pinned sources |
| `tools/rom_tool.py` | Checks a game file against the known dumps, converts `.v64`/`.n64` to `.z64` |
| `port/` | The port patches and the manifest of the decompilation files |
