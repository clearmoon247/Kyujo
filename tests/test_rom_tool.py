# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Host tests for tools/rom_tool.py. Uses made-up byte strings only; no ROM content is stored in the repository.

If the environment variable GAME_ROM names a file, one extra test checks it against the known v1.0 hash and round-trips it
through every byte order. Nothing from that file is written anywhere except a temporary directory that is removed afterwards.
"""
import hashlib
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import rom_tool  # noqa: E402


def fake_z64(size=0x2000, fill=0x5A):
    data = bytearray([fill]) * size
    data[0:4] = bytes.fromhex("80371240")
    for i in range(4, 64):
        data[i] = i & 0xFF
    return bytes(data)


def to_v64(z):
    out = bytearray(z)
    out[0::2], out[1::2] = z[1::2], z[0::2]
    return bytes(out)


def to_n64(z):
    out = bytearray(len(z))
    for i in range(4):
        out[i::4] = z[3 - i::4]
    return bytes(out)


class DetectTests(unittest.TestCase):
    def test_orders(self):
        z = fake_z64()
        self.assertEqual(rom_tool.detect_order(z[:4]), "z64")
        self.assertEqual(rom_tool.detect_order(to_v64(z)[:4]), "v64")
        self.assertEqual(rom_tool.detect_order(to_n64(z)[:4]), "n64")
        self.assertIsNone(rom_tool.detect_order(b"\0\0\0\0"))

    def test_conversion_round_trip(self):
        z = fake_z64()
        self.assertEqual(rom_tool.to_z64(to_v64(z), "v64"), z)
        self.assertEqual(rom_tool.to_z64(to_n64(z), "n64"), z)
        self.assertEqual(rom_tool.to_z64(z, "z64"), z)

    def test_bad_lengths(self):
        with self.assertRaises(ValueError):
            rom_tool.to_z64(b"\x01\x02\x03", "v64")
        with self.assertRaises(ValueError):
            rom_tool.to_z64(b"\x01\x02\x03\x04\x05\x06", "n64")


class InspectTests(unittest.TestCase):
    def test_unknown_but_valid_rom(self):
        info = rom_tool.inspect_bytes(fake_z64())
        self.assertIsNone(info.name)
        self.assertFalse(info.usable)
        self.assertEqual(info.md5, hashlib.md5(fake_z64()).hexdigest())

    def test_same_hash_for_every_byte_order(self):
        z = fake_z64()
        self.assertEqual(rom_tool.inspect_bytes(to_v64(z)).md5, rom_tool.inspect_bytes(to_n64(z)).md5)

    def test_rejects_garbage(self):
        for blob in (b"", b"\0" * 100, b"\1" * 0x2000):
            with self.assertRaises(ValueError):
                rom_tool.inspect_bytes(blob)

    def test_known_table_is_consistent(self):
        usable = [n for n, (_, ok) in rom_tool.KNOWN.items() if ok]
        self.assertEqual(usable, ["Pokemon Stadium (USA) v1.0"])
        for md5, _ in rom_tool.KNOWN.values():
            self.assertEqual(len(md5), 32)
            int(md5, 16)

    def test_find_rom_ignores_unknown(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "a.z64"), "wb") as f:
                f.write(fake_z64())
            with open(os.path.join(d, "notes.txt"), "w") as f:
                f.write("x")
            self.assertIsNone(rom_tool.find_rom(d))


@unittest.skipUnless(os.environ.get("GAME_ROM"), "set GAME_ROM to run the real-ROM check")
class RealRomTests(unittest.TestCase):
    def test_v10_all_orders(self):
        path = os.environ["GAME_ROM"]
        info = rom_tool.inspect_file(path)
        self.assertTrue(info.usable, rom_tool.describe(info))
        self.assertEqual(info.md5, "ed1378bc12115f71209a77844965ba50")
        with open(path, "rb") as f:
            z = rom_tool.to_z64(f.read(), info.order)
        with tempfile.TemporaryDirectory() as d:
            for name, blob in (("a.v64", to_v64(z)), ("b.n64", to_n64(z)), ("c.z64", z)):
                p = os.path.join(d, name)
                with open(p, "wb") as f:
                    f.write(blob)
                got = rom_tool.inspect_file(p)
                self.assertTrue(got.usable)
                self.assertEqual(got.md5, info.md5)
            self.assertIsNotNone(rom_tool.find_rom(d))


if __name__ == "__main__":
    unittest.main()
