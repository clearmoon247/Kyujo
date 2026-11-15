# Windows setup (the game is built on the player's own computer)

*Players: you don't need this page. Download `Kyujo-Setup-Windows.zip` from the release page, unzip it, double-click `Setup.bat` ([`BUILDING.md`](../BUILDING.md#windows)). This page is for people who maintain the setup, want an offline copy, or want to know exactly what it does.*

**Why.** `kyujo.exe` contains the game's code (compiled from the decompilation) and about 0.66 MB of data tables that are byte-for-byte in the ROM. Handing that file around is the thing to avoid. The setup kit contains none of it: the player adds their own ROM, and the exe is built on their computer before the first start.

**Status:** every build step is verified on Linux with the same tools; `Setup.ps1` itself (the PowerShell around them) is the part that needs a run on Windows. What was checked, and what was not, is listed at the end.

## The two editions (same scripts, same steps)

| | Online edition | Offline edition |
|---|---|---|
| Contains | the port's own files only (build scripts, platform code, patches, setup): about 0.5 MB | the same, plus the compiler package (143 MB, unmodified upstream zip) and a zip of only the 1,160 decomp files the build reads (2.7 MB) |
| Needs internet | yes, on the first run, for two downloads from github.com | no |
| Contains decomp source files | **no** | **yes** (`downloads\decomp-<commit>-build-files.zip`) |
| May be published | **yes**: it is what the GitHub release carries (`Kyujo-Setup-Windows.zip`); the kit holds none of the decomp's files | **no**: the decomp has no stated licence. Private use only (`PRIVATE-DO-NOT-PUBLISH.txt` is in the kit). |

Neither edition contains a game program or anything extracted from a ROM. A build from "only the ROM and the build files" is not possible: the game's code is not in the ROM as source; it is the decompilation (a reconstruction written by hand over years). Only a static recompiler could derive code from a ROM, which is a different project.

Both editions look in `downloads\` first and download only what is missing and allowed, so an online kit can be turned into an offline run by putting the two files into `downloads\`.

## What the player does

1. Unpack the zip to a short folder (`C:\Kyujo`).
2. Double-click `Setup.bat` (or drop the ROM file on it). Choose the ROM.
3. Wait a few minutes. Start the game with `Play Kyujo`.

## What `Setup.ps1` does (and where the checks are)

1. Windows 10/11 64-bit; folder path at most 100 characters (the build makes files with names up to about 115 characters below `work\`; Windows stops at 260 in total).
2. ROM: file dialog; immediate size (33,554,432) and header check. After Python is available the full MD5 check (`tools/rom_tool.py check`) accepts only Pokémon Stadium (USA) v1.0; `.v64`/`.n64` files are converted once to `Game\kyujo-rom.z64`.
3. Compiler: `llvm-mingw-20240619-ucrt-x86_64.zip` (LLVM/clang 18.1.8, lld, mingw-w64 headers and libraries, Python 3.11) from `downloads\` or from github.com, **SHA-256 checked** (`setup/components.json`); unpacked to `work\tc` without the other CPU targets and the C++ headers (about 370 MB). The C headers are moved to `i686-w64-mingw32\include`, where the build looks for them.
4. Game source: the decomp at the pinned commit, from `downloads\` or from the official download address built from `setup/components.json` and the `source` line of `port/decomp-manifest.txt`. PowerShell unpacks only the 1,160 files the manifest lists into `work\decomp` (the compiler package's Python has **no zlib**, so Python cannot read a compressed zip; this was the first real failure, `No module named 'zlib'`, 2026-11-12). **Every file is checked against `port/decomp-manifest.txt`** (SHA-256); the setup stops and names the files that differ. A later run reuses `work\decomp` when it still matches.
5. Build: `python build_native.py --profile win32` with the packaged compiler, then `pe_check.py` on the result. Output goes to `work\logs\build.log`.
6. `Game\kyujo.exe`, `Game\kyujo.cfg` (`game_rom=`), a shortcut, and an offer to start the game.

The setup writes only inside its own folder, installs nothing, needs no administrator rights. The exe it builds has no "downloaded from the internet" mark, so SmartScreen has no reason to stop it.

## Making the kits

```
python3 tools/native/package_windows_setup.py --edition online  --out Kyujo-Setup-Windows.zip
python3 tools/native/package_windows_setup.py --edition offline --out Kyujo-Setup-offline.zip \
        --toolchain-zip llvm-mingw-20240619-ucrt-x86_64.zip --decomp <decomp checkout, folder or zip>
```

The kit is made from the files git tracks (commit first). `setup/kit-files.sha256` inside the kit lists every file's SHA-256; `setup/BUILD-INFO.txt` says which port commit it is.

## What was checked, and what was not

Checked on Linux (the build tools are the same on every host; only the scripts around them differ):
- A Linux program and a Windows exe built from a zip holding only the 1,160 manifest files, with `git` and `patch` replaced by programs that fail if run, are identical to the normal builds (the Windows exe is byte-identical between a zip and a checkout; the Linux program equals the shipped one).
- The same build under a strict-ASCII locale and in folders whose names contain spaces; long argument lists go through `@response` files; `tests/native/srctree/test_srctree.py` (17 tests) covers the patch applier and the source checks.
- A Windows exe built with the exact llvm-mingw release the setup downloads (its Linux-hosted twin, same version) passes `pe_check.py` (32-bit, GUI subsystem, no ASLR, imports only system DLLs).
- `tests/native/setup/test_setup_python.py`: every script the setup starts with that Python runs with `zlib`, `bz2`, `lzma`, `ssl`, `sqlite3`, `ctypes` made unimportable; the manifest-only unpack of a compressed zip verifies as a folder; a full win32 build with those modules blocked passes (2026-11-12).
- The unpacking rules (`tests/native/setup/toolchain_rules.py`): everything the build needs is present after unpacking the real zip with the setup's skip/map rules.
- The kit packager and the contents of both kits.

**Not checked (needs a Windows run):** `Setup.ps1` itself (no PowerShell was run; it was written to work in Windows PowerShell 5.1 and is plain ASCII); the Windows-hosted `clang.exe`/`ld.lld.exe`/`python.exe` of the llvm-mingw package (its Python 3.11 is a MinGW build; the setup tests that it can import what the build uses before using it: `hashlib`, `concurrent.futures`, `subprocess`, `json`, `struct`, `difflib`, `tempfile`, ... but not `zlib`, which it does not have); Windows Defender behaviour while compiling; the time a build takes on a real PC; and the resulting exe (built with a different mingw-w64 runtime than the one tested so far) on a real Windows 10/11.

Known limits: 64-bit Windows only for building (the game runs on any Windows 10/11 that can run 32-bit programs); a folder path with letters outside plain English was not tested.
