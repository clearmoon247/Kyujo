# Behaviour specification (reimplement from this; do not copy the prototype)

This is a description, in prose and tables, of what the earlier private prototype did and what it taught. It is the requirements list for the new implementation. It contains no code from the prototype. Where the new renderer, translator or runtime makes an item meaningless, the table says so.

Evidence level: **H** = observed working on a console (firmware 21.2.0, Atmosphère 1.10.2); **W** = written but never confirmed on hardware; **D** = deliberate design rule.

**How to read this for the Windows/Linux port (added 2026-10-24).** The evidence levels describe the *Switch prototype*; nothing in this document has been observed on Windows or Linux. Rules about game behaviour, controls, configuration, the cartridge picker and the stall acceptance test carry over as requirements. Items that exist because of the Switch (title takeover, Atmosphère, SD-card I/O, Mesa/nouveau shader-compile costs, `.nro` packaging, the translator) apply only to a later Switch build; on desktop the nearest equivalent rule is used (for example "no slow file I/O while holding a game lock" still applies). Where an item is ambiguous for desktop, ask the owner instead of guessing.

## 1. Platform and start-up

| Requirement | Level |
|---|---|
| Runs only as a title takeover (launched while holding **R** on an installed game). Applet/Album mode leaves too little memory. The README must say so. | H |
| Needs the user's own Pokémon Stadium (USA) v1.0 ROM in the data folder; any of `.z64`, `.n64`, `.v64`; the first file that matches the known dump is used. No ROM, wrong ROM, or unsupported revision: show a clear on-screen message, never a crash. | H |
| Data folder `sdmc:/switch/<app folder>/` holds the ROM, settings, `launcher.cfg`, logs, learned shader lists, saves. Name chosen by the owner. | D |
| Start-up order: (1) read settings, (2) cartridge picker if wanted (before any window exists), (3) open log files, (4) find and check the ROM, (5) create window and renderer (warm-up behind the start-up screen), (6) start the game. | H |
| Exit goes through the process-exit service call; never run the normal application-exit path while other threads still run. Exiting returns to the Home menu (hbmenu return is a nice-to-have). | H |
| The window is not resizable; docked/handheld changes must not crash. SDL events are pumped from exactly one thread. | H |
| Threads are pinned by name: graphics on core 2; game, VI, timer and RSP/audio-task threads on core 1. Thread stack size and thread detaching need Horizon-specific handling: stacks are sized explicitly (the default 1 MiB was a constraint), and detached threads are reaped by a helper because `pthread_detach` does not behave as on Linux. | H |
| The thread that runs RSP/graphics task submission must have a **higher** priority than the guest game threads. With equal or lower priority, a busy-wait in the JPEG decode path starved it and the game stalled. | H |
| No `mmap`, no `dlopen` on Horizon: the guest memory window comes from a one-time eager allocation; nothing may depend on dynamic loading. | H |
| A crash report (registers, stack sample, last log lines) is written to the data folder. | H |
| Shared memory between GPU services needs an enlarged transfer-memory size (the prototype used 32 MiB). | H |
| FAT quirks: a rename cannot replace an existing file; a copy helper that sets permissions fails. Use remove-then-rename, or write via a temporary name. | H |

## 2. Controls

| Requirement | Level |
|---|---|
| Up to four players; controllers are assigned to N64 ports in the order they appear; hot-plug works. Only one-player handheld/Joy-Con behaviour was verified. | H (1 player), W (more) |
| Layout (Nintendo 64 Online style): A→A, B→B, ZL→Z, L→L, R and ZR→R, + → Start, right stick → C buttons, D-pad → D-pad, left stick → analog stick. | H |
| `nintendo_buttons` (default on): the game's confirm is the pad's A (right), cancel is the pad's B (below). Off = by position. | H |
| `left_stick_dpad` (default on): the left stick also presses the D-pad (menus read only the D-pad). One direction at a time (larger axis wins); press at 18000 of 32767, release below 12000. The analog value still goes to the game. | H |
| Left stick dead zone about 8000 raw units, rescaled to [-1, 1] with Y inverted; right stick C-button threshold 16000; ZL/ZR analog triggers count as pressed above 16000. | H |
| A port with no pad but a configured Transfer Pak cartridge still answers as present (idle). Port 1 always answers as present before any pad is opened. | H |
| Rumble: while the game keeps rumble on, re-arm a short (≈100 ms) pulse every video interrupt. | H |

## 3. Settings file

`key=value` lines, `#` comments, defaults on any problem, warnings logged. Implemented in `src/common/config.*` (tested). Keys carried over: `resolution_factor`, `widescreen`, `audio_buffer_frames`, `nintendo_buttons`, `left_stick_dpad`, `gb_picker`, `picker_wait_ms`, `gb_view`, `vi_hold`, `vi_field`, `splash`, `shader_warm_ms`, `shader_warm_extra`, `shader_prune`, `shader_opt`, `stall_boost`, `log_buffered`, `gbcart_ring`, `vi_probe`.

