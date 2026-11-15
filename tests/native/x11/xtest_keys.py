#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Sends fake key presses to an X server with the XTEST extension, speaking the X protocol directly (no Xlib is needed).
Used to test src/platform/host_x11.c against Xvfb: usage  xtest_keys.py DISPLAY_NUMBER  KEYCODE[:HOLD_SECONDS] ...
Each key is pressed, held, released, then a short pause follows."""
import socket, struct, sys, time

def connect(num):
    s = socket.socket(socket.AF_UNIX)
    s.connect("/tmp/.X11-unix/X%d" % num)
    s.sendall(struct.pack("<BxHHHHxx", ord("l"), 11, 0, 0, 0))          # no authorization
    head = s.recv(8)
    if head[0] != 1:
        raise SystemExit("X server refused: %r" % head)
    n = struct.unpack("<H", head[6:8])[0] * 4
    body = b""
    while len(body) < n:
        body += s.recv(n - len(body))
    return s

def query_extension(s, name):
    nb = name.encode()
    pad = (-len(nb)) % 4
    s.sendall(struct.pack("<BxHHxx", 98, 2 + (len(nb) + pad) // 4, len(nb)) + nb + b"\0" * pad)
    r = s.recv(32)
    if r[0] != 1 or not r[8]:
        raise SystemExit("extension %s missing" % name)
    return r[9]

def fake_key(s, major, press, keycode):
    s.sendall(struct.pack("<BBH", major, 2, 9) + struct.pack("<BBxxIIxxxxxxxxHHxxxxxxxB", 2 if press else 3, keycode, 0, 0, 0, 0, 0))

if __name__ == "__main__":
    s = connect(int(sys.argv[1]))
    major = query_extension(s, "XTEST")
    for arg in sys.argv[2:]:
        code, _, hold = arg.partition(":")
        fake_key(s, major, True, int(code))
        time.sleep(float(hold or 0.3))
        fake_key(s, major, False, int(code))
        time.sleep(0.2)
