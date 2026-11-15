/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_pulse.c - a PulseAudio native-protocol client for playback. See host_pulse.h.
 *
 * The protocol, from the PulseAudio sources (src/pulsecore/pstream.c, tagstruct.h, native-common.h, protocol-native.c, src/pulse/stream.c, context.c):
 *   FRAMING. Everything is sent in frames: a 20-byte descriptor of five big-endian 32-bit numbers, then the payload.
 *       [0] payload length   [1] channel (0xFFFFFFFF = a command)   [2] offset high   [3] offset low   [4] flags
 *     A command frame has channel 0xFFFFFFFF, offset 0 and flags 0; its payload is a "tagstruct". A sound frame has the stream's channel number,
 *     the write offset (0, with seek mode RELATIVE = 0 in the low byte of flags) and the raw sound as payload.
 *   TAGSTRUCT. A list of typed values: a tag byte, then the value. Used here: 'L' u32 (big endian), 'B' u8, '1' / '0' booleans (no value
 *     bytes), 't' string (NUL-terminated) / 'N' null string, 'x' arbitrary (u32 length + bytes), 'a' sample spec (u8 format, u8 channels, u32 rate),
 *     'm' channel map (u8 count, one u8 each), 'v' volumes (u8 count, one u32 each), 'P' property list, 'U' 64-bit time, 'f' format info.
 *   COMMANDS. Every command starts with a u32 command number and a u32 tag (a counter; the reply carries the same tag):
 *       AUTH 8 (u32 version, 256-byte cookie)  ->  REPLY 2 (u32 server version)
 *       SET_CLIENT_NAME 9 (property list)  ->  REPLY (u32 client index)
 *       CREATE_PLAYBACK_STREAM 3 (many fields, see create_stream)  ->  REPLY (channel, stream index, bytes wanted, buffer sizes ...)
 *     After that the server sends REQUEST 61 (tag, channel, bytes): "I will accept this many more bytes", and UNDERFLOW 63 when it ran dry.
 *     An ERROR reply is command 0 with the tag and an error number.
 *   VERSION. The client names its protocol version in AUTH; the server answers with its own; both use the lower one. Fields that were added in
 *     later versions are sent only if the version is high enough (the conditions below are the ones in stream.c).
 *   LOGIN. A unix-socket client is let in because the kernel tells the server who connected (the same user, or a member of the server's group);
 *     the 256-byte cookie of ~/.config/pulse/cookie is sent as well, all zeros if there is no such file (what libpulse does).
 *   NO SHARED MEMORY: the client does not offer shared memory, so the server expects the sound inside the frames.
 *
 * Status: written from the sources above and checked against a fake server in tests/native/pulse (which only proves the code does what this
 * file says, not what a real server does); the first test against a real server is the user's Batocera.
 * HOST-side file (Linux).
 */
#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "host_pulse.h"

#define MY_VERSION 35u                          /* the newest protocol version this client knows (PulseAudio 15 and later speak it) */
#define CMD_ERROR 0
#define CMD_REPLY 2
#define CMD_CREATE_PLAYBACK_STREAM 3
#define CMD_AUTH 8
#define CMD_SET_CLIENT_NAME 9
#define CMD_REQUEST 61
#define CMD_OVERFLOW 62
#define CMD_UNDERFLOW 63
#define CMD_PLAYBACK_STREAM_KILLED 64
#define COOKIE_LENGTH 256
#define INVALID_INDEX 0xFFFFFFFFu
#define OUTBUF 262144u
#define INBUF 65536u

struct Pulse {
    int fd;
    int dead;
    unsigned version;                            /* the version both sides use */
    unsigned tag;                                /* next command tag */
    uint32_t channel;                            /* the stream's channel number */
    uint32_t requested;                          /* bytes the server will accept now */
    unsigned rate;
    unsigned cushion_ms;
    int primed;                                  /* the cushion has been sent */
    unsigned requests, underruns, dropped_total, other_events;
    char path[110];
    unsigned char *out;                          /* bytes not yet accepted by the socket */
    unsigned out_len;
    unsigned char in[INBUF];
    unsigned in_len;
    int skip;                                    /* bytes of an oversized frame still to be thrown away */
    unsigned char reply_copy[INBUF];             /* the last reply, kept for the caller of transact() (the input buffer moves on) */
};

/* ---- tagstruct writer ---- */
typedef struct { unsigned char b[1024]; unsigned n; } TS;
static void ts_u8(TS *t, unsigned v) { t->b[t->n++] = (unsigned char)v; }
static void ts_raw32(TS *t, uint32_t v) { t->b[t->n++] = (unsigned char)(v >> 24); t->b[t->n++] = (unsigned char)(v >> 16); t->b[t->n++] = (unsigned char)(v >> 8); t->b[t->n++] = (unsigned char)v; }
static void ts_u32(TS *t, uint32_t v) { ts_u8(t, 'L'); ts_raw32(t, v); }
static void ts_byte(TS *t, unsigned v) { ts_u8(t, 'B'); ts_u8(t, v); }          /* a typed u8 value (ts_u8 alone writes a raw byte, used inside other items) */
static void ts_bool(TS *t, int v) { ts_u8(t, v ? '1' : '0'); }
static void ts_str(TS *t, const char *s)
{
    if (!s) { ts_u8(t, 'N'); return; }
    ts_u8(t, 't');
    memcpy(t->b + t->n, s, strlen(s) + 1);
    t->n += (unsigned)strlen(s) + 1;
}
static void ts_arbitrary(TS *t, const void *d, unsigned len) { ts_u8(t, 'x'); ts_raw32(t, len); memcpy(t->b + t->n, d, len); t->n += len; }
static void ts_sample_spec(TS *t, unsigned rate) { ts_u8(t, 'a'); ts_u8(t, 3 /* S16LE */); ts_u8(t, 2); ts_raw32(t, rate); }
static void ts_channel_map(TS *t) { ts_u8(t, 'm'); ts_u8(t, 2); ts_u8(t, 1 /* front left */); ts_u8(t, 2 /* front right */); }
static void ts_cvolume(TS *t) { ts_u8(t, 'v'); ts_u8(t, 2); ts_raw32(t, 0x10000); ts_raw32(t, 0x10000); }    /* 0x10000 = 100 % */
/* a property list with one string property (value stored with its NUL), or an empty one */
static void ts_proplist(TS *t, const char *key, const char *value)
{
    ts_u8(t, 'P');
    if (key) { ts_str(t, key); ts_u32(t, (uint32_t)strlen(value) + 1); ts_arbitrary(t, value, (unsigned)strlen(value) + 1); }
    ts_str(t, NULL);
}

/* ---- tagstruct reader ---- */
typedef struct { const unsigned char *p; unsigned n, i; } TR;
static int tr_u32(TR *r, uint32_t *v)
{
    if (r->i + 5 > r->n || r->p[r->i] != 'L') return -1;
    *v = (uint32_t)r->p[r->i + 1] << 24 | (uint32_t)r->p[r->i + 2] << 16 | (uint32_t)r->p[r->i + 3] << 8 | r->p[r->i + 4];
    r->i += 5;
    return 0;
}

/* ---- sending ---- */

static void be32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

static void flush_out(Pulse *p)
{
    while (p->out_len > 0 && !p->dead) {
        ssize_t n = send(p->fd, p->out, p->out_len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return;
            p->dead = 1;
            return;
        }
        memmove(p->out, p->out + n, p->out_len - (unsigned)n);
        p->out_len -= (unsigned)n;
    }
}

/* Queues one frame. Returns 0, or -1 if the output buffer has no room (nothing is queued then). */
static int queue_frame(Pulse *p, uint32_t channel, uint32_t flags, const void *payload, unsigned len)
{
    unsigned char d[20];
    if (p->out_len + 20 + len > OUTBUF) { flush_out(p); if (p->out_len + 20 + len > OUTBUF) return -1; }
    be32(d, len); be32(d + 4, channel); be32(d + 8, 0); be32(d + 12, 0); be32(d + 16, flags);
    memcpy(p->out + p->out_len, d, 20);
    memcpy(p->out + p->out_len + 20, payload, len);
    p->out_len += 20 + len;
    flush_out(p);
    return 0;
}

static unsigned new_tag(Pulse *p) { return p->tag++; }

/* ---- receiving ---- */

typedef struct { uint32_t channel; unsigned len; const unsigned char *data; } Frame;

/* Takes the next complete frame out of the input buffer (at in[0]); returns 1 and sets *f (valid until consume_frame), 0 if more bytes are needed. */
static int next_frame(Pulse *p, Frame *f)
{
    uint32_t len;
    while (p->skip > 0 && p->in_len > 0) {                         /* the rest of an oversized frame */
        unsigned n = p->in_len < (unsigned)p->skip ? p->in_len : (unsigned)p->skip;
        memmove(p->in, p->in + n, p->in_len - n);
        p->in_len -= n;
        p->skip -= (int)n;
    }
    if (p->in_len < 20) return 0;
    len = (uint32_t)p->in[0] << 24 | (uint32_t)p->in[1] << 16 | (uint32_t)p->in[2] << 8 | p->in[3];
    if (len > INBUF - 20) {                                        /* too big to keep (never happens for the commands we get): skip it */
        unsigned have = p->in_len - 20;
        p->skip = (int)(len - have);
        p->in_len = 0;
        return 0;
    }
    if (p->in_len < 20 + len) return 0;
    f->channel = (uint32_t)p->in[4] << 24 | (uint32_t)p->in[5] << 16 | (uint32_t)p->in[6] << 8 | p->in[7];
    f->len = len;
    f->data = p->in + 20;
    return 1;
}

static void consume_frame(Pulse *p, const Frame *f)
{
    unsigned used = 20 + f->len;
    memmove(p->in, p->in + used, p->in_len - used);
    p->in_len -= used;
}

/* Reads what the socket has (waiting up to timeout_ms for the first byte). Returns 0, or -1 if the connection is gone. */
static int fill_input(Pulse *p, int timeout_ms)
{
    struct pollfd pf = {p->fd, POLLIN, 0};
    ssize_t n;
    if (p->in_len >= INBUF) return 0;
    if (poll(&pf, 1, timeout_ms) <= 0) return 0;
    n = recv(p->fd, p->in + p->in_len, INBUF - p->in_len, MSG_DONTWAIT);
    if (n == 0) { p->dead = 1; return -1; }
    if (n < 0) { if (errno == EAGAIN || errno == EINTR) return 0; p->dead = 1; return -1; }
    p->in_len += (unsigned)n;
    return 0;
}

/* A command arriving while the stream runs. */
static void handle_command(Pulse *p, const Frame *f)
{
    TR r = {f->data, f->len, 0};
    uint32_t cmd, tag, ch, v;
    if (tr_u32(&r, &cmd) || tr_u32(&r, &tag)) return;
    switch (cmd) {
    case CMD_REQUEST:
        if (tr_u32(&r, &ch) == 0 && tr_u32(&r, &v) == 0 && ch == p->channel) { p->requested += v; p->requests++; }
        break;
    case CMD_UNDERFLOW:
        p->underruns++;
        p->primed = 0;                                             /* the next sound is preceded by the cushion again */
        if (p->underruns <= 5) fprintf(stderr, "kyujo: audio: the sound server reports an underflow (it ran out of sound; %u so far)\n", p->underruns);
        break;
    case CMD_OVERFLOW:
        break;
    case CMD_PLAYBACK_STREAM_KILLED:
        fprintf(stderr, "kyujo: audio: the sound server closed the stream\n");
        p->dead = 1;
        break;
    default:                                                       /* suspended, moved, started, events ...: nothing to do, but the log shows what the server sent: when */
        if (p->other_events++ < 8)                                 /* the sound stops for no reason this is the first place to look (the numbers are PulseAudio's COMMAND_* values) */
            fprintf(stderr, "kyujo: audio: the sound server sent command %u (not acted on)\n", (unsigned)cmd);
        break;
    }
}

static void pump(Pulse *p)
{
    Frame f;
    flush_out(p);
    fill_input(p, 0);
    while (next_frame(p, &f)) {
        if (f.channel == 0xFFFFFFFFu) handle_command(p, &f);
        consume_frame(p, &f);
    }
}

static const char *error_name(uint32_t e)
{
    static const char *names[] = {"OK", "access denied", "unknown command", "invalid argument", "entity exists", "no such entity", "connection refused", "protocol error",
                                  "timeout", "no authentication key", "internal error", "connection terminated", "entity killed", "invalid server", "module initialisation failed",
                                  "bad state", "no data", "incompatible protocol version", "too large", "not supported", "unknown error code", "no such extension", "obsolete functionality",
                                  "missing implementation", "client forked", "input/output error", "device or resource busy"};
    return e < sizeof names / sizeof names[0] ? names[e] : "unknown error";
}

/* Sends a command and waits (up to 3 s) for its reply. On success *reply points to the reply's tagstruct positioned after command and tag.
 * The frame stays in the input buffer until the next call, so the pointer is valid until then. */
static int transact(Pulse *p, TS *t, TR *reply, char *err, unsigned errsz, const char *what)
{
    struct timespec t0, t1;
    unsigned want_tag = p->tag - 1;
    Frame f;
    if (queue_frame(p, 0xFFFFFFFFu, 0, t->b, t->n) != 0) { snprintf(err, errsz, "%s: output buffer full", what); return -1; }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        long ms;
        while (next_frame(p, &f)) {
            if (f.channel == 0xFFFFFFFFu) {
                TR r = {f.data, f.len, 0};
                uint32_t cmd, tag;
                if (tr_u32(&r, &cmd) == 0 && tr_u32(&r, &tag) == 0) {
                    if ((cmd == CMD_REPLY || cmd == CMD_ERROR) && tag == want_tag) {
                        if (cmd == CMD_ERROR) {
                            uint32_t e = 0;
                            tr_u32(&r, &e);
                            snprintf(err, errsz, "%s: the sound server said \"%s\" (error %u)", what, error_name(e), (unsigned)e);
                            consume_frame(p, &f);
                            return -1;
                        }
                        memcpy(p->reply_copy, f.data, f.len);
                        reply->p = p->reply_copy; reply->n = f.len; reply->i = r.i;
                        consume_frame(p, &f);
                        return 0;
                    }
                    handle_command(p, &f);
                }
            }
            consume_frame(p, &f);
        }
        if (p->dead) { snprintf(err, errsz, "%s: the sound server closed the connection", what); return -1; }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms > 3000) { snprintf(err, errsz, "%s: no answer from the sound server in 3 seconds", what); return -1; }
        flush_out(p);
        fill_input(p, 50);
    }
}

