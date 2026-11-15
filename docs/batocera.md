<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Dan B. Cooper -->
# Kyujo on Batocera (and other Linux)

You need: a Batocera machine (x86-64), your own game file (the North American version 1.0, `.z64`), a way to copy files to the machine (the network share `\\BATOCERA\share`, or a USB stick), and `build/Kyujo-Linux.zip` from `./build-linux.sh` (see [`BUILDING.md`](../BUILDING.md#linux-and-batocera)). The program is one file and brings everything it uses; the machine only has to be able to run 32-bit programs.

## 1. Put it on the machine

1. Unpack the zip on your PC. You get a file `Kyujo.sh` and a folder `kyujo`.
2. Copy **both** into `roms/ports` (on the share: `\\BATOCERA\share\roms\ports`; on the machine: `/userdata/roms/ports`).
3. Copy your game file into the folder `kyujo`, next to the program. The start script finds the first `.z64`, `.n64` or `.v64` there.

## 2. Play

1. In EmulationStation: *Start* menu → *Game settings* → *Update game lists*, open the **Ports** system and start **Kyujo**. (Over SSH: `cd /userdata/roms/ports && ./Kyujo.sh`.)
2. The first start writes `kyujo.cfg` with full screen on. The title screen appears after a few seconds. Everything the program prints goes to `kyujo/kyujo.log` (the previous run's log is kept as `kyujo.log.old`).
3. To leave: open the menu → *Quit game*.

Controls: arrows = stick, X or Space = A, Z or Backspace = B, C = Z trigger, Enter = Start, Q / E = L / R, W A S D = D-pad, I J K L = C buttons, F11 or Alt+Enter = full screen, **F1 or Esc = the menu**. A standard gamepad (Xbox, PlayStation, Switch Pro, 8BitDo, ...) starts with A = A, B or X = B, triggers = Z, bumpers = L / R, Start = Start, D-pad, left stick = stick, right stick = C buttons, **Back / Select = open the menu**. Other pads get a guessed layout you can change in the menu (Controllers). More: [`using-kyujo.md`](using-kyujo.md).

## 3. If something doesn't work: the test program

`kyujo/run-probe.sh` checks the three risky parts, one at a time, without the game. Over SSH, while looking at the screen of the machine:

    cd /userdata/roms/ports/kyujo
    ./run-probe.sh

1. **x11:** opens a window with a colour test picture for 10 seconds (eight colour bars, a grey ramp and a red frame). Keys you press are printed; **F11** tries full screen; **Esc** ends this part early.
2. **pads:** for 10 seconds it prints every gamepad button and stick movement.
3. **sound:** plays a 440 Hz tone for 2 seconds; you should hear a steady beep.

Everything it prints is also saved in `probe-output.txt` in that folder. If a part fails, the file says why (for example "no X authority cookie found" or "no sound server found"). Attach it, and `kyujo.log`, to a bug report.

The first lines of `kyujo.log` say which of these happened:

- `window: no window (...)`: the program could not open a window; the reason follows (`DISPLAY is not set`, `no X authority cookie found`, `the X server refused the connection ...`). It then runs without a window.
- `audio: no sound device: no sound server found; tried ...`: no PulseAudio or PipeWire socket was found at the usual places. It then runs silent.
- `gamepads: cannot open /dev/input/eventN`: permissions.
- Messages about the ROM: it must be the North American version 1.0.

## 4. Good to know

Full screen depends on the window manager (Batocera uses openbox). The Game Boy cartridge features work as on the other systems; the Game Boy file is chosen in the menu.

*Licence note:* the zip contains the game's code (the program is compiled from the decompilation). It is for you only; do not share it. The port's own code is GPL-3.0-only; the game's code in this program is not covered by that (see `NOTICE.md`).
