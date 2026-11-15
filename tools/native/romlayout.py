#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Reads the decomp's ROM layout (yamls/us/rom.yaml) and the documented symbol addresses (linker_scripts/us/*).

Why this exists: the game's C code refers to its own data and overlays through symbols the original linker defined
(`fragment1_ROM_START`, `album_ui_ROM_END`, `D_1002480`...). A native build has no such linker script, so the values are
reproduced here from the two documents the decomp itself uses, and the reproduction is CHECKED against the values the decomp
documents explicitly (see `check()`), so a parsing mistake shows up as a mismatch instead of as a silently wrong ROM offset.

Standard library only (rom.yaml is read with a line parser; it is a regular, hand-formatted file and PyYAML is not available on
the development machine). The parser accepts exactly the shapes used in rom.yaml and stops with an error on anything else.

    romlayout.py <decomp-dir>        prints the cross-check report
"""
import glob
import os
import re
import sys

# `- name: foo`, `type: code`, `start: 0x1000`, `vram: 0x80000400`, `subsegments:`
SEG_KEY = re.compile(r"^\s{2,4}(?:-\s+)?(type|start|vram):\s*(.*?)\s*(#.*)?$")      # the only segment keys this module needs
SUB_LIST = re.compile(r"^\s+-\s+\[\s*(0x[0-9A-Fa-f]+|\d+)\s*(?:,\s*([^\]#]*?))?\s*\]\s*(#.*)?$")
SUB_DICT = re.compile(r"^\s+-\s+\{(.*)\}\s*(#.*)?$")


def _num(text):
    return int(text, 0)


def parse_rom_yaml(path):
    """Returns a list of top-level segments:
         {"name", "type", "start", "vram", "subs": [{"start": int, "type": str, "name": str|None}, ...]}
       Only the ROM-resident subsegments (those given as `[offset, type, name]` lists) are returned in "subs"; `.bss`-style
       dict entries have no ROM position and are skipped."""
    segs, cur, in_subs = [], None, False
    with open(path, encoding="utf-8") as f:
        for ln, line in enumerate(f, 1):
            if line.lstrip().startswith("#") or not line.strip():
                continue
            if re.match(r"^  - name:", line):                       # a new top-level segment
                cur = {"name": line.split(":", 1)[1].split("#")[0].strip(), "type": None, "start": None, "vram": None, "subs": []}
                segs.append(cur)
                in_subs = False
                continue
            if cur is None:
                continue
            if re.match(r"^\s+subsegments:\s*$", line):
                in_subs = True
                continue
            if in_subs:
                m = SUB_LIST.match(line)
                if m:
                    fields = [x.strip() for x in (m.group(2) or "").split(",")] if m.group(2) else []
                    cur["subs"].append({"start": _num(m.group(1)), "type": fields[0] if fields else "",
                                        "name": fields[1] if len(fields) > 1 and fields[1] else None})
                    continue
                if SUB_DICT.match(line):                           # {vram: ..., type: .bss, ...}: no ROM position
                    continue
                if re.match(r"^\s+-\s", line):
                    raise SystemExit("%s:%d: unrecognised subsegment line: %r" % (path, ln, line.rstrip()))
                in_subs = False
            m = SEG_KEY.match(line)
            if m:
                cur[m.group(1)] = m.group(2) if m.group(1) == "type" else _num(m.group(2))
    return segs


def basename_of(name):
    """splat names a subsegment's symbols after the last path component (`fragments/foo/bar` -> `bar`)."""
    return os.path.basename(name) if name else None


