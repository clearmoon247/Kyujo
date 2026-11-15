#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Builds an AppDir (and, when given the AppImage runtime, an AppImage) from the built Linux program.

    package_appimage.py --program <out>/linux32/kyujo --probe <out>/linux32/kyujo_probe --appdir <dir> [--out Kyujo.AppImage (--runtime-file <runtime-x86_64> | --runtime-from <appimagetool.AppImage>) [--mksquashfs <path>]]

The AppDir holds AppRun (tools/linux/appimage/AppRun), the .desktop file, the icon (assets/icon.jpg as PNG), usr/bin/kyujo (the game: one
static 32-bit program, no libraries to bundle) and kyujo_probe, and the licence files. The ROM is not in it.
THE APPIMAGE CONTAINS THE GAME'S CODE (compiled from the decompilation): for the builder's own use only, never to be published.
The game's data folder is outside the image (see AppRun). Needs Pillow for the icon.
"""
import argparse, os, shutil, subprocess, sys, tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent

DESKTOP = """[Desktop Entry]
Type=Application
Name=Kyujo
Comment=Native port of the game; needs your own ROM
Exec=AppRun
Icon=kyujo
Categories=Game;
Terminal=false
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--program", required=True)
    ap.add_argument("--probe", required=True)
    ap.add_argument("--appdir", required=True)
    ap.add_argument("--out")
    ap.add_argument("--runtime-file", help="the AppImage type-2 runtime (runtime-x86_64)")
    ap.add_argument("--runtime-from", help="take the runtime from the front of this AppImage (the official appimagetool release is one)")
    ap.add_argument("--mksquashfs", default="mksquashfs")
    a = ap.parse_args()
    d = Path(a.appdir)
    if d.exists():
        shutil.rmtree(d)
    (d / "usr" / "bin").mkdir(parents=True)
    shutil.copy2(a.program, d / "usr" / "bin" / "kyujo")
    shutil.copy2(a.probe, d / "usr" / "bin" / "kyujo_probe")
    shutil.copy2(REPO / "tools" / "linux" / "appimage" / "AppRun", d / "AppRun")
    for n in ("usr/bin/kyujo", "usr/bin/kyujo_probe", "AppRun"):
        os.chmod(d / n, 0o755)
    (d / "kyujo.desktop").write_text(DESKTOP)
    from PIL import Image
    Image.open(REPO / "assets" / "icon.jpg").convert("RGB").save(d / "kyujo.png")
    shutil.copy2(d / "kyujo.png", d / ".DirIcon")
    doc = d / "usr" / "share" / "doc" / "kyujo"
    doc.mkdir(parents=True)
    for n in ("LICENSE", "NOTICE.md", "THIRD-PARTY.md"):
        if (REPO / n).exists():
            shutil.copy2(REPO / n, doc / n)
    print("AppDir: %s" % d)
    if a.out:
        if not (a.runtime_file or a.runtime_from):
            sys.exit("--out needs --runtime-file or --runtime-from")
        if a.runtime_from:      # the runtime is the ELF program at the front: everything up to the end of its section headers
            import struct
            head = Path(a.runtime_from).read_bytes()
            shoff, = struct.unpack_from("<Q", head, 0x28)
            shentsize, shnum = struct.unpack_from("<HH", head, 0x3A)
            end = shoff + shentsize * shnum
            if head[:4] != b"\x7fELF" or head[end:end + 4] != b"hsqs":
                sys.exit("%s does not look like an AppImage" % a.runtime_from)
            runtime = head[:end]
        else:
            runtime = Path(a.runtime_file).read_bytes()
        # An AppImage (type 2) is the runtime program followed by a squashfs of the AppDir; that is all appimagetool does too.
        fs = Path(str(a.out) + ".squashfs")
        if fs.exists():
            fs.unlink()
        subprocess.run([a.mksquashfs, str(d), str(fs), "-root-owned", "-noappend", "-comp", "zstd", "-quiet"], check=True)
        with open(a.out, "wb") as o:
            o.write(runtime)
            o.write(fs.read_bytes())
        fs.unlink()
        os.chmod(a.out, 0o755)
        print("AppImage: %s" % a.out)


if __name__ == "__main__":
    main()
