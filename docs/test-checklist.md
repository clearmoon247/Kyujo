# Things that must still be tested / not forgotten

Kept here so they survive between sessions. Items are added when the user or a finding asks for it.

## Requested by the user

- **Prime Cup -> Doduo GB Tower.** After the Prime Cup is completed, the game unlocks the Doduo GB Tower
  (the decomp's `gbTowerUnlockTier`: 0 = normal, 1 = Doduo, 2 = Dodrio). The user asked (twice) that this is not forgotten.
  - The TRIGGER (a real Prime Cup clear) is NOT TESTED: it needs a full cup run, which cannot be scripted yet.
    What the decomp does (`BattlePrep_MarkFirstClearBonus`, read, not observed): when session mode 3 (Nintendo Cup, uses `nintendoCupClears`)
    or mode 6 (Prime Cup, uses `primeCupClears`) is finished at `modeSelection == 3` and that mode's bit (8 resp. 0x40) is not yet in
    `clearedModes`, the tier goes up by one (maximum 2). So the FIRST of those two cups cleared gives Doduo and the second gives Dodrio.
    The user's statement ("after the Prime Cup, Doduo") holds if the Prime Cup is the first of the two. This is how the decomp reads; it was
    not checked against a console.
  - The CONSUMPTION side WAS tested on Linux 32-bit by forcing the stored value from a debugger (not through the cup): with tier 1 Area Select
    shows the "DODUO GB TOWER" banner and Doduo on the Game Boy picture, and the tower runs Pokemon Red with the Doduo wallpaper and Game Boy;
    with tier 2 it shows "DODRIO GB TOWER". The value is lost whenever the game reloads its save bank, so the test hooks the options read.
  - To test the trigger later: complete the cup in the port (or find a way to fast-forward battles), check the unlock message
    (`transitionFlags |= 8` for Doduo, `|= 0x10` for Dodrio), and check that it survives a save and restart.

## Game Boy cartridges through GB Tower (Linux 32-bit, local test copies only; Windows NEVER run)

| Cartridge | Result on Linux 32-bit | Not yet checked |
|---|---|---|
| Pokemon Red (US) | Boots, loads the save, play, in-game save (valid checksum), GB Tower menu, Quit, correct return to Area Select | audio never listened to; after patch 0024 the tower's audio is no longer silent (tonal content measured in the WAV) |
| Pokemon Blue (US) | Title, Continue from a real save (player NICK, 8 badges), walking, in-game save (file rewritten, checksum valid), GB Tower menu, Quit, correct return to Area Select | audio never listened to; save was checked only for a valid checksum, not by reloading it |
| Pokemon Yellow (US) | After patch 0022: yellow/green Pikachu frame, colour picture, title screen, menu, Oak intro | New game play beyond Oak's first lines, in-game save, Pikachu cry audio. Menu and Quit were run (return to Area Select correct) |
| Pokemon Midori (Japan, Green) | Works through an adapter (docs/known-decomp-issues.md): boots, Continue with the real save, plays, saves. Linux with the user's own ROM and save; **Windows (user, test 13, 2026-10-28): all test-13 checks passed, including changing the party in the GB Tower, saving, and using that Pokemon in a Tournament battle** | - |

## Open findings

- FIXED (patch 0024, needs the Windows retest): Game Boy Tower had no audio. Cause: C bit-fields with the wrong bit order on x86
  (see `docs/known-decomp-issues.md`). Measured on Linux only: the device-rate WAV of a Red session contains music with note-frequency peaks
  (440/465/587/783 Hz) while the tower runs; it has not been listened to. The audio has a DC offset (the mean of a second is about 3000..8000
  of 32767) because the emulator's mix is unsigned-biased; the program passes it on unchanged.
- FIXED on Linux, Windows retest pending: dark Pokemon (light normalisation), vertical shake in menus (interlace field offset), blinking
  (window back buffer; this one is NOT verified at all, it cannot be seen without a window).

- FIXED (overlay variables are now reset on every load of a fragment, see docs/architecture.md): returning to the Area Select
  screen after the GB Tower state used to show a black screen. Keep playtesting repeated visits to every mode, because the reset
  applies to ALL fragments and could expose other places that relied on stale values.
- FIXED (patch 0022): Pokemon Yellow showed a flat blue picture. Cause: the colour RSP program reads its constants at
  "program start + 0x9F0", which only works when the constants follow the program with no padding (the console's link layout).
- Other data that the program (C code, asm or RSP code) reads at a fixed distance from other data may exist. Searches so far were
  rough and are NOT exhaustive.
- FIXED (patch 0023, found by the array-bounds build, `docs/known-decomp-issues.md`): the move table (165 entries) was split into three
  arrays and read 4 bytes off for moves 69 and up. **Not observed in play**: no battle was compared before and after, and what the function
  at `3D140_2.c:2560` feeds (a move-effect / damage estimate) was not traced. Worth watching: moves with a high number (for example
  Hyper Beam, Surf, Blizzard-class moves in later cups) behaving oddly in battle.
- FIXED defensively: garbage texture / frame-buffer / RSP-DMA addresses crashed the renderer (seen twice in long runs of the bounds-checking
  build, cause not found). They are now refused and logged once as `first bad RDRAM access`. **If that line ever appears in a
  `kyujo-log.txt`, send the log**: it means a display list carried an address outside the program.
- Coverage of the bounds checking: one scripted battle and the menus. Other cups, Gym Leader Castle, Event Battle, most mini-games were not run
  with it.

- NEEDS WINDOWS RETEST (reported in the second Windows test): (1) battle text sharp? (fixed on Linux, rendering only); (2) audio lag in Game Pak Check menu and
  overworld; (3) GB Tower "Loading" time (was ~2 minutes) and whether inputs work during it; (4) battle speed. Please send the `kyujo: perf:`
  lines of `kyujo-log.txt` from the menu, a battle and the GB Tower loading: they say how many fields per second the PC reaches (60 = full speed).

## NEEDS WINDOWS RETEST (third build: graphics in slices)

Build: the zip made from this commit. Please run and send the `kyujo: perf:` lines of `kyujo-log.txt` for each step:

1. Main menu / Game Pak Check, 30 seconds: fields per second and the audio count (`audio N x`) per 5 seconds. Say whether the audio still lags, and where.
2. One battle with the attack animations, 1 minute: fields per second, and whether the text and the Pokémon level and status look right.
3. GB Tower: from "Loading" until the Game Boy title is playable. Note the time, and whether input works during Loading.
4. Any `image address outside the program` or `first bad RDRAM access` line in the log (these are findings: send the log).
