#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Makes the Windows setup kit: a zip that a player unpacks, runs Setup.bat from, and ends up with a game built on their own computer.

    package_windows_setup.py --edition online  --out Kyujo-Setup-Windows.zip
    package_windows_setup.py --edition offline --out Kyujo-Setup-offline.zip \\
        --toolchain-zip llvm-mingw-20240619-ucrt-x86_64.zip --decomp <decomp checkout, folder or zip>

Both editions contain the port's own files (the build scripts, the platform code, the patches) and the setup. Neither contains a game
program or anything taken from a ROM.
  online   Small. On the player's computer the setup downloads the compiler (github.com/mstorsjo/llvm-mingw, checked by SHA-256) and the
           game's source code (from the official place named in port/decomp-manifest.txt, at the pinned commit, every file checked against the manifest).
  offline  Also holds those two downloads (downloads\\), so nothing is fetched. The decomp part is a zip of only the 1,160 files the build
           reads. THIS EDITION CONTAINS THE DECOMPILATION'S SOURCE FILES, which have no stated licence: it is for the maker's own use and
           trusted people, and must not be published (the kit says so in PRIVATE-DO-NOT-PUBLISH.txt).

Standard library only. Run it from a checkout of this repository (it copies the files `git ls-files` lists, so uncommitted new files are
not included; commit first).
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import srctree  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
KIT = os.path.join(REPO, "dist", "windows-setup")
# What the build needs from this repository, laid out the same way inside the kit's app\ folder.
APP_PATHS = ["tools/native", "tools/rom_tool.py", "src", "port", "assets/icon.ico", "NOTICE.md", "LICENSE", "THIRD-PARTY.md"]
SKIP_NAMES = ("__pycache__",)
SKIP_SUFFIXES = (".pyc", ".sh")                # the .sh files are developer scripts for Linux
SKIP_PREFIXES = ("tools/native/test_", "tools/native/census", "tools/native/ubsan", "tools/native/uninit", "tools/native/profile_report",
                 "tools/native/check_struct_layout", "tools/native/mkpatch", "tools/native/patch_index", "tools/native/build_test",
                 "tools/native/gen_gbjp_names", "tools/native/package_", "src/common/")


def git(*args):
    return subprocess.run(["git", "-C", REPO] + list(args), capture_output=True, text=True, check=True).stdout


def app_files():
    out = []
    for rel in git("ls-files", "--", *APP_PATHS).splitlines():
        if any(part in SKIP_NAMES for part in rel.split("/")) or rel.endswith(SKIP_SUFFIXES) or rel.startswith(SKIP_PREFIXES):
            continue
        out.append(rel)
    return sorted(out)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def build_files_zip(decomp, dest):
    """A zip of only the files in the manifest (read from `decomp`, verified first), laid out under decomp-<commit>/ (the setup finds the root folder itself)."""
    import tempfile
    commit, entries = srctree.read_manifest()
    with tempfile.TemporaryDirectory() as tmp:
        folder = srctree.open_source(decomp, tmp, quiet=True)        # raises with the list of differences if it is not the pinned decomp
        with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for sha, size, rel in entries:
                info = zipfile.ZipInfo("decomp-%s/%s" % (commit, rel), date_time=(2026, 1, 1, 0, 0, 0))     # fixed time: same input, same zip
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                with open(os.path.join(folder, *rel.split("/")), "rb") as f:
                    z.writestr(info, f.read())
    return commit


