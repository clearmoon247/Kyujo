#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""A tiny fake window manager, only for testing full screen (src/platform/host_x11.c) on Xvfb, which has no window manager.

It does what an EWMH window manager does for this program, no more:
  - MapRequest: shows the window; if the window asked for _NET_WM_STATE_FULLSCREEN in its _NET_WM_STATE property it is first made as big
    as the screen;
  - ConfigureRequest: grants it;
  - ClientMessage _NET_WM_STATE sent to the root window (data: action 0 = remove, 1 = add, 2 = toggle; the state atom): sets or clears the
    property on the window and resizes it to the screen / back to its previous size.
It is written from the EWMH specification, so it proves that the program sends what the specification says, not that a real window manager
(Batocera uses openbox) agrees.
usage: fake_wm.py DISPLAY_NUMBER
"""
import socket, struct, sys

def main():
    num = int(sys.argv[1])
    s = socket.socket(socket.AF_UNIX); s.connect("/tmp/.X11-unix/X%d" % num)
    s.sendall(struct.pack("<BxHHHHxx", ord("l"), 11, 0, 0, 0))
    head = s.recv(8)
    n = struct.unpack("<H", head[6:8])[0] * 4
    body = b""
    while len(body) < n: body += s.recv(n - len(body))
    vendor_len, nfmt = struct.unpack("<H", body[16:18])[0], body[21]
    off = 32 + ((vendor_len + 3) & ~3) + nfmt * 8
    root = struct.unpack("<I", body[off:off + 4])[0]
    sw, sh = struct.unpack("<HH", body[off + 20:off + 24])
    seq = 0
    def req(opcode, data, payload):
        nonlocal seq
        pad = (-len(payload)) % 4
        s.sendall(struct.pack("<BBH", opcode, data, (4 + len(payload) + pad) // 4) + payload + b"\0" * pad); seq += 1; return seq
    def recv_exact(k):
        b = b""
        while len(b) < k:
            c = s.recv(k - len(b))
            if not c: raise SystemExit("server gone")
            b += c
        return b
    def read_packet():
        p = recv_exact(32)
        if p[0] == 1:
            extra = struct.unpack("<I", p[4:8])[0] * 4
            return p, recv_exact(extra) if extra else b""
        return p, b""
    def intern(name):
        want = req(16, 0, struct.pack("<HH", len(name), 0) + name.encode())
        while True:
            p, extra = read_packet()
            if p[0] == 1: return struct.unpack("<I", p[8:12])[0]
    a_state, a_full = intern("_NET_WM_STATE"), intern("_NET_WM_STATE_FULLSCREEN")
    # become the window manager: select SubstructureRedirect | SubstructureNotify on the root window
    req(2, 0, struct.pack("<II", root, 0x800) + struct.pack("<I", 0x180000))
    saved = {}
    def get_state(win):
        req(20, 0, struct.pack("<IIIII", win, a_state, 0, 0, 64))
        while True:
            p, extra = read_packet()
            if p[0] == 1:
                n = struct.unpack("<I", p[16:20])[0]
                return [struct.unpack("<I", extra[i*4:i*4+4])[0] for i in range(n)] if p[1] == 32 else []
    def set_state(win, atoms):
        req(18, 0, struct.pack("<IIIB3xI", win, a_state, 4, 32, len(atoms)) + b"".join(struct.pack("<I", a) for a in atoms))
    def get_geometry(win):
        req(14, 0, struct.pack("<I", win))
        while True:
            p, extra = read_packet()
            if p[0] == 1: return struct.unpack("<hhHH", p[12:20])
    def resize(win, x, y, w, h):
        req(12, 0, struct.pack("<IHxx", win, 0x0F) + struct.pack("<iiII", x, y, w, h))
    print("fake wm: running on display %d (screen %dx%d)" % (num, sw, sh), flush=True)
    while True:
        p, _ = read_packet()
        t = p[0] & 0x7f
        if t == 20:                                           # MapRequest: parent @4, window @8
            win = struct.unpack("<I", p[8:12])[0]
            if a_full in get_state(win):
                saved[win] = get_geometry(win)
                resize(win, 0, 0, sw, sh)
            req(8, 0, struct.pack("<I", win))
        elif t == 23:                                         # ConfigureRequest: window @8, value mask @26 ... grant width/height
            win = struct.unpack("<I", p[8:12])[0]
            x, y, w, h = struct.unpack("<hhHH", p[16:24])
            resize(win, x, y, w, h)
        elif t == 33:                                         # ClientMessage: window @4, type @8, data @12
            win, mtype = struct.unpack("<II", p[4:12])
            action, first = struct.unpack("<II", p[12:20])
            if mtype == a_state and first == a_full:
                cur = get_state(win)
                on = a_full in cur
                want = (action == 1) or (action == 2 and not on)
                print("fake wm: _NET_WM_STATE_FULLSCREEN action=%d -> %s" % (action, "on" if want else "off"), flush=True)
                if want and not on:
                    saved[win] = get_geometry(win)
                    set_state(win, cur + [a_full]); resize(win, 0, 0, sw, sh)
                elif not want and on:
                    set_state(win, [a for a in cur if a != a_full]); x, y, w, h = saved.get(win, (0, 0, 640, 480)); resize(win, x, y, w, h)

if __name__ == "__main__":
    main()
