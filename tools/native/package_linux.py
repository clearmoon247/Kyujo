#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Packs the built Linux program (for Batocera and other Linux systems) and its files into one zip.

    package_linux.py --program <out>/linux32/kyujo --probe <out>/linux32/kyujo_probe [--decomp <ignored>] --out <file.zip>

The zip is meant to be unpacked into Batocera's ports folder (/userdata/roms/ports). It holds:
    Kyujo.sh                  the start script for the Ports menu (tools/linux/kyujo-launcher.sh)
    kyujo/kyujo             the game (one static 32-bit program; runs on a 64-bit Linux that has the 32-bit loader)
    kyujo/kyujo_probe       the test program for the window, gamepads and sound (no game code in it)
    kyujo/run-probe.sh            runs the probe and keeps its output in probe-output.txt
    kyujo/README-BATOCERA.md      docs/batocera.md
    kyujo/NOTICE.md, LICENSE, THIRD-PARTY.md, BUILD-INFO.txt

THE ZIP CONTAINS THE GAME'S CODE (the program is compiled from the decompilation). It is for the builder's own use only: never
commit it, never upload it anywhere public. The ROM is not in it. Standard library only.
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
    except Exception as e:
        return "unknown (%s)" % e.__class__.__name__


def add(z, path, name, mode=0o644):
    info = zipfile.ZipInfo(name, date_time=time.localtime()[:6])
    info.external_attr = (0o100000 | mode) << 16                   # a regular file with these unix permissions
    info.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(info, Path(path).read_bytes() if not isinstance(path, bytes) else path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--program", required=True)
    ap.add_argument("--probe", required=True)
    ap.add_argument("--decomp", help="accepted for old command lines; the decomp commit is read from port/decomp-manifest.txt")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    prog, probe = Path(a.program), Path(a.probe)
    for p in (prog, probe):
        if not p.is_file():
            sys.exit("no such file: %s" % p)
        h = p.read_bytes()[:20]
        if h[:4] != b"\x7fELF" or h[4] != 1 or h[18:20] != b"\x03\x00":
            sys.exit("%s is not a 32-bit x86 ELF program" % p)
    sha = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (prog, probe)}
    patches = sorted(p.name for p in (REPO / "port" / "patches").glob("*.patch"))
    dirty = git(["status", "--porcelain"], REPO)
    info = [
        "Kyujo - Linux / Batocera build (32-bit x86, static)",
        "built:            %s" % time.strftime("%Y-%m-%d %H:%M:%S %z"),
        "port commit:      %s%s" % (git(["rev-parse", "HEAD"], REPO), "  (WORKING TREE HAD UNCOMMITTED CHANGES)" if dirty and not dirty.startswith("unknown") else ""),
        "decomp commit:    %s  (pinned in port/decomp-manifest.txt; every file the build reads was checked against it; not modified, port patches are applied to a copy)" % srctree.read_manifest()[0],
        "port patches:     %d (%s ... %s)" % (len(patches), patches[0] if patches else "-", patches[-1] if patches else "-"),
        "kyujo:      %d bytes, SHA-256 %s" % (prog.stat().st_size, sha[prog.name]),
        "kyujo_probe:%d bytes, SHA-256 %s" % (probe.stat().st_size, sha[probe.name]),
        "",
        "HOW TO USE: Batocera: copy Kyujo.sh and the folder kyujo into roms/ports, put your ROM into the folder kyujo, start Kyujo from the Ports menu.",
        "Other Linux: ./kyujo/kyujo --rom <your ROM>. More: README-BATOCERA.md.",
        "",
        "It contains the game's code, which is not ours. Do not share it. The port's own code is GPL-3.0-only, but this program combines it with code that has no stated licence (see NOTICE.md).",
    ]
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w") as z:
        add(z, HERE.parent / "linux" / "kyujo-launcher.sh", "Kyujo.sh", 0o755)
        add(z, prog, "kyujo/kyujo", 0o755)
        add(z, probe, "kyujo/kyujo_probe", 0o755)
        add(z, HERE.parent / "linux" / "run-probe.sh", "kyujo/run-probe.sh", 0o755)
        add(z, REPO / "docs" / "batocera.md", "kyujo/README-BATOCERA.md")
        add(z, REPO / "NOTICE.md", "kyujo/NOTICE.md")
        add(z, REPO / "LICENSE", "kyujo/LICENSE")
        add(z, REPO / "THIRD-PARTY.md", "kyujo/THIRD-PARTY.md")
        add(z, ("\n".join(info) + "\n").encode(), "kyujo/BUILD-INFO.txt")
    print("wrote %s (%d bytes)" % (out, out.stat().st_size))
    for k, v in sha.items():
        print("  %s SHA-256 %s" % (k, v))


if __name__ == "__main__":
    main()
