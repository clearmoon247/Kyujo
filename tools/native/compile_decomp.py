#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Compiles every C file of the Team Hippodrome decompilation as a *native* (little-endian, 32-bit pointer) object file.

This is an experiment harness, not the final build: it answers "does the decomp's C compile for this target, and what does it
still need from the outside?" so the answer can be repeated after every change. It never modifies the decomp checkout.

    compile_decomp.py --decomp ext/decomp --target i386-linux-musl --out build/native-i386 [--opt O0] [--jobs 2]
    compile_decomp.py --decomp ext/decomp --target i686-w64-windows-gnu --out build/native-win32
    compile_decomp.py --decomp ext/decomp --profile linux32 --out build/linux32     (triple + layout flags from targets.py)

Output: <out>/obj/<source path with '/' replaced by '_'>.o and <out>/compile_report.json
(per-file status, number of warnings, first error line).

Which files: the decomp's own Makefile compiles `git ls-files -- src | *.c` (C_FILES); this script does the same, so files that
exist on disk but are not tracked (analysis copies in a developer checkout) are not built. The decomp's hand-written assembly
(`*.s`, 10 segments) is not compiled here: it needs C replacements (see docs/native-port-feasibility.md).

Compiler flags, and why each is there (the decomp's own host syntax check, Makefile rule CC_CHECK, is the reference):
  -fno-builtin         the decomp defines its own memcpy/strlen/etc. (src/hal_libc.c); clang must not substitute its own.
  -funsigned-char      IDO's MIPS `char` is unsigned; the same choice keeps comparisons and table lookups identical.
  -std=gnu89           the sources are C89 with GNU extensions (declarations after statements are not used, K&R-style
                       prototypes are).
  -nostdinc            the decomp ships its own <string.h>/<stdarg.h>/<stdlib.h>; host headers must not leak in.
  -fno-common          each global is defined exactly once (no tentative-definition merging).
  -fno-strict-aliasing, -fwrapv
                       the original was compiled by IDO, which has no type-based alias analysis and wraps signed overflow;
                       clang at -O1 and above would otherwise be free to exploit both.
  -Wno-error=...       clang 18 turns pointer/integer mixing and incompatible function pointers into hard errors; the decomp's
                       Makefile disables exactly these (-Wno-int-conversion, -Wno-incompatible-pointer-types,
                       -Wno-incompatible-function-pointer-types, -Wno-return-mismatch) for its host check. They are kept as
                       warnings here and counted, because they mark places that are only correct when pointers are 32 bits wide.
  Defines              copied from the decomp Makefile (VERSION_US, F3DEX_GBI_2, _FINALROM, N_MICRO, NDEBUG, LANGUAGE_C and the
                       MIPS ABI macros that its headers test). `_MIPS_SZPTR=32`/`_MIPS_SZLONG=32` are *true* for a 32-bit
                       native build; they are not true for a 64-bit build (see docs).

Standard library only. Needs clang on PATH.
"""
import argparse
import glob
import hashlib
import json
import re
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# `python3 -I` does not add the script's folder to the import path; do it explicitly, for this repository's own modules only.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import proc  # noqa: E402
import targets  # noqa: E402
import be_memory  # noqa: E402
import ir_sizes  # noqa: E402

# Defines taken from the decomp Makefile (BUILD_DEFINES, COMMON_DEFINES, RELEASE_DEFINES, GBI_DEFINES, C_DEFINES, MIPS_BUILTIN_DEFS).
DEFINES = [
    "VERSION_US=1",
    "_MIPS_SZLONG=32", "NDEBUG", "_FINALROM", "N_MICRO", "F3DEX_GBI_2",
    "BUILD_VERSION=VERSION_I_P",          # LIBULTRA_DEFINES: ULTRALIB_VERSION is I_P in the Makefile
    "LANGUAGE_C", "_LANGUAGE_C",
    "MIPSEB", "_MIPS_FPSET=16", "_MIPS_ISA=2", "_ABIO32=1", "_MIPS_SIM=_ABIO32", "_MIPS_SZINT=32", "_MIPS_SZPTR=32",
]
# Include directories from the decomp Makefile (IINC). "-I <dir of the file>" is added per file, as the Makefile does.
INCLUDES = ["include", "src", "src/libnaudio", "assets/us", ".", "lib/ultralib/include", "lib/ultralib/include/PR",
            "lib/ultralib/include/ido"]
# lib/ultralib is built by its own Makefile (target libultra_rom, version I_P): CPPFLAGS and IINC from Makefile and Makefile.ido,
# plus the IDO compiler's predefined macros (LANGUAGE_C, MIPSEB, ...), which clang must be told explicitly.
ULTRALIB_DEFINES = ["_MIPS_SZLONG=32", "F3DEX_GBI", "BUILD_VERSION=7", 'BUILD_VERSION_STRING="2.0I_P"', "NDEBUG", "_FINALROM",
                    "LANGUAGE_C", "_LANGUAGE_C", "MIPSEB", "_MIPS_FPSET=16", "_MIPS_ISA=2", "_ABIO32=1", "_MIPS_SIM=_ABIO32",
                    "_MIPS_SZINT=32", "_MIPS_SZPTR=32"]
ULTRALIB_INCLUDES = ["lib/ultralib/include", "lib/ultralib/include/ido", "lib/ultralib/include/PR"]
WARNING_DOWNGRADES = ["int-conversion", "incompatible-function-pointer-types", "incompatible-pointer-types", "return-type"]


def _tracked(root, prefix):
    """Files under `prefix` of `root`: the git-tracked ones when root is a git checkout (the decomp), otherwise every file
       (root is then a patched copy made by srctree.py, which contains only tracked files anyway)."""
    if os.path.exists(os.path.join(root, ".git")):
        out = subprocess.run(["git", "-C", root, "ls-files", "--", prefix], capture_output=True, text=True, check=True).stdout
        return out.splitlines()
    found = []
    for d, _, names in os.walk(os.path.join(root, prefix)):
        # always forward slashes, whatever the host: the rest of the build compares these names with strings such as "lib/ultralib/src/voice/"
        # and turns "/" into "_" for object file names, which a Windows backslash would silently defeat
        found += [os.path.relpath(os.path.join(d, n), root).replace(os.sep, "/") for n in names]
    return found


def list_ultralib_c_files(decomp):
    """Every C file of the bundled libultra source (lib/ultralib/src)."""
    return sorted(f for f in _tracked(decomp, "lib/ultralib/src") if f.endswith(".c"))


def list_c_files(decomp):
    """The decomp's own definition of "the C files of the build": tracked files under src/ ending in .c."""
    return sorted(f for f in _tracked(decomp, "src") if f.endswith(".c"))


REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def known_bitfield_files():
    """port/bitfield_known.txt: source files (path relative to the decomp tree) that still contain C bit-fields, which the
       big-endian rewrite cannot translate correctly. Listed files compile with a warning; every other file is refused if it
       uses a bit-field. One path per line, '#' starts a comment."""
    path = os.path.join(REPO, "port", "bitfield_known.txt")
    names = set()
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#", 1)[0].strip()
            if line:
                names.add(line)
    return names


_BE_SIGNATURE = hashlib.sha256(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "be_memory.py"), "rb").read()).hexdigest()


