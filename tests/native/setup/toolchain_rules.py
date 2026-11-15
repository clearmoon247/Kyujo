#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Applies the unpacking rules of dist/windows-setup/setup/components.json to the llvm-mingw zip, the way Setup.ps1 does (same order:
drop the top folder, drop what the 'skip' wildcards match, then move what the 'map' entries name), and checks that everything in 'needs' exists.

    toolchain_rules.py llvm-mingw-20240619-ucrt-x86_64.zip [--extract DIR]

Why a Python copy of a PowerShell loop: PowerShell cannot be run where this was written, so the RULES (data) are tested here, with the
only difference being the wildcard matcher (fnmatch for PowerShell's -like: both are case-insensitive, `*` matches anything including `/`).
--extract writes the files, so a build can be tried against the result (the headers and libraries are the same on every host).
"""
import fnmatch
import hashlib
import json
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
COMPONENTS = os.path.join(HERE, "..", "..", "..", "dist", "windows-setup", "setup", "components.json")


def like(text, pattern):
    return fnmatch.fnmatch(text.lower(), pattern.lower())


def plan(names, c):
    """-> {destination path: source name} for the zip's file entries."""
    out = {}
    for name in names:
        if not name.startswith(c["top"]):
            continue
        rel = name[len(c["top"]):]
        if not rel:
            continue
        if any(like(rel, p) for p in c["skip"]):
            continue
        skipped = False
        for m in c.get("map", []):
            if rel.startswith(m["from"]):
                skipped = any(like(rel, p) for p in m.get("skip", []))
                rel = m["to"] + rel[len(m["from"]):]
                break
        if skipped or not rel or rel.endswith("/"):
            continue
        out[rel] = name
    return out


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    zpath = args[0]
    dest = args[args.index("--extract") + 1] if "--extract" in args else None
    c = json.load(open(COMPONENTS, encoding="utf-8"))["toolchain"]
    h = hashlib.sha256()
    with open(zpath, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    print("zip SHA-256 %s: %s" % (h.hexdigest(), "matches components.json" if h.hexdigest() == c["sha256"] else "DIFFERENT from components.json"))
    z = zipfile.ZipFile(zpath)
    p = plan([i.filename for i in z.infolist()], c)
    size = sum(z.getinfo(src).file_size for src in p.values())
    print("%d files, %.0f MB unpacked" % (len(p), size / 1e6))
    bad = [n for n in c["needs"] if n not in p]
    for n in c["needs"]:
        print("  %-60s %s" % (n, "ok" if n in p else "MISSING"))
    longest = max(p, key=len)
    print("longest unpacked path below work\\tc: %d characters (%s)" % (len(longest), longest))
    if dest:
        for rel, src in p.items():
            target = os.path.join(dest, *rel.split("/"))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(z.read(src))
        print("extracted to", dest)
    sys.exit(1 if bad or h.hexdigest() != c["sha256"] else 0)


if __name__ == "__main__":
    main()
