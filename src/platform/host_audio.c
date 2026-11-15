/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_audio.c - where the audio interface (src/ultra/hw_ai.c) hands the samples it is "playing".
 *
 * Two sinks:
 *   KYUJO_AUDIO_WAV=<file>   (any system) append every buffer to <file> as a 16-bit stereo WAV (the header is rewritten after each
 *                                  buffer, so the file is valid even when the program is stopped by a signal). For development, and for
 *                                  checking the audio on a machine without a sound device.
 *   KYUJO_AUDIO_DEVICE_WAV=<file>   (any system) the same, but of the stream that is sent to the sound device (after the
 *                                  conversion to the fixed device rate below), so the conversion can be checked without Windows.
 *   the sound device               Windows: the classic waveOut API of winmm.dll, a ring of buffers. Partly run by the first Windows
 *                                  tester (the Game Boy Tower had no sound; see "Device rate" below for the cause).
 *                                  Linux: PulseAudio / PipeWire's PulseAudio server through its native protocol (host_pulse.c). Written
 *                                  for Batocera and tested only against a fake server; not yet heard on a real one.
 *                                  Other systems have no device output yet.
 * Without any of them, the samples are dropped.
 *
 * Device rate.  The console's audio rate is the video clock divided by a divisor the game chooses, e.g. 32006 Hz in the menus and 48009 Hz
 * in the Game Boy Tower: odd rates that are not on the list of standard rates. The first Windows test had no sound in the Game Boy Tower.
 * THE CAUSE OF THAT SILENCE WAS NOT THE DEVICE: it reproduced on Linux with the WAV sink, where every buffer the Game Boy emulator
 * produced was all zeros, because the emulator read its sound registers through C bit-fields whose bit order differs between MIPS and
 * x86 (fixed by port patch 0024; see docs/known-decomp-issues.md). After that fix the Linux device-rate WAV contains tonal music in the
 * tower (spectral peaks at note frequencies, 440/465/587/783 Hz in samples checked). It has still not been LISTENED to, and the Windows
 * device path (waveOut) has not been run with it.
 * The device handling below is separate hardening, kept because an odd-rate waveOut open might also fail on some machines: the previous
 * code reopened the device with the new odd rate each time the game changed it and gave up for good if that open failed. Now the device
 * is opened ONCE at a fixed, standard 48000 Hz, and every buffer is converted to that rate here (band-limited resampler, see to_device_rate; state carried
 * across buffers and rate changes). A failed open is retried every two seconds and its Windows error text is printed.
 *
 * Cushion (Windows and Linux device paths). A late buffer (the game thread busy for a few fields) leaves the device with nothing to play, which is
 * heard as a stutter. When the device is found idle, KYUJO_AUDIO_CUSHION_MS (default 80, 0 = off) of silence is queued first, so the
 * device keeps that much audio in hand. tests/native/audio/run_cushion_test.py checks the queueing logic against a simulated device on
 * Linux; that the real device behaves like the simulation has not been tested.
 *
 * Byte order: the buffer lies in the game's memory, which holds the console's BIG-endian bytes (see src/platform/platform.h), and the
 * RSP interpreter wrote it by DMA straight from the microcode's memory, so each 16-bit sample is stored high byte first. This file swaps
 * to the little-endian WAV format. Left sample first, then right (the AI's order: n64brew "Audio Interface").
 *
 * HOST-side file.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "platform.h"
#include "host_ui.h"

#define DEVICE_RATE 48000u                      /* the fixed rate of the sound device (see "Device rate" above) */

/* Cushion (KYUJO_AUDIO_CUSHION_MS, default 80, 0 = off). The game hands over audio at the pace of the clock, so with nothing queued
 * ahead, one late buffer (the game thread was busy for a few fields) leaves the device with nothing to play = a click or stutter. Whenever
 * the device is found idle (at start-up, and after it ran dry) this much silence is queued before the next buffer; the device then always
 * has that much audio in hand, and a hold-up of the game shorter than the cushion is not heard. The price is that the sound comes this
 * much later than the picture.
 *
 * Start-up cushion (KYUJO_AUDIO_START_CUSHION_MS, default 300, Windows only). The first Windows tester heard the "Pika" voice before the N64
 * logo stutter, on every build (not on Linux or the Switch). His log showed the game thread holding still for up to 77 ms at that moment (first
 * uses of the renderer, the files and the picture are all new then), longer than the 80 ms cushion. So the FIRST time the device is idle, this
 * much silence is queued instead; the host then plays the sound slightly fast (the resampler eats 1 percent of the time, a pitch change
 * of 17 cents, starting 3 s after the first sound) until the queue is down to the normal cushion, so the sound is not 300 ms behind the picture for the rest of the session. */
static unsigned cushion_ms(void)
{
    static int ms = -1;
    if (ms < 0) {
        const char *e = getenv("KYUJO_AUDIO_CUSHION_MS");
        ms = (e && *e) ? atoi(e) : 80;
        if (ms < 0) ms = 0;
        if (ms > 400) ms = 400;                                /* stays well inside the NSLOTS buffers */
    }
    return (unsigned)ms;
}

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>

/* waveOut: the samples are queued in a ring of NSLOTS buffers; a slot is free again when Windows marks its header WHDR_DONE. If all slots
 * are in use the buffer is dropped (the game produces audio at the pace of the host clock, so this happens only if the sound device stalls). */
#define NSLOTS 64                               /* room for the start-up cushion (300 ms) and the game's buffers of a few ms each */
static HWAVEOUT sOut;
static int sOutFailed;                          /* the last open failed: wait before trying again */
static DWORD sRetryAt;                          /* GetTickCount() value before which no new open is tried */
static unsigned sQueued, sDropped, sOpenFails;  /* statistics for the diagnostics line */
static struct { WAVEHDR hdr; unsigned cap; int prepared; } sSlot[NSLOTS];

/* One line to stderr for an MMRESULT, with Windows' own text for it. */
static void mm_error(const char *what, MMRESULT r)
{
    char text[MAXERRORLENGTH];
    if (waveOutGetErrorTextA(r, text, sizeof text) != MMSYSERR_NOERROR) snprintf(text, sizeof text, "no text");
    fprintf(stderr, "kyujo: audio: %s failed: MMRESULT %u (%s)\n", what, (unsigned)r, text);
}

/* Queues one buffer in a free slot. Returns 0 if it could not be queued (all slots in use, out of memory, or a Windows error). */
static int queue_buffer(const unsigned char *le, unsigned len)
{
    int i;
    MMRESULT r;
    for (i = 0; i < NSLOTS; i++) {
        if (!sSlot[i].prepared || (sSlot[i].hdr.dwFlags & WHDR_DONE)) break;
    }
    if (i == NSLOTS) {                                         /* all queued: drop */
        if (sDropped++ == 0) fprintf(stderr, "kyujo: audio: sound device queue full, dropping buffers (first time)\n");
        return 0;
    }
    if (sSlot[i].prepared) { waveOutUnprepareHeader(sOut, &sSlot[i].hdr, sizeof(WAVEHDR)); sSlot[i].prepared = 0; }
    if (sSlot[i].cap < len) {
        free(sSlot[i].hdr.lpData);
        sSlot[i].hdr.lpData = (LPSTR)malloc(len);
        sSlot[i].cap = sSlot[i].hdr.lpData ? len : 0;
        if (!sSlot[i].hdr.lpData) return 0;
    }
    memcpy(sSlot[i].hdr.lpData, le, len);
    sSlot[i].hdr.dwBufferLength = len;
    sSlot[i].hdr.dwFlags = 0;
    r = waveOutPrepareHeader(sOut, &sSlot[i].hdr, sizeof(WAVEHDR));
    if (r != MMSYSERR_NOERROR) { if (sOpenFails++ < 5) mm_error("waveOutPrepareHeader", r); return 0; }
    sSlot[i].prepared = 1;
    r = waveOutWrite(sOut, &sSlot[i].hdr, sizeof(WAVEHDR));
    if (r != MMSYSERR_NOERROR) { if (sOpenFails++ < 5) mm_error("waveOutWrite", r); return 0; }
    if (++sQueued == 1) fprintf(stderr, "kyujo: audio: first buffer queued to the sound device\n");
    return 1;
}

/* Number of queued buffers the device has not finished playing. 0 means the device is playing nothing (start-up, or it ran dry). */
static int buffers_in_flight(void)
{
    int i, n = 0;
    for (i = 0; i < NSLOTS; i++) {
        if (sSlot[i].prepared && !(((volatile WAVEHDR *)&sSlot[i].hdr)->dwFlags & WHDR_DONE)) n++;
    }
    return n;
}

/* KYUJO_AUDIO_START_CUSHION_MS: the silence queued the first time the device is idle (default 300, 0..1000; never less than the normal cushion). */
static unsigned start_cushion_ms(void)
{
    static int ms = -1;
    if (ms < 0) {
        const char *e = getenv("KYUJO_AUDIO_START_CUSHION_MS");
        ms = (e && *e) ? atoi(e) : 300;
        if (ms < 0) ms = 0;
        if (ms > 1000) ms = 1000;
        if ((unsigned)ms < cushion_ms()) ms = (int)cushion_ms();
    }
    return (unsigned)ms;
}

/* Opens the device (48000 Hz, 16 bit, stereo). Returns 1 when it is open. Used by the early open thread and by device_write. */
static unsigned sOpenMs;                        /* how long the successful open took */
static int try_open(void)
{
    MMRESULT r;
    WAVEFORMATEX f;
    unsigned long long t0 = host_wall_ns();
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = DEVICE_RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = DEVICE_RATE * 4;
    r = waveOutOpen(&sOut, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        sOut = NULL;
        sOutFailed = 1;
        sRetryAt = GetTickCount() + 2000;
        if (sOpenFails++ < 5) mm_error("waveOutOpen (48000 Hz, 16 bit, stereo)", r);
        return 0;
    }
    sOutFailed = 0;
    sOpenMs = (unsigned)((host_wall_ns() - t0) / 1000000ull);
    fprintf(stderr, "kyujo: audio: sound device opened in %u ms (48000 Hz, 16 bit, stereo), cushion %u ms, start-up cushion %u ms\n", sOpenMs, cushion_ms(), start_cushion_ms());
    return 1;
}

/* The device is opened at program start on a short thread (host_audio_prepare), not at the game's first sound: opening can take tens of
 * milliseconds on some machines (Bluetooth, USB and virtual devices are slower than a built-in codec), and at that moment the game is
 * already running against the clock. */
static HANDLE sOpenEvent;                       /* set when the early open attempt is over */
static DWORD WINAPI early_open_thread(LPVOID unused)
{
    (void)unused;
    try_open();
    SetEvent(sOpenEvent);
    return 0;
}
void host_audio_prepare(void)
{
    HANDLE h;
    if (sOpenEvent || sOut) return;
    sOpenEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!sOpenEvent) return;
    h = CreateThread(NULL, 0, early_open_thread, NULL, 0, NULL);
    if (!h) { CloseHandle(sOpenEvent); sOpenEvent = NULL; return; }
    CloseHandle(h);
}