def derive_rom_symbols(segs, rom_size=None):
    """{symbol: value} for `<name>_ROM_START`, `<name>_ROM_END` (end = start of the next ROM-resident entry) and, for top-level
       segments, `<name>_TEXT_START` (the virtual address). Unnamed subsegments are named `_<HEXSTART>` as the decomp does."""
    out = {}
    for si, seg in enumerate(segs):
        if seg["start"] is None:
            continue
        nxt_seg = next((s["start"] for s in segs[si + 1:] if s["start"] is not None), rom_size)
        out[seg["name"] + "_ROM_START"] = seg["start"]
        if nxt_seg is not None:
            out[seg["name"] + "_ROM_END"] = nxt_seg
        if seg["vram"] is not None:
            out[seg["name"] + "_TEXT_START"] = seg["vram"]
        subs = [s for s in seg["subs"] if s["type"] not in ("pad",)]
        for i, sub in enumerate(subs):
            nm = basename_of(sub["name"]) or "_%X" % sub["start"]
            end = subs[i + 1]["start"] if i + 1 < len(subs) else nxt_seg
            out.setdefault(nm + "_ROM_START", sub["start"])
            if end is not None:
                out.setdefault(nm + "_ROM_END", end)
    return out


def read_documented(decomp, derived=None):
    """Every `name = value;` definition in linker_scripts/us/*: {name: value} (the decomp's own documentation of addresses).
       The one other shape that occurs, `name = other + 0xN;`, is evaluated when `other` is documented or in `derived`; a line
       whose `other` is unknown is reported by name rather than guessed."""
    defs, exprs = {}, {}
    for f in sorted(glob.glob(os.path.join(decomp, "linker_scripts", "us", "*"))):
        with open(f, errors="replace") as fh:
            for line in fh:
                m = re.match(r"\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;", line)
                if m:
                    defs[m.group(1)] = int(m.group(2), 0)
                    continue
                m = re.match(r"\s*(\w+)\s*=\s*(\w+)\s*\+\s*(0x[0-9A-Fa-f]+|\d+)\s*;", line)
                if m:
                    exprs[m.group(1)] = (m.group(2), int(m.group(3), 0))
    for name, (base, add) in exprs.items():
        if base in defs:
            defs[name] = defs[base] + add
        elif derived and base in derived:
            defs[name] = derived[base] + add
        else:
            raise SystemExit("linker_scripts/us: %s = %s + 0x%X: %s is neither documented nor derivable from rom.yaml" % (name, base, add, base))
    return defs


def check(decomp):
    """Compares derived values with the explicit ones. Returns (agree, disagree_list, derived_only_count)."""
    segs = parse_rom_yaml(os.path.join(decomp, "yamls", "us", "rom.yaml"))
    derived = derive_rom_symbols(segs)
    documented = read_documented(decomp)
    rom_like = {k: v for k, v in documented.items() if re.search(r"_ROM_(START|END)$|_TEXT_START$", k)}
    agree = [k for k, v in rom_like.items() if derived.get(k) == v]
    disagree = [(k, v, derived.get(k)) for k, v in rom_like.items() if k in derived and derived[k] != v]
    absent = [k for k in rom_like if k not in derived]
    return segs, derived, agree, disagree, absent


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    segs, derived, agree, disagree, absent = check(sys.argv[1])
    print("top-level segments: %d, ROM subsegments: %d, derived symbols: %d" % (len(segs), sum(len(s["subs"]) for s in segs), len(derived)))
    print("documented ROM/TEXT symbols reproduced exactly: %d   disagree: %d   not derivable: %d" % (len(agree), len(disagree), len(absent)))
    for k, v, d in disagree[:15]:
        print("  DISAGREE %s documented 0x%X derived %s" % (k, v, hex(d) if d is not None else None))
    for k in absent[:10]:
        print("  not derived:", k)


def read_pool_bytes(decomp):
    """Size of the main memory pool: POOL_END_6MB - POOL_START from include/variables.h (the pool array `gPool` is declared with exactly
       this size in src/heap.c)."""
    text = open(os.path.join(decomp, "include", "variables.h")).read()
    vals = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"#define\s+(POOL_START|POOL_END_6MB)\s+(0x[0-9A-Fa-f]+)", text)}
    if "POOL_START" not in vals or "POOL_END_6MB" not in vals:
        raise SystemExit("include/variables.h no longer defines POOL_START / POOL_END_6MB")
    return vals["POOL_END_6MB"] - vals["POOL_START"]
