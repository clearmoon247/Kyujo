# Move animation call trace and move tester (development)

Records which game functions run while something happens in the game (for example a move's animation), with the picture number of the first call
of each. Written for the graphics glitches in move animations (Night Shade, Fire Blast): it shows which effect and particle functions the game
runs, so they can be compared with what the renderer draws.

## Build (Linux)

Instrumenting every game function makes the program too big (`tools/native/imagelimit.py` keeps 1.75 MiB free below 0x01000000), so name the
files to watch. Use a separate output folder; the normal build is not affected.

    python3 tools/native/build_native.py --decomp <decomp> --profile linux32 --sysroot <musl-i386 sysroot> --out build/trace \
        --trace-calls battle_engine_315D50,battle_engine_32B680,battle_engine_move_animation,battle_engine_2F3CB0

Each word is matched against the path of a game source file. The four above hold the move-effect queue (`BattleAnim_QueueMoveEffectScript`,
`BattleAnim_RunEffectList`), the particle functions (`ParticleEffect_*`, `Gfx_PaletteParticleDL*`), the move-animation start/tick code and the
animation state-row table. The build also writes `kyujo.syms` (code symbols, `<hex address> <name>`) next to the program; the trace reads it
to print names. A symbol file is written for the Windows build too, but nobody has checked on Windows that its addresses match the running exe (not verified); without a match the trace shows addresses.

## Use

* `KYUJO_TRACE=<file>` records from the start until the program exits.
* Control channel (`KYUJO_CONTROL`): `trace <file>` opens a trace, `trace stop` closes it, `mark <word>` writes a labelled line.
* `KYUJO_TRACE_FULL=1` writes every call instead of the first call of each function (large; `KYUJO_TRACE_MAX` lines, default 5 million).
* `KYUJO_SYMS=<file>` names the symbol file if it is not next to the program.

Output: first calls as `F<picture> T<thread> +<depth> <function>`, `MARK F<picture> <word>`, then a summary
`calls=<n> first=F<picture> last=F<picture> <function>` in order of first call.

## Example (Night Shade, Gengar, Poke Cup)

The effect chain, in order: `BattleScene_EnterRow3StartMoveAnim` -> `BattleAnim_PlayMoveEffectScript` -> `BattleAnim_QueueMoveEffectScript` ->
`BattleAnim_RunEffectList` -> `ParticleEffect_PaletteMultiBurstCleanupWithTintFade` and `BattleAnim_CallbackStartEffects95To97EndEffects62And63ModelFadeIn`,
drawn each picture by `Gfx_PaletteParticleDL` (and `Gfx_PaletteParticleDLCallback`). Only functions of the files named at build time are listed.

## Move animation test page

Menu (F1) -> "Move animation test": choose the Pokemon that uses the move, the move, and (optionally) the opponent's Pokemon, then "Start the test".
The game restarts itself and, on its own:

1. plays through its menus with a fixed list of button presses (title -> Stadium -> Poke Cup -> three rental Pokemon -> battle; about 4 minutes);
2. when the battle engine is loaded, patch 0029 (`memmap.c`, `Port_ApplyMoveTest`) puts the chosen species and its single move into the first
   player's lead Pokemon (and the opponent's lead species), so the game builds the battle with them;
3. at the battle's command menu it starts the call trace, presses A (Battle) and the four C buttons (only the slot that holds the move reacts), and
   records 600 pictures;
4. writes `movefx-<pokemon>-<move>.log` in the program's folder and shows "Move test finished".

The log is the call trace described above: the game functions that ran, in order of first call, with picture numbers and marks `battle_menu`,
`move_buttons`, `done`. For Night Shade it lists `BattleAnim_QueueMoveEffectScript`, `BattleAnim_RunEffectList`, `ParticleEffect_*` and
`Gfx_PaletteParticleDL`. It needs a program built with `--trace-calls`; an ordinary program still plays the battle and the log says that no call was recorded.

From a shell: `KYUJO_MOVETEST=94,101,0 ./kyujo` (Gengar, Night Shade, opponent unchanged); species 1-151 and moves 1-165 are the game's own
numbers. `KYUJO_MOVETEST_QUIT=1` ends the program when the test is over.

### Many tests

    python3 tools/native/movetest_batch.py --program build/trace/kyujo --rom <rom.z64> --out logs 94:101 6:83 ...

Each pair `SPECIES:MOVE[:OPPONENT]` is a fresh start (about 5 minutes), headless, Linux. An existing log of the same name is kept.

### Names in a log from another machine

`python3 tools/native/trace_resolve.py movefx-....log kyujo.syms > resolved.log` replaces addresses by names. The `.syms` file must come from the same build
as the program that wrote the log.

### Limits (what is not known or does not work)

* The route assumes the game's default menus. Another save file or a Game Boy cartridge screen can put it off; the log then says `never_reached_the_battle`.
* Only the first player's lead Pokemon is changed. Its name on the screen stays the original's (the model and the move are the chosen ones); the Pokemon's
  stats, level and moves other than the chosen one are the rental's, and the other slots are emptied.
* Some moves need a state the test does not create (a target that can be hit, a status). The animation may then differ from the normal one; the log
  records what ran, not whether it matches a real battle.
* Tested on Linux only (Night Shade, Gengar). The Windows program compiles and has not been run.
* The Windows executable has no usable symbol table: the program may print addresses; use `trace_resolve.py` with the `.syms` of the same build.

Note: the in-game "Move animation test" page was removed from the menu (it was only for debugging). The tester itself is still there as a developer tool: start the program with `KYUJO_MOVETEST=<species>,<move>[,<opponent species>]`.
