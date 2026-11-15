<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Dan B. Cooper -->
# Building Kyujo

You only need this page to build for Linux, Android or Switch, or to work on Kyujo. **On Windows you don't build anything by hand**: download `Kyujo-Setup-Windows.zip` from the [latest release](../../releases/latest) and double-click `Setup.bat` ([details](#windows)).

**You never need your game file to build.** The program reads it when you play. A finished program contains the game's code, so it is for your own use: don't share it or attach it to a release (see [`NOTICE.md`](NOTICE.md)).

| System | Command | Notes |
|---|---|---|
| [Windows](#windows) | the setup from the release page | double-click |
| [Linux and Batocera](#linux-and-batocera) | `./build-linux.sh` (a zip) or `./build-appimage.sh` (one AppImage file) | a few minutes |
| [Android](#android) | `./build-android.sh` | first run 30 to 90 minutes, about 4 GB of free disk space |
| [Nintendo Switch](#nintendo-switch) | `./build-switch.sh` | first run 30 to 90 minutes |

All of these run on Linux, or on Windows inside **WSL** (see [below](#on-windows-use-wsl)). Each script fetches and builds the tools it needs by itself, checks every download, and tells you in plain words if something is missing.

Get the sources first, either way works:

```
git clone --recurse-submodules https://github.com/clearmoon247/Kyujo.git
cd Kyujo
```

or download the source zip from the [releases page](../../releases) (or the green **Code** button), unpack it and open a terminal in that folder. The scripts download the game's decompilation (by Team Hippodrome) themselves in both cases.

**Run the scripts as your normal user, without `sudo`** (`./build-switch.sh`, not `sudo ./build-switch.sh`). A script asks for your password itself, only at the moment it has to install something (devkitPro on the Switch build). Running everything as root leaves root-owned files in your folder, so the scripts refuse to. If a script says "Permission denied" (some unzip tools drop the executable flag), run it as `bash ./build-switch.sh`, or once `chmod +x *.sh`.

## Windows

1. Download **`Kyujo-Setup-Windows.zip`** from the [latest release](../../releases/latest) and unzip it into a **short** folder such as `C:\Kyujo` (not deep inside Downloads or OneDrive: Windows limits path length).
2. Double-click **`Setup.bat`** (or drag your game file onto it). Choose your game file when asked.
3. Wait a few minutes; don't close the window. The first run downloads two things from github.com and checks each against a fixed hash: the compiler (`llvm-mingw`, about 140 MB) and the game's source code from Team Hippodrome (about 43 MB).
4. Start **Play Kyujo** from the same folder.

It writes only inside its own folder, installs nothing and needs no administrator rights. To remove everything, delete the folder. If a message says `SETUP STOPPED`, it says what to fix; run `Setup.bat` again and it continues where it can. The logs are `work\logs\setup.log` and `work\logs\build.log` (they contain no game data).

For people who maintain the setup, or want an offline copy: [`docs/windows-setup.md`](docs/windows-setup.md).

## Linux and Batocera

On a 64-bit Linux PC (or WSL) with clang 18 or newer:

```
sudo apt install clang lld llvm git make python3     # Debian / Ubuntu; other systems: the same packages
./build-linux.sh
```

The script checks your tools, fetches the decompilation, makes the small C library the program needs (once), builds, and writes **`build/Kyujo-Linux.zip`**. It takes a few minutes and needs the internet the first time.

- **Batocera:** unpack the zip, copy `Kyujo.sh` and the folder `kyujo` into `roms/ports`, copy your game file into the folder `kyujo`, update the game list, and start **Kyujo** from the Ports menu. A longer guide, with a test program for window, pad and sound: [`docs/batocera.md`](docs/batocera.md).
- **Other Linux:** `./kyujo/kyujo --rom /path/to/your-game.z64` (a 64-bit Linux with X11 and a PulseAudio-compatible sound server; no 32-bit libraries are needed).

**As one AppImage file** (for a Linux desktop; Batocera has no AppImage launcher, use the zip there):

```
sudo apt install squashfs-tools curl python3-pil     # one-time, on top of the packages above
./build-appimage.sh
```

It runs `./build-linux.sh` first, then writes **`build/Kyujo-x86_64.AppImage`** (about 3 MB). Start it with `chmod +x build/Kyujo-x86_64.AppImage && ./build/Kyujo-x86_64.AppImage`. Your game file is not inside it: put it in `~/.local/share/Kyujo` (or next to the AppImage); settings, saves and the log are kept there too. If it asks for FUSE, run `sudo apt install libfuse2` once, or start it with `--appimage-extract-and-run`. The script downloads the official AppImage runtime (from the `appimagetool` 1.9.0 release, checked against a fixed hash). Like every built program, the AppImage contains the game's code: it is for you, not for sharing.

Settings (optional): `KYUJO_DECOMP=<folder or zip of the decompilation>` to use one you already have; `KYUJO_WORK=<folder>` for the C library (default `.work`).

## Android

```
sudo apt install git clang lld llvm cmake ninja-build make python3 python3-pil unzip curl default-jdk-headless   # once (Debian / Ubuntu / WSL)
./build-android.sh
```

The script asks once to accept Google's Android SDK licence (or run it with `KYUJO_YES=1`), downloads the SDK and NDK (about 1.5 GB), builds its own copies of the other tools, compiles the game, and writes **`build/Kyujo-Android.apk`**. The first run takes 30 to 90 minutes depending on your computer; later runs reuse the tools and take about 20.

Install the APK on your phone (copy it over and open it; allow "install unknown apps" for the app you open it from), or with a cable: `adb install build/Kyujo-Android.apk`. Open **Kyujo**, tap **Choose the Pokemon Stadium ROM**, pick your game file, tap **Play**. The app runs on arm64 and x86_64 devices (Android 7 or newer). More about the app: [`android/README.md`](android/README.md).

## Nintendo Switch

```
sudo apt install git clang lld llvm cmake ninja-build make python3 python3-pil unzip curl wget   # once (Debian / Ubuntu / WSL)
./build-switch.sh
```

The script builds its own copies of the WebAssembly tools, then needs **devkitPro**, the Switch homebrew toolchain. If you don't have it, the script offers to install it with devkitPro's own installer (it asks for `sudo` once and installs into `/opt/devkitpro`); if you already have it (`DEVKITPRO` set, or `/opt/devkitpro`), it uses that. It writes **`build/kyujo.nro`**, with the Kyujo icon. The first run takes 30 to 90 minutes; the slow part is the large generated C code.

Copy `kyujo.nro` to the SD card as `switch/kyujo/kyujo.nro`, put your game file in the same folder (or choose it in the game's menu), and start **Kyujo** from the Homebrew Menu. Details and measurements: [`docs/switch-android-plan.md`](docs/switch-android-plan.md).

## On Windows: use WSL

The Linux, Android and Switch scripts need Linux. On Windows 10/11 that means WSL: open PowerShell as administrator, run `wsl --install` (it installs Ubuntu), restart, open **Ubuntu** from the Start menu, and follow the Linux steps above inside it. Your Windows files are under `/mnt/c/...`, but build in your Linux home folder (`~`): it is much faster. The finished `.apk` or `.nro` can be copied out with `explorer.exe .`.

## Settings you will rarely need

All optional, set in front of the command: `KYUJO_DECOMP=<folder or zip>` to use a copy of the decompilation you already have; `KYUJO_WORK=<folder>` for where the tools are kept (default `.work`: about 3 GB for Android, less for the others); `KYUJO_JOBS=<n>` for the number of parallel jobs (default from your CPU count and free memory; fewer jobs use less memory); `KYUJO_YES=1` to answer "yes" to the questions (Android licence, installing devkitPro).

## For developers

```
tools/native/check_build.sh ext/decomp <work> build        # builds Linux and Windows programs and runs every check that needs no Windows
python3 tests/native/srctree/test_srctree.py                # the patch applier and the source checks
cmake -S . -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host --output-on-failure   # portable C++ helpers
```

More: [`docs/README.md`](docs/README.md) (index of the developer documentation), [`docs/build-windows-linux.md`](docs/build-windows-linux.md) (every step of the native build and what each check proves) and [`CONTRIBUTING.md`](CONTRIBUTING.md).
