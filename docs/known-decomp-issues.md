# Known issues in the decomp's C that matter for a native build

The decomp's C is written to reproduce the console's machine code, not to be correct C. A few constructs that were harmless on the
console misbehave when the same C is compiled for x86. Fixed ones are port patches (see `port-patches.md`); this file lists what is
known, what was only *read* and what is still unchecked. Nothing here has been compared with the console itself.

## Fixed by a patch
| Problem | Where | Patch |
|---|---|---|
| Overrunning arrays / data referenced only through an overrun | `gBaseStats`, `D_800780B3` (aliases in `tools/native/gen_platform_symbols.py`, `NAMED_SHIFTS`) | alias, no patch |
| Functions without `return` whose result is used | 35 functions | 0015 |
| Read through an uninitialised pointer | `BattleAI_SelectBestScoredMove` (`ptr->unk_57 * 0`) | 0016 |
| Store one past the end of an array that only works because the console's linker put another variable right behind it ("layout accident", see below) | `sBattleSceneStateTickSlots[7]` (battle animation ticks) | 0017 |
| same | `D_8122B2D8[2]` (GB Tower emulator buffer pointers; entry 2 is really `D_8122B2E0`) | 0018 |
| A garbage value that is below every console address (0x80xxxxxx) but above every native address ("address-space accident", see below) | `main_pool_pop_state`, left-chain walk (crash when GB Tower is left) | 0019 |
| Layout accident: a loop steps back 0x10 bytes from a title string to reach the one before it, which is only valid if the four title strings are adjacent | `GbPak_DetectSupportedCartridgeVariant` ("Transfer Pak is not set properly") | 0020 |
| Data pointers stored as raw bytes holding console link addresses (the first case where the layout is baked into DATA) | GB Tower sound-effect stream table `D_81228654` (crash on the first sound effect) | 0021 |
| Layout accident inside RSP code: the colour RSP program loads its constants from "program start + 0x9F0", which needs the constants array to follow the program with no padding (native alignment added 4 bytes) | GB Tower colour program `D_812297C0` / constants `D_8122A1A4` (Pokemon Yellow showed a flat blue picture) | 0022 |
| Layout accident in DATA: the move table (165 three-byte entries) is declared as three arrays that the console's linker put back to back; the native 8-byte alignment put 4 bytes of padding after the first, so moves 69..165 were read 4 bytes off | `D_800780B4` / `D_80078180` / `D_800781E0` (`3D140_2.c`; found by the array-bounds build, index 258 into a 204-byte array) | 0023 |
| C bit-fields read on x86 with the opposite bit order to MIPS: the Game Boy sound registers `NR10..NR52` are read through the struct `GbApuRegs` (`GB_APU_REGS.power` ...). `power` (bit 7 of NR52) read bit 0, so the emulator believed the sound was off and ignored every register write: the Game Boy Tower was SILENT | `gb_tower_emulator_86CB0.c` | 0024 |
| Data symbol that the decomp names by its console address but that lies OUTSIDE every variable the port defines, or inside a code overlay | `D_8120D7F3` (= `D_8120D820 - 0x2D`, INFERRED from the access pattern, not documented), `D_8120EA80`, `D_8120EA86` (documented interior aliases of `sGbApuRegs`) | aliases in `gen_platform_symbols.py` (`NAMED_SHIFTS`), no patch |

### The "bit-field order" class (patch 0024)
MIPS puts the first declared C bit-field in the most significant bits of a byte, x86 in the least significant. `tools/native/be_memory.py` refuses
a file that uses bit-fields unless it is listed in `port/bitfield_known.txt`; that list is now EMPTY (0024 removed the last entry). Found when the first
Windows tester reported "GB Tower has no audio": the same silence reproduced on Linux, and the gdb traces showed sound-register writes with real values
reaching `func_8120806C` while every buffer given to `osGbSetNextBuffer` was zeros. After 0024 the Linux device-rate WAV of the tower contains tonal
music (spectral peaks at note frequencies); it has not been listened to. Two libultra files with bit-fields (`sprite.c`, `spriteex*.c`, `syncputchars.c`)
are not compiled; if the linker ever asks for them they need the same treatment.

### The "layout accident" class
The console's linker put the decomp's variables in the order of the source files; the native linker does not. Code that writes past the end
of an array (or that reads through a symbol whose address is only meaningful next to another variable) therefore hits a *different* variable
natively. Found so far by running the game: 0017 (a retired callback slot overwrote the active-Pokemon table: crash after minutes of
battle), 0018 (the GB Tower emulator state pointer was overwritten: the emulator waited forever on garbage), 0020 (a title-table walk that needs adjacent strings), 0021 (pointers stored as raw address bytes), 0022 (an RSP program that reads its constants at a fixed distance from its own start), plus the three aliases
`gBaseStats`, `D_800780B3`, `D_8120D7F3`. There is no tool that finds these; `tools/native/check_struct_layout.py` compares structure sizes and field
offsets, not where variables are placed. Each one so far showed up as a crash or hang minutes into a particular feature, so **more are likely in code that has not been played**.

