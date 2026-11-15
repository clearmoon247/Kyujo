# Architecture decision record

Status: revised 2026-10-24 after the project owner chose a **native source port, Windows first, Linux alongside, Switch later**. The project is named *Kyujo*.

## Goal

Run Pokémon Stadium (USA v1.0) as a **native program on Windows 10/11 (x64)**, and on Linux, from the player's own ROM, by compiling the Team Hippodrome decompilation (see `NOTICE.md`) with a new platform layer. A Nintendo Switch build remains a goal for later; section "What carries over to the Switch" says honestly how much of the early work helps it.

## Decisions

| # | Decision | Why |
|---|---|---|
| D1 | The port lives in its own repository. The decompilation is a pinned git submodule (`ext/decomp`), never modified. | The team's matching CI and history stay untouched. |
| D2 | **No N64Recomp**, and no code derived from it. | Owner's decision. |
| D3 | **Native source port**: the decomp's C is compiled as an ordinary program; the hardware (libultra, RCP, audio, controllers, cartridge) is replaced by a platform layer. Replaces the earlier idea of a machine-code translator. | Owner's choice (2026-10-24). The decomp provides every function in C, so no translation of machine code is needed; see `native-port-feasibility.md` for measurements. |
| D4 | **32-bit first.** The first builds are 32-bit x86 (`i686` Windows, `i386` Linux) so that the game's 32-bit pointers and data layouts hold unchanged. A later, separate stage makes the code 64-bit clean. | Measured: all 349 files compile and all 5,684 checked struct sizes/offsets equal the N64 layout with one layout flag per host. The 32-bit build already reports about 4,750 pointer/integer-mixing warnings (2,545 + 1,615 + 595); those are places that are right only because pointers are 32 bits wide. A 64-bit build has not been tried. |
| D5 | Graphics: an **HLE renderer in the Fast3D style** (F3DEX2 → OpenGL 3.3+) behind a renderer interface. | Owner's decision. Licence of any borrowed code checked before it is copied. |
| D6 | Everything from the earlier private prototype is **reimplemented from the written behaviour specification** (`docs/spec/behaviour-spec.md`); nothing is copied. | Owner's decision. |
| D7 | A new original icon (`tools/make_icon.py`). | Owner's decision. |
| D8 | Every build step is done and checked on the development machine; the owner's Windows machine is for **functional** validation only. Anything that could not be run here is labelled as such. | Owner's requirement. |
| D9 | Code is written to be read: comments explain *why*, behaviour is checked against documentation where it exists, and unknowns are written down as unknowns. | Owner's requirement. |

## Targets

| Target | Role | Build tools | Run here? |
|---|---|---|---|
| `i686-w64-windows-gnu` (UCRT, Windows 10+) | First deliverable. 32-bit exe, runs on x64 Windows 10/11 | clang + lld + mingw-w64 headers/libs built from source by `tools/toolchain` | **No** (no Windows or Wine). Static PE checks only |
| `i386-linux-musl`, static | Development target; the only one whose program can be run and debugged where it is built | same | **Yes** |
| x86_64 Windows / Linux | Reached after the 64-bit clean-up (D4) | same | Windows no, Linux yes |
| AArch64 Switch | Later. Needs the 64-bit clean-up and libnx | devkitA64 | No |

Rejected earlier, recorded so it is not re-proposed by accident: AArch64 ILP32 for a Switch build (libnx and newlib are LP64, and Horizon cannot be made to place memory below 4 GiB).

## Design

```
decomp C (unchanged, ext/decomp/src/**.c)
        │ compiled for 32-bit x86, little-endian
        ▼
   game objects ──links with──► platform layer (this repository)
                                  ├─ libultra replacement: threads, message queues, timers, VI/PI/SI/AI, flash, RSP task submit
                                  ├─ rom: loads the player's ROM, byte-swaps by blob type, resolves segments and overlays
                                  ├─ gfx: F3DEX2 display lists → OpenGL
                                  ├─ audio: native synthesis in place of the aspMain microcode
                                  ├─ input: SDL controllers, left-stick-as-D-pad rule from the spec
                                  ├─ gbcore: new Game Boy CPU core in C (replaces a hand-written MIPS assembly segment)
                                  └─ host: window, GL context, config files (SDL2)
```

Facts that shape the design (all measured; see the feasibility document): 349 C files; 1,299 symbols still to supply, of which 809 are segment offsets and 289 ROM ranges that are *constants to compute*, 89 libultra OS functions, 17 `gu` and 6 `al` functions; 10 hand-written assembly segments needing C replacements; 75 bit-field declarations whose bit order differs between MIPS and x86.

## Open design problems (not yet solved; none is hidden)

