# Native source port: what was measured

Question: can the Team Hippodrome decompilation (see `NOTICE.md`, commit `8efced6`, 2026-10-22) be compiled as an ordinary program for Windows and Linux, and what stands between "compiles" and "plays"?

Everything below was measured with the scripts in `tools/native/` against that commit, using clang 18.1.3. Every number can be reproduced with the commands in the last section. Statements that are **not** measurements are marked *(assumption)* or *(documentation)*.

## 1. Strategy

Compile the decomp's C unchanged, as a **32-bit little-endian x86** program first. A 32-bit build keeps `sizeof(void*) == sizeof(long) == 4`, as on the N64, so structures that hold pointers and ROM data laid out for the N64 keep their size and field offsets. 64-bit hosts (Windows x64, Switch) are reached later by a deliberate clean-up of pointer-bearing data; that step is not started and is the largest known piece of future work. Windows 10/11 x64 runs 32-bit programs (WOW64), so this is a legitimate first target; it is a development strategy, **not** a result that carries over to the Switch.

## 2. Results

### 2.1 The C compiles (349 files, 2 targets)

| Target | Files | Compiled | Failed |
|---|---|---|---|
| `i386-linux-musl` (-O0) | 349 | 349 | 0 |
| `i686-w64-windows-gnu` (-O0) | 349 | 349 | 0 |

Flags are those of the decomp's own host check, plus `-fno-strict-aliasing -fwrapv` (the original compiler did neither type-based alias analysis nor undefined signed overflow). Clang 18 makes some pointer/integer mixes hard errors; they are downgraded exactly as the decomp's Makefile does, and they are **counted**, because they mark code that is only right when pointers are 32 bits wide:

| Warning | Count |
|---|---|
| incompatible-pointer-types | 2,545 |
| int-conversion | 1,615 |
| incompatible-function-pointer-types | 595 |
| pointer-sign | 262 |

These are why a 64-bit build is not a recompile. Compiling at -O0 shows only that the code is accepted; behaviour at higher optimisation levels is not yet tested.

### 2.2 What is still missing (link census, `tools/native/census.py`)

After compiling, 1,299 symbols (1,298 on Windows) are referenced but defined nowhere, with **0 symbols defined twice**:

| Kind | Count | Meaning |
|---|---|---|
| segment offsets (`D_1002480`…) | 809 | Constants: offsets into RCP segments. A segment table in the renderer must resolve them; they are not variables |
| ROM ranges (`*_ROM_START/_END`, `fragmentN_*`) | 289 | Derivable from `yamls/us/rom.yaml` |
| `D_8…`/`func_8…`/`jtbl_8…` | 33 | Data/code the decomp has not written in C yet |
| libultra OS | 89 | Replaced by this project's own implementation |
| libultra `gu` / `al` | 17 / 6 | Maths and audio-library routines. `lib/ultralib` has both C and hand-written MIPS assembly (`src/mgu/*.s`, `sqrtf.s`) for these; the assembly ones need C equivalents |
| compiler helpers | 9 (8) | Supplied by compiler-rt; the extra one on Linux is `_GLOBAL_OFFSET_TABLE_` |
| other | 47 | Each to be decided by hand |

In addition the decomp has **10 hand-written assembly segments** that cannot be compiled for x86 and need C replacements: boot, entry (bootloader), exception setup, the Yay0 decoder, `fabsf`, libleo's bootstrap, libgsm, a second Yay0 audio decoder, fragment 31's table, and **the Game Boy CPU core of the Transfer-Pak emulator** (`gb_tower_emulator_code_asm`). The count and names are those of the `hasm` entries in `yamls/us/rom.yaml`; their sizes have not been measured yet, so "the only large one" is not claimed.

### 2.3 Data layout agrees with the N64 build (`tools/native/check_struct_layout.py`)

For 132 files containing annotated structs, `sizeof` and `offsetof` were measured by clang for the native target and for a 32-bit big-endian **MIPS o32** target (the ABI the game was built for), and compared. 5,684 values in all:

| Native build | Differences from MIPS o32 | With flag | Differences |
|---|---|---|---|
| 32-bit Linux | 14 | `-malign-double` | **0** |
| 32-bit Windows | 1 | `-mno-ms-bitfields` | **0** |

