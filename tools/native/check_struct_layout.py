#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Checks that a *native* compile of the decomp lays out its structs the way the game's MIPS build does.

Why this matters: the game reads raw ROM data, DMA'd tables and 32-bit pointers through C structs. If a struct is a different
size on the host, or a field sits at a different offset, that data is misread. This script measures the difference instead of
assuming there is none.

    check_struct_layout.py --decomp ext/decomp --target i386-linux-musl [-- extra clang flags]
                           [--reference mips-unknown-linux-gnu] [--show 20] [--show-unchecked 10]

What is compared (two independent comparisons, reported separately)

  A. NATIVE vs REFERENCE (the pass/fail result).  For every annotated struct, `sizeof` and `offsetof` of each top-level field are
     measured with clang for the native target and for the reference target, a 32-bit big-endian MIPS o32 target (the ABI the
     game was built for). Any difference is a real layout difference that the port must fix (for example with -malign-double).
     The reference is clang's MIPS o32 layout rules, which for ordinary structs equal the original IDO compiler's; this is an
     assumption (bit-fields and rare corner cases could differ) and is stated here so a "pass" is not over-read.

  B. DOCUMENTATION vs REFERENCE (information only).  The decomp annotates struct fields with `/* 0xOFF */` and the closing line with
     `// size = 0xNN`. These comments were written by people and are sometimes loose (a size that is the sum of the fields without
     padding, a size marked `??`, an array dimension miscounted). Each disagreement between the comments and the MIPS reference
     is listed so nobody mistakes a documentation slip for a port bug. It does NOT affect the exit status.

How values are measured: the script generates `_Static_assert(EXPR == <impossible value>, "P<n>")` lines. Clang's failure note then
prints the true value ("expression evaluates to '24 == ...'"), which is parsed. The number of values received must equal the number
of expressions asked for; otherwise the file is reported as "unchecked" (never as a pass).

Which structs: `typedef struct [Tag] { ... } Alias; // size = 0xNN`, starting in column 0 (file scope), in the tracked .h/.c
files under src/ and include/. An Alias defined twice in one file (alternative layouts), or a body containing `#if`/`#else` lines, cannot be read
from text and is skipped; such structs are counted and nameable with --show-unchecked. A size
comment containing `?` is treated as uncertain and not used for comparison B. Fields inside nested unions/structs are skipped
(their offsets are relative to the nested block).

Exit status: 0 when comparison A finds no difference and no file is unchecked; 1 otherwise. Ambiguous aliases do not
change the exit status but are always printed as a count.
Standard library only. Needs clang on PATH.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

DEFINES = ["VERSION_US=1", "_MIPS_SZLONG=32", "NDEBUG", "_FINALROM", "N_MICRO", "F3DEX_GBI_2", "BUILD_VERSION=VERSION_I_P",
           "LANGUAGE_C", "_LANGUAGE_C", "MIPSEB", "_MIPS_FPSET=16", "_MIPS_ISA=2", "_ABIO32=1", "_MIPS_SIM=_ABIO32",
           "_MIPS_SZINT=32", "_MIPS_SZPTR=32"]
INCLUDES = ["include", "src", "src/libnaudio", "assets/us", ".", "lib/ultralib/include", "lib/ultralib/include/PR",
            "lib/ultralib/include/ido"]
IMPOSSIBLE = "0xFFFFFFFFFFFFFFF1ULL"        # no real size/offset is this large, so every assertion fails and reports its value

TYPEDEF_OPEN = re.compile(r"^typedef\s+struct\s*(\w*)\s*\{\s*(//.*)?$")      # column 0 only: file-scope definitions
CLOSE = re.compile(r"^\s*\}\s*(\w+)\s*;\s*//\s*size\s*=\s*(0x[0-9A-Fa-f]+)(.*)$")
FIELD = re.compile(r"^\s*/\*\s*(0x[0-9A-Fa-f]+)\s*\*/\s*(.*?)\s*;")
NOTE_VALUE = re.compile(r"expression evaluates to '(\d+) == \d+'")
FAIL_LINE = re.compile(r"static assertion failed.*: P(\d+)\s*$")


