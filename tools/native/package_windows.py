#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Packs the built Windows program and its files into one zip for the person who will run it.

    package_windows.py --exe <out>/win32/kyujo.exe [--decomp <ignored>] --out <file.zip>

The zip holds: kyujo.exe (it has no console window, keeps its own log and settings, and needs no .bat file), docs/windows-test-guide.md as README-FIRST.txt-style text (kept as .md),
NOTICE.md and a BUILD-INFO.txt that records which commits it was built from, the exe's SHA-256, and the plain statement that the exe
the statement of what has been tested.

THE ZIP CONTAINS THE GAME'S CODE (the exe is compiled from the decompilation). It is for the builder and own use only:
never commit it, never upload it anywhere public. Standard library only.
"""
import argparse, hashlib, subprocess, sys, time, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import srctree  # noqa: E402
REPO = HERE.parent.parent


def git(args, cwd):
    try:
        return subprocess.run(["git"] + args, cwd=str(cwd), capture_output=True, text=True, check=True).stdout.strip()
    except Exception as e:                                   # a missing git or a non-repository must not stop packaging
        return "unknown (%s)" % e.__class__.__name__


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--decomp", help="accepted for old command lines; the decomp commit is read from port/decomp-manifest.txt")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    exe = Path(a.exe)
    if not exe.is_file():
        sys.exit("no such file: %s" % exe)
    data = exe.read_bytes()
    if data[:2] != b"MZ":
        sys.exit("%s is not a Windows program (no MZ header)" % exe)
    sha = hashlib.sha256(data).hexdigest()
    patches = sorted(p.name for p in (REPO / "port" / "patches").glob("*.patch"))
    dirty = git(["status", "--porcelain"], REPO)
    info = [
        "Kyujo - Windows test build (32-bit x86)",
        "built:            %s" % time.strftime("%Y-%m-%d %H:%M:%S %z"),
        "port commit:      %s%s" % (git(["rev-parse", "HEAD"], REPO), "  (WORKING TREE HAD UNCOMMITTED CHANGES)" if dirty and not dirty.startswith("unknown") else ""),
        "decomp commit:    %s  (pinned in port/decomp-manifest.txt; every file the build reads was checked against it; not modified, port patches are applied to a copy)" % srctree.read_manifest()[0],
        "port patches:     %d (%s ... %s)" % (len(patches), patches[0] if patches else "-", patches[-1] if patches else "-"),
        "kyujo.exe:  %d bytes, SHA-256 %s" % (len(data), sha),
        "",
        "HOW TO USE: put kyujo.exe in a folder of its own and double-click it (a window asks for your ROM). using-kyujo.md in this zip explains the menu and keys.",
        "",
        "It contains the game's code, which is not ours. Do not share it. The port's own code is GPL-3.0-only, but this program combines it with code that has no stated licence (see NOTICE.md).",
    ]
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(exe, "kyujo.exe")
        z.write(REPO / "docs" / "using-kyujo.md", "using-kyujo.md")
        z.write(REPO / "NOTICE.md", "NOTICE.md")
        z.write(REPO / "LICENSE", "LICENSE")
        z.write(REPO / "THIRD-PARTY.md", "THIRD-PARTY.md")
        z.writestr("BUILD-INFO.txt", "\r\n".join(info) + "\r\n")
    print("wrote %s (%d bytes); exe SHA-256 %s" % (out, out.stat().st_size, sha))


if __name__ == "__main__":
    main()
