<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Dan B. Cooper -->
<p align="center"><img src="assets/icon.jpg" width="160" alt="Kyujo icon: a floodlit arena at night"></p>

# Kyujo

Play an N64 classic as a native program on **Windows, Linux (including Batocera), Nintendo Switch and Android**, using your own game file. Kyujo is built from the community decompilation of the game by **[Team Hippodrome](https://github.com/team-hippodrome/pokestadium)** and **[Pret](https://github.com/pret/pokestadium)** whose years of work make this possible (see [Credits](#credits)).

> **Not affiliated with or endorsed by Nintendo, Game Freak, Creatures Inc. or The Pokémon Company.** Game and company names are used only to say which game and which hardware Kyujo works with. See [`NOTICE.md`](NOTICE.md).

**Contents:** [Get Kyujo](#get-kyujo) · [What you need](#what-you-need) · [Features](#features) · [FAQ](#faq) · [Known issues](#known-issues) · [Building](#building) · [Reporting a problem](#reporting-a-problem) · [Credits](#credits) · [Licence](#licence)

## Get Kyujo

Pick your system. You never download the game from here: you bring your own copy of it.

| System | What to do |
|---|---|
| **Windows 10 / 11** | 1. Download **`Kyujo-Setup-Windows.zip`** from the [latest release](../../releases/latest) and unzip it into a short folder such as `C:\Kyujo`.<br>2. Double-click **`Setup.bat`** and choose your game file when asked.<br>3. Wait a few minutes (it needs the internet once), then start **Play Kyujo** from the same folder. |
| **Linux / Batocera** | Run `git clone --recurse-submodules https://github.com/clearmoon247/Kyujo.git`, then `./build-linux.sh`. It builds `build/Kyujo-Linux.zip` with a start script for Batocera's Ports menu (`./build-appimage.sh` makes a single AppImage file instead). See [`BUILDING.md`](BUILDING.md#linux-and-batocera). |
| **Android** | Run `./build-android.sh` ([`BUILDING.md`](BUILDING.md#android)), install the `.apk` it makes, open **Kyujo**, choose your game file, tap **Play**. |
| **Nintendo Switch** | Run `./build-switch.sh` ([`BUILDING.md`](BUILDING.md#nintendo-switch)), copy the `.nro` it makes to `switch/kyujo/` on the SD card and start it from the Homebrew Menu. |

**Why is there no ready-made `.exe`, `.apk` or `.nro` to download?** A finished Kyujo program contains the game's code, and that code belongs to its owners, so it can't be handed out. Instead the setup builds the program **on your own computer**, from your own game file, in a few minutes. It installs nothing, needs no administrator rights, and everything stays inside its folder; delete the folder to remove it.

## What you need

- **The game:** the North American N64 version 1.0 of *Pokémon Stadium* as a ROM file (`.z64`, `.n64` or `.v64`). Kyujo checks that it is the right one, never changes it, and never uploads it.
- **Windows:** Windows 10 or 11, about 1 GB of free space while building, internet access for the first run.
- **Linux / Batocera:** a 64-bit PC. To build: `clang` 18 or newer and a few standard tools ([list](BUILDING.md#linux-and-batocera)).
- **Android:** Android 7 or newer with OpenGL ES 3.0 (arm64 or x86_64).
- **Switch:** a console that runs homebrew (Atmosphère or similar).
- **Optional:** a gamepad (Xbox, PlayStation, Switch Pro and most others are found automatically) and your own Game Boy games and saves for the Transfer Pak features.

## Features

- **The real game code**, running natively: no emulator is involved. The console's hardware is reproduced by hand.
- **An in-game menu** (F1 or Esc; on a pad, its menu or Back button) for settings, controllers, saves and Game Boy cartridges. It works the same on every system.
- **Controllers:** keyboard and gamepads with a profile each, live button test, rebinding by choosing a button, a dead zone, the stick as a D-pad. On Android, on-screen controls appear while no gamepad is connected.
- **Game Boy Tower and Transfer Pak:** use your own Game Boy games and saves (US Red, Blue, Yellow and the Japanese Green). On Android you allow one folder once and a game's save of the same name is picked up with it.
- **Saves you can see and back up:** one `.sav` file next to the program; the menu can load another one, start a new one or make a backup copy.
- **Options:** full screen, widescreen stretch, volume, Game Boy loading speed, and a few optional cheats that are off by default.

How to use all of it: [`docs/using-kyujo.md`](docs/using-kyujo.md).

## FAQ

**Which game file do I need?** The North American (USA) version 1.0 of Pokémon Stadium. Other versions and other dumps are refused with a message that says so. `.v64` and `.n64` files are converted automatically; your original is not touched.

**Where are my saves and settings?** In the folder of the program: `kyujo.sav` (the game's save) and `kyujo.cfg` (settings). On Android they are in the app's private storage; use **Save a backup of the saves and settings** in the launcher. On Switch the folder is `sdmc:/switch/kyujo/`.

**Where is the log?** `kyujo-log.txt` next to the program (`KYUJO-SDL-LOG.TXT` on Switch and Android). Attach it to a bug report.

**Windows SmartScreen or my antivirus warns about `kyujo.exe`.** The program was built on your computer and nobody has signed it, so Windows doesn't know it. It is made from the open sources in this repository; if you want to check, the setup log shows every step.

**Can I share the program I built?** No. It contains the game's code, which isn't ours to give away. Share this repository and the setup instead, and let others build their own.

**Does it need the internet?** Only the first Windows setup (two downloads from github.com, each checked against a fixed hash before use) and building on Linux. Playing never does.

**How do I choose a different game file?** In the in-game menu, *Stadium ROM and save*. Windows: the choice is also stored in `kyujo.cfg` (`game_rom=`).

**Can I use my Game Boy games on Android?** Yes: open the game's menu → Game Boy cartridges, tap *Game Boy ROM*, allow the folder that holds your games once, and tap a game. Its save (same name, `.sav`) comes with it.

## Known issues

- Game Boy Tower battle sound can have small gaps.
- A black frame can appear in some battle camera transitions.
- Loading times are still being worked on.

The complete list, with causes: [`docs/known-decomp-issues.md`](docs/known-decomp-issues.md) (problems that come from the original game or its decompilation) and [`docs/port-patches.md`](docs/port-patches.md) (every change Kyujo makes to the decompilation, with the reason).

## Building

You don't need to build anything to play on Windows: the setup does it. To build yourself, or for the other systems, see **[`BUILDING.md`](BUILDING.md)**. The developer documentation (design, tests, how the port works) is indexed in [`docs/README.md`](docs/README.md).

## Reporting a problem

Open an [issue](../../issues/new/choose) and tell us your system and what happened. Attach the log (see the FAQ); it contains no game data. Please don't attach the game file, a built program or a save you want to keep private.

## Credits

- **[Team Hippodrome](https://github.com/team-hippodrome/pokestadium)** and **[Pret](https://github.com/pret/pokestadium)** for the decompilation of the game: its logic, data and structure are their work, and Kyujo is only the platform layer around it. The decompilation is **not copied into this repository and not modified**: it is fetched from their repository (a git submodule pinned to one commit) and a few small, documented patches are applied to a copy while building. Please look at their project too.
- The other components Kyujo uses (SDL2, wabt, and others) and their licences: [`THIRD-PARTY.md`](THIRD-PARTY.md).

## Licence

Kyujo's own code is licensed **GPL-3.0-only** ([`LICENSE`](LICENSE)). That covers our files; it doesn't cover the game or the decompilation, which keep their own owners and terms. What may and may not be shared, and why: [`NOTICE.md`](NOTICE.md).

Never in this repository: game files in any form, anything generated from them, built programs, and Game Boy games or saves (`.gitignore` blocks the usual file types).