### The "stack-slot padding accident" (build-tool rule 7, not a patch)
The console's compiler gave every local array a whole number of 4-byte words, so a text buffer declared one or two bytes too short for what is written
into it (usually forgetting the terminating zero) still stayed inside its own slot. clang places a local `[2 x i8]` in exactly two bytes, and the
bytes after it belong to the next local variable. Found in `BattleHud_DrawLevelNumber` (`battle_engine_2FA4D0.c`): `s8 sp58[2]` receives
`sprintf(sp58, "%d", level)`, which is 3 bytes for levels 10 to 99 and 4 for level 100. In the native build the stored pointer `var_s2` sits right
after the buffer, and the pointers are held in big-endian order, so the zero byte cleared the pointer's most significant byte
(measured on Linux: `0xF529DBDA` became `0x0029DBDA`). The loop then read a "string" from wherever the damaged pointer pointed and drew a glyph
for each byte, using `temp_s4[byte - '0']` as the texture pointer: garbage glyphs after the level in the stat boxes ("10 ~~~ OK"), display lists
whose `G_SETTIMG` operand is pixel data, and the interpreter's "image address outside the program" lines. It was intermittent because what the
damaged pointer pointed at depends on where the operating system placed the thread's stack. The same shape exists in
`BattleHud_DrawHpFractionText` (`s8 sp70[7]` receives `"%3d/%3d"`, 8 bytes with the terminator).
Fixed for the whole class by `tools/native/be_memory.py` rule 7: local `[N x i8]` slots are rounded up to a multiple of 4. Evidence: the disassembly
of the built function (buffer at `ebp-0x20..-0x1d`, pointer at `ebp-0x1c`; before the rule: buffer `ebp-0x1e..-0x1d`, pointer `ebp-0x1c..-0x19`
with the zero landing on `ebp-0x1c`), the unit test `test_be_memory.py` (`test_stack_slots`), and the Linux runs listed in the investigation log.
Not covered: an overflow of more than the rounding gives (it would also have overwritten a neighbour on the console).

Found together with it: build-tool rule 6 (a function that runs off its end returns 0) had never run, because it looked for the return slot on the first line of the function and clang prints `entry:` first. It is active now (unit test `test_return_slot`). Its effect on play has been checked only by one Linux run that reaches the battle screen without problems; it has not been compared with the console.

### The "address-space accident" (patch 0019)
`main_pool_pop_state` walks the pool's left chain with `while (listHeadL <= argptr)`. After GB Tower, `argptr` is a `prev` pointer read from
bytes that the emulator's staging DMA overwrote with ROM data: `0x654D2A32` (US ROM bytes at 0xDFA88; computed from the DMA ranges, **not
observed on a console**). On the console that value is a harmless "address" below the pool, so the walk stops; natively the pool is at a
low address and the value lies above it, so the walk continued into garbage and crashed. The patch also requires the pointer to lie inside
the pool. **The staging DMA itself overruns its buffer** (ROM 0xADC80..0xF4130 = 0x464B0 bytes written at heap+0x80000, past the end of the
0x1FEAE8 block by 0x146A8 = 83,624 bytes). That is the decomp's behaviour on the console too; it was left as it is (the walk is bounded instead)
so the port keeps the console's behaviour. Raised here so the team can decide whether to fix the overrun itself.

### Code overlays keep their variables between loads (port design, not a decomp bug)
On the console every load of an overlay copies its data from the ROM and clears its zero-filled part. Natively the overlay's variables are plain
statics and kept what the last visit left, so the second visit to a screen started from the first visit's state (found when the Area Select
screen was black after leaving GB Tower, even after just backing out of the cartridge screen). Fixed in the port layer (not a patch): each
overlay's variables are placed in their own linker sections (`tools/native/be_memory.py`, `assign_fragment_section`), the start-up values are
saved once (`host_fragments_snapshot`), and `HostNoteFragmentLoaded` restores them at every load. Limits: variables that live outside an overlay's own
source files are not covered; a second load of an overlay that is still loaded would reset the first copy's variables (the console would have kept two sets).
Several directories can share one overlay slot (e.g. `1` and `gb_tower_emulator` at 0x81200000); all of them are reset when that slot is loaded.

## Uninitialised reads that were found by `tools/native/uninit_scan.py` but NOT fixed
The tool reports 29 findings with `--build <build dir>`. Those read so far (by a person reading the C, not tested on hardware):

* `MiniFx_EmitParticles` (`minigame_actor_framework_119FB0.c`): the loop `for (i = 0; i < sp2C; i++)` uses `sp2C`, which is never assigned.
  Natively the count is whatever the stack held (an `s16`, so up to 32767 particles). The likely intent is the emitter's particle count
  (`arg0->unk_24`, which the first branch passes to `Particle_AllocChain`) but that is a GUESS and not checked against the ROM's code.
  **Not patched; check when the mini-games are played** (symptom: hang or pool exhaustion when a particle emitter fires).
* `BattleAI_...` call `BattleAI_ScoreSwitchUrgencyFromThreatMove(arg0, arg1, sp69)` with `sp69` never set (`u8`, used as a move number).
  Effect: the AI's switch scoring uses an arbitrary move; no crash seen, not fixed.
* `MiniFx`-style visual randomness: `spA8` in `battle_engine_357C30.c` (phase of a wobble) - visual only.
* `clefairy_game.c`: `var_a3 = var_a3;`, `metapod_game.c`: `var_s5 = sp40` (only for an invalid difficulty), `table_view.c`
  `Text_CompareU16Terminated` (`tmpa` read before it is set, result apparently unused), `text_system.c` (`replacement`).
* The "sometimes uninitialised" findings (audio command parameters, `jpeg_stream.c`, `memory.c`, lab list ...) were NOT read yet.

The rest of the list is open work, not a statement that those places are harmless.


### Windows: the program must not be moved (found by review, 2026-10-25)
The game keeps pointers in 24-bit fields, so the whole program image must lie below 0x01000000 at its own link address (`tools/native/imagelimit.py`).
The first Windows builds had the "can be moved" (ASLR, DYNAMIC_BASE) flag set by the linker, so Windows could have loaded them at a random address
and the game would have failed in confusing ways. Fixed: `--no-dynamicbase` in `tools/native/targets.py`, a check in `tools/native/pe_check.py`
(the flag is off) and a start-up check in `src/platform/host_main.c` (the loaded image is below 0x01000000, otherwise a message box says so).
**None of this has run on Windows**; it is verified only by inspecting the built file. Any Windows exe handed out before 2026-10-25 13:00 has the flag set.