/* ---- finding the server ---- */

static int try_connect(const char *path)
{
    struct sockaddr_un a;
    int fd;
    if (strlen(path) >= sizeof a.sun_path) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}

static int load_cookie(unsigned char cookie[COOKIE_LENGTH])
{
    char path[400];
    const char *e;
    const char *cands[4];
    int i, n = 0;
    FILE *f;
    memset(cookie, 0, COOKIE_LENGTH);
    if ((e = getenv("PULSE_COOKIE")) && *e) cands[n++] = e;
    if ((e = getenv("HOME")) && *e) {
        snprintf(path, sizeof path, "%s/.config/pulse/cookie", e);
        cands[n++] = path;
    }
    for (i = 0; i < n; i++) {
        f = fopen(cands[i], "rb");
        if (!f) continue;
        if (fread(cookie, 1, COOKIE_LENGTH, f) == COOKIE_LENGTH) { fclose(f); return 1; }
        fclose(f);
        memset(cookie, 0, COOKIE_LENGTH);
    }
    return 0;
}

/* ---- opening ---- */

Pulse *pulse_open(const char *app_name, unsigned rate, char *err, unsigned errsz)
{
    Pulse *p = (Pulse *)calloc(1, sizeof *p);
    char cand[8][160], tried[1024];
    int ncand = 0, i;
    const char *e;
    unsigned char cookie[COOKIE_LENGTH];
    TS t;
    TR r;
    uint32_t v, ch, idx, req, maxlen, tlen, prebuf, minreq;
    unsigned tlength_bytes = rate * 4u / 5u, minreq_bytes = rate * 4u / 50u;      /* the server keeps up to 200 ms; we keep about the cushion (80 ms) in it, so there is room for a late/early block */
    if (!p) { snprintf(err, errsz, "out of memory"); return NULL; }
    p->out = (unsigned char *)malloc(OUTBUF);
    if (!p->out) { free(p); snprintf(err, errsz, "out of memory"); return NULL; }
    p->fd = -1;
    p->rate = rate;
    p->cushion_ms = 80;

    /* the sockets to try, in the order libpulse uses */
    if ((e = getenv("PULSE_SERVER")) && *e) {
        const char *s = e;
        if (*s == '{') { const char *close_brace = strchr(s, '}'); if (close_brace) s = close_brace + 1; }
        if (strncmp(s, "unix:", 5) == 0) s += 5;
        if (*s == '/') snprintf(cand[ncand++], sizeof cand[0], "%s", s);
    }
    if ((e = getenv("PULSE_RUNTIME_PATH")) && *e) snprintf(cand[ncand++], sizeof cand[0], "%s/native", e);
    if ((e = getenv("XDG_RUNTIME_DIR")) && *e) snprintf(cand[ncand++], sizeof cand[0], "%s/pulse/native", e);
    snprintf(cand[ncand++], sizeof cand[0], "/run/user/%d/pulse/native", (int)getuid());
    snprintf(cand[ncand++], sizeof cand[0], "/var/run/pulse/native");
    snprintf(cand[ncand++], sizeof cand[0], "/run/pulse/native");
    tried[0] = 0;
    for (i = 0; i < ncand && p->fd < 0; i++) {
        p->fd = try_connect(cand[i]);
        if (p->fd >= 0) snprintf(p->path, sizeof p->path, "%s", cand[i]);
        else { size_t l = strlen(tried); snprintf(tried + l, sizeof tried - l, "%s%s (%s)", l ? ", " : "", cand[i], strerror(errno)); }
    }
    if (p->fd < 0) { snprintf(err, errsz, "no sound server found; tried: %s", tried); free(p->out); free(p); return NULL; }

    /* 1. log in */
    load_cookie(cookie);
    t.n = 0;
    ts_u32(&t, CMD_AUTH); ts_u32(&t, new_tag(p)); ts_u32(&t, MY_VERSION); ts_arbitrary(&t, cookie, COOKIE_LENGTH);
    if (transact(p, &t, &r, err, errsz, "login") != 0) goto fail;
    if (tr_u32(&r, &v) != 0) { snprintf(err, errsz, "login: the reply has no version"); goto fail; }
    p->version = (v & 0xFFFFu) < MY_VERSION ? (v & 0xFFFFu) : MY_VERSION;
    if (p->version < 13) { snprintf(err, errsz, "the sound server speaks protocol version %u; at least 13 is needed", p->version); goto fail; }

    /* 2. name this client */
    t.n = 0;
    ts_u32(&t, CMD_SET_CLIENT_NAME); ts_u32(&t, new_tag(p)); ts_proplist(&t, "application.name", app_name);
    if (transact(p, &t, &r, err, errsz, "naming the client") != 0) goto fail;

    /* 3. the playback stream. Fields as in create_stream() of src/pulse/stream.c; the version conditions are the same. */
    t.n = 0;
    ts_u32(&t, CMD_CREATE_PLAYBACK_STREAM); ts_u32(&t, new_tag(p));
    ts_sample_spec(&t, rate);
    ts_channel_map(&t);
    ts_u32(&t, INVALID_INDEX);                   /* sink: the default one */
    ts_str(&t, NULL);                            /* sink name: none */
    ts_u32(&t, tlength_bytes * 4u);              /* maxlength */
    ts_bool(&t, 0);                              /* corked: no, start playing */
    ts_u32(&t, tlength_bytes);                   /* tlength: how much sound the server may hold (200 ms); the credit it gives us is this minus what it holds */
    ts_u32(&t, 0);                               /* prebuf 0: play at once, do not wait for the buffer to fill */
    ts_u32(&t, minreq_bytes);                    /* minreq: ask for more when 20 ms are missing */
    ts_u32(&t, 0);                               /* sync id */
    ts_cvolume(&t);
    if (p->version >= 12) { int k; for (k = 0; k < 7; k++) ts_bool(&t, 0); }          /* no_remap no_remix fix_format fix_rate fix_channels no_move variable_rate */
    if (p->version >= 13) { ts_bool(&t, 0); ts_bool(&t, 0); ts_proplist(&t, "media.name", app_name); }   /* muted, adjust_latency, proplist */
    if (p->version >= 14) { ts_bool(&t, 0); ts_bool(&t, 0); }                           /* volume_set, early_requests */
    if (p->version >= 15) { ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0); }           /* muted_set, dont_inhibit_auto_suspend, fail_on_suspend */
    if (p->version >= 17) ts_bool(&t, 0);                                               /* relative_volume */
    if (p->version >= 18) ts_bool(&t, 0);                                               /* passthrough */
    if (p->version >= 21) ts_byte(&t, 0);                                                 /* n_formats: none, the sample spec above is used */
    if (transact(p, &t, &r, err, errsz, "opening the playback stream") != 0) goto fail;
    if (tr_u32(&r, &ch) || tr_u32(&r, &idx) || tr_u32(&r, &req)) { snprintf(err, errsz, "opening the playback stream: short reply"); goto fail; }
    p->channel = ch;
    p->requested = req;
    if (p->version >= 9 && tr_u32(&r, &maxlen) == 0 && tr_u32(&r, &tlen) == 0 && tr_u32(&r, &prebuf) == 0 && tr_u32(&r, &minreq) == 0)
        fprintf(stderr, "kyujo: audio: PulseAudio stream open on %s: protocol %u, buffer %u bytes, prebuf %u, minreq %u, first request %u bytes\n",
                p->path, p->version, (unsigned)tlen, (unsigned)prebuf, (unsigned)minreq, (unsigned)req);
    (void)idx; (void)maxlen;
    /* from now on nothing blocks */
    fcntl(p->fd, F_SETFL, fcntl(p->fd, F_GETFL, 0) | O_NONBLOCK);
    return p;
