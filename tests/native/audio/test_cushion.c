/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * test_cushion.c - checks the sound-device cushion of src/platform/host_audio.c (the Windows waveOut path) on Linux.
 *
 * host_audio.c is compiled with -D_WIN32 against fake <windows.h>/<mmsystem.h> headers (tests/native/audio/fake) whose waveOut functions
 * are simulated below: a device that plays the queued buffers one after the other in real (simulated) time at 48000 Hz and marks each
 * header WHDR_DONE when it has played. The test then feeds host_audio_buffer() the way the game does (one buffer per 1/60 s) with hold-ups,
 * and measures how long the simulated device sat idle after it had started ("audible gap").
 *
 *   test_cushion <cushion_ms> <scenario> [start_cushion_ms]
 *                       scenario 1: steady, then one 50 ms hold-up (at 5 s)     scenario 2: steady, then one 200 ms hold-up (at 5 s)
 *                       scenario 3: one 150 ms hold-up 0.3 s after the start (the start-up stall of the Windows "Pika" voice)
 *                       scenario 4: steady, nothing else (for the queue length at the end: the start-up cushion is run down)
 *
 * It prints "gap_ms=<n> silence_buffers=<n> buffers=<n>"; tests/native/audio/run_cushion_test.py checks the numbers.
 * This proves the queueing logic only. That the real Windows sound device behaves as the simulation (e.g. WHDR_DONE timing) is NOT proven here.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include "windows.h"
#include "host_ui.h"

void host_audio_buffer(unsigned int addr, unsigned int len, unsigned int rate);

/* host_audio.c reads the volume setting (menu: Options); the test sets it with TEST_VOLUME (percent, default 100). */
HostSettings *host_settings(void)
{
    static HostSettings s;
    const char *v = getenv("TEST_VOLUME");
    s.volume = v ? atoi(v) : 100;
    return &s;
}

/* ---- the simulated device ---------------------------------------------------------------------------------------------------- */
#define MAXQ 4096
static double sNow;                       /* simulated time, ms */
static struct { WAVEHDR *h; double end; } sQ[MAXQ];
static int sQn;
static double sPlayEnd = -1;              /* time at which the device finishes everything queued so far (-1: nothing played yet) */
static double sIdle;                      /* ms the device sat idle after it had started */
static int sBuffers, sSilence;
static unsigned sSilenceBytes;
static int sPeak;                         /* the largest 16-bit sample (absolute value) that was queued */

/* game variables that host_audio.c's stream dump (development aid) reads */
unsigned char D_800FCEF0[2] = {2, 2};
unsigned D_800FCF28[2], D_800FCF30[2], D_800FCF38[2];
DWORD GetTickCount(void) { return (DWORD)sNow; }
unsigned long long host_wall_ns(void) { return (unsigned long long)(sNow * 1e6); }
HANDLE CreateEventA(void *a, BOOL m, BOOL i, const char *n) { (void)a; (void)m; (void)i; (void)n; return (HANDLE)1; }
HANDLE CreateThread(void *a, size_t s, DWORD (WINAPI *fn)(LPVOID), LPVOID arg, DWORD f, DWORD *id) { (void)a; (void)s; (void)fn; (void)arg; (void)f; (void)id; return NULL; }
DWORD WaitForSingleObject(HANDLE h, DWORD ms) { (void)h; (void)ms; return 0; }
BOOL SetEvent(HANDLE h) { (void)h; return 1; }
BOOL CloseHandle(HANDLE h) { (void)h; return 1; }
MMRESULT waveOutOpen(HWAVEOUT *h, unsigned id, const WAVEFORMATEX *f, DWORD a, DWORD b, DWORD c) { (void)id; (void)f; (void)a; (void)b; (void)c; *h = (HWAVEOUT)1; return 0; }
MMRESULT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *w, unsigned size) { (void)h; (void)size; w->dwFlags &= ~(DWORD)WHDR_DONE; return 0; }
MMRESULT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *w, unsigned size) { (void)h; (void)w; (void)size; return 0; }
MMRESULT waveOutGetErrorTextA(MMRESULT r, char *t, unsigned n) { (void)r; snprintf(t, n, "fake"); return 0; }
MMRESULT waveOutWrite(HWAVEOUT h, WAVEHDR *w, unsigned size)
{
    double start, ms = (double)w->dwBufferLength / (48000.0 * 4.0) * 1000.0;
    int allzero = 1;
    unsigned i;
    (void)h; (void)size;
    for (i = 0; i < w->dwBufferLength; i++) if (w->lpData[i]) { allzero = 0; break; }
    for (i = 0; i + 1 < w->dwBufferLength; i += 2) {
        int v = (short)((unsigned char)w->lpData[i] | ((unsigned char)w->lpData[i + 1] << 8));
        if (v < 0) v = -v;
        if (v > sPeak) sPeak = v;
    }
    if (sPlayEnd < 0) start = sNow;                         /* first buffer: the device starts now */
    else if (sPlayEnd <= sNow) { sIdle += sNow - sPlayEnd; start = sNow; }     /* it had run dry: idle until now */
    else start = sPlayEnd;
    sPlayEnd = start + ms;
    if (sQn == MAXQ) { fprintf(stderr, "test: too many buffers\n"); exit(3); }
    sQ[sQn].h = w; sQ[sQn].end = sPlayEnd; sQn++;
    sBuffers++;
    if (allzero) { sSilence++; sSilenceBytes += w->dwBufferLength; }
    return 0;
}
/* the device has played what is due by `now`: mark those headers done */
static void device_advance(void)
{
    int i;
    for (i = 0; i < sQn; i++) if (sQ[i].h && sQ[i].end <= sNow) { sQ[i].h->dwFlags |= WHDR_DONE; sQ[i].h = NULL; }
}

/* ---- the game side ------------------------------------------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const double frame_ms = 1000.0 / 60.0;
    const unsigned rate = 32000, frames = 533;              /* one field of audio at 32 kHz */
    unsigned char *buf;
    int n, scenario;
    double holdup_ms;

    if (argc < 3) { fprintf(stderr, "usage: test_cushion <cushion_ms> <scenario 1..4> [start_cushion_ms]\n"); return 2; }
    setenv("KYUJO_AUDIO_CUSHION_MS", argv[1], 1);
    setenv("KYUJO_AUDIO_START_CUSHION_MS", argc > 3 ? argv[3] : argv[1], 1);
    scenario = atoi(argv[2]);
    holdup_ms = scenario == 1 ? 50.0 : scenario == 3 ? 150.0 : scenario == 4 ? 0.0 : 200.0;
    /* the game passes 32-bit addresses: allocate below 2 GB */
    buf = (unsigned char *)mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); return 2; }
    for (n = 0; n < (int)(frames * 4); n += 4) { buf[n] = 0x10; buf[n + 1] = 0x00; buf[n + 2] = 0x10; buf[n + 3] = 0x00; }   /* non-zero samples, big-endian */

    for (n = 0; n < (scenario == 4 ? 1800 : 600); n++) {    /* 10 s of game time (30 s for scenario 4) */
        sNow += frame_ms;
        if (n == (scenario == 3 ? 18 : 300)) sNow += holdup_ms;                    /* the game thread was busy: this buffer is late */
        device_advance();
        host_audio_buffer((unsigned)(uintptr_t)buf, frames * 4, rate);
    }
    printf("gap_ms=%.1f silence_buffers=%d buffers=%d peak=%d queue_ms=%.1f\n", sIdle, sSilence, sBuffers, sPeak, sPlayEnd > sNow ? sPlayEnd - sNow : 0.0);
    return 0;
}
