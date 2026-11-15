#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Builds the native program: patched decomp C + native libultra + hand-written platform code + generated constants/stubs.

    build_native.py --decomp ext/decomp --profile linux32 --sysroot work/sysroot/musl-i386 --out build/linux32

Steps (each prints what it did; any failure stops the build with the tool's own message):
  0. srctree.py: check the decomp against port/decomp-manifest.txt, copy its sources and apply port/patches/*.patch
                                                                              -> <out>/tree      (the decomp itself is never edited)
  1. compile the game's C files from the tree                                 -> <out>/game/obj
  2. compile libultra's C files from the tree, collect them in an archive     -> <out>/libultra/libultra.a
        (voice/ is skipped: unused by this game; files listed in port/ultralib_replaced.txt are skipped: replaced by src/ultra/)
  3. compile src/ultra/*.c and src/game_asm/*.c (GAME side: replace the assembly)  -> <out>/ultra
  4. compile src/platform/*.c (HOST side: may use the C library)              -> <out>/host
  5. gen_platform_symbols.py: constants and stubs for what is still missing   -> <out>/gen
  6. assemble abs_symbols.S, compile stubs.c, fragment_table.c and fragment_data.c
  7. link, then check the program's address range (imagelimit.py)            -> <out>/<program>

Nothing derived from a ROM is produced (no ROM is needed to build).
Standard library only; needs clang, lld, llvm-ar and llvm-nm/llvm-readobj on PATH.
"""
import argparse
import glob
import json
import re
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# `python3 -I` does not add the script's folder to the import path; do it explicitly, for this repository's own modules only.
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compile_decomp  # noqa: E402
import proc  # noqa: E402
import srctree         # noqa: E402
import winres          # noqa: E402
import targets         # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SKIPPED_LIBULTRA_DIRS = ("lib/ultralib/src/voice/",)      # voice recognition: the game does not use it (it needs another header set)


def run(cmd, what):
    r = proc.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("FAILED: %s\n  command: %s\n%s\n" % (what, " ".join(cmd), r.stderr[-4000:]))
        sys.exit(1)
    return r


def read_replaced(path):
    """port/ultralib_replaced.txt: one libultra source path (relative to lib/ultralib/src) per line; '#' starts a comment."""
    if not os.path.exists(path):
        return set()
    names = set()
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if line:
            names.add("lib/ultralib/src/" + line)
    return names


TRACE_FLAG = "-finstrument-functions"        # makes clang call __cyg_profile_func_enter/exit (received by src/platform/host_trace.c)


def flags_for(rel, extra, trace_match):
    """The compile flags of one game file: `extra`, plus the call-trace flag when the file's path contains one of the --trace-calls words."""
    if trace_match and any(w in rel for w in trace_match):
        return list(extra) + [TRACE_FLAG]
    return list(extra)


def compile_all(tree, t, opt, files, out_dir, jobs, extra, lib, force=False, aliases=None, trace_match=(), keep_ir=False, backend_opt=None):
    """aliases: {object file name: [(symbol, target, offset)]} from the previous build's plan (step 5b); see compile_decomp.alias_ir."""
    os.makedirs(out_dir, exist_ok=True)
    aliases = aliases or {}
    def one(f):
        mine = aliases.get(os.path.basename(compile_decomp.object_path(out_dir, f)), [])
        return compile_decomp.compile_one(tree, t["triple"], opt, out_dir, f, flags_for(f, extra, trace_match), lib, force, aliases=[tuple(a) for a in mine], keep_ir=keep_ir, backend_opt=backend_opt)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        return list(pool.map(one, files))


def write_symbol_file(program, out_path):
    """Writes "<hex address> <name>" for every code symbol of the linked program, sorted by address. Only the development call trace
    (src/platform/host_trace.c) reads it, to print function names. Tested on the Linux program; for the Windows exe the file is written too, but it is NOT verified that its addresses match the running program."""
    r = subprocess.run(["llvm-nm", "--defined-only", "-n", program], capture_output=True, text=True)
    lines = []
    if r.returncode == 0:
        for ln in r.stdout.splitlines():
            parts = ln.split(None, 2)
            if len(parts) == 3 and parts[1] in ("T", "t", "W", "w"):
                lines.append("%s %s" % (parts[0], parts[2]))
    if lines:
        with open(out_path, "w") as f:
            f.write("\n".join(lines) + "\n")
        print("   symbol file for the call trace: %s (%d code symbols)" % (out_path, len(lines)))
    else:
        print("   no symbol file written (llvm-nm found no code symbols; the call trace will print addresses)")


def wasm_finish(args, BACKEND, t, tree, gen_dir, game_obj, lib_obj, ultra_obj, host_obj, archive, files, lib_files, ultra_src):
    """Steps 6 and 7 for the WebAssembly core: absolute constants into the code, the generated pieces, the link order, wasm-ld, and a check of the result."""
    triple = t["triple"]
    sysroot = args.sysroot
    absmap = json.load(open(os.path.join(gen_dir, "abs_map.json")))
    gepmap = json.load(open(os.path.join(gen_dir, "gep_map.json")))
    # 6a. Second objects with the absolute symbols substituted (only those whose IR uses one). Pass-1 objects stay as they are: the next run of the
    # generator scans them, and it must still see the symbols as unresolved.
    abs_dirs = {}
    pats = re.compile(r"@(%s)(?![\w.$])" % "|".join(re.escape(n) for n in sorted(set(absmap) | set(gepmap), key=len, reverse=True)))
    def second(obj):
        ir = obj[:-2] + ".be.ll"
        d2 = os.path.join(os.path.dirname(os.path.dirname(obj)), "abs_" + os.path.basename(os.path.dirname(obj)))   # a sibling folder: the generator scans obj/ recursively
        os.makedirs(d2, exist_ok=True)
        out = os.path.join(d2, os.path.basename(obj))
        if not os.path.exists(ir) or not pats.search(open(ir).read()):
            if os.path.exists(out):
                os.remove(out)
            return obj
        if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(ir) and os.path.getmtime(out) >= os.path.getmtime(os.path.join(gen_dir, "abs_map.json")) and os.path.getmtime(out) >= os.path.getmtime(os.path.join(gen_dir, "gep_map.json")):
            return out
        ok, msg = compile_decomp.recompile_ir(obj, absmap, triple, BACKEND or args.opt, t["cflags"], out, gepmap)
        if not ok:
            sys.exit("recompiling %s with the absolute symbols failed: %s" % (obj, msg[:300]))
        return out
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        game_objs = list(pool.map(second, sorted(glob.glob(os.path.join(game_obj, "*.o")))))
        ultra_objs = list(pool.map(second, sorted(glob.glob(os.path.join(ultra_obj, "*.o")))))
        lib_objs = list(pool.map(second, sorted(glob.glob(os.path.join(lib_obj, "*.o")))))
    n_abs = sum(1 for o in game_objs + ultra_objs + lib_objs if os.sep + "abs_" in o)
    archive2 = os.path.join(args.out, "libultra", "libultra_abs.a")
    if os.path.exists(archive2):
        os.remove(archive2)
    run(["llvm-ar", "rcs", archive2] + lib_objs, "archive libultra (second objects)")
    print("6. absolute symbols (%d) substituted in %d objects" % (len(absmap), n_abs))

    # 6b. Generated pieces and markers.
    cc = ["clang", "--target=" + triple, "-nostdinc", "-isystem", targets.clang_resource_include(), "-fno-builtin", "-fno-pic", "-fno-pie", "-O1", "-mbulk-memory"]
    gen_objs = {}
    for name in ("stubs", "fragment_table", "fragment_data"):
        gen_objs[name] = os.path.join(gen_dir, name + ".o")
        run(cc + ["-c", os.path.join(gen_dir, name + ".c"), "-o", gen_objs[name]], "compile %s.c" % name)
    gen_objs["image"] = os.path.join(gen_dir, "gen_image.o")
    run(["clang", "--target=" + triple, "-c", os.path.join(gen_dir, "gen_image.ll"), "-o", gen_objs["image"]], "compile gen_image.ll")
    mark = os.path.join(args.out, "markers")
    os.makedirs(mark, exist_ok=True)
    def marker(src_text, name):
        c = os.path.join(mark, name + ".c")
        with open(c, "w") as f:
            f.write(src_text)
        o = os.path.join(mark, name + ".o")
        run(cc + ["-c", c, "-o", o], "compile marker " + name)
        return o
    be_begin = os.path.join(mark, "be_begin.o")
    be_end = os.path.join(mark, "be_end.o")
    run(cc + ["-c", os.path.join(REPO, "src", "platform", "wasm_markers", "be_begin.c"), "-o", be_begin], "compile be_begin.c")
    run(cc + ["-c", os.path.join(REPO, "src", "platform", "wasm_markers", "be_end.c"), "-o", be_end], "compile be_end.c")
    overlays = json.load(open(os.path.join(gen_dir, "overlays.json")))
    frag_dirs = compile_decomp.fragment_dirs(tree)
    begin_of, end_of = {}, {}
    for idx in overlays:
        b = "".join('unsigned char gFrag%02d%sBegin __attribute__((section("f%s%02d"), used));\n' % (idx, tag, tag, idx) for tag in "dz")
        e = "".join('unsigned char gFrag%02d%sEnd __attribute__((section("f%s%02d"), used));\n' % (idx, tag, tag, idx) for tag in "dz")
        begin_of[idx] = marker(b, "frag%02d_begin" % idx)
        end_of[idx] = marker(e, "frag%02d_end" % idx)

    # 6c. Link order: be_begin first and be_end last; each overlay's files between its own markers (same-named sections are joined in link order).
    def overlay_of(obj):
        b = os.path.basename(obj)
        for idx, d in sorted(enumerate(frag_dirs), key=lambda x: -len(x[1])):
            if b.startswith("src_fragments_%s_" % d):
                return idx
        return None
    order = [be_begin]
    by_overlay = {}
    for o in game_objs:
        idx = overlay_of(o)
        if idx in begin_of:
            by_overlay.setdefault(idx, []).append(o)
        else:
            order.append(o)
    for idx in sorted(by_overlay):
        order += [begin_of[idx]] + by_overlay[idx] + [end_of[idx]]
    order += ultra_objs + sorted(glob.glob(os.path.join(host_obj, "*.o"))) + [gen_objs[k] for k in sorted(gen_objs)]
    libs = os.path.join(sysroot, "lib", "wasm32-wasi")
    program = os.path.join(args.out, t["program"])
    cmd = ["wasm-ld", "--import-undefined", "--no-gc-sections", "--stack-first", "-z", "stack-size=524288", "--global-base=524288",
           "--initial-memory=%d" % (128 << 20), "--max-memory=%d" % (128 << 20), "--error-limit=0", "--export=_start", "--export-if-defined=core_raw_input_ptr",
           "--export-if-defined=core_raw_input_commit", "--export=__stack_pointer", "--export=__heap_base", "--export=__data_end", "--export=gMainImage",
           "--export-if-defined=gBeFixupsBegin", "--export-if-defined=gBeFixupsEnd"] + ["--export-if-defined=gFrag%02d%s%s" % (i, tg, w) for i in overlays for tg in "dz" for w in ("Begin", "End")] + [
           "-o", program, os.path.join(libs, "crt1-command.o")] + order + [archive2, os.path.join(libs, "libc.a"), os.path.join(libs, "libclang_rt.builtins-wasm32.a"), be_end]
    run(cmd, "link")
    print("7. linked %s (%d bytes)" % (program, os.path.getsize(program)))
    return 0



def main():
    proc.ensure_utf8_mode()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decomp", required=True, help="the decomp: a git checkout, a folder or a .zip (checked against --manifest)")
    ap.add_argument("--manifest", default=srctree.DEFAULT_MANIFEST, help="the list of decomp files (and hashes) the build reads")
    ap.add_argument("--profile", required=True, choices=sorted(targets.TARGETS))
    ap.add_argument("--sysroot", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--patches", default=os.path.join(REPO, "port", "patches"))
    ap.add_argument("--opt", default="O0", help="optimisation level of the GAME code (decomp, libultra replacements): the big-endian memory "
                    "rewrite (be_memory.py) is written for unoptimised IR, so this stays O0")
    ap.add_argument("--backend-opt", default=None, help="optimisation level of the code generator for GAME code (default: the same as --opt). "
                    "The front end always uses --opt, because the big-endian rewrite is written for unoptimised IR")
    ap.add_argument("--host-opt", default="O2", help="optimisation level of the host-side code (src/platform, src/render: scheduler, hardware "
                    "model, software renderer). It is ordinary C, so it can be optimised; the renderer is the hot spot")
    ap.add_argument("--trace-calls", default="", metavar="WORDS", help="development: comma-separated words; every GAME file whose path contains one is compiled with "
                    "-finstrument-functions so that host_trace.c can record which of its functions run (docs/move-trace.md). Instrumenting ALL files "
                    "makes the program too big (imagelimit.py), so name folders, e.g. fragments/battle_engine,particle. Use a separate --out directory.")
    ap.add_argument("--define", action="append", default=[], metavar="NAME", help="a macro for the host-side code (src/platform, src/render, src/rsp), e.g. HOST_SHELL_PICKER for the Android "
                    "build's own game module; the game code is not affected")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = ap.parse_args()
    trace_match = [w for w in args.trace_calls.split(",") if w]
    for name in ("decomp", "manifest", "sysroot", "out", "patches"):          # the compiles run in other folders: every path must be absolute
        setattr(args, name, os.path.abspath(getattr(args, name)))

    t = targets.TARGETS[args.profile]
    BACKEND = args.backend_opt            # O0 everywhere by default: the validated Linux/Windows setting. A global-changing optimiser (O1+) shrank D_800A83A0 from 4 to 1 byte (GlobalOpt), so code generation above O0 needs a pipeline that leaves globals alone first
    KEEP = args.profile == "wasm32"          # the WebAssembly build keeps the rewritten IR (absolute symbols are substituted in it, see wasm_finish)
    tree = os.path.join(args.out, "tree")
    game_obj = os.path.join(args.out, "game", "obj")
    lib_obj = os.path.join(args.out, "libultra", "obj")
    ultra_obj = os.path.join(args.out, "ultra")
    host_obj = os.path.join(args.out, "host")
    gen_dir = os.path.join(args.out, "gen")
    os.makedirs(args.out, exist_ok=True)

    # 0. patched source tree ---------------------------------------------------------------------------------------------------
    try:
        decomp = srctree.open_source(args.decomp, args.out, args.manifest)       # a checkout, a folder or a zip; every file verified
    except srctree.SourceError as e:
        sys.exit(str(e))
    rebuilt = srctree.make(decomp, tree, args.patches, args.manifest)
    print("0. source tree: %s (%d patches)" % ("rebuilt" if rebuilt else "up to date", len(srctree.patch_files(args.patches))))

    # 1. game code ------------------------------------------------------------------------------------------------------------
    files = compile_decomp.list_c_files(tree)
    # The aliases of step 5b are part of the game objects. The previous build's plan is used here so that an unchanged tree is a cache hit
    # (the plan only depends on the decomp's documented addresses, so it is stable); on a first build there is none and 5b adds them.
    plan_file = os.path.join(gen_dir, "alias_plan.json")
    previous_plan = json.load(open(plan_file)) if os.path.exists(plan_file) else {}
    results = compile_all(tree, t, args.opt, files, game_obj, args.jobs, t["cflags"], lib=False, aliases=previous_plan, trace_match=trace_match, keep_ir=KEEP, backend_opt=BACKEND)
    failed = [r for r in results if not r["ok"]]
    if failed:
        sys.exit("game code failed to compile (%d files):\n" % len(failed) +
                 "\n".join("  %s: %s" % (r["file"], (r["first_error"] or "")[:160]) for r in failed[:40]))
    print("1. game code: %d files compiled" % len(files))
    known = sorted((r["file"], r["bitfield_functions"]) for r in results if r.get("bitfield_functions"))
    if known:
        print("   WARNING: %d file(s) use bit-fields (port/bitfield_known.txt); their bit-field values are WRONG until fixed:\n%s" %
              (len(known), "\n".join("     %s (%d function(s))" % (f, len(fn)) for f, fn in known)))

    # 2. libultra ---------------------------------------------------------------------------------------------------------------
    replaced = read_replaced(os.path.join(REPO, "port", "ultralib_replaced.txt"))
    lib_files = [f for f in compile_decomp.list_ultralib_c_files(tree) if f not in replaced and not f.startswith(SKIPPED_LIBULTRA_DIRS)]
    lib_flags = t["cflags"] + ["-I" + os.path.join(tree, "lib", "ultralib", "src")]
    results = compile_all(tree, t, args.opt, lib_files, lib_obj, args.jobs, lib_flags, lib=True, keep_ir=KEEP, backend_opt=BACKEND)
    # Objects left over from files that are no longer compiled (a new entry in ultralib_replaced.txt) must not reach the archive.
    wanted = {f.replace("/", "_")[:-2] + ".o" for f in lib_files}
    for stale in glob.glob(os.path.join(lib_obj, "*.o*")):
        if os.path.basename(stale).split(".o")[0] + ".o" not in wanted:
            os.remove(stale)
    bad = [r for r in results if not r["ok"]]
    # A libultra file that does not compile is not fatal by itself (the bundled library has files for other boards); it is fatal
    # only if the linker then needs a symbol that file would have defined, and that shows up by name in step 5/7.
    archive = os.path.join(args.out, "libultra", "libultra.a")
    if os.path.exists(archive):
        os.remove(archive)
    ok_objs = sorted(glob.glob(os.path.join(lib_obj, "*.o")))
    run(["llvm-ar", "rcs", archive] + ok_objs, "archive libultra")
    print("2. libultra: %d C files, %d compiled into %s%s" % (len(lib_files), len(lib_files) - len(bad), os.path.relpath(archive, args.out),
          "" if not bad else "; NOT compiled (the linker step reports it if the game needs them):\n" +
          "\n".join("     %s: %s" % (os.path.basename(r["file"]), (r["first_error"] or "?").split(": ", 2)[-1][:110]) for r in sorted(bad, key=lambda r: r["file"]))))

    # 3. libultra replacements (game side, written for this port) ---------------------------------------------------------------
    # src/ultra/*.c replace libultra's assembly; src/game_asm/*.c replace the game's own hand-written assembly (rom.yaml `hasm` segments).
    ultra_src = sorted(glob.glob(os.path.join(REPO, "src", "ultra", "*.c")) + glob.glob(os.path.join(REPO, "src", "game_asm", "*.c")))
    results = compile_all(tree, t, args.opt, ultra_src, ultra_obj, args.jobs,
                          lib_flags + ["-I" + os.path.join(tree, "lib", "ultralib", "src", "os"), "-I" + os.path.join(REPO, "src", "platform"), "-Wall"], lib=True, force=True, keep_ir=KEEP, backend_opt=BACKEND)
    failed = [r for r in results if not r["ok"]]
    if failed:
        sys.exit("src/ultra failed to compile: %s: %s" % (failed[0]["file"], failed[0]["first_error"]))
    print("3. libultra replacements: %d files compiled" % len(ultra_src))

    # 4. host-side platform code ---------------------------------------------------------------------------------------------------
    os.makedirs(host_obj, exist_ok=True)
    host_flags = targets.host_include_flags(args.profile, args.sysroot) + t["cflags"] + \
        ["-std=gnu11", "-Wall", "-Wextra", "-g", "-" + args.host_opt, "-fno-strict-aliasing", "-I" + os.path.join(REPO, "src", "platform")] + ["-D" + d for d in args.define]
    # the renderer (RSP display-list interpreter, software RDP, video output) is host-side code too
    host_sources = sorted(glob.glob(os.path.join(REPO, "src", "platform", "*.c")) + glob.glob(os.path.join(REPO, "src", "render", "*.c")) +
                          glob.glob(os.path.join(REPO, "src", "rsp", "*.c")))
    if args.profile == "wasm32":
        # The sandboxed core has no window, sound-server or gamepad code of its own: the shell (SDL2) does that, through host_window_shell.c.
        skip = {"host_x11.c", "host_evdev.c", "host_pulse.c", "host_window.c", "host_window_linux.c", "host_crashchain.c", "host_ubsan.c", "rdp.c", "vi_out.c", "rsp_cpu.c"}   # rdp.c, vi_out.c, rsp_cpu.c: native side of the shell (tools/native/build_shell.py)
        host_sources = [f for f in host_sources if os.path.basename(f) not in skip]
    host_flags = host_flags + ["-I" + os.path.join(REPO, "src", "render")]
    keep = set(os.path.basename(f)[:-2] + ".o" for f in host_sources) | {"movetest_names.o"}
    for stale in glob.glob(os.path.join(host_obj, "*.o")):                      # an object of a file this profile no longer compiles would be linked in too
        if os.path.basename(stale) not in keep:
            os.remove(stale)
    for src in host_sources:
        run(["clang"] + host_flags + ["-c", src, "-o", os.path.join(host_obj, os.path.basename(src)[:-2] + ".o")], "compile " + src)
    # The species and move names of the "Move animation test" page (src/platform/host_movetest.c), read from the decomp being built. Compiled into the
    # host objects (not the generated ones) so that the stub generator below sees that the two tables exist.
    os.makedirs(gen_dir, exist_ok=True)
    run(proc.python_cmd() + [os.path.join(HERE, "gen_movetest_names.py"), tree, os.path.join(gen_dir, "movetest_names.c")], "generate the move test names")
    run(["clang"] + host_flags + ["-c", os.path.join(gen_dir, "movetest_names.c"), "-o", os.path.join(host_obj, "movetest_names.o")], "compile movetest_names.c")
    print("4. platform code: %d files compiled" % len(host_sources))

    # 5. constants and stubs --------------------------------------------------------------------------------------------------------
    os.makedirs(gen_dir, exist_ok=True)
    gen_cmd = proc.python_cmd() + [os.path.join(HERE, "gen_platform_symbols.py"), "--decomp", decomp, "--tree", tree, "--profile", args.profile,
               "--sysroot", args.sysroot, "--objs", game_obj, ultra_obj, host_obj, "--archives", archive, "--out", gen_dir]
    r = run(gen_cmd, "generate symbols")
    print("5. generated symbols:\n" + "\n".join("     " + l for l in r.stdout.splitlines()))

    # 5a. a function that the port's own code calls but no file defines is not a build error (it becomes a stub that stops the game with its name when it is
    # called): host_window_prepare was missing from the shell's window file and the game on the Switch would have stopped at its start. Only the functions
    # listed in port/allowed_stubs.txt may be stubbed (the decomp's own unfinished ones); any other fails the build.
    allowed = set(l.split("#")[0].strip() for l in open(os.path.join(REPO, "port", "allowed_stubs.txt")).read().splitlines())
    stubs = sorted(set(json.load(open(os.path.join(gen_dir, "report.json")))["function_stubs"]) - allowed)
    if stubs:
        sys.exit("functions that nothing defines (they would stop the game when called): %s\n  implement them for profile %s, or, for a function of the decomp that "
                 "is never called in play, list them in port/allowed_stubs.txt" % (", ".join(stubs), args.profile))

    # 5b. aliases into C variables. gen_platform_symbols.py decided which documented symbols live INSIDE a variable the port compiles from
    # C; those symbols are compiled into the object that defines the variable (compile_decomp.alias_ir). The plan depends only on the
    # decomp's documented addresses, so a second build finds the same plan and every compile is a cache hit. If something was rebuilt, the
    # generator runs again so that its output reflects the objects as they now are.
    plan = json.load(open(os.path.join(gen_dir, "alias_plan.json")))
    by_object = {os.path.basename(compile_decomp.object_path(game_obj, f)): f for f in files}
    rebuilt_any = False
    for obj_name, aliases in sorted(plan.items()):
        rel = by_object[obj_name]
        obj = os.path.join(game_obj, obj_name)
        before = os.path.getmtime(obj)
        res = compile_decomp.compile_one(tree, t["triple"], args.opt, game_obj, rel, flags_for(rel, t["cflags"], trace_match), lib=False, aliases=[tuple(a) for a in aliases], keep_ir=KEEP, backend_opt=BACKEND)
        if not res["ok"]:
            sys.exit("recompiling %s with its aliases failed: %s" % (rel, res["first_error"]))
        rebuilt_any |= os.path.getmtime(obj) != before
    print("5b. aliases into C variables: %d symbols in %d objects%s" % (sum(len(v) for v in plan.values()), len(plan),
                                                                      " (objects rebuilt, symbols regenerated)" if rebuilt_any else ""))
    if rebuilt_any:
        r = run(gen_cmd, "generate symbols (second run)")
        print("\n".join("     " + l for l in r.stdout.splitlines()))

    if args.profile == "wasm32":
        return wasm_finish(args, BACKEND, t, tree, gen_dir, game_obj, lib_obj, ultra_obj, host_obj, archive, files, lib_files, ultra_src)

    # 6. assemble / compile generated files ---------------------------------------------------------------------------------------------
    run(["clang", "--target=" + t["triple"], "-c", os.path.join(gen_dir, "abs_symbols.S"), "-o", os.path.join(gen_dir, "abs_symbols.o")], "assemble abs_symbols.S")
    run(["clang", "--target=" + t["triple"], "-nostdinc", "-isystem", targets.clang_resource_include(), "-fno-builtin", "-fno-pic", "-fno-pie",
         "-c", os.path.join(gen_dir, "stubs.c"), "-o", os.path.join(gen_dir, "stubs.o")], "compile stubs.c")
    # fragment_table.c is HOST data (read by src/platform/host_fragment.c in the host's own byte order), so it is compiled plainly.
    run(["clang", "--target=" + t["triple"], "-nostdinc", "-isystem", targets.clang_resource_include(), "-fno-builtin", "-fno-pic", "-fno-pie",
         "-c", os.path.join(gen_dir, "fragment_table.c"), "-o", os.path.join(gen_dir, "fragment_table.o")], "compile fragment_table.c")
    # fragment_data.c is host data too (the places of the code overlays' variables, see host_fragment.c).
    run(["clang", "--target=" + t["triple"], "-nostdinc", "-isystem", targets.clang_resource_include(), "-fno-builtin", "-fno-pic", "-fno-pie",
         "-c", os.path.join(gen_dir, "fragment_data.c"), "-o", os.path.join(gen_dir, "fragment_data.o")], "compile fragment_data.c")
    gen_objs = [os.path.join(gen_dir, "abs_symbols.o"), os.path.join(gen_dir, "stubs.o"), os.path.join(gen_dir, "fragment_table.o"),
                os.path.join(gen_dir, "fragment_data.o")]

    # 7. link ----------------------------------------------------------------------------------------------------------------------------
    program = os.path.join(args.out, t["program"])
    objs = sorted(glob.glob(os.path.join(game_obj, "*.o"))) + sorted(glob.glob(os.path.join(ultra_obj, "*.o"))) + \
        sorted(glob.glob(os.path.join(host_obj, "*.o"))) + gen_objs + [archive]
    if args.profile == "win32":                                   # program icon and version text (optional: skipped when a tool is missing)
        res = winres.make_resource(os.path.join(args.out, "winres"))
        if res:
            objs.append(res)
    run(targets.link_command(args.profile, args.sysroot, objs, program, gui=(args.profile == "win32")), "link")
    print("7. linked %s (%d bytes)" % (program, os.path.getsize(program)))
    write_symbol_file(program, os.path.join(args.out, "kyujo.syms"))
    r = subprocess.run(proc.python_cmd() + [os.path.join(HERE, "imagelimit.py"), program], capture_output=True, text=True)
    print("   address range: " + r.stdout.strip().replace("\n", "; "))
    if r.returncode != 0:
        sys.exit("FAILED: the program's memory reaches the range the game uses for segment numbers (see tools/native/imagelimit.py)")
    if args.profile == "linux32":
        # 8. the Linux probe: a small program that checks the window, the gamepads and the sound on the player's machine without the game
        # (tools/linux/kyujo_probe.c). It is linked from the SAME host objects as the game uses for those three parts.
        probe_dir = os.path.join(args.out, "probe")
        os.makedirs(probe_dir, exist_ok=True)
        probe_obj = os.path.join(probe_dir, "kyujo_probe.o")
        run(["clang"] + host_flags + ["-c", os.path.join(REPO, "tools", "linux", "kyujo_probe.c"), "-o", probe_obj], "compile the probe")
        parts = [os.path.join(host_obj, n + ".o") for n in ("host_x11", "host_evdev", "host_pulse", "host_keys")]
        probe = os.path.join(args.out, "kyujo_probe")
        link = targets.link_command(args.profile, args.sysroot, [probe_obj] + parts, probe)
        link = [a for a in link if not a.startswith("-Wl,--image-base")]   # the probe has no game memory model; any base will do
        run(link, "link the probe")
        print("8. linked %s (%d bytes)" % (probe, os.path.getsize(probe)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
