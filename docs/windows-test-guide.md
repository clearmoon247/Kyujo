# Windows test notes (history)

*Historical notes from the test rounds on real hardware, kept for the record. Players: see [`using-kyujo.md`](using-kyujo.md).*

**Read this first.** The Windows program has been **built and statically checked, but never run** by its developer (no Windows or Wine on the development machine). The same game code runs correctly on Linux 32-bit (see `docs/test-checklist.md`), so problems you meet are most likely in the Windows-only parts: the window, keyboard and gamepad, sound output, the ROM dialog, threads and timers, and file saving. **Your first run is the first test of all of that.** Please report what you see exactly, including "nothing happened".

You need: Windows 10 or 11 (64-bit is fine; the program is 32-bit), your own *Pokémon Stadium (USA)* ROM (`.z64`; the program refuses any other dump and says so), and optionally your own *Pokémon Red/Blue/Yellow (US)* `.gb` files with their `.sav` files.

## 1. Run

1. Put `kyujo.exe` in a folder of its own (the exe is built for you; it is never published, because it contains the game). There are no `.bat` files any more.
2. Double-click `kyujo.exe`. A file dialog asks for the ROM (your choice is remembered in `kyujo.cfg`). Or drag the ROM onto the exe.
3. A window titled "Pokemon Stadium (source port, untested on Windows) - N pictures/s" should open. **No console window should appear any more.**
4. Everything the program prints goes to `kyujo-log.txt` in the same folder. **Attach it to every report.** If the program stops with an error it also shows a message box with the text.

If the program says Windows loaded it at the wrong address, that is a real finding (the game needs the whole program below address 16 MiB, so the program is built without address randomisation and checks this at start): report the message and your Windows version.

Windows SmartScreen or an antivirus may warn about an unsigned program; that is expected for a home-made exe.

## 2. Keys

| Key | N64 controller |
|---|---|
| Arrow keys | Control stick |
| X or Space | A |
| Z or Backspace | B |
| C | Z trigger |
| Enter | START |
| Q / E | L / R |
| W A S D | D-pad up / left / down / right |
| I J K L | C-up / C-left / C-down / C-right |

Keys count only while the game window is in front. Gamepads are read too: an Xbox-style one (XInput) has A = A, X or B = B, triggers = Z, bumpers = L / R, Start = START, D-pad, left stick = stick, right stick = C buttons; any other pad (DualShock, DualSense, Switch Pro, Bluetooth ...) starts with a guessed layout that you can change in the menu. F11 or Alt+Enter = full screen. **Gamepad input has never been tried.** The window title shows how many pictures per second are shown: the console shows 60 (NTSC); tell me the number you see in the title bar in the title screen and in a battle.

## 3. What to test, in this order

Write down for each line: works / does not work / odd, and the log file if it did not work.

**A00000000000. Test 15: the SETUP KIT (build the game on your own computer)** (this is a different kind of test: you test the setup, then the game it makes)
Test 14 result (thank you): the Hall of Fame crash is fixed on Windows and Linux. New: a kit that builds the game on the player's computer from their own ROM, so no exe has to be handed around. Read `docs/windows-setup.md` for what it does. **Nobody has ever run `Setup.ps1`** (no PowerShell here), so the first failure is likely to be a setup mistake, not a game bug; the messages are meant to say what went wrong. Use the ONLINE kit first if your PC is online, the OFFLINE kit (private, contains source files, do not share) to test the offline path.
- [ ] Unpack the zip to a SHORT folder (`C:\PokemonStadium`). Double-click `Setup.bat`. Did a window ask for your ROM? Did it accept it (and refuse a wrong file with a clear message, if you try one)?
- [ ] Step 3 (compiler): online kit: does the download show a percentage and finish? Does the message say the SHA-256 is ok? Unpacking takes a while (about 4,000 files, 370 MB).
- [ ] Step 4: "all files match the manifest". Step 5: the build runs for several minutes with lines like "1. game code: ... compiled". **How many minutes did it take, on what PC (CPU, RAM, antivirus)?**
- [ ] Windows may ask (SmartScreen, firewall, antivirus) at some point: write down exactly what and when.
- [ ] At the end: `Play Pokemon Stadium` appears in the kit folder, and `Game\kyujo.exe` starts the game with your ROM already chosen. Does it behave like the exe from tests 13/14 (menu F1, sound, Hall of Fame)? It is built by a different compiler runtime (llvm-mingw 20240619) than your earlier exe, so please play a little (title, a battle, the GB Tower with a cartridge) and say if anything is new.
- [ ] If anything stops: send `work\logs\setup.log` and `work\logs\build.log` (they hold no ROM data) and tell me what was on the screen.
- [ ] Run `Setup.bat` a second time: it should skip what is done (unpacked compiler, matching downloads) and finish faster.

