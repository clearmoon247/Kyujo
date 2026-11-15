/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * jpeg_ref_check.c - compares src/render/jpeg_task.c with an independent decoder (see jpeg_ref_prep.py).
 * Usage: jpeg_ref_check <prefix>. Reads <prefix>.mcus/.q/.ref/.info, converts every MCU with jpeg_mcu_to_rgba5551 and reports the
 * difference to the reference (each channel expanded from 5 bits back to 8 as v*8+4 for the comparison; the reference is exact 8-bit,
 * so differences up to 4 levels are the 5-bit quantisation alone). Exit status 0 when the mean absolute error is under 6 levels and
 * no pixel is off by more than 40 (libjpeg, which Pillow uses, smooths the chroma when upsampling; this port replicates it, so sharp colour edges differ more).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "jpeg_task.h"

static uint8_t *slurp(const char *prefix, const char *ext, size_t *n)
{
    char path[512]; FILE *f; uint8_t *b;
    snprintf(path, sizeof path, "%s.%s", prefix, ext);
    f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n) { perror("read"); exit(2); }
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    size_t nm, nq, nr, ni; uint8_t *mcus, *q, *ref, *info; unsigned w, h, mx, my, x, y, i;
    uint16_t qy[64], qc[64]; double sum = 0; unsigned maxd = 0; unsigned long cnt = 0;
    if (argc < 2) { fprintf(stderr, "usage: jpeg_ref_check <prefix>\n"); return 2; }
    mcus = slurp(argv[1], "mcus", &nm); q = slurp(argv[1], "q", &nq); ref = slurp(argv[1], "ref", &nr); info = slurp(argv[1], "info", &ni);
    info[ni] = 0;
    if (sscanf((char *)info, "%u %u", &w, &h) != 2) return 2;
    for (i = 0; i < 64; i++) { qy[i] = (uint16_t)((q[2 * i] << 8) | q[2 * i + 1]); qc[i] = (uint16_t)((q[128 + 2 * i] << 8) | q[128 + 2 * i + 1]); }
    mx = (w + 15) / 16; my = (h + 15) / 16;
    for (y = 0; y < my; y++) for (x = 0; x < mx; x++) {
        uint8_t px[0x200]; unsigned py, pxx;
        jpeg_mcu_to_rgba5551(mcus + (size_t)(y * mx + x) * 0x300, qy, qc, qc, px);
        for (py = 0; py < 16; py++) for (pxx = 0; pxx < 16; pxx++) {
            unsigned X = x * 16 + pxx, Y = y * 16 + py, v, c;
            if (X >= w || Y >= h) continue;
            v = (unsigned)((px[(py * 16 + pxx) * 2] << 8) | px[(py * 16 + pxx) * 2 + 1]);
            for (c = 0; c < 3; c++) {
                unsigned got = ((v >> (11 - 5 * c)) & 31) * 8 + 4, want = ref[(Y * w + X) * 3 + c];
                unsigned d = got > want ? got - want : want - got;
                sum += d; cnt++; if (d > maxd) maxd = d;
            }
        }
    }
    printf("%ux%u: mean abs error %.3f levels, max %u\n", w, h, sum / (double)cnt, maxd);
    return (sum / (double)cnt < 6.0 && maxd <= 40) ? 0 : 1;
}