_SYMBOL_ADDRESSES = {}


def symbol_addresses(decomp):
    """{name: console address} of the symbols the decomp documents for the US version (linker_scripts/us/symbol_addrs*.txt). be_memory uses
    it to put an overlay's variables in the console's order (be_memory.order_fragment_globals)."""
    if decomp not in _SYMBOL_ADDRESSES:
        found = {}
        for path in sorted(glob.glob(os.path.join(decomp, "linker_scripts", "us", "symbol_addrs*.txt"))):
            for line in open(path):
                m = re.match(r"^(\w+)\s*=\s*(0x[0-9A-Fa-f]+);", line.strip())
                if m:
                    found.setdefault(m.group(1), int(m.group(2), 16))
        _SYMBOL_ADDRESSES[decomp] = found
    return _SYMBOL_ADDRESSES[decomp]


_FRAGMENT_DIRS = {}


def fragment_dirs(decomp):
    """The code overlays ("fragments") of the decomp: the sorted names of the directories under src/fragments. A file's number below is
    its directory's position in this list; gen_platform_symbols.py uses the same list to build the table that host_fragment.c reads,
    so both sides must (and do) see the same decomp tree."""
    if decomp not in _FRAGMENT_DIRS:
        root = os.path.join(decomp, "src", "fragments")
        _FRAGMENT_DIRS[decomp] = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d))) if os.path.isdir(root) else []
    return _FRAGMENT_DIRS[decomp]