def field_name(decl):
    """Last identifier of a declaration such as 'Gfx* gfx', 'u8 data[4]', 'void (*cb)(int)'; None if undeterminable."""
    decl = decl.strip()
    m = re.search(r"\(\s*\*\s*(\w+)\s*\)", decl)         # function pointer member
    if m:
        return m.group(1)
    decl = re.sub(r"\[[^\]]*\]", "", decl)               # drop array extents
    m = re.search(r"(\w+)\s*$", decl)
    return m.group(1) if m else None


def extract_structs(text):
    """Yields (alias, documented_size_or_None, [(documented_offset, field_name), ...], has_preprocessor_lines) per struct."""
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        if TYPEDEF_OPEN.match(lines[i]):
            depth, fields, j, has_cpp = 1, [], i + 1, False
            while j < len(lines) and depth > 0:
                line = lines[j]
                if line.lstrip().startswith("#"):
                    has_cpp = True               # '#if'/'#else' inside the body: fields differ per build, text cannot tell which
                code = re.sub(r"/\*.*?\*/", "", line)       # braces inside comments do not count
                code = re.sub(r"//.*$", "", code)
                if depth == 1:
                    m = FIELD.match(line)
                    # A field line that itself opens a nested block ('union {', 'struct {') is not a plain field.
                    if m and "{" not in code and "}" not in code and ":" not in m.group(2):
                        name = field_name(m.group(2))
                        if name:
                            fields.append((int(m.group(1), 16), name))
                depth += code.count("{") - code.count("}")
                if depth == 0:
                    c = CLOSE.match(line)
                    if c:
                        uncertain = "?" in c.group(3)
                        yield c.group(1), (None if uncertain else int(c.group(2), 16)), fields, has_cpp
                j += 1
            i = j
        else:
            i += 1


def measure(decomp, target, extra_flags, rel, structs, resdir):
    """Returns ([values in expression order], error_text_or_None) for one source file on one target."""
    exprs = []
    for alias, _, fields, _ in structs:
        exprs.append("sizeof(%s)" % alias)
        for _, name in fields:
            exprs.append("__builtin_offsetof(%s, %s)" % (alias, name))
    src = '#include "global.h"\n#include "%s"\n' % os.path.join(decomp, rel)
    src += "".join("_Static_assert(%s == %s, \"P%d\");\n" % (e, IMPOSSIBLE, n) for n, e in enumerate(exprs))
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(src)
        path = f.name
    try:
        cmd = ["clang", "--target=" + target, "-fsyntax-only", "-fno-builtin", "-funsigned-char", "-std=gnu11", "-nostdinc",
               "-isystem", os.path.join(resdir, "include"), "-ferror-limit=0", "-w",
               "-Wno-error=int-conversion", "-Wno-error=incompatible-function-pointer-types",
               "-Wno-error=incompatible-pointer-types", "-Wno-error=return-type",
               "-Wno-error=implicit-function-declaration"] + extra_flags
        cmd += ["-D" + d for d in DEFINES] + ["-I" + os.path.join(decomp, i) for i in INCLUDES]
        cmd += ["-I" + os.path.join(decomp, os.path.dirname(rel)), path]
        r = subprocess.run(cmd, capture_output=True, text=True)
    finally:
        os.unlink(path)
    values = {}
    pending = None
    for line in r.stderr.splitlines():
        m = FAIL_LINE.search(line)
        if m:
            pending = int(m.group(1))
            continue
        m = NOTE_VALUE.search(line)
        if m and pending is not None:
            values[pending] = int(m.group(1))
            pending = None
    if len(values) != len(exprs):
        first = next((l for l in r.stderr.splitlines() if "error:" in l and "static assertion" not in l), "missing values")
        return None, first
    return [values[n] for n in range(len(exprs))], None


UNDEFINED = re.compile(r"(?:undeclared identifier|unknown type name) '(\w+)'")


