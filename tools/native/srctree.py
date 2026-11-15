#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Makes the patched copy of the decomp sources that the native build compiles. The decomp itself is never modified.

    srctree.py verify   --decomp SOURCE                                    # is SOURCE the pinned decomp? (checks every file's SHA-256)
    srctree.py make     --decomp SOURCE --patches port/patches --tree build/linux32/tree
    srctree.py manifest --decomp CHECKOUT --out port/decomp-manifest.txt   # developers only: pin a new decomp version (needs git)
    srctree.py diff     --decomp SOURCE --tree build/linux32/tree --name my-change FILE [FILE...] > port/patches/my-change.patch

SOURCE is where the decomp's files come from, in any of these forms:
    * a git checkout of the decomp (git is NOT run: only the files named in the manifest are read, so any folder with those files works),
    * a plain folder holding those files (an unpacked download),
    * a .zip of the decomp (a GitHub "Download ZIP" or any zip holding the manifest's files; the single top folder GitHub adds is skipped).

The manifest (port/decomp-manifest.txt) is the exact list of decomp files the build reads (path, size, SHA-256) and the commit they
belong to. Every file is checked against it before anything is built, so the build either uses exactly the pinned decomp or stops
with the list of files that differ. Files with Windows line endings (CRLF, what a git checkout with autocrlf makes) are accepted
when they match after converting back to LF; the build then works from a verified LF copy.

Why patches: a handful of places in the game's C assume an N64 (fixed RAM addresses, memory-mapped hardware, an idle loop that spins
forever). Each such place is changed by a small patch in port/patches/ (only the changed lines are in it; `patchapply.py --explain` shows it as a full diff), so a reviewer sees exactly what differs from the decomp,
why (the patch header explains), and a decomp update that moves the code makes the patch fail to apply instead of silently
misbehaving. The patches are applied by patchapply.py (pure Python: no `patch` program is needed).

`make` copies the manifest's files of the directories the build compiles from, then applies every *.patch in the patch directory in
name order (use a numeric prefix to fix the order). The result is stamped with a hash of (manifest, patch contents); when the stamp is
unchanged the tree is left alone, so file timestamps stay valid for incremental builds.

