<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Dan B. Cooper -->
# Android app

The Android build of the native port: the same game library as the Switch (wasm2c of the game, the GPU renderer, `shell/sdl/main_sdl.c`) in an app whose Java side makes the
file handling and the on-screen controls. No Gradle: `tools/android/build_apk.py` calls the SDK's own tools.

## For the player

1. Install `kyujo-android.apk` (allow "install unknown apps" for the app you open it from).
2. Open **Kyujo**, tap **Choose the Pokemon Stadium ROM** and pick your own ROM file. The app copies it into its private folder; it is never uploaded.
3. Tap **Play**. Landscape only.
4. Game Boy Tower: in the game's own menu (Back or the MENU button, then Game Boy cartridges, a controller) the page is the same as on the other platforms. The first time, **Game Boy ROM** asks you to allow
   the folder that holds your Game Boy games and saves (once; it is remembered; newer Android does not allow the Download folder itself, so use a folder inside it). The games of that folder are listed: tap one.
   The save with the same name next to it (Game.gb, Game.sav) comes with it automatically and is stored as `<name>.sav`, so the page shows `<name>.sav (automatic)`; without a save the game starts a new one.
   "Other folder" changes the folder; "Pick one file instead" (in the first question) takes a single game file. **Save file** lists the saves of the folder to use a different one (shown under its own name;
   **Use the automatic save file name** goes back). A save the game already had is never lost: if a different one comes in, the old one is kept as `<name>-backup-N.sav`.
   The launcher's **Add a Game Boy game and its save** does the same without the game.

Controls: with no gamepad connected, an N64 pad is drawn over the game (D-pad or analog stick on the left, A/B, the four C buttons, L, Z, R, Start; the small MENU button
opens the game's own menu, as Back does). A gamepad (Bluetooth or USB) hides it and is configured in the game's controller menu like on every other platform.
The launcher's "On-screen controls" button cycles D-pad / analog stick / off.

Saves: the app's private storage. Uninstalling or clearing the data removes them, so use **Save a backup of the saves and settings** now and then (a zip you keep wherever you like; the ROM is not in it).
For a bug report use **Save the log** and send that file.

## Layout

| Path | What |
|---|---|
| `AndroidManifest.xml` | landscape, OpenGL ES 3.0, no permissions (files go through the system file picker) |
| `java/org/kyujo/app/LauncherActivity.java` | first screen: Play, ROM / GB import, backup, log, controls setting |
| `java/org/kyujo/app/GameActivity.java` | SDL's activity; adds the controls; hands the app folder to the game; opens the system's file picker for the game's Game Pak menu (`pickGb`) |
| `java/org/kyujo/app/TouchOverlay.java` | the on-screen controls (multi-touch, JNI to the game) |
| `java/org/kyujo/app/Storage.java` | the app folder, importing a chosen document, the backup zip, the logs |

SDL2's Java layer (`org.libsdl.app`) is compiled from the SDL source the build is given.

## Build

Most people should run `./build-android.sh` from the repository root; it fetches every tool below and checks it ([`BUILDING.md`](../BUILDING.md#android)). The command it runs, for people who want to drive the steps by hand:

    tools/android/build_apk.py --sdk $ANDROID_SDK --ndk $ANDROID_NDK --sdl SDL2-2.32.x-source \
        --wasm bw/kyujo.wasm --wabt wabt-checkout --out bw/android [--abis arm64-v8a x86_64]

Needs CMake, Ninja, a JDK 11+, Pillow, the Android SDK (build-tools 34, platform 34) and NDK r27. The package is signed with a generated debug key unless `--keystore` is given.
`--abis x86_64` adds the library for the emulator and for Chromebooks.

## How the game is wired in

* `main()` of `shell/sdl/main_sdl.c` is SDL's `SDL_main` here; the game runs in SDL's thread of `libmain.so`. Its folder is `getFilesDir()` (`SDL_AndroidGetInternalStoragePath`).
* Touch: `TouchOverlay` → `nativeTouch` → one more pad named "Touch controls" in `read_input`, used only while no gamepad is connected.
* Lifecycle: pausing the activity (home, screen off, a call) holds the game still and silences it (`pause_gate`); coming back rebinds the EGL surface (`sSurfaceEpoch`).
* Hints: Back goes to the game as a button (it opens the menu) instead of closing the app; the event loop is not blocked in the background.