/* The audio that is queued and not yet played, in milliseconds at DEVICE_RATE (whole buffers: the one being played counts in full). */
static unsigned queued_ms(void)
{
    int i;
    unsigned long long bytes = 0;
    for (i = 0; i < NSLOTS; i++) {
        if (sSlot[i].prepared && !(((volatile WAVEHDR *)&sSlot[i].hdr)->dwFlags & WHDR_DONE)) bytes += sSlot[i].hdr.dwBufferLength;
    }
    return (unsigned)(bytes * 1000ull / (DEVICE_RATE * 4ull));
}

/* Start-up measurements for the first seconds after the first sound, printed once: what the tester's log needs to show where a stutter at
 * launch comes from (a late game thread shows as a long wait between two buffers, a drained device as a dry event). */
static struct {
    unsigned long long first_ns, last_ns, max_wait_ns, max_wait_at_ns;
    unsigned buffers, dry, min_queue_ms, lines;
    int reported;
} sStart = { 0, 0, 0, 0, 0, 0, 100000, 0, 0 };

static void start_note(unsigned qms_before)
{
    unsigned long long now = host_wall_ns();
    if (sStart.reported) return;
    if (!sStart.first_ns) sStart.first_ns = now;
    else {
        unsigned long long wait = now - sStart.last_ns;
        if (wait > sStart.max_wait_ns) { sStart.max_wait_ns = wait; sStart.max_wait_at_ns = now - sStart.first_ns; }
        if (wait > 40000000ull && sStart.lines++ < 8)
            fprintf(stderr, "kyujo: audio: start-up: %llu ms between two buffers at %llu ms after the first (the game thread was late); %u ms were queued before this buffer\n",
                    wait / 1000000ull, (now - sStart.first_ns) / 1000000ull, qms_before);
    }
    sStart.last_ns = now;
    sStart.buffers++;
    if (sStart.buffers > 2 && qms_before < sStart.min_queue_ms) sStart.min_queue_ms = qms_before;
    if (now - sStart.first_ns > 8000000000ull) {
        sStart.reported = 1;
        fprintf(stderr, "kyujo: audio: first 8 s: %u buffers, longest wait between two buffers %llu ms (at %llu ms), lowest queue %u ms, device ran dry %u times, opened in %u ms\n",
                sStart.buffers, sStart.max_wait_ns / 1000000ull, sStart.max_wait_at_ns / 1000000ull, sStart.min_queue_ms, sStart.dry, sOpenMs);
    }
}

