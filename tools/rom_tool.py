#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Checks and converts Nintendo 64 ROM images. Standard library only.

    rom_tool.py check   <rom>                 say which game and byte order a file is
    rom_tool.py convert <rom> <out.z64>       write the big-endian (.z64) form of <rom>
    rom_tool.py find    <folder>              print the first usable ROM in a folder

A ROM is "usable" when it is a known retail dump that this project can build from. Today that is Pokemon Stadium (USA) v1.0
only; the other dumps the decompilation can build are recognised so the message can say why they are refused.

Byte orders: .z64 is big-endian (first word 0x80371240), .v64 swaps each pair of bytes (0x37804012), .n64 swaps each
word (0x40123780).

The known hashes are the MD5 sums of the big-endian files, as listed in the decompilation's README. This tool never
modifies its input and never writes anything but the output file named on the command line.
"""
import hashlib
import os
import sys

ORDERS = {
    bytes.fromhex("80371240"): "z64",
    bytes.fromhex("37804012"): "v64",
    bytes.fromhex("40123780"): "n64",
}

# name -> (md5 of the .z64 form, usable by this project today)
KNOWN = {
    "Pokemon Stadium (USA) v1.0": ("ed1378bc12115f71209a77844965ba50", True),
    "Pokemon Stadium (USA) Rev 1": ("40e03eda831c01e0a12294287fd240e2", False),
    "Pokemon Stadium (USA) Rev 2": ("6dc6820cef755fc1253d06df45c9bd2a", False),
    "Pocket Monsters Stadium 2 (Japan)": ("2449bb712a64e3363a6cbd56f5adeda5", False),
    "the game (Europe)": ("2859090d78581e0925a3af8045e81e4b", False),
    "the game (Europe) Rev 1": ("31ee2de8e65e30f5934c450dbaa924f0", False),
    "the game (France)": ("0e85a098d0f0e8a7eb572a69612a6873", False),
    "the game (Germany)": ("24be2ccb0dea0755908c02bf67e22fe5", False),
    "the game (Spain)": ("d14a499bc4e324974eae3e42dec58625", False),
    "the game (Italy)": ("a81321759af38beb30a40fdaca2ea22a", False),
}
BY_MD5 = {md5: (name, usable) for name, (md5, usable) in KNOWN.items()}

MIN_SIZE = 0x1000          # smaller than a boot block cannot be a ROM
MAX_SIZE = 64 * 1024 * 1024


def detect_order(head):
    """Byte order from the first four bytes: 'z64', 'v64', 'n64', or None."""
    return ORDERS.get(bytes(head[:4]))


def to_z64(data, order):
    """Returns the big-endian form of `data`. Raises ValueError if the length does not suit the swap."""
    if order == "z64":
        return bytes(data)
    if order == "v64":
        if len(data) % 2:
            raise ValueError("a .v64 image must have an even length")
        out = bytearray(data)
        out[0::2], out[1::2] = data[1::2], data[0::2]
        return bytes(out)
    if order == "n64":
        if len(data) % 4:
            raise ValueError("an .n64 image must be a multiple of 4 bytes")
        out = bytearray(len(data))
        for i in range(4):
            out[i::4] = data[3 - i::4]
        return bytes(out)
    raise ValueError("unknown byte order %r" % (order,))


class RomInfo:
    def __init__(self, order, size, md5, name, usable):
        self.order, self.size, self.md5, self.name, self.usable = order, size, md5, name, usable

    def __repr__(self):
        return "RomInfo(%r, %d, %s, %r, usable=%r)" % (self.order, self.size, self.md5, self.name, self.usable)


def inspect_bytes(data):
    """Identifies a ROM image held in memory. Returns RomInfo, or raises ValueError with a reason fit to show a user."""
    if len(data) < MIN_SIZE:
        raise ValueError("file is too small to be a Nintendo 64 ROM (%d bytes)" % len(data))
    if len(data) > MAX_SIZE:
        raise ValueError("file is larger than any Nintendo 64 ROM (%d bytes)" % len(data))
    order = detect_order(data[:4])
    if order is None:
        raise ValueError("not a Nintendo 64 ROM: unrecognised first word %s" % bytes(data[:4]).hex())
    z64 = to_z64(data, order)
    md5 = hashlib.md5(z64).hexdigest()
    name, usable = BY_MD5.get(md5, (None, False))
    return RomInfo(order, len(data), md5, name, usable)


def inspect_file(path):
    with open(path, "rb") as f:
        return inspect_bytes(f.read(MAX_SIZE + 1))


def describe(info):
    if info.name is None:
        return "unknown ROM (md5 of the .z64 form: %s), %s byte order" % (info.md5, info.order)
    status = "usable" if info.usable else "recognised, but not supported yet"
    return "%s: %s (%s byte order, md5 %s)" % (info.name, status, info.order, info.md5)


def find_rom(folder):
    """First file in `folder` (sorted by name, not recursive) with a ROM extension that is a usable dump, or None."""
    for entry in sorted(os.listdir(folder)):
        if not entry.lower().endswith((".z64", ".n64", ".v64")):
            continue
        path = os.path.join(folder, entry)
        try:
            if inspect_file(path).usable:
                return path
        except (OSError, ValueError):
            continue
    return None


def main(argv):
    if len(argv) >= 3 and argv[1] == "check":
        try:
            info = inspect_file(argv[2])
        except (OSError, ValueError) as err:
            print("error:", err)
            return 2
        print(describe(info))
        return 0 if info.usable else 1
    if len(argv) == 4 and argv[1] == "convert":
        try:
            with open(argv[2], "rb") as f:
                data = f.read(MAX_SIZE + 1)
            info = inspect_bytes(data)
            out = to_z64(data, info.order)
        except (OSError, ValueError) as err:
            print("error:", err)
            return 2
        if os.path.abspath(argv[2]) == os.path.abspath(argv[3]):
            print("error: the output must be a different file from the input")
            return 2
        with open(argv[3], "wb") as f:
            f.write(out)
        print("wrote", argv[3], "-", describe(info))
        return 0
    if len(argv) == 3 and argv[1] == "find":
        try:
            found = find_rom(argv[2])
        except OSError as err:
            print("error:", err)
            return 2
        if found is None:
            print("no usable ROM found in", argv[2])
            return 1
        print(found)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
