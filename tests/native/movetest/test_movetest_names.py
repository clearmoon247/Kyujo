# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Tests tools/native/gen_movetest_names.py on a made-up decomp tree (no game files): the parsing of the headers, the display names, the refusal of a
layout it does not understand. Run: python3 -I tests/native/movetest/test_movetest_names.py"""
import os
import subprocess
import sys
import tempfile
import unittest

GEN = os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools", "native", "gen_movetest_names.py")


def make_tree(species_count=151, move_count=165):
    d = tempfile.mkdtemp()
    os.makedirs(os.path.join(d, "include"))
    with open(os.path.join(d, "include", "pokemon.h"), "w") as f:
        for i in range(1, species_count + 1):
            f.write("#define MON_%d   %d\n" % (i, i))
        f.write("#define SUBSTITUTE_DOLL    152\n#define RGBY_MON_1  0x15\n#define MON_LATE 7\n")   # after RGBY_: must be ignored
    with open(os.path.join(d, "include", "moves.h"), "w") as f:
        for i in range(1, move_count + 1):
            f.write("#define MOVE_%d   0x%02X\n" % (i, i))
        f.write("#define ABC_FIRST   0   // a comment\n#define MOVE_LATE 0x02\n")
    return d


class Names(unittest.TestCase):
    def run_gen(self, tree):
        out = os.path.join(tree, "names.c")
        r = subprocess.run([sys.executable, "-I", GEN, tree, out], capture_output=True, text=True)
        return r, out

    def test_names_and_order(self):
        r, out = self.run_gen(make_tree())
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out) as f:
            text = f.read()
        self.assertIn('gMoveTestSpeciesName[152]', text)
        self.assertIn('gMoveTestMoveName[166]', text)
        self.assertIn('"Mon 1",', text)
        self.assertIn('"Move 165",', text)
        self.assertNotIn("Late", text)          # lines after RGBY_ / ABC_ are not read

    def test_special_names(self):
        tree = make_tree()
        with open(os.path.join(tree, "include", "pokemon.h"), "w") as f:
            for i in range(1, 152):
                f.write("#define %s   %d\n" % ("FARFETCH_D" if i == 83 else "MR_MIME" if i == 122 else "MON_%d" % i, i))
        r, out = self.run_gen(tree)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out) as f:
            text = f.read()
        self.assertIn("\"Farfetch'd\",", text)
        self.assertIn('"Mr. Mime",', text)

    def test_wrong_layout_is_refused(self):
        r, _ = self.run_gen(make_tree(species_count=150))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("expected 151 species", r.stderr)


if __name__ == "__main__":
    unittest.main()
