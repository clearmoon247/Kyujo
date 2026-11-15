#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Tests tools/native/patchapply.py (the pure-Python `patch`) and the decomp-source checks of tools/native/srctree.py.
Everything is made up on the spot (tiny files, a tiny manifest): no decomp and no ROM are needed.

    python3 -I tests/native/srctree/test_srctree.py

What each group shows:
  * patchapply: a patch moves with the code (offset), is refused when the text it expects is not there (no fuzz), is refused when
    it could land in two places, creates / deletes files, understands "\\ No newline at end of file", keeps CRLF files CRLF,
    and writes nothing when any one hunk fails.
  * srctree: a folder, a zip (with GitHub's single top folder) and a CRLF checkout are all accepted when the content matches the
    manifest, and any missing or changed file is reported by name.
(That the REAL 28 patches give the same tree as GNU patch was checked once by diffing the two trees; the real build is the other check.)
"""
import hashlib
import os
import sys
import shutil
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools", "native"))
import patchapply  # noqa: E402
import srctree     # noqa: E402

BASE = "".join("line %d\n" % i for i in range(1, 41)).encode()


def rd(path):
    with open(path, "rb") as f:
        return f.read()


def diff(path, hunks, old="a/", new="b/"):
    return ("--- %s%s\n+++ %s%s\n%s" % (old, path, new, path, hunks)).encode()


class PatchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.t = self.tmp.name
        os.makedirs(os.path.join(self.t, "src"))
        self.put("src/f.c", BASE)

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, rel, data):
        with open(os.path.join(self.t, rel), "wb") as f:
            f.write(data)

    def get(self, rel):
        return rd(os.path.join(self.t, rel))

    def test_simple_and_header_text_ignored(self):
        p = b"Why: free text with a --- dash line and +++ words\n\n" + diff("src/f.c", "@@ -10,3 +10,3 @@\n line 10\n-line 11\n+LINE 11\n line 12\n")
        patchapply.apply_patch(p, self.t)
        self.assertEqual(self.get("src/f.c"), BASE.replace(b"line 11\n", b"LINE 11\n"))

    def test_offset(self):                      # the code moved down by 5 lines; the header still says 10
        self.put("src/f.c", b"new\n" * 5 + BASE)
        patchapply.apply_patch(diff("src/f.c", "@@ -10,3 +10,3 @@\n line 10\n-line 11\n+LINE 11\n line 12\n"), self.t)
        self.assertIn(b"LINE 11\n", self.get("src/f.c"))
        self.assertNotIn(b"line 11\n", self.get("src/f.c"))

    def test_mismatch_refused_and_nothing_written(self):
        p = diff("src/f.c", "@@ -2,1 +2,1 @@\n-line 2\n+X\n") + diff("src/g.c", "@@ -1,1 +1,1 @@\n-nope\n+X\n")
        self.put("src/g.c", b"other\n")
        with self.assertRaises(patchapply.PatchError):
            patchapply.apply_patch(p, self.t)
        self.assertEqual(self.get("src/f.c"), BASE)                  # the first file was not written either

    def test_no_fuzz(self):                    # a context line that differs is a mismatch, not "close enough"
        with self.assertRaises(patchapply.PatchError):
            patchapply.apply_patch(diff("src/f.c", "@@ -10,3 +10,3 @@\n line 10\n-line 11\n+LINE 11\n line 99\n"), self.t)

    def test_ambiguous_refused(self):
        self.put("src/f.c", b"a\nb\nc\n" + b"x\n" * 21 + b"a\nb\nc\n")      # the same 3 lines at lines 1 and 25, both 12 away from line 13
        with self.assertRaises(patchapply.PatchError) as cm:
            patchapply.apply_patch(diff("src/f.c", "@@ -13,3 +13,3 @@\n a\n-b\n+B\n c\n"), self.t)
        self.assertIn("equally near", str(cm.exception))

    def test_create_and_delete(self):
        patchapply.apply_patch(b"--- /dev/null\n+++ b/src/new.c\n@@ -0,0 +1,2 @@\n+one\n+two\n", self.t)
        self.assertEqual(self.get("src/new.c"), b"one\ntwo\n")
        patchapply.apply_patch(b"--- a/src/new.c\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-one\n-two\n", self.t)
        self.assertFalse(os.path.exists(os.path.join(self.t, "src/new.c")))
        with self.assertRaises(patchapply.PatchError):               # creating over an existing file
            patchapply.apply_patch(b"--- /dev/null\n+++ b/src/f.c\n@@ -0,0 +1,1 @@\n+x\n", self.t)

    def test_no_newline_marker(self):
        self.put("src/n.c", b"a\nb")
        patchapply.apply_patch(b"--- a/src/n.c\n+++ b/src/n.c\n@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+c\n\\ No newline at end of file\n", self.t)
        self.assertEqual(self.get("src/n.c"), b"a\nc")

    def test_crlf_file_keeps_crlf(self):
        self.put("src/c.c", b"a\r\nb\r\nc\r\n")
        patchapply.apply_patch(diff("src/c.c", "@@ -1,3 +1,3 @@\n a\r\n-b\r\n+B\r\n c\r\n"), self.t)
        self.assertEqual(self.get("src/c.c"), b"a\r\nB\r\nc\r\n")

    def test_two_hunks_and_two_patches_in_a_row(self):
        h = "@@ -2,1 +2,2 @@\n line 2\n+extra\n@@ -30,1 +31,1 @@\n-line 30\n+LINE 30\n"
        patchapply.apply_patch(diff("src/f.c", h), self.t)
        out = self.get("src/f.c")
        self.assertTrue(out.startswith(b"line 1\nline 2\nextra\nline 3\n"))
        self.assertIn(b"LINE 30\n", out)
        patchapply.apply_patch(diff("src/f.c", "@@ -1,2 +1,2 @@\n-line 1\n+first\n line 2\n"), self.t)       # a second patch on the result
        self.assertTrue(self.get("src/f.c").startswith(b"first\nline 2\n"))

    def test_unsafe_path_refused(self):
        for bad in ("../x.c", "a/../../x.c"):
            with self.assertRaises(patchapply.PatchError):
                patchapply.apply_patch(("--- %s\n+++ %s\n@@ -1,1 +1,1 @@\n-a\n+b\n" % (bad, bad)).encode(), self.t)

    def test_unsupported_features_refused(self):
        with self.assertRaises(patchapply.PatchError):
            patchapply.apply_patch(b"--- a/src/f.c\n+++ b/src/f.c\nGIT binary patch\n", self.t)


class AnchoredTests(unittest.TestCase):
    """The anchored format: the patch holds only the added lines and the SHA-256 of the old lines (none of the original's text)."""
    UNIFIED = diff("src/f.c", "@@ -10,3 +10,3 @@\n line 10\n-line 11\n+LINE 11\n line 12\n@@ -30,1 +31,2 @@\n line 30\n+extra 30\n")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.t = self.tmp.name
        os.makedirs(os.path.join(self.t, "src"))
        with open(os.path.join(self.t, "src", "f.c"), "wb") as f:
            f.write(BASE)

    def tearDown(self):
        self.tmp.cleanup()

    def anchored(self):
        return b"Why text\n\n" + patchapply.anchor(self.UNIFIED, self.t)

    def test_same_result_as_the_unified_patch_and_no_original_text(self):
        a = self.anchored()
        for line in (b"line 10", b"line 11", b"line 12", b"line 30"):
            self.assertNotIn(line + b"\n", a)                              # the original's lines are not in the patch
        self.assertIn(b"+LINE 11\n", a)
        self.assertTrue(a.startswith(b"Why text\n\n--- a/src/f.c\n+++ b/src/f.c\n@@ -10,3 sha256="))
        other = tempfile.mkdtemp()
        try:
            os.makedirs(os.path.join(other, "src"))
            with open(os.path.join(other, "src", "f.c"), "wb") as f:
                f.write(BASE)
            patchapply.apply_patch(self.UNIFIED, other)
            patchapply.apply_patch(a, self.t)
            self.assertEqual(rd(os.path.join(self.t, "src", "f.c")), rd(os.path.join(other, "src", "f.c")))
        finally:
            shutil.rmtree(other)

    def test_moved_code_is_found_by_hash_and_changed_code_is_refused(self):
        a = self.anchored()
        with open(os.path.join(self.t, "src", "f.c"), "wb") as f:
            f.write(b"new\n" * 5 + BASE)                                    # everything moved down 5 lines
        patchapply.apply_patch(a, self.t)
        self.assertIn(b"LINE 11\n", rd(os.path.join(self.t, "src", "f.c")))
        with open(os.path.join(self.t, "src", "f.c"), "wb") as f:
            f.write(BASE.replace(b"line 12\n", b"line 12 changed\n"))        # one of the old lines differs: no fuzz
        with self.assertRaises(patchapply.PatchError):
            patchapply.apply_patch(a, self.t)
        self.assertNotIn(b"LINE 11", rd(os.path.join(self.t, "src", "f.c")))   # and nothing was written

    def test_new_file_and_deleted_file_and_two_patches_in_a_row(self):
        created = patchapply.anchor(b"--- /dev/null\n+++ b/src/n.c\n@@ -0,0 +1,2 @@\n+one\n+two\n", self.t)
        patchapply.apply_patch(created, self.t)
        self.assertEqual(rd(os.path.join(self.t, "src", "n.c")), b"one\ntwo\n")
        deleted = patchapply.anchor(b"--- a/src/n.c\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-one\n-two\n", self.t)
        self.assertNotIn(b"one", deleted)
        patchapply.apply_patch(deleted, self.t)
        self.assertFalse(os.path.exists(os.path.join(self.t, "src", "n.c")))
        first = patchapply.anchor(diff("src/f.c", "@@ -1,2 +1,2 @@\n-line 1\n+first\n line 2\n"), self.t)
        patchapply.apply_patch(first, self.t)
        second = patchapply.anchor(diff("src/f.c", "@@ -1,2 +1,2 @@\n-first\n+FIRST\n line 2\n"), self.t)    # made against the result of the first
        patchapply.apply_patch(second, self.t)
        self.assertTrue(rd(os.path.join(self.t, "src", "f.c")).startswith(b"FIRST\nline 2\n"))

    def test_no_newline_at_end_and_explain(self):
        with open(os.path.join(self.t, "src", "n.c"), "wb") as f:
            f.write(b"a\nb")
        a = patchapply.anchor(b"--- a/src/n.c\n+++ b/src/n.c\n@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+c\n\\ No newline at end of file\n", self.t)
        self.assertIn(b"+c\n\\ No newline at end of file\n", a)
        shown = patchapply.explain(a, self.t)                                 # the full diff, made from the files
        self.assertIn("-b", shown)
        self.assertIn("+c", shown)
        patchapply.apply_patch(a, self.t)
        self.assertEqual(rd(os.path.join(self.t, "src", "n.c")), b"a\nc")

    def test_malformed_anchored_hunks_refused(self):
        bad = b"--- a/src/f.c\n+++ b/src/f.c\n@@ -10,3 sha256=" + b"0" * 64 + b" @@\n=1\n"             # accounts for 1 of 3 old lines
        with self.assertRaises(patchapply.PatchError):
            patchapply.apply_patch(bad, self.t)


class SourceTests(unittest.TestCase):
    FILES = {"src/a.c": b"int a;\nint b;\n", "include/x.h": b"#define X 1\n", "yamls/us/rom.yaml": b"name: x\n", "other/not_listed.txt": b"x"}

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.t = self.tmp.name
        self.manifest = os.path.join(self.t, "manifest.txt")
        with open(self.manifest, "w") as f:
            f.write("# test\ncommit " + "ab" * 20 + "\nsource https://example.invalid/decomp\n")
            for rel in ("include/x.h", "src/a.c", "yamls/us/rom.yaml"):
                f.write("%s %d %s\n" % (hashlib.sha256(self.FILES[rel]).hexdigest(), len(self.FILES[rel]), rel))

    def tearDown(self):
        self.tmp.cleanup()

    def folder(self, name, files, crlf=False):
        root = os.path.join(self.t, name)
        for rel, data in files.items():
            p = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(data.replace(b"\n", b"\r\n") if crlf else data)
        return root

    def zipfile_of(self, name, files, prefix="decomp-abc/"):
        p = os.path.join(self.t, name)
        with zipfile.ZipFile(p, "w") as z:
            for rel, data in files.items():
                z.writestr(prefix + rel, data)
        return p

    def open(self, src):
        return srctree.open_source(src, os.path.join(self.t, "work"), self.manifest, quiet=True)

    def test_folder_used_in_place(self):
        root = self.folder("ok", self.FILES)
        self.assertEqual(self.open(root), root)                       # nothing copied: the folder itself is used

    def test_zip_with_github_prefix(self):
        out = self.open(self.zipfile_of("g.zip", self.FILES))
        self.assertEqual(rd(os.path.join(out, "src", "a.c")), self.FILES["src/a.c"])
        self.assertFalse(os.path.exists(os.path.join(out, "other")))  # only manifest files are taken from a zip
        self.open(self.zipfile_of("n.zip", self.FILES, prefix=""))    # and a zip without the extra top folder

    def test_crlf_checkout_is_converted(self):
        root = self.folder("crlf", self.FILES, crlf=True)
        out = self.open(root)
        self.assertNotEqual(out, root)
        self.assertEqual(rd(os.path.join(out, "src", "a.c")), self.FILES["src/a.c"])

    def test_missing_and_changed_files_named(self):
        files = dict(self.FILES)
        del files["include/x.h"]
        files["src/a.c"] += b"// changed\n"
        with self.assertRaises(srctree.SourceError) as cm:
            self.open(self.folder("bad", files))
        msg = str(cm.exception)
        self.assertIn("missing: include/x.h", msg)
        self.assertIn("differs: src/a.c", msg)
        self.assertIn("example.invalid/decomp/archive/" + "ab" * 20, msg)

    def test_not_a_decomp_zip(self):
        with self.assertRaises(srctree.SourceError):
            self.open(self.zipfile_of("w.zip", {"readme.txt": b"hi"}))

    def test_make_copies_manifest_files_and_applies_patches_and_stamps(self):
        src = self.open(self.folder("ok", self.FILES))
        pdir = os.path.join(self.t, "patches")
        os.makedirs(pdir)
        with open(os.path.join(pdir, "0001-x.patch"), "wb") as f:
            f.write(diff("src/a.c", "@@ -1,2 +1,2 @@\n-int a;\n+int a = 1;\n int b;\n"))
        tree = os.path.join(self.t, "tree")
        self.assertTrue(srctree.make(src, tree, pdir, self.manifest))
        self.assertEqual(rd(os.path.join(tree, "src", "a.c")), b"int a = 1;\nint b;\n")
        self.assertTrue(os.path.exists(os.path.join(tree, "include", "x.h")))
        self.assertFalse(os.path.exists(os.path.join(tree, "yamls")))        # not a compile input
        self.assertFalse(srctree.make(src, tree, pdir, self.manifest))       # unchanged -> left alone
        with open(os.path.join(pdir, "0002-y.patch"), "wb") as f:
            f.write(diff("src/a.c", "@@ -2,1 +2,1 @@\n-int b;\n+int b = 2;\n"))
        self.assertTrue(srctree.make(src, tree, pdir, self.manifest))        # a new patch -> rebuilt
        self.assertEqual(rd(os.path.join(tree, "src", "a.c")), b"int a = 1;\nint b = 2;\n")


class BuildLibPinTests(unittest.TestCase):
    def test_decomp_pin_matches_submodule(self):
        """tools/build-lib.sh downloads the decompilation by commit when there is no git data; it must be the submodule's commit."""
        import re, subprocess
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
        pin = re.search(r"DECOMP_REV=([0-9a-f]{40})", open(os.path.join(root, "tools", "build-lib.sh")).read()).group(1)
        try:
            out = subprocess.run(["git", "-C", root, "ls-tree", "HEAD", "ext/decomp"], capture_output=True, text=True, check=True).stdout
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("not a git checkout")
        self.assertEqual(out.split()[2], pin)


if __name__ == "__main__":
    unittest.main(verbosity=1)
