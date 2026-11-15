#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Makes the Windows resource file (program icon and version text) that build_native.py links into kyujo.exe.

    make_resource(work_dir) -> path of app.res, or None (with a printed reason) when it cannot be made

Only small sizes are embedded: the program image must keep 1.75 MiB free below 0x01000000 (tools/native/imagelimit.py), and a 256-pixel icon costs 120 KB of it.
The icon is assets/icon.ico, made from assets/icon.jpg (original artwork drawn by tools/make_icon.py) with the Pillow call below (so a machine without
Pillow, such as the Windows setup's embedded Python, uses the file as it is); the .rc text is compiled with llvm-rc. Nothing here is needed for the
program to work: when llvm-rc is missing the build goes on without an icon.
"""
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent

RC_TEXT = '''1 ICON "icon.ico"
1 VERSIONINFO
FILEVERSION 0,1,0,0
PRODUCTVERSION 0,1,0,0
FILEOS 0x40004
FILETYPE 1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "FileDescription", "Kyujo (test build, unofficial)"
      VALUE "ProductName", "kyujo"
      VALUE "LegalCopyright", "Not affiliated with Nintendo, Game Freak, Creatures or The Pokemon Company. Port code: GPL-3.0-only (see NOTICE.md). The game code in this program is not covered."
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
'''


def make_ico(dest):
    """assets/icon.jpg -> a multi-size .ico at `dest` (Pillow). Returns False, with a printed reason, when that cannot be done."""
    try:
        from PIL import Image
    except ImportError:
        print("   (no assets/icon.ico and no Pillow: building without an icon)")
        return False
    src = REPO / "assets" / "icon.jpg"
    if not src.is_file():
        print("   (assets/icon.jpg missing: building without an icon)")
        return False
    Image.open(src).convert("RGBA").save(dest, format="ICO", sizes=[(16, 16), (32, 32), (48, 48)])
    return True


def make_resource(work_dir):
    work = Path(work_dir)
    work.mkdir(parents=True, exist_ok=True)
    rc_tool = shutil.which("llvm-rc") or shutil.which("llvm-rc-18")
    if not rc_tool:
        print("   (no llvm-rc: building without an icon)")
        return None
    ico = REPO / "assets" / "icon.ico"
    if ico.is_file():
        shutil.copyfile(ico, work / "icon.ico")
    else:                                                       # make it from the picture (needs Pillow); `winres.py --write-ico` stores the result
        if not make_ico(work / "icon.ico"):
            return None
    (work / "app.rc").write_text(RC_TEXT, encoding="ascii")
    r = subprocess.run([rc_tool, "/FO", str(work / "app.res"), str(work / "app.rc")], cwd=str(work), capture_output=True, text=True)
    if r.returncode != 0 or not (work / "app.res").is_file():
        print("   (llvm-rc failed, building without an icon): " + (r.stderr or r.stdout).strip()[:300])
        return None
    return str(work / "app.res")


if __name__ == "__main__" and "--write-ico" in sys.argv:
    make_ico(REPO / "assets" / "icon.ico")
    print("wrote assets/icon.ico")
