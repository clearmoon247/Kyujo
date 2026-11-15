#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Writes a port patch (port/patches/<name>.patch) from the difference between the decomp and an edited source tree.

    srctree.py make --decomp ext/decomp --patches port/patches --tree work/tree-edit      # a tree with the existing patches
    ... edit files under work/tree-edit ...
    mkpatch.py --decomp ext/decomp --tree work/tree-edit --name 0007-my-change --why why.txt  path/in/decomp.c [more paths]

The patch file starts with the text of --why (what changed and WHY, in plain words - reviewers read it first), then the diff in the anchored format of
patchapply.py (-p1 paths; the changed lines only, the original's lines named by SHA-256, so no text of the decomp is stored here). Number patches in the order they must apply. If the new edit touches a file an
earlier patch already changed, the diff is against the pristine decomp file and would also contain the earlier change: edit the
earlier patch's tree state out first, or make one combined patch for that file.
Standard library only.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import srctree  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decomp", required=True)
    ap.add_argument("--tree", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--why", required=True, help="text file with the patch description")
    ap.add_argument("--out-dir", default=os.path.join(REPO, "port", "patches"))
    ap.add_argument("files", nargs="+", help="paths relative to the decomp root")
    a = ap.parse_args()
    header = open(a.why).read().rstrip("\n")
    body = srctree.diff(a.decomp, a.tree, a.name, a.files)
    if not body:
        sys.exit("no difference between the decomp and the tree for the given files")
    path = os.path.join(a.out_dir, a.name + ".patch")
    with open(path, "w") as f:
        f.write(header + "\n\n" + body)
    print("wrote " + path)


if __name__ == "__main__":
    main()
