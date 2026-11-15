/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_ubsan.c - report-and-continue handler for a development build that checks array indexing (see PORT_EXTRA_CFLAGS in
 * tools/native/targets.py). In a normal build nothing calls it.
 *
 * Build:  PORT_EXTRA_CFLAGS="-fsanitize=array-bounds -fsanitize-recover=array-bounds" build_native.py ... --out <separate dir>
 * Then every index past the DECLARED size of an array makes the game call __ubsan_handle_out_of_bounds(data, index) instead of stopping; this
 * file prints the source line once per site (and again at the 2nd, 4th, 8th ... hit with the count) and lets the game go on.
 *
 * What a report means. The console's linker and this port's linker place separate variables differently, so an access past the end of one array
 * reads or writes DIFFERENT neighbours here than on the console ("layout accident", docs/known-decomp-issues.md). Reports where the array is a
 * member of a larger structure are NOT accidents of that kind: the structure's layout is the same on both, so the neighbour is the same field
 * (these show a decomp declaration that is too small, harmless for the port). A report on a stand-alone array needs a look.
 *
 * Byte order: the report's data record lives in game memory, whose initialised data is held big-endian (tools/native/be_memory.py), so its
 * 32-bit fields are read with a byte swap. The file name text is plain bytes.
 * HOST-side file.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SITES 8192
/* The table is allocated on the FIRST report, not declared as a static array: this file is part of every build, and a 128 KiB static table
 * would have used up room that the program image needs (the image must stay below 16 MiB with 2 MiB to spare, tools/native/imagelimit.py;
 * check_build.sh caught exactly that). Normal builds never call the handler, so they never allocate it. */
static struct Site { uint32_t key; unsigned long long hits; } *sSite;

static uint32_t be32(const uint32_t *p) { uint32_t v = *p; return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }

void __ubsan_handle_out_of_bounds(void *data, uintptr_t index)
{
    const uint32_t *d = (const uint32_t *)data;                  /* { { const char *file; u32 line; u32 column; }, array type, index type } */
    uint32_t key = (uint32_t)(uintptr_t)data, h = (key >> 2) % SITES;
    unsigned probes = 0;
    if (!sSite && !(sSite = calloc(SITES, sizeof *sSite))) return;      /* out of memory: stay silent rather than stop the game */
    while (sSite[h].key != 0 && sSite[h].key != key && probes++ < SITES) h = (h + 1) % SITES;
    if (sSite[h].key == 0) sSite[h].key = key;
    sSite[h].hits++;
    if ((sSite[h].hits & (sSite[h].hits - 1)) == 0) {            /* 1st, 2nd, 4th, 8th ... hit */
        const char *file = (const char *)(uintptr_t)be32(d);
        const char *type = (const char *)(uintptr_t)be32(d + 3);   /* type descriptor: u16 kind, u16 info, then the type's name as text */
        fprintf(stderr, "ubsan: array index out of bounds at %s:%u:%u (index %ld; array type %s; hit %llu)\n", file ? file : "?", (unsigned)be32(d + 1),
                (unsigned)be32(d + 2), (long)(intptr_t)index, type ? type + 4 : "?", sSite[h].hits);
    }
}
