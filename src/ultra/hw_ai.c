/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * hw_ai.c - the audio interface (AI): the chip that streams 16-bit stereo samples from memory to the digital-to-analog converter.
 *
 * What the game relies on (registers: lib/ultralib/include/PR/rcp.h):
 *   - It writes a buffer's address (AI_DRAM_ADDR_REG) and length (AI_LEN_REG) once per audio frame (osAiSetNextBuffer). At most two
 *     buffers are held: the one being played and one waiting (AI_STATUS_FIFO_FULL, bit 31, is set when both slots are in use;
 *     AI_STATUS_DMA_BUSY, bit 30, while one is playing).
 *   - It reads AI_LEN_REG to learn how many bytes of the playing buffer are still to be played (src/3D140.c, Audio_CreateFrameTask,
 *     and osAiGetLength) and sizes its next audio frame from that. The register therefore has to count down in real time.
 *   - The playback rate is the video clock divided by (AI_DACRATE + 1); a sample frame (left + right) is 4 bytes.
 *
 * Sources for the behaviour (n64brew's "Audio Interface" page, https://n64brew.dev/wiki/Audio_Interface, and the libultra manual page
 * osAiSetNextBuffer on ultra64.ca): length reads back the bytes remaining; rate = video clock / (DACRATE + 1) (the page's example:
 * DACRATE 1103 gives 44136 Hz on NTSC); the AI interrupt is raised when a DMA transfer STARTS (not when it ends); writing AI_STATUS
 * acknowledges the interrupt; reads of the write-only registers return a mirror of AI_LEN; a buffer is a multiple of 8 bytes.
 * NOT stated there, and chosen here: when the buffer in play ends and one is waiting, the waiting one starts at that instant
 * (seamless), and a third write while both slots are in use is ignored.
 *
 * Time model. Nothing runs in the background: the state is brought up to date from the host clock whenever a register is touched.
 * A buffer that has finished and has a successor therefore "starts" (and raises its interrupt) when the game next touches the AI,
 * not at the exact instant. Nobody in this game waits for the AI interrupt (grep OS_EVENT_AI finds no user), so the delay is
 * unobservable; a consumer that does wait for it would need a timer, and this is the place to add it.
 *
 * The samples: start_buffer() hands each buffer to host_audio_buffer() (src/platform/host_audio.c), which today can write a WAV file.
 * Playing them on a sound device is not done yet. The pacing above follows the host clock, so a device must consume at the same rate.
 *
 * GAME-side file.
 */
#include "PR/os_internal.h"
#include "PR/rcp.h"
#include "platform.h"
#include "hw_internal.h"

extern s32 osViClock;                   /* video clock in Hz, set by osInitialize from the TV type (see hw_regs.c) */

typedef struct {
    u32 addr;                           /* where the samples are (a host pointer in this port) */
    u32 len;                            /* bytes, multiple of 8 */
    u32 seq;                            /* trace only (KYUJO_AUDIO_TRACE): audio microcode runs started when the game handed the buffer over */
    unsigned long long t_ns;            /* trace only: when */
} AiBuffer;

static AiBuffer sPlaying, sWaiting;
static int sPlayingValid, sWaitingValid;
static unsigned long long sPlayStartNs;     /* host clock when the playing buffer started */
static unsigned long long sGapStartNs;      /* host clock when the last buffer ended with no successor (valid while sGapOpen) */
static int sGapOpen;
static u32 sDramAddr, sControl, sDacRate, sBitRate;

/* Sample frames per second, from the documented formula. 0 if the clock is not set up yet. */
static unsigned long long sample_rate(void)
{
    return (unsigned long long)(u32)osViClock / ((unsigned long long)(sDacRate & 0x3FFF) + 1);
}

/* How long a buffer of `len` bytes plays, in nanoseconds. */
static unsigned long long duration_ns(u32 len)
{
    unsigned long long rate = sample_rate();
    if (rate == 0) return 0;
    return (unsigned long long)(len / 4) * 1000000000ULL / rate;
}

/* Development / tester trace (KYUJO_AUDIO_TRACE=1): one log line for each of the first 240 buffers the output takes over:
 *   AITRACE n hash same3 frame-bytes runs-started runs-done runs-started-at-hand-over run-active hand-over-to-start-ms start-lateness-ms ms-since-last-run-start ms-since-last-run-end t-ms
 * "same3" is 1 when the buffer holds exactly the samples of the buffer three before it (the game cycles three output buffers): a buffer the audio microcode did not
 * rewrite is played again. Decimal numbers only, so that no C library is needed here. */