**A0000000000. Test 14: Hall of Fame crash fixed** (please do this part first)
Test 13 result (thank you): Midori worked, including changing the party in the GB Tower, saving, and using that Pokémon in a Tournament battle. The Hall of Fame crash (also reported on Batocera) is fixed in this build: the gallery's pillar-glow code overwrote one of its own variables with a float (docs/known-decomp-issues.md, "Hall of Fame").
- [ ] **Hall of Fame on a fresh save** (or any save): Area Select -> Pokémon Stadium -> down, then left (Victory Palace / Hall of Fame) -> A. Scroll with left/right and with L/R through the pedestals, also past No.001 and No.151 (it wraps round). It used to crash; now the name plates should show the right names and the program should keep running. B leaves.
- [ ] Anything else that looks different or crashes. Please attach `kyujo-log.txt` (a crash now writes a `CRASH:` line with the address).

**A000000000. Test 13: Pocket Monsters Midori (Japanese Green) in the GB Tower**
New: your Midori ROM (512 KiB `.gb`) and its `.sav` (32 KiB) can be chosen like Red, Blue or Yellow. **Work on a copy of your real save**: the game writes into the `.sav` you choose (the program also keeps `<name>.sav.bak` once).
- [ ] Menu (F1) -> Game Boy cartridges -> Controller 1 -> Game Boy ROM: pick the Midori `.gb`; Save file: pick (a copy of) the Midori `.sav`; Restart game. The log should say: *Game Boy cartridge on port 1 is a Japanese Pocket Monsters Green: the Stadium sees it as an international cartridge...*.
- [ ] Area Select -> GB Tower: the cartridge screen shows your trainer name in Latin letters (e.g. グリーン as **GURI-N**) with your trainer ID. Press A on the pak: "Loading..." (about 4 s with *Faster*, about 30 s with *Real speed*), the green "GREEN" border, the GAME FREAK intro, the title (Japanese logo, "Green Version").
- [ ] Press Enter (START) on the title: the CONTINUE menu (つづきからはじめる) shows **your** data (name, badges, Pokédex count, play time). Continue, walk a few steps, **save in the game** (START -> レポート -> はい), then C-up (I) -> Quit Game -> Yes. Come back (GB Tower, same cartridge): does CONTINUE show the new play time?
- [ ] Anything that looks wrong: the picture, the sound, a game that stops. Please attach `kyujo-log.txt` and tell me the Game Boy game's own message if it shows one.
- Reported working on Windows (user, 2026-10-28): after changing the party in Midori and saving, that Pokémon was used in a Tournament battle. Other Stadium modes (Free Battle, the Lab) were not tried; changes made on the Stadium side never reach the Japanese save.

