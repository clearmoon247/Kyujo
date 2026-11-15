#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Checks the parts of the build scripts that behave differently when the host's path separator is a backslash (Windows), by pretending:
os.path.relpath is made to return backslashes, which is what it does on Windows.

    python3 -I tests/native/setup/test_windows_paths.py

Found by reading, not by a Windows run: before the fix `_tracked` returned `src\\x\\y.c` on Windows, which defeats `startswith("lib/ultralib/src/voice/")`,
the replaced-file list and the "/" -> "_" object naming; the voice files would have been compiled and the replaced libultra files would have been
defined twice.
"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools", "native"))
import compile_decomp  # noqa: E402


class WindowsPaths(unittest.TestCase):
    def test_listing_uses_forward_slashes_even_when_relpath_does_not(self):
        real, real_sep = os.path.relpath, os.sep
        with tempfile.TemporaryDirectory() as root:
            for rel in ("src/a.c", "src/fragments/f1/b.c", "lib/ultralib/src/voice/v.c", "lib/ultralib/src/os/o.c", "src/x.h"):
                p = os.path.join(root, *rel.split("/"))
                os.makedirs(os.path.dirname(p), exist_ok=True)
                open(p, "w").close()
            os.path.relpath = lambda path, start=None: real(path, start).replace("/", "\\")        # what Windows does ...
            os.sep = "\\"                                                                           # ... and its separator
            try:
                game = compile_decomp.list_c_files(root)
                lib = compile_decomp.list_ultralib_c_files(root)
            finally:
                os.path.relpath, os.sep = real, real_sep
        self.assertEqual(game, ["src/a.c", "src/fragments/f1/b.c"])
        self.assertEqual(lib, ["lib/ultralib/src/os/o.c", "lib/ultralib/src/voice/v.c"])
        self.assertTrue(lib[1].startswith("lib/ultralib/src/voice/"))

    def test_object_names_have_no_separator(self):
        name = compile_decomp.object_path("out", "src/fragments/f1/b.c")
        self.assertEqual(os.path.basename(name), "src_fragments_f1_b.o")


if __name__ == "__main__":
    unittest.main()