/* Sends `len` bytes of little-endian 16-bit stereo at DEVICE_RATE to the device. */
static void device_write(const unsigned char *le, unsigned len)
{
    static int first_idle = 1;
    unsigned qms;
    if (sOpenEvent) {                           /* the early open is still running: wait for it (normally long finished) */
        WaitForSingleObject(sOpenEvent, 3000);
        CloseHandle(sOpenEvent);
        sOpenEvent = NULL;
    }
    if (sOutFailed && (long)(GetTickCount() - sRetryAt) < 0) return;      /* still waiting for the retry time */
    if (!sOut && !try_open()) return;
    qms = queued_ms();
    start_note(qms);
    if (cushion_ms() > 0 && buffers_in_flight() == 0) {
        static unsigned char *silence;
        static unsigned silence_len, ran_dry;
        unsigned ms = first_idle ? start_cushion_ms() : cushion_ms();
        unsigned want = (ms * DEVICE_RATE / 1000u) * 4u;
        if (!silence || silence_len != want) {
            free(silence);
            silence = (unsigned char *)calloc(1, want);
            silence_len = silence ? want : 0;
        }
        if (!first_idle) {
            sStart.dry++;
            if (sQueued > 0 && ++ran_dry <= 10) fprintf(stderr, "kyujo: audio: the sound device had run dry (%u so far); %u ms of silence queued as a cushion\n", ran_dry, ms);
        }
        if (silence) (void)queue_buffer(silence, silence_len);
        first_idle = 0;
    }
    (void)queue_buffer(le, len);
}

