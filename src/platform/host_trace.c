/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_trace.c - development call trace of the GAME code (move animation debugging).
 *
 * How it works: a build made with PORT_EXTRA_CFLAGS=-finstrument-functions (see docs/move-trace.md; it needs its own --out directory) makes clang
 * call __cyg_profile_func_enter(function, caller) at the start of every GAME function (decomp C files). This file receives those calls. Nothing
 * here runs in a normal build: the hooks are only called by instrumented code, and when no trace is open they return at once.
 *
 * Starting and stopping:
 *   - KYUJO_TRACE=<file>   opens the trace when the program starts (everything is recorded until it exits)
 *   - the control channel (host_control.c): `trace start <file>` / `trace stop`; a `mark <text>` line writes a labelled line into the trace
 *   - host_trace_start / host_trace_stop / host_trace_mark below, for the move tester
 *
 * What is written (text, one record per line):
 *   - "F<frame> T<thread> +<depth> <function>"  the FIRST call of each function in this trace (what ran, in what order);
 *     with KYUJO_TRACE_FULL=1 EVERY call is written instead (large: millions of lines; capped at KYUJO_TRACE_MAX lines, default 5 million).
 *   - "MARK F<frame> <text>"  from host_trace_mark
 *   - at the end a summary: one line per function, "calls=<n> first=F<frame> last=F<frame> <function>", in order of first call.
 * <frame> is host_gfx_frame_count(): the number of pictures shown so far. Function names come from "kyujo.syms" (written next to the program
 * by tools/native/build_native.py: "<hex address> <name>" lines, sorted); without that file the address is written as 0x<hex>.
 *
 * Threads: the game may run on several threads. The counters are protected by a tiny spin lock; the depth is per thread (it is only a hint).
 * HOST-side file: never instrumented itself (every function carries no_instrument_function).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#ifndef _WIN32
#include <unistd.h>
#endif

#define NOINST __attribute__((no_instrument_function))

/* ---- symbol table ------------------------------------------------------------------------------------------------------------ */
typedef struct { unsigned addr; const char *name; } Sym;
static Sym *sSyms;
static unsigned sNumSyms;

static NOINST int sym_cmp(const void *a, const void *b)
{
    unsigned x = ((const Sym *)a)->addr, y = ((const Sym *)b)->addr;
    return x < y ? -1 : x > y;
}

/* Reads "<hex address> <name>" lines. Returns the number of symbols (0 if the file is missing: addresses are then written as numbers). */
static NOINST unsigned load_syms(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    unsigned cap = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *end;
        unsigned long a = strtoul(line, &end, 16);
        size_t len;
        if (end == line || *end != ' ') continue;
        end++;
        len = strcspn(end, "\r\n");
        if (len == 0) continue;
        if (sNumSyms == cap) {
            cap = cap ? cap * 2 : 4096;
            sSyms = (Sym *)realloc(sSyms, cap * sizeof(Sym));
            if (!sSyms) { sNumSyms = 0; fclose(f); return 0; }
        }
        sSyms[sNumSyms].addr = (unsigned)a;
        {   /* (not strndup: the Windows C library has none) */
            char *copy = (char *)malloc(len + 1);
            if (copy) { memcpy(copy, end, len); copy[len] = 0; }
            sSyms[sNumSyms].name = copy ? copy : "?";
        }
        sNumSyms++;
    }
    fclose(f);
    qsort(sSyms, sNumSyms, sizeof(Sym), sym_cmp);
    return sNumSyms;
}

