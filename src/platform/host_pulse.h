/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_pulse.h - plays 16-bit stereo sound through a PulseAudio server (or PipeWire's PulseAudio server), speaking the server's native protocol
 * on its unix socket directly. No library is needed, which is what lets the static 32-bit Linux program make sound on a system that has only
 * 64-bit audio libraries (Batocera is such a system). Batocera uses PipeWire, which offers this protocol through "pipewire-pulse".
 *
 * One Pulse belongs to one thread (the thread that calls pulse_write). Nothing here blocks the caller after pulse_open() returns: data that
 * the server does not want yet is dropped (the game produces sound at the pace of its clock, like the Windows version).
 * HOST-side file (Linux).
 */
#ifndef HOST_PULSE_H
#define HOST_PULSE_H

#include <stdint.h>

typedef struct Pulse Pulse;

/* Connects, logs in, and opens a playback stream (interleaved signed 16-bit little-endian, 2 channels, `rate` Hz). Waits at most about 3 seconds.
 * Returns NULL on failure with a one-line reason in err (it names every socket that was tried). */
Pulse *pulse_open(const char *app_name, unsigned rate, char *err, unsigned errsz);
void pulse_close(Pulse *p);

/* Sends up to `bytes` bytes (a multiple of 4) of sound. Returns how many were accepted; the rest is dropped because the server's buffer is
 * full. Before the first sound (and after the server ran out of data) `cushion_ms` of silence is sent first, so that a late block is not heard
 * as a gap. Also reads what the server has sent (requests for more data). */
unsigned pulse_write(Pulse *p, const void *pcm, unsigned bytes);
void pulse_set_cushion_ms(Pulse *p, unsigned ms);
int pulse_alive(const Pulse *p);                      /* 0 once the connection is lost or the server closed the stream */

const char *pulse_server_path(const Pulse *p);
unsigned pulse_version(const Pulse *p);               /* the protocol version both sides use */
unsigned pulse_requests(const Pulse *p);              /* data requests received from the server */
unsigned pulse_underruns(const Pulse *p);             /* times the server ran out of data */

#endif
