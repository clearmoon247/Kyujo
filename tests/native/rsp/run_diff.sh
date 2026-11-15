#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
# run_diff.sh [scratch dir]  - builds and runs diff_vs_cxd4.c against the cxd4 vector unit (fetched into the scratch directory, not into the repo).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-${TMPDIR:-/tmp}/rsp-diff}
mkdir -p "$WORK"
if [ ! -d "$WORK/cxd4" ]; then
    git clone --depth 1 https://github.com/mupen64plus/mupen64plus-rsp-cxd4.git "$WORK/cxd4"
fi
echo "reference: $(git -C "$WORK/cxd4" rev-parse HEAD)"
# The reference is compiled WITHOUT SSE2 (plain C lanes); -w: its own warnings are not our business.
gcc -O1 -w -g -I"$WORK/cxd4" -I"$HERE/../../../src/platform" -o "$WORK/diff_vs_cxd4" "$HERE/diff_vs_cxd4.c" \
    "$WORK/cxd4/vu/vu.c" "$WORK/cxd4/vu/multiply.c" "$WORK/cxd4/vu/add.c" "$WORK/cxd4/vu/logical.c" "$WORK/cxd4/vu/select.c" "$WORK/cxd4/vu/divide.c"
"$WORK/diff_vs_cxd4" "${TRIALS:-20000}"