/* How much faster than real time the sound is played (1.0 = not at all): 1 percent (17 cents) while the device queue is longer than the normal cushion
 * plus a margin, but only from 3 s after the first sound, so that the start-up (the "Pika" voice, the logo) is played at exactly the right pitch
 * and with the full start-up cushion; the 300 ms are then run down in about 16 s. */
static double drain_factor(void)
{
    unsigned q = queued_ms(), keep = cushion_ms() + 60;
    if (!sOut || !sStart.first_ns || host_wall_ns() - sStart.first_ns < 3000000000ull) return 1.0;
    return q > keep ? 1.01 : 1.0;
}
#elif defined(__linux__)
/* Linux: PulseAudio / PipeWire's PulseAudio server, spoken to directly (host_pulse.c). Nothing here waits for the sound server: data it does
 * not want yet is dropped; a lost connection or a missing server is retried every two seconds (the first failures are logged). Without a
 * server (a development machine, a container) the program runs silently, as it did before this device existed. */
#include <time.h>
#include "host_pulse.h"

static Pulse *sPulse;
static unsigned long long sRetryAtMs, sLastWriteMs, sDropReportMs;
static unsigned sFailures, sDroppedBytes;

static unsigned long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

static double drain_factor(void) { return 1.0; }
void host_audio_prepare(void) {}

static void device_write(const unsigned char *le, unsigned len)
{
    unsigned taken;
    if (!sPulse) {
        char err[400];
        const char *off = getenv("KYUJO_AUDIO_DEVICE");
        if (off && strcmp(off, "none") == 0) return;                      /* sound device switched off (development) */
        if (mono_ms() < sRetryAtMs) return;
        sPulse = pulse_open("Pokemon Stadium", DEVICE_RATE, err, sizeof err);
        if (!sPulse) {
            sRetryAtMs = mono_ms() + 2000;
            if (sFailures++ < 3) fprintf(stderr, "kyujo: audio: no sound device: %s%s\n", err, sFailures == 3 ? " (not reporting further failures; still retrying)" : "");
            return;
        }
        pulse_set_cushion_ms(sPulse, cushion_ms());
        fprintf(stderr, "kyujo: audio: sound device opened (PulseAudio protocol, 48000 Hz, 16 bit, stereo), cushion %u ms\n", cushion_ms());
    }
    /* Safety net: no sound at all for 3 s (the menu normally keeps the stream fed, see host_audio_idle_tick). A sound server may have stopped an idle
     * stream, and a stream that was stopped may not start again by itself, so start from scratch: a new connection and a new stream, which is what
     * works at program start. The log line says how long the pause was, so a report shows whether this happened. */
    if (sLastWriteMs && mono_ms() - sLastWriteMs > 3000) {
        fprintf(stderr, "kyujo: audio: no sound was sent for %llu ms; opening a new stream (%u requests, %u underflows so far)\n",
                mono_ms() - sLastWriteMs, pulse_requests(sPulse), pulse_underruns(sPulse));
        pulse_close(sPulse);
        sPulse = NULL;
        sLastWriteMs = 0;
        device_write(le, len);                                  /* once more: opens the new stream and sends the sound */
        return;
    }
    sLastWriteMs = mono_ms();
    taken = pulse_write(sPulse, le, len);
    if (taken < len) {                                          /* the server did not ask for this much: the rest is dropped; reported at most every 10 s */
        sDroppedBytes += len - taken;
        if (mono_ms() - sDropReportMs > 10000) {
            fprintf(stderr, "kyujo: audio: the sound server did not take all the sound; %u bytes dropped so far (%u requests, %u underflows)\n",
                    sDroppedBytes, pulse_requests(sPulse), pulse_underruns(sPulse));
            sDropReportMs = mono_ms();
        }
    }
    if (!pulse_alive(sPulse)) {
        fprintf(stderr, "kyujo: audio: the connection to the sound server was lost (%u requests, %u underruns, %u bytes dropped); reconnecting\n",
                pulse_requests(sPulse), pulse_underruns(sPulse), sDroppedBytes);
        pulse_close(sPulse);
        sPulse = NULL;
        sRetryAtMs = mono_ms() + 2000;
    }
}
#elif defined(__wasm__)
/* The Switch / Android shell plays the sound (SDL audio there). It takes 48000 Hz 16-bit little-endian stereo, like the other devices, and
 * does its own buffering: it drops what it cannot take, so this call never waits. */