**A00000000. Test 12: crash after the rival (credits) fixed** (please do this part first)
Test 11 result (thank you): the game crashed with exception 0xC0000005 at 0x00156E4E when the Rival was beaten, during the "You just won" / credits scenes. Cause (found with your log, the exe's symbols and a debugger run on Linux): the port stored the variables of a code overlay in a different order than the console; the "You just won" scene loads 0xBC0 bytes into one variable that is followed by a second one, and natively an unrelated variable (the pointer of the model tree) sat behind it and was overwritten, so `Completion_InitCamera` used a garbage pointer. Now every overlay's variables are placed in the console's order (details in docs/known-decomp-issues.md). Checked on Linux: the two completion scenes ("You just won EEVEE!", the Psyduck one) and the credits now play; the Windows exe has the same layout (symbol addresses compared), but I have not run it.
- [ ] **Beat the Rival again** (your `kyujo-backup-5.sav`, or any save before the Rival): the scene after the battle, the Hall of Fame and the credits should play to the end and return to the title. If it crashes again, send `kyujo-log.txt` (the message box shows the address).
- [ ] **Dodrio Tower**: if the credits finish, check the Game Boy Tower menu: is the Dodrio Tower unlocked now? (The Doduo Tower is already unlocked in your save.)
- [ ] Anything else that used the overlay variables (Game Boy Tower with Red/Blue/Yellow, mini-games, the Gym Castle) should be unchanged; tell me if something now looks different or crashes.

**A0000000. Test 11: cheat HP bar after a miss, GB Tower widescreen without border, Faster as default** (please do this part first)
Test 10 result (thank you): stick as D-pad works in the overworld, Faster works on Red, Blue and Yellow and they save. New in this build:
- [ ] **Cheat after a miss or failed move**: L + R (+ Z) when the first attack misses or its effect fails: the opponent's HP bar now drops at once (it crawled before). After a hit the bar still animates as the game does. Please try a miss, a failed effect and a normal hit, once with L+R and once with L+R+Z.
- [ ] **Widescreen in the GB Tower**: with Options -> Widescreen on, the Game Boy picture has no black border on the four sides any more (the built-in black frame is cropped, only while widescreen is on). Off: unchanged.
- [ ] **Game Boy cartridge loading** is now *Faster* by default; *Real speed* is the option. The setting is stored under a new name (`pak_real_speed`), so the old `fast_pak` line of test 10 is ignored: a copy of your old cfg also gets Faster. Check Options shows "Faster" on a fresh start and that "Real speed" still works.
- [ ] **Pocket Monsters Midori (Japanese Green)**: see Test 13 (it now works on Linux; this replaces the earlier "not possible").

**A000000. Test 10: stick as D-pad, centred title, faster Game Boy loading, widescreen** (please do this part first)
Test 9 result (thank you): controller pages, mapping and testing work; restart now comes to the front. New in this build, all from your report:
- [ ] **Left stick in the game's own menus**: the game reads only the D-pad there (overworld menu, Pokémon selection, GB Tower), which is why only the D-pad worked. Now a stick tilted past 40 % also presses the D-pad direction (Options: *Stick also moves the D-pad*, on). The keyboard's stick keys (arrows) do the same. Tell me if a mini-game (Ekans, gallery camera, which really use the stick) feels different; switch the option off to compare.
- [ ] **Title** "Pokemon Stadium" of the main menu is centred.
- [ ] **GB Tower loading**: the "Loading..." time is the time the console's Transfer Pak bus needs (about 25,000 reads of 32 bytes, 1.15 ms each); your 29 s (Red/Blue) and 18 s (Yellow) match that. New option *Game Boy cartridge loading: Real speed / Faster* (default Real speed). Faster waits a fifth of the bus time. Measured here with Red: "Loading..." about 27 s -> about 4 s, title and Oak's intro of Yellow work. Please try Faster with Red, Blue and Yellow, play a bit, **save in the Game Boy game**, leave the tower and come back (does the save load?). Tell me the load times and whether anything is wrong.
- [ ] **Widescreen**: Options -> *Widescreen (stretches the picture)*. On: the picture fills the whole window or screen (no side bars). It is the same 4:3 picture made wider, so everything looks wider; the game does not draw a wider view. Off: 4:3 with black bars as before.

**A00000. Test 9: Project64-style controller page, d-pad and stick in the menus, Exit, restart in front** (please do this part first)
Test 8 result (thank you): the automatic save selector for the Game Boy ROM is good, the controller was listed, both cheats worked. New in this build, all asked for by you:
- [ ] **Controllers**: lists *Keyboard* and every gamepad found. Choose one and a **single page** opens, laid out like Project64's input settings (the game's letters): *Plays as N64 controller*, *Left stick dead zone* (every gamepad has its own), then the groups *Digital pad*, *Triggers* (L, R, Z), *Start and menu*, *C buttons*, *A and B* (the keyboard also has *Analog stick*; a gamepad's left stick always is the analog stick). Each cell shows the button now bound; a cell turns **green while you hold that button** (live check). Buttons at the bottom: *Setup*, *Reset*, *Test* (gamepads), *Back*.
- [ ] **Navigation**: move between the cells with the arrow keys, the **D-pad or the left stick** (gamepad), or the Xbox pad's LB/RB for pages; Enter or A sets the cell under the cursor (press the new key/button; Delete clears; Esc or B cancels). Left/Right on *Plays as* and *Dead zone* change the value. The same D-pad / left stick works in every other menu screen.
- [ ] **Setup** (like Project64): asks for each N64 button in turn (A, B, Z, Start, D-pad, L, R, C buttons; a gamepad then its menu button; the keyboard then the four stick directions). **For the C buttons on a gamepad push the right stick** in the direction (or press any button). Esc skips one; the pad's menu button (F1 on the keyboard) stops. Setup replaces what was bound before, a single cell *adds* the new button and keeps the old one as a second.
- [ ] **Exit menu**: the last entry of the main menu closes the menu (same as *Resume game* at the top). If you meant a different "Exit" (closing the whole program is *Quit game*), tell me.
- [ ] **Restart**: after "Restart now" the new game window should come **to the front** (before it opened behind other windows). This is the one thing I could not try on Linux at all.
- [ ] Anything clumsy left in the controller pages? Please say which control (keyboard, which pad) you used.

**A0000. Test 8: program menus, any gamepad, restart question, cheat order** (please do this part first)
Test 7 result (thank you): the in-game menu is good; knock-out worked, win did not, and a Bluetooth pad was not found. New in this build:
- [ ] **Window menu bar** (File, Controllers, Game Paks, Options, Help): each entry opens the matching in-game menu screen. File has *Game ROM and save file*, *Restart game*, *Exit*. Options has *Full screen* (also **F11** or Alt+Enter; the menu bar hides in full screen and the mouse pointer too).
- [ ] **Game save file**: File → Game ROM and save file. *Load a different save file* (browser), *Start a new empty save file* (makes `kyujo-2.sav` and so on next to the current one), *Back up the current save file* (`kyujo-backup-1.sav`), *Use the default save file again*. They need a restart; the menu asks.
- [ ] **Restart question**: change a Game Boy cartridge or the game ROM/save, then leave that part of the menu (or close the menu): it asks *Restart now / Later*. "Later" keeps the change and the main menu shows *Restart game: needed*.
- [ ] **Controllers**: Controllers lists the keyboard and **every gamepad found** (Xbox-style pads and generic ones, e.g. your Bluetooth pad). Choose one: *Plays as N64 controller* (port), *Buttons...* (press a button for each N64 button; Delete clears; Esc or the pad's menu button cancels), *Test this gamepad...* (shows what is pressed, the stick/axis values and what the N64 controller receives, live), *Restore the default buttons*. Plugging a pad in or out shows a message on the picture and the log. **Please tell me: the name the menu shows for your pad, whether it is listed under Controllers, and what Test shows when you press its buttons** (for a generic pad the numbers may need rebinding; the log lines starting `kyujo: gamepads:` say what Windows reported).
- [ ] **Cheat win**: Options shows the keys. Knock-out = Q + E; win = Q + E + **C** (the Z key on the keyboard is the B button, not the Z trigger). Pressing them in any order works now. Please try both in the first and second battle and tell me which keys you pressed if it still fails.
- [ ] **Options**: Sound volume (Left/Right), Full screen, Cheat on/off. **About** shows where the settings, save and log files are.

**A000. Test 7: the new menu, controllers, cheat, no console window**
Test 6 result (thank you): menu sound clear, no lag in battle, a full round without problems. New in this build, all asked for by you:
- [ ] **No second (console) window** opens with the game. (The exe now uses the Windows GUI subsystem; the log is `kyujo-log.txt`. If something goes wrong at start you still get a message box.)
- [ ] **Menu**: press **F1** or **Esc** (gamepad: the **Back** button). The game stops (picture and sound) and a menu appears in the game's own letters on the background of the game's *Game Pak Check* page. Up/Down, Enter or Space to choose, Esc or Backspace to go back (gamepad: D-pad or left stick, A, B). Esc/F1 on the first screen, or "Resume game", continues the game exactly where it stopped.
- [ ] **Game Boy cartridges**: Game Boy cartridges → Controller 1 Transfer Pak → Game Boy ROM → pick your `.gb` / `.gbc` file in the file browser (it lists folders and the right file types; "Show all files" lists everything) → optionally Save file (choose a `.sav`, or "New file here"). Then **Restart game** (the game looks for cartridges only when it starts, as on a console). In GB Tower the cartridge name should now show. Your choice is remembered in `kyujo.cfg`; the `.sav` is changed by the game, the original is kept once as `<name>.sav.bak`.
- [ ] **Game ROM and save**: shown in the menu; a change applies at the next start.
- [ ] **Controllers**: Controllers → shows which N64 port the keyboard and each of the four gamepads drive (plug in or unplug a gamepad: a message appears on the picture). Rebind a key or a gamepad button (choose it, press the new key; Delete clears, Esc cancels; F1 and the gamepad Back button are reserved for the menu). Try a gamepad if you have one: **this was never run on real hardware**, tell me what it does.
- [ ] **Cheat** (menu: "Cheat: L+R knocks out opponent", on by default): in a battle press **L + R together** (default keys **Q + E**; gamepad: both bumpers). A message appears at the top; when you then choose a move, the turn is played and the opponent's Pokémon faints with the normal animation. **L + R + Z** (Q + E + C) instead wins the whole battle. Switch the cheat off in the menu and check that nothing happens.
- [ ] Tell me anything that looks or feels wrong in the menu (letters, background, speed, a key that does not work), and attach `kyujo-log.txt`.

**A00. Test 6: what changed since test 5, and what to look at** (please do this part first)
Test 5 result (thank you): speed much better, stat boxes fixed (level and status shown correctly). Still open: poor sound quality in the menus (overworld menu, cup selection), and a few laggy moments in battle.
- [ ] Menu sound (main menu, cup selection, Pokémon selection): clear now? Cause found in the test 5 log: in the menus the game stood still at every picture change until the drawing of the last frame was done (30 to 40 ms), so about a third of the sound blocks were never made (`audio 200x` per 5 s in the log instead of 300; `audio-gap` 100x). Two changes: (1) a picture is only waited for when the drawing is into that very picture; (2) when the game asks the drawing to pause so that sound can run, it is told "paused" at once while the drawing goes on in the background, as the console's RDP does (before, the pause waited for the end of one big triangle, up to 60 ms). On Linux the menu sound counts are now 296 to 300 per 5 s.
- [ ] Laggy moments in battle: fewer? (These changes also remove most waiting in battle; I did not measure a battle with the same moves as yours, because the Linux script only just reaches one.) If a lag remains, please copy the `perf:` lines from just that time; the new `gfx-wait` item says how long the game stood still for the drawing.
- [ ] Anything new that looks wrong at scene changes (a flash of garbage, a wrong texture for one frame, a freeze, a crash)? A new safety rule makes the game wait for the drawing before it frees memory (patch 0025); if you see such a thing, send the log and say what scene it was.
- Only if something looks wrong, to find which change is responsible, start from a command prompt in the folder with one of these set (then run `kyujo.exe` from that prompt):
  `set KYUJO_GFX_THREAD=0` (old single-thread drawing, slow), `set KYUJO_AUDIO_CUSHION_MS=0` (no sound cushion).

**A0. Retest of what you reported (new build)**
- [ ] Flicker/blinking of graphics and objects: gone? (A window back buffer was added; I could not see this problem here, so I cannot say it is fixed.)
- [ ] Vertical shaking in the menus: gone? (Measured gone on Linux.)
- [ ] Pokémon are now bright instead of dark/shadowed? (Fixed light handling; tested against numbers on Linux, not on a real picture next to a console.)
- [ ] Game Boy Tower has sound? (Fixed a real bug that silenced it on Linux too; the device output of Windows is still untested. If it is still silent, send the log: it now has lines starting `kyujo: audio:` that say whether the sound device opened and how many buffers were queued or dropped.)

**A. Start-up**
- [ ] The ROM dialog (or drag and drop) works; a wrong file is refused with a message box.
- [ ] The window opens and the Pokémon Stadium title screen appears with the right colours. (Linux showed it correctly.)
- [ ] Pictures per second in the title bar: ______ (title), ______ (menu), ______ (battle).
- [ ] Sound: is there music and sound at all? Is it the right pitch/speed, clear, or crackling? (**Sound has never been listened to by the developer on any system.** The Game Boy Tower's sound has a constant offset (DC) that is passed on as the game makes it; a faint click at start/stop is possible.)
- [ ] Keyboard input moves the cursor; START passes the title screen.

**B. Menus and modes** (these ran on Linux)
- [ ] START → "Game Pak Check" screen → Area Select.
- [ ] Cursor over Stadium, Pokémon Lab, Kids Club, Free Battle, Victory Palace, GB Tower shows the right banner text.
- [ ] Kids Club → Pick a game → a mini-game (Magikarp's Splash ran on Linux; press START to begin, A to jump).
- [ ] Stadium → a cup → team selection with the game's rental Pokémon → a battle. Do a few moves. (Battles ran on Linux for minutes; the very first battle command crash class has been fixed, but long battles are exactly where hidden problems showed up.)
- [ ] Closing the window with the X ends the program.

**C. Saving**
- [ ] After changing something that the game saves (for example, an option), end the program and start it again: is it still changed? A file `kyujo.sav` should exist in the folder.

**D. Game Boy cartridge in GB Tower** (only if you have the files; choose them in the menu: F1 → Game Boy cartridges → Controller 1 → Game Boy ROM, then Restart game)
- [ ] "Game Pak Check" shows the cartridge name (for example RED) and an ID.
- [ ] Area Select → GB Tower → the cartridge → the game boots (boot logo, then Pokémon title). Linux ran Red, Blue and Yellow.
- [ ] In the Game Boy game: Enter = Game Boy START, X = A, Z = B, W A S D = D-pad (tested on Linux with the N64 D-pad; whether the control stick, i.e. the arrow keys, also steers the Game Boy was not tested). Load your save, walk, **save in the game**, then C-up (I) → GB Tower menu → Quit Game → Yes. Is the `.sav` changed and does the game load it again?
- [ ] Yellow shows its colours (a flat blue picture would be the old bug).
- [ ] Pokémon Lab shows the cartridge's name and a message (for Red on Linux: "The Game Pak wasn't saved at a POKéMON CENTER" for a save made elsewhere).

**E. Not testable yet, do not worry about it:** the Doduo / Dodrio GB Tower unlock needs a finished cup; it is listed in `docs/test-checklist.md` and will be tested later.

## 4. What to report

- Windows version (Win + R → `winver`), CPU model, how much memory.
- The text of `kyujo-log.txt` (attach), and the text of any message box.
- Which line above failed, and what you saw instead (a screenshot helps).
- If the program uses a lot of CPU or runs slowly: the lines starting `kyujo: perf:` in `kyujo-log.txt` (one every 5 seconds). They look like
  `perf: 5.0 s  fields 300 (60.0/s)  gfx 120x 14.2 ms avg (34%)  picture ...`: `fields/s` is how many video pictures the game handled per second (60 = full speed;
  lower = slower than a console), `gfx` is the time to draw one frame's display list. Please copy the lines from (a) the main menu, (b) a battle, (c) while GB Tower says "Loading".
  The log also says which part ran: audio = the sound microcode run by the RSP interpreter, mips = the MIPS interpreter running the GB Tower's hand-written assembly.

Messages that are a FINDING, please report them with the log: `first bad RDRAM access: address ...` or `RSP DMA skipped, DRAM address outside game memory` (the game handed the hardware an address outside the program; the program skips that access instead of crashing, the picture may show one wrong texture).

Known, harmless messages in the log: `renderer: not implemented, used for the first time: ...` (a rarely used drawing feature is not done yet, the picture may differ slightly), `kyujo: audio: the game's sample rate is now N Hz` and `audio rate changed from 32006 to 48009 Hz` (normal when GB Tower starts).

## 5. Known limits

- The Linux build used for development has no window or sound output, so **nothing about Windows' window, sound, keyboard or gamepad has been exercised before you.**
- Only the US Pokémon Stadium ROM is accepted. Game Boy cartridges: US Red, Blue, Yellow tested. The Japanese *Green* (Midori) works through an adapter (Test 13; tested on Linux and, by the user, on Windows).
- A Controller Pak and rumble are not emulated. Only controller 1 is read.
- The renderer is software only; speed on your PC is unknown.
- The port's own code is GPL-3.0-only; the game's code inside the exe has no stated licence and is not covered (see `NOTICE.md`). Do not share the exe.