def check_file(decomp, native, reference, extra_flags, rel, structs, resdir):
    """Measures one file on both targets. A struct whose alias the compiler says does not exist in this build (it sits in an
    `#ifdef VERSION_JP` block, say) is dropped and listed under "not_in_build"; any other compile problem leaves the file unchecked."""
    not_in_build = []
    for _ in range(25):
        if not structs:
            return {"file": rel, "checked": 0, "native_diffs": [], "doc_diffs": [], "not_in_build": not_in_build}
        nat, err1 = measure(decomp, native, extra_flags, rel, structs, resdir)
        ref, err2 = measure(decomp, reference, [], rel, structs, resdir) if nat is not None else (None, None)
        err = err1 or err2
        if nat is not None and ref is not None:
            break
        m = UNDEFINED.search(err or "")
        if not m or m.group(1) not in [st[0] for st in structs]:
            return {"file": rel, "unchecked": err}
        not_in_build.append("%s: %s (not defined in this build)" % (rel, m.group(1)))
        structs = [st for st in structs if st[0] != m.group(1)]
    else:
        return {"file": rel, "unchecked": "too many undefined aliases"}
    native_diffs, doc_diffs, n, k = [], [], 0, 0
    for alias, doc_size, fields, _ in structs:
        items = [("sizeof", doc_size)] + [("." + name, off) for off, name in fields]
        for label, doc in items:
            n += 1
            what = "sizeof(%s)" % alias if label == "sizeof" else alias + label
            if nat[k] != ref[k]:
                native_diffs.append("%s: native %d, MIPS %d" % (what, nat[k], ref[k]))
            if doc is not None and doc != ref[k]:
                doc_diffs.append("%s: comment %d, MIPS %d" % (what, doc, ref[k]))
            k += 1
    return {"file": rel, "checked": n, "native_diffs": native_diffs, "doc_diffs": doc_diffs, "not_in_build": not_in_build}


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:                                         # everything after "--" goes to clang for the native target only
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decomp", required=True)
    ap.add_argument("--target", required=True, help="native clang target triple")
    ap.add_argument("--reference", default="mips-unknown-linux-gnu", help="reference target (MIPS o32), default %(default)s")
    ap.add_argument("--show", type=int, default=10, help="how many native-vs-MIPS differences to print")
    ap.add_argument("--show-docs", type=int, default=0, help="how many comment-vs-MIPS disagreements to print")
    ap.add_argument("--show-unchecked", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = ap.parse_args(argv)

    resdir = subprocess.run(["clang", "--print-resource-dir"], capture_output=True, text=True, check=True).stdout.strip()
    tracked = subprocess.run(["git", "-C", args.decomp, "ls-files", "--", "src", "include"], capture_output=True, text=True,
                             check=True).stdout.splitlines()
    work, ambiguous = [], []
    for rel in tracked:
        if rel.endswith((".h", ".c")):
            with open(os.path.join(args.decomp, rel), encoding="utf-8", errors="replace") as f:
                structs = list(extract_structs(f.read()))
            counts = {}
            for alias, _, _, _ in structs:
                counts[alias] = counts.get(alias, 0) + 1
            for alias, _, _, cpp in structs:
                if cpp and counts[alias] == 1:
                    ambiguous.append("%s: %s (#if inside body)" % (rel, alias))
            for alias in sorted(a for a, n in counts.items() if n > 1):
                ambiguous.append("%s: %s" % (rel, alias))
            structs = [st for st in structs if counts[st[0]] == 1 and not st[3]]
            if structs:
                work.append((rel, structs))
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda w: check_file(args.decomp, args.target, args.reference, extra, w[0], w[1], resdir), work))

    done = [r for r in results if "checked" in r]
    for r in done:
        ambiguous.extend(r["not_in_build"])
    unchecked = [r for r in results if "unchecked" in r]
    nat = [(r["file"], d) for r in done for d in r["native_diffs"]]
    docs = [(r["file"], d) for r in done for d in r["doc_diffs"]]
    print("native %s   reference %s   native flags: %s" % (args.target, args.reference, " ".join(extra) or "(none)"))
    print("  files with annotated structs: %d   checked: %d   unchecked (cannot compile alone): %d"
          % (len(results), len(done), len(unchecked)))
    print("  values compared: %d" % sum(r["checked"] for r in done))
    print("  skipped (defined twice, #if in body, or not in this build): %d" % len(ambiguous))
    for a in ambiguous[:args.show_unchecked]:
        print("   ambiguous %s" % a)
    print("  A. native differs from MIPS o32:      %d" % len(nat))
    print("  B. comments differ from MIPS o32:     %d   (documentation slips; informational)" % len(docs))
    for f, d in nat[:args.show]:
        print("   A  %s  %s" % (f, d))
    for f, d in docs[:args.show_docs]:
        print("   B  %s  %s" % (f, d))
    for r in unchecked[:args.show_unchecked]:
        print("   unchecked %s: %s" % (r["file"], r["unchecked"]))
    return 0 if not nat and not unchecked else 1


if __name__ == "__main__":
    sys.exit(main())