__attribute__((import_module("shell"), import_name("audio_write"))) extern void shell_audio_write(const void *le, unsigned int len);
static void device_write(const unsigned char *le, unsigned len) { shell_audio_write(le, len); }
static double drain_factor(void) { return 1.0; }
void host_audio_prepare(void) {}
#else
static void device_write(const unsigned char *le, unsigned len) { (void)le; (void)len; }
static double drain_factor(void) { return 1.0; }
void host_audio_prepare(void) {}
#endif

/* ---- conversion to the device rate -------------------------------------------------------------------------------------------- */

/*
 * Converts `frames` stereo frames of little-endian 16-bit samples at `rate` Hz to DEVICE_RATE with a band-limited (windowed-sinc) resampler:
 * 64 taps (RS_HALF = 32 input frames each side), Kaiser window (beta 9), cut-off at 95 percent of the lower of the two Nyquist frequencies,
 * kernel tabulated in RS_PHASES fractional positions (linear interpolation between them) and normalised to a gain of exactly 1 at DC.
 * The first version used linear interpolation: it lets the "images" of the sound through (a 12 kHz tone also comes out at 20 kHz), which the
 * device-rate WAV of a battle showed as 0.3 to 0.6 percent of the energy above 16 kHz, where the game has nothing: hiss on bright and noisy
 * sounds (the Windows tester heard static in a battle, 2026-11-12). With this kernel that part is more than 60 dB lower.
 * State is kept between calls so that consecutive buffers join without a click, and a change of `rate` simply changes the step and rebuilds the
 * kernel:
 *   sTail = the last 2 * RS_HALF input frames of the previous buffer (zeros at the start);
 *   sPos  = position of the next output frame in input-frame units relative to the first frame of the next buffer (negative: inside sTail).
 * The output lags the input by RS_HALF input frames (1 ms). The result is clamped to 16 bits (the filter overshoots near full scale).
 * Returns the number of output frames written to *out (a malloc'd block owned by this file, valid until the next call).
 */
#define RS_HALF 32
#define RS_TAPS (2 * RS_HALF)
#define RS_PHASES 512
static double sPos;
static float sTail[2 * RS_TAPS];                                /* RS_TAPS frames, left and right interleaved */
static float *sKernel;                                          /* (RS_PHASES + 1) * RS_TAPS weights */
static double sKernelCut;

static double bessel_i0(double x)                               /* modified Bessel function of the first kind, order 0 (power series) */
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    int k;
    for (k = 1; k < 60; k++) { term *= q / ((double)k * (double)k); sum += term; if (term < 1e-12 * sum) break; }
    return sum;
}

/* Builds the kernel for the cut-off `cut` (1.0 = the input's Nyquist frequency). Entry [ph][t] is the weight of input frame (floor(t) - RS_HALF + 1 + t) for an output at
 * fractional position ph / RS_PHASES. */
