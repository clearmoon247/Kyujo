#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""A fake PulseAudio server for testing src/platform/host_pulse.c without a real server.

It reads the client's commands field by field in the order protocol-native.c of PulseAudio reads them (command_auth, command_set_client_name,
command_create_playback_stream; PulseAudio source, version 35 field list) and is strict: a wrong tag, a missing or an extra field is an error.
It then behaves like a sound card: it consumes 192000 bytes per second (48 kHz, 16 bit, stereo) and asks for more with REQUEST commands, so the
client's flow control is exercised. At the end it writes what it received to a WAV file.
What this proves: the client writes what THIS script (and the source it was written from) expects. It does not prove a real server agrees.

usage: fake_pulse_server.py SOCKET_PATH OUT_WAV [SECONDS_TO_RUN] [--underflow-at SECONDS] [--suspend-after SECONDS]

--suspend-after S models a failure that is SUSPECTED on a real server (PipeWire stops a stream that has been idle for a while and does not
restart it by itself): when no sound has arrived on a connection for S seconds, the stream is "suspended": the server sends one command (number
80, a stand-in) and from then on throws away everything that arrives on that connection (while still asking for more, as the Batocera log suggests:
the program did not report any dropped sound). A NEW connection starts with a working stream. This is
a model made for testing the program's defences (silence while the menu is open, a new stream after a long pause); it is not a copy of a real
server's behaviour.
The server serves one connection after the other until SECONDS_TO_RUN is over; what every connection delivered is written to the WAV, in order.
"""
import os, socket, struct, sys, threading, time, wave

CMD_ERROR, CMD_REPLY, CMD_CREATE_PLAYBACK_STREAM, CMD_AUTH, CMD_SET_CLIENT_NAME = 0, 2, 3, 8, 9
CMD_REQUEST, CMD_UNDERFLOW = 61, 63
SERVER_VERSION = 35

class TS:
    """tagstruct reader; every get_* checks the tag byte"""
    def __init__(self, data): self.d, self.i = data, 0
    def tag(self, t):
        if self.i >= len(self.d) or self.d[self.i:self.i+1] != t:
            raise ValueError("expected tag %r at %d, got %r" % (t, self.i, self.d[self.i:self.i+1]))
        self.i += 1
    def u32(self): self.tag(b'L'); v = struct.unpack(">I", self.d[self.i:self.i+4])[0]; self.i += 4; return v
    def u8(self): self.tag(b'B'); v = self.d[self.i]; self.i += 1; return v
    def boolean(self):
        if self.d[self.i:self.i+1] not in (b'0', b'1'): raise ValueError("expected boolean at %d" % self.i)
        v = self.d[self.i:self.i+1] == b'1'; self.i += 1; return v
    def string(self):
        if self.d[self.i:self.i+1] == b'N': self.i += 1; return None
        self.tag(b't'); e = self.d.index(b'\0', self.i); v = self.d[self.i:e].decode(); self.i = e + 1; return v
    def arbitrary(self): self.tag(b'x'); n = struct.unpack(">I", self.d[self.i:self.i+4])[0]; self.i += 4; v = self.d[self.i:self.i+n]; self.i += n; return v
    def sample_spec(self): self.tag(b'a'); f, c, r = struct.unpack(">BBI", self.d[self.i:self.i+6]); self.i += 6; return f, c, r
    def channel_map(self): self.tag(b'm'); n = self.d[self.i]; self.i += 1 + n; return n
    def cvolume(self): self.tag(b'v'); n = self.d[self.i]; self.i += 1 + 4 * n; return n
    def proplist(self):
        self.tag(b'P'); props = {}
        while True:
            k = self.string()
            if k is None: return props
            n = self.u32(); props[k] = self.arbitrary()
            assert len(props[k]) == n
    def eof(self):
        if self.i != len(self.d): raise ValueError("%d unread bytes at the end" % (len(self.d) - self.i))

def w32(v): return b'L' + struct.pack(">I", v)
def frame(channel, payload, flags=0): return struct.pack(">IIIII", len(payload), channel, 0, 0, flags) + payload
def reply(tag, body=b''): return frame(0xFFFFFFFF, w32(CMD_REPLY) + w32(tag) + body)

def serve(conn, deadline, got_all, underflow_at, suspend_after, log):
    """Serves one connection until the client closes it or the deadline passes. Sound that was 'played' is appended to got_all."""
    rx = b''
    def read_frame():
        nonlocal rx
        while True:
            if len(rx) >= 20:
                n, ch, oh, ol, fl = struct.unpack(">IIIII", rx[:20])
                if len(rx) >= 20 + n:
                    p = rx[20:20+n]; rx = rx[20+n:]; return ch, fl, p
            d = conn.recv(65536)
            if not d: return None
            rx += d
    version = None; channel = 5; wanted = 19200; got = bytearray(); lock = threading.Lock(); first_data = None
    last_data = [None]; suspended = [False]
    stop = threading.Event()
    def sender_loop():
        # the "sound card": it plays 192000 bytes per second, exactly (the time of every step is computed from the start, so sleeping a little
        # too long does not slow it down), and asks for as much more as it has played. Playing starts when the first sound arrives.
        played = 0; did_underflow = False; t_start = None
        while not stop.is_set():
            time.sleep(0.005)
            try:
                with lock:
                    if first_data is None: continue
                    if t_start is None: t_start = time.time()
                    if suspend_after and not suspended[0] and time.time() - last_data[0] > suspend_after:     # the card has had nothing to play for that long
                        suspended[0] = True
                        log("stream suspended (no sound for %.1f s)" % (time.time() - last_data[0]))
                        conn.sendall(frame(0xFFFFFFFF, w32(80) + w32(0xFFFFFFFF) + w32(channel) + b'1'))
                    due = int((time.time() - t_start) * 192000) & ~3
                    n = min(due - played, len(got) - played)
                    if n >= 3840 or (n > 0 and n >= due - played and n >= 1920):
                        n &= ~3
                        played += n
                        conn.sendall(frame(0xFFFFFFFF, w32(CMD_REQUEST) + w32(0xFFFFFFFF) + w32(channel) + w32(n)))
                    if underflow_at and not did_underflow and time.time() - t_start > underflow_at:
                        did_underflow = True
                        conn.sendall(frame(0xFFFFFFFF, w32(CMD_UNDERFLOW) + w32(0xFFFFFFFF) + w32(channel) + b'r' + struct.pack(">q", 0)))
            except OSError:
                return                                          # the client has gone
    threading.Thread(target=sender_loop, daemon=True).start()
    conn.settimeout(0.5)
    while time.time() < deadline:
        try: f = read_frame()
        except socket.timeout: continue
        except ConnectionResetError: break               # the client closed the connection
        if f is None: break
        ch, fl, p = f
        if ch != 0xFFFFFFFF:
            assert fl & 0xFF == 0, "seek mode must be RELATIVE (0)"
            with lock:
                if suspended[0]:                             # the model of a stopped stream: the sound is thrown away, but the server goes on asking for more
                    conn.sendall(frame(0xFFFFFFFF, w32(CMD_REQUEST) + w32(0xFFFFFFFF) + w32(channel) + w32(len(p))))   # (the user's Batocera log shows no dropped sound, so the server kept accepting it)
                    continue
                if first_data is None: first_data = time.time()
                last_data[0] = time.time()
                got += p
            continue
        t = TS(p); cmd = t.u32(); tag = t.u32()
        if cmd == CMD_AUTH:
            v = t.u32(); cookie = t.arbitrary(); t.eof()
            assert len(cookie) == 256, "cookie must be 256 bytes"
            assert v & 0xC0000000 == 0, "no shm/memfd flags expected"
            version = min(v & 0xFFFF, SERVER_VERSION)
            conn.sendall(reply(tag, w32(SERVER_VERSION)))
            log("AUTH ok, client version %d -> using %d" % (v & 0xFFFF, version))
        elif cmd == CMD_SET_CLIENT_NAME:
            props = t.proplist(); t.eof()
            log("client name: %s" % props)
            conn.sendall(reply(tag, w32(1)))
        elif cmd == CMD_CREATE_PLAYBACK_STREAM:
            spec = t.sample_spec(); cm = t.channel_map(); sink = t.u32(); sinkname = t.string()
            maxlength = t.u32(); corked = t.boolean(); tlength = t.u32(); prebuf = t.u32(); minreq = t.u32(); sync = t.u32(); t.cvolume()
            flags = [t.boolean() for _ in range(7)]                       # version >= 12
            muted = t.boolean(); adjust = t.boolean(); props = t.proplist()   # >= 13
            volume_set = t.boolean(); early = t.boolean()                 # >= 14
            muted_set = t.boolean(); inhibit = t.boolean(); fail = t.boolean()   # >= 15
            relative = t.boolean()                                        # >= 17
            passthrough = t.boolean()                                     # >= 18
            nformats = t.u8()                                             # >= 21
            for _ in range(nformats): raise ValueError("formats not expected")
            t.eof()
            log("stream: spec %s chmap %s sink %s %s maxlength %d corked %s tlength %d prebuf %d minreq %d %s" % (spec, cm, hex(sink), sinkname, maxlength, corked, tlength, prebuf, minreq, props))
            assert spec == (3, 2, 48000) and sink == 0xFFFFFFFF and not corked
            wanted = tlength
            body = w32(channel) + w32(77) + w32(tlength)
            body += w32(maxlength) + w32(tlength) + w32(prebuf) + w32(minreq)                 # >= 9
            body += b'a' + struct.pack(">BBI", 3, 2, 48000) + b'm\x02\x01\x02' + w32(0) + b't' + b'fake sink\0' + b'0'   # >= 12
            body += b'U' + struct.pack(">Q", 20000)                                          # >= 13
            body += b'f' + b'B\x01' + b'PN'                                                  # >= 21: format info (PCM, empty proplist)
            conn.sendall(reply(tag, body))
            conn.sendall(frame(0xFFFFFFFF, w32(CMD_REQUEST) + w32(0xFFFFFFFF) + w32(channel) + w32(0)))   # a zero-size request must be harmless
        else:
            raise ValueError("unexpected command %d" % cmd)
    stop.set()
    got_all += got
    conn.close()

def main():
    path, out_wav = sys.argv[1], sys.argv[2]
    run_for = float(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith('--') else 6.0
    underflow_at = float(sys.argv[sys.argv.index('--underflow-at') + 1]) if '--underflow-at' in sys.argv else None
    suspend_after = float(sys.argv[sys.argv.index('--suspend-after') + 1]) if '--suspend-after' in sys.argv else None
    if os.path.exists(path): os.unlink(path)
    srv = socket.socket(socket.AF_UNIX); srv.bind(path); srv.listen(1)
    srv.settimeout(0.5)
    deadline = time.time() + run_for
    got_all = bytearray()
    t0 = time.time()
    def log(msg): print("[%6.2f s] %s" % (time.time() - t0, msg), flush=True)
    n_conn = 0
    while time.time() < deadline:
        try: conn, _ = srv.accept()
        except socket.timeout: continue
        n_conn += 1
        log("connection %d" % n_conn)
        serve(conn, deadline, got_all, underflow_at, suspend_after, log)
    with wave.open(out_wav, 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000); w.writeframes(bytes(got_all))
    print("received", len(got_all), "bytes of sound in", n_conn, "connection(s)")

if __name__ == "__main__":
    main()