## Array-bounds checking (development tool, added 2026-10-25)
Every layout accident so far was found by playing until something crashed. To look for them without waiting for a crash, the game side can be
compiled with clang's `-fsanitize=array-bounds` in report-and-continue mode:

```
PORT_EXTRA_CFLAGS="-fsanitize=array-bounds -fsanitize-recover=array-bounds" \
  python3 tools/native/build_native.py --decomp <decomp> --profile linux32 --sysroot <sysroot> --out <separate dir>
```

Every index past the DECLARED size of an array then calls `__ubsan_handle_out_of_bounds` (`src/platform/host_ubsan.c`), which prints the source
line, the index and the array type to stderr (each site at its 1st, 2nd, 4th, 8th ... hit, so the log stays small but is *sampled*: a rare
large index can be missed). `tools/native/ubsan_triage.py <log> <tree>` lists each site once with its source line. A human then decides for each:

* **benign**: the array is a member of a structure whose real size is larger than the decomp declares (`unk_0A[1]`, `moveCues[165]` with
  the documented aliasing, `audioInfo[2]`), or one table the decomp split into several neighbouring symbols that ARE adjacent in the
  native program too (`D_84210DA0` + `D_84210DD8`: checked with `nm -n`, 0x38 apart exactly as on the console);
* **suspect**: a stand-alone variable indexed past its end, where the native neighbour may differ from the console's.

What this found, with the scripted battle of `docs/test-checklist.md` run for 25 minutes (three runs, different timing):
* **a real layout accident**: patch 0023 (the move table).
* about 70 distinct out-of-range sites in total; all the others examined were benign by the rules above. The ones read in detail:
  rental team selection (`D_84210DA0[4]`, `D_8423D408[0][i]` with `i` up to 11 = the second row of the same 2 x 6 array,
  `D_84211574[12]`/`D_8421158C[12]` read one entry past the end for one animation frame: the same on the console, value unused afterwards),
  `battle_order_select.c:251` (`D_8480369C[row][3]` reads the first entry of the next row of the same array), `battle_prep_397B10.c`
  (`unk_10[0..7]` runs into `unk20`, which is the second half of an 8-entry pointer array), `battle_engine_361050.c` (members of the
  `gBattleAI_ActivePokemon[2]` elements), `session.c`/`battle_engine_main.c` (`unk_028[1]` is the start of a 6-entry array inside
  `ExtendedRosterInfo`), `audio_stream_channel.c` (`unk_1598[1]` is a buffer that extends to the next member at 0x25C4), `3D140.c`
  (`audioInfo[2]`, three buffers), `geo_render.c` (`unk_000[1]` is a stack of vectors), `battle_engine_359F90.c:789` (`unk_5C[6]` filled
  with 8 bytes: the 2 spare bytes `unk62` follow).
