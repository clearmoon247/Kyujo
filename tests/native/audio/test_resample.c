/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * test_resample.c - checks the sample-rate conversion of src/platform/host_audio.c (to_device_rate: the game's 32006 Hz to the device's 48000 Hz).
 * host_audio.c is included here (compiled with -D_WIN32 against the fake Windows headers) so that the static function can be called.
 * Prints one "name value" line per measurement; tests/native/audio/run_resample_test.py checks the numbers.
 *   gain_1k          level of a 1 kHz tone after conversion relative to before, in dB (must be about 0)
 *   gain_10k         the same at 10 kHz
 *   image_12k_db     a 12 kHz tone: level at its image frequency 20 kHz (32006 - 12000) relative to the tone, in dB (linear interpolation: about -17)
 *   split_diff       largest sample difference between converting a signal in one buffer and in 533-frame buffers (must be 0 or 1)
 *   clamp_ok         a full-scale square wave never wraps around (1) or does (0)
 *   rate_change_ok   switching the rate between buffers gives no jump larger than the signal's own slope (1/0)
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "windows.h"
#include "host_ui.h"
HostSettings *host_settings(void) { static HostSettings s; s.volume = 100; return &s; }
/* the sound device is not used here: stubs for the fake waveOut API */
DWORD GetTickCount(void) { return 0; }
unsigned long long host_wall_ns(void) { return 0; }
HANDLE CreateEventA(void *a, BOOL m, BOOL i, const char *n) { (void)a; (void)m; (void)i; (void)n; return NULL; }
HANDLE CreateThread(void *a, size_t s, DWORD (WINAPI *fn)(LPVOID), LPVOID arg, DWORD f, DWORD *id) { (void)a; (void)s; (void)fn; (void)arg; (void)f; (void)id; return NULL; }
DWORD WaitForSingleObject(HANDLE h, DWORD ms) { (void)h; (void)ms; return 0; }
BOOL SetEvent(HANDLE h) { (void)h; return 1; }
BOOL CloseHandle(HANDLE h) { (void)h; return 1; }
unsigned char D_800FCEF0[2] = {2, 2};                        /* game variables that the stream dump of host_audio.c reads */
unsigned D_800FCF28[2], D_800FCF30[2], D_800FCF38[2];
MMRESULT waveOutOpen(HWAVEOUT *h, unsigned id, const WAVEFORMATEX *f, DWORD a, DWORD b, DWORD c) { (void)h; (void)id; (void)f; (void)a; (void)b; (void)c; return 1; }
MMRESULT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *w, unsigned size) { (void)h; (void)w; (void)size; return 1; }
MMRESULT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *w, unsigned size) { (void)h; (void)w; (void)size; return 1; }
MMRESULT waveOutWrite(HWAVEOUT h, WAVEHDR *w, unsigned size) { (void)h; (void)w; (void)size; return 1; }
MMRESULT waveOutGetErrorTextA(MMRESULT r, char *text, unsigned size) { (void)r; (void)text; (void)size; return 1; }
#include "../../../src/platform/host_audio.c"

static double tone_level(const short *x, unsigned n, double f, double rate)    /* amplitude of frequency f in the left channel (stride 2), Goertzel-like */
{
    double re = 0, im = 0;
    unsigned i;
    for (i = 0; i < n; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1)); double a = 2 * M_PI * f * i / rate; re += w * x[2 * i] * cos(a); im += w * x[2 * i] * sin(a); }
    return 2.0 * sqrt(re * re + im * im) / (0.5 * n);
}

static unsigned char *make_tone(unsigned frames, double f, double rate, double amp)
{
    unsigned char *b = (unsigned char *)malloc((size_t)frames * 4);
    unsigned i;
    for (i = 0; i < frames; i++) {
        short v = (short)lrint(amp * sin(2 * M_PI * f * i / rate));
        b[i * 4] = (unsigned char)v; b[i * 4 + 1] = (unsigned char)((unsigned short)v >> 8); b[i * 4 + 2] = b[i * 4]; b[i * 4 + 3] = b[i * 4 + 1];
    }
    return b;
}