def fragment_index_of(decomp, rel):
    """The overlay number of the source file `rel` (relative to the decomp), or None if it is not in a fragment directory."""
    parts = rel.replace("\\", "/").split("/")
    if len(parts) >= 4 and parts[0] == "src" and parts[1] == "fragments":
        return fragment_dirs(decomp).index(parts[2])
    return None


def object_path(out_obj_dir, rel):
    """Where compile_one puts the object of `rel` (a path relative to the decomp, or an absolute path of this repository's own file)."""
    return os.path.join(out_obj_dir, (os.path.basename(rel) if os.path.isabs(rel) else rel.replace("/", "_"))[:-2] + ".o")


def alias_ir(aliases):
    """LLVM IR text defining the extra symbols of compile_one(aliases=...).

    Why: the decomp's linker scripts define a few symbols as addresses INSIDE a C variable that the game's headers know under another
    name (for example `D_800733BF = D_80073230 + 0x18F`, or the 1-based table base `D_8006FEE8 = D_8006FF00 - 0x18`). The console's
    linker placed them there; this port's linker does not know about them, and pointing them at a raw copy of the ROM's data would give
    code a table whose pointers are console addresses. An IR alias puts the symbol at the right place inside the real variable. It is
    done in IR because it is the one form that both the ELF and the Windows (COFF) toolchains of this project accept: lld's MinGW driver
    has neither --defsym nor linker scripts, and an assembler alias needs both symbols in one file. (Checked with a test build for both
    targets: the alias lands at variable + offset.)"""
    out = []
    for name, target, offset in aliases:
        out.append("@%s = dso_local alias i8, getelementptr (i8, ptr @%s, i32 %d)\n" % (name, target, offset))
    return ("\n; ---- aliases into variables of this file (see tools/native/compile_decomp.py, alias_ir)\n" + "".join(out)) if out else ""


def absolute_ir(text, absmap, gepmap=None):
    """The IR `text` with every use of a symbol of `absmap` ({name: value}) replaced by the constant address, and the declarations of those symbols removed.

    Why: the console's linker defined `D_1002480`, `fragment1_ROM_START` ... as plain numbers. ELF and COFF linkers can be given such absolute symbols
    (tools/native/gen_platform_symbols.py writes them into an assembler file); wasm-ld cannot, so on WebAssembly the number is put where the symbol was.
    `@name` becomes `inttoptr (i32 value to ptr)`, which LLVM folds into the instruction that uses it (also inside a constant initialiser).
    gepmap ({name: [variable, offset]}): symbols that name a place BEFORE a variable (`D_800780B3` = one byte before `D_800780B4`); WebAssembly cannot
    have such a symbol, so each use becomes `getelementptr (i8, ptr @variable, i32 offset)`, and the variable is declared if the file does not have it."""
    gepmap = gepmap or {}
    names = sorted(set(absmap) | set(gepmap), key=len, reverse=True)
    if not names:
        return text
    text = re.compile(r"^@(%s) = external[^\n]*\n" % "|".join(re.escape(n) for n in names), re.M).sub("", text)
    use = re.compile(r"@(%s)(?![\w.$])" % "|".join(re.escape(n) for n in names))
    needed = set()
    def repl(m):
        n = m.group(1)
        if n in absmap:
            return "inttoptr (i32 %d to ptr)" % absmap[n]
        target, off = gepmap[n]
        needed.add(target)
        return "getelementptr (i8, ptr @%s, i32 %d)" % (target, off)
    text = use.sub(repl, text)
    extra = "".join("@%s = external global i8\n" % t for t in sorted(needed) if not re.search(r"^@%s = " % re.escape(t), text, re.M))
    return text + ("\n; ---- variables that gepmap symbols are relative to\n" + extra if extra else "")


