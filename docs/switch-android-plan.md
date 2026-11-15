# Nintendo Switch (NRO) and Android: plan, evidence and open decisions

> **About this file.** It is the working record of the Switch and Android port, kept as it was written. It began before the project was named Kyujo: older entries use the working names of the time, and the commit hashes in it belong to the project's earlier private history, not to this repository. The status of each platform today is in the README.

Status: written 2026-11-07, before any Switch or Android code exists. Nothing in this file has been run on a Switch or an Android
device; the statements marked MEASURED were measured on the development machine (Linux x86-64, clang 18.1.3), the others are
plans or reading of documentation, and are labelled that way.

Scope given by the owner: make the port run as a Switch homebrew program (`.nro`) and on Android devices with similar hardware
(64-bit ARM). The old Switch project (Osaka-NX) is **not** a source: it is used only to learn which Switch features exist
(a list of libnx function names, see section 6). No code of it, and none of the project it was derived from, is used here.

## 1. What blocks a Switch / Android build today

The port is a 32-bit x86 program. Four facts about it matter:

| # | Fact | Where it comes from | Consequence for ARM |
|---|---|---|---|
| F1 | Game pointers are 32 bits and equal the console's physical addresses; the program image lives below 16 MiB (`IMAGE_BASE 0x20000`). | `docs/architecture.md`, `tools/native/targets.py` | Switch (Horizon) and Android are 64-bit systems. A normal 64-bit build gives 8-byte pointers: every structure that holds a pointer changes size (the game's data comes from the ROM with the console's layout), and static display lists such as `gsDPSetTextureImage(... (u32)&tex ...)` stop compiling. |
| F2 | All memory of game-side code is big-endian; it is produced by rewriting the compiler's LLVM IR (`tools/native/be_memory.py`). | same | Good news: the build already works on IR, so a new target costs little (section 3). |
| F3 | Pictures are made by a software RDP (`src/render/rdp.c`) after a hand-written F3DEX2 interpreter (`f3dex2.c`). | the native-port status note (project docs) | MEASURED earlier on a 2-core Linux machine: 24 to 35 ms per display list. A Switch core (Cortex-A57 at about 1 GHz) is several times slower. This is the main speed risk (section 5). |
| F4 | The host layer is Win32 and X11/PulseAudio/evdev code written by hand. | `src/platform/` | Needs a Switch shell and an Android shell (section 4). |

The 32-bit pointers (F1) are the central problem. The alternatives, with what is known:

| Option | Verdict | Evidence |
|---|---|---|
| 64-bit clean-up of the decomp (make every pointer 8 bytes) | Rejected | The 32-bit build already reports about 4,750 pointer/integer mixing warnings (`-Wint-conversion` 1,613, `-Wincompatible-pointer-types` 2,543, `-Wincompatible-function-pointer-types` 595 in the wasm32 compile below); ROM data structures and static display lists assume 4-byte pointers. A human-audited rewrite of 349 files is far larger than the other options. |
| AArch64 ILP32 (32-bit pointers in a 64-bit process) | Rejected (earlier decision, `docs/architecture.md`) | Needs all game memory below 4 GiB. libnx/newlib are LP64 and Horizon cannot be made to place memory there; Android has no ILP32 libc. |
| 32-bit ARM (AArch32) | Rejected | Horizon homebrew has no AArch32 libnx; many current Android phones have dropped 32-bit execution. |
| **Run the 32-bit game in a software "sandbox": compile it for WebAssembly (wasm32, 32-bit pointers by definition), translate the wasm to C at build time (wasm2c), compile that C for the target.** | **Chosen, pending owner approval of the translator (section 8)** | Section 3. The sandbox keeps ILP32 semantics exactly, needs no executable memory (important: no JIT on Switch), and works on every 64-bit CPU. Float behaviour becomes the same on all hosts (IEEE, no x87). |

## 2. Design: a portable core in a sandbox, and one small shell per system

```
 build time:   game C + libultra replacement + RSP/RDP + Game Boy parts  --clang (IR, be_memory.py)-->  wasm32  --wasm2c-->  core.c (many files)
 run time:     [ shell (native, per system): window/GPU, audio out, input, files, time, threads ]  <--imports/exports-->  [ core: the game, 32-bit ]
```

* **Core** = everything that today is "game side" (decomp, libultra, `src/ultra`, `src/game_asm`) plus the host-side files that are
  plain C with no operating-system calls: the F3DEX2/RDP renderer, the RSP interpreter, the MIPS interpreter, Game Boy cartridge
  and save logic, the in-game menus (`host_menu.c` and friends). The core's memory is one flat array (the wasm "linear memory");
  an address in it is a 32-bit number, exactly what the game expects.
