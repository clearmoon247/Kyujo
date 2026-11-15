#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Applies a unified diff (the format `diff -u` / `git diff` / srctree.py diff write) to a directory tree. Pure Python, so the
build needs neither `patch` nor `git` (a Windows machine has neither by default).

    patchapply.py [-p N] PATCH-FILE TREE
    patchapply.py --explain [-p N] PATCH-FILE TREE      # prints the patch as an ordinary unified diff, made from the TREE it would apply to (nothing is written)
    patchapply.py --anchor [-p N] PATCH-FILE TREE       # prints a unified patch in the anchored format below (nothing is written)

Two hunk formats are understood (one patch file uses one of them):
  * unified:  `@@ -10,3 +10,4 @@` followed by context, `-` and `+` lines (what `diff -u` writes).
  * anchored: `@@ -10,5 sha256=<64 hex> @@` followed by `=K` (keep K lines), `-K` (delete K lines) and `+text` (add this line). It names the
    5 old lines starting at line 10 only by their SHA-256, so the patch file holds ONLY the added lines: none of the original's text.
    Port patches use this format (the decomp's authors have published no licence; see NOTICE.md), `--explain` shows them in full.

Rules (stricter than GNU patch on purpose, so a patch that no longer fits the decomp stops the build instead of landing in the
wrong place):
  * A hunk's context and removed lines must match the file EXACTLY (no fuzz). The hunk may sit at a different line than its header
    says (the decomp changed above it): the nearest exact match to the header position is used, and if two equally near places
    match, the patch is refused as ambiguous.
  * Everything before the first `--- ` line is free text (the patch header that explains why) and is ignored.
  * Supported: modified files, created files (`--- /dev/null`), deleted files (`+++ /dev/null`), `\\ No newline at end of file`.
    Not supported (refused with a message): renames, binary patches, mode changes.
  * Lines are compared as bytes, the line ending is part of the line; new text keeps the endings written in the patch.
  * All files are checked and computed in memory first; nothing is written unless every hunk of the patch applies.

Standard library only.
"""
import difflib
import hashlib
import os
import re
import sys

HUNK = re.compile(rb"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
COUNT = re.compile(rb"^[=-][0-9]+\r?\n?$")
AHUNK = re.compile(rb"^@@ -(\d+),(\d+) sha256=([0-9a-f]{64}) @@")


class PatchError(Exception):
    pass


def _strip(path, strip):
    """`a/src/x.c` with strip=1 -> `src/x.c`. Refuses paths that would leave the tree."""
    parts = path.replace("\\", "/").split("/")
    parts = parts[strip:]
    if not parts or any(p in ("", "..") for p in parts) or os.path.isabs(path):
        raise PatchError("unsafe or too short path in patch: %r" % path)
    return "/".join(parts)


def _parse_anchored_hunk(lines, i, m):
    """The hunk whose header is lines[i]. -> (next index, (old_start, ops, sha256 hex)); ops = [('=', n) | ('-', n) | ('+', line_bytes)]."""
    old_start, old_len, digest = int(m.group(1)), int(m.group(2)), m.group(3).decode()
    i += 1
    ops, counted = [], 0
    while i < len(lines) and (lines[i][:1] in (b"+", b"\\") or COUNT.match(lines[i])):      # a `--- a/file` line is the next file, not a count
        l = lines[i]
        if l[:1] == b"\\":                                  # "\ No newline at end of file" refers to the added line just before
            if not ops or ops[-1][0] != "+":
                raise PatchError("misplaced '\\ No newline' marker")
            ops[-1] = ("+", ops[-1][1].rstrip(b"\r\n"))
        elif l[:1] == b"+":
            ops.append(("+", l[1:]))
        else:
            n = int(l[1:])
            if n <= 0:
                raise PatchError("malformed hunk line: %r" % l[:60])
            counted += n
            ops.append((l[:1].decode(), n))
        i += 1
    if counted != old_len:
        raise PatchError("hunk @@ -%d,%d sha256=.. does not account for exactly its %d old lines" % (old_start, old_len, old_len))
    return i, (old_start, ops, digest)


def parse(data, strip=1):
    """Splits the patch into [(old_path|None, new_path|None, [hunk])]. A unified hunk is (old_start, [(kind, line_bytes)], None) with kind in ' -+',
       an anchored one (old_start, [('=', n) | ('-', n) | ('+', line_bytes)], sha256 of its old lines).
       A line that ends without a newline in the patch (`\\ No newline`) has its trailing newline removed."""
    lines = data.splitlines(keepends=True)
    i, files = 0, []
    while i < len(lines):
        if lines[i].startswith(b"--- ") and i + 1 < len(lines) and lines[i + 1].startswith(b"+++ "):
            old = lines[i][4:].split(b"\t")[0].rstrip(b"\r\n").decode("utf-8", "surrogateescape")
            new = lines[i + 1][4:].split(b"\t")[0].rstrip(b"\r\n").decode("utf-8", "surrogateescape")
            i += 2
            hunks = []
            while i < len(lines) and not (lines[i].startswith(b"--- ") and i + 1 < len(lines) and lines[i + 1].startswith(b"+++ ")):
                am = AHUNK.match(lines[i])
                if am:
                    i, hunk = _parse_anchored_hunk(lines, i, am)
                    hunks.append(hunk)
                    continue
                m = HUNK.match(lines[i])
                if not m:
                    if lines[i].startswith((b"diff ", b"index ", b"old mode", b"new mode", b"rename ", b"similarity ", b"GIT binary patch", b"Binary files")):
                        raise PatchError("unsupported patch feature: %s" % lines[i].decode("utf-8", "replace").strip())
                    i += 1                                       # text between files
                    continue
                old_start, old_len = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
                new_len = int(m.group(4) if m.group(4) is not None else 1)
                i += 1
                body, n_old, n_new = [], 0, 0
                while i < len(lines) and (n_old < old_len or n_new < new_len or lines[i].startswith(b"\\")):
                    l = lines[i]
                    if l.startswith(b"\\"):                      # "\ No newline at end of file" refers to the line just before
                        if not body:
                            raise PatchError("misplaced '\\ No newline' marker")
                        kind, text = body[-1]
                        body[-1] = (kind, text.rstrip(b"\r\n"))
                    elif l[:1] in (b" ", b"-", b"+"):
                        kind = l[:1].decode()
                        body.append((kind, l[1:]))
                        n_old += kind in " -"
                        n_new += kind in " +"
                    elif l in (b"\n", b"\r\n"):                  # an empty context line whose leading space was stripped by an editor
                        body.append((" ", l))
                        n_old += 1
                        n_new += 1
                    else:
                        raise PatchError("malformed hunk line: %r" % l[:60])
                    i += 1
                if (n_old, n_new) != (old_len, new_len):
                    raise PatchError("hunk @@ -%d,%d +.. is truncated" % (old_start, old_len))
                hunks.append((old_start, body, None))
            old_p = None if old == "/dev/null" else _strip(old, strip)
            new_p = None if new == "/dev/null" else _strip(new, strip)
            if old_p is None and new_p is None:
                raise PatchError("a patch for /dev/null -> /dev/null")
            files.append((old_p, new_p, hunks))
        else:
            i += 1
    if not files:
        raise PatchError("no file diff found")
    return files


def _digest(lines):
    return hashlib.sha256(b"".join(lines)).hexdigest()


def apply_hunks(content_lines, hunks, name, hits=None):
    """Returns the new list of lines. Hunks are applied in order, each searched near where it should be, given the shift so far.
       `hits`, if a list, receives the index (0-based) where each hunk was found and the number of old lines it covers."""
    out, pos = [], 0                            # out: result; pos: next unread index of content_lines
    for old_start, body, digest in hunks:
        if digest is None:
            old = [t for k, t in body if k in " -"]
            new = [t for k, t in body if k in " +"]
            matches = lambda c: content_lines[c:c + len(old)] == old
            what = "does not match the file (first old line: %r)" % ((old[0].decode("utf-8", "replace").rstrip() if old else "")[:70],)
        else:
            old_len = sum(n for k, n in body if k in "=-")
            old = [None] * old_len
            matches = lambda c: _digest(content_lines[c:c + old_len]) == digest
            what = "does not match the file (the %d old lines there do not have the SHA-256 the patch names)" % old_len
        # 0-based index where the hunk's first old line should be. A hunk without old lines (no context at all) inserts AFTER line old_start.
        want = old_start if not old else max(old_start - 1, 0)
        hit = None
        for dist in range(0, len(content_lines) + 1):
            cands = [c for c in {want - dist, want + dist} if pos <= c <= len(content_lines) - len(old)]
            ok = sorted(c for c in cands if matches(c))
            if len(ok) > 1:
                raise PatchError("%s: hunk at line %d matches two places equally near (%d and %d); refusing" % (name, old_start, ok[0] + 1, ok[1] + 1))
            if ok:
                hit = ok[0]
                break
            if want - dist < pos and want + dist > len(content_lines) - len(old):
                break
        if hit is None:
            raise PatchError("%s: hunk at line %d %s" % (name, old_start, what))
        out.extend(content_lines[pos:hit])
        if digest is None:
            out.extend(new)
        else:
            cur = hit
            for kind, v in body:
                if kind == "=":
                    out.extend(content_lines[cur:cur + v])
                    cur += v
                elif kind == "-":
                    cur += v
                else:
                    out.append(v)
        pos = hit + len(old)
        if hits is not None:
            hits.append((hit, len(old)))
    out.extend(content_lines[pos:])
    return out


def compute(patch_bytes, tree, strip=1, label="patch", trace=None):
    """The result of the patch as {rel path: new bytes, or None for a deleted file}; nothing is written. `trace`, if given, is called as
       trace(old_path, new_path, old_lines, hunks, hits) for every file part (the anchoring tool uses it)."""
    work = {}
    for old_p, new_p, hunks in parse(patch_bytes, strip):
        src = old_p if old_p is not None else new_p
        if old_p is None:
            if os.path.exists(os.path.join(tree, new_p)) or new_p in work:
                raise PatchError("%s: %s already exists" % (label, new_p))
            before = []
        else:
            cur = work[old_p] if old_p in work else None
            if old_p not in work:
                path = os.path.join(tree, old_p)
                if not os.path.isfile(path):
                    raise PatchError("%s: %s does not exist" % (label, old_p))
                with open(path, "rb") as f:
                    cur = f.read()
            if cur is None:
                raise PatchError("%s: %s was deleted by an earlier part of this patch" % (label, old_p))
            before = cur.splitlines(keepends=True)
        hits = []
        try:
            after = apply_hunks(before, hunks, src, hits)
        except PatchError as e:
            raise PatchError("%s: %s" % (label, e))
        if trace:
            trace(old_p, new_p, before, hunks, hits)
        if new_p is None:
            if b"".join(after):
                raise PatchError("%s: %s is not empty after the deleting patch" % (label, old_p))
            work[old_p] = None
        else:
            work[new_p] = b"".join(after)
            if old_p is not None and old_p != new_p:
                raise PatchError("%s: renames are not supported" % label)
    return work


def apply_patch(patch_bytes, tree, strip=1, label="patch"):
    """Applies the patch to `tree`. Returns the list of files changed. Raises PatchError, writing nothing, if any hunk fails."""
    work = compute(patch_bytes, tree, strip, label)
    for rel, data in work.items():
        path = os.path.join(tree, rel)
        if data is None:
            os.remove(path)
        else:
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
    return sorted(work)


def anchor(patch_bytes, tree, strip=1, label="patch"):
    """The same patch in the anchored format (no original text in it), made against `tree`, the files as they are just BEFORE the patch.
       The free text before the first `--- ` line is kept. The hunks are put where they really apply in `tree`."""
    lines = patch_bytes.splitlines(keepends=True)
    first = next((n for n, l in enumerate(lines) if l.startswith(b"--- ") and n + 1 < len(lines) and lines[n + 1].startswith(b"+++ ")), None)
    if first is None:
        raise PatchError("no file diff found")
    out = [b"".join(lines[:first])]

    def trace(old_p, new_p, before, hunks, hits):
        out.append(b"--- " + (b"/dev/null" if old_p is None else b"a/" + old_p.encode("utf-8", "surrogateescape")) + b"\n")
        out.append(b"+++ " + (b"/dev/null" if new_p is None else b"b/" + new_p.encode("utf-8", "surrogateescape")) + b"\n")
        for (old_start, body, digest), (hit, n) in zip(hunks, hits):
            if digest is not None:
                raise PatchError("%s: already an anchored patch" % label)
            start = hit if n else old_start                 # a hunk without old lines: "insert after line old_start"
            out.append(b"@@ -%d,%d sha256=%s @@\n" % (start + 1 if n else start, n, _digest(before[hit:hit + n]).encode()))
            run = None
            for kind, t in body:
                if kind == "+":
                    run = None
                    out.append(b"+" + t + (b"" if t.endswith(b"\n") else b"\n\\ No newline at end of file\n"))
                else:
                    sym = "=" if kind == " " else "-"
                    if run and run[0] == sym:
                        run[1] += 1
                        out[run[2]] = b"%s%d\n" % (sym.encode(), run[1])
                    else:
                        out.append(b"%s1\n" % sym.encode())
                        run = [sym, 1, len(out) - 1]
    compute(patch_bytes, tree, strip, label, trace)
    return b"".join(out)


def explain(patch_bytes, tree, strip=1, label="patch"):
    """The patch as an ordinary unified diff (made from `tree`, the files before the patch), for reading. Nothing is written."""
    out = []
    for rel, new in sorted(compute(patch_bytes, tree, strip, label).items()):
        path = os.path.join(tree, rel)
        old = []
        if os.path.isfile(path):
            with open(path, "rb") as f:
                old = f.read().splitlines(keepends=True)
        new = [] if new is None else new.splitlines(keepends=True)
        out.extend(difflib.unified_diff([l.decode("utf-8", "replace") for l in old], [l.decode("utf-8", "replace") for l in new], "a/" + rel, "b/" + rel))
    return "".join(out)


def main():
    args = sys.argv[1:]
    strip, show, anch = 1, False, False
    if args[:1] == ["--explain"]:
        show, args = True, args[1:]
    elif args[:1] == ["--anchor"]:
        anch, args = True, args[1:]
    if args[:1] == ["-p"] and len(args) > 1:
        strip, args = int(args[1]), args[2:]
    if len(args) != 2:
        sys.exit(__doc__)
    try:
        if show:
            sys.stdout.write(explain(open(args[0], "rb").read(), args[1], strip, os.path.basename(args[0])))
            return
        if anch:
            sys.stdout.buffer.write(anchor(open(args[0], "rb").read(), args[1], strip, os.path.basename(args[0])))
            return
        files = apply_patch(open(args[0], "rb").read(), args[1], strip, os.path.basename(args[0]))
    except PatchError as e:
        sys.exit("patch failed: %s" % e)
    print("patched: " + " ".join(files))


if __name__ == "__main__":
    main()