/* The symbol that starts exactly at `addr` (function entry points are exact), or NULL. */
static NOINST const char *sym_name(unsigned addr)
{
    unsigned lo = 0, hi = sNumSyms;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (sSyms[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return (lo < sNumSyms && sSyms[lo].addr == addr) ? sSyms[lo].name : NULL;
}

/* ---- per-function counters (open-addressing hash on the function address) ------------------------------------------------------- */
#define TRACE_SLOTS 16384                         /* a power of two; the game has about 8 800 functions */
typedef struct { unsigned addr, calls, first, last, order; } Slot;
static Slot *sSlot;                               /* allocated when a trace starts: a static table would enlarge every program's image (imagelimit.py) */
static unsigned sOrder;                           /* how many different functions were seen so far */

static volatile int sOpen;                        /* 1 while a trace is open */
static FILE *sOut;
static int sFull;                                 /* write every call */
static unsigned long sMaxLines = 5000000UL, sLines;
static char sLock;                                /* spin lock (__atomic test-and-set) */
static __thread int tDepth;
static __thread int tThreadId;                    /* 0 = not yet numbered */
static int sThreads;

static NOINST void lock(void)   { while (__atomic_test_and_set(&sLock, __ATOMIC_ACQUIRE)) { } }
static NOINST void unlock(void) { __atomic_clear(&sLock, __ATOMIC_RELEASE); }

static NOINST void write_name(FILE *f, unsigned addr)
{
    const char *n = sym_name(addr);
    if (n) fputs(n, f); else fprintf(f, "0x%x", addr);
}

NOINST void __cyg_profile_func_enter(void *fn, void *caller)
{
    unsigned addr = (unsigned)(unsigned long)fn, h, k;
    Slot *s;
    (void)caller;
    if (!sOpen) return;
    if (!tThreadId) tThreadId = __atomic_add_fetch(&sThreads, 1, __ATOMIC_RELAXED);
    tDepth++;
    lock();
    if (!sOpen) { unlock(); return; }
    h = (addr >> 2) * 2654435761u;
    for (k = 0; k < TRACE_SLOTS; k++) {
        s = &sSlot[(h + k) & (TRACE_SLOTS - 1)];
        if (s->addr == addr || s->addr == 0) break;
    }
    if (k == TRACE_SLOTS) { unlock(); return; }   /* table full: cannot happen with ~8 000 functions */
    if (s->addr == 0) {                           /* first call of this function in this trace */
        s->addr = addr; s->first = host_gfx_frame_count(); s->order = ++sOrder;
        if (!sFull) {
            fprintf(sOut, "F%u T%d +%d ", s->first, tThreadId, tDepth);
            write_name(sOut, addr);
            fputc('\n', sOut);
            sLines++;
        }
    }
    s->calls++; s->last = host_gfx_frame_count();
    if (sFull && sLines < sMaxLines) {
        fprintf(sOut, "F%u T%d +%d ", s->last, tThreadId, tDepth);
        write_name(sOut, addr);
        fputc('\n', sOut);
        sLines++;
    }
    unlock();
}

NOINST void __cyg_profile_func_exit(void *fn, void *caller)
{
    (void)fn; (void)caller;
    if (tDepth > 0) tDepth--;
}

/* ---- control ------------------------------------------------------------------------------------------------------------------- */
static NOINST int cmp_order(const void *a, const void *b)
{
    unsigned x = ((const Slot *)a)->order, y = ((const Slot *)b)->order;
    return x < y ? -1 : x > y;
}

/* Opens a trace. Returns 0 on success. The symbol file is "<syms>" if given, else KYUJO_SYMS, else "kyujo.syms" in the program's folder. */
NOINST int host_trace_start(const char *path, const char *syms)
{
    const char *e;
    if (sOpen) return -1;
    sOut = fopen(path, "w");
    if (!sOut) { fprintf(stderr, "kyujo: trace: cannot create '%s'\n", path); return -1; }
    if (!sNumSyms) {
        /* Where the symbol file may be, in this order: the argument, KYUJO_SYMS, the settings folder, the folder of the program. */
        char app[1024], exe[1024];
        const char *cand[4];
        unsigned i, nc = 0;
        app[0] = exe[0] = 0;
        if (syms) cand[nc++] = syms;
        if (getenv("KYUJO_SYMS")) cand[nc++] = getenv("KYUJO_SYMS");
        host_app_path(app, sizeof app, "kyujo.syms");
        cand[nc++] = app;
#ifndef _WIN32
        {
            ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 8);
            if (n > 0) { exe[n] = 0; strcat(exe, ".syms"); cand[nc++] = exe; }
        }
#endif
        for (i = 0; i < nc && !load_syms(cand[i]); i++) { }
        if (i < nc) fprintf(stderr, "kyujo: trace: %u symbols from %s\n", sNumSyms, cand[i]);
        else fprintf(stderr, "kyujo: trace: no symbol file found (tried %u places); functions will be written as addresses\n", nc);
    }
    e = getenv("KYUJO_TRACE_FULL"); sFull = e && e[0] == '1';
    e = getenv("KYUJO_TRACE_MAX");  if (e && atol(e) > 0) sMaxLines = (unsigned long)atol(e);
    free(sSlot);
    sSlot = (Slot *)calloc(TRACE_SLOTS, sizeof(Slot));
    if (!sSlot) { fprintf(stderr, "kyujo: trace: out of memory\n"); fclose(sOut); sOut = NULL; return -1; }
    sOrder = 0; sLines = 0;
    fprintf(sOut, "# kyujo call trace (%s). F = pictures shown so far, T = game thread, + = call depth\n", sFull ? "every call" : "first call of each function");
    sOpen = 1;
    fprintf(stderr, "kyujo: trace: recording to %s (%s)\n", path, sFull ? "every call" : "first calls + summary");
    return 0;
}

NOINST void host_trace_mark(const char *text)
{
    if (!sOpen) return;
    lock();
    if (sOut) fprintf(sOut, "MARK F%u %s\n", host_gfx_frame_count(), text);
    unlock();
}

NOINST void host_trace_stop(void)
{
    unsigned i, n = 0;
    Slot *sorted;
    if (!sOpen) return;
    lock();
    sOpen = 0;
    sorted = (Slot *)malloc(TRACE_SLOTS * sizeof(Slot));
    if (!sorted) { fclose(sOut); sOut = NULL; unlock(); return; }
    for (i = 0; i < TRACE_SLOTS; i++) if (sSlot[i].addr) sorted[n++] = sSlot[i];
    qsort(sorted, n, sizeof(Slot), cmp_order);
    fprintf(sOut, "# summary: %u different functions, in order of first call\n", n);
    for (i = 0; i < n; i++) {
        fprintf(sOut, "calls=%u first=F%u last=F%u ", sorted[i].calls, sorted[i].first, sorted[i].last);
        write_name(sOut, sorted[i].addr);
        fputc('\n', sOut);
    }
    if (n == 0) fprintf(sOut, "# no call was recorded: this program was not built with --trace-calls (docs/move-trace.md), or nothing instrumented ran\n");
    if (sFull && sLines >= sMaxLines) fprintf(sOut, "# the full call list was cut at %lu lines (KYUJO_TRACE_MAX)\n", sMaxLines);
    free(sorted);
    fclose(sOut);
    sOut = NULL;
    unlock();
    fprintf(stderr, "kyujo: trace: closed, %u functions\n", n);
}

/* KYUJO_TRACE=<file>: record from the start of the program until it exits. */
NOINST void host_trace_init(void)
{
    const char *p = getenv("KYUJO_TRACE");
    if (p && p[0]) {
        host_trace_start(p, NULL);
        atexit(host_trace_stop);
    }
}