* **Shell** = what must talk to the system: show a picture, play samples, read pads, open files, tell time, sleep, run threads. About
  15 functions of `src/platform/platform.h` (section 4). The shell is ordinary 64-bit C and can read the core's memory as
  `base + 32-bit address` (the renderer's output picture, audio buffers).
* One core, built once per CPU (x86-64 for desktop tests, AArch64 for Switch and Android). The same shell interface is also what a
  later Windows x64 and Linux x64 build would use, which retires the 32-bit special case.

Why not interpret the wasm at run time: an interpreter is too slow here; ahead-of-time compilers need executable memory at run time
(not available to Switch homebrew without the JIT service, and discouraged on Android). Translating to C at build time needs neither.

## 3. Evidence gathered so far (MEASURED, 2026-11-07, development machine)

* All **349** game C files compile for `wasm32-unknown-unknown` through the existing IR pipeline (`tools/native/compile_decomp.py`
  with `--target wasm32-unknown-unknown`, which includes the big-endian rewrite): **349 compiled, 0 failed, 39 s** on two cores,
  14.9 MB of objects. The target-independence of the pipeline was not assumed; it was run.
* The objects leave **1,311 symbols undefined** (the same set as the 32-bit x86 build; 843 of them are segment/ROM-address constants
  of the form `D_xxxxxxxx`, the rest libultra replacement functions and the platform layer). So the boundary is the one the port already
  has.
* The wasm toolchain pieces on the development machine: clang 18.1.3 with the wasm32 target, `wasm-ld`. Not yet present: a
  wasm-to-C translator and a C library for wasm32 (section 8).

NOT yet known: the size of the translated C (the game plus the renderer will be several MB of wasm; the translated C is much larger,
and its compile time on two cores is unknown), the speed of the translated game code on an A57, whether the whole port boots
under the sandbox. These are the first things the spike must answer.

## 4. The shell interface (what platform.h already says)

Functions the sandbox needs from the outside (all exist today in `src/platform/platform.h` or `host_ui.h`; the shell implements them
per system): `host_window_present` (picture), `host_audio_buffer` (samples), `host_input_read` (pads), `host_rom_load`/`host_copy_from_rom`,
`host_save_*`, `host_gbpak_*` (file access), `host_now_ns`/`host_wall_ns`/`host_sleep_ns` (time), `host_thread_*`/`host_baton_*`
(the one-thread-at-a-time scheme, implemented with the shell's native threads that each enter the core), `host_log`/`host_exit`,
`host_window_pick_rom`, fullscreen. The current threading model (one host thread per game thread, exactly one running, baton passing)
survives the move: each native thread calls into the core with its own stack area.

Rule that must stay true: results cross the boundary by value (BYTE-ORDER RULE in `platform.h`), and a pointer is a 32-bit core address
that the shell converts with the core's base.

## 5. Speed: the renderer decides whether Switch is playable

Plan in stages, each measurable on its own:
1. Functional first: same software renderer, picture scaled by the shell. Likely too slow on Switch, probably acceptable on a fast phone; measure.
2. Cheap wins: a worker thread for the RDP (the Switch has three cores for homebrew), NEON for the inner loops, scaling on the GPU.
   (Note: an earlier threaded graphics worker was reverted because the game rewrote display-list memory while the worker read it;
   the fix `HostHwGfxCompleteFor` exists but the worker is single-task and needs the same check.)
3. A GPU renderer: translate the RDP-level commands that `f3dex2.c` already produces (`rdp_triangle`, `rdp_texture_rectangle`,
   `rdp_command`) into OpenGL ES 3 draws. Render-to-texture and frame-buffer read-back tricks of the game (copy effects, the battle
   transitions) are the hard part; they decide how much of the accuracy of the software RDP can be kept.

## 6. Switch (Horizon OS) facts

Source: libnx (switchbrew/libnx, ISC licence), read from its headers; Osaka-NX contributed only the NAMES of features below, not code.
* Program format `.nro`: needs an ELF-to-NRO packer (devkitPro's `elf2nro` is not reachable from the development machine; the NRO
  format is documented on switchbrew and a small packer is a few hundred lines; to be written and checked against libnx's `nro.h`).
* Picture: libnx `Framebuffer` (software, RGBA8, 1280x720; `framebufferCreate`/`Begin`/`End`, `nwindowGetDefault`, `nwindowSetDimensions`).
  No GL is needed for stages 1 and 2.
* Input: `padConfigureInput`, `padInitializeAny`/`Default`, `padUpdate`, `padGetButtons`/`Down`, sticks; touch via the HID touch screen API (for the menu).
* Audio: libnx `audout` (simple PCM output) or `audren`; a ring buffer fed by the core's audio.
* Applet lifecycle: `appletMainLoop` must be polled; HOME/sleep/focus handled by the shell; `appletSetCpuBoostMode` for load time;
  homebrew in applet mode gets little memory (the album-launch route), title takeover gets more. The core needs the 8 MiB RDRAM, a 12 MiB ROM image
  and the picture buffers: well within either, but must be measured.
* Files: SD card through the C library after libnx mounts it; the ROM and saves under `sdmc:/switch/kyujo/` (path to be decided).
* Memory below 4 GiB is not assumed anywhere (that is the point of the sandbox).
* Cannot be tested on the development machine: everything in this list. The first test must be on the owner's console.

## 7. Android facts (plan, written before the build; what was built is section 44)
* 64-bit ARM only (arm64-v8a); also x86-64 for emulators.
* Native activity with a thin shell (no Java game logic). Picture: `ANativeWindow` (software) first, EGL/GLES later. Audio: AAudio. Input: gamepads and
  touch overlay through the NDK input API. ROM and saves: the Storage Access Framework (scoped storage) or the app's own folder.
* The NDK and SDK can be downloaded from the development machine (dl.google.com answered); Java and Gradle are installed. Not yet tried.
* An APK needs a signing key; the owner's choice.

## 8. Decisions (owner, 2026-11-07) and spike results
Decided:
1. Translator: **build wabt `wasm2c` from source** (Apache-2.0), pinned revision. Approved by the owner.
2. First Switch goal: **wait for the GPU renderer** (no software-rendering release first).
3. C library for the core: wasi-libc could not be built here (build refused by the approval check), so the core gets its **own small freestanding libc**
   (the game already ships its own memcpy/strlen/etc. in src/hal_libc.c); the shell supplies files, time, threads and logging as wasm imports.
Still open: Android packaging (signing key, minimum Android version).

Spike results (measured on the development machine, x86-64):
* All 349 game C files compile to wasm32 through the existing big-endian rewrite pipeline (39 s, 4 jobs).
* They link into ONE valid wasm module (8.0 MB, `wasm-validate` passes) once `bootdisk.c` declares `_bcopy` (it relied on an implicit int declaration;
  needs a port patch). Ten more functions are declared with different return types in different files; wasm-ld warns and would make a trapping stub,
  so each needs a prototype patch or an IR-level adapter before the core runs.
* `wasm2c --num-outputs=8` turns it into about 90 MB of C in 15 s. One 8 MB chunk compiles at -O2 in 13 s, so the whole core is about 2.5 minutes on one core.
* Not yet done: libultra, src/ultra, src/game_asm, generated tables, host-side code and the libc in wasm; `D_xxxxxxxx` absolute symbols (IR-level constants);
  boot under the sandbox; run speed; AArch64 compile; Switch and Android shells; GPU renderer.

## 9. Revision 2026-11-08: reuse the ecosystem, do not scratch-build
Owner direction: best quality over fastest output; reuse what already exists; Switch first, with a framework that makes the Android deploy simple.
Revised decisions (replace the earlier "own libc / own NRO packer / own shell" ideas):
| Need | Decision | Why |
|---|---|---|
| wasm to C | wabt `wasm2c` (built) | exists, Apache-2.0 |
| C library for the core | **wasi-libc** (built from the pinned checkout, owner-approved; sysroot in the scratch area, `libc.a` present) | standard wasm libc; replaces the "own libc" idea |
| Switch toolchain | **devkitPro** (devkitA64, libnx, switch-tools `elf2nro`, portlibs) | the standard route; replaces the "own NRO packer". BLOCKED: devkitpro.org, apt.devkitpro.org and github.com/devkitPro answer 403 from here; owner must allow these hosts |
| Window, GPU context, audio, input | **SDL2** on both systems (devkitPro portlib on Switch, SDL2 Android project) | one shell for Switch and Android; replaces hand-written libnx / NativeActivity shells |
| libultra | keep the port's existing `src/ultra` (already tested); libultraship (MIT, SDL3, Metal/OpenGL; Android arm64 listed, Switch not) is a reference, not a dependency | no duplicate work; reuse where it fits |
| GPU renderer | decision pending a code review of libultraship's Fast3D GL backend and paraLLEl-RDP (Vulkan; not available on Switch homebrew) against our RDP-level boundary (`rdp.h`) | avoid writing a renderer from nothing if a proven GLES3 backend can be adapted |
| libnx features | use libnx directly only where SDL2 has no equivalent (applet lifecycle, CPU boost) | |
Sources checked: https://github.com/Kenix3/libultraship (MIT; Android NDK arm64-v8a, SDL3, Metal/OpenGL; no Switch).

## 10. Result 2026-11-08: the sandboxed core boots and matches the Linux build
Built and run on the development machine (x86-64 Linux), headless, with the owner's ROM (not part of any output):
* `build_native.py --profile wasm32` builds the whole core (game 349 files, libultra, replacements, 35 host-side files) into ONE wasm module
  (`wasm_check.py`: static data ends at 0xA1EB40, 199 start-up fix-up tables and 79 overlay variable sections in order).
* Patch 0034 gives every call the callee's signature (14 functions); indirect calls are made lenient in the generated C (`build_shell.py`,
  `patch_lenient_calls`), because the decomp calls function pointers through other types, harmless on MIPS and x86 but a trap in wasm.
* `build_shell.py` turns the module into C (wabt wasm2c, about 350 MB at -O0 game code; 4.5 min to compile on 2 cores) and links it with `shell/core`
  (instance, WASI calls over POSIX, threads and locks) and the headless front `shell/headless`.
* The game starts, shows the title screen, runs the attract demo at 60 fields/s; of 490 distinct pictures in 15 s, 486 are byte-identical to the
  Linux build's pictures (the other 4 are at the ends of the two time windows). Not measured: speed on ARM, anything on Switch/Android.
* Known deviations and open points: float-to-int of NaN (wasm gives 0, MIPS gives INT_MAX; not checked whether the game ever converts one);
  game code is generated at O0 (wasm2c C is large; an optimiser pass that leaves globals alone is future work, GlobalOpt shrank a variable);
  restart is not implemented in the shell; the picture is still the software renderer's RGB frame (the GPU renderer comes next).

## 11. GPU renderer: review of the options, measured data, decision (2026-11-08)
Owner direction: best quality; reuse what exists; Switch first, Android through the same code. The software renderer (`src/render/rdp.c`) is too slow on the
Switch CPU (section 5) and is the reference for what the pictures must look like.

### 11.1 What the game really draws (MEASURED)
`KYUJO_RDP_CENSUS=<file>` (development switch in `rdp.c`, off by default) counts every primitive by its full set of combiner, blender, z and texture
settings. Scripted run from the title screen through the menus into a battle, 10 minutes, USA ROM, Linux build: 21.9 million primitives, 122 distinct setting sets.
* Cycle types: 1-cycle 8.9 M primitives, 2-cycle 12.8 M, fill 69 k, copy 77 k. 45 distinct colour-combiner settings.
* Blender, all of it fits "fixed-function blending + a fog/shade step in the shader": (a) opaque (no blend, the anti-aliasing edge blend never applies because
  the model treats coverage as full), (b) the usual alpha blend `cc*A + fb*(1-A)` (8.2 M 1-cycle primitives), (c) 2-cycle with a first-cycle fog mix
  (fog colour by shade alpha) and the last cycle one of (a)/(b), (d) "keep the frame buffer" (colour write off). The blender's second input is always the frame buffer.
* Z: compare+update, compare only, none; zmode opaque or translucent (never decal or interpenetrating). Alpha compare (cut-out) on a few. Dither: the RGB
  dither is ON in practically every primitive (the software renderer does not draw it yet). Perspective correction on for 61 % of primitives.
* Never used: LOD / mip-maps, detail, sharpen, chroma key. Bilinear (the console's 3-point filter) on 99.9 %.
* Texture formats seen: RGBA16, RGBA32, IA8, IA16, I4, I8, CI4, CI8 (TLUT in 32 k primitives).
* RENDER TO TEXTURE IS USED. 32 distinct colour images are selected during the run (the two/three 320x240 frame buffers plus about 28 small off-screen
  buffers of 64..172 pixels width and 44..204 rows, and two 320x240 32-bit ones) and 24 % of all texture-image set-ups point inside one of them.
  A GPU renderer must therefore keep every colour image as a surface and let later texture loads read from it (or write it back to game memory first).
  Unclear and still to be measured: the 251 k texture set-ups near the z-buffer (could be a scratch colour image or a false hit of the crude address test).

### 11.2 Options reviewed
| Option | Licence | Verdict |
|---|---|---|
| libultraship Fast3D (`interpreter.cpp` + `gfx_opengl.cpp`) | MIT | Replaces the RSP and RDP model we already have and tested; handles blending with a fixed approximation (alpha on/off, fog as vertex alpha), no TMEM, ImGui and its resource system entangled in the back-end interface. Would be an adaptation of a 5.5 k-line interpreter for less accuracy than we have. Used as a REFERENCE for shader and framebuffer-API patterns only. |
| GlideN64 | GPL-3.0 (compatible) | Best quality of the HLE renderers, but is its own RSP HLE + GL back end in one code base (C++, OpenGL 3.3/ES 3.1 with extensions); extracting the back end is a rewrite of its front. Reference for framebuffer-to-RDRAM synchronisation. |
| paraLLEl-RDP | MIT | Exact, Vulkan only; Vulkan is not available to Switch homebrew. Android could use it, Switch not, so it cannot be the shared framework. |
| angrylion | MAME licence | Exact, software, same speed problem as our renderer. Useful as a reference of behaviour. |
| Own GLES 3.0 back end at the `rdp.h` boundary | GPL-3.0 (own) | Keeps the verified RSP model, TMEM and texture decode; replaces only the per-pixel stage. Every frame can be compared with the software renderer's picture. One code path for Switch (Mesa/nouveau GLES) and Android (GLES). **Chosen.** |

### 11.3 Decision: the RDP moves to the native shell, with a swappable pixel stage
1. `rdp.c` (state, TMEM, tile loads, texture decode, scissor, colour-image tracking) stops being game code. It is compiled natively (x86-64 for tests, AArch64 on
   Switch/Android) and called from the wasm core through shell imports (`shell.rdp_command`, `rdp_triangle`, `rdp_texture_rectangle`, `rdp_set_other_modes`, ...).
   RDRAM is the module's linear memory: a game address is an offset, so `ram(addr)` becomes `mem_base + addr` (bounds-checked as now).
   Gains: native -O2 code with NEON instead of wasm2c bounds-checked code for the software path; the GPU back end can read RDRAM directly.
2. The pixel stage has two back ends behind one interface: the existing software rasteriser (reference, fallback) and `rdp_gles.c` (GLES 3.0).
3. GLES back end: vertices go to a batch buffer; one specialised fragment shader per (combiner settings, blender stage, flags), built on first use and cached
   (later: `glProgramBinary` cache on disk). The first blender cycle (fog mix) runs in the shader, the last one in fixed-function blending. 3-point texture filter,
   N64 wrap/mirror/clamp and the dither in the shader. Colour images are GL textures attached to FBOs; z-buffer a depth texture.
4. RDRAM coherence (as GlideN64 does): a colour image that the GPU has drawn to is "GPU-dirty". A texture load whose source range is inside a GPU-dirty image
   either samples that image directly (exact match: the fast path for the render-to-texture buffers) or first reads it back into RDRAM (`glReadPixels` converted to
   the 16-bit big-endian format). The VI picture comes from the surface at the VI origin. CPU writes into a surface (rare; measured later) mark it for upload.
5. Verification: headless GLES 3.2 on Mesa llvmpipe (works here) renders the same scripted run; pictures are compared with the software renderer's. Differences
   from dither/coverage/filtering are expected and listed; everything else must match.
6. Memory consequence: the native renderer holds a pointer to wasm memory, so the module's memory must not move: initial = maximum size. To be measured (heap use of the game).

### 11.4 Status (2026-11-08)
Done, all on the development machine (Mesa llvmpipe, GLES 3.2 through EGL surfaceless); NOT yet run on a Switch or an Android device.
- Step 1: `rdp.c` and `vi_out.c` run in the native shell; the wasm side calls `shell.rdp_*` / `shell.vi_scanout` (`src/render/rdp_client.c`, `shell/core/rdp_glue.c`). Pictures identical to the all-in-wasm build (696 of 700 frames of a second run match the first run byte for byte; the rest are timing offsets).
- Step 2 and 3: the GPU back end (`rdp_gpu.h`, `shell/core/gpu_gles.c`). It runs the intro at 60 fields/s on a software GL implementation on two CPU cores.
- Correctness tool: `KYUJO_GPU_VERIFY=1` draws every command with the software renderer AND the GPU, reads the colour image back after each command and reports the commands after which more than 300 pixels differ by more than 24 (of 255) in any channel. It is slow (about 0.3 s per frame) and is the regression test for any change to the GPU path. `_VERIFY_DUMP=dir` writes the first pictures that drift apart.
- What it found and what was fixed: (1) a scrolling texture (clouds, water: IA16/RGBA16 tile with mask 5, shift 15) moves the tile window (`sl`,`tl`) while the lower right corner moves faster, so the window grows past the cache limit (16384 texels) or becomes negative; the cache then gave the GPU a 1x1 black texture. Cache extents now follow what `wrap_coord` can reach (mask) instead of the window size. This also helps the software renderer (fewer fallbacks). (2) The depth clear to "max" is exactly 1.0.
- Remaining differences found by the check: one 1-cycle textured mode (`comb FFFFFF/FFFCF87C`) differs by about 400 pixels per draw, along triangle edges (edge/coverage rule); not looked at further. Not modelled: dither, coverage (always full), decal z mode, fill/copy-mode triangles, CPU writes into colour images.
- Measured on llvmpipe only; speed on the Switch GPU (Maxwell, GLES through SDL2/EGL) is unknown. Next: the render-to-texture fast path, a window/present path, the SDL2 shell.
- Toolchain: devkitA64 (gcc 15.2.0, elf2nro, nacptool, SDL2/EGL/GLES portlibs) supplied by the owner as a docker image, extracted and verified; the whole generated core compiles for AArch64.

## 12. Switch shell and first NRO (2026-11-08)
What exists (all built on the development machine; the NRO has NOT been run on a console yet):
* `shell/sdl/main_sdl.c`: one SDL2 shell for Switch (devkitPro's SDL2 port), desktop Linux (development, `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy` works without a
  display) and, later, Android. SDL2 gives the window, the OpenGL ES 3 context, game controllers, keyboard and the audio device; the picture is drawn by the GPU back end's
  render thread (`gles_present`: letterboxed 4:3 into the window, bilinear). The game runs on its own thread, the main thread pumps events and publishes the input.
  Audio: ring buffer between the game's audio call and SDL's callback, playback starts after 43 ms are queued and restarts after a dry run (underruns are counted in the log).
* Switch specifics (`#ifdef __SWITCH__`): the folder is `sdmc:/switch/kyujo/` (settings, saves, ROM, log `kyujo-sdl-log.txt`); the first `.z64/.n64/.v64` file there is the
  ROM; the log goes to that file (nxlink stdio too); arguments from hbmenu are ignored. Pad defaults come from the game's own Xbox-layout profile; Minus opens the in-game menu,
  where the buttons can be changed.
* Build: `tools/native/build_shell.py --front sdl --target switch --devkitpro DIR --gpu` (devkitA64 gcc 15.2, `-O1` generated C, `-O2` renderer) then nacptool/elf2nro. Needed three small compat
  files (`shell/switch/compat`): no `sys/mman.h`, `sigsetjmp`, `sysconf`, `pread`, `pwrite` in devkitA64's newlib.
* Bug found and fixed on the way: the shell's input thread entered the core with shadow-stack pointer 0 (every thread that runs core code needs its own); the game exited at start-up
  as soon as input was pushed. The core now exports `core_shell_stack_top`.
* Core memory is now fixed at 128 MiB (initial = max): the GPU renderer keeps a pointer into it. The shell logs the size every 10 s (it never grew in the runs so far).
* `tools/native/optimize_wasm.py` runs binaryen's wasm-opt -O2 over the module (binaryen built from source, Apache-2.0): 32 MB -> 10.6 MB, generated C 348 MB -> 155 MB,
  every picture of a 20 s run and a 30 s scripted-menu run is byte-identical to the unoptimised build. CPU time on x86-64 did not change: the game's code is not what the machine is busy with
  (llvmpipe, the RDP front end and the audio mixing are). The Switch NRO is built from the optimised module.
* Known after the first hardware test (section 13): it starts, the GL driver accepts the shaders, the speed and the audio timing were not good enough, and the game hung after Start.


## 13. First hardware results (Switch, hbmenu) and what they led to

* Test 1: launches from hbmenu quickly, picture good up to the title; 54 fields/s, audio broken up (35 underruns in 10 s), white screen and hang after Start (the console itself stayed usable).
* Diagnostics added (kept in the code, they cost nothing): render-thread watchdog with phase names, shader build times (13-16 shaders, 25-125 ms each on NV120, no failures), a negative
  cache for failed programs, WASI open logging, `STALL:` lines from the SDL main loop (render thread state, whether the wasm lock is held, last file call, and with `--trace-fn` the last 32 game
  functions entered). `tools/native/optimize_wasm.py --function-map` writes the index -> name map of the optimised module for reading those numbers.
* The hang was NOT the GPU: render thread idle, wasm lock held for 60+ s, game functions in the ring were only `AudioStream_IsPlaying` / `Audio_GetActivityScore` = the decomp's
  `Audio_WaitForIdle` busy loop (`while (Audio_GetActivityScore() != 0)`). The loop calls no OS function, and the device threads that must set `host_preempt_flag` (so that the loop
  can take the audio "interrupt") need the wasm lock, which the spinner never released. On x86 the audio is normally already quiet when the loop is entered, so it never showed. Fix: the generated
  code has a `YIELD_POINT` at every loop header; a 2 ms ticker raises `gYieldReq` while some thread waits for the lock and the holder then hands it over (core_host.c, "fairness of the lock").
  No measurable cost on x86.
* Performance: a sampling profile (LD_PRELOAD SIGPROF sampler, `nm` resolution) showed the RSP interpreter (`host_rsp_run`, audio microcode) at ~73 % of all in-executable CPU time, and it ran
  inside wasm under the lock. `src/rsp/rsp_cpu.c` is now compiled into the native shell (`src/rsp/rsp_client.c` is the core-side stand-in with `shell` imports `rsp_*`; `sp_dma_copy` takes the
  DRAM address as an address, `SP_DRAM_PTR`) and runs without the lock. Still the largest single cost natively (~60 % of in-exe samples on x86); next steps: faster vector unit (NEON or
  lane-array layout), and measuring on the console.
* Not yet verified on hardware: the lock fix, the native RSP (test 3 and later), audio underruns after that change.


## 14. Hardware tests 3-7: the Game Pak check slowdown, direct present, controller labels, a menu crash

* Test 5 (lock fix): the game got past Start; the Game Pak check screen froze mid-transition and the audio was slow with long gaps. Test 6: same. The function sampler (`sdl: profile:`) showed
  the wasm lock free 96-97 % of the time and the game's gfx/audio tasks at 1-50 per 5 s while the video field clock still ran at 60/s, so the game was starved, not busy.
* Cause (found from the `perf:` lines: `picture 301x 16.5 ms (99 %)`): `host_vi_commit` runs inside the video interrupt, on the game thread that holds the baton. It converted every field's picture with
  `vi_scanout` -> `gles_read_picture`, which waits for the render thread to drain its queue, and that includes the previous picture's `eglSwapBuffers` (vsync, swap interval 1). On NV120 the whole
  16.5 ms field went into that wait, so no other game thread ran. On Linux (llvmpipe, no vsync) the same call cost 4-6 ms, which is why it was never seen.
* Fix 1 (test 7): the direct present path. `vi_present` -> `gpu_show_picture` queues `OP_SHOW`; the render thread draws the colour surface that holds the picture (same choice as `do_read`) straight into
  the window (sub-rectangle uniforms `uRect`/`uClamp` in the presentation shader). No read-back, no RGB buffer, no upload. The old path is still used when something needs the pixels in the game's
  memory: frame dumps, screenshots, short on-screen messages (`host_menu_overlay_active`), or no surface covers the picture. `KYUJO_NO_DIRECT_PRESENT=1` forces the old path (the screenshots
  of both paths agree: title screen identical, animation frames differ only by timing). Result on hardware (test 7): Game Pak check passes, `picture` ~0 ms most of the time, the game reaches the menus.
* Fix 2 (test 8): in test 7 the game still stalled in stretches (`game waited 9369 ms for it`): my pacing wait (game at most two pictures ahead of the GPU) blocked the video interrupt again, because
  the display takes exactly one picture per vsync and the game produces exactly one per field, so a backlog of two never drains. The wait is gone: a show that has not started is replaced by a newer
  one (`Cmd.skip`, `gLastShow`), the game never waits for the picture; the stats line counts `dropped`.
* Controller labels (test 7 report): SDL names buttons by position, the Switch labels them the other way, so the right-hand button (labelled A) must be the game's A. `read_input` swaps A/B and X/Y on
  `__SWITCH__`.
* Crash handler (test 8): `__libnx_exception_handler` writes `sdl: CRASH: ... pc +0x... caller +0x...` (offsets from `__start__`; resolve with `aarch64-none-elf-addr2line -f -e core_sdl.elf`).
* Menu crash and missing rental icons (tests 7 and 8; Stadium Select -> cup -> "Select entry Pokemon"): reproduced on Linux with a scripted run (Start, then A presses; the game trapped after ~40 s with
  `core: call through function-table slot 1623679175 failed`, a garbage "function" in `main_pool_pop_state`'s walk over the pool blocks' free callbacks). A RAM dump at the trap (`KYUJO_TRAP_DUMP=file`
  writes the first 16 MiB) showed the right-hand pool block at 0x571260 filled with 0x60C760C7 (a framebuffer clear colour). Cause: the GPU renderer keeps colour surfaces newer than RAM and copies
  them back only when the game reads them (texture load) or when a surface of another shape replaces them; a surface the game never read stayed "newer" for ever, the pool gave its memory to other
  data, and the late write-back (`surf_get`, "a surface of another shape over the same memory") overwrote pool block headers and decompressed assets (`KYUJO_GPU_SYNCTRACE=1` logs every sync request,
  write-back and drop). The console's RDP writes into RAM at once, so there the game just overwrites the memory later. Fix (patch 0035): `main_pool_alloc_from_pool` reports the range it hands out
  (`__osHostPoolMemoryReused` -> `rdp_memory_reused` -> `gpu_invalidate`), the renderer drops the surfaces over it WITHOUT writing them back. The scripted run now goes on for 150 s+ without a trap.
  Not covered: memory reused without a pool allocation (fixed-address overlay data, sub-allocators inside a block).
  Also: the trap message names the failing function-table slot, and `main_pool_pop_state`-style crashes can be analysed from the RAM dump.
* Audio at start-up (test 7): `audio-gap` of 40-140 ms avg in the intro with `audio 8-12 ms avg (50-70 %)`: the audio microcode interpreter on the Cortex-A57 is the limit there (still open).
* Hardware test 9 (Select entry Pokemon page, restart, audio): see section 15.


## 15. Hardware test 9 and what it led to (2026-11-08)

* Select entry Pokemon page (rental icons missing, cursor stuck): a ninth case of the decomp artifact "arrays split into separate symbols that the code indexes as one array". The console linker placed
  `D_84210DA0[]` and `D_84210DD8[]` back to back; the native linker aligns each array to 8 bytes, so indexing past the end of the first array read the wrong data. Patch 0036 merges the two
  tables into one (`D_84210DD8` becomes a pointer into `D_84210DA0`). Verified with a scripted run on Linux. Other instances may exist (an array-bounds build of the decomp would find them).
* "The game could not be started again" (GB game on Pak 1): the shell had no restart. `CoreFront.restart` was added (all files that include `core_host.h` must be recompiled when its layout changes).
  Switch: `envSetNextLoad(path, "\"path\"")` then a normal exit, hbloader chain-loads the NRO. Linux: `execv("/proc/self/exe")`. The previous log is kept as `kyujo-sdl-log.prev.txt`.
  NOT yet tested on hardware.
* Save writes: every flash command rewrote the whole 128 KiB file (tmp + rename, ~200 ms on the SD card). Now write-behind: written 0.3 s after the last command, at most 2 s after the first, and always
  on exit, restart, path change and backup (`host_save_flush`).
* Audio: the RSP audio task ran 9-15 ms on the Cortex-A57 (60 % of game-thread time). The vector unit now uses 128-bit GNU vector types (wider types are scalarised by GCC on NEON), with
  little-endian load/store fast paths. Bit-exactness: differential test against mupen64plus-rsp-cxd4 (test-only reference, not shipped; 200k trials with gcc and clang), load/store unit test, and a 215M-instruction
  in-game cross-check build (`-DPS_RSP_CROSSCHECK`). x86 SSE2: ~10 ns per step against ~15-17 before. Function tracing (`--trace-fn`) is off in the test 11 NRO.
* Open: audio on hardware (expected ~2x faster interpreter; unverified). If still poor: run the audio RSP task on its own thread, trim the scalar dispatch loop, larger SDL audio prime/ring (now 8192 bytes ~ 43 ms).


## 16. Hardware tests 10-13: restart crash, CPU clock, shader warm-up (2026-11-08)

* Test 10 (SIMD RSP, no tracing): audio "much better"; cheats work. Restart crashed.
* Restart, cause 1 (test 11 log: `sdl: next load` was the last line): a chain-loaded .nro runs in the same process as the old one, so threads of the old program must be gone first.
  `core_threads_stop()` (core_host.c) sets a shutdown flag; every core thread checks it where it comes back from the host into the game (lock take, mutex/cond waits, sleeps) and exits;
  the main thread broadcasts all condition variables and waits up to 1.5 s. Test 12 log: "14 did not leave in time", and the restarted copy: "cannot start the game thread".
* Restart, cause 2: `w2c_shell_restart` -> `on_restart` -> `pthread_exit` ended the calling game thread WHILE HOLDING the wasm lock, so every other thread waited for ever behind it. The lock is released
  first now and the thread is taken off the live count. Verified on Linux with the real menu path (`KYUJO_MENU_OPEN=6` + scripted Down/A): "0 did not leave", the new copy starts.
* Audio in battles: the log shows `audio` 11-14 ms per task, 60-70 % of a core, plus gfx 20-30 %: the 3 usable A57 cores run at 1020 MHz. Test 13 asks the clock service for 1785 MHz
  (`clkrst` on 8.0.0+, `pcv` before; the previous rate is restored on exit; `no-cpu-boost` file in switch/kyujo disables; the periodic `sdl: audio:` line prints `cpu N MHz`).
* Battle hitches / black frames: shaders built at first use (30-170 ms each on the render thread). Every program key the game needs is appended to `shader-keys.bin` (full ShaderKey, 24 bytes)
  in switch/kyujo; the next run builds them all on the render thread right after it starts (while the main thread loads the ROMs), before the game's first picture
  (Linux: 25 programs in 1.7 s). `KYUJO_NO_WARMUP=1` turns it off.
* Test 14 tried building them on a second thread with a second EGL context sharing objects (surfaceless). Hardware result: the first launch (no list yet) was good; later launches showed
  distorted intro graphics (`GL error 0x501` after draw) and one start-up crash (data abort in a driver thread, the log's last line was the second-context message). So on this Mesa 20.1 / NV120 driver
  shared contexts across threads are not usable; removed in test 15. The GL query showed `program binary formats: 0` (no binary cache possible) and `KHR_parallel_shader_compile` advertised.
* CPU clock request worked on hardware: `1020 MHz -> 1785 MHz: result 0x0`; the audio task dropped from 8-14 ms to 2.4-3.4 ms per run in the intro.

## 17. Hardware tests 14-17: restart, stretch, Lab screens, GB Tower speed (2026-11-08)

* Tests 15/16: Full Screen option on Switch/Android is ignored and hidden (`KYUJO_FIXED_WINDOW`); the picture stretches to the whole screen by default (`fill_screen=1`, old `widescreen` key ignored on the
  wasm build); "standard aspect" is the option. Plumbing: game host `host_window_sync_view` -> import `shell_set_widescreen` -> `gles_set_stretch`.
* Restart, cause 3 (test 16 log: the restarted copy dies with a data abort in the middle of shader warm-up): libnx `nvInitialize` gives the driver an 8 MB transfer-memory block taken from the heap; it stays
  mapped to the nv service until `nvExit`. A chain-loaded NRO runs in the same process and finds the block still taken. After `SDL_Quit()` the shell now calls `nvExit()` until
  `serviceIsActive(nvGetServiceSession())` is false and logs `sdl: graphics service closed (N reference(s) left open by the driver)`. Test 17 is the first build with it; not yet confirmed on hardware.
* Oak's Lab (PC / trading machine / Pokedex) boxes: GPU log said `blender setting not supported on the GPU: other modes 00504240 cycle 1`. Both cycles are "pixel over frame buffer" with the same alpha:
  R = (P*A + M*(1-A))*A + M*(1-A) = P*A^2 + M*(1-A^2), i.e. an ordinary alpha blend with the alpha squared. `blend_plan` has a new mode `BM_ALPHA_SQ`. Verified in a headless GPU run of the Lab room
  (glowing see-through static, as the software renderer draws it).
* GB Tower slow logo / choppy sound (test 16 log): `mips` (the interpreter running the Game Boy core, host_mips.c) took 34-46 ms per frame during the boot logo (21 frames/s), `audio-gap` 50-64 %,
  91-106 audio underruns. The audio task could not get the (cooperative) wasm lock. In steady state the 15.6 ms "mips" is mostly the core waiting for the next frame, not CPU.
  Fix: pre-decoded instruction cache (16384 entries, direct-mapped, valid for address + raw word so replaced overlays decode again), `$zero` writes redirected to a sink register, aligned loads/stores with
  one range test, one switch dispatch. About 4x faster on Linux in the boot phase (16 ms -> 4.3 ms per call, the old figure is capped by frame pacing). The old interpreter is kept
  (`KYUJO_MIPS_REF=1`) and `KYUJO_MIPS_CHECK=1` runs both in lockstep (registers, hi/lo, next address, after every instruction or branch+delay slot; RCP accesses and native calls are done once):
  the whole GB Tower boot (24 000+ polls, Red logo, intro) ran with no difference. Test 18 carries it.
* Still open: black frame in a battle camera transition; restart with an attached game (test 17); stretch/standard aspect feedback.

## 18. Test 18 result and test 19 (2026-11-08)

* Test 18 on hardware: Lab screens correct; stretch/standard aspect good in handheld and docked; restart still crashes ("software was closed because an error occurred"); GB Tower logo/chime slow and then "nothing plays".
* Restart: the log shows `graphics service closed (0 reference(s) left open)` and the restarted copy still dies at the 20th shader of the warm-up (same place as test 16): chain-loading a second copy of the .nro into the loader's process
  is not usable with this driver, whatever is closed. Test 19: `restart_in_process()` (main_sdl.c): the game thread ends (`on_restart` -> pthread_exit, flag), `core_threads_stop`, join, `gles_quiesce()` (flush + drop caches), audio ring emptied,
  `core_reset()` (core_host.c: wasm2c free + instantiate again, mutex/cond tables and the WASI fd table reset, thread shadow stacks re-made through a generation counter), a new game thread. Window/GL/sound/controllers stay. Linux (Xvfb, menu path): four restarts in a row, pictures after each.
  If it fails the old exit-and-chain-load path is the fallback. This is also the path for Android (no chain-load there).
* GB Tower (test 18 log): logo phase = cartridge read: pak accesses 2566 -> 26252 in about 20 s (Linux: about 12 s), `mips` 38 ms per call, 26 calls/s; steady state `mips` 15.7 ms, 60 calls/s. The interpreter speed-up gave 46 -> 38 ms on the Switch but 4x on Linux, so
  the time there is not mostly interpretation (the core also waits: Joybus wire time 1.152 ms per read via the timer thread, thread hand-offs at every pak read, audio task 10 ms in this mode against 2.7 ms on Linux). The interpreted code spins until the frame is due, so
  its instruction count says little. Test 19 adds `mips-yield`, `mips-native`, `mips-kbranch` to the perf line and a `gbemu:` line (calls per 5 s, Game Boy pc/sp; its MHz figure is mislabeled) to tell a stuck emulator from a slow one.

## 19. Test 19 result, the GB Tower hand-over and test 20 (2026-11-09)

* Test 19 on hardware: restart still ends with "software was closed because of an error"; GB Tower stays on the Nintendo logo (slow logo, laggy chime). The log shows the cause of the restart crash: after `core_reset` the first RDP
  access aborts with `core: the core's memory moved` (`rdp_glue.c` remembered the first instance's memory block; the new instance lives elsewhere). Fix: `rdp_glue_memory_replaced()` is called by `core_reset()`.
  Test: `KYUJO_TEST_MOVE_MEMORY=1` (core_host.c keeps a 160 MiB block alive so the new instance cannot get the old address): repeated menu restarts on Linux, no abort, Main() runs again each time.
* GB Tower stuck at pc 0x0100 (test 19 log): the Game Boy emulator runs the boot logo (pc 0x66 for three 5-second windows, about 26 calls/s) while the cartridge streams in (pak accesses 10 k, 19 k, 27.5 k), then pc 0x0100 for 40+ s, with only about 600 pak
  accesses per 5 s (the per-frame status polling that `func_8120572C` sends in state 0 only). How the game does it (decomp `func_8120572C`): a known cartridge (global checksum 0x91E6 Red, 0x9D0A Blue, 0x47C Yellow) takes the quick start, state 5 -> 6: the Game Boy
  emulator starts at once with its boot logo while the Transfer Pak service runs a script that loads banks and RAM (bank 0, RAM, bank 1, then the other banks). The hand-over to the cartridge (`GbEmu_InitContext`) is checked in state 6 only, in the frame where
  bank flags 0/1 and RAM flags 0-3 are set and the Game Boy pc is 0x100. State 6 ends (state 0) as soon as the service is idle. On the console the boot (seconds) is always over before the 27 s load; on the Switch the emulator runs at about 40 % speed, so the
  load can end first: state 0 is entered, the hand-over is never checked, the Game Boy waits at 0x100 forever.
  Reproduced on Linux with `KYUJO_GBEMU_TEST_EARLY0=1 KYUJO_GBEMU_NOFIX=1` (ends state 6 once banks 0/1 are loaded and the pc is not yet 0x100): pc 0x0100 forever, `svc req 7` polling, exactly the Switch picture. Slowing the emulator alone does not
  reproduce it (the slow-down holds the wasm lock, which slows the pak service the same).
  Fix (host_gbemu.c, in the `GbCpu_Execute` hook): once state 6 was seen and the hand-over (`D_8122C4F6`) has not happened, a state change 6 -> 0 is undone, so the hand-over check keeps running until it succeeds (log line "the cartridge load finished before the Game Boy
  boot ... staying in the loading state"). `sQuickLoad` is cleared by every `GbEmu_InitContext`. With the knob on and the fix on, the Linux run hands over normally.
  The same hook now logs every loader transition (`gbemu: loader: state .. rom .. ram .. svc req .. busy .. handed-over .. pc ..`) and, every 5 s, a `gbemu: state ...` line, for the next hardware log.
* Osaka-NX comparison: it HLEs the pak reads (instant) and runs the emulator natively, so its boot always wins the race; its forced RSP signal-3 "nudge" is off by default there ("does not help").
* New dev knobs: `KYUJO_PAK_PERCENT=<n>` (Transfer Pak time as a percent of the console's), `KYUJO_GBEMU_NOFIX`, `KYUJO_GBEMU_TEST_EARLY0`. The `gbemu:` line now reports calls, average cycle budget and pc/sp (the old "MHz" label was wrong).
* Test 20 = restart (memory moved) fix + GB Tower hand-over fix. To check on the Switch: attach the cartridge, choose restart (expect the game to come back instead of the error), then GB Tower; send `kyujo-sdl-log.txt` (and `.prev.txt`).

## 20. Test 20 result and test 21 (2026-11-09)

* Test 20 on hardware: attach + restart works and is fast; GB Tower now gets past the logo into the game (the hand-over fix is confirmed by the log: `the cartridge load finished before the Game Boy boot ... staying in the loading state`, then handed-over 1 at pc 0100).
  Still slow: the boot logo and chime, the game itself laggy, audio glitching. Gym Leader Castle battles: audio glitches in the transition menu, blurry trainer-name text on the "who fights" page and after the battle (the "keep battling" prompt crisp), lag at the end of every battle.
* CPU clock: the log shows `cpu 1785 MHz` for the first half of the session and `cpu 1020 MHz` from the second GB Tower run on (nothing in the log says why; the system puts its own clock back at times).
  At 1020 MHz the audio task took 8-10 ms per run in the battles (2.4-3.4 ms at 1785 MHz in the intro), which is the battle lag. Fix: `cpu_keep()` (main_sdl.c) looks at the rate once a second and asks for 1785 MHz again, logging `sdl: cpu clock was N MHz: asked for 1785 MHz again`.
* Blurry text: the picture is stretched to the screen with plain bilinear filtering. The battle pictures are 320 x 237, shown at 4 x 3 on 1280 x 720, so every thin glyph blurs; the 640-wide pages (the "keep battling" prompt) are not stretched as much. The GPU renderer and the software renderer
  draw the same glyphs (compared on the tournament "1P TRAINER / JUGGLER" page), so the blur is the scaling. The presentation shader (gpu_gles.c) is now "sharp bilinear": each source texel keeps its colour over its window pixels and only a one-window-pixel band along the edges blends
  (parameters: picture size, window pixels per texel). Checked on a 1280 x 720 window capture (KYUJO_GPU_SHOT): glyph edges crisp. No setting for it yet (ask if a "smooth" option is wanted).
* Audio on its own thread: the RSP audio microcode (the real microcode on the interpreter in src/rsp/rsp_cpu.c) took 10 ms per run in GB Tower on the Switch (2.5 ms on Linux), all of it inside the SP_STATUS write that starts it, with the game's single lock held, so audio + Game Boy core + picture
  could not fit in 16.7 ms. It now runs on a second worker thread (`host_job2_*` in host_sched.c, `host_rsp_run_async`), as the RSP runs beside the CPU on the console; hw_sp.c reports its end (SP_STATUS, interrupt) from the places that look (SP_STATUS reads, HostIdle, HostPreemptPoll, the
  graphics pump) and lets it finish before the game touches SP registers or memory (`audio_settle`). `host_preempt_flag` stays up while either worker's result waits (`host_job_taken`). KYUJO_AUDIO_THREAD=0 gives the old behaviour. Linux: tournament run, GB Tower run and 8 menu restarts
  (memory moved, threads all left) fine. The sound stream is not bit-identical between two runs even in the old mode (the game's own timing), so the check is by behaviour, not by hash.
* Test 21 = clock keep-alive + sharp scaling + audio worker. Please send `kyujo-sdl-log.txt` (and `.prev.txt`) again: the `perf` audio/mips lines in GB Tower and in a battle, any `cpu clock was` lines.

## 21. Test 21 result and test 22

Test 21 (GB Tower hand-over fix, audio worker, sharp scaling, clock keep-alive), user report: first launch showed a solid black screen (a relaunch was fine); GB Tower opened in 11.5 s but the logo drop was laggy; menus in the GB Tower missed directional presses; the in-battle speed-up caused audio issues; trainer-name text blurrier; audio in the Gym Castle battle nearly perfect.

Log findings:
- Black first launch: `sdl: cannot start the game thread (error 12: Not enough space)` and then a crash in the failure path (the render thread was running while `main` returned). The loader process heap is still short right after a previous run. Fix: `start_game_thread()` retries 8 times (250 ms apart, stack 8, 8, 4, 4, 2, 2, 1, 1 MiB; the log names the try that worked) and a failure now leaves through the normal teardown.
- GB mode audio: still ~10 ms per task even with the worker, but the game thread waits only ~1.3 ms on the lock; the clock keep-alive caught a 1020 MHz reset and restored 1785 MHz.
- Blur: the trainer text is already soft in the game's own 320x237 picture (checked on a saved frame), so the present filter can only change it a little. The simulated 1px-band "sharp" filter is slightly crisper than plain bilinear at 1280x720, but the player saw it as worse. Now a three-way option, default **smooth** (plain bilinear): Minus + right-stick click (F2 on a keyboard) cycles smooth / sharp / firm (2 px band) and logs `sdl: picture scaling: <name>`.
- Dropped menu presses: the game reads the controller once per frame (100+ ms per frame in the GB Tower), so a short tap between two reads is lost. `latch_input()` in main_sdl.c keeps every digital input, stick direction and key down until the game has shown two more pictures (at most 300 ms).

Open: GB mode audio cost (10 ms per task) and the MIPS interpreter speed (the GB Tower boot is still slow), battle speed-up audio, one `audio-gap 1504 ms`, black frame in the battle camera transition.

## 22. Test 22 result and test 23 (2026-11-09)

Test 22 on hardware (user): the black first launch did not come back (retry/smaller game-thread stacks not yet proven, no failure seen); GB Tower logo still ~13 s in the default "Faster" cartridge-load mode; with "Game Boy cartridge loading" = Real Speed the logo dropped in 7.25 s with a smooth chime, but the "Loading" bar then took 25 s and no input registered (the game ignores input while the cartridge is read, as on the console, about 27 s at real wire speed). Gym Castle trainer text is still blurry next to the crisp previous page (Juggler worst).

Findings:
- Logo timing: the boot logo is ~420 Game Boy frames = 7 s at full speed (the 7.25 s). In "Faster" mode the extra pak accesses (about 35k, ~30 us of CPU each on the Switch) land in the same fields as the GB core, which already uses ~16 ms of the 16.7 ms field, so the GB runs at ~57 % speed (13 s). The cost is CPU, not the wire time. The right fix is a faster interpreter, not a different pak speed.
- Blur: the screenshots show the lower-resolution battle page (320x237) against a 640-wide page, so the blur is the game's own picture size; the scaling cycle (Minus + right-stick click, or F2: smooth / sharp / firm) is the only lever. Not yet tried by the user.
- MIPS interpreter: the Game Boy core (hasm, ~4200 instructions, run by our interpreter) is now run by a block interpreter (host_mips.c, straight-line blocks, delay slot folded, computed-goto dispatch, direct-mapped block table with generation + first-word check, flushed at GB init). Inside the wasm2c module this gave nothing (translated C, bounds checks), so the same engine is compiled natively in the shell (shell/core/mips_native.c, ~600 lines) and reads/writes the core memory directly; it calls back into the module only for a fragment's native function, a preemption poll and RCP register accesses (exports core_mips_native_call / preempt_poll / hw_read / hw_write). tests/native/mips_fuzz compares reference, block and native engines on 300k random programs: all equal. On Linux the GB logo phase costs about half of the old engine's time per call (25-29 vs 55-58 ns per branch); the Switch should gain similarly.
- A scare worth recording: a diagnostic that printed "N branches in one call" counted per thread cumulatively, so it looked like a hang at the RSP SIG3 wait (a0 0xA4040000 loop at the pool address of the GB's per-frame RSP hand-shake). It was the normal per-frame wait, which completes (hwlog showed 589 completions in 120 s); the report was removed.
- KYUJO_MIPS_NATIVE=0 restores the old interpreter.

Test 23 = native block interpreter for the GB core. Please send `kyujo-sdl-log.txt` (and `.prev.txt`): the `perf` lines with `mips` and `audio-gap` in the GB Tower, and the time of the logo drop in Faster mode.

## 23. Test 23 result and test 24 (2026-11-09)

Test 23 (native block interpreter for the GB core on the Switch): FAILED. Faster mode froze the game before the Nintendo logo dropped; Real speed gave a black screen instead of the GB Tower frame, then froze; both needed closing from the host OS. Logs: `STALL ... wasm lock held`, no `mips` perf lines at all, game functions list empty, last host call a file write (the GB save) - the game thread is stuck inside the first Game Boy frames. The same engine runs the GB Tower fine on Linux x86 (hand-over, ~2 ms per call, 300k-program fuzz equal), so the failure is specific to the Switch (aarch64, devkitA64 gcc -O1, PIE, much slower CPU). Not reproducible here: there is no aarch64 emulator or cross libc in the sandbox (no apt/pip access), and starving the Linux build of CPU did not reproduce it within the time available.

Test 24 = test 22's behaviour by default plus:
- Setting `gb_fast_cpu` (0 default) and an Options entry "Game Boy processor: Standard / Fast (experimental)" selects the native interpreter. KYUJO_MIPS_NATIVE=1/0 overrides it (Linux testing).
- Stall diagnostics: when the picture stalls, main_sdl.c prints `sdl: STALL: native MIPS interpreter: branches (newest first): ...; v0 a0 t0 t1 s0 sp ra; last RCP access ...; preempt flag` (mips_native_describe). If the user switches to Fast and it freezes, the log says where.

## 24. Test 24 result and test 25 (2026-11-09)

Test 24 (user): long black screen before the N64 logo; Standard processor + Real speed = the old behaviour (long loading, logo plays correctly); Fast processor: black page after choosing the GB game, with either cartridge speed; rapid D-pad presses in the GB Tower register only half (8 presses = 4 moves).

Findings:
- Fast freeze, from the new stall report: `native MIPS interpreter: branches 3DFD18 x12 ...; t0 A4040000; last RCP access: read A4040010 = 0x243 (10.5 million reads)`; preempt flag 0, wasm lock held. 0x243 is the state after an audio-type task (HALT|BROKE|INTR_BREAK|SIG2): the Game Boy core waits for the RSP microcode's SIG3 (signal bit 3, `while (!(SP_STATUS & SIG3))` in the hasm per-frame hand-shake) but no microcode is running and none gets started. The microcode is a scheduler task (Sched_SubmitTask) started by the scheduler thread, so the Game Boy thread has to let that thread run while it waits. With the module's interpreter the Game Boy core is slow enough (16 ms per call on the Switch) that the task is already running when it arrives at the wait; with the fast one it gets there first and spins, and the scheduler thread never gets to run. Not yet fixed: this build adds the counters (SP status/flags, task starts/ends, preemption polls, interrupt state, running thread priority, events delivered per kind) to the stall report to see which part of that chain is missing.
- Stall report cap: menu-open pauses used up the 30 reports; the cap is now per stall (reset when a picture comes), the interpreter line is printed for the first 6 reports of a stall.
- Black screen at start: `gles: warm-up: 131 programs built in 6618 ms` before the first picture (first picture 5.2 s after start). Now only the first 20 saved shader programs (the start-up ones, in order of first use) are built before the first picture, the rest one at a time when the render thread is idle.
- D-pad: the input latch (hold a press for two pictures) merged a second press that came while the first was still latched, so the game saw one press. Presses that arrive meanwhile are now counted (up to 8) and replayed as separate taps (down two pictures, up one picture).

## 25. Test 25 result and test 26 (2026-11-09)

Test 25 (user, no logs attached): Fast processor still gives the black screen (both cartridge speeds); uneven movement in a battle with Dodrio at speed 2, best seen moving horizontally (frame pacing, needs the `perf`/gles lines of that battle); Minus is the options menu on the Switch, so the Minus + stick-click scaling shortcut is unusable; wanted: D-pad queue 60 deep, and a loading picture like Osaka-NX's before the N64 logo.

Test 26:
- D-pad tap queue 60 (main_sdl.c, `latch_step`).
- Options > "Picture scaling: Smooth / Sharp / Firm" (setting `scale_style`, sent to the shell with the widescreen flag, `shell_set_widescreen(w | style << 1)`); the Minus + R3 / F2 shortcut is removed.
- Start-up picture (gpu_gles.c `splash_draw`): "PLEASE WAIT / PREPARING GRAPHICS n/m" with a progress bar, drawn on the render thread with a built-in 5x7 font (no game data; Osaka-NX's picture uses the ROM's menu background and font through its skin loader, which this port does not have), refreshed after each warm-up shader, then "STARTING THE GAME" until the first game picture replaces it. Checked in a 1280x720 window capture on Linux.
- Fast-processor freeze: no new data (test 25's report lines were not sent). Hypothesis from the thread table of the decomp: the Game Boy emulator thread (priority 15) waits for SIG3 of an RSP audio task that the GB audio thread (priority 17, woken by an AI event) starts; if no event reaches the game while the emulator thread spins, the audio thread never runs. The stall report prints the events delivered per kind, polls, interrupt state and RSP task starts/ends to confirm this.

## 26. Test 26 result and test 27 (2026-11-09)

Test 26 (user, `Kyujo_logs_build_26.zip`):
- Start-up picture: wanted in the style and colours of the Game Pak Check screens (gold wallpaper, the same translucent blue box).
- D-pad queue: 60 is too many, make it 20.
- GB Tower: cartridge loading Faster + processor Standard has the usual lag for the Nintendo logo and chime. Both on Fast: the page now freezes after the Doduo frame is chosen (before: black screen).
- Picture scaling: Smooth best, Firm second.

Findings from the logs:
- The log of the Fast attempt has no `gbemu:` lines and no `mips` perf line at all: the game stopped before the Game Boy core ran a single call. After the stop there is a 2.4 s `WATCHDOG render thread ... draw upload`, then `fields 158 (31/s)`, then nothing. The STALL lines that follow came after the player opened the menu, and they said "no interpreted code running" which is unreliable: `sCur` (the interpreter's "current run") was saved and restored per run, and runs of different threads do not nest, so after thread A left a run while B was still inside one, the pointer was back to null.
- The stall monitor only looked at pictures and counted every shown picture (the renderer shows repeats), so it did not see a stopped game; the cumulative 30-report cap was also easy to use up with menu pauses.
- Every `mips` perf line in the Standard sessions is the module interpreter (21-29 ms per call, 49-59 thousand branches per call); there was no line for the Fast session at all.

Test 27:
- D-pad tap queue 20.
- Start-up picture restyled (gpu_gles.c `splash_draw`): flat wallpaper in the middle of the game's two menu colours (120,100,40 / 180,180,100 -> 150,140,70; the wallpaper's fine pattern is ROM data and is left out), heading box and text box as rounded translucent blue (30,30,130, alpha 170, radius 16, as the game builds its boxes), white text, yellow caption, progress bar (white frame, dark blue track, green fill). Layout as Osaka-NX's start-up screen. Only plain numbers, nothing taken from the ROM.
- Diagnostics: the interpreter no longer keeps a pointer into a thread's state (a register copy every 256 branches and an "inside the interpreter" count instead); the scheduler (os_sched.c `dbg_threads`) writes a table of the newest 24 threads (id, priority, state, queue, saved pc) into `host_dbg`; the stall report prints `STALL: scheduler: ... threads (id/priority state queue pc): ...`; the monitor now watches the pictures plus the events delivered to the game (`core_dbg_progress`), resets its report count whenever something moves, and prints up to 12 reports per stall. `kyujo: MIPS engine: ...` is logged once when the first machine code runs, so the log says which interpreter was used.

Cause of the Fast-processor freeze (found on Linux, where the same hang showed up once the machine was busy; gdb on the hung process): the core runs all game threads under one global lock (`sWasmLock`, core_host.c). A thread that wants the lock (here the timer thread that posts the video retrace) sets `gYieldReq`, and the holder hands the lock over at its next loop header in the generated wasm code (`core_yield_point`). The native interpreter is not generated code, so its wait loop (the Game Boy core polling SP_STATUS for SIG3) never reached a loop header: the timer thread waited for the lock for ever, the retrace was never posted, the scheduler thread never started the audio task, the wait never ended. The module interpreter runs as wasm and had the headers. Fix: a taken back branch in mips_native.c also calls `core_yield_point()` when `gYieldReq` is set. After it the Linux GB Tower run (KYUJO_MIPS_NATIVE=1) passes the boot with `mips` 1.1 ms per call (the module interpreter: 20-30 ms on the Switch). This also matches test 23's "STALL ... wasm lock held". The "frozen after the Doduo frame" of test 26 is this hang: the Game Boy core starts its first wait there.

## 27. Test 27 result and test 28 (2026-11-09)

Test 27 (user): best run so far. All three GB Tower speeds play as expected with Fast processor and Faster cartridge loading. The tap queue is to go (it causes other input problems now that the processor no longer lags). Pidgeot has a slightly different palette (red tint on the body): possibly authentic, not yet checked. The box around "POKEMON STADIUM" on the start-up picture was not centred around its text (much less blue above than below).

Test 28: the D-pad tap queue is removed again (`latch_step`: a press during the hold extends it; the two-picture / 300 ms hold from test 21 stays). Start-up heading box 200x73 at y 32 with the two text lines at y 46 and 70 (14 px above and below); main box texts re-spaced (18 px above, 16 px below). Pidgeot: needs the screen it was seen on and a screenshot; first check is the same scene in the software renderer (if it shows the tint too, it is the game's data or the RSP lighting, not the GLES shader).

Pidgeot colour (test 28 follow-up): the user sees the normal colours in the Hall of Fame and a tint in battle. Headless battles with the move-test harness (`KYUJO_MOVETEST=<species>,33,0`, ~5 min each; `core_headless` without and with `--gpu`, `--frames DIR` needs the directory to exist) show the lead Pokemon (Pidgeot, Raticate, Kangaskhan) in an olive / yellow-green cast in BOTH renderers, picture for picture the same colour, so the GLES shader is not the cause. The harness only rewrites the species of the lead (the name stays "Gastly"), and opponents in the harness keep their own (Ekans, Machop, Hitmonlee look right), so these frames may be affected by the harness and prove nothing about the real game. Open: a screenshot of the user's battle Pidgeot (and whether other brown / tan Pokemon in battle also look off); if confirmed, next step is to dump the vertex colours / combiner of a Pokemon draw in battle (RSP lighting output) against the same model in the Hall of Fame.

Pidgeot colour, result: AUTHENTIC game behaviour, nothing to fix. Pokemon Stadium recolours a Pokemon that has a nickname: the 8-bit sum of the nickname bytes, the Original Trainer name bytes and the Trainer ID selects a hue rotation between a species-specific minimum and maximum (Pidgeot: -35 .. +20 degrees); the Hall of Fame shows the default colours (source: Bulbapedia, User:MisterE13/Stadium hue shifts; the game has its own HSV code, `Color_HSVInterpolate` in matrix.c). The user's battle Pidgeot is called "BILLY". Checked on our side before finding this: lights in battle are plain white (ambient 127,127,127, one white directional light; new dev switch `KYUJO_LIGHT_TRACE=<n>` in f3dex2.c), the combiner is TEXEL0 x PRIM x SHADE with white prim, GLES and software renderer give the same picture. New dev switch `KYUJO_PRIM_TRACE=1` (rdp.c) lists each new combiner / colour-register / texture-format combination once.

Confirmed by the user (2026-11-10): removing the nickname brought the battle Pidgeot back to the normal colours. Closed.

## 28. Test 28 result and test 29 (2026-11-10)

Test 28 (user, no logs): movement at Dodrio (GB Tower) speed 2 feels better. The D-pad is still not registered for every press at Dodrio speed 3, "regardless of speed" setting.

Cause found by reading the input path: `core_push_input` (core_host.c) took the core lock with a plain `trylock` and gave up when the game held it. A busy game (GB Tower at the top speed, battles) holds the lock nearly all the time, so state changes were handed to the game late or, for a short press, not at all (the latch releases after two pictures or 300 ms whether or not the press had reached the game).

Test 29: `core_push_input` now registers as a waiter and raises `gYieldReq` (the holder hands the lock over at its next loop header or, in the native interpreter, back branch), waiting up to 5 ms for it; the latch (`main_sdl.c`) keeps a press down until the push has succeeded (`sInputUnpushed`); the log says `sdl: input: N of M pushes found the core busy in 10 s` when pushes fail.

## 29. Test 29 result and test 30 (2026-11-10)

Test 29 (user): "That worked perfectly." Inputs are as expected for GB Tower at every speed. Two features requested: a first-start Game Pak Setup page when no Game Pak was detected, and the start-up (shader loading) page to fill the screen (vertical black bars on both sides).

Test 30:
- **Game Pak Setup page** (`host_menu.c`, `HM_SETUP`): (native Windows/Linux builds until 2026-11-12: a few seconds after start, 240 pictures; now, like the shell builds, before the game starts, see section 37) the menu opens by itself on the Game Boy cartridges screen, titled "Game Pak Setup", with a short explanation, when `pak_setup_done` is 0 and no Transfer Pak has a cartridge. Closing it ("Done") sets the new setting `pak_setup_done=1` (host_settings.c), so the page comes once. Not shown in scripted/dev runs (KYUJO_INPUT, KYUJO_RAW, KYUJO_MOVETEST, KYUJO_NO_SETUP_PAGE). Menu > Game Boy cartridges still reaches the same screen any time.
- **Start-up page fills the window** (`gpu_gles.c` `splash_draw`): the canvas now has the window's own shape (480 high, as wide as the window needs, never below 640x480), the 640x480 layout is centred in it, and the presenter fills the window for this picture (`gPresFill`), so there are no side bars at 16:9. Text keeps its proportions.

## 30. Test 30 result and test 31 (2026-11-10)

Test 30 (user): Game Pak Setup page and full-screen start-up page seen. Requests: the start-up page should use the same wallpaper as the setup page; the setup page's title should be centred; the setup page should come before the shaders and the intro; Game Boy processor showed "Standard" in GB Tower, which loaded slower than expected: Fast (and Faster loading) should be the defaults and need not be in the menu; the Options page still names the keyboard keys for the cheat (Q + E, Q + E + C) instead of the Switch buttons. Also reported: "The software was closed because an error occurred" once, right at launch (the next launch was fine; it had happened before without being reported in the status updates).

Test 31:
- **Order at start**: `main()` (host_main.c) calls `host_first_run_setup()` (host_menu.c) right before the game's `Main()`. It gives the shell the wallpaper, shows the Game Pak Setup page when no Game Boy cartridge is set up and the page was not shown before, then calls `shell.graphics_go`. The shell's render thread no longer warms the shaders at its start: it waits for `graphics_go` (or for the first drawing command, as a fallback) and then shows "PREPARING GRAPHICS n/m". While the page is open the game has not started (no restart needed after choosing a cartridge; `sBoot` in host_menu.c). New wasm imports `shell.graphics_go` and `shell.set_backdrop` (core_host.c, CoreFront.graphics_go / set_backdrop, gpu_gles.c `gles_graphics_go` / `gles_set_backdrop`).
- **Same wallpaper**: the game's menu wallpaper tile (128 x 64, built from the player's ROM at run time by `host_gameui_background`) is passed to the shell in memory and tiled over the start-up picture. Nothing is stored.
- Title of the Game Pak Setup page centred like the main menu's.
- **Defaults**: Fast Game Boy processor and Faster cartridge loading are on and are no longer in Options. The settings file keys changed: `gb_cpu_standard=1` (advanced) switches the fast processor off, `pak_loading_real_speed=1` gives the console's loading time; the old keys `gb_fast_cpu` and `pak_real_speed` (written by every earlier build whenever the settings were saved, so existing files held "Standard") are ignored.
- **Console wording**: `HostRawInput.console` (set by the Switch shell) makes the menu name buttons the console's way: the cheat line reads "L + R knocks out, L + R + ZL wins" (from the first connected gamepad's bindings), footers say "A: choose ... B: back", "Minus: menu" replaces "F1 or Esc: menu" and F11.
- **Launch error**: no log of the failing run exists (the next launch rotates it away). The Switch shell now keeps the log of any run that did not end with "sdl: leaving" as `kyujo-sdl-log.crash.txt` (later runs do not touch it) and logs the memory the system gave the program and the applet type at start (a launch from the album or over another program gives far less memory than a title takeover; a shortage would explain an error at launch that does not repeat).

Test 31 result: the Game Pak Setup page looks right but takes no controller input. Cause: the Switch shell (`main_sdl.c`) only pushed input once `sGameUp` was set, and test 31 made `sGameUp` wait for the game's own first picture (to keep the stall monitor quiet during the page). Test 32: a separate `sInputOn` (any picture, including the page's) enables the input latch and pushes; `sGameUp` stays for the stall monitor only. Checked on Linux with injected key presses (Down, Return, Escape on the page; the page closes and settings are saved with `pak_setup_done=1`).

## 31. Test 32 result and test 33 (2026-11-10)

Test 32 (user, logs + Atmosphere crash report attached): the Game Pak Setup page works with the controller and navigates; the setup page still has vertical black bars at the sides; after setting up the Game Paks, Options > Quit game and starting the program again crashed ("error occurred"). Also asked: "Stick also moves the D-pad" should be the default and not an option.

Crash analysis (the relaunch's log ends right after the ROM was opened; the Atmosphere report names every thread): the crashing thread is the render thread, in its very first start-up picture: `render_main > splash_draw > do_present > glTexImage2D > Mesa st_TexImage > nvc0_miptree_transfer_map > nouveau_bo_new > _memalign_r > _malloc_r`, a data abort on a wild address (0x883A1E048) inside newlib's `malloc`, i.e. the C heap was damaged or unusable when the first GPU buffer was allocated. The game thread was reading the ROM (`fd_read`, bounds-checked) and the main thread was in its event loop; nothing of the new start-up code (backdrop, graphics_go) had run yet. The first run of the same session (same build) was fine. This is the same kind of failure as the test 21 "cannot start the game thread (Not enough space)" and the earlier restart crashes in the driver: a start in the loader process right after another run of the same program. A hypothesis that newlib's `calloc` skips zeroing reused heap memory was checked and is wrong (`_calloc_r` always memsets). Root cause not found; test 33 adds the heap numbers at start (`sdl: heap at start: ...`) and a way to test the "same process" theory: Quit now offers "Yes, quit and close the software" on the console (exit code 100: the shell ends the whole process with `svcExitProcess`, so the next start is a fresh one) next to "quit to the launcher".

Test 33 also: the menu / Game Pak Setup picture is composed on a canvas with the window's shape (`HostRawInput.view_w/view_h` from the shell; `present_wide` in host_menu.c): the 640x480 menu in the middle, the game's wallpaper tile continuing left and right with the same phase; no bars and no stretched text. "Stick also moves the D-pad" is no longer in Options (always on; `stick_dpad_off=1` in the settings file, advanced; the old `stick_dpad` key is ignored).

## 32. Test 33 result and test 34 (2026-11-10)

Test 33 (user, logs + one Atmosphere report attached): both ways of leaving fail. "Quit and close the software" (new in test 33: `svcExitProcess` after the usual teardown) crashed on close; "Quit to the launcher" (back to hbmenu) followed by starting the program again crashed at the start, exactly like test 32.

Analysis of the relaunch crash (report 23:05, thread list included):
- The crash is the render thread's very first start-up picture again (`splash_draw > glTexImage2D > ... > nouveau_bo_new > _memalign_r > _malloc_r`). The data abort is `ESR 0x92000047`: DFSC 0x07, a **translation fault at level 3 on a write**, so the page is *not mapped*; it is not a permission problem or a bad pointer inside the program. (The fault address 0xC6A838048 is a chunk header, 16 bytes into a 16-byte-aligned address, in the heap region.)
- The report lists five threads (main, two SDL threads, render, game), all with new thread ids: **no thread of the first run is left behind**, which rules out the idea that an old thread's stack stays aliased in the heap.
- The first run in a loader process is always fine and the heap numbers at start are identical for both runs, so the program itself behaves the same; what differs is the state of the pages in the loader's heap. (Also consistent with test 21's "not enough space" at start and tests 16 to 19.)
- hbloader's source (`nx-hbloader/source/main.c`) shows how the process is reused: the heap is set up once per process (`setupHbHeap`), every NRO is read into the start of it, mapped as code (`svcMapProcessCodeMemory`) and unmapped again on exit; the NRO's own heap is the rest and is **not cleared or checked between programs**. `.bss` is cleared by libnx's `__nx_dynamic` (so it is not stale `.bss`).
- No log of the "quit and close" run exists (the next start rotated it away, and its log ended in "sdl: leaving", so the crash-log retention did not keep it). The only report in the zip is the relaunch crash. The Osaka-NX Switch build also ends with `svcExitProcess`, but only after stopping its threads and closing the audio: it never tears the graphics driver down.

Test 34 (diagnosis built in, with two mitigations):
- **Memory map in the log** (`mem_map_log`, main_sdl.c): every mapped block of the address space (range, type, permission, attributes, IPC and device reference counts) at start, at "leaving, before the graphics teardown", "leaving, after the graphics teardown" and when the program crashes; the crash handler also names the block that contains the fault address. Comparing "after the teardown" of one run with "start" of the next shows which pages are left in what state, and which step leaves them.
- **Heap guard** (`__libnx_initheap` override, same as libnx's own except for the scan): the heap the loader hands over is walked with `svcQueryMemory`; the first page that is not alike (type/permission) is logged ("C HEAP CUT SHORT") and malloc stops in front of it instead of running into it. The log also says how far the C heap grew (`C heap used up to N of M MiB`).
- **Heap check at the end of a run** (after the graphics teardown): if the heap is no longer as given, the process ends with `svcExitProcess` instead of going back to the loader (the next start is a fresh process; the system goes to Home instead of hbmenu) and the log says why. If the heap is as given, the run returns to the launcher as before.
- **Quit and close the software** now ends like Osaka-NX: threads stopped, audio closed, memory map logged, CPU clock restored, `svcExitProcess`, without tearing the graphics driver down.
- **Log retention**: the last line of a normal run is "sdl: returning to the launcher". A log without it is kept as `kyujo-sdl-log.crash.txt`, or as `kyujo-sdl-log.close.txt` when its last line is "sdl: closing the software" (so the log of a close is no longer overwritten by the next start).
- Open: the cause of the unmapped page is still unproven (candidates: pages held by the graphics driver or the audio service after teardown, pages still owned by a transfer memory). The test 34 logs should settle it; if the exit check reports DAMAGED, the first different page and its type tell which service holds it.

## 33. Test 34 result and test 35 (2026-11-11)

Test 34 (user, logs only, no Atmosphere report): both "quit to the launcher" and "quit and close" ended with the system's "software was closed because an error occurred" dialog. No exception was logged (no `CRASH` line, no report): the logs show both runs ended in `svcExitProcess`.

Findings:
- **Quit to the launcher ended in `svcExitProcess` too, because of a false alarm in the new heap check.** The check ran before `socketExit()`, and the bsd service's transfer memory (a 2.2 MiB block inside the C heap, perm none, attr locked) was still held, so the heap "looked damaged". The check therefore never let a run return to hbloader and the relaunch could not be tested again.
- **A bare `svcExitProcess` is shown as an error by the system.** libnx closes an application normally through the applet service's own exit (`_appletSelfExit`), selected by `__nx_applet_exit_mode` (applet.c: with mode 1, `appletExit` installs `_appletExitProcess` as the loader's exit function and the process ends through it; hbmenu itself sets `__nx_applet_exit_mode = 1` before it returns when it ends). Osaka-NX's bare `svcExitProcess` was not a proof that it is quiet.
- **The graphics teardown leaves the heap clean.** The map "after the graphics teardown" shows only the NRO's own code alias in front of the C heap and one remaining locked block (the bsd transfer memory); every device mapping of the driver (about 40 blocks) and the 8 MiB nv transfer memory are gone. The map at start shows the same bsd block. So the pages left behind in test 32/33's relaunch crash are not from the graphics driver.
- Start of a fresh process: heap 0x7E9B6A000, 3158 MiB, one block alike (the guard scan runs inside `__libnx_initheap`, before anything else locks pages).

Test 35:
- Teardown order at the end: threads, audio, GL/SDL, nv service, CPU clock, **`socketExit()`**, then the memory map and the heap check (so the bsd block is gone when it runs).
- "Quit and close the software", and a heap that is not as given, both end with `__nx_applet_exit_mode = 1; exit(0)`: the process ends through the applet service, like hbmenu.
- "Quit to the launcher" with a clean heap returns to hbloader as before; the log of that run (map "start" of the next run, and the crash handler's block at the fault address if it crashes again) shows what the relaunch finds.
- Log marker lines: `sdl: returning to the launcher` (back to hbmenu), `sdl: closing the software` (ends the process; kept as kyujo-sdl-log.close.txt), anything else is kept as .crash.txt.

## 34. Windows and Linux repo snapshot (2026-11-11)

Request (user): a copy of the files needed to build the repository for the Windows and Linux ports, ready for the team before the Switch build is final (a clean-install pass comes later).

Done: `docs/build-windows-linux.md` (tools, steps, packaging, what is verified; linked from README); a source snapshot (`git archive` of HEAD, our files only: no decomp, ROM, Game Boy files or built programs; the decomp is the `ext/decomp` submodule) and a refreshed online Windows setup kit (0.6 MB, built from tracked files only). Built programs are not part of the hand-over: they contain the decomp's code.

Found while verifying from a clean unpack: the native Linux program no longer built. Its image had grown to 16.40 MiB, past the 16 MiB segment-address limit (`imagelimit.py` wants 2 MiB of room), because the Nov 8-9 MIPS block-interpreter work added about 2.4 MiB of static tables to `host_mips.c` (block cache, decode cache, stacks) and the dev primitive trace had a 160 KiB static table. Fix: the tables are allocated on first use in the native programs (`MIPS_STATIC_TABLES` keeps them static in the WebAssembly build, so the Switch/SDL shell is unchanged), the trace table when the trace is on. Result: linux32 13.74 MiB (2.13 MiB room), win32 13.57 MiB (2.31 MiB room); both checks in `check_build.sh` pass; the Linux program starts on Xvfb with a real ROM and shows the Game Pak Setup page; the three Python-side MIPS interpreters agree on 20,000 random programs. Not run: GB Tower on the native programs after the change (no Game Boy ROM on the build machine), anything on real Windows. `tests/native/mips_fuzz/fuzz_native_side.c` no longer compiles against the shell's current `mips_native.c` (stale test, not part of the Windows/Linux programs).

Lesson for the record: the native Windows/Linux builds were not rebuilt between Nov 8 and Nov 11 while the shell work changed shared files; run `check_build.sh` after any change to `src/` (shared files) before handing out a snapshot.

## 35. Test 35 result and test 36 (2026-11-12)

Test 35 (user): **both exits close the software successfully, without an error** (the applet-service exit, `__nx_applet_exit_mode = 1`, is the right way to end the process). The logs also show why the relaunch crashed:
- At the end of a run, after the graphics teardown, `socketExit()` and `nvExit`, the memory map still lists **about 14 blocks of 2 MiB + 4 KiB (`type 05 perm 0 attr 1`, locked) in the C heap, between 9 and 100 MiB from its start**, plus the two 0x21000 blocks of the audio threads' stacks. The 8 MiB nv transfer memory is gone; the blocks left are the graphics driver's buffers (Mesa/nouveau on nv), which nvservices keeps locked after the session is closed. (Before the teardown there are 19 locked blocks, after it 16.)
- The next program loaded into the same hbloader process gets the same heap addresses; its malloc grows into the first of these blocks ("translation fault", a page with permission none is unmapped), which is the crash of tests 32/33 (first big allocation of the render thread's first picture).
- The heap check of test 34/35 therefore reported "DAMAGED" every time (correct, but then it always closed the process, so the relaunch never ran).

Test 36: `__libnx_initheap` now picks the **largest stretch of ordinary pages** (heap type, read/write, not locked) of the heap the loader hands over, and gives only that to malloc (a fresh process has one stretch, the whole heap, as before; a start after another run skips the locked blocks, about 2.9 GiB remain). The log line `C heap: ... N block(s) in all, M skipped` and `C HEAP: pages left locked by the program before this one are skipped` show it. At the end of a run, "Quit to the launcher" returns to hbmenu as long as a stretch of at least 768 MiB is left (each run leaves about 30 MiB locked inside the stretch it used, so several restarts fit); below that it closes the software (fresh process next time), and the log says `TOO LITTLE LEFT`. Open: whether the first relaunch now works on hardware, and how many launcher round trips fit.

## 36. Test 36 result, the Windows setup failure and the Gym Castle Rival crash (2026-11-12)

**Test 36 (user): successful.** "Quit to Launcher" returns to hbmenu and "Quit and close the software" ends at the HOS home page, no error dialog, and the program can be started again from hbmenu (the locked Mesa pages are skipped, see section 35).

**Windows setup (`PokemonStadiumPort-Setup-online-207589d`, first run on a real PC):** stopped at step 3 with `ModuleNotFoundError: No module named 'zlib'`. The Python inside the llvm-mingw package is a small build **with no zlib** (also no bz2, lzma, ssl, sqlite3, ctypes; checked in the zip: `lib-dynload` has no zlib; the unpack rules had not dropped it). Two things needed it: Setup.ps1's own self-test line, and `srctree.py verify`, which reads the decomp zip from GitHub (compressed). Fixed (commit c0e1f70): the self-test asks only for what the build imports; PowerShell (`System.IO.Compression`) unpacks only the 1,160 manifest files into `work\decomp` and `srctree.py verify` checks that folder; a later run reuses `work\decomp` when it still matches; `srctree.py` says clearly what is wrong if it is given a compressed zip and has no zlib. Test: `tests/native/setup/test_setup_python.py` runs every script the setup starts with those modules made unimportable; a full win32 build with them blocked passes (13.57 MiB, 2.31 MiB room). Running the Windows Python under Wine here was not done (blocked by the permission check), so the rest of `Setup.ps1` (steps 4 to 6) is still unproven on Windows. Kit: `PokemonStadiumPort-Setup-online-c0e1f70.zip`.

**Gym Castle Rival crash (user: "close without error or crash page, landed on hbmenu"):** the log has `core: the game trapped in a game thread: Out-of-bounds access in linear memory or a table (1)`, exit code 70; the game's own thread ended, and the shell went back to the launcher as designed. Reproduced on Linux with the user's save (Gym Leader Castle > Elite Four (round 2) with a rental team, four fights won with the L+R+Z cheat, then the Rival selected: the trap is instant). `gdb` on `wasm_rt_trap` gave `Game_Thread > Fragment_LoadAndCall > BattlePrep_RunRulesScreen > BattlePrep_ShowRoundIntro > BattlePrep_InitRoundIntroScene > BattlePrep_InitBannerScene > BattlePrep_LoadBannerGeoLayouts > BattlePrep_AdjustBannerTextureOffset` (names from `wo/fnmap.txt`; the wasm only has function numbers). Cause: the banner table `D_84B0FC6C` has nine entries, the Rival is index 9; on the console the next array sat right behind it (index 9 = `D_84B0FC90[0]` = `D_3048210`, the next banner picture), here it is something else, and the scan for the first non-empty column of that "picture" has no end. **Port patch 0037** adds the tenth entry. Details in `docs/port-patches.md` and `docs/known-decomp-issues.md`.

**On-screen stop message (new):** when the game ends with code 70 the shell now shows the start-up picture with "THE GAME STOPPED / AN INTERNAL ERROR: CODE 70 / DETAILS: KYUJO-SDL-LOG.TXT / RETURNING TO THE LAUNCHER" for 8 seconds (a button after 1.5 s skips it), then goes back to hbmenu as before. `gles_show_notice` in `gpu_gles.c`.

Note for the tester: a reproduction route without a registered team is possible (rental team, any six) and the cheat wins each Elite Four round; the whole route is about 12 minutes of scripted key presses on the Linux SDL build.

**Windows setup, second run (kit c0e1f70, 20:27):** steps 1 to 3 now pass (download, SHA-256, unpack of 4388 files), then the Python self-test failed with `NameError: name 'bx' is not defined`. Cause: the self-test line had `hashlib.md5(b"x")`; **Windows PowerShell 5.1 drops the double quotes inside an argument when it starts a native program**, so Python saw `b x`. My own check ran the line from a Linux shell, where the quotes survive. Fixed by removing the quotes (`hashlib.md5()`); `tests/native/setup/test_setup_python.py` now fails if the line contains a double quote and runs the line itself. Steps 4 to 6 of Setup.ps1 are still unproven on Windows; none of the other arguments the script gives Python contains a double quote (paths with spaces are quoted by PowerShell itself).

**Windows setup, third run (kit c85e9d8, 20:32):** steps 1 to 4 pass (Python works, ROM accepted, source unpacked and all 1,160 files match). Step 5, the build, fails in `src/platform/host_trace.c`: `strndup` is not declared by the llvm-mingw package's headers (the Windows C library has no `strndup`). Our own mingw-w64 sysroot, which every earlier Windows build used, does declare it, so the file compiled here. Fixed by using malloc + memcpy. Because the two header sets differ, the Windows program is now also built against the real package (`tests/native/setup/build_against_package.sh`): it compiles all 44 platform files, links and passes `pe_check` (13.57 MiB, 2.30 MiB room). Run it after any change to `src/platform` or `src/render`.

## 37. Test 37 result and the GB Tower regression on Windows (2026-11-12)

**Test 37 (user, Switch hardware): successful.** The Gym Castle Rival (round 2) battle completes, the prize Pokemon arrives in Oak's lab, and the Surf Pikachu unlock also works (patch 0037).

**Windows (kit a6e0d00, user's PC):** the setup now builds a working exe (steps 1 to 5 pass). Reported: (1) **GB Tower stops the program** with `machine code at 0x004B6644 [console address 0x81209874]: a load or store outside game memory (0x1000..0x00FFFFFF)`, then `FATAL: the MIPS interpreter (host_mips.c) met something it does not support`; (2) lag and audio problems in the opening; (3) the Game Pak Setup page appears after the N64 logo and the intro have started; build 18 was the last stable Windows build.

**Cause of (1):** commit 65261eb allocated the interpreter's stacks with `calloc` (to keep the native image under 16 MiB). The interpreted code's stack pointer is the address of one of those stacks, and the interpreter accepts only addresses below 16 MiB, so a heap address failed on the first stack access (the Linux native snapshot had the same bug; the shell/WebAssembly builds keep static stacks and were never affected). **Fix:** the stacks are static again in every build (128 KiB; the big decode/block/arena tables stay run-time allocated), and `mips_mem()` stops with a clear message if the stacks ever land above 16 MiB. The native images now have 2.01 MiB (linux32) and 2.18 MiB (win32) of room below the limit. Verified: `check_build.sh` passes for both profiles; on the Linux native build GB Tower starts with the Red cartridge (the MIPS engine runs, no fatal, the Game Boy screen boots to the title).

**Open:** the opening lag/audio on Windows (the log shows 60 fields/s, audio 293 to 301 buffers per 5 s, graphics 28 ms at 68 percent in busy scenes, one 45 ms audio gap at the start: the same numbers as earlier Windows tests on that PC) and the late Game Pak Setup page (native builds show it over the running game after about 4 s; the shell builds show it before the game starts). A comparison with build 18 is needed.

**Late Game Pak Setup page, fixed (commit after 698cf9e):** the native builds opened the page over the running game after 240 pictures, which paused the intro and its sound in the middle of the N64 logo (the user's "audio issues and lag in the opening"). They now use the same path as the shell builds: `host_first_run_setup()` runs for every build before `Main()`, shows the page on the first start when no cartridge is set up, and the game starts when the page is left (Esc or Done). Checked on the Linux native build (fresh settings, no cartridge: the page is the first picture, Esc starts the game). The 240-picture trigger in `host_menu_poll` is removed.

**Opening performance, Linux comparison (same machine, same ROM, 40 s of the opening, no cartridge):** the repo at 3553060 (the end of the Windows test series) and the current tree both run at 59.9 to 60.5 fields per second, graphics 3 to 16 ms per frame, audio 0.7 to 2.4 ms; the current tree is the same or slightly faster, so there is no code regression in the opening on the native build. What is left on the user's PC is Windows-specific (graphics 28 ms per frame in busy scenes, one 45 ms audio gap when the sound device opens); needs the build-18 log lines for comparison.

## 38. Windows test 695d1e8: static in the battle audio, new resampler (2026-11-12)

**Windows (kit 695d1e8, user's PC):** the setup page comes first now; graphics good; Game Boy cartridges 1 to 4 loaded; the cheat "win the battle" worked. **New: static / noise in the audio during the battle.** The log has no dropped-buffer line and no new audio-gap line during the battle (the only gaps are 3 x 23 ms in the first five seconds and one 5.6 ms gap later), so the static is in the signal, not missed buffers.

**Measured on Linux** (a Poké Cup battle with a rental team, the game-rate and device-rate WAV sinks, 461 s): the stream sent to the device had **0.34 to 0.62 percent of its energy above 16 kHz** in the loud passages (0.3 to 0.6 percent, about 22 dB below the signal), where the game's own stream has none. Cause: the conversion from the game's 32006 Hz to the device's 48000 Hz used **linear interpolation**, which lets the mirror images of the sound through (a 12 kHz tone also appears at 20 kHz, only 17 dB down). On bright, noisy battle sounds that is hiss.

**Change:** `to_device_rate` (host_audio.c) is now a band-limited resampler: 64 taps, Kaiser window (beta 9), cut-off at 95 percent of the lower Nyquist frequency, 512 tabulated phases, unity gain at DC, result clamped to 16 bits. Checked by `tests/native/audio/run_resample_test.py`: gain 0.000 dB at 1 and 10 kHz, the 12 kHz tone's image at -103 dB, the same samples for one buffer or many, no wrap-around at full scale, no click when the rate changes (32006 to 48009 Hz). On the recorded battle the energy above 16 kHz went from 0.34 percent to 0.00 percent; the conversion costs about 0.35 percent of one core. The cushion test (`run_cushion_test.py`) had been broken since the idle tick was added (the Linux keep-alive code needs `<time.h>`); fixed, passes.

**Not proven:** that this is what the tester heard (nobody has listened to the Linux output, and the Windows device path was not run here). The game itself clips about 390 of 14.8 million samples in this recording; the hardware does the same. If static remains, the next things to test are the device buffer size (waveOut, 12 slots) and the Windows mixer's own handling of 48 kHz; a recording of the Windows device-rate stream (`KYUJO_AUDIO_DEVICE_WAV=file`) from the same battle would show whether the stream itself is clean there.

## 39. AppImage for the Linux build (2026-11-12, started)

Request: build the Linux program as an AppImage. The program is one static 32-bit executable (own X11 and PulseAudio protocol code, no shared libraries), so there is nothing to bundle; the image only adds the start script, a `.desktop` file and the icon. **Prepared:** `tools/linux/appimage/AppRun` (the data folder is outside the read-only image: `$KYUJO_HOME`, else `~/.local/share/PokemonStadiumPort`; it finds the ROM there or next to the AppImage, explains when it is missing, `--probe` runs the test program, `--data-folder` prints the folder) and `tools/native/package_appimage.py` (builds the AppDir; with `--appimagetool` it builds the image). The AppDir was run here: no ROM gives the message and exit 2, a ROM in the data folder starts the game with settings and log in that folder. **Not done:** the image itself. The sandbox did not allow running a downloaded `appimagetool` (a third-party binary from GitHub), so the final step waits for the owner's decision: (a) allow running the official appimagetool here, (b) build the squashfs with squashfs-tools and the official type-2 runtime file, or (c) the owner runs appimagetool on a Linux machine on the AppDir. Limits to tell the tester: the image needs FUSE 2 on the machine (or `--appimage-extract-and-run`); the 32-bit program needs a kernel with 32-bit support (as before); Batocera has no AppImage launcher, so for Batocera the zip stays the right package.

## 40. NRO result (Rival R2 + Mewtwo R2), AppImage, announcer static (2026-11-13)

**User, Switch (latest NRO):** completed the game with both Rival R2 and Mewtwo R2, both sets of credits, no crash and no error. **Audio:** the noise exists on every platform and is specific to the announcer.

**AppImage:** `PokemonStadiumPort-13f4393-x86_64.AppImage` built and delivered (section 39); waits for the user's Linux/Batocera test.

**Announcer static, cause (measured on Linux):** the audio RSP task loads the announcer / cry stream rings (and the Game Boy ring) 4 to 6 pulls after `n_alMainBusPull` built the command, because the microcode runs on a worker thread; the producer (`func_80044EA4`) had by then lapped or straddled the region in 3.5 percent of the loads (0.2 percent with the task run synchronously). Details: docs/known-decomp-issues.md.

**Fix:** port patch 0038 (`audio-ring-snapshot`) copies each pull's 184 samples into a 16-slot ring at build time. Click detector on the same battle route: 1.39 percent of loud blocks rough before, 0.56 percent after (synchronous floor 0.47 percent). 17 KB more image: the linux32 room check (`imagelimit.py`) now requires 1.75 MiB (was 2 MiB; the MIPS stacks are inside the image since 2026-11-12, so the margin only covers growth); linux32 has 1.99 MiB, win32 2.16 MiB.

**Open:** nobody has listened to the result yet (needs the users' ears on Windows / Linux / Switch).

**Update, user (Fri 19:34):** first Switch test of test 38: the audio is good. The user asked to continue with the remaining features while Windows and Linux are tested (section 41).

## 41. Cheats: Hall of Fame complete, Kids Club CPU vs CPU (2026-11-13)

Two options in Options (menu), both **off by default** (settings file `cheat_hall_of_fame=` and `cheat_cpu_vs_cpu=`; `KYUJO_CHEAT_HOF=1` / `KYUJO_CHEAT_CPUVS=1` force them for one run). The host answers `__osHostCheatOption(1|2)` (host_cheat.c), the game side is two port patches.

**Patch 0039, Hall of Fame complete.** The Hall of Fame (Victory Palace) is "complete" when typed record 0x13 exists for all 151 species (`VictoryPalace_LoadSpeciesRecord`); entering it then plays the completion scene (`State_VictoryPalace` -> `Stage_RunDemoScene(0x1A)`, "For your 151 Pokemon HALL OF FAME, you won AMNESIA PSYDUCK!") unless `Save_GetOptionsField1F` says it was seen. With the option on, a species without a record gets one made up in memory when it is asked for (first cup, level 50, ID 00000, species name, the player's label as trainer); nothing is written by this function and real records are never replaced. **What the game then does is real**: after the scene it stores the "seen" flag and Psyduck as the chosen Pokemon in the save (as after a genuine completion), so turning the option off later does not undo that.

**Patch 0040, Kids Club CPU vs CPU.** On "Who wants to play?" the cursor could only go down to "Ready!" while player 1 was human. With the option on it goes whatever player 1 is, so COM vs COM vs COM vs COM can be started: the COM LEVEL page follows, then the game picker and the game.

**Checked on Linux (the tester's save copy, scripted):** the join screen with four COM and Ready! selectable; COM LEVEL page; Magikarp's Splash, Run Rattata Run, Dig Dig Dig (Sandshrew) and Clefairy Says played through with four computers ("COM WINS", "Play again / Play a different game / Quit" menu works, Clefairy Says runs); not tried: the other five games (Sushi, Drowzee, Dynamo, Pikachu, Ekans, Metapod and tournament mode "Who's the best?"; the code counts human players in a few of them and was read: zero humans gives a computer count of four). Hall of Fame: the completion scene played, "Received PSYDUCK!", the save was written (copy), then the gallery shows No.001 BULBASAUR, No.002 IVYSAUR ... with the plate (NAME, LEVEL 50, OT TRAINER, ID 00000, RENTAL, HALL OF FAME POKE CUP). The Options page fits the screen (picture checked); the menu toggles themselves were not clicked through (the control channel does not drive the menu), the settings round trip is in the UI unit test (62 checks).

**Not verified:** Windows, Switch (the Options menu rows, the completion scene on the console renderer).

## 42. Windows: the "Pika" voice at launch stutters on Windows only (2026-11-13/14)

**User (Fri 22:13):** both cheats work. The Pikachu at launch is very stuttery on every Windows build so far; not on Batocera or Switch. **Clarified (Fri 23:58): it is the "Pika" VOICE, before the N64 logo (audio, not picture).** Sections below up to "Finding and change" chased a picture stutter by mistake; the input-thread change is a real fix of a 21 ms window-thread stall but is not the cause of this symptom.

**What is known:** the game code, the renderer and the frame pacing are the same on Windows and Linux (timeBeginPeriod(1) and a spin for the last 2.5 ms are in `host_sched.c`; the Linux comparison of the opening at 3553060 and now showed no regression). Windows-only parts: the window thread (`host_window.c`: every picture posts `WM_PS_FRAME`, the window thread scales it with `scale.c` and does one `StretchDIBits`; the same thread polls the pads on a 10 ms timer, XInput slots without a pad once a second) and `waveOut`. Scaling is not the cost (`scale.c` on this Xeon: 1.3 ms at 640x480, 4 ms at 1080p, 10 ms at 4K). The earlier Windows logs only had averages (graphics 28 ms at 68 percent in busy scenes, 60 fields/s), which cannot show a stutter.

**Change (diagnostics only, no behaviour change):** the 5-second `perf:` line now also has `max` for graphics and picture, `field-gap max N ms late M` (longest wait between two video fields and the number of fields later than 25 ms; a field is 16.7 ms), and on Windows `paint Nx avg max` (the window thread's paints) and `pad-poll max` (the slowest 10 ms timer poll of the window thread, which holds up the paints behind it). Checked on Linux (the line prints, `late 0`), compiles for Windows against the real llvm-mingw package.

**Next:** the user's Windows log from the launch (first 60 s) and, if possible, the same from Batocera for comparison; the CPU of the Windows PC and the window size (or full screen). If `field-gap`/`late` is high with small `gfx max`, it is pacing/scheduling; if `gfx max` is above 33 ms in that scene it is the renderer's cost; if `paint max` or `pad-poll max` is large it is the window thread.

**First timed Windows log (user, build 11627da, HP ProBook 445 G9, Ryzen family 25 model 80, ~2.3 GHz; the log has only ONE perf line because the menu was opened after about 5 s, so the Pikachu scene itself is not in it):** gfx 115 tasks 7.6 ms avg, **max 12 ms** (the renderer is not the problem on this laptop); fields 59.7/s; `field-gap max 77 ms late 2` (start-up: audio device opening, first lists); picture 224 new pictures for 299 fields (a field without a new frame buffer is not a picture; Linux menu: 299 of 300); paint 253x avg 1.2 ms max 3; audio-gap 2 x 35.6 ms (start); **`pad-poll max 21 ms`**.

**Finding and change (build below):** the pad poll ran as the window thread's 10 ms timer, so one slow poll (XInput for slots without a pad is the well-known slow call, three empty slots at about 7 ms each; also the Raw Input device list every 2 s) held up every paint behind it: a 21 ms poll is longer than a field, so a late picture about once a second, only on Windows (Batocera and the Switch poll elsewhere). The polling now runs on its own thread (`input_thread`, every 10 ms; single writer of `host_raw_publish`), `WM_TIMER`/`WM_KILLFOCUS` no longer poll (the thread sees at its next poll that the window lost the front and releases the keys). `pad-poll max` in the perf line now measures that thread and no longer delays paints. Compiles against the real llvm-mingw package; NOT run on Windows. Not proven to be the whole stutter: the Pikachu scene is not in the log yet.

**Next:** a log of at least 60 s from the start (do not open the menu), to see `field-gap`, `late`, `gfx max` and `paint max` in the Pikachu scene.


**Audio at launch (Sat 2026-11-14).** The user's 5 s launch log (build 11627da) shows: sound device opened at the first buffer (opening is done on the game thread), cushion 80 ms, `audio-gap 2 x 35.6 ms`, `field-gap max 77 ms late 2`, and the renderer's "first use" message just before. The game thread therefore stood still for up to 77 ms right when the voice starts. The 80 ms cushion is queued when the device is first found idle, i.e. at the first buffer, and a stall of that size eats all of it; the device ran dry (no "ran dry" line was printed because the first-time notice was suppressed for the first buffer). Linux (Pulse) and Switch (SDL, its own larger buffer) do not show it. What the cause of the 77 ms is was NOT found (candidates: first use of the renderer/shaders, first file reads, device open); the change makes the sound independent of it:
- `host_audio_prepare()` (called from `host_main.c` before the game's data is loaded): Windows opens the waveOut device on a short thread at program start; the first buffer waits for it only if it is not done yet. `NSLOTS` 12 -> 64.
- Start-up cushion: the first time the device is idle, 300 ms of silence is queued (`KYUJO_AUDIO_START_CUSHION_MS`, default 300; later dry-outs get the normal 80 ms). From 3 s after the first sound the resampler plays 1 percent fast (17 cents) while the queue is longer than cushion + 60 ms, so the 300 ms is run down in about 16 s and sound is not 300 ms behind the picture for the whole session. The first 3 s (the voice, the logo) are at exact pitch.
- Diagnostics in the log: `sound device opened in N ms`, `start-up: N ms between two buffers at T ms after the first (the game thread was late); Q ms were queued before this buffer` (stalls over 40 ms in the first 8 s, at most 8 lines), the first "ran dry" is now printed, and one summary after 8 s: `first 8 s: buffers, longest wait between two buffers, lowest queue, device ran dry N times, opened in N ms`.
- `tests/native/audio/run_cushion_test.py` (simulated device) gained: a 150 ms stall just after the start is heard with the 80 ms start cushion (gap 71 ms) and not heard with 300 ms (gap 0); the 300 ms is run down to about 140 ms in 30 s; with no start cushion nothing is sped up. The test and `test_resample.c` had stopped building (game variables referenced by the stream dump, now stubbed); they pass again.
- NOT run on Windows. If the voice still stutters, the new `start-up:` lines say whether the game thread was late (and by how much, and when) or the device (open time, dry count).

**Result of kit 784acc2 (user, Sat 00:59): the "Pika" voice is still very garbled, at the first start and at later starts.** The log (new diagnostics) shows the device opened in 42 ms before the first sound, 350 ms queued at the one late buffer (`start-up: 57 ms between two buffers at 104 ms after the first`), so **the device did not run dry: the garbling is in the samples the game produced, not in the delivery.** The start-up cushion cannot help with that (kept; it is harmless and its log lines are useful).

**What was found on Linux (not on Windows): the voice is timing sensitive.** The Pika voice is not a streamed voice (KYUJO_STREAM_DUMP is empty at launch); it is a normal sample sound. Captures of the game's own sample stream (KYUJO_AUDIO_WAV) of the first 1.5 s: clean runs are bit-identical (rms difference 0 to 20); with CPU hogs competing for the core 5 of 8 runs differ, and in the damaged stretch (1200 to 1700 samples = 2 to 3 audio frames) the samples are an exact copy of the audio 1656 samples (3 frames, the game cycles three output buffers) earlier, or a frame of silence: a frame that was not (re)written is played. Not a delivery problem (the same on the WAV sink). Checked and ruled out: the audio worker still running when a buffer is taken over (counted: never, in 40 runs; the experimental wait-for-the-task change was removed), samples DMA'd while the task runs (frequent, harmless by the game's FRAME_LAG rule).

**Cause found for Windows: the game thread stops at the first picture.** On Windows the window (class, menu bar, window thread, input thread, XInput) was created at the FIRST PICTURE (`start_window()` from `host_window_present`, and the first picture is made while the first sound is already playing), and the game thread WAITS for it (`WaitForSingleObject(sReady)`). The log order shows it: first buffer queued, then the renderer's first-use line, then "gamepads: XInput is available" (printed by the thread started with the window), and the 57 to 77 ms hold-up of the field/audio clock at that moment. A held-up game thread loses retrace/audio frames (the scheduler then plays an unwritten frame), which is the garbled Pika voice. Linux creates its window too at the first picture (`host_window_wanted`), Batocera and the Switch have faster or no such start, which fits "only Windows" (Linux also logged "audio underruns at start" in the outstanding list).

**Change:** `host_window_prepare()` (platform.h; Windows creates the window, Linux opens the X window, others nothing) is called from `host_main.c` before the game's data is set up, so the first picture finds it ready; it logs `window: created in N ms (before the game starts ...)`. Not run on Windows (compiled against the real llvm-mingw package; Linux run checked: window opens, perf line normal).

**Next:** the user's launch log from kit 784acc2's successor. Lines to look at: `window: created in`, `sound device opened in`, `start-up: N ms between two buffers` (should be gone or small), `first 8 s: ... longest wait between two buffers`. If the voice is still damaged and no long wait is logged, the stale-buffer replay needs a different cause: compare Windows with the Linux load capture method above (`KYUJO_AUDIO_WAV`, `tests` none yet).

**Result of kit 9f146db (user, Sat 03:07): still garbled. Recordings from Windows (`Record-Pika-Audio.bat`: `pika-game.wav` and `pika-device.wav`): both damaged, so it is the game's own sample stream, not the device path.** Compared with the Linux reference of the first 1.3 s: 0 to 0.5 s bit-identical; from 0.627 s the Windows stream has 7 jumps over 16000 (Linux: 0) and the damaged chunks are exact replays: chunks (184 samples; a frame is 3 chunks = 552, or 2 = 368) 109-111 equal chunks 101-103 (3 frames earlier), and 118-120, 127-129, 136-138 repeat 109-111 every 9 chunks (3 frames), until about 0.8 s, when it recovers. So one of the game's three output buffers is not rewritten by the audio microcode, three times in a row, and the stale content is played again. Why is NOT known. Ruled out on Linux: a slow audio worker (KYUJO_DEBUG_AUDIO_LAG_MS 2 to 40 ms: no damage), the buffer taken over while its run is active (never, 40 runs), CPU starvation reproduces only a different kind of damage (a 3-frame replay after a stall). Not the scheduler dropping the audio task via yield (yield is virtual here).

**Next step (kit with trace):** `KYUJO_AUDIO_TRACE=1` (the helper bat sets it) logs one `AITRACE` line per output buffer for the first 240: n, hash, same3 (1 = identical to the buffer three before), bytes, audio runs started / done / started at hand-over, run active, hand-over to start ms, start lateness ms, ms since the last run started / ended, time. On Windows the replayed buffers should show in the log with the state of the audio runs at that moment; compare with a Linux trace (all `same3` 0, started = done + 0 or 1, hand-over to start about 16 ms).

**Result of kit 1dfb4c0 (user, Sat 03:35; AITRACE log): the replayed buffers are exactly the ones whose audio task was never started.** Windows log: `same3 = 1` at n = 31, 34, 37, 40, 43, 52, 55, 62, 65 (period 3), each preceded by a buffer where "runs started" did not advance and an audio run was still on the worker for 17 ms (`since last run start 17`, `since last run end 34`): the audio microcode run of that frame was not reported finished by the time the next retrace came.

**Mechanism (reproduced on Linux with CPU hogs competing for the core, `KYUJO_AUDIO_TRACE=1`, now also `EVT` timeline lines: event delivery, audio run start/collected, AI_LEN read, AI buffer hand-over, each with the scheduler's four task slots A14 A18 A1C A20 as digits).** The game's scheduler (src/scheduler.c) has room for ONE pending audio task (`unk_A20`): `Sched_SubmitTask` overwrites it, `Sched_PromotePendingTasks` moves it to `unk_A18` at the next retrace only if A18 is empty. On the console the audio thread (id 4, priority 80) builds a frame in about 1 ms right after the retrace, so the task is always submitted and promoted one retrace later. On a slow or starved host (a thread that waits for the CPU) the build is still running when the NEXT retrace is delivered: the scheduler finds nothing to promote, the audio thread's next wake-up message is already queued, it builds again at once, and the second task replaces the first in A20: the first never runs, its output buffer is not rewritten and is played again (in the stream: a 3-frame-old copy). Also the end of an audio run that was still on its worker was only noticed at the idle loop or an SP_STATUS read, so a retrace could be delivered before the SP interrupt of the last run (the scheduler then saw the old audio task still active and could not start the new one).

**Change (all in the game-side hardware layer, no decomp edit):**
1. `os_sched.c` `service_events`: a video retrace is HELD BACK while the audio thread is runnable or running (`hold_retrace`, bounded to 100 ms, logged as `scheduler: a video retrace was held back for the audio thread, ms: N` when over 20 ms, at most 20 lines). The host being slow now costs a late field instead of a dropped audio task. This is what the console always did (the audio frame is over long before the retrace).
2. `os_sched.c`: `HostHwRspPump()` at the top of `HostPreemptPoll` and `HostHwRspSettle()` in `service_events`: an audio run that has ended is reported (SP interrupt, lower event number than VI) before the retrace is delivered.
3. `hw_ai.c` `start_buffer`: the run still on the worker is waited for (about 1 ms) before the samples of a buffer are taken (a buffer handed over to an idle AI at the moment its run starts).
4. Trace: `EVT` lines (first 4500) and `AITRACE` now have the retrace and SP-event counts; `SCHEDTRACE` prints the slots at each retrace (first 240).

**Linux measurement** (3 busy loops + taskset to one core, first 240 output buffers, count of `same3 = 1`): before: 2, 2, 6, 0, 5, 0 ... per run (up to 7); with only the pump/settle change: unchanged; with the retrace hold: 0, 1, 0, 0, 0, 0; plus the wait at the buffer take-over: **0 in 10 runs**. Normal runs: 0; fields 60/s; a whole battle route (bat_route.sh, 3 min): no hold over 20 ms, perf line normal. Synchronous audio (`KYUJO_AUDIO_THREAD=0`) does not cure it by itself (2, 2, 1, 4, 1, 3), which fits the mechanism (it is the audio THREAD that is late, not the microcode).

**Not run on Windows.** If the voice is still damaged: the log's `EVT` lines around the first `AITRACE ... same3 = 1` show what happened (see above for how to read them: slot digits A14 A18 A1C A20, 1 = graphics task, 2 = audio task).

**Result of kit 2d639a3 (user, Sat 2026-11-14 04:27): the Pika voice at launch is FIXED on Windows.** Closed. The retrace hold (`hold_retrace` in `os_sched.c`), the audio-run settle/pump and the settle at the AI buffer take-over stay; the `EVT`/`SCHEDTRACE` lines stay behind `KYUJO_AUDIO_TRACE=1` for future audio work.


## 43. NRO test 40: the audio fix on the Switch, and a start-up bug the Linux run caught (2026-11-14)

The scheduler/audio fix of section 42 lives in the game-side hardware layer (`src/ultra/`), which every build shares, so the Switch NRO needs only a rebuild. The first rebuild was
**invalid**: the game exited with code 70 at the start on the Linux SDL shell (the same code as the NRO). Cause: the wasm, Switch and Android builds take their window from the
shell, and `src/platform/host_window_shell.c` had no `host_window_prepare()` (added to `platform.h` for the Windows start-up fix), so wasm2c turned the missing import into a stub
that traps. Fix: an empty `host_window_prepare` in that file. New guard in `tools/native/build_native.py` (step 5a): the build stops when `report.json` lists a function that
nothing defines and `port/allowed_stubs.txt` does not name it (the list has one entry, `__LeoBootGame3`, the 64DD boot path, never reached). `check_build.sh` passes with the guard.
Delivered as `kyujo_test40.nro` (the Linux SDL shell of the same code ran 60 s without a trap; the NRO itself is untested until the owner runs it).

**Test:** start the game, listen to the first sound (Pika voice, announcer) and a battle; send `kyujo-sdl-log.txt` if anything is wrong or the game exits at the start.

## 44. Android port (2026-11-14): built, NOT run on a device

Owner direction (2026-11-14): flesh the Android port out as far as possible, following the same standards; test the NRO the next day.

**What exists** (`android/`, `tools/android/build_apk.py`, `tools/native/build_shell.py --target android`; `android/README.md` is the player-facing text):
* **One game library for all.** `shell/sdl/main_sdl.c` is the same shell as on the Switch and the Linux SDL shell; Android-only code is under `#ifdef __ANDROID__`: `main` is SDL's `SDL_main`;
  the game's folder is `SDL_AndroidGetInternalStoragePath()` (the app's private `files` folder; `rom.z64` etc. are found there like on the Switch's SD card); `SDL_HINT_ANDROID_TRAP_BACK_BUTTON`
  (Back becomes `SDL_SCANCODE_AC_BACK`, mapped to Escape: the game's menu), `SDL_HINT_ANDROID_BLOCK_ON_PAUSE=0`; full screen; the log is rotated to `kyujo-sdl-log.prev.txt`.
* **Build without Gradle** (`build_apk.py`): SDL2 2.32.10 built from source with the NDK's CMake toolchain (per ABI, 16 KiB page size, stripped); the game library `libmain.so` through
  `build_shell.py --target android` (NDK clang API 24, `-fPIC`, links `libSDL2 libEGL libGLESv3 liblog libandroid`, `--no-undefined`); SDL's Java layer plus `android/java` compiled with
  `javac --release 8` and turned into dex; manifest and icons (made from `assets/icon.jpg`) with `aapt2`; zipalign; `apksigner` with a generated debug key unless `--keystore` is given
  (the owner's choice for a release key; nothing is published). ABIs: arm64-v8a (devices) and x86_64 (emulator, Chromebook).
* **d8 problem found and worked around:** the SDK's d8 (build-tools 34, R8 8.2.2) throws a NullPointerException on every anonymous class compiled by JDK 21 (the constructor carries a
  `MethodParameters` entry with no name). `--d8-jar` takes R8 8.13.19 from Google's Maven repository (SHA-1 checked); `build_apk.py` documents it.
* **Java layer** (`org.kyujo.app`): `LauncherActivity` (Play; choose the ROM; add Game Boy ROMs/saves into the folder `GB`; backup zip of saves and settings, restore; save the log;
  on-screen controls setting), `Storage` (all file work, on a background thread so a 32 MiB ROM does not hold the screen: the system file picker gives a document, the bytes are copied into the app folder, never a path of another app; no permission is
  requested; the ROM is never in a backup), `GameActivity` (SDL's activity; its own process `:game`, which is killed when the game screen closes, because the game's static state is made for
  one run per process; the launcher is not affected), `TouchOverlay` (see next).
* **On-screen controls**: an N64 pad drawn over the game: D-pad or analog stick (setting), A, B, four C buttons, L, Z, R, Start, a Menu button. Multi-touch by pointer id; a finger that starts on
  the stick keeps it, other fingers are hit-tested at every event (sliding between buttons works, A+B and C while moving work). The overlay reports masks to `read_input` through JNI
  (`nativeTouch`, `nativeActive`); the shell turns them into one more Xbox-layout pad, "Touch controls", **only while no gamepad is connected** (the Java side hides the view when
  `InputDevice` lists a gamepad/joystick; the native side double-checks `sPad`). The game's own controller menu therefore applies to it. `HostRawInput.touch` makes the game's menu text say
  "Menu button or Back".
* **Lifecycle**: when the app goes to the background SDL (non-blocking mode) pauses its audio devices and the system destroys the surface; `pause_gate()` holds the game's picture and sound hand-over
  while paused, `gl_swap` does not swap into a missing surface, and after the return the render thread releases and takes the OpenGL context again (`sSurfaceEpoch`), which binds it to the new surface
  (read against SDL 2.32.10's `SDL_androidevents.c`/`SDL_android.c`: the context backup runs on the event thread, where our context is not current, so it saves nothing and the surface is
  freed by the system; the context itself stays with the render thread).

**Checked here:** the arm64 `libmain.so` links with no undefined symbols, exports `SDL_main` and both JNI functions with the exact names, LOAD segments aligned to 16 KiB; `libSDL2.so` has the
52 SDLActivity natives; the APK verifies with apksigner (v2/v3 scheme), `aapt2 dump badging` shows launchable `LauncherActivity`, GLES 3.0, min SDK 24, target 34, arm64-v8a;
`main_sdl.c` compiles for Android with `-Wall -Wextra` and no warning; the same code runs on the Linux SDL shell (60 s, no trap).
`tests/native/android/test_android_sources.py` (runs anywhere): every `native` method has a JNI function of the exact name and shape, the button bits of `TouchOverlay.java` equal the `TB_*` bits of `main_sdl.c`, the manifest asks for no permission and names existing classes (checked to fail when a bit or a name is changed). `tests/native/licence` passes again (it failed before on HTML/XML comment headers, three untagged fuzz files and the Switch icon). The release APK with both ABIs is 9.9 MB.

**NOT checked (no device or emulator here):** installation, the touch controls (position, feel, hit areas), start-up, rendering on a mobile GPU (Mali/Adreno differences are the main risk; the renderer
was developed on Mesa llvmpipe and the Switch's Mesa/nouveau), the background/foreground return, sound, speed (the Switch NRO speed is the only ARM measurement), the file picker flows.

**Test (owner, any arm64 phone/tablet, Android 7 or later; a controller is optional):**
1. Install `kyujo-android.apk`, open the app, choose the ROM, tap Play.
2. Does the picture show? Do the controls respond? Does the game run at speed (the log's `perf:` lines)? Is the sound right?
3. Press Home and return: does the picture come back and continue?
4. Try a Bluetooth/USB controller: the overlay should disappear and the pad work.
5. "Save the log" in the launcher and send it; also say the device model and Android version.

**Disclosed:** the SDK licences were accepted with `sdkmanager --licenses` in the build environment; the APK is signed with a generated **debug** key (installs over itself only while the key is kept;
a release key is the owner's decision); data lives in the app's private storage, so uninstalling or clearing data deletes saves (use the backup button).


## 45. Android test 1 result and the Game Boy file flow (2026-11-14)

**Owner's first test of `kyujo-android-16d94bf.apk` on a device: passed.** The picture shows, the touch controls respond and feel right, speed and sound are as expected, Home puts the game in the
background and returns without error, Back opens the game's menu. One oddity: the Game Boy files. The ROM had to be given twice and the save added by hand in the setup page; the owner wants to choose the
Game Boy files (with the save selected automatically, as on the other platforms) from the game's own menu.

**Cause:** the first design had a launcher import (files into the folder GB) and then the game's in-window browser for the same ROM again, and the save was found only when its name was the ROM's name plus `.sav`
(as on the PC, where the files sit together); a save called "Pokemon Red.sav" next to "Pokemon Red (UE) [S][!].gb" was not found.

**Change (built, not yet run on a device):**
* The game's Game Pak menu has, when the shell has a system picker (Android; `shell_pick_gb` import, `CoreFront.pick_gb`), **Choose a Game Boy ROM (and its save)** and **Choose a save file**; the in-window browser stays as
  "Game Boy ROM / Save file already in the app" and now opens in the folder GB. Windows, Linux, Switch and the browser behave as before (no picker: the old items).
* `GameActivity.pickGb` opens `ACTION_OPEN_DOCUMENT` (multiple selection), copies the files into `GB/` on a thread, pairs the save with the ROM (`Storage.importGbSet`): by name apart from the tags in () and [] and punctuation
  ("Pokemon Red (UE) [S][!].gb" ~ "Pokemon Red.sav"), or when it is the only save among the files; the save is stored as `<rom>.sav`, which the game's automatic save name finds. **A save that is already there is never overwritten**:
  unless identical it is kept as `<name>-backup-N.sav`. Several ROMs: the first is used. Answer to the game through JNI `nativeGbPicked`; the game's menu loop polls (`host_pick_gb_poll`) and inserts the cartridge exactly like a file chosen
  in the browser. The full sentence goes to a toast, a short line (the menu has one line of about 40 letters) to the menu.
* The launcher's Game Boy button uses the same pairing.
* **Checked here:** the menu side end to end on the Linux SDL shell with a fake picker (`KYUJO_FAKE_GB_PICK`, a test hook that exists only off Android) and a synthetic header-only .gb file made for the test: the picker starts, the
  answer crosses the wasm boundary (strings and ints written into the game's memory), the cartridge is inserted, the save shows as "test.sav (automatic)" (screenshots read). `tests/native/android` now also runs the name pairing and the
  save placement (including the backup numbering) on a plain JVM, and checks that the JNI method signature string the native side uses matches the Java method.
* **Not checked (no device):** the real picker, multi-select, a content provider that gives odd names.


## 46. Android test 2 result and the Game Boy page redesign (2026-11-14)

**Owner's feedback on build 2 (`0c87793`):** the Game Boy page had become too messy; it should look and work like the other builds: the save is selected automatically from the ROM's name with `.sav`, and can be modified. The
ROM could be chosen, but the save was not included by default. Windows, Linux and Switch stay exactly as validated; only Android changes.

**Cause:** build 2 added extra menu items and a separate "already in the app" browser, and a ROM picked alone left the save unset (the picker pairs a save only when it is picked in the same selection).

**Change (built, picker not yet run on a device):**
* **Isolation by compile flag.** All Android-only picker code in `host_menu.c`, `host_ui.h`, `host_window_shell.c` is behind `#ifdef HOST_SHELL_PICKER`, passed with the new `build_native.py --define NAME` only for the Android wasm
  (`and3.sh`). With the flag off the preprocessed output of those three files is identical to commit `16d94bf` (checked with `clang -E -P`), so the Windows, Linux and Switch modules are unchanged; the Switch NRO test40 stays the one to test.
  `shell/sdl/main_sdl.c` has the picker under `#if defined(__ANDROID__) || !defined(__SWITCH__)` (the Linux shell uses it only for the fake picker).
* **The page is the standard Game Pak page** (ROM item, Save file item, "Use the automatic save file name"). The system picker only replaces the file browser: **Game Boy ROM** (kind 0) picks the ROM and its save; **Save file** (kind 1) picks a
  custom save under its own name (needs a ROM first: "Choose the Game Boy ROM first.").
* **Save by default:** `Storage.importGbSet` stores the paired save as `<rom>.sav`. A ROM picked without a save now chains a second picker for the save (`GameActivity`, `REQ_GB_SAVE`); cancelling it leaves the ROM in with a new empty
  save, unless `<rom>.sav` already exists in the app. The page then shows `<name>.sav (automatic)`; a custom save (not equal to the automatic path) shows its own name.
* **Checked here:** Linux SDL shell with the fake picker (`KYUJO_FAKE_GB_PICK`, `KYUJO_FAKE_GB_PICK_SAVE`, exist only off Android/Switch) and synthetic 32 KiB header-only .gb and 8 KiB .sav files: page shows `test.gb`,
  `test.sav (automatic)`; after the Save-file pick `other.sav` plus "Use the automatic save file name" (screenshots read). The APK verifies (signature, `nativeGbPicked` in both ABI libraries, new classes in the dex);
  `tests/native/android` (JNI shapes, name pairing, save placement on a JVM) and the licence test pass.
* **Not checked (no device):** the real picker, multi-select, odd provider names, the chained second picker.
* **Disclosed:** `host_menu.c` got a final one-line reformat after the Android wasm was built (logic identical); the APK is debug-signed; data is in the app's private storage.

**Build 4 (follow-up, same day):** the owner's test of build 3 was "much better"; one thing left: the save shown on the page after choosing the ROM had the ROM's name with no extension, and the
settings line for the save must end in .sav. The cause was not found in the code path (here the page shows `<rom>.sav (automatic)`), so the ways it can arise were closed: (1) a save chosen without
.sav/.srm in its name (or one small extra file chosen with the ROM when no file is a save) is stored as `<name>.sav` (`Storage.saveName`, `MAX_SAVE_BYTES`); (2) `host_settings_fix_gb_saves` (flag
HOST_SHELL_PICKER only: Android) drops a save setting that does not end in .sav/.srm at start-up and when the Game Pak page opens, so the automatic `<rom>.sav` applies (a stale setting from build 2 is
repaired: tested on the Linux shell with `p1_save=/GB/<rom name>` in the settings: logged and the cartridge uses `<rom>.sav`). Flag-off preprocessing of host_menu.c, host_main.c, host_paths.c is again identical to 16d94bf.
If it still shows wrong on the device, the log lines "Game Boy file picker" and "Game Boy cartridge on port" give the real path.

## 47. Build 5: the Game Boy save is found by name beside the game (2026-11-14)

**Owner's test of build 4:** choosing the ROM and the save did not work and was not intuitive (the picker needed a long-press to bring the save along); "the user should not have to specify the save: it is taken from the
folder of the ROM, same file name, other ending, as on every other platform". Fair: builds 2 to 4 asked the player to do what the PC does by itself.

**Change:** `GbFlow` (new, used by the game's menu and the launcher) replaces the multiple-selection picker. Android lets an app read only what the player allows, so the player allows **one folder** once
(`ACTION_OPEN_DOCUMENT_TREE`, kept with a persistable permission; "Choose your Game Boy folder" with a plain explanation). The Game Boy ROM item then lists the games (.gb, .gbc; the folder and one level of
sub-folders); one tap; the game and the save with the same name (case ignored, .sav or .srm) from the same folder are copied into GB, the save as `<game>.sav` (the automatic name; a save already there that differs is kept as a backup);
no save beside it: a new one (or the one already in the app). Save file item: the saves of the folder are listed, the one chosen is kept under its own name. "Other folder" and "Pick one file instead"
(single file, no long-press, for a game that is in a place that cannot be allowed, e.g. the Download folder itself on Android 11 and later) are the fallbacks. `importGbSet`, the name matching and the second picker are gone.
**Not checked (no device):** the folder picker, the listing through `DocumentsContract`, a provider that refuses the folder, the dialogs. Checked: it compiles into the package, the Storage tests (save placement, names) pass, the licence test passes.

## 48. Kyujo: the new repository (2026-11-14)

**Owner's decision:** the project becomes its own repository, named **Kyujo** (the next games: Kyujo0 and Kyujo2); the decompilation by Team Hippodrome is referenced and credited, not copied; the name, the app and the repository must not use the game's or the company's words; the Android logo is also the Switch icon.

**What was done.** The working tree was copied into a new repository with a single first commit (the earlier private history stays in the old repository, which is kept). Renamed: the repository and product (`Kyujo`), the programs (`kyujo`, `kyujo.exe`, `kyujo.nro`, `kyujo-android.apk`), the settings, save and log files (`kyujo.cfg`, `kyujo.sav`, `kyujo-log.txt`, `kyujo-sdl-log.txt`), every environment variable (`KYUJO_*`), the settings key (`game_rom`), the Android package (`org.kyujo.app`, the label, the launcher text), the Switch title and author (nacp) and folder (`sdmc:/switch/kyujo`), the window titles, menus, the start-up picture (its heading now reads KYUJO), the C++ namespace and library, and the licence tags ("Dan B. Cooper"). The submodule path became `ext/decomp`
(URL unchanged: `https://github.com/team-hippodrome/pokestadium`, pinned `8efced6`).
**Not renamed on purpose:** the decompilation's own symbols and file names (our code mentions them), `port/patches/*` and `port/decomp-manifest.txt` (they are the decompilation's text and hashes), the upstream download file name, which is written once, in `dist/windows-setup/setup/components.json` (the offline kit's own zip is now `decomp-<commit>-build-files.zip`; `srctree.py` finds a zip's root folder from the manifest's paths), the ROM identification table in `tools/rom_tool.py` (names the dumps), and descriptions of the game's own features in the docs. The README says which game Kyujo works with and credits Team Hippodrome first.
**Icon:** `assets/icon.jpg` (the Android logo) is now the `.nro` icon (`build_shell.py` reads it; the old `tools/native/switch_icon.jpg`, a ball-like drawing, is gone); verified by extracting the icon from the built `.nro` (same MD5 as the file) and its title and author.
**No interaction with the earlier name (owner's decision, 2026-11-14):** the earlier name could collide with a known game's files on a system, so Kyujo reads and writes nothing under it: no copying of old settings or save files, no old settings key, no renaming of an old Switch folder, no old backup file names accepted on Android. Kyujo is a new program with its own files; a player who wants an old N64 save copies it into the new folder and names it `kyujo.sav` by hand. The only places that still hold the upstream repository's name are its credit and download address (`README.md`, `NOTICE.md`, `THIRD-PARTY.md`, `.gitmodules`, `port/decomp-manifest.txt`, `components.json`) and the decompilation's own text inside `port/patches/*`.
**A bug the rename caused, found and fixed:** `core_pass_environment` compared the first 12 characters of each variable with the prefix (the length of the old prefix): with the new prefix no `KYUJO_*` variable reached the game inside the Switch/Android shell (test hooks only; found when a screenshot test produced nothing). Fixed to 6; the screenshot test works again. Lesson: look for lengths tied to renamed strings (searched: no others).
**Checked:** `check_build.sh` (Linux 32-bit and Windows 32-bit builds, every test that runs here, the Windows image checks), the Linux program started with the owner's ROM (60 pictures/s headless), the Android source tests, the licence test, the APK (signature, package, label, JNI symbols in both ABIs), the SDL shell with the menu screenshots, the setup kit and Linux zip and AppImage packaging. **Not checked here:** the Windows program on Windows, the `.nro` on a Switch, the APK on a phone (these are the owner's next round, as before).

## 49. No old name, and what may be handed out (2026-11-14)

**Owner's questions:** (1) Kyujo must not interact with the earlier name at all (it could overlap with a known game's files on a system); the test builds are removed before the new source is tested. (2) If the setup files contain original source code, how can they be distributed?

**(1) Done.** Removed: the copy of `<old>.cfg` / `<old>.sav` (`host_main.c`), the old settings key (`host_settings.c` and its test), the Switch folder rename (`main_sdl.c`), the old file names accepted by the Android backup restore (`Storage.java`), the README paragraph. The upstream repository's name now stands only where it is the credit or the download address: `README.md`, `NOTICE.md`, `THIRD-PARTY.md`, `.gitmodules`, `port/decomp-manifest.txt` (its `source` line) and `dist/windows-setup/setup/components.json` (the download file name, written once; `Setup.ps1` and `package_windows_setup.py` read it from there). The offline kit's own zip is now `decomp-<commit>-build-files.zip`. Nothing under the old name is read, written, copied or renamed by any Kyujo program.

**(2) What was measured (not assumed).** The online setup kit contains no file that is identical to a decomp file (SHA-256 against `port/decomp-manifest.txt`); the player's computer downloads the decomp and the compiler from their official addresses and every file is checked against the hash list before use. But the 40 port patches were unified diffs, so they carried the decomp's lines around each change: 909 lines (218 removed, 691 context, about 34 KB) of the decomp's text. The decomp has no stated licence, so that text should not be redistributed.

**Fix: anchored patches.** `patchapply.py` now reads a second hunk format: `@@ -10,5 sha256=<hex> @@` then `=3` (keep 3 lines), `-1` (delete 1), `+text` (add this line). The patch names the old lines by position and SHA-256 and holds only the lines we add; applying is as strict as before (no fuzz; a moved hunk is found by its hash; two equally near matches are refused; nothing is written unless every hunk fits). All 40 patches were converted with `patchapply.anchor()`. **Proof of equivalence:** the tree made from the anchored patches and the tree made from the old unified patches were compared file by file (1,158 files): byte-identical. The full build (Linux 32-bit, Windows 32-bit, wasm for Switch and Android) was rerun from the anchored patches. The patch files shrank from 127,515 to about 102,700 bytes and no longer contain the original's lines (checked by searching every old line in the new files; the only hits are our own explanatory sentences in the patch heads). `mkpatch.py` / `srctree.py diff` now write the anchored format; `patchapply.py --explain PATCH TREE` prints a patch as a normal diff, made on the player's own copy, so a reviewer still sees everything.

**What may be handed out.** Source of this repository and the **online** Windows kit: yes (nothing of the decomp, the game or a ROM). Built programs (`.exe`, `.nro`, `.apk`, AppImage, Linux/Windows zips) and the **offline** kit: no (they contain compiled decomp code and, for the offline kit, its source files). Each player builds their own, or receives a build privately from the maker for testing. **Remaining honest doubt:** an added line that is a small edit of an original line can still echo its expression; and the decomp has no licence at all, so strictly nobody has been granted any right in it. The proper fix is the decomp authors' written agreement: ask Team Hippodrome (issue tracker of their repository) whether they agree and will state a licence. This is not legal advice.

## 50. Presentation for players and releases (2026-11-14, evening)

Owner's request: the setup and README were non-intuitive and mentioned team members and testing; make both as easy as possible for building and for releases (model: another recompilation project's README). Done: player-first `README.md` (get it per system, requirements, features, FAQ, known issues, credits), `BUILDING.md` (one section per system), `build-linux.sh` (one command for Linux and Batocera, tested end to end with the real game start), the Windows kit's `README-SETUP.txt` as a three-step guide, `docs/using-kyujo.md` as the player's guide, `docs/batocera.md` (replaces the test-round guide that shipped inside the Linux zip), `docs/README.md` (index for developers), `CONTRIBUTING.md`, `RELEASING.md`, issue templates. A release carries one file, `Kyujo-Setup-Windows.zip` (the online setup kit; same name every release so the README link never changes); built programs are never attached (they contain the game's code). Android and Switch still need their toolchains (SDK/NDK, devkitPro, wabt, binaryen, wasi-libc): BUILDING.md lists the exact commands; a one-command bootstrap that downloads pinned, hash-checked toolchains (like the Windows setup does) is the next step for them.

## 51. One-command builds for Android and Switch (2026-11-14, night)

Owner's request: the Android and Switch build instructions had too many variables; make them as easy as possible for the most users, with extra scripts if needed. Done: `build-android.sh` and `build-switch.sh` (like `build-linux.sh`) over one shared `tools/build-lib.sh`. The scripts check the host, fetch the decompilation (or use `KYUJO_DECOMP`), and build the WebAssembly C library (wasi-libc + compiler-rt builtins via `tools/toolchain/build_builtins.py`, which gained the wasm32 generic long-double block), wabt (wasm2c) and binaryen (wasm-opt) from pinned commits, then the Android SDK/NDK/SDL/R8 (one licence question; `KYUJO_YES=1` answers it) or devkitPro (`$DEVKITPRO`, `/opt/devkitpro`, or an install through devkitPro's own pacman script). Every download is hash- or commit-checked; the only settings left are the optional `KYUJO_DECOMP`, `KYUJO_WORK`, `KYUJO_JOBS`, `KYUJO_YES`. Outputs: `build/Kyujo-Android.apk` (arm64-v8a + x86_64) and `build/kyujo.nro`. The tool folder `.work` is about 3 GB for Android.

**Validated here:** both scripts from an empty tool folder (Android: SDK, NDK, tools, APK: apksigner v2/v3 verifies, package/SDK levels right, `nativeGbPicked` present in both ABIs, no old-name strings; Switch with an existing devkitPro: NRO with icon section, no old-name strings). The WebAssembly module is not byte-identical to the one built earlier with hand-placed tools (it differs by about 8 KB and by embedded build-folder paths such as the sysroot location, 60 occurrences); the earlier wasi sysroot itself is bit-identical. Not validated: devkitPro's automatic install path (the sandbox cannot reach it), anything on a real Android device or Switch (to be tested by the owner), and Setup.ps1 on Windows.

## 52. AppImage by script (2026-11-14, night)

The owner asked how to build the AppImage (it was only a Python tool, undocumented). Added `build-appimage.sh` (runs `build-linux.sh`, then packs `build/Kyujo-x86_64.AppImage`) and documented it in BUILDING.md. `package_appimage.py` no longer calls `appimagetool` (it needed `desktop-file-validate`, an extra package): it now writes the image itself, as appimagetool does: the type-2 runtime followed by a zstd squashfs of the AppDir (`mksquashfs` from squashfs-tools). The runtime is cut from the front of the official `appimagetool-x86_64.AppImage` 1.9.0 release (SHA-256 `46fdd785...`, pinned in the script; the hash was taken from a download made here, so it is trust-on-first-download). **Tested here:** the script end to end; the image starts with `--appimage-extract-and-run` and shows the no-ROM message with the data folder. **Not tested:** mounting through FUSE and running the game from the image on a real desktop (the earlier AppImage, built by hand with the old tools, was also still waiting for the owner's test).