Not carried because they were specific to the old stack: `gl_copy_color`, `gl_swap_mode` (GLideN64 options), `interp_discovery` and `hash_miss_dump` (recompiler runtime features). The new renderer may define its own keys.

Semantics worth keeping:

* `widescreen`: 2 stretch everything to 16:9 (default); 1 widen 3D scenes only (experimental, menus and transitions glitch); 0 4:3 with black bars.
* `resolution_factor`: 1 = 320×240, 2 = 640×480 (default), 3 = 960×720.
* `audio_buffer_frames` ≥ 1024: smaller buffers starved audio on this platform.
* `gb_view`: 0 as drawn; 1 bezel centred in Game Boy proportions; 2 bezel stretched to the whole window (default); 3 bezel in a centred 4:3 box.
* `vi_field`: how the interlaced 480i menus are shown: 1 = always upper field (steady, default), 2 = lower, 0 = alternate (visible jitter).

## 4. Video-interface behaviour the game forces on any renderer

* **Mode-change gap (H):** on pick-Pokémon → pick-team the game switches between 320×240 and 640×480 and then submits no display list for about 100 presents (≈1.7 s). A renderer that can only show RDRAM then shows garbage. Rule: after a mode change, hold the previous picture until the first rendered picture appears, for at most `vi_hold` presents (default 150). Risk: a screen that legitimately shows RDRAM content for a long time after a mode change (Game Boy Tower entry) would show the old picture for up to 2.5 s; no regression was reported.
* **Interlaced menus (H):** the 480i menus alternate fields; showing one field removes the jitter.
* **Framebuffer readback (H):** reading the colour buffer back into RDRAM every frame cost hundreds of ms on page-transition display lists; off by default. Reading the whole window for the Game Boy view was also too expensive: find the bezel from one row and one column, not the whole frame.
* **First use of a 640×480 framebuffer (H):** creating it costs 100–370 ms at the first switch; consider creating it at start-up.

## 5. Shader/pipeline warm-up and the start-up screen

Facts (H): on Mesa nouveau, each combiner shader takes 100–500 ms to compile on the GL thread; there is no program-binary support and no cache that survives a restart (the disk cache needs an ELF build-id, which a homebrew `.nro` lacks); `GL_KHR_parallel_shader_compile` does not make compilation asynchronous. Without a warm-up the game froze for that long the first time each effect appeared (Gym Leader Castle).

Requirements:

1. Before the game starts, build the pipelines known to be needed, within `shader_warm_ms` (default 45000; 0 = off, effects then build on demand and freeze the game each time).
2. Sources of keys: a built-in seed list, a file of keys that grows by itself, and (option `shader_warm_extra`) keys a scan of the game says it may use: 0 none, 1 likely, 2 all.
3. Learning: record every combiner key the game actually draws with, in a file that only grows, written off the render thread. Pruning (`shader_prune` = 1, default): once 40 used keys are recorded, warm up only those plus the self-growing list; a never-seen effect is built once on demand (150–500 ms) and remembered. 2 = also keep the seed list; 0 = always the full list (≈135 shaders, ≈24 s).
4. `shader_opt`: set the driver's compiler optimisation level while compiling (nouveau default is 3; 1 is the default here; -1 leaves the driver default). Effect on start-up time was never measured.
5. Record per-shader build times (slowest first) in a file and log a summary line (mean, median, 90th percentile, maximum).
6. A shader-compile stall must be impossible to confuse with a game stall: log `shader #N: build X us`.
7. Not done and risky: compiling on worker threads with shared GL contexts.

Start-up screen (W): while the warm-up runs, show a "Please wait" picture in the style of the game's own menus (menu background, the game's font, a blue panel, a progress bar and "n / total") instead of a black window. It is drawn by the CPU from assets read from the user's ROM at run time, uploaded as a 640×480 texture and stretched to the window (full window when `widescreen=2`, else 4:3 with bars). It refreshes at most every 500 ms, restores every piece of GL state it touches, and ends with a last "Starting the game…" frame. `splash=0` leaves the window black. Without a ROM skin it is skipped and a log line says why.

## 6. Cartridge picker (Transfer Pak / Game Boy Tower)

Needed because Stadium 1 reads Game Boy cartridge files assigned to controller ports; it reads **Generation 1 only**: Red, Blue, Yellow, Japanese Green (not Gold/Silver/Crystal, which belong to Stadium 2).