static int rs_build(double cut)
{
    const double pi = 3.14159265358979323846, beta = 9.0, i0b = bessel_i0(beta);
    int ph, t;
    if (sKernel && fabs(sKernelCut - cut) < 0.01) return 1;      /* (rates like 32000 and 32006 share a kernel) */
    if (!sKernel) sKernel = (float *)malloc(sizeof(float) * (RS_PHASES + 1) * RS_TAPS);
    if (!sKernel) return 0;
    for (ph = 0; ph <= RS_PHASES; ph++) {
        double frac = (double)ph / RS_PHASES, w[RS_TAPS], sum = 0.0;
        for (t = 0; t < RS_TAPS; t++) {
            double x = (double)(t - RS_HALF + 1) - frac;       /* distance (input frames) from the input frame to the output position */
            double u = x / RS_HALF, win, sinc;
            if (u <= -1.0 || u >= 1.0) win = 0.0; else win = bessel_i0(beta * sqrt(1.0 - u * u)) / i0b;
            sinc = (x > -1e-9 && x < 1e-9) ? cut : sin(pi * cut * x) / (pi * x);
            w[t] = sinc * win;
            sum += w[t];
        }
        for (t = 0; t < RS_TAPS; t++) sKernel[(size_t)ph * RS_TAPS + t] = (float)(w[t] / sum);
    }
    sKernelCut = cut;
    return 1;
}

static unsigned to_device_rate(const unsigned char *le, unsigned frames, unsigned rate, unsigned char **out)
{
    static unsigned char *buf;
    static float *in;
    static unsigned cap, in_cap;
    double step = (double)rate / (double)DEVICE_RATE;           /* input frames per output frame */
    double adv = step * drain_factor();                         /* the step actually taken (a little larger while the device queue is being run down) */
    unsigned need, n = 0, i;
    if (rate == 0 || frames == 0) return 0;
    need = (unsigned)((double)frames / step) + 4;
    if (!rs_build((step > 1.0 ? 1.0 / step : 1.0) * 0.95)) return 0;
    if (cap < need * 4) {
        free(buf);
        buf = (unsigned char *)malloc((size_t)need * 4);
        cap = buf ? need * 4 : 0;
        if (!buf) return 0;
    }
    if (in_cap < frames + RS_TAPS) {
        free(in);
        in = (float *)malloc(sizeof(float) * 2 * (size_t)(frames + RS_TAPS));
        in_cap = in ? frames + RS_TAPS : 0;
        if (!in) return 0;
    }
    memcpy(in, sTail, sizeof sTail);                            /* in[0 .. RS_TAPS-1] = the previous buffer's last frames (input positions -RS_TAPS .. -1) */
    for (i = 0; i < frames; i++) {
        in[2 * (RS_TAPS + i)] = (float)(short)(le[i * 4] | (le[i * 4 + 1] << 8));
        in[2 * (RS_TAPS + i) + 1] = (float)(short)(le[i * 4 + 2] | (le[i * 4 + 3] << 8));
    }
    while (n < need) {
        int base = (int)floor(sPos);                            /* the input frame at or before the output position */
        double fr = sPos - (double)base, pf = fr * RS_PHASES;
        int ph = (int)pf, t;
        float mix = (float)(pf - (double)ph), l = 0.f, r = 0.f;
        const float *k0, *k1;
        const float *src;
        float ol, orr;
        if (base + RS_HALF >= (int)frames) break;               /* the last input frame this output needs comes with the next buffer */
        k0 = sKernel + (size_t)ph * RS_TAPS;
        k1 = k0 + RS_TAPS;
        src = in + 2 * (size_t)(RS_TAPS + base - RS_HALF + 1);
        for (t = 0; t < RS_TAPS; t++) {
            float wgt = k0[t] + (k1[t] - k0[t]) * mix;
            l += wgt * src[2 * t];
            r += wgt * src[2 * t + 1];
        }
        ol = l < -32768.f ? -32768.f : l > 32767.f ? 32767.f : l;
        orr = r < -32768.f ? -32768.f : r > 32767.f ? 32767.f : r;
        {
            int a = (int)(ol < 0.f ? ol - 0.5f : ol + 0.5f), b = (int)(orr < 0.f ? orr - 0.5f : orr + 0.5f);
            buf[n * 4] = (unsigned char)a; buf[n * 4 + 1] = (unsigned char)((unsigned)a >> 8);
            buf[n * 4 + 2] = (unsigned char)b; buf[n * 4 + 3] = (unsigned char)((unsigned)b >> 8);
        }
        n++;
        sPos += adv;
    }
    sPos -= (double)frames;                                     /* now relative to the next buffer */
    memcpy(sTail, in + 2 * frames, sizeof sTail);               /* the last RS_TAPS frames of (tail + this buffer) */
    *out = buf;
    return n;
}

