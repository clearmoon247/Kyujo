#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Per-function table from a KYUJO_PROFILE file (see src/platform/host_perf.c).

    profile_report.py <profile file> <program (the linux32 build)> [top N]

The profile is raw little-endian 32-bit instruction addresses, one per 2 ms of CPU time. They are matched against `nm -n` of the program
(not position independent, so the addresses are final). Percentages are of all samples. Standard library plus the `nm` tool."""
import bisect, struct, subprocess, sys, collections

prof, prog = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
data = open(prof, "rb").read()
samples = struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4])
syms = []
for line in subprocess.run(["nm", "-n", "--defined-only", prog], capture_output=True, text=True, check=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
cnt = collections.Counter()
for s in samples:
    i = bisect.bisect_right(addrs, s) - 1
    cnt[syms[i][1] if i >= 0 else "?"] += 1
n = len(samples)
print("%d samples (%.1f s of CPU time)" % (n, n * 0.002))
for name, c in cnt.most_common(top):
    print("%6.2f%%  %s" % (100.0 * c / n, name))
