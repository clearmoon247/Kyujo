#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Builds a DEVELOPMENT copy of the source tree that compares the working software renderer (src/render/rdp.c) with an OLD version of it on REAL
game frames, inside one run of the program.

    make_rdp_ab_tree.py --ref-commit <git commit with the trusted rdp.c> --out <new directory>
    then build <out> like the normal tree (tools/native/build_native.py inside <out>) and run the game with
    KYUJO_GFX_BENCH=<seconds,seconds,...>:<runs>     (see src/render/gfx_host.c)

What the copy adds (the shipped tree is not touched; the copy is never committed):
  - src/render/rdp_ref.c   the trusted rdp.c with every public name prefixed ref_ (plus rdp_dev_color_image_hash, which the working file also has)
  - src/render/gfx_ab.c    records the commands f3dex2.c sends to the renderer while the benchmark's first run draws a frame, replays them into
                           ref_rdp_*, and lets gfx_host.c compare the two colour-image hashes and times
  - src/render/f3dex2.c    its four calls into the renderer go through the recorder (macros after the #include of rdp.h)
  - src/render/gfx_host.c  the benchmark calls ab_start / ab_stop / ab_replay_ref and prints a "gfx-ab:" line: IDENTICAL or DIFFERENT, and the speed-up
Only the colour image is compared (the z-buffer is not).
"""
import argparse, os, shutil, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
PUBLIC = ["rdp_reset", "rdp_command", "rdp_set_other_modes", "rdp_texture_rectangle", "rdp_triangle", "rdp_full_syncs", "rdp_color_image_addr",
          "rdp_report_unimplemented", "rdp_profile_report", "rdp_stats", "rdp_dev_color_image_hash", "host_rdp_trace"]

GFX_AB_C = r'''/*
 * gfx_ab.c - DEVELOPMENT ONLY (see tests/native/render/make_rdp_ab_tree.py; not part of the shipped program).
 *
 * Records the RDP commands that f3dex2.c sends while a display list runs (f3dex2.c is patched to call ab_rdp_* instead of rdp_*), and replays
 * the recording into rdp_ref.c, the trusted copy of the renderer from before the speed-up work. The picture the reference draws must be
 * identical, byte for byte, to the one the working renderer drew from the same commands - on a REAL game frame, in the same process.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "rdp.h"

/* the reference renderer (rdp_ref.c) */
void ref_rdp_reset(void);
void ref_rdp_command(uint32_t hi, uint32_t lo);
void ref_rdp_set_other_modes(uint32_t hi, uint32_t lo);
void ref_rdp_texture_rectangle(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, int flip);
void ref_rdp_triangle(const RdpVert *a, const RdpVert *b, const RdpVert *c, int tile, int level, int has_shade, int has_tex, int has_z);
uint64_t ref_rdp_dev_color_image_hash(void);

enum { EV_CMD, EV_MODES, EV_RECT, EV_TRI };
typedef struct Ev {
    int kind;
    uint32_t a, b, c, d;
    int flip, tile, level, shade, tex, z;
    RdpVert v[3];
} Ev;
static Ev *sEv;
static size_t sN, sCap;
static int sRec;

static Ev *push(int kind)
{
    Ev *e;
    if (!sRec) return NULL;
    if (sN == sCap) {
        sCap = sCap ? sCap * 2 : 65536;
        sEv = (Ev *)realloc(sEv, sCap * sizeof *sEv);
        if (!sEv) { fprintf(stderr, "gfx_ab: out of memory\n"); exit(2); }
    }
    e = &sEv[sN++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    return e;
}

void ab_start(void) { sN = 0; sRec = 1; }
void ab_stop(void) { sRec = 0; }

void ab_rdp_command(uint32_t hi, uint32_t lo)
{
    Ev *e = push(EV_CMD);
    if (e) { e->a = hi; e->b = lo; }
    rdp_command(hi, lo);
}
void ab_rdp_set_other_modes(uint32_t hi, uint32_t lo)
{
    Ev *e = push(EV_MODES);
    if (e) { e->a = hi; e->b = lo; }
    rdp_set_other_modes(hi, lo);
}
void ab_rdp_texture_rectangle(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, int flip)
{
    Ev *e = push(EV_RECT);
    if (e) { e->a = w0; e->b = w1; e->c = half1; e->d = half2; e->flip = flip; }
    rdp_texture_rectangle(w0, w1, half1, half2, flip);
}
void ab_rdp_triangle(const RdpVert *a, const RdpVert *b, const RdpVert *c, int tile, int level, int has_shade, int has_tex, int has_z)
{
    Ev *e = push(EV_TRI);
    if (e) { e->v[0] = *a; e->v[1] = *b; e->v[2] = *c; e->tile = tile; e->level = level; e->shade = has_shade; e->tex = has_tex; e->z = has_z; }
    rdp_triangle(a, b, c, tile, level, has_shade, has_tex, has_z);
}

/* Replays the recording into the reference `reps` times (each from the power-up state). Returns the colour-image hash after the last replay;
 * *fastest_ms receives the fastest replay's time. Returns 0 and *n_events = 0 if nothing was recorded. */
uint64_t ab_replay_ref(int reps, double *fastest_ms, size_t *n_events)
{
    uint64_t h = 0;
    int r;
    size_t i;
    *fastest_ms = 1e30;
    *n_events = sN;
    for (r = 0; r < reps; r++) {
        unsigned long long t0;
        double ms;
        ref_rdp_reset();
        t0 = host_now_ns();
        for (i = 0; i < sN; i++) {
            const Ev *e = &sEv[i];
            switch (e->kind) {
            case EV_CMD:   ref_rdp_command(e->a, e->b); break;
            case EV_MODES: ref_rdp_set_other_modes(e->a, e->b); break;
            case EV_RECT:  ref_rdp_texture_rectangle(e->a, e->b, e->c, e->d, e->flip); break;
            default:       ref_rdp_triangle(&e->v[0], &e->v[1], &e->v[2], e->tile, e->level, e->shade, e->tex, e->z); break;
            }
        }
        ms = (double)(host_now_ns() - t0) / 1e6;
        if (ms < *fastest_ms) *fastest_ms = ms;
        h = ref_rdp_dev_color_image_hash();
    }
    return h;
}
'''


def replace_once(text, old, new, what):
    if text.count(old) != 1:
        sys.exit("make_rdp_ab_tree: cannot find the place to patch in %s (the source changed; update this script)" % what)
    return text.replace(old, new, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref-commit", required=True, help="git commit whose src/render/rdp.c is the trusted reference")
    ap.add_argument("--out", required=True, help="directory to create (must not exist)")
    a = ap.parse_args()
    if os.path.exists(a.out):
        sys.exit("make_rdp_ab_tree: %s exists" % a.out)
    shutil.copytree(ROOT, a.out, ignore=shutil.ignore_patterns(".git"))
    ref = subprocess.run(["git", "-C", ROOT, "show", a.ref_commit + ":src/render/rdp.c"], capture_output=True, text=True, check=True).stdout
    cur = open(os.path.join(ROOT, "src", "render", "rdp.c")).read()
    i = cur.index("/* Development (KYUJO_GFX_BENCH")
    hashfn = cur[i:cur.index("\n}\n", i) + 3]
    if "rdp_dev_color_image_hash" not in ref:
        ref += "\n" + hashfn                                    # the reference predates the hash function: append the (trivial) one
    pre = "/* REFERENCE COPY of src/render/rdp.c from commit %s, public names prefixed ref_ (development only: make_rdp_ab_tree.py). */\n" % a.ref_commit
    pre += "".join("#define %s ref_%s\n" % (n, n) for n in PUBLIC)
    open(os.path.join(a.out, "src", "render", "rdp_ref.c"), "w").write(pre + ref)
    open(os.path.join(a.out, "src", "render", "gfx_ab.c"), "w").write(GFX_AB_C)

    p = os.path.join(a.out, "src", "render", "f3dex2.c")
    t = open(p).read()
    t = replace_once(t, '#include "rdp.h"\n', '''#include "rdp.h"
/* DEV ONLY (make_rdp_ab_tree.py): record what is sent to the renderer. */
void ab_rdp_command(uint32_t, uint32_t);
void ab_rdp_set_other_modes(uint32_t, uint32_t);
void ab_rdp_texture_rectangle(uint32_t, uint32_t, uint32_t, uint32_t, int);
void ab_rdp_triangle(const RdpVert *, const RdpVert *, const RdpVert *, int, int, int, int, int);
#define rdp_command ab_rdp_command
#define rdp_set_other_modes ab_rdp_set_other_modes
#define rdp_texture_rectangle ab_rdp_texture_rectangle
#define rdp_triangle ab_rdp_triangle
''', "f3dex2.c")
    open(p, "w").write(t)

    p = os.path.join(a.out, "src", "render", "gfx_host.c")
    t = open(p).read()
    t = replace_once(t, "#define BENCH_MAX_POINTS 16", "#define BENCH_MAX_POINTS 16\nvoid ab_start(void); void ab_stop(void); uint64_t ab_replay_ref(int, double *, size_t *);", "gfx_host.c")
    t = replace_once(t, "    rdp_reset();                        /* every run starts", "    ab_start();                         /* DEV ONLY: record the commands of the first run */\n    rdp_reset();                        /* every run starts", "gfx_host.c")
    t = replace_once(t, "        rdp_stats(&tri1, &rect1, &pix1);\n        rdp_profile_report(ns[i]);", "        ab_stop();                      /* only the first run is recorded */\n        rdp_stats(&tri1, &rect1, &pix1);\n        rdp_profile_report(ns[i]);", "gfx_host.c")
    t = replace_once(t, "    free(ns);\n}\n\n/* Called at every task start", '''    {   /* DEV ONLY: the same commands through the reference renderer; the picture must be identical, and the times give the speed-up on this frame */
        double ref_ms = 0;
        size_t nev = 0;
        uint64_t ref_hash = ab_replay_ref(reps < 10 ? reps : 10, &ref_ms, &nev);
        fprintf(stderr, "gfx-ab: %llu commands recorded; reference renderer fastest %.2f ms (working renderer fastest %.2f ms: %.2fx); reference picture hash %016llX -> %s\\n",
                (unsigned long long)nev, ref_ms, (double)ns[0] / 1e6, ref_ms / ((double)ns[0] / 1e6), (unsigned long long)ref_hash,
                ref_hash == (uint64_t)last_hash && ref_hash != 0 ? "IDENTICAL" : "DIFFERENT !!!");
    }
    free(ns);
}

/* Called at every task start''', "gfx_host.c")
    open(p, "w").write(t)
    print("made %s; build it with its own tools/native/build_native.py" % a.out)


if __name__ == "__main__":
    main()