def readme(edition, commit, components):
    c = components["toolchain"]
    online = edition == "online"
    lines = [
        "KYUJO FOR WINDOWS - SETUP" + ("" if online else " (OFFLINE edition)"),
        "=" * 70,
        "",
        "This builds Kyujo on YOUR computer from the open-source decompilation of the game and your own copy of it. Nothing in this kit is a game",
        "program, and nothing in it was taken from a game file.",
        "",
        "YOU NEED: the North American (USA) version 1.0 of the game as a ROM file (.z64, .n64 or .v64), Windows 10 or 11, about 1 GB of free space" + (", and the internet (once)." if online else "."),
        "",
        "HOW",
        "  1. Unzip this folder to a SHORT path, for example C:\\Kyujo (not deep inside Downloads or OneDrive: Windows limits path length).",
        "  2. Double-click Setup.bat. Choose your ROM file when asked. (Or drag the ROM file onto Setup.bat.)",
        "  3. Wait a few minutes and don't close the window. When it says Done, start 'Play Kyujo' from this folder.",
        "",
        "PLAYING: F1 or Esc opens the in-game menu (settings, controllers, saves, Game Boy cartridges).",
        "  Keys: arrows = stick, X or Space = A, Z or Backspace = B, C = Z, Enter = START, Q/E = L/R, W A S D = D-pad, I J K L = C buttons. Gamepads work too.",
        "",
        "PROBLEMS: if a message says SETUP STOPPED, it says what to fix; run Setup.bat again afterwards and it continues where it can.",
        "  To report one, open an issue at the project's page and attach work\\logs\\setup.log and work\\logs\\build.log (they contain no game data).",
        "",
        "WHAT THE SETUP DOES",
        "  - checks that your ROM is the supported one (it is read, never modified, never copied unless it needs a byte-order conversion);",
        ("  - downloads the compiler package %s (about %d MB) from github.com and checks its SHA-256 before using it;" % (c["file"], c["size"] // 1000000)) if online else
        ("  - uses the compiler package %s that is in downloads\\ (SHA-256 checked before use);" % c["file"]),
        ("  - downloads the game's source code (about 43 MB, commit %s) from %s;" % (commit[:10], srctree.source_url())) if online else
        ("  - uses the game's source files in downloads\\ (the %d files the build reads);" % len(srctree.read_manifest()[1])),
        "  - checks every source file against app\\port\\decomp-manifest.txt (SHA-256) and stops if one differs;",
        "  - builds Game\\kyujo.exe, checks it, writes Game\\kyujo.cfg with your ROM, and makes a shortcut.",
        "  It writes only inside this folder, installs nothing, and needs no administrator rights. To remove everything, delete the folder.",
        "",
        "SIZE: about 0.6 GB while building (work\\ can be deleted afterwards; the next Setup run then unpacks and builds again).",
        "SmartScreen or antivirus may comment on the new kyujo.exe: it is a program nobody has signed. It is built from the open sources named above.",
        "",
    ]
    if online:
        lines += ["NEEDS THE INTERNET only for the downloads above (github.com). To prepare an offline run, put the files into downloads\\ yourself:",
                  "  %s   from %s" % (c["file"], c["url"]),
                  "  %s   from <source>/archive/%s.zip" % (components["decomp"]["download_name"].replace("{commit}", commit), commit), ""]
    else:
        lines += ["WORKS WITHOUT THE INTERNET: everything it needs is in downloads\\.", "",
                  "THIS EDITION CONTAINS THE DECOMPILATION'S SOURCE FILES (downloads\\decomp-%s-build-files.zip). They have no stated licence." % commit,
                  "Keep this kit private. Read PRIVATE-DO-NOT-PUBLISH.txt.", ""]
    lines += ["LICENCES: this port's own code is GPL-3.0-only (app\\LICENSE). That does not cover the game or the decompilation, which have their own",
              "owners and no stated licence; for that reason the exe you build is for your own use and must not be shared (app\\NOTICE.md explains).",
              "The other components and their licences are listed in app\\THIRD-PARTY.md; the compiler package carries its own licence texts",
              "inside its zip (LLVM: Apache-2.0 with LLVM exception; mingw-w64; Python: PSF).", ""]
    return "\r\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--edition", required=True, choices=("online", "offline"))
    ap.add_argument("--out", required=True, help="the zip to write")
    ap.add_argument("--toolchain-zip", help="offline: the llvm-mingw zip (its SHA-256 must be the one in setup/components.json)")
    ap.add_argument("--decomp", help="offline: the decomp (checkout, folder or zip)")
    a = ap.parse_args()

    with open(os.path.join(KIT, "setup", "components.json"), encoding="utf-8") as f:
        components = json.load(f)
    commit, entries = srctree.read_manifest()
    dirty = git("status", "--porcelain", "--", *(APP_PATHS + ["dist/windows-setup"])).strip()
    files = app_files()
    when = time.strftime("%Y-%m-%d %H:%M:%S %z")

    extra = {}                                                                 # path inside the kit -> bytes or a file path
    if a.edition == "offline":
        if not a.toolchain_zip or not a.decomp:
            sys.exit("--edition offline needs --toolchain-zip and --decomp")
        want = components["toolchain"]["sha256"]
        have = sha256_file(a.toolchain_zip)
        if have != want:
            sys.exit("%s has SHA-256 %s, the setup expects %s" % (a.toolchain_zip, have, want))
        import tempfile
        tmpdir = tempfile.mkdtemp()
        bf = os.path.join(tmpdir, "decomp-%s-build-files.zip" % commit)
        try:
            build_files_zip(a.decomp, bf)
        except srctree.SourceError as e:
            sys.exit(str(e))
        extra["downloads/" + components["toolchain"]["file"]] = a.toolchain_zip
        extra["downloads/decomp-%s-build-files.zip" % commit] = bf
        extra["PRIVATE-DO-NOT-PUBLISH.txt"] = (
            "This offline kit contains the source files of the decompilation (downloads\\decomp-%s-build-files.zip).\r\n"
            "The decompilation project has not published a licence, so nobody may redistribute these files. This kit is for its maker and for\r\n"
            "people the maker trusts. Do not upload it, do not post a link to it. The ONLINE edition contains none of these files.\r\n"
            "(The maker is not a lawyer; this is a precaution, not legal advice.)\r\n" % commit).encode()

    manifest_lines = []
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        def add(name, data_or_path):
            if isinstance(data_or_path, bytes):
                data = data_or_path
                z.writestr(name, data)
            else:
                with open(data_or_path, "rb") as f:
                    data = f.read()
                z.writestr(name, data)
            manifest_lines.append("%s  %s" % (hashlib.sha256(data).hexdigest(), name))
        for rel in ("Setup.bat", "setup/Setup.ps1", "setup/components.json"):
            with open(os.path.join(KIT, *rel.split("/")), "rb") as f:
                data = f.read()
            if rel.endswith((".bat", ".ps1")):                                  # Windows batch files and scripts want CRLF line ends
                data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            add(rel, data)
        add("README-SETUP.txt", readme(a.edition, commit, components).encode("ascii", "replace"))
        for rel in files:
            with open(os.path.join(REPO, rel), "rb") as f:
                data = f.read()
            add("app/" + rel, data)
        for name, src in extra.items():
            add(name, src)
        info = ("Kyujo - Windows setup kit (%s edition)\r\nbuilt: %s\r\nport commit: %s%s\r\ndecomp commit (pinned): %s\r\n"
                % (a.edition, when, git("rev-parse", "HEAD").strip(), "  (UNCOMMITTED CHANGES IN THE PACKAGED FILES)" if dirty else "", commit))
        add("setup/BUILD-INFO.txt", info.encode())
        z.writestr("setup/kit-files.sha256", "\n".join(manifest_lines) + "\n")
    print("wrote %s (%.1f MB): %s edition, %d app files%s" % (a.out, os.path.getsize(a.out) / 1e6, a.edition, len(files),
          ", with the compiler package and the %d decomp files" % len(entries) if a.edition == "offline" else ""))
    if dirty:
        print("WARNING: uncommitted changes in the packaged files:\n" + dirty)


if __name__ == "__main__":
    main()