/* ---- WAV files (development sinks) ------------------------------------------------------------------------------------------- */

/* A growing 16-bit stereo WAV file. The header is rewritten after each buffer so the file is valid even when the program is killed. */
typedef struct WavSink { FILE *f; unsigned rate; uint32_t bytes; } WavSink;

static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

/* Writes the 44-byte RIFF/WAVE header for `w->bytes` bytes of 16-bit stereo PCM (the canonical layout of the PCM format, 16 bytes of "fmt "). */
static void wav_header(WavSink *w)
{
    unsigned char h[44];
    memcpy(h, "RIFF", 4);          put32(h + 4, 36 + w->bytes);
    memcpy(h + 8, "WAVEfmt ", 8);  put32(h + 16, 16);
    put16(h + 20, 1);              /* PCM */
    put16(h + 22, 2);              /* channels */
    put32(h + 24, w->rate);
    put32(h + 28, w->rate * 4);    /* bytes per second */
    put16(h + 32, 4);              /* block align */
    put16(h + 34, 16);             /* bits per sample */
    memcpy(h + 36, "data", 4);     put32(h + 40, w->bytes);
    fseek(w->f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, w->f);
    fseek(w->f, 0, SEEK_END);
}

/* Opens the file named by the environment variable `var` (if set) as a sink with the given rate. */
static void wav_open(WavSink *w, const char *var, unsigned rate)
{
    const char *path = getenv(var);
    if (!path || !*path) return;
    w->f = fopen(path, "wb+");
    if (!w->f) { fprintf(stderr, "kyujo: cannot create %s\n", path); return; }
    w->rate = rate;
    w->bytes = 0;
    wav_header(w);
}

static void wav_append(WavSink *w, const unsigned char *le, unsigned len)
{
    if (!w->f || len == 0) return;
    fwrite(le, 1, len, w->f);
    w->bytes += len;
    wav_header(w);
    fflush(w->f);
}

/* The volume setting (menu: Options, 0..100 percent) applied to the device stream: 16-bit little-endian samples scaled by volume/100 in 8.8 fixed
 * point. 100 leaves the samples untouched (so the development WAV files and the tests are not changed). */
static void apply_volume(unsigned char *p, unsigned samples, int percent)
{
    unsigned i;
    int gain = percent * 256 / 100;
    if (percent >= 100) return;
    for (i = 0; i < samples; i++) {
        int v = (short)(p[2 * i] | (p[2 * i + 1] << 8));
        v = v * gain / 256;
        p[2 * i] = (unsigned char)v;
        p[2 * i + 1] = (unsigned char)(v >> 8);
    }
}

/* ---- the entry point ------------------------------------------------------------------------------------------------------------ */

#if !defined(__wasm__)
/* Development aid (not used in normal play): KYUJO_STREAM_DUMP=<file> records, once per audio task, the state of the game's streamed voice channels
 * (the announcer's GSM voice, src/audio_stream.c): for each of the two channels one record
 *   u8 channel, u8 state (D_800FCEF0; 2 = idle), u16 0, u32 read position (D_800FCF30), u32 write position (D_800FCF38), then the 0x1140 bytes of the
 *   channel's ring buffer (D_800FCF28) exactly as the game memory holds them (big-endian samples, left and right alike).
 * It reads the game's variables through their linker symbols (game memory holds big-endian words, swapped here) and the ring by its address without
 * 0x80000000, so it is only for the native Windows/Linux builds. Idle channels are skipped. tools/native/analyse_stream_dump.py reads the file. */
extern unsigned char D_800FCEF0[2];            /* game symbols (src/audio_stream.c) */
extern unsigned D_800FCF28[2], D_800FCF30[2], D_800FCF38[2];
#define BE32(v) ((((v) & 0xFFu) << 24) | (((v) & 0xFF00u) << 8) | (((v) >> 8) & 0xFF00u) | ((v) >> 24))
static void stream_dump(void)
{
    static FILE *f;
    static int tried;
    unsigned ch;
    if (!tried) {
        const char *path = getenv("KYUJO_STREAM_DUMP");
        tried = 1;
        if (path && path[0]) { f = fopen(path, "wb"); if (f) fprintf(stderr, "kyujo: stream dump to %s\n", path); }
    }
    if (!f) return;
    for (ch = 0; ch < 2; ch++) {
        unsigned char state = D_800FCEF0[ch];
        unsigned ring, rdpos, wrpos;
        unsigned char head[12];
        if (state == 2) continue;
        ring = BE32(D_800FCF28[ch]) & 0x00FFFFFFu;
        rdpos = BE32(D_800FCF30[ch]);
        wrpos = BE32(D_800FCF38[ch]);
        if (ring == 0 || ring + 0x1140u > 0x00800000u) continue;
        head[0] = (unsigned char)ch; head[1] = state; head[2] = head[3] = 0;
        memcpy(head + 4, &rdpos, 4); memcpy(head + 8, &wrpos, 4);
        fwrite(head, 1, 12, f);
        fwrite((const void *)(uintptr_t)ring, 1, 0x1140, f);
    }
}
#endif