def recompile_ir(obj, absmap, target, opt, extra_flags, out_obj, gepmap=None):
    """Second object from `<obj without .o>.be.ll` (kept by compile_one(keep_ir=True)) with the absolute symbols of absmap substituted. Returns (ok, message)."""
    base = obj[:-2]
    text = absolute_ir(open(base + ".be.ll").read(), absmap, gepmap)
    tmp = out_obj[:-2] + ".abs.ll"
    with open(tmp, "w") as f:
        f.write(text)
    r = proc.run(["clang", "--target=" + target, "-" + opt, "-c", "-Wno-unused-command-line-argument"] + list(extra_flags) + [tmp, "-o", out_obj],
                 capture_output=True, text=True)
    os.remove(tmp)
    return r.returncode == 0, r.stderr.strip()


def compile_one(decomp, target, opt, out_obj_dir, rel, extra_flags=(), lib=False, force=False, be=True, aliases=(), keep_ir=False, backend_opt=None):
    """Compiles one source file of the decomp (rel = path relative to the decomp root) to an object file.
       be=True (the default, and the right choice for every GAME-side file): the file is compiled through tools/native/be_memory.py
       so that its memory is big-endian like the console's (see that file). Host-side files do not use this function.
       Results are cached next to the object: a file is recompiled only if its source is newer than the object or the
       compiler command line (or, for be=True, the rewrite tool) changed. Headers are not tracked, so `force=True` skips the
       cache (used for this repository's own files, whose headers change).
       aliases (be=True only): [(name, target, offset)] - extra global symbols `name` placed at `target` + `offset` bytes, where `target`
       is a variable defined in this file. See alias_ir() for why they exist.
       backend_opt (be=True only): optimisation level of the code generator that runs on the rewritten IR (default: `opt`). The front end stays at `opt`
       (the rewrite is written for the IR of unoptimised code); the WebAssembly build uses O0 there and O2 here.
       keep_ir (be=True only): keep the rewritten IR (<object>.be.ll) so that absolute_ir()/recompile_ir() can build a second object from it
       without the front end (the WebAssembly build, which cannot define absolute symbols in the linker)."""
    # `rel` is relative to `decomp`; an absolute path (a hand-written file of this repository) is also accepted.
    obj = object_path(out_obj_dir, rel)
    cmd = ["clang", "--target=" + target, "-" + opt, "-fno-builtin", "-funsigned-char", "-std=gnu89", "-nostdinc",
           "-fno-common", "-fno-strict-aliasing", "-fwrapv"] + list(extra_flags)
    cmd += ["-Wno-error=" + w for w in WARNING_DOWNGRADES]
    cmd += ["-D" + d for d in (ULTRALIB_DEFINES if lib else DEFINES)]
    cmd += ["-I" + os.path.join(decomp, i) for i in (ULTRALIB_INCLUDES if lib else INCLUDES)]
    src = os.path.join(decomp, rel)
    cmd += ["-I" + os.path.join(decomp, os.path.dirname(rel)), src]
    sig = hashlib.sha256((" ".join(cmd) + (_BE_SIGNATURE if be else "") + repr(sorted(aliases)) + "|bo=" + str(backend_opt) + "|sizes-v1|align8|fragsec2|" + repr(fragment_index_of(decomp, rel) if not os.path.isabs(rel) else None)).encode()).hexdigest()
    cache = obj + ".cc.json"
    if not force and os.path.exists(obj) and os.path.exists(cache) and os.path.exists(obj + ".sizes.json") and os.path.getmtime(obj) >= os.path.getmtime(src) \
            and (not keep_ir or os.path.exists(obj[:-2] + ".be.ll")):
        try:
            cached = json.load(open(cache))
            if cached["sig"] == sig:
                return cached["result"]
        except (ValueError, KeyError):
            pass
    bitfield_functions = []
    sizes = {}
    if not be:
        r = proc.run(cmd + ["-c", "-o", obj], capture_output=True, text=True)
        stderr = r.stderr
        ok = r.returncode == 0
        be_error = None
        if ok and "D_8" in open(src, errors="replace").read():      # only files that define console-address variables need their sizes
            r3 = proc.run(cmd + ["-S", "-emit-llvm", "-o", obj[:-2] + ".ll"], capture_output=True, text=True)
            if r3.returncode == 0:
                try:
                    sizes = ir_sizes.ir_global_sizes(open(obj[:-2] + ".ll").read())
                except ir_sizes.IrError as e:
                    ok, be_error = False, "cannot compute variable sizes: %s" % e
            if os.path.exists(obj[:-2] + ".ll"):
                os.remove(obj[:-2] + ".ll")
    else:
        # Front end only: UNOPTIMISED IR with value names kept (the rewrite relies on clang's own names, e.g. `argp.cur`).
        base = obj[:-2]
        r = proc.run(cmd + ["-fno-discard-value-names", "-Xclang", "-disable-llvm-passes", "-S", "-emit-llvm", "-o", base + ".ll"],
                           capture_output=True, text=True)
        stderr = r.stderr
        ok = r.returncode == 0
        be_error = None
        if ok:
            try:
                ir_text = open(base + ".ll").read()
                sizes = ir_sizes.ir_global_sizes(ir_text)           # before the rewrite; see ir_sizes.py for why (COFF has no symbol sizes)
            except ir_sizes.IrError as e:
                ok, be_error = False, "cannot compute variable sizes: %s" % e
        if ok:
            try:
                allow = known_bitfield_files()
                # Console addresses for ordering an overlay's variables: the documented symbols, plus the variables that an alias places
                # (`D_8120EA80 = sGbApuRegs + 0` says sGbApuRegs is at 0x8120EA80).
                known_addresses = dict(symbol_addresses(decomp))
                for alias_name, alias_target, alias_offset in aliases:
                    am = re.fullmatch(r"D_([0-9A-Fa-f]{8})", alias_name)
                    if am:
                        known_addresses.setdefault(alias_target, int(am.group(1), 16) - alias_offset)
                new_text, stats = be_memory.transform(ir_text, rel if not os.path.isabs(rel) else os.path.basename(rel), allow,
                                                          preempt=not os.path.isabs(rel),   # scheduling points: decomp sources only, not this port's hardware model
                                                          fragment_index=None if os.path.isabs(rel) else fragment_index_of(decomp, rel),
                                                          symbol_addresses=known_addresses)
                bitfield_functions = sorted(set(stats["bitfield"]))
                for name, _, _ in aliases:
                    # This file may itself declare the alias as an external variable (`extern u8 D_800733BF[];`); the definition replaces it.
                    new_text = re.sub(r"^@%s = external[^\n]*\n" % re.escape(name), "", new_text, flags=re.M)
                new_text, _ = ir_sizes.raise_alignment(new_text)        # 8-byte alignment for every object of 8 bytes or more (see there)
                with open(base + ".be.ll", "w") as f:
                    f.write(new_text + alias_ir(aliases))
            except be_memory.BeError as e:
                ok, be_error = False, str(e)
        if ok:
            # Back end: the usual optimiser and code generator on the rewritten IR.
            r2 = proc.run(["clang", "--target=" + target, "-" + (backend_opt or opt), "-c", "-Wno-unused-command-line-argument"] + list(extra_flags) +
                                [base + ".be.ll", "-o", obj], capture_output=True, text=True)
            ok = r2.returncode == 0
            if not ok:
                be_error = "code generation of the rewritten IR failed: " + (r2.stderr.strip().splitlines() or ["?"])[0]
                stderr += r2.stderr
        for tmp in (base + ".ll", base + ".be.ll"):          # intermediate files are large; the object is what matters
            if os.path.exists(tmp) and ok and not (keep_ir and tmp.endswith(".be.ll")):
                os.remove(tmp)
    # Sidecar with the size of every console-address variable the object defines (gen_platform_symbols.py reads it).
    if ok:
        with open(obj + ".sizes.json", "w") as f:
            json.dump(sizes, f)
    # Sidecar listing the alias symbols of this object: nm cannot tell them from real variables (it gives an alias its target's size),
    # and gen_platform_symbols.py must.
    side = obj + ".aliases.json"
    if aliases and ok:
        with open(side, "w") as f:
            json.dump(sorted(a[0] for a in aliases), f)
    elif os.path.exists(side):
        os.remove(side)
    errors = [l for l in stderr.splitlines() if "error:" in l]
    if be_error:
        errors = [be_error.splitlines()[0]] + errors
    # Count warnings by clang's own category tag, e.g. "[-Wint-conversion]": these are the places that assume 32-bit pointers.
    warnings = {}
    for l in stderr.splitlines():
        if "warning:" in l and l.rstrip().endswith("]") and "[-W" in l:
            tag = l[l.rindex("[-W"):].strip("[]")
            warnings[tag] = warnings.get(tag, 0) + 1
    result = {"file": rel, "ok": ok, "first_error": errors[0] if errors else None, "warnings": warnings,
              "bitfield_functions": bitfield_functions}
    if result["ok"]:
        with open(cache, "w") as f:
            json.dump({"sig": sig, "result": result}, f)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decomp", required=True, help="checkout of the decompilation named in port/decomp-manifest.txt (read only)")
    ap.add_argument("--target", help="clang target triple, e.g. i386-linux-musl (experiments); prefer --profile")
    ap.add_argument("--profile", choices=sorted(targets.TARGETS), help="build target from targets.py: triple plus the layout/float flags")
    ap.add_argument("--out", required=True)
    ap.add_argument("--opt", default="O0", help="optimisation level without the dash (O0, O1, O2); default O0")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = ap.parse_args()
    if bool(args.target) == bool(args.profile):
        ap.error("give exactly one of --target and --profile")
    triple = args.target or targets.TARGETS[args.profile]["triple"]
    extra = targets.TARGETS[args.profile]["cflags"] if args.profile else []

    obj_dir = os.path.join(args.out, "obj")
    os.makedirs(obj_dir, exist_ok=True)
    files = list_c_files(args.decomp)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda f: compile_one(args.decomp, triple, args.opt, obj_dir, f, extra), files))
    failed = [r for r in results if not r["ok"]]
    with open(os.path.join(args.out, "compile_report.json"), "w") as f:
        json.dump({"target": triple, "extra_flags": extra, "opt": args.opt, "files": len(files), "failed": len(failed), "results": results}, f, indent=1)
    print("%d files, %d compiled, %d failed (target %s, -%s, extra flags: %s)" % (len(files), len(files) - len(failed), len(failed), triple, args.opt, " ".join(extra) or "none"))
    totals = {}
    for r in results:
        for tag, n in r["warnings"].items():
            totals[tag] = totals.get(tag, 0) + n
    for tag, n in sorted(totals.items(), key=lambda kv: -kv[1])[:12]:
        print("  warnings %-45s %6d" % (tag, n))
    for r in failed[:20]:
        print("  FAILED %s: %s" % (r["file"], r["first_error"]))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
