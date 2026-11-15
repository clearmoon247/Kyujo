#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
#
# Builds Kyujo for Linux as a single AppImage file. One command:   ./build-appimage.sh
#
# It runs ./build-linux.sh first (see that file for what it needs), then packs the program into build/Kyujo-x86_64.AppImage.
# Your ROM is not inside: the AppImage looks for it in ~/.local/share/Kyujo (or next to the AppImage).
# One extra package:   sudo apt install squashfs-tools     (and python3-pil, curl, if not installed yet)
# Settings (all optional): the same as build-linux.sh (KYUJO_DECOMP, KYUJO_WORK).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
WORK="${KYUJO_WORK:-$PWD/.work}"
fail() { echo; echo "Kyujo build stopped: $*" >&2; exit 1; }

for t in curl mksquashfs python3 sha256sum; do
    command -v "$t" >/dev/null 2>&1 || fail "'$t' is not installed. On Debian/Ubuntu: sudo apt install squashfs-tools curl python3 python3-pil"
done
python3 -c "import PIL" 2>/dev/null || fail "Python's Pillow is missing: sudo apt install python3-pil   (or: pip install pillow)"

bash ./build-linux.sh       # builds build/linux32/kyujo (and the zip for Batocera)

echo
echo "== Making the AppImage"
TOOL="$WORK/appimagetool-1.9.0-x86_64.AppImage"
SHA=46fdd785094c7f6e545b61afcfb0f3d98d8eab243f644b4b17698c01d06083d1
if [ ! -f "$TOOL" ] || [ "$(sha256sum "$TOOL" | cut -d' ' -f1)" != "$SHA" ]; then
    echo "   downloading the AppImage runtime (from the official appimagetool release, about 12 MB, checked)"
    mkdir -p "$WORK"
    curl -fL --retry 3 -sS -o "$TOOL.part" "https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage" \
        || fail "could not download the AppImage runtime (needs internet access to github.com)"
    [ "$(sha256sum "$TOOL.part" | cut -d' ' -f1)" = "$SHA" ] || { rm -f "$TOOL.part"; fail "the downloaded file is not the expected one (SHA-256 differs); it was not used"; }
    mv "$TOOL.part" "$TOOL"
fi
python3 tools/native/package_appimage.py --program build/linux32/kyujo --probe build/linux32/kyujo_probe \
    --appdir "$WORK/AppDir" --out build/Kyujo-x86_64.AppImage --runtime-from "$TOOL" >/dev/null \
    || fail "packing the AppImage failed; the message above says why"

cat <<DONE

Done: build/Kyujo-x86_64.AppImage
  Run it:  chmod +x build/Kyujo-x86_64.AppImage && ./build/Kyujo-x86_64.AppImage
  Put your game file (.z64 / .n64 / .v64) in ~/.local/share/Kyujo (or next to the AppImage). Settings, saves and the log go there too.
  If it says it needs FUSE:  sudo apt install libfuse2   (or run it with --appimage-extract-and-run).
  Batocera has no AppImage launcher: use build/Kyujo-Linux.zip there.
This program contains the game's code: it is for you; do not share it (see NOTICE.md).
DONE