1. **Overlays.** The game loads 77 overlays of code at fixed virtual addresses and relocates them with `src/memmap.c`. Natively, overlays can be linked in statically (simplest) or built as separate libraries. At symbol level static linking looks possible: the 349 compiled objects define no global symbol twice. Whether the *behaviour* survives (overlays that are never resident together may rely on sharing memory) is untested. Decision pending a prototype.
2. **Segments and loaded data.** 809 segment-offset constants and the byte-swap policy for each ROM blob type.
3. **Bit-field and endianness fixes** in a handful of headers (`include/color.h` first).
4. **Float determinism** (SSE2 on, no x87).
5. **Getting from 32-bit to 64-bit.**

## What carries over to the Switch

Carries over: the platform layer's structure, the renderer, audio synthesis, input rules, the Game Boy core, the behaviour specification, and the tooling that measures the decomp. Does **not** carry over: the 32-bit-first strategy. A Switch build needs the 64-bit clean-up (open problem 5) regardless of how well the Windows port works.

## Milestones

| | Milestone | Done when |
|---|---|---|
| M0 | Scaffold | ROM checker, settings, controller, launcher.cfg modules tested on a host; icon; spec *(done)* |
| M1 | Measurements and toolchain | 32-bit Linux/Windows toolchains reproducible from pinned sources; decomp compiles for both; census and layout checks *(done; see `native-port-feasibility.md`)* |
| M2 | Stub platform layer | A 32-bit Linux program links with every missing symbol stubbed to stop with its own name, and runs until the first one is hit *(done 2026-10-24: stops at `osInitialize`; the Windows exe links and passes static checks, not run)* |
| M3 | Boot | libultra OS replacement, ROM loading, segment table, endian policy; the game's boot code runs on Linux to the first frame request |
| M4 | Pixels | Fast3D-style renderer: title screen, menus |
| M5 | Windows build | The same program as a Windows exe, validated on the owner's machine |
| M6 | Playable | Audio, input, Game Boy core, saves; behaviour spec met; stall check overnight |
| M7 | 64-bit clean-up, then Switch | separate plan |

## Open items for the owner

* Project name. (Licence for new code: decided 2026-10-28, GPL-3.0-only; see `NOTICE.md`.)
* Whether the owner wants the toolchain smoke-test `.exe` run on Windows before more is built on it.
* Whether saves from the earlier prototype should be importable.


## Implementation notes added while building the GB Tower support (current state; the sections above are the original plan)

Everything here was found by running the game on Linux 32-bit. **Windows binaries have never been run by the author of this file** (no Windows or Wine
in the build environment); they are only built and statically checked (`tools/native/check_build.sh`).

* **Transfer Pak and the Game Boy cartridge** (`src/platform/host_gbpak.c`, `src/ultra/hw_pif.c`): the PIF model answers the pak commands (read /
  write 32 bytes, CRC-8 and address checksum as in n64brew's Joybus page) with a cartridge given on the command line (`--gb-rom`, `--gb-save`). The
  cartridge's MBC1/3/5 bank switching and save RAM are modelled; the real-time clock is absent. Not verified against hardware.
* **Joybus transfer time** (`hw_pif.c`, `hw_regs.c`, `host_sched.c: host_event_post_after`): the SI interrupt of a PIF read DMA is raised after
  (command bytes + answer bytes) x 8 bits x 4 us (n64brew: each bit is 4 us). A 32-byte pak read therefore takes 1.152 ms. This is needed for
  correctness, not realism: GB Tower starts the Game Boy boot program while the cartridge is still being read and only re-initialises the
  emulator if the boot logo has finished (PC = 0x100) before the read ends. With instant transfers the read finished mid-logo and the emulator
  waited forever. The PIF's own processing time and stop bits are not counted, so real transfers are somewhat slower.
* **MIPS interpreter for the Game Boy CPU core** (`src/platform/host_mips.c`, `host_gbemu.c`): the core is hand-written MIPS assembly in the
  overlay, so it is run by a strict interpreter that stops on anything it does not know. Only the instructions the core uses are implemented.
* **RSP interpreter** (`src/rsp/rsp_cpu.c`): runs the Game Boy display microcodes. Instruction coverage is checked against cxd4
  (`tests/native/rsp`, 22 vector instructions, all equal at 20000 random trials). LPV/SPV: element 0 only, any alignment (as cxd4).
  The microcode waits for the CPU to clear SP signal 3; the interpreter yields when it sees the status polled unchanged and resumes on the CPU's
  next access (`hw_sp.c`), instead of spinning (a deadlock otherwise: both sides waited for each other).
* **Overlay variables** are reset at every load of the overlay (see `known-decomp-issues.md`).
* **Flags that are assumptions**, not facts: `DPC_CLOCK` counts RSP instructions; `D_8120D7F3` alias; Transfer Pak info status 0x03 then 0x01; blank
  save RAM reads as 0xFF.