Standard library only (`manifest` additionally runs git).
"""
import argparse
import difflib
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import patchapply  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_MANIFEST = os.path.join(REPO, "port", "decomp-manifest.txt")

# Directories (relative to the decomp root) copied into the tree: everything the compiler reads. Nothing outside the manifest is copied.
# linker_scripts/us: the documented symbol addresses (symbol_addrs*.txt), which be_memory.py uses to order an overlay's variables.
COPY_DIRS = ["src", "include", "lib/ultralib/include", "lib/ultralib/src", "linker_scripts/us"]
# Files the build reads straight from the decomp (not through the tree), besides COPY_DIRS: the ROM layout (romlayout.py) and the
# Japanese symbol list (gen_platform_symbols.py cross-checks addresses with it).
EXTRA_FILES = ["yamls/us/rom.yaml", "linker_scripts/jp/symbol_addrs.txt"]


class SourceError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------- the manifest

def read_manifest(path=DEFAULT_MANIFEST):
    """-> (commit, [(sha256, size, rel)]). Lines: `commit <hex>`, `source <url>` (see source_url), then `<sha256> <size> <path>`;
       '#' starts a comment."""
    commit, entries = None, []
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    for n, line in enumerate(lines, 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.startswith("commit "):
            commit = line.split()[1]
            continue
        if line.startswith("source "):
            continue
        parts = line.split(" ", 2)
        if len(parts) != 3 or len(parts[0]) != 64:
            raise SourceError("%s:%d: not a manifest line: %r" % (path, n, line[:60]))
        entries.append((parts[0], int(parts[1]), parts[2]))
    if commit is None or not entries:
        raise SourceError("%s: no commit line or no files" % path)
    return commit, entries


def write_manifest(decomp, out):
    """Developers only. Lists the files of a git checkout that the build needs (COPY_DIRS + EXTRA_FILES), with hashes."""
    rels = []
    for d in COPY_DIRS:
        rels += subprocess.run(["git", "-C", decomp, "ls-files", "--", d], capture_output=True, text=True, check=True).stdout.splitlines()
    rels += EXTRA_FILES
    commit = subprocess.run(["git", "-C", decomp, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    url = subprocess.run(["git", "-C", decomp, "remote", "get-url", "origin"], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", decomp, "status", "--porcelain", "--"] + COPY_DIRS + EXTRA_FILES, capture_output=True, text=True, check=True).stdout
    if dirty.strip():
        raise SourceError("the checkout has uncommitted changes in the files the build reads; the manifest would not describe the commit:\n" + dirty)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The decomp files the native build reads: SHA-256, size in bytes, path. Written by `srctree.py manifest`; checked by `srctree.py verify`.\n"
                "# Nothing here is the port's own code: these are the unmodified files of the decomp commit below.\n"
                "commit %s\n" % commit + ("source %s\n" % url if url else ""))
        for rel in sorted(set(rels)):
            data = open(os.path.join(decomp, rel), "rb").read()
            f.write("%s %d %s\n" % (hashlib.sha256(data).hexdigest(), len(data), rel))
    return commit, len(set(rels))


def source_url(manifest_path=DEFAULT_MANIFEST):
    """The `source <url>` line: where the pinned decomp is published (for messages and for the setup's download)."""
    with open(manifest_path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("source "):
                return line.split()[1]
    return None


def manifest_stamp(manifest_path):
    with open(manifest_path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


# ---------------------------------------------------------------------------------------------------------------- the source

def _lf(data):
    return data.replace(b"\r\n", b"\n")


def _zip_prefix(names, entries):
    """The folder inside the zip that holds the decomp's root (GitHub: `<repo>-<sha>/`), found from the manifest's own paths."""
    probe = "yamls/us/rom.yaml"
    hits = [n for n in names if n == probe or n.endswith("/" + probe)]
    if len(hits) != 1:
        raise SourceError("the zip does not look like the decomp: expected exactly one %s, found %d" % (probe, len(hits)))
    return hits[0][:-len(probe)]


def open_source(source, work, manifest_path=DEFAULT_MANIFEST, quiet=False):
    """Returns a folder that holds every manifest file with exactly the manifest's content, laid out like the decomp.
       That is `source` itself when it is a folder whose files all match byte for byte; otherwise a verified copy made in `work`
       (a zip, or a folder whose files match only after CRLF -> LF). Raises SourceError listing what differs."""
    commit, entries = read_manifest(manifest_path)
    source = os.path.abspath(source)
    if os.path.isfile(source):
        if not zipfile.is_zipfile(source):
            raise SourceError("%s is a file but not a zip" % source)
        zf = zipfile.ZipFile(source)
        try:
            import zlib  # noqa: F401
        except ImportError:
            # the Python in the Windows compiler package (llvm-mingw) is built without zlib; Setup.ps1 unpacks the zip itself and passes a folder
            if any(i.compress_type != zipfile.ZIP_STORED for i in zf.infolist()):
                raise SourceError("%s is a compressed zip and this Python has no zlib module: unpack it to a folder and pass the folder" % source)
        prefix = _zip_prefix(zf.namelist(), entries)
        names = set(zf.namelist())
        def fetch(rel):
            return zf.read(prefix + rel) if prefix + rel in names else None
        is_zip = True
    elif os.path.isdir(source):
        def fetch(rel):
            p = os.path.join(source, *rel.split("/"))
            if not os.path.isfile(p):
                return None
            with open(p, "rb") as f:
                return f.read()
        is_zip = False
    else:
        raise SourceError("%s does not exist" % source)
    missing, differ, content, converted = [], [], {}, 0
    for sha, size, rel in entries:
        data = fetch(rel)
        if data is None:
            missing.append(rel)
            continue
        if hashlib.sha256(data).hexdigest() != sha:
            lf = _lf(data)
            if hashlib.sha256(lf).hexdigest() == sha:
                data, converted = lf, converted + 1
            else:
                differ.append(rel)
                continue
        content[rel] = data
    if missing or differ:
        lines = ["%s is not the pinned decomp (commit %s): %d of %d files are missing, %d differ." % (source, commit[:10], len(missing), len(entries), len(differ))]
        lines += ["  missing: " + r for r in missing[:8]] + ["  differs: " + r for r in differ[:8]]
        if len(missing) + len(differ) > 16:
            lines.append("  ... and %d more" % (len(missing) + len(differ) - 16))
        url = source_url(manifest_path)
        lines.append("Use the decomp at commit %s%s." % (commit, " (zip: %s/archive/%s.zip, or a checkout of that commit)" % (url.rstrip("/"), commit) if url else ""))
        raise SourceError("\n".join(lines))
    if not is_zip and not converted:
        if not quiet:
            print("decomp source: %s (%d files verified against the manifest, commit %s)" % (source, len(entries), commit[:10]))
        return source
    mirror = os.path.join(work, "decomp")
    if os.path.isdir(mirror):
        shutil.rmtree(mirror)
    for rel, data in content.items():
        dst = os.path.join(mirror, *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
    if not quiet:
        print("decomp source: %s -> verified copy %s (%d files, commit %s%s)" % (source, mirror, len(entries), commit[:10],
              ", %d files converted from CRLF" % converted if converted else ""))
    return mirror


# ---------------------------------------------------------------------------------------------------------------- the tree

def _read_text(path):
    with open(path) as f:
        return f.read()


def patch_files(patch_dir):
    if not patch_dir or not os.path.isdir(patch_dir):
        return []
    return sorted(os.path.join(patch_dir, f) for f in os.listdir(patch_dir) if f.endswith(".patch"))


def stamp_of(manifest_path, patches):
    h = hashlib.sha256()
    h.update(manifest_stamp(manifest_path).encode())            # which decomp files (their hashes) and which commit
    h.update(" ".join(COPY_DIRS).encode())                      # a tree made with other directories is out of date
    for p in patches:
        h.update(os.path.basename(p).encode())
        with open(p, "rb") as f:
            h.update(f.read())
    return h.hexdigest()


def make(decomp, tree, patch_dir, manifest_path=DEFAULT_MANIFEST):
    """`decomp` must be a folder returned by open_source (verified). Copies the manifest's COPY_DIRS files and applies the patches."""
    commit, entries = read_manifest(manifest_path)
    patches = patch_files(patch_dir)
    stamp = stamp_of(manifest_path, patches)
    stamp_path = os.path.join(tree, ".stamp")
    if os.path.exists(stamp_path) and _read_text(stamp_path).strip() == stamp:
        return False                                           # up to date; leave timestamps alone
    if os.path.isdir(tree):
        shutil.rmtree(tree)
    prefixes = tuple(d + "/" for d in COPY_DIRS)
    for _, _, rel in entries:
        if rel.startswith(prefixes):
            dst = os.path.join(tree, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(os.path.join(decomp, *rel.split("/")), dst)
    for p in patches:
        try:
            with open(p, "rb") as f:
                patchapply.apply_patch(f.read(), tree, 1, os.path.basename(p))
        except patchapply.PatchError as e:
            sys.exit("patch %s does not apply to decomp commit %s:\n%s" % (os.path.basename(p), commit[:10], e))
    with open(stamp_path, "w") as f:
        f.write(stamp + "\n")
    return True


def diff(decomp, tree, name, files):
    """The patch (paths a/<rel> b/<rel>, -p1) that turns the decomp's files into the tree's, for the files given, in the ANCHORED format of
       patchapply.py: the changed lines are written, the original's lines are only named by their SHA-256 (the decomp's authors have published
       no licence, so none of their text goes into this repository)."""
    out = []
    for rel in files:
        a = open(os.path.join(decomp, rel), encoding="utf-8", errors="surrogateescape").read().splitlines(keepends=True)
        b = open(os.path.join(tree, rel), encoding="utf-8", errors="surrogateescape").read().splitlines(keepends=True)
        out.extend(difflib.unified_diff(a, b, "a/" + rel, "b/" + rel))
    unified = "".join(out)
    if not unified:
        return ""
    return patchapply.anchor(unified.encode("utf-8", "surrogateescape"), decomp, 1, name).decode("utf-8", "surrogateescape")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("verify", "make"):
        m = sub.add_parser(name)
        m.add_argument("--decomp", required=True, help="a checkout, a folder or a .zip of the decomp")
        m.add_argument("--manifest", default=DEFAULT_MANIFEST)
        m.add_argument("--work", help="where a verified copy is made when needed (default: next to --tree, or a temp folder)")
        if name == "make":
            m.add_argument("--patches")
            m.add_argument("--tree", required=True)
    g = sub.add_parser("manifest")
    g.add_argument("--decomp", required=True)
    g.add_argument("--out", default=DEFAULT_MANIFEST)
    d = sub.add_parser("diff")
    d.add_argument("--decomp", required=True)
    d.add_argument("--tree", required=True)
    d.add_argument("--name", required=True)
    d.add_argument("files", nargs="+")
    args = ap.parse_args()
    try:
        if args.cmd == "manifest":
            commit, n = write_manifest(args.decomp, args.out)
            print("wrote %s: commit %s, %d files" % (args.out, commit[:10], n))
        elif args.cmd == "verify":
            import tempfile
            with tempfile.TemporaryDirectory() as tmp:
                open_source(args.decomp, args.work or tmp, args.manifest)
        elif args.cmd == "make":
            work = args.work or os.path.dirname(os.path.abspath(args.tree))
            src = open_source(args.decomp, work, args.manifest)
            print("tree rebuilt" if make(src, args.tree, args.patches, args.manifest) else "tree up to date")
        else:
            sys.stdout.write(diff(args.decomp, args.tree, args.name, args.files))
    except SourceError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