* also read and found benign: `battle_prep_39ED60.c:251` (`unk_28[4]` plus the 4 spare bytes `unk2C` that follow it in the same structure),
  `battle_engine_361050.c` `D_843C5310[n].unk_00[i]` (inside one 0x20-byte `SpeciesLevelUpLearnset` element), `3D140_2.c:661`
  (`D_80078410[row][col]` is data of the console's main image that the port does not compile from C, so it is whole), and
  `conteepwrite.c:151` (libultra's `ramarray[15]` followed by `pifstatus`, by design).
* **coverage limit**: the tool only sees the code paths that were played (one scripted battle and the menus). Sites on paths that were not
  played, for example other cups, Gym Leader Castle trainers, Event Battle, most of the Pokedex and the mini-games, were never reported
  because they never ran, not because they were checked. **It does not prove that no other layout accident exists.**
* It also does not see pointer arithmetic (`(u8*)&a + n`, `memcpy` across variables) or overruns inside a single declaration; the 83,624-byte
  DMA staging overrun of patch 0019 is of that kind.

A related, cheaper audit (not a tool in the repository): for every pair of data symbols that touch on the console (`b = a + sizeof a`, using the
address in the symbol's name or in `linker_scripts/us/symbol_addrs*.txt`), compare with the native addresses (`nm -nS`). About a third of the
2,400 such pairs are padded or reordered natively, nearly all of them harmless because nothing indexes across them; the list is only a
starting point, since the code that does cross is the interesting part. The move table above is one of them.

### A garbage texture address (found by the same runs)
Several long runs of the bounds-checking build ended in, or reported, a `LoadBlock` from an address outside the program: `0x00FFDB4B` and
`0x00006150` (both crashed the renderer before the guard existed), later `0x00000000`, `0x000038F0` and `0x00FFEDA5` (refused and logged).
The renderer, the video output and the RSP DMA had assumed that any address below 16 MiB was valid memory. They now ask `host_game_range_ok`
(`src/platform/host_range.c`, which reads the program's own segment table) first, skip the access and print `first bad RDRAM access` once.
The display-list interpreter also prints (at most 8 times) `gfx: image address outside the program: command ... operand ... = segment N offset
... segment base ...`. One such run printed, again and again for four different display lists:
`command 0xFD (SetTextureImage), operand 0xFFFFEDA5 = segment 15, offset 0xFFEDA5, segment base 0`. So the display list refers to segment 15
whose base was never set (0), with the offset `-0x125B` as a 24-bit number: the game computed "null pointer minus 0x125B". On the console the
RDP would then read whatever lies at (0xFFEDA5 mod RDRAM size); here the access is skipped and that one texture is not loaded. The
earlier values (0, 0x38F0, 0x6150) look like the same thing with other offsets. Which game code produces the pointer was NOT found.
It was seen only with the bounds-checking build (slower; 5 of its 11 long runs showed an address outside the program); two 8-minute runs of the normal build of the same scripts
printed nothing, which does not show that the normal build never does it. On Windows `host_game_range_ok` uses the PE header
(`SizeOfImage`); **that code has never run.**

### NaN in the battle camera (found by the same runs; NOT fixed, effect unknown)
`MathUtil_Atan2Lookup` indexes `gArctanTable[(s32)(y / x * 1024 + 0.5f)]`. In the scripted battles it was called with `y` = NaN (at least 1024 times
per run, 4096 in the longest; scripts 5, 6 and 7, bounds-checking build) from `BattleScene_IsOwnerInViewWedgeAlt` <- `BattleScene_UpdateOpponentVisibilityCulling`
<- `BattleScene_UpdateOwnerVisibilitySingle`. A debugger dump at one such call showed the stage camera (`sStageCamera`, at `+0xA8` eye and
`+0xB4` target) holding garbage such as `[2.6e19, 13.0, 0.0]` and `[+inf, -inf, NaN]`; later in the same battle the camera is written with
sensible values (a pan from 145 to -37 was traced). So the NaN occurs in frames where the camera has not been set up yet; the origin of the
garbage (an uninitialised or stale value, the port's zeroed memory versus the console's RDRAM, or an ordering that is the same on the
console) was not found. Two consequences are known:
* `(s32)NaN` is `0x80000000` on x86 but `0x7FFFFFFF` on MIPS; the table index is then element 0 here and element -1 on the console (the 2 bytes
  before the table), a different angle. Float-to-integer conversion of NaN or out-of-range values is a general difference of this kind;
  it was not searched for further.
* All comparisons with NaN are false, so `BattleScene_IsOwnerInViewWedgeAlt` returns 0 and the opponent's model is flagged hidden for those
  frames. In the screenshots taken during battles the opponent was visible, so the effect did not last, but the number of affected frames
  was not measured.

## Renderer findings from the first Windows test (not decomp bugs; fixed in the port's own code)
* **Dark Pokemon**: the game draws Pokemon with a modelview scale of 0.1 and relies on the microcode normalising the transformed light direction.
  `f3dex2.c` used the scaled direction, so lighting came out about ten times too dark. Fixed; the regression test is `tests/native/render/rdp_gbi_check.c`.
  The exact fixed-point scaling of the real microcode (about 1%) is not verified (`docs/rdp-notes.md`).
* **Vertical shaking in menus**: the interlaced menus alternate two fields whose first line differs by one line; the scan-out ignored it.
  Fixed in `vi_out.c` from libultra's mode tables (field 0 `vStart` 35, field 1 37). Measured on Linux: the picture shift between frames went from +-1 line to 0.
* **Blinking graphics (Windows only)**: NOT reproduced here, because the Linux build has no window. The old window code painted straight onto the window
  (erase, then draw), which flickers. It now draws into a memory bitmap and copies it once. Unverified until the Windows retest.
* **Fuzzy battle text (Windows report, normal tournament battle)**: the sampler moved every bilinear sample by half a texel (an OpenGL habit), so 1:1 text
  rectangles (7 x 12 glyphs, IA8, `dsdx = 1.0`) were blended 50/50 with the neighbouring texel. Fixed from the manual's average-mode statement
  (`docs/rdp-notes.md`); the same before/after frame was compared on Linux (crop of "There's no will to fight!": dim and blurred before, crisp after).
  Two other causes of blur are on the WINDOW side and unverified on Windows: the old paint stretched 320-wide battle pictures with GDI's
  HALFTONE mode; the new `scale.c` does a sharp-bilinear scale in the program, with a unit test.

## Speed findings from the first Windows test (reported: laggy sound in menus/overworld, ~2 minute "loading" in GB Tower, laggy first battle)
Linux sandbox numbers (2 slow cores; NOT the tester's PC): before the work below, display lists took 45-60 ms each (about 35 fields/s in
battles, 60 are needed); after, 24-35 ms (about 40-50 fields/s). The program is still slower than real time in battles on that machine, so
it is NOT claimed fast enough on Windows; the new speed monitor (`kyujo: perf:` lines in the log, every 5 s) will show the real numbers.
* **Renderer**: profile-driven: the colour combiner is decoded once per Set_Combine_Mode instead of per channel per pixel, texels are cached per
  tile, triangle edges are stepped incrementally, RDRAM bounds checks are inlined. A self-verifying build (`-DPS_RDP_VERIFY`) recomputed every
  result the slow way during a full battle: no mismatch.
* **GB Tower "Loading" (~2 min)**: HYPOTHESIS, not measured on Windows. The game waits for controller-bus replies that take about 1.15 ms of console time;
  Windows rounds condition-variable timeouts up to its clock tick (about 1 to 15.6 ms), which would make each wait many times too long. The scheduler
  now busy-waits the last 2.5 ms before a device deadline on Windows (`OS_SPIN_NS`, `host_sched.c`). Costs CPU while waiting.
* **Window**: the game thread used to hold a lock during the slow GDI stretch; it now only copies the picture (`host_window.c`, `scale.c`).
  HYPOTHESIS for the laggy menu sound; unverified.

## Third Windows test (user's log, 2026-10-25 evening): what the log showed and what was tried
* Battles: about 60 fields/s most of the time, down to ~27 in some animations. Menus before the Game Pak Check: about 40 fields/s (31 ms per
  menu frame). The sound task runs at 60/s in battles but only ~20-27/s in the slow menus, which matches the "laggy audio" report.
* The menu frames are made of ~1000 small 2-cycle textured triangles (19-26 ms per menu frame on the sandbox, measured with
  `KYUJO_RDP_PROFILE=<ms>`, see `src/render/rdp.c`). Per-primitive cost is about 200-280 ns per pixel for 2-cycle textured triangles.
* TRIED AND REVERTED: running graphics tasks on a background worker thread so the game thread (and audio) would not wait for them. With
  yield/resume it failed intermittently (about half the runs): the game rewrote display-list memory while the worker still read it, so a
  sub-list changed during its task and the picture filled with garbage addresses. Not fixed; the change was NOT committed. Any future
  attempt must first make the game's display-list buffers safe against that (the scheduler must not reuse them until the RSP task has
  completed, which the game only signals through the RSP interrupt).
* "Image address outside the program" messages in battles (segment 15 operand 0xFFFFFFFF or 0xFFFFEDA5 with no segment set): these point
  to garbage texture pointers in the game's display lists. Not explained yet. They are the most likely cause of the wrong Pokemon level
  and status graphics; NOT verified.
* GB Tower "Loading" finishing before the Game Boy title could play: NOT investigated yet.

## Fourth round (2026-10-25 late): bad texture addresses and GB Tower loading order
* Bad texture addresses ("image address outside the program", segment 15 operands 0xFFFFDB4B / 0xFFFFEDA5 / 0xFFFFFFFF): NOT reproduced
  in the single-thread build with the scripted run (6 short runs, and a 15-minute battle run with the diagnostics on). The target
  display-list memory is in the game's pool (`gPool`), and the sub-lists it points to contain 0xFF words, so the game wrote or reused that
  memory. Diagnostic kept: `KYUJO_GFX_BAD_CONTEXT=1` prints the segment table and the last 24 commands at each bad address (dev only).
  Forcing a per-field delay (`KYUJO_SLOW_MS`) did not reproduce it. The user's log has the bad addresses in battle with their own input
  sequence; the exact trigger is still unknown.
* GB Tower loading on Linux, timed from pressing A on the cartridge: "Loading..." with the Game Boy boot logo, copyright and GAME FREAK
  intro for ~30 s, then the Red title screen while the bar still says "Loading...", and the "Button: Menu" prompt appears ~2 s later
  (between samples at 29.6 s and 31.9 s, screenshots in the scratchpad). The decomp gates the state on the cartridge ROM read and checksum
  (`gb_tower_emulator_7F9A0.c`, `GbPak_ReadRomAndChecksum`). Whether the console shows the prompt only when the title is playable is NOT
  known; it needs a reference from a console.
* Menu speed and menu audio lag: O3 gives no gain (measured). The remaining renderer cost is per-texel work (texel fetch, bilinear sample,
  combiner). The real fix is an RSP that runs while the CPU goes on, with the game's yield requests answered. A worker-thread attempt failed
  (see the third-test section above) and was reverted.


## Fifth round (2026-10-26): graphics tasks run in slices (single thread, no yield before)
* Change: the RSP's graphics task no longer runs to its end inside the game's SP start. `gfx_task_begin` / `gfx_task_run` in
  `src/render/f3dex2.c` keep the interpreter's position between calls. `src/ultra/hw_sp.c` starts the task (`gfx_start`) and advances it
  (`gfx_pump`): from `HostIdle` and `HostPreemptPoll` (`src/ultra/os_sched.c`), before a field is shown (`HostHwGfxComplete`, called from
  `vi_write`), and at once when the game writes SP_SET_YIELD (SIG0) while the task runs. A yield stops at the next command, sets
  SP_STATUS_YIELDED (SIG1) and raises the SP interrupt, which is what the game's `Sched_HandleRspCompletion` / `osSpTaskYielded` read. The
  next start with the task still yielded resumes it without reloading its state. No thread is involved, so this is the same model a
  Switch port would use.
* Why: the game's yield to audio (`__scYield` in libultra's `sched.c`) only happens at a retrace while a graphics task is still running and
  an audio task is queued. The old code finished every list inside SP start, so that case could never arise and audio always waited.
* Measured on Linux (2 cores, the same scripted menu and battle input, 40 s each, no diagnostics): fields per 5 s 301/267/240/299/298/296
  for the new build against 290/224/215/289/297/300 for the previous commit (6d36e0b); audio tasks per 5 s 245/187/175/258/289/290
  against 263/191/164/248/293/286. So the speed is unchanged or slightly better, and **the audio task rate did NOT change**. This does not
  by itself fix the menu audio lag.
* Yield path exercised: with `KYUJO_GFX_NO_IDLE=1` (development only: the task then stays running until the retrace) a 40 s run had
  one SP_SET_YIELD while running, one yield, one resume, and no errors. Normal runs had no yields at all, because the task finishes during
  idle time before the next retrace.
* A 110 s scripted battle run with frame dumps: no "image address outside the program" lines, and the menu picture is correct.
* Open: the perceived menu audio lag is still NOT explained by the counts. The remaining cost is the renderer itself (menus 20 to 40 ms of
  graphics per field in the mid-run samples). Needs a Windows measurement with the perf lines first.

## GB Tower "Loading" time is the Transfer Pak bus time (2026-10-27)
* The user's Windows times: about 29 s (Red, Blue) and 18 s (Yellow) from choosing the cartridge to a playable Game Boy picture. Measured on Linux with Red
  (`kyujo: perf:` lines, new `pak-accesses` running total): about 20,000 Transfer Pak accesses (accessory reads/writes of 32 bytes) are made while
  "Loading..." is shown, 3,200 per 5 s, i.e. about 1.5 ms each; the modelled Joybus time of one is 1.152 ms (`hw_pif.c`, 36 bytes x 32 us). So the wait is the
  modelled bus time, which is what a console's Joybus also needs (the console adds the PIF's own time on top; the console's real loading time was NOT measured).
  After the load, about 120 accesses per second continue while the game runs (pak checks / save RAM), a separate matter.
* Option `pak_real_speed` (Options: "Game Boy cartridge loading": *Faster*, the default, or *Real speed*) scales the wire time of accessory reads/writes to 20 %
  or 100 % (`host_pak_time_percent`). Only the wait changes. Measured with Red: "Loading..." about 27 s -> about 4 s; Red's title screen and Yellow's title and
  Oak intro appear and respond. The user (Windows, test 10) confirmed that Faster works with Red, Blue and Yellow and that their saves are kept, and asked for it to
  be the default (test 11). The key of test 10 (`fast_pak`) is ignored, so a cfg written by test 10 gets the new default.

## Debug knock-out cheat: slow HP bar after a missed or failed move (2026-10-27)
* Reported on Windows (test 10): when the first attack misses, or its effect fails, the cheat still works but the opponent's HP bar drains very slowly.
* Cause (read in `battle_engine_309ED0.c`, reproduced on Linux with a forced miss): the HP bar shows `BattleMon.unk_02` (roster), which follows the real HP
  (`runtime.unk_0C`) in steps of max HP / 90 through a float tracker `D_843900A8`. After a damaging move the scene calls
  `BattleScene_AnimateDisplayedHpToRuntime` every frame; after a miss/fail (message phase 8) only once per presentation round (about 50 frames), so a full bar
  needs over a minute. The cheat only set `unk_0C = 0`.
* Fix (patch 0026): when the shown HP equals the real HP (nothing is animating) the cheat also sets the shown HP and the tracker to 0, so the bar drops at once;
  after a damaging move the normal animation runs. Verified on Linux, before/after with a forced miss.

## Pocket Monsters Green (Midori, Japanese) as a Game Boy cartridge: works through an adapter (2026-10-27)
* Requested in test 10 and first judged impossible ("the US game refuses it", correct for the game as it is). Solved in test 13 on Linux by making the host present the
  cartridge as one the US game accepts. The test ROM is 512 KiB, MBC1 + RAM + battery, destination code 0 (Japan), title "POKEMON GREEN", valid header and global checksums.
* Refusals in the US game, and what the port does about each (`src/platform/host_gbjp.c`, `host_gbpak.c`, patch 0027):
  1. `GbPak_IsPokemonCartridge` / `GbCart_IdentifyGame` accept only destination code 1, and the Game Boy emulator wants a 1 MiB ROM (size code 5) whose byte sum
     equals the header's global checksum: the host builds a 1 MiB image (Midori's 512 KiB, the upper half zero), sets size code 5 and destination 1, recomputes the
     header and global checksums, and sets the chip type to MBC3 (see 4).
  2. The emulator's title table has the GREEN entry disabled (its last two bytes are FF FF; the Stadium compares all 16 title bytes), and its start-up refuses
     variant 2 (Green) by name: patch 0027 enables the entry and removes the refusal. The emulator's table of known ROM checksums covers only Red, Blue and
     Yellow, so Midori takes the generic path (the whole ROM is read and the byte sum checked), which works.
  3. The Stadium's own save code (`GbSave_*`) reads the international layout (main data at 0x2580 with the checksum at 0x3523, 11-character names, 6 boxes of
     20 per bank); Midori's layout differs (checksum at 0x3594, 6-character names, 4 boxes...). The host keeps two views of the RAM: the real Japanese save, which
     only the Game Boy emulator's own cartridge accesses see (patch 0027 flags those accesses), and an international-layout copy built from it (Japanese kana
     names turned into Latin letters, "グリーン" -> "GURI-N", using the kana table read from a Japanese Stadium ROM at 0x77BD4; `tools/native/gen_gbjp_names.py`),
     which the Stadium's save code sees. The Stadium's writes go only to the copy and never reach the Japanese `.sav` (a later stage may write them back).
  4. The Stadium's transfer code reads ROM banks 0-63 with one 7-bit write to 0x2100 and RAM banks with a write to 0x4000, as for an MBC3. With the header
     saying MBC1, the Game Boy core ran the title screen and then executed an invalid opcode (found with a store watch on the "corrupted" flag, `KYUJO_MIPS_WATCH`,
     and a dump of the loaded fragment, `KYUJO_GBEMU_DUMP`; the flag is set by the handler of invalid opcodes) and the Stadium said "saved data corrupted". Presenting
     the cartridge as MBC3+RAM+battery (the chip the core is proven with) fixed it; Pokemon Green needs nothing MBC1 has and MBC3 lacks.
  5. The Stadium's emulator probe blocks (it writes small test patterns into the save RAM and later checks them) run in the emulator's service thread, so the
     whole thread job is flagged "real save" (the first try flagged only the data transfers and the later check failed: "Unable to SAVE").
