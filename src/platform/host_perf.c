/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_perf.c - a tiny speed monitor: where does the wall-clock time go?
 *
 * WHY: the first Windows tester reported lag (laggy sound in menus and overworld, a ~2 minute "loading" in the Game Boy Tower, a laggy
 * first battle). Nothing in the program measured speed, so the numbers are needed before anything can be said about the cause.
 *
 * WHAT: callers wrap a piece of work with host_now_ns() and report its duration with host_perf_add(kind, ns). Every PERF_PERIOD_NS of
 * wall-clock time one line is written to stderr (which is kyujo-log.txt on Windows):
 *
 *   kyujo: perf: 5.0 s  fields 300 (60.0/s)  gfx 120 tasks 14.2 ms avg (34% of time)  picture 60 x 3.1 ms (4%)  rsp-audio ...
 *
 *   fields   = video retraces the game handled (the console runs 60 per second; fewer means the game is running slower than real time)
 *   gfx      = display lists run (F3DEX2 interpreter + software RDP) - number, average duration, share of the wall-clock time
 *   picture  = frame-buffer -> RGB conversion and window hand-over, per presented picture
 *   audio    = the RSP interpreter running the audio microcode (or the Game Boy Tower's microcodes)
 *   mips     = the MIPS interpreter running the Game Boy Tower's hand-written assembly (outermost calls only)
 *   jpeg     = the JPEG task
 *   audio-gap = silent gaps of the audio interface: the playing buffer ended before the game supplied the next one (number, average
 *              length, share of the time). Each gap is a click or a stutter in the sound.
 * A share is (time spent in that kind) / (wall-clock time of the period). Kinds run on different host threads can overlap in time, but the
 * game runs one thread at a time (the "baton", see host_sched.c), so shares of the game's kinds add up to at most about 100%.
 *
 * Thread safety: all reports come from the thread holding the baton, except the picture hand-over, which is also on that thread.
 * HOST-side file.
 */
#define _GNU_SOURCE                      /* REG_EIP in <ucontext.h> (the development profiler below) */
#include <stdio.h>
#include <string.h>

#include "platform.h"

#define PERF_PERIOD_NS 5000000000ULL

unsigned long long HostPifPakAccesses(void);                /* hw_pif.c: Transfer Pak reads and writes so far */

static unsigned long long sSum[PERF_KINDS];
static unsigned long long sMax[PERF_KINDS];                 /* the longest single piece of work of each kind in the period */
static unsigned sCount[PERF_KINDS];
static unsigned long long sPeriodStart;
static unsigned long long sLastField, sFieldGapMax;         /* wall-clock distance between two video fields the game handled */
static unsigned sFieldLate;                                 /* fields that came more than 25 ms after the one before (a field is 16.7 ms) */

#ifdef _WIN32
int host_window_paint_stats(unsigned *count, unsigned *sum_us, unsigned *max_us, unsigned *poll_max_us);     /* host_window.c: reads and resets the window thread's paint timings */
#else
static int host_window_paint_stats(unsigned *count, unsigned *sum_us, unsigned *max_us, unsigned *poll_max_us) { (void)count; (void)sum_us; (void)max_us; (void)poll_max_us; return 0; }
#endif

void host_perf_add(int kind, unsigned long long ns)
{
    unsigned long long now = host_now_ns();
    if (kind >= 0 && kind < PERF_KINDS) {
        sSum[kind] += ns;
        sCount[kind]++;
        if (ns > sMax[kind]) sMax[kind] = ns;
        if (kind == PERF_FIELDS) {
            if (sLastField) {
                unsigned long long gap = now - sLastField;
                if (gap > sFieldGapMax) sFieldGapMax = gap;
                if (gap > 25000000ULL) sFieldLate++;
            }
            sLastField = now;
        }
    }
    if (sPeriodStart == 0) sPeriodStart = now;
    if (now - sPeriodStart >= PERF_PERIOD_NS) {
        double secs = (double)(now - sPeriodStart) / 1e9;
        static const char *const name[PERF_KINDS] = { "fields", "gfx", "picture", "audio", "mips", "jpeg", "audio-gap", "gfx-wait", "mips-yield", "mips-native", "mips-kbranch" };
        char line[720];
        size_t n = 0;
        int k;
        n += (size_t)snprintf(line + n, sizeof line - n, "kyujo: perf: %.1f s ", secs);
        for (k = 0; k < PERF_KINDS && n < sizeof line - 80; k++) {
            if (k == PERF_FIELDS) {
                n += (size_t)snprintf(line + n, sizeof line - n, " fields %u (%.1f/s)", sCount[k], (double)sCount[k] / secs);
            } else if (sCount[k]) {
                n += (size_t)snprintf(line + n, sizeof line - n, "  %s %ux %.1f ms avg (%.0f%%)", name[k], sCount[k],
                                      (double)sSum[k] / 1e6 / (double)sCount[k], 100.0 * (double)sSum[k] / (double)(now - sPeriodStart));
                if (k == PERF_GFX || k == PERF_PICTURE) n += (size_t)snprintf(line + n, sizeof line - n, " max %.0f", (double)sMax[k] / 1e6);
            }
        }
        /* the smoothness numbers: the longest wait between two fields and how many fields were late; on Windows also the window thread's paints */
        n += (size_t)snprintf(line + n, sizeof line - n, "  field-gap max %.0f ms late %u", (double)sFieldGapMax / 1e6, sFieldLate);
        {
            unsigned pc = 0, ps = 0, pm = 0, pl = 0;
            if (host_window_paint_stats(&pc, &ps, &pm, &pl) && pc) n += (size_t)snprintf(line + n, sizeof line - n, "  paint %ux %.1f ms avg max %.0f  pad-poll max %.0f", pc, (double)ps / 1e3 / (double)pc, (double)pm / 1e3, (double)pl / 1e3);
        }
        if (HostPifPakAccesses()) snprintf(line + n, sizeof line - n, "  pak-accesses %llu", HostPifPakAccesses());   /* running total: the loading of a Game Boy cartridge is made of these */
        fprintf(stderr, "%s\n", line);
        memset(sSum, 0, sizeof sSum);
        memset(sMax, 0, sizeof sMax);
        memset(sCount, 0, sizeof sCount);
        sFieldGapMax = 0;
        sFieldLate = 0;
        sPeriodStart = now;
    }
}

/* ------------------------------------------------------------------------------------------------------------------------ */
/* Development sampling profiler (Linux only): KYUJO_PROFILE=<file> writes the instruction address that was executing at each  */
/* SIGPROF tick (every 2 ms of CPU time) as raw 32-bit values. tools/native/profile_report.py turns the file into a per-function    */
/* table with the program's symbol table. Not available on Windows. The program is not position independent, so addresses are final. */
/* KYUJO_PROFILE_FROM=<seconds> drops the ticks of the first <seconds> of wall-clock time (to profile only the battle).          */
/* ------------------------------------------------------------------------------------------------------------------------ */
#if !defined(_WIN32) && !defined(__wasm__)
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <ucontext.h>
#include <time.h>

#define PROF_BUF 2048
static unsigned sProfBuf[PROF_BUF];
static volatile unsigned sProfN;
static int sProfFd = -1;

static long sProfSkipSeconds;                 /* KYUJO_PROFILE_FROM=<seconds>: ignore the ticks of the first part of the run */
static long sProfStartSec;

static void prof_tick(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    (void)sig; (void)si;
    if (sProfSkipSeconds) {                    /* clock_gettime is async-signal-safe */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_sec - sProfStartSec < sProfSkipSeconds) return;
    }
    sProfBuf[sProfN++] = (unsigned)uc->uc_mcontext.gregs[REG_EIP];
    if (sProfN == PROF_BUF) {                      /* write() is async-signal-safe; the buffer is flushed whole */
        ssize_t r = write(sProfFd, sProfBuf, sizeof sProfBuf);
        (void)r;
        sProfN = 0;
    }
}

void host_perf_start_profiler(void)
{
    const char *path = getenv("KYUJO_PROFILE");
    struct sigaction sa;
    struct itimerval it;
    if (!path) return;
    if (getenv("KYUJO_PROFILE_FROM")) {      /* lets one run profile only the battle, which starts about 4 minutes in */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        sProfStartSec = ts.tv_sec;
        sProfSkipSeconds = atol(getenv("KYUJO_PROFILE_FROM"));
    }
    sProfFd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sProfFd < 0) { fprintf(stderr, "kyujo: cannot open %s for the profile\n", path); return; }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_tick;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 2000;
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, NULL);
    fprintf(stderr, "kyujo: profiling to %s (development)\n", path);
}
#else
void host_perf_start_profiler(void) { }
#endif
