#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Runs the "Move animation test" (src/platform/host_movetest.c) for a list of Pokemon/move pairs, one after the other, headless, and collects the logs.

    movetest_batch.py --program build/trace/kyujo --rom <rom.z64> --out logs  94:101  94:3  [--timeout 900]

Each pair is <species 1-151>:<move 1-165> (optionally :<opponent species>). Every test is a fresh start of the program, so it takes about 5 minutes
(the game plays through its own menus to a battle first); a list of 20 moves runs for about 100 minutes. Logs are named movefx-<pokemon>-<move>.log in --out.
An existing log of the same name in --out is kept (the new one is dropped), so earlier results are never overwritten. The program must be built with --trace-calls to record
anything (docs/move-trace.md). Linux only (headless). Standard library only."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--program", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=int, default=900, help="seconds allowed for one test (default 900)")
    ap.add_argument("pairs", nargs="+", metavar="SPECIES:MOVE[:OPPONENT]")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    failed = 0
    for pair in a.pairs:
        parts = pair.split(":")
        if len(parts) not in (2, 3) or not all(p.isdigit() for p in parts):
            sys.exit("not a pair: %r (expected SPECIES:MOVE or SPECIES:MOVE:OPPONENT, numbers)" % pair)
        arg = ",".join(parts if len(parts) == 3 else parts + ["0"])
        home = tempfile.mkdtemp(prefix="movetest-")
        env = dict(os.environ, KYUJO_MOVETEST=arg, KYUJO_MOVETEST_QUIT="1", KYUJO_HOME=home, KYUJO_HEADLESS="1",
                   KYUJO_ROM=os.path.abspath(a.rom))
        print("test %s ..." % pair, flush=True)
        try:
            r = subprocess.run([os.path.abspath(a.program)], env=env, capture_output=True, text=True, timeout=a.timeout)
            status = "ok" if r.returncode == 0 else "exit code %d" % r.returncode
        except subprocess.TimeoutExpired:
            status = "timed out after %d s" % a.timeout
        logs = [f for f in os.listdir(home) if f.startswith("movefx-") and f.endswith(".log")]
        for f in logs:
            if os.path.exists(os.path.join(a.out, f)):                       # an earlier run's log of the same pair: keep it, say so
                print("  (log %s already exists in %s: kept)" % (f, a.out))
            else:
                shutil.move(os.path.join(home, f), os.path.join(a.out, f))
        shutil.rmtree(home, ignore_errors=True)
        print("  %s%s" % (status, "; log: " + ", ".join(logs) if logs else "; no log"), flush=True)
        failed += status != "ok"
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