* Verified on Linux with the user's real Midori ROM and save: the Stadium's GB Tower screen shows the trainer "GURI-N" ID 44837; the Game Boy emulator boots
  Midori, CONTINUE shows the real save (グリーン, 8 badges, 151 Pokedex, 107:49), the game walks, saves (the `.sav` differs only in the game's own fields, their checksum
  stays consistent, and 8 scratch bytes at 0xC and 0x1C of RAM bank 0 which the Stadium's probe writes on every cartridge) and Quit Game returns to the Stadium.
* Windows result (user, test 13 exe, 2026-10-28): all checks passed, including editing the party in Midori inside the GB Tower, saving, and then using that new
  Pokemon in a Tournament battle, so the international-layout view and the converter work in that Stadium mode on a real PC. Not tried: the other modes (Free Battle, Lab...).
* Not done yet: the converter (94 Pokemon) is checked against the Stadium's rules by `tests/native/gbjp/test_gbjp.c`; modes other than the Tournament were not tried; names appear in romaji; writing Stadium changes back to the Japanese save.
* Not possible: running the Japanese Stadium itself (the decomp's JP target has 3,859 functions still in assembly, `COVERAGE.md`).

## Overlay variables follow the console's order: crash in `Completion_InitCamera` after the Rival / a cup clear (2026-10-27)
* Reported on Windows (test 10): after beating the Rival the game crashed (`0xC0000005 at 0x00156E4E`, read of 0x1E1E4D0C). The address is inside
  `Completion_InitCamera` (`demo_scenes_161E60.c`, the "Completion" scene: mode 21 after a Gym Leader Castle clear, mode 26 after the Hall of Fame / Rival),
  found with `llvm-nm` on the exe. Reproduced on Linux by running that scene directly (scratch build, `KYUJO_DEMOJUMP`, not committed): same crash.
* Cause: `Completion_LoadModels` DMAs 0xBC0 bytes into `D_86B0EBE8[0xA60]`. The decomp declares `D_86B0F648[0x180]` right behind it (0xA60 + 0x180 =
  0xBE0), so on the console the load fills both. Natively the program's variables of one overlay were laid out in the order clang emitted them (the order of
  first use, not of declaration), so the variable behind `D_86B0EBE8` was another one and the load overwrote `D_86B0EBC0`, the scene's model tree pointer,
  which `Completion_InitCamera` then dereferenced. Found with a hardware watchpoint on `D_86B0EBC0`: the writer was a PI DMA of ROM 0x7703B0, length 0xBC0.
* Fix (`tools/native/be_memory.py`, `order_fragment_globals`): the variables of an overlay's data and zero sections are sorted by the console address in
  their console address: from the name (`D_<address>` or `<function>.D_<address>`), or else from the decomp's documented symbols
  (`linker_scripts/us/symbol_addrs*.txt`, plus the variables that an alias of step 5b places, e.g. `D_8120EA80 = sGbApuRegs`). A variable with no known address
  stays right behind the known one before it. Unit-tested in `test_fragment_sections.py`; after the fix `nm` shows `D_86B0EBE8` followed by `D_86B0F648`, and on
  Linux the Completion scenes ("You just won EEVEE!", "Amnesia Psyduck") and the credits play.
* First attempt, wrong: putting unknown (unnamed) variables FIRST moved the Game Boy emulator's variables (`sGbApuRegs`, `gGbApuSquare1`, ...) away from
  their neighbours; the Game Boy Tower then crashed ~11 s after choosing a cartridge (`func_81209374`, audio stream id garbage). Found by running the Game Boy
  Tower on the new build before shipping it; the rule above replaced it.
* Windows result (user, test 12 exe, 2026-10-27): the credits after the Rival play to the end, and the Doduo/Dodrio Tower unlocks work. Not checked: other overlays beyond the scenes played, and whether another overlay has an overrun that the old order hid (the new order is the console's, so it should only remove such
  bugs, but only the scenes named above, the cheat battle and the Game Boy Tower were played after the change).

## Hall of Fame (Victory Palace gallery) crash: a local variable overwritten by a float (2026-10-28, patch 0028)
* Reported by the user on Batocera (Linux) and on Windows: the program crashes when the Hall of Fame (Area Select -> Pokemon Stadium -> down, left -> the pedestal gallery) is
  browsed on a fresh save. Reproduced on Linux 32-bit with the scripted route and a fresh save: SIGSEGV in `strlen` called by `sprintf` in `func_81003A54` (builds the display list of
  the name plate), whose `%s` pointer came from `Text_GetString(table, D_800AF770[effectSlot % 5])`; `D_800AF770[0]` held 0x99 (153), not a species number.
* Cause: `VictoryPalace_UpdateLeftPillarGlow` declares `s16 sp30; // purposely the wrong type to fix the stack, should be f32` and passes `&sp30` as the `f32*` of
  `Vec3f_CalculateDistanceAngles`, which stores 4 bytes. On the console the extra bytes land in unused stack padding. Natively clang puts the next local, `s16 idx` (= 5, the
  index into `D_800AF770`), right behind it, so the float overwrote `idx` (disassembly: `&sp30` = `-0x18(%ebp)`, `idx` = `-0x16(%ebp)`); `ptr = &D_800AF770[idx]` then pointed to a
  wrong byte and the glow value (0x33, 0x66, 0x99, 0xCC, 0xFF, written every frame) was stored there. A gdb watchpoint on `D_800AF770[0]` named the writer. The decomp's
  own `Gallery_SetCameraAngleOffsets` and `Intro_LightAngleCallbackA` declare the same variable as `f32` and were not affected; the other callers of the function (gallery.c,
  demo_scenes, battle_engine) were checked: only this one had the wrong type.
* Fix: port patch 0028 declares `sp30` as `f32`. Disassembly of the new build: `&sp30` = `-0x1c(%ebp)`, `idx` untouched. Linux, fresh save, the same route: 12 scrolls right, left/right
  scrolling, 30 scrolls left (wrapping round to No.133), L/R paging, back out with B: no crash, the name plates are right. Not run on Windows (the same compiler layout applies).
* Class: **stack layout accident** (a decomp "match the stack" hack that is only harmless with the console compiler's frame layout). Other `purposely`/`wrong type` hacks: a search of the decomp for
  such comments found only this one; wrong-typed locals without a comment cannot be found this way.
* New: a crash on Linux is now written to the log (`CRASH: signal 11 ... eip ... address touched ...`, `host_main.c`); before, the log of the user's crash showed nothing. The program is linked
  at a fixed address, so the `eip` can be looked up with `nm` in the build's program file.

## Game Boy save: the verify buffer overflowed into the scheduler (2026-11-01, patch 0033; layout-accident class)
Reported by a Windows tester (test16, test17): after a Prime Cup round 2 / Master Ball win with a Yellow Pikachu the "teach SURF" scene crashed on "Saving", every time, inside `Sched_NotifyClients`
(the scheduler walked its client list and met a garbage `next`). Test16's crash handler then wrote the half-saved RAM into yellow.sav (see host_gbpak.c, `save_safe_to_write`).

* How it was found: test17's crash report lists the scheduler client list; the third node had the Game Boy text of box Pokemon names in its bytes. Linux reproduction: patches 0030 + 0032 show the scene
  with the cartridge's first party Pikachu (`KYUJO_FORCE_PIKACHU_REWARD=1`; route: win a Poke Cup round with the cheat, "Save and Quit" at "Keep battling?"), which crashed at the same place with the
  same pointer (0x50828B8E). Under gdb the list was walked at the crash and the memory around the damaged node held the 20 x 11-byte nickname tables of the cartridge's box banks.
* Cause: `GbSave_WriteBoxBank` verifies each bank by `GbPak_MbcRead(..., &D_800ACA78, ..., 0x1A60)` (0x15A0 on the JP version) but `D_800ACA78` is declared as one `GbSaveMainData` (0xFC0 bytes).
  The decomp carries the next array `D_800ADA38[0xAA8]` as "unreferenced bss" - on the console that array sat right behind the buffer and took the extra 0xAA0 bytes. Natively the array is not kept there, so the bytes
  overwrote the next objects (the scheduler client list).
* Fix: patch 0033 makes the buffer and the "unreferenced" array one struct. Every save that rewrites the box banks was affected (Pikachu reward, trades, GB Tower saves), not only this one.
* Class: **layout accident** (same as 0022, 0023, 0031): a decomp array split in two whose second half is "unreferenced". A search of the decomp for "unreferenced bss" and similar comments found only this array (the other, `D_884001EC` "bss pad" in pokemon_lab_support.c, is a plain pad).

## Gym Leader Castle: selecting the Rival after the Elite Four (round 2) trapped the game (2026-11-12, patch 0037; layout-accident class)

* Symptom (Switch tester, test 36): after the Elite Four of round 2, selecting the Rival closed the game to hbmenu with no error page. The log has `core: the game trapped in a game thread: Out-of-bounds access in linear memory or a table (1)` and exit code 70.
* Reproduced on Linux with the tester's save (the Elite Four won with the L+R+Z cheat, then the Rival selected, instantly). `gdb` on `wasm_rt_trap`: `Game_Thread > Fragment_LoadAndCall > BattlePrep_RunRulesScreen > BattlePrep_ShowRoundIntro > BattlePrep_InitRoundIntroScene > BattlePrep_InitBannerScene > BattlePrep_LoadBannerGeoLayouts > BattlePrep_AdjustBannerTextureOffset`. Function names come from `fnmap.txt` (the wasm has numbers only: `w2c_kyujo_f3155` is the function at index 3155).
* Cause: the banner table `D_84B0FC6C` has nine entries and the Rival is index 9; on the console the next array sat right behind it (index 9 was `D_84B0FC90[0]` = `D_3048210`, the next banner). Natively that memory is something else; the first-column scan then ran on garbage without an end.
* Fix: patch 0037 adds the tenth entry.
* Class: **layout accident** (same as 0022, 0023, 0031, 0033, 0036). A native x86 build would not have trapped, it would have drawn the banner from garbage: this is one more reason to try every menu path in the WebAssembly build, which traps where the native builds guess.
* Search done for the same pattern in the file: the other tables indexed by `modeSelection`/`mode` (`D_84B0FC40`, `D_84B0FC48`, `D_84B0FC54`, `D_84B0FCAC`) are long enough for the values the code can give them.

## Announcer speech static in battles: the RSP reads the sample rings too late (2026-11-13, patch 0038; timing class, not a decomp bug)

* Symptom (tester, Windows, Linux and Switch alike; also listed upstream as "announcer speech still carries static during battles"): clicks and static in the announcer's voice during battles; the Pokemon cries and the music are clean.
* Measured on Linux (stream dump + a temporary RSP DMA probe, one Poke Cup battle): `n_alMainBusPull` (src/libnaudio/n_mainbus.c) builds two `aLoadBuffer` commands per pull that read 0x170 bytes of a ring the CPU keeps filling (`D_800FCF28[i]`, the streamed voices: ch 1 = announcer, ch 0 = cries; and the Game Boy ring `D_800FC6D8`). On the console the RSP runs the list right after it is built. In the port the audio microcode runs on its own thread and the load happens 4 to 6 pulls after the read pointer moved in 63 percent of the cases; in 3.5 percent of all loads the producer (`func_80044EA4`) had lapped or straddled the region, so the RSP got newer samples than the pull was built for. With the audio task run synchronously (`KYUJO_AUDIO_THREAD=0`) torn loads were 0.2 percent.
* Fix: patch 0038 copies the 184 samples of each pull into a 16-slot-per-source snapshot ring at build time and points the commands at the copy (17 KB of image; the lag measured is up to 6 pulls).
* Check: share of loud 92-sample blocks with a jump above 6 times the mean difference (a crude click detector): old build 1.39 percent, with the patch 0.48 percent (48 slots) and 0.56 percent (16 slots), the synchronous-task floor 0.47 percent. So the clicks are down to the level of the speech itself. Not yet confirmed by ear on any platform.
* Not covered: the sample DMA buffers (instrument samples) are read by the same late task; no noise was reported for them.