static void trace_buffer(AiBuffer b, unsigned long long at_ns)
{
    static unsigned n;
    static unsigned long long hist[3];
    unsigned long long h = 1469598103934665603ULL, now = host_now_ns();
    const unsigned char *p = (const unsigned char *)(unsigned long)b.addr;
    unsigned vals[16], nz = 0, k, vi, d, pos = 0;
    char m[200];
    if (n >= 240) return;
    for (k = 0; k + 8 <= b.len; k += 8) {
        if (p[k] | p[k + 1] | p[k + 2] | p[k + 3] | p[k + 4] | p[k + 5]) nz = 1;
        h = (h ^ ((unsigned long long)p[k] | ((unsigned long long)p[k + 1] << 8) | ((unsigned long long)p[k + 2] << 16) | ((unsigned long long)p[k + 3] << 24))) * 1099511628211ULL;
    }
    vals[0] = n;
    vals[1] = (unsigned)(h >> 40);
    vals[2] = (nz && h == hist[n % 3]) ? 1u : 0u;
    vals[3] = b.len;
    vals[4] = HostHwRspJobSeq();
    vals[5] = HostHwRspJobsDone();
    vals[6] = b.seq;
    vals[7] = (unsigned)HostHwRspBusy();
    vals[8] = (unsigned)((now - b.t_ns) / 1000000ull);
    vals[9] = (unsigned)((now - at_ns) / 1000000ull);
    vals[10] = (unsigned)(HostHwRspSinceStartNs() / 1000000ull);
    vals[11] = (unsigned)(HostHwRspSinceEndNs() / 1000000ull);
    vals[12] = host_dbg[16 + 7];                    /* video retraces delivered to the game so far */
    vals[13] = host_dbg[16 + 4];                    /* SP (RSP task done) events delivered */
    vals[14] = (unsigned)(now / 1000000ull);
    hist[n % 3] = h;
    n++;
    for (vi = 0; vi < 15; vi++) {
        char t[12];
        unsigned tn = 0, v = vals[vi];
        if (pos + 14 >= sizeof m) break;
        if (vi == 0) { const char *l = "AITRACE"; while (*l) m[pos++] = *l++; }
        m[pos++] = ' ';
        if (v == 0) t[tn++] = '0';
        while (v) { t[tn++] = (char)('0' + v % 10); v /= 10; }
        for (d = tn; d > 0; d--) m[pos++] = t[d - 1];
    }
    m[pos] = 0;
    host_log(m);
}

/* Trace (KYUJO_AUDIO_TRACE=1): "EVT tag value ms", one line for each thing the audio path does, for the first 4500: a timeline to read next to AITRACE. */
extern unsigned char D_800A62E0[];                  /* the game's scheduler state (src/scheduler.h) */
static unsigned slot_type(unsigned off)
{
    unsigned long p = (unsigned long)*(unsigned *)(D_800A62E0 + off);
    return p ? *(unsigned *)(p + 0x20) : 0;
}
void HostTraceEv(const char *tag, unsigned v)
{
    static unsigned lines;
    char m[80];
    unsigned pos = 0, vi, tn, d, x;
    unsigned vals[3];
    char t[12];
    if (lines >= 4500 || !host_audio_trace_enabled()) return;
    lines++;
    vals[0] = v;
    vals[1] = (unsigned)(host_now_ns() / 1000000ull);
    vals[2] = slot_type(0xA14) * 1000 + slot_type(0xA18) * 100 + slot_type(0xA1C) * 10 + slot_type(0xA20);   /* A14 A18 A1C A20 as digits */
    { const char *l = "EVT "; while (*l) m[pos++] = *l++; }
    while (*tag && pos < 40) m[pos++] = *tag++;
    for (vi = 0; vi < 3; vi++) {
        x = vals[vi]; tn = 0;
        m[pos++] = ' ';
        if (x == 0) t[tn++] = '0';
        while (x) { t[tn++] = (char)('0' + x % 10); x /= 10; }
        for (d = tn; d > 0; d--) m[pos++] = t[d - 1];
    }
    m[pos] = 0;
    host_log(m);
}

static void start_buffer(AiBuffer b, unsigned long long at_ns)
{
    /* The samples are taken now. If the game hands over a buffer while the audio microcode run that fills it is still on its worker thread (a run starts at the retrace, the
     * hand-over follows at once; on the console the DMA reads the data up to a frame later), the run is waited for first: about a millisecond, and the buffer holds the new sound
     * instead of the previous contents. */
    HostHwRspSettle();
    if (host_audio_trace_enabled()) trace_buffer(b, at_ns);
    if (sGapOpen) {                     /* the audio ran dry until now: report how long (speed monitor, src/platform/host_perf.c) */
        host_perf_add(PERF_AIGAP, at_ns > sGapStartNs ? at_ns - sGapStartNs : 0);
        sGapOpen = 0;
    }
    sPlaying = b;
    sPlayingValid = 1;
    sPlayStartNs = at_ns;
    host_audio_buffer(b.addr, b.len, (unsigned int)sample_rate());      /* the samples go to the output (src/platform/host_audio.c) */
    HostHwRaiseAiIrq();                 /* "raised when a DMA transfer starts" */
}