* Linux differs because 64-bit members are aligned to 4 bytes inside structs; MIPS aligns them to 8. `-malign-double` fixes it.
* Windows differs in one struct (`GbApuRegs`) because mingw targets use Microsoft's bit-field packing; `-mno-ms-bitfields` fixes it.
* **19 structs were not checked** and are listed by `--show-unchecked`: nine `Gb*` structs defined twice in `gb_data.h` under conditional compilation, nine whose bodies contain `#if`, and one that exists only in the Japanese build. A text-based checker cannot know which branch applies. *Their layout is unverified.*
* The decomp's own `// size = 0x…` comments disagree with the MIPS layout in 101 places. Examples: a size that is the sum of field sizes without padding, a size marked `??`, a miscounted array. These are documentation slips, not port problems; the comparison that decides is native vs MIPS.
* The MIPS reference is clang's o32 layout *(assumption: equal to the original IDO compiler's for ordinary structs)*.

### 2.4 What this check cannot see: bit-field bit order

Matching sizes and offsets says nothing about **which bits** a bit-field occupies. MIPS (big-endian) allocates the first declared bit-field in the most significant bits; x86 (little-endian) allocates it in the least significant bits. The decomp has 75 bit-field declarations: 63 in the Game Boy emulator's C file and 12 in `include/color.h`:

```c
typedef union { struct { u16 r : 5; u16 g : 5; u16 b : 5; u16 a : 1; }; u16 rgba; } Color_RGBA16;
```

On the N64 `r` is the top five bits of the 16-bit value; compiled natively it would be the bottom five. Colours read from the ROM through this type would be scrambled. Fix, planned: replace such unions by accessor macros that use shifts and masks, which give the same answer on every host. The emulator's register bit-fields go away with the new Game Boy core.

## 3. Other known hazards (not yet worked on)

1. **Endianness of loaded data.** Everything read from the ROM is big-endian. Each blob needs a defined byte-swap policy. No blob has been classified yet.
2. **32-bit addresses in data.** Assets and overlays contain 32-bit pointers that the game relocates itself (`src/memmap.c` is the MIPS relocation loader and is *not* usable natively). A segment table and an overlay strategy are to be designed; this is the central design task of the next milestone.
3. **Float behaviour.** The N64 FPU is IEEE single precision with specific rounding; 32-bit x86 may use x87 (extended precision). Use SSE2 (`-msse2 -mfpmath=sse`) so results do not depend on the register file. Not yet tested.
4. **libultra replacement**: threads, message queues, timers, VI/PI/SI/AI, flash/SRAM, RSP task submission. Not started.
5. **Graphics and audio**: F3DEX2 → OpenGL 3.3+ renderer; native synthesis replacing the `aspMain` RSP microcode. Not started.
6. **Game Boy core**: written fresh in C and validated against published documentation and public test suites *(to be verified when chosen; names not yet confirmed)*.
7. **Higher optimisation levels** may expose undefined behaviour that -O0 hides.

## 4. Toolchain evidence (`tools/toolchain/`)

| Output | Built here | Run here | Evidence |
|---|---|---|---|
| 32-bit Linux static program | yes | **yes** | prints `SMOKE OK` (64-bit division, double maths, `snprintf`, pointer size) |
| 32-bit Windows `.exe` | yes | **no** | valid PE (`I386`), console subsystem, imports only `KERNEL32` and Windows 10 UCRT DLLs |
| 64-bit Windows `.exe` | yes | **no** | valid PE (`AMD64`), same imports |

No Windows binary has been executed by the person who wrote this document. The first run happens on the project owner's Windows machine.

## 5. Reproduce

```
tools/toolchain/build_sysroots.sh WORK musl-i386          # also mingw-i686, mingw-x86_64
tools/toolchain/smoke_test.sh WORK/sysroot/musl-i386 musl-i386
python3 tools/native/compile_decomp.py --decomp ext/decomp --target i386-linux-musl --out build/native-i386
python3 tools/native/census.py build/native-i386/obj
python3 tools/native/check_struct_layout.py --decomp ext/decomp --target i386-linux-musl -- -malign-double
python3 tools/native/check_struct_layout.py --decomp ext/decomp --target i686-w64-windows-gnu -- -mno-ms-bitfields
```
