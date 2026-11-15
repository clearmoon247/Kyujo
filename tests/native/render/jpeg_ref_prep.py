#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""
jpeg_ref_prep.py - prepares an INDEPENDENT check of src/render/jpeg_task.c.

It takes a baseline 4:2:0 JPEG (here: one cut out of the player's own ROM, never committed), Huffman-decodes the first MCUs itself
(own implementation of ITU-T T.81 decoding, not the game's code) into exactly the layout Jpeg_DecodeMcus leaves in memory (6 blocks of
64 big-endian s16 in zig-zag order, DC already accumulated), and writes
   <out>.mcus   the coefficient blocks of every MCU, concatenated (0x300 bytes each)
   <out>.q      Y table then chroma table, 64 big-endian u16 each, zig-zag order
   <out>.ref    Pillow's decode of the same JPEG as RGB888 (width*height*3) - the ground truth
   <out>.info   "width height"
and jpeg_ref_check.c turns .mcus into pixels with the host code so they can be compared with .ref.
Usage: jpeg_ref_prep.py <file.jpg> <out-prefix>
"""
import struct, sys
from PIL import Image

def parse(d):
    i = 2; qt = {}; ht = {}; sof = None; sos = None
    while i < len(d):
        assert d[i] == 0xFF
        m = d[i + 1]; ln = struct.unpack('>H', d[i + 2:i + 4])[0]; seg = d[i + 4:i + 2 + ln]
        if m == 0xDB:
            j = 0
            while j < len(seg):
                pq_tq = seg[j]; assert pq_tq >> 4 == 0
                qt[pq_tq & 15] = list(seg[j + 1:j + 65]); j += 65
        elif m == 0xC4:
            j = 0
            while j < len(seg):
                tc_th = seg[j]; counts = list(seg[j + 1:j + 17]); n = sum(counts)
                ht[(tc_th >> 4, tc_th & 15)] = (counts, list(seg[j + 17:j + 17 + n])); j += 17 + n
        elif m == 0xC0:
            sof = seg
        elif m == 0xDA:
            sos = seg; i += 2 + ln; break
        i += 2 + ln
    return qt, ht, sof, sos, d[i:]

def build_codes(counts, syms):
    codes = {}; code = 0; k = 0
    for ln in range(1, 17):
        for _ in range(counts[ln - 1]):
            codes[(ln, code)] = syms[k]; k += 1; code += 1
        code <<= 1
    return codes

class Bits:
    def __init__(s, data):
        out = bytearray(); i = 0
        while i < len(data):
            b = data[i]
            if b == 0xFF:
                if data[i + 1] == 0: out.append(0xFF); i += 2; continue
                break
            out.append(b); i += 1
        s.d = bytes(out); s.p = 0
    def bit(s):
        b = (s.d[s.p >> 3] >> (7 - (s.p & 7))) & 1; s.p += 1; return b
    def bits(s, n):
        v = 0
        for _ in range(n): v = (v << 1) | s.bit()
        return v

def sym(b, codes):
    code = 0
    for ln in range(1, 17):
        code = (code << 1) | b.bit()
        if (ln, code) in codes: return codes[(ln, code)]
    raise ValueError('bad code')

def extend(v, n):
    return v if n == 0 or v >= (1 << (n - 1)) else v - (1 << n) + 1

def main():
    d = open(sys.argv[1], 'rb').read(); out = sys.argv[2]
    qt, ht, sof, sos, scan = parse(d)
    h, w = struct.unpack('>HH', sof[1:5]); ncomp = sof[5]
    assert ncomp == 3 and sof[7] == 0x22 and sof[10] == 0x11 and sof[13] == 0x11, 'need 4:2:0 baseline'
    # scan component -> table selectors
    tabs = {}
    for c in range(sos[0]): tabs[sos[1 + 2 * c]] = (sos[2 + 2 * c] >> 4, sos[2 + 2 * c] & 15)
    codes = {k: build_codes(*v) for k, v in ht.items()}
    b = Bits(scan); pred = {1: 0, 2: 0, 3: 0}
    def block(comp):
        dcT, acT = tabs[comp]; co = [0] * 64
        n = sym(b, codes[(0, dcT)]); pred[comp] += extend(b.bits(n), n) if n else 0; co[0] = pred[comp]
        k = 1
        while k < 64:
            rs = sym(b, codes[(1, acT)]); r, s = rs >> 4, rs & 15
            if s == 0:
                if r == 15: k += 16; continue
                break
            k += r; co[k] = extend(b.bits(s), s); k += 1
        return co
    mcus = bytearray()
    for _ in range(((h + 15) // 16) * ((w + 15) // 16)):
        for comp in (1, 1, 1, 1, 2, 3):
            mcus += b''.join(struct.pack('>h', c) for c in block(comp))
    open(out + '.mcus', 'wb').write(mcus)
    qy, qc = qt[0], qt[1]
    open(out + '.q', 'wb').write(b''.join(struct.pack('>H', x) for x in qy + qc))
    im = Image.open(sys.argv[1]).convert('RGB'); assert im.size == (w, h)
    open(out + '.ref', 'wb').write(im.tobytes())
    open(out + '.info', 'w').write('%d %d\n' % (w, h))
    print('ok', w, h, len(mcus) // 0x300, 'MCUs')
main()
