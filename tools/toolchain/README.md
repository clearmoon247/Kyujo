# Cross toolchain for the native Windows and Linux builds

The port is developed on a machine that has **clang/LLVM, lld and git but no cross C library** and cannot install packages. These scripts build what is missing, from pinned source revisions, so the result is reproducible and every step can be read.

| Script | What it does |
|---|---|
| `build_sysroots.sh <work> musl-i386` | 32-bit Linux C library (musl, static) → programs that run on any 64-bit Linux kernel |
| `build_sysroots.sh <work> mingw-i686` | 32-bit Windows headers and import libraries (mingw-w64, UCRT) |
| `build_sysroots.sh <work> mingw-x86_64` | 64-bit Windows, same |
| `build_builtins.py` | Compiles compiler-rt's `builtins` (64-bit divide, float conversions…) from the LLVM 18.1.3 source list; called by the script above |
| `smoke_test.sh <sysroot> <name> [out]` | Builds a known-answer program with that sysroot and checks it (see below) |

Requirements: clang ≥ 18, lld, `llvm-ar`, `llvm-ranlib`, `llvm-dlltool`, `llvm-readobj`, git (network access to github.com), make, Python 3.8+.
Pinned sources: musl `bd54dc3e…`, mingw-w64 `7ce22f66…`, LLVM `llvmorg-18.1.3` (full hashes in `build_sysroots.sh`). Licences: see `NOTICE.md`.

```
tools/toolchain/build_sysroots.sh  work  musl-i386
tools/toolchain/smoke_test.sh      work/sysroot/musl-i386  musl-i386
```

## What the smoke test proves, and what it does not

| Target | Built | Run on the build machine | Checked statically |
|---|---|---|---|
| `musl-i386` | yes | **yes** (a 64-bit Linux kernel runs 32-bit programs); must print `SMOKE OK` | ELF class 32-bit, little-endian, `EM_386` |
| `mingw-i686`, `mingw-x86_64` | yes | **no** – there is no Windows or Wine here | PE machine type, subsystem, entry point, imported DLLs |

So for Windows the statement is: "links without errors and the file headers and imports are what Windows 10/11 expects". It is **not** "was seen to run". The first execution happens on a Windows machine; the test program prints `SMOKE OK` and exits 0 when its checks pass. The imports are only `KERNEL32.dll` and the `api-ms-win-crt-*.dll` set; according to Microsoft's documentation the Universal C Runtime is part of Windows 10 and later, so no runtime has to be shipped. That sentence rests on the documentation, not on a test here.

## Messages that look alarming and are not

`ignoring unknown argument: --target=...` during the mingw-w64 configure step comes from `llvm-dlltool`, which does not understand a flag the build passes to it. The import libraries are still produced and linking works (the smoke test is the evidence).
