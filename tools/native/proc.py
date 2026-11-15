#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Runs a program, handing a very long argument list over in a response file (`@file`).

Why: Windows refuses a command line longer than 32,767 characters, and the final link of the game names over 700 object files by their full
paths. clang, lld, llvm-ar and llvm-nm all read `@file`. The same rule is used on every host (not only on Windows), so Linux builds and
checks exercise the code path that a Windows build depends on.

Quoting: one argument per line, double quotes around an argument that contains a space, and every backslash turned into a forward slash
(Windows accepts both in a path, and the LLVM tools' two tokenisers - "GNU" and "Windows" - disagree about backslashes but agree about
double-quoted text without them). No argument of this build contains a backslash that is not a path separator or a double quote.

Standard library only.
"""
import os
import subprocess
import sys
import tempfile

LIMIT = 8000          # characters of argument text above which a response file is used (Windows' limit is 32,767; this leaves a wide margin)


def _quote(arg):
    arg = arg.replace("\\", "/")
    if '"' in arg:
        raise ValueError("an argument with a double quote cannot go into a response file: %r" % arg)
    return '"%s"' % arg if any(c in arg for c in " \t") else arg


def run(cmd, **kwargs):
    """subprocess.run(cmd, **kwargs), with `@file` replacing the arguments when they are long. The program (cmd[0]) is never moved."""
    cmd = list(cmd)
    if sum(len(a) + 1 for a in cmd) <= LIMIT:
        return subprocess.run(cmd, **kwargs)
    fd, path = tempfile.mkstemp(prefix="args-", suffix=".rsp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(_quote(a) for a in cmd[1:]) + "\n")
        return subprocess.run([cmd[0], "@" + path], **kwargs)
    finally:
        os.remove(path)


# ---------------------------------------------------------------------------------------------------------------- Python's text encoding

def python_cmd():
    """argv start for running another script of this build: this Python, in isolated mode, with UTF-8 text files.
    Why UTF-8: the decomp has 22 source files with non-ASCII characters, and Windows' Python reads text files in the system's ANSI code page
    (cp1252 ...) unless told otherwise, which would decode them wrongly (or fail). `-X utf8` makes every open() use UTF-8 on every host."""
    return [sys.executable, "-X", "utf8", "-I"]


def ensure_utf8_mode():
    """Call first thing in a script that reads text files. If this Python is not in UTF-8 mode, runs the same command again in it."""
    if sys.flags.utf8_mode:
        return
    cmd = [sys.executable, "-X", "utf8"] + (["-I"] if sys.flags.isolated else []) + sys.argv
    sys.exit(subprocess.call(cmd))
