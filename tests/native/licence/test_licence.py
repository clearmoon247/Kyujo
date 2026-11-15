#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Checks that the licence is applied to every file the repository tracks.

  * LICENSE is the unmodified GNU GPL version 3 text (SHA-256 of the FSF's gpl-3.0.txt).
  * Every tracked file is either tagged `SPDX-License-Identifier: GPL-3.0-only` in its first 400 bytes, or matched by an annotation in
    REUSE.toml that says GPL-3.0-only, or is on the short list of files that are not ours (LICENSE; the submodule pointer).
  * No file is tagged with a different or an "or later" identifier by mistake.

Run: python3 tests/native/licence/test_licence.py          (needs Python 3.11+ for tomllib, and git)
"""
import hashlib
import os
import subprocess
import sys
import unittest

try:
    import tomllib
except ImportError:                                    # Python < 3.11 (the MinGW Python in the Windows kit is 3.11, so this is a dev-box case)
    tomllib = None

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
GPL3_SHA256 = "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"       # the FSF's gpl-3.0.txt, 35149 bytes
NOT_OURS = {"LICENSE", "ext/decomp"}              # the licence text itself; the submodule pointer (the decomp is a separate project)
TAG = b"SPDX-License-Identifier: "


def tracked():
    out = subprocess.run(["git", "-C", REPO, "ls-files", "-z"], capture_output=True, check=True).stdout
    return [p for p in out.decode().split("\0") if p]


def glob_to_re(pattern):
    """REUSE.toml globs: `*` = anything except `/`, `**` = anything including `/`. fnmatch's `*` also crosses `/`, so translate by hand."""
    import re
    out, i = [], 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("^" + "".join(out) + "$")


class LicenceTests(unittest.TestCase):
    def test_license_file_is_the_gpl3_text(self):
        with open(os.path.join(REPO, "LICENSE"), "rb") as f:
            self.assertEqual(hashlib.sha256(f.read()).hexdigest(), GPL3_SHA256)

    @unittest.skipIf(tomllib is None, "needs Python 3.11 (tomllib)")
    def test_every_file_is_tagged_or_annotated(self):
        with open(os.path.join(REPO, "REUSE.toml"), "rb") as f:
            conf = tomllib.load(f)
        patterns = []
        for ann in conf["annotations"]:
            self.assertEqual(ann["SPDX-License-Identifier"], "GPL-3.0-only")
            patterns += [glob_to_re(p) for p in ann["path"]]
        untagged, wrong = [], []
        for rel in tracked():
            if rel in NOT_OURS:
                continue
            with open(os.path.join(REPO, rel), "rb") as f:
                head = f.read(400)
            i = head.find(TAG)
            if i >= 0:
                ident = head[i + len(TAG):].split(b"\n")[0].replace(b"*/", b"").replace(b"-->", b"").strip().decode()      # C, HTML and XML comment closers
                if ident != "GPL-3.0-only":
                    wrong.append((rel, ident))
            elif not any(p.match(rel) for p in patterns):
                untagged.append(rel)
        self.assertEqual(wrong, [], "files tagged with another identifier")
        self.assertEqual(untagged, [], "files with neither an SPDX tag nor a REUSE.toml annotation")

    def test_no_annotation_pattern_is_dead(self):
        """A pattern that matches nothing is a typo that would silently leave files uncovered."""
        if tomllib is None:
            self.skipTest("needs Python 3.11 (tomllib)")
        with open(os.path.join(REPO, "REUSE.toml"), "rb") as f:
            conf = tomllib.load(f)
        files = tracked()
        for ann in conf["annotations"]:
            for pat in ann["path"]:
                rx = glob_to_re(pat)
                self.assertTrue(any(rx.match(p) for p in files), "REUSE.toml pattern matches no tracked file: " + pat)


if __name__ == "__main__":
    unittest.main()
