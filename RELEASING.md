<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Dan B. Cooper -->
# Making a release

For maintainers. A release carries **one file players need**, the Windows setup, plus the source that GitHub adds. It **never carries a built program**: `.exe`, `.nro`, `.apk`, AppImage, the Linux zip and the offline setup all contain the game's code (see [`NOTICE.md`](NOTICE.md)).

1. Check that everything is committed and the tests pass:
   ```
   tools/native/check_build.sh ext/decomp <work> build
   python3 tests/native/srctree/test_srctree.py
   ```
2. Choose the version number (`X.Y.Z`), write it in the file `VERSION` (the Android app and the Switch show it; it says `dev` until then), commit, and tag it:
   ```
   git tag -a vX.Y.Z -m "Kyujo X.Y.Z"
   git push origin vX.Y.Z
   ```
3. Make the setup zip from the tagged commit. The name stays the same for every release, so the README link never changes:
   ```
   python3 tools/native/package_windows_setup.py --edition online --out Kyujo-Setup-Windows.zip
   ```
4. Create the release and attach that one file:
   ```
   gh release create vX.Y.Z Kyujo-Setup-Windows.zip --title "Kyujo X.Y.Z" --notes-file RELEASE-NOTES.md
   ```
   The notes should say, in plain words: what is new or fixed, anything players must know, and this line: *"Windows: download `Kyujo-Setup-Windows.zip`, unzip it into a short folder, double-click `Setup.bat`. Other systems: see BUILDING.md."*
5. Open the release page and check that the zip downloads, and that it contains `Setup.bat`, `setup\`, `app\` and `README-SETUP.txt` and **nothing** under `downloads\`.

If the pinned decompilation commit changes, regenerate `port/decomp-manifest.txt` (`python3 tools/native/srctree.py manifest --decomp <checkout>`) and test a full setup before tagging.