| Requirement | Level |
|---|---|
| `launcher.cfg` holds `pN_rom` / `pN_save` for N=1..4 (module `src/common/launcher_cfg.*`, tested): relative paths resolve against the config's folder, rooted paths against the device root, a `sdmc:` prefix is accepted when reading and rewritten as a rooted path; other lines are preserved; the previous file is kept as `.bak`. A hand-written example file ships in the repository. | H |
| Before the game starts a screen scans the SD card for `.gb`/`.gbc` ROMs and `.sav`/`.srm` saves: the data folder (3 levels), `sdmc:/roms` (4 levels) and the card root (2 levels, skipping system folders such as `Nintendo`). Reads only the first 0x150 bytes of each ROM; bounded by an entry limit (≈30,000). | H |
| Recognises Generation 1 ROMs from the cartridge header; counts other Game Boy files separately; checks a save by size (32 KiB) and the Generation 1 main-data checksum; shows trainer name and ID for valid saves; pairs a ROM with the best save (same file name, same folder, any valid save, then the rest). | H |
| Opens by itself on the first start when ROMs are found. Later each start shows a short prompt (`picker_wait_ms`, default 1500): press or hold **L, R, ZL, ZR or −** to open it, otherwise the game starts. `gb_picker` = auto / always / never. | H |
| Keys: Up/Down (or Left/Right) pick a port, **A** choose cartridge then save, **X** change only the save, **Y** clear the port, **+** save and start, **B** or **−** start without changes. | H |
| Look: with the user's ROM present, drawn like the game's own "Game Pak Check" menu (font, rounded panel corners, coloured cartridge icons for Red/Green/Blue/Yellow, "1P"–"4P" labels, selection arrow, repeating logo tile of the menu background), read from the ROM at run time and kept in memory only; otherwise a plain text screen. Nothing from the ROM is stored in the repository or the build. | H |
| The save file is written back during play; the README must tell users to back up the original `.sav`. | D |
| The Game Boy Tower screen itself: default view stretches the bezel over the whole window (`gb_view`). Stepped motion, centring and black side bars were all problems in the prototype and must be checked. | H |
| The Transfer Pak access log is **off** by default: writing it while holding the pak's lock froze the game for 4–11 s (≈0.93 ms per entry). | H |

## 7. Logging and diagnostics (design rules)

* **No diagnostic may do SD-card I/O while holding a lock the game can take, and none may write one line per write call.** Each Horizon write costs about 4 ms. Three separate freezes in the prototype came from this (the Transfer Pak ring dump, the "hash miss" log flood, a per-frame transition dump).
* `log_buffered` (default on): two log files (main and error output) go through 32/64 KiB memory buffers emptied by a background thread every 50 ms; cost: up to 50 ms of log lost after a hard crash.
* A heartbeat line every 10 s with counters: video interrupts (normal ≈ +600), display lists sent (≈ 300), presents, audio buffers (≈ 600), time inside the renderer; a window where audio or display-list counts are well below normal while video interrupts are normal is a **game-side stall**.
* A stall sampler records which guest function was running when the game stopped making progress, and a script turns the addresses into function names. A scheduler watchdog logs `prof end: stalled N ms`.
* Slow display lists (over a threshold) are logged individually (first 80).
* Keep the previous run's log (`.prev`).

## 8. Performance lessons (known costs on this hardware)

* Cup-load and menu display lists of 190–560 ms exist (texture uploads or framebuffer creation on first use suspected): measure, do not assume.
* Per 10 s in battle: display-list processing 1.3–3.0 s and presents ≈0.4 s were normal.
* A CPU boost request during loading stalls (`stall_boost`) is cheap to offer.
* Memory is tight in title takeover: the renderer, Mesa, the ROM and the guest window must fit together.
* The prototype's binary was ≈200 MB at the smallest optimisation setting; code size is a design target of the translator, not an afterthought.
* A previous bug class: audio starvation below 1024-frame buffers; a gfx-queue deadlock that caused a black screen (pending screen updates must be queued, not dropped or awaited under a lock).

## 9. Knock-out stalls (regression test for the whole port)

In Gym Leader Castle the prototype froze at each knock-out for 4–11 s, then ≈1.3–1.6 s, from three separate causes (shader compile, Transfer Pak log dump, log flood at an unknown overlay load). Acceptance test for the new port: play Gym Leader Castle to three knock-outs; no heartbeat window may show a stall above ≈30 ms, and no overlay may be "unidentified".

## 10. Data-folder migration

The prototype renamed its old data folder at first start. A new project with a new name has no installed base, so this is **optional**: a one-time import from the prototype's folders (ROM, saves, `launcher.cfg`, learned shader lists) can be added if the owner wants it.

## 11. Build experience

* One command from a ROM to an `.nro`, with a pre-flight check of the ROM (any byte order, converted to `.z64`), the machine (disk, RAM, tools, network) and the toolchain (devkitPro, or a Docker image), and a timing line per stage.
* On failure write a report with the home path replaced by `~` and no ROM data.
* Windows: a WSL2 route first; a native route is attractive now because the decomp ships Windows IDO binaries and its Makefile has a native-Windows branch.
* Parallel jobs are bounded by RAM (about 3 GB per big compile in the prototype); order big files first.
* Original icon generated by a short program; `NOTICE.md` states licences, the no-affiliation line, and that no ROM content is in the repository.

## 12. Things deliberately left out

GoldenEye-specific features (an Invincible option, a one-controller multiplayer gate) belong to a different game. The overnight soak test carries over as a general idea: run unattended for hours, watch heartbeat counters and memory, fail on a stall or leak.