fail:
    close(p->fd);
    free(p->out);
    free(p);
    return NULL;
}

void pulse_close(Pulse *p)
{
    if (!p) return;
    if (p->fd >= 0) close(p->fd);
    free(p->out);
    free(p);
}

void pulse_set_cushion_ms(Pulse *p, unsigned ms) { if (p) p->cushion_ms = ms > 400 ? 400 : ms; }
int pulse_alive(const Pulse *p) { return p && !p->dead; }
const char *pulse_server_path(const Pulse *p) { return p ? p->path : ""; }
unsigned pulse_version(const Pulse *p) { return p ? p->version : 0; }
unsigned pulse_requests(const Pulse *p) { return p ? p->requests : 0; }
unsigned pulse_underruns(const Pulse *p) { return p ? p->underruns : 0; }

/* Sends `bytes` of sound as one frame, as much of it as the server wants. Returns the bytes sent. */
static unsigned send_sound(Pulse *p, const void *pcm, unsigned bytes)
{
    unsigned n = bytes < p->requested ? bytes : p->requested;
    n &= ~3u;                                    /* whole frames only */
    if (n == 0) return 0;
    if (n > 60000) n = 60000;                    /* stays far below the largest frame */
    if (queue_frame(p, p->channel, 0 /* seek mode RELATIVE */, pcm, n) != 0) return 0;
    p->requested -= n;
    return n;
}

unsigned pulse_write(Pulse *p, const void *pcm, unsigned bytes)
{
    unsigned done = 0;
    if (!p || p->dead) return 0;
    pump(p);
    if (p->dead) return 0;
    if (!p->primed) {                            /* at the start and after an underrun: a cushion of silence first */
        static unsigned char silence[4096];
        unsigned want = p->cushion_ms * p->rate / 1000u * 4u;
        while (want > 0) {
            unsigned n = want < sizeof silence ? want : (unsigned)sizeof silence;
            if (send_sound(p, silence, n) != n) break;
            want -= n;
        }
        p->primed = 1;
    }
    while (done < bytes) {
        unsigned n = send_sound(p, (const unsigned char *)pcm + done, bytes - done);
        if (n == 0) break;
        done += n;
    }
    p->dropped_total += bytes - done;
    return done;
}

#endif /* __linux__ */
