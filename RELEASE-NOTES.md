<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Dan B. Cooper -->
# Kyujo: first release

Play an N64 classic as a native program on **Windows, Linux (including Batocera), Android and Nintendo Switch**, from your own game file. No emulator: Kyujo runs the game's code (the community decompilation by [Team Hippodrome](https://github.com/team-hippodrome/pokestadium)) and **[Pret](https://github.com/pret/pokestadium)** and reproduces the console's hardware by hand.

## Get it

**Windows:** download **`Kyujo-Setup-Windows.zip`** below, unzip it into a short folder such as `C:\Kyujo`, double-click **`Setup.bat`**, choose your game file, and wait a few minutes (it needs the internet once). Then start **Play Kyujo**. It installs nothing and needs no administrator rights.

**Linux, Batocera, Android, Switch:** build it on your own computer with one command; see [`BUILDING.md`](BUILDING.md).

| System | Command | You get |
|---|---|---|
| Linux / Batocera | `./build-linux.sh` | `build/Kyujo-Linux.zip` with a start script for Batocera's Ports menu |
| Linux desktop | `./build-appimage.sh` | `build/Kyujo-x86_64.AppImage` |
| Android | `./build-android.sh` | `build/Kyujo-Android.apk` (arm64 and x86_64) |
| Nintendo Switch | `./build-switch.sh` | `build/kyujo.nro` for the Homebrew Menu |

**Why no ready-made `.exe`, `.apk` or `.nro`?** A finished program contains the game's code, which isn't ours to hand out. So everything is built on your own computer from your own game file, and nothing of the game is ever uploaded.

**You need** the North American version 1.0 of the game as a ROM file (`.z64`, `.n64` or `.v64`). Kyujo checks that it is the right one and never changes it.

## What's in this release

- The real game code running natively on all four systems, with the same in-game menu everywhere (F1 or Esc; on a pad, its menu or Back button).
- **Controllers:** keyboard and gamepads, one profile each, a live button test, rebinding by choosing a button, a dead zone, and the stick as a D-pad. On Android, on-screen controls appear while no gamepad is connected.
- **Game Boy Tower and Transfer Pak** with your own Game Boy games and saves (US Red, Blue, Yellow and the Japanese Green). On Android you allow one folder once, and a game's save of the same name comes with it.
- **Saves you can see and back up:** one `.sav` file next to the program. The menu can load another save, start a new one or make a backup copy.
- **Options:** full screen, widescreen stretch, volume, Game Boy loading speed, and a few optional cheats, off by default.
- **Windows setup:** two downloads from github.com, each checked against a fixed hash; everything stays in its folder.
- **One-command builds** for Linux, the AppImage, Android and Switch. Each script fetches and checks the tools it needs and says in plain words what is missing.
- Licensed **GPL-3.0-only**. The decompilation is not copied into this repository and not modified; a few small, documented patches are applied to a copy while building ([`docs/port-patches.md`](docs/port-patches.md)).

## Known issues

- Game Boy Tower battle sound can have small gaps.
- A black frame can appear in some battle camera transitions.
- Loading times are still being worked on.
- Android and the AppImage are the newest ports and have had the least testing on real devices; the Windows setup script is also new. Please report what you find.
- Windows SmartScreen or your antivirus may warn about `kyujo.exe`: the program is built on your computer and unsigned. The setup log shows every step.
- Problems that come from the original game or its decompilation: [`docs/known-decomp-issues.md`](docs/known-decomp-issues.md).

## Reporting a problem

Open an [issue](../../issues/new/choose) with your system and what happened, and attach the log (`kyujo-log.txt` next to the program; `KYUJO-SDL-LOG.TXT` on Switch and Android). It contains no game data. Please don't attach the game file, a built program or a save you want to keep private.

## Credits

Thanks to **Team Hippodrome** and **Pret** for the decompilation, whose years of work make Kyujo possible. Other components and their licences: [`THIRD-PARTY.md`](THIRD-PARTY.md).

*Not affiliated with or endorsed by Nintendo, Game Freak, Creatures Inc. or The Pokémon Company. Game and company names are used only to say which game and hardware Kyujo works with ([`NOTICE.md`](NOTICE.md)).*
