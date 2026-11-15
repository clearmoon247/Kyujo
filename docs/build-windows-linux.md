<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Dan B. Cooper -->
# Building the Windows and Linux ports (developer reference)

*Just want to build? See [`BUILDING.md`](../BUILDING.md): `./build-linux.sh` on Linux, the setup on Windows. This page is the step-by-step reference behind them.*

This page collects what a developer needs to build the **native Windows (32-bit .exe)** and **native Linux (32-bit static, for Batocera and other Linux)** programs from this repository. It is a map to the existing scripts, not a new build system. The Nintendo Switch / Android shell (`shell/`, the WebAssembly pipeline) is separate and is not needed for either of these.

## What is in the repository and what is not

In: our own source (`src/`, `tools/`, `tests/`, `port/`, `docs/`, `dist/`, `assets/`), licensed GPL-3.0-only (`LICENSE`, `NOTICE.md`, `THIRD-PARTY.md`).
Not in, by design:
- **The decompilation.** It is a git submodule (`ext/decomp`, Team Hippodrome, pinned commit `8efced6`). It has no stated licence: do not copy it into this repository, do not publish a package that contains it. The build also accepts a plain folder or a zip of it, checked file by file against `port/decomp-manifest.txt`.
- **The game ROM, anything made from it, Game Boy ROMs and saves.** The programs read the player's ROM when they run; nothing is stored. `.gitignore` blocks the usual extensions.
- **Built programs** (`kyujo`, `kyujo.exe`, the setup kit's "offline" edition). They are compiled from the decompilation, so they contain the game's code: keep them private, never commit or publish them.

## 1. Tools

| Need | For |
|---|---|
| Linux x86-64 build machine (or WSL) | the developer build of both targets |
| clang >= 18 and lld, llvm-ar, llvm-ranlib, llvm-dlltool, llvm-readobj, git, make, Python 3.8+ | everything |
| network access to github.com (git only) | fetching the pinned sources of the C libraries |

Python scripts use the standard library only.

## 2. Once: the C libraries for the two targets

```
git submodule update --init ext/decomp             # or pass --decomp <folder-or-zip> below
tools/toolchain/build_sysroots.sh <work> musl-i386       # 32-bit Linux (static musl)
tools/toolchain/build_sysroots.sh <work> mingw-i686      # 32-bit Windows (mingw-w64, UCRT)
```
Sources are pinned by commit in the script (`tools/toolchain/README.md` explains each step). Takes a few minutes.

## 3. Build

```
python3 tools/native/build_native.py --decomp ext/decomp --profile linux32 --sysroot <work>/sysroot/musl-i386  --out build/linux32
python3 tools/native/build_native.py --decomp ext/decomp --profile win32   --sysroot <work>/sysroot/mingw-i686 --out build/win32
```
Or both, with every check that can run without Windows:
```
tools/native/check_build.sh ext/decomp <work> build        # linux32: runs with no ROM and with a wrong file (both must be refused), scheduler self-test
                                                                  # win32:   PE type, GUI subsystem, imports, the 0x01002480 constant is not relocated
```
`build_native.py` also applies the port patches (`port/patches/`, explained in `docs/port-patches.md`) to a copy of the decomp, converts the game's memory accesses to the console's byte order (`be_memory.py`), compiles, links, and fails the build if the program image grows past 16 MiB minus 1.75 MiB (`imagelimit.py`: addresses at or above 0x01000000 would be taken for segment numbers). **If a new static table pushes the image over that limit, allocate it at run time instead** (this happened once: the MIPS interpreter's tables, now allocated on first use). **Exception: the MIPS interpreter's stacks (128 KiB) must stay static, inside the image**: the interpreted code's stack pointer is their address, and the interpreter only accepts load/store addresses below 16 MiB. They were once allocated at run time (commit 65261eb) and GB Tower then stopped with "a load or store outside game memory" on Windows (2026-11-12); `mips_mem()` now checks their address. The linux32 image therefore has only about 2.0 MiB of room (the check requires 1.75 MiB): **do not add static data to the native image without checking `imagelimit.py`**.

Outputs: `build/linux32/kyujo` (+ `kyujo_probe`, a test program for window, pads and sound) and `build/win32/kyujo.exe`.

## 4. Run

- Linux: `./kyujo --rom <your Pokemon Stadium (USA) .z64>` (needs a 64-bit Linux with the 32-bit loader, X11, PulseAudio-protocol sound server, evdev pads; no 32-bit libraries). With no ROM it exits with status 2 and says so. `tools/linux/` has the Batocera launcher and the probe; `docs/batocera.md` is the tester's guide.
- Windows: double-click `kyujo.exe` (file dialog for the ROM; settings and log next to it). `docs/using-kyujo.md` is the player's guide.

## 5. Packaging (private: the packages contain the game's code)

```
python3 tools/native/package_linux.py   --program build/linux32/kyujo --probe build/linux32/kyujo_probe --out <zip>   # PRIVATE: contains the game's code
python3 tools/native/package_windows.py --exe build/win32/kyujo.exe --out <zip>                                            # PRIVATE: contains the game's code
```

## 6. Windows setup kit (the exe is built on the player's computer)

```
python3 tools/native/package_windows_setup.py --edition online --out Kyujo-Setup-Windows.zip
```
The online edition holds only our files (about 0.5 MB). On the player's PC `Setup.bat` downloads the pinned llvm-mingw compiler (SHA-256 checked) and the decomp at the pinned commit (every file checked against the manifest), builds `kyujo.exe` next to the player's ROM, and offers to start it. See `docs/windows-setup.md`. The "offline" edition bundles the decomp's files and is for the maker's own use only.

## 7. Tests

```
cmake -S . -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host --output-on-failure   # portable C++ helpers
python3 tests/native/srctree/test_srctree.py                                                                          # patch applier and source checks
```
More runners under `tests/native/` (x11, pulse, evdev, render, rsp, ...) are described in the comments at the top of each.

## 8. What was verified for this snapshot (2026-11-11)

Built from a clean unpack of `git archive` (no working-tree files), decomp at `8efced6` from a checkout, sysroots from `build_sysroots.sh`, with `tools/native/check_build.sh`:
- **linux32**: builds (image 13.87 MiB, 2.01 MiB of room below the 16 MiB limit), refuses to start without the right ROM (exit 2) and with a wrong file, scheduler self-test passes (22 checks). Run on a virtual X server with a real ROM: the game starts, and the Game Pak Setup page appears (windowed, 640x480).
- **win32**: builds (13.69 MiB, 2.18 MiB of room), passes the static checks (32-bit x86, GUI subsystem, system DLL imports only, the 0x01002480 constant intact and not relocated). **Not run** here (no Windows).
- The differential test of the MIPS interpreters (reference, per-instruction and block interpreter, `tests/native/mips_fuzz`, wasm side) gives equal results on 20,000 random programs after the table change. Its native-shell half (`fuzz_native_side.c`) no longer builds (it predates the shell's current `mips_native.c`): a known stale test, unrelated to the Windows/Linux programs.
- **Found and fixed while preparing this snapshot**: the Linux image had grown to 16.40 MiB (over the limit, build failed) because of the MIPS interpreter's block cache (about 2.4 MiB of static tables) and a 160 KiB debug table. They are now allocated on first use (`host_mips.c`, `rdp.c`). The GB Tower path that uses them was not run (no Game Boy ROM on the build machine).

Not verified here: anything on real Windows (including `Setup.ps1`), real gamepads on Batocera, GB Tower after the table change, the setup kit's downloads. Earlier test rounds are recorded in `docs/test-checklist.md`, `docs/windows-test-guide.md` and `docs/batocera.md`.
