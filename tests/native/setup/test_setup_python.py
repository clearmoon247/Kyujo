#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""The Python that Setup.ps1 uses (the one inside the llvm-mingw package) is a small build WITHOUT zlib, bz2, lzma, ssl, sqlite3 or ctypes. It was found
the hard way: the first real run of the setup stopped with "No module named 'zlib'". This test keeps that from happening again, without Windows:

  1. every program Setup.ps1 starts with that Python is run here with those modules made unimportable (--help or a real small job);
  2. a compressed zip (the form GitHub serves) is unpacked the way Setup.ps1 does it (only the manifest's files) and verified as a folder;
  3. the self-test line in Setup.ps1 and the llvm-mingw zip's Python (when the zip is given) agree: no module in the self-test that the package lacks.

    python3 -I tests/native/setup/test_setup_python.py [llvm-mingw-zip] [decomp-zip]
"""
import os
import re
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
NATIVE = os.path.join(ROOT, "tools", "native")
SETUP_PS1 = os.path.join(ROOT, "dist", "windows-setup", "setup", "Setup.ps1")
MANIFEST = os.path.join(ROOT, "port", "decomp-manifest.txt")
MISSING = ("zlib", "bz2", "lzma", "ssl", "_ssl", "sqlite3", "_sqlite3", "ctypes", "_ctypes", "_hashlib")   # _hashlib is absent too: hashlib uses its built-in code

BLOCK = ("import sys\n"
         "class B:\n"
         "    def find_spec(self, name, path=None, target=None):\n"
         "        if name in %r: raise ImportError('blocked (not in the llvm-mingw Python): ' + name)\n"
         "sys.meta_path.insert(0, B())\n"
         "sys.argv = sys.argv[1:]\n"
         "import runpy; runpy.run_path(sys.argv[0], run_name='__main__')\n") % (MISSING,)


def run_blocked(script, *args):
    with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as f:
        f.write(BLOCK)
    try:
        return subprocess.run([sys.executable, "-I", f.name, script] + list(args), capture_output=True, text=True, timeout=600)
    finally:
        os.unlink(f.name)


def run_blocked_code(code):
    with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as f:
        f.write(BLOCK.replace("import runpy; runpy.run_path(sys.argv[0], run_name='__main__')", "exec(compile(sys.argv[0], '<test>', 'exec'))"))
    try:
        return subprocess.run([sys.executable, "-I", f.name, code], capture_output=True, text=True, timeout=60)
    finally:
        os.unlink(f.name)


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def main():
    llvm_zip = sys.argv[1] if len(sys.argv) > 1 else None
    decomp_zip = sys.argv[2] if len(sys.argv) > 2 else None

    # 1. the programs Setup.ps1 starts
    for script, args in (("build_native.py", ["--help"]), ("pe_check.py", ["--help"]), ("srctree.py", ["--help"]),
                         (os.path.join("..", "rom_tool.py"), ["--help"])):
        r = run_blocked(os.path.join(NATIVE, script), *args)
        if "blocked (not in the llvm-mingw Python)" in r.stderr:
            fail("%s needs a module the Windows setup's Python does not have:\n%s" % (script, r.stderr[-600:]))
        print("ok   %-16s starts without %s" % (script, ", ".join(MISSING[:4]) + ", ..."))

    # 2. the self-test line
    text = open(SETUP_PS1, encoding="utf-8").read()
    m = re.search(r"\$test = '(import [^']*)'", text)
    if not m:
        fail("Setup.ps1 has no self-test line")
    mods = set(re.findall(r"[A-Za-z_][\w.]*", m.group(1).split(";")[0].replace("import", "")))
    bad = sorted(mods & set(MISSING))
    if bad:
        fail("Setup.ps1's self-test imports %s, which the compiler package's Python does not have" % bad)
    print("ok   self-test imports only:", ", ".join(sorted(mods)))
    # Windows PowerShell 5.1 hands a native program its arguments without the double quotes inside them (hashlib.md5(b"x") became b x: the first real
    # run of the fixed setup stopped with NameError: name 'bx' is not defined), so the one-line programs must not contain any.
    if '"' in m.group(1):
        fail("Setup.ps1's self-test contains a double quote; Windows PowerShell 5.1 would drop it when starting python.exe")
    r = run_blocked_code(m.group(1))
    if r.returncode != 0:
        fail("the self-test line does not run (with zlib etc. blocked):\n%s" % r.stderr[-400:])
    print("ok   the self-test line runs with zlib etc. blocked, no double quotes in it:", r.stdout.strip())

    # 3. what the llvm-mingw zip has
    if llvm_zip:
        names = zipfile.ZipFile(llvm_zip).namelist()
        pyd = [n.split("/")[-1].split(".")[0] for n in names if "/python/lib/python3.11/lib-dynload/" in n]
        for lack in ("zlib", "_hashlib", "_ssl", "bz2", "_lzma"):
            if lack in pyd:
                print("note: the package now HAS %s; the MISSING list in this test can be shortened" % lack)
        for need in ("_md5", "_sha256", "_json", "_struct", "_random"):
            if need not in pyd:
                fail("the compiler package's Python has no %s module (needed by hashlib/json/struct/tempfile)" % need)
        print("ok   the package's Python has _md5 _sha256 _json _struct _random (and %s zlib)" % ("DOES have" if "zlib" in pyd else "has no"))

    # 4. a compressed zip, unpacked like Setup.ps1's Expand-Decomp (manifest files only), then verified as a folder
    if decomp_zip:
        wanted = set()
        for line in open(MANIFEST, encoding="utf-8"):
            p = line.rstrip("\n").split(" ", 2)
            if len(p) == 3 and len(p[0]) == 64:
                wanted.add(p[2])
        with tempfile.TemporaryDirectory() as d:
            z = zipfile.ZipFile(decomp_zip)
            n = 0
            for e in z.infolist():
                if "/" not in e.filename:
                    continue
                rel = e.filename.split("/", 1)[1]
                if rel in wanted:
                    t = os.path.join(d, *rel.split("/"))
                    os.makedirs(os.path.dirname(t), exist_ok=True)
                    with open(t, "wb") as f:
                        f.write(z.read(e))
                    n += 1
            r = run_blocked(os.path.join(NATIVE, "srctree.py"), "verify", "--decomp", d, "--manifest", MANIFEST)
            if r.returncode != 0:
                fail("verify of the unpacked folder failed:\n%s%s" % (r.stdout[-600:], r.stderr[-600:]))
            print("ok   unpacked %d of %d manifest files; verify (with zlib blocked): %s" % (n, len(wanted), (r.stdout.strip().splitlines() or ["ok"])[-1]))
            r = run_blocked(os.path.join(NATIVE, "srctree.py"), "verify", "--decomp", decomp_zip, "--manifest", MANIFEST)
            if r.returncode == 0 or "no zlib module" not in (r.stdout + r.stderr):
                fail("verify of a compressed zip without zlib should say so clearly:\n%s%s" % (r.stdout[-300:], r.stderr[-300:]))
            print("ok   a compressed zip without zlib is refused with a clear message")
    print("all good")


if __name__ == "__main__":
    main()
