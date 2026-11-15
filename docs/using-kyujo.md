<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Dan B. Cooper -->
# Using Kyujo

## Starting

- **Windows:** start **Play Kyujo** (or `Game\kyujo.exe`) in the folder the setup made. The first time, a window asks for your game file; the choice is remembered in `kyujo.cfg`. You can also drag the file onto `kyujo.exe`.
- **Linux / Batocera:** start **Kyujo** from the Ports menu (Batocera), or run `./kyujo/kyujo --rom /path/to/your-game.z64`.
- **Android:** open **Kyujo**, tap **Choose the Pokemon Stadium ROM** once, then **Play** (landscape).
- **Switch:** start **Kyujo** from the Homebrew Menu.

Everything Kyujo writes (settings, the game's save, the log) stays next to the program, or in the app's own storage on Android. Your game file is never copied anywhere else, changed or uploaded.

## The menu

Press **F1** or **Esc** at any time (gamepad: its menu button, **Back** on an Xbox-style pad, Back or MENU on Android). The game stops while the menu is open. On Windows you can also use the menu bar of the window. The menu is operated with the keyboard, the D-pad or the left stick, and holds:

- **Game Boy cartridges:** your own Game Boy game and save for each of the four Transfer Paks (US Red, Blue and Yellow; the Japanese *Green* through an adapter). Other Japanese cartridges are refused.
- **Controllers:** the keyboard and every gamepad found, each with its own page: which N64 controller it drives, every button shown in a grid, rebinding by choosing a button or with *Setup* (asks for each button in turn; a gamepad's right stick can give the C buttons), a live highlight of what is held, a test screen and a dead zone.
- **Stadium ROM and save:** choose another save, start a new one, make a backup copy.
- **Options:** full screen, widescreen stretch, sound volume, the stick as a D-pad, and the optional cheats.
- **Information** about where the files are, **Restart**, **Quit** and **Exit menu** (back to the game).

The game reads the Game Boy cartridges, the game file and the save only when it starts, so after changing one the menu asks *Restart now / Later*.

## Keys

Defaults (changeable in the menu):

| Key | N64 controller |
|---|---|
| Arrow keys | control stick |
| X or Space | A |
| Z or Backspace | B (the **Z key is the B button**) |
| C | Z trigger |
| Enter | Start |
| Q / E | L / R |
| W A S D | D-pad |
| I J K L | C-up / C-left / C-down / C-right |
| F11 or Alt+Enter | full screen |

Gamepads are found automatically: Xbox-style pads and every other pad the system lists (DualShock/DualSense, Switch Pro, Bluetooth pads), up to four. A generic pad's buttons are only numbered, so check its layout in *Test this gamepad* and rebind what is wrong. In Game Boy Tower, C-up opens the GB Tower menu.

**Android:** while no gamepad is connected an N64 pad is drawn over the game (D-pad or analog stick on the left, A/B, the four C buttons, L, Z, R, Start; the small MENU button opens the menu). The launcher's *On-screen controls* button cycles D-pad / analog stick / off.

## Game Boy games and saves

Open the menu → *Game Boy cartridges*, choose a controller, then *Game Boy ROM*. On Windows, Linux and Switch pick the file; its save is the file of the same name ending in `.sav` beside it. **On Android** you allow the folder that holds your Game Boy games once (it is remembered), and tap a game: the save of the same name comes with it automatically, and is shown as `<name>.sav (automatic)`. *Other folder* changes the folder. Without a save, the game starts a new one. A save the game already had is never lost: if a different one comes in, the old one is kept as `<name>-backup-N.sav`.

## Saves and backups

The game's save is `kyujo.sav` next to the program. On Android it is in the app's private storage, which is removed if you uninstall the app or clear its data: use **Save a backup of the saves and settings** in the launcher now and then (a zip you keep wherever you like; the game file is not in it).

## Optional cheats

In the menu's Options (they can also be set in `kyujo.cfg`). The battle shortcut is on by default; the other two are off:

- **Battle shortcut:** in a battle, **L + R** (Q + E) knocks out the opponent's current Pokémon once you choose your next move; **L + R + Z** (Q + E + C) makes you win the battle.
- **Hall of Fame complete** (`cheat_hall_of_fame=1`): shows all 151 Pokémon in the Hall of Fame, so the game plays its completion scene and records the reward as after a real completion. The missing Pokémon are made up on screen, not written to your save.
- **Kids Club CPU vs CPU** (`cheat_cpu_vs_cpu=1`): lets you choose *Ready!* on *Who wants to play?* with all four players set to COM, so the computers play the mini-game against each other.

## If something goes wrong

Look at the log: `kyujo-log.txt` next to the program (`KYUJO-SDL-LOG.TXT` on Switch and Android; on Android the launcher's **Save the log**). It contains no game data. To report a problem, open an [issue](../../../issues/new/choose) and attach it. `--console` shows a console window on Windows instead of writing the log to a file (for developers).