/* converts `frames` of `in` in chunks of `chunk` frames; returns all output frames (malloc'd), count in *n */
static short *convert(const unsigned char *in, unsigned frames, unsigned chunk, unsigned rate, unsigned *n)
{
    short *o = (short *)malloc(sizeof(short) * 2 * (frames * 2 + 100));
    unsigned done = 0, total = 0;
    while (done < frames) {
        unsigned c = frames - done < chunk ? frames - done : chunk, got;
        unsigned char *out;
        got = to_device_rate(in + (size_t)done * 4, c, rate, &out);
        memcpy(o + 2 * (size_t)total, out, (size_t)got * 4);
        total += got; done += c;
    }
    *n = total;
    return o;
}

static void reset(void) { sPos = 0; memset(sTail, 0, sizeof sTail); }

int main(void)
{
    const unsigned N = 64000;
    unsigned n, i, m;
    unsigned char *t;
    short *o, *o2;
    double a_in, a_out, worst;

    reset(); t = make_tone(N, 1000, 32006, 10000); o = convert(t, N, N, 32006, &n);
    a_out = tone_level(o + 2 * 2000, 40000, 1000, 48000); a_in = 10000;
    printf("gain_1k %.3f\n", 20 * log10(a_out / a_in)); free(t); free(o);

    reset(); t = make_tone(N, 10000, 32006, 10000); o = convert(t, N, N, 32006, &n);
    a_out = tone_level(o + 2 * 2000, 40000, 10000, 48000);
    printf("gain_10k %.3f\n", 20 * log10(a_out / a_in)); free(t); free(o);

    reset(); t = make_tone(N, 12000, 32006, 10000); o = convert(t, N, N, 32006, &n);
    a_out = tone_level(o + 2 * 2000, 40000, 12000, 48000);
    printf("image_12k_db %.1f\n", 20 * log10(tone_level(o + 2 * 2000, 40000, 32006.0 - 12000.0, 48000) / a_out)); free(t); free(o);

    reset(); t = make_tone(N, 3000, 32006, 12000); o = convert(t, N, N, 32006, &n);
    reset(); o2 = convert(t, N, 533, 32006, &m);
    worst = 0; if (n != m) worst = 99999;
    for (i = 0; i < 2 * (n < m ? n : m); i++) { double d = fabs((double)o[i] - o2[i]); if (d > worst) worst = d; }
    printf("split_diff %.0f\n", worst); free(t); free(o); free(o2);

    {   /* full-scale square wave: the filter overshoots, the result must be clamped, not wrapped around */
        unsigned char *sq = (unsigned char *)malloc((size_t)N * 4);
        int ok = 1;
        for (i = 0; i < N; i++) { short v = ((i / 40) & 1) ? 32767 : -32768; sq[i * 4] = (unsigned char)v; sq[i * 4 + 1] = (unsigned char)((unsigned short)v >> 8); sq[i * 4 + 2] = sq[i * 4]; sq[i * 4 + 3] = sq[i * 4 + 1]; }
        reset(); o = convert(sq, N, 533, 32006, &n);
        for (i = 4000; i + 1 < n; i++) {                       /* in the middle of each flat stretch the output must have the stretch's sign (a wrapped overshoot has the other) */
            double pos = (double)i * 32006.0 / 48000.0;
            int into = (int)pos % 40;
            int high = (((int)pos / 40) & 1) != 0;
            if (into < 10 || into > 30) continue;
            if (high ? o[2 * i] < 20000 : o[2 * i] > -20000) ok = 0;
        }
        printf("clamp_ok %d\n", ok); free(sq); free(o);
    }

    {   /* the rate changes between buffers (the game does that: 32006 Hz in the menus, 48009 Hz in the Game Boy Tower): no click */
        unsigned char *a = make_tone(8000, 1000, 32006, 10000), *b = make_tone(8000, 1000, 48009, 10000), *out;
        unsigned g1, g2, k, ok = 1;
        reset();
        g1 = to_device_rate(a, 8000, 32006, &out);
        {   short *keep = (short *)malloc((size_t)g1 * 4); memcpy(keep, out, (size_t)g1 * 4);
            g2 = to_device_rate(b, 8000, 48009, &out);
            for (k = g1 - 3; k + 1 < g1; k++) if (abs(keep[2 * k] - keep[2 * k + 2]) > 3000) ok = 0;
            for (k = 0; k < 3 && k + 1 < g2; k++) if (abs(((short *)out)[2 * k] - ((short *)out)[2 * k + 2]) > 3000) ok = 0;
            if (abs(keep[2 * (g1 - 1)] - ((short *)out)[0]) > 3000) ok = 0;
            free(keep); }
        printf("rate_change_ok %d\n", ok); free(a); free(b);
    }
    return 0;
}