/* Moves on to the waiting buffer for as long as the playing one has finished by `now`. */
static void advance(unsigned long long now)
{
    while (sPlayingValid && now >= sPlayStartNs + duration_ns(sPlaying.len)) {
        unsigned long long end = sPlayStartNs + duration_ns(sPlaying.len);
        if (sWaitingValid) {
            sWaitingValid = 0;
            start_buffer(sWaiting, end);
        } else {
            sPlayingValid = 0;
            sGapStartNs = end;          /* silence starts here and lasts until the game supplies the next buffer */
            sGapOpen = 1;
        }
    }
}

static u32 remaining_bytes(void)
{
    unsigned long long now = host_now_ns();
    unsigned long long frames, consumed;
    advance(now);
    if (!sPlayingValid) return 0;
    frames = (now - sPlayStartNs) * sample_rate() / 1000000000ULL;
    consumed = (frames * 4) & ~7ULL;                       /* whole 8-byte units */
    return consumed >= sPlaying.len ? 0 : (u32)(sPlaying.len - consumed);
}

unsigned HostAiLenReads;                                   /* trace: reads of AI_LEN (one per Audio_CreateFrameTask) */
u32 HostAiRead32(u32 reg)
{
    advance(host_now_ns());
    switch (reg) {
    case AI_LEN_REG:
        HostAiLenReads++;
        HostTraceEv("ai-len-read", HostAiLenReads);
    case AI_DRAM_ADDR_REG:                                 /* write-only: "Reads return a mirror of AI_LENGTH" */
    case AI_CONTROL_REG:
        return remaining_bytes();
    case AI_STATUS_REG:
        return (sPlayingValid && sWaitingValid ? AI_STATUS_FIFO_FULL : 0) | (sPlayingValid ? AI_STATUS_DMA_BUSY : 0) |
               ((sControl & 1) ? (1u << 25) : 0);          /* bit 25: ENABLED (n64brew; rcp.h does not name it) */
    case AI_DACRATE_REG:
    case AI_BITRATE_REG:
    default:
        host_missing_register("AI register read", reg);
    }
}

void HostAiWrite32(u32 reg, u32 v)
{
    switch (reg) {
    case AI_DRAM_ADDR_REG:
        if (v & 7) host_missing_register("AI DMA address that is not 8-byte aligned:", v);
        /* The register holds a 24-bit RDRAM address: "DRAM_ADDR[23:0]" (n64brew.dev/wiki/Audio_Interface); bits 31..24 do not exist.
         * The game relies on that: the GB Tower code (osGbSetNextBuffer, a copy of osAiSetNextBuffer) writes OS_PHYSICAL_TO_K0(buffer),
         * i.e. 0x80xxxxxx, where libultra's own function writes the physical address. In this port a native pointer IS the physical
         * address (below 16 MiB, docs/architecture.md), so masking gives exactly what the hardware would use. */
        sDramAddr = v & 0x00FFFFFFu;
        break;
    case AI_LEN_REG: {
        AiBuffer b;
        b.addr = sDramAddr;
        b.len = v & 0x3FFF8;                               /* 18 bits, multiple of 8 */
        b.seq = HostHwRspJobSeq();
        b.t_ns = host_now_ns();
        HostTraceEv("ai-set-buffer", b.len);
        if (b.len == 0) host_missing_register("AI DMA of length zero:", v);
        advance(host_now_ns());
        if (!sPlayingValid) {
            start_buffer(b, host_now_ns());
        } else if (!sWaitingValid) {
            sWaiting = b;
            sWaitingValid = 1;
        }                                                  /* both slots in use: the write is ignored (assumption, see above) */
        break;
    }
    case AI_CONTROL_REG:  sControl = v & 1; break;
    case AI_STATUS_REG:   HostHwAckAiIrq(); break;         /* "Writes to it acknowledge the AI interrupt" */
    case AI_DACRATE_REG:  sDacRate = v & 0x3FFF; break;
    case AI_BITRATE_REG:  sBitRate = v & 0xF; break;
    default:
        host_missing_register("AI register write", reg);
    }
}