int host_audio_trace_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("KYUJO_AUDIO_TRACE"); on = (e && e[0] == '1') ? 1 : 0; }
    return on;
}

void host_audio_buffer(unsigned int addr, unsigned int len, unsigned int rate)
{
    static WavSink gameWav, deviceWav;          /* the stream at the game's own rate / at the device rate */
    static int sTried;
    static unsigned lastRate;
    const unsigned char *src = (const unsigned char *)(uintptr_t)addr;
    unsigned i;

    if (!sTried) {
        sTried = 1;
        wav_open(&gameWav, "KYUJO_AUDIO_WAV", rate);
        wav_open(&deviceWav, "KYUJO_AUDIO_DEVICE_WAV", DEVICE_RATE);
    }
    if (len == 0) return;
#if !defined(__wasm__)
    stream_dump();
#endif
    if (rate != lastRate) {                     /* a diagnostic that the tester can report: the game's audio rate over time */
        fprintf(stderr, "kyujo: audio: the game's sample rate is now %u Hz (was %u Hz)\n", rate, lastRate);
        lastRate = rate;
    }
    {   /* the device and the WAV format want little-endian samples: convert once */
        static unsigned char *conv;
        static unsigned conv_cap;
        unsigned char *dev;
        unsigned dev_frames;
        if (conv_cap < len) { free(conv); conv = (unsigned char *)malloc(len); conv_cap = conv ? len : 0; }
        if (!conv) return;
        for (i = 0; i + 1 < len; i += 2) { conv[i] = src[i + 1]; conv[i + 1] = src[i]; }       /* big-endian samples -> little-endian */
        if (gameWav.f) {
            if (rate != gameWav.rate) {
                static int warned;
                if (!warned) { fprintf(stderr, "kyujo: audio rate changed from %u to %u Hz; the WAV keeps %u Hz\n", gameWav.rate, rate, gameWav.rate); warned = 1; }
            }
            wav_append(&gameWav, conv, len);
        }
        dev_frames = to_device_rate(conv, len / 4, rate, &dev);
        if (dev_frames) {
            apply_volume(dev, dev_frames * 2, host_settings()->volume);
            device_write(dev, dev_frames * 4);
            wav_append(&deviceWav, dev, dev_frames * 4);
        }
    }
}

/* While the menu is open the game stands still and produces no sound (host_clock_pause), so the sound device would get nothing for as long as the
 * menu stays open. A sound server may treat a stream that has had nothing for a few seconds as idle and stop playing it (PipeWire has a timeout of
 * this kind), and a stream that was stopped this way may not start again by itself: reported on Batocera, "no sound after closing the menu". The menu
 * loop therefore calls this about every 16 ms; it feeds the device exactly the silence that the time since the last call is worth, so the stream
 * never runs dry while the game is paused. The sound of the game continues right behind it. Only the Linux (PulseAudio) device needs this: waveOut
 * on Windows plays what it is given and has no such idle rule. */
void host_audio_idle_tick(void)
{
#ifdef __linux__
    static unsigned long long lastNs;
    static const unsigned char zero[4096];                      /* static zeros: 4 KiB of image */
    struct timespec ts;
    unsigned long long now, frames;
    static int off = -1;
    if (off < 0) off = getenv("KYUJO_AUDIO_NO_KEEPALIVE") != NULL;         /* development: switch it off, to test the safety net in device_write */
    if (off) return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    if (lastNs == 0 || now - lastNs > 250000000ull) { lastNs = now; return; }     /* the first call of a pause (or a new pause): only remember the time */
    frames = (now - lastNs) * DEVICE_RATE / 1000000000ull;
    if (frames == 0) return;
    lastNs += frames * 1000000000ull / DEVICE_RATE;             /* keep the remainder, so the amount written follows the clock exactly */
    while (frames > 0) {
        unsigned n = frames > sizeof zero / 4 ? (unsigned)(sizeof zero / 4) : (unsigned)frames;
        device_write(zero, n * 4);
        frames -= n;
    }
#endif
}
