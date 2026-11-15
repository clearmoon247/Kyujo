/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * jpeg_task.c - the game's JPEG microcode ("njpgdsp", RSP task type 4), done on the host.
 *
 * WHAT THE GAME DOES (decomp src/jpeg_stream.c, Jpeg_DecodeImage; src/jpeg_decoder.c, Jpeg_DecodeMcus):
 *   For every 16x16-pixel block (MCU) of the picture, the CPU Huffman-decodes the block's coefficients into the output buffer and then
 *   starts an RSP task with a 0x20-byte parameter block (JpegTaskData, src/jpeg_decoder.h). The task turns the coefficients into pixels
 *   IN PLACE (input and output start at the same address); the CPU then moves on 0x200 bytes (= 16*16 pixels * 2 bytes) for the next MCU.
 *
 *   Input, as Jpeg_DecodeMcus leaves it, for mode 2 (4:2:0, the only mode this game's pictures use: the decomp's comment in
 *   Jpeg_ParseStreamMarkers says every JPEG in the ROM has component id 0x22):
 *       6 blocks of 64 big-endian s16 coefficients, 0x300 bytes: Y(top-left), Y(top-right), Y(bottom-left), Y(bottom-right), Cb, Cr.
 *       Coefficients are in the file's ZIG-ZAG order (Jpeg_DecodeBlock stores them in the order decoded, it never un-zigzags).
 *       The DC coefficient is already the running sum (Jpeg_DecodeBlock: `*unk += coeff; mcu[0] = *unk`), i.e. absolute, not a difference.
 *   Parameter block (all big-endian words): +00 output/input address, +04 number of MCUs, +08 mode (0 or 2),
 *       +0C / +10 / +14 physical addresses of the Y / Cb / Cr quantisation tables: 64 big-endian u16 each, in zig-zag order too
 *       (JpegUtils_ProcessQuantizationTable copies the DQT bytes in file order).
 *   Output: 16x16 pixels, row by row, RGBA5551 big-endian (the layout of OoT's Jpeg_CopyToZbuffer, which copies 16 pixels per row, is
 *       the same ABI in the sibling decomp; here it is checked against the picture the game draws and against an independent decoder).
 *
 * WHAT IS NOT KNOWN (no documentation of the microcode itself was found; its machine code is not executed here):
 *   - the exact fixed-point arithmetic of the original (this uses a floating-point inverse DCT, so pixel values may differ by a few
 *     levels from the console's),
 *   - how it upsamples the chroma (here: each chroma sample covers 2x2 pixels, the simplest reading of 4:2:0),
 *   - how it rounds to 5 bits per channel (here: truncation, value >> 3),
 *   - what a count other than 1, or mode 0, does (the game never uses them; they are reported and ignored).
 *
 * HOST-side. Reads/writes game memory at native addresses (native pointer == physical address, see src/platform/platform.h).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "jpeg_task.h"

/* Zig-zag order: ZIGZAG[k] = index (row*8+col) in the natural 8x8 block of the k-th coefficient in the stream. ITU-T T.81 Figure A.6. */
static const uint8_t ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static float sCos[8][8];            /* sCos[x][u] = C(u)/2 * cos((2x+1) u pi / 16): one-dimensional inverse DCT basis, T.81 A.3.3 */
static int sCosReady;

static void init_cos(void)
{
    int x, u;
    for (x = 0; x < 8; x++) {
        for (u = 0; u < 8; u++) {
            double c = (u == 0) ? 0.70710678118654752440 : 1.0;
            sCos[x][u] = (float)(0.5 * c * cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
        }
    }
    sCosReady = 1;
}

static int16_t rd16(const uint8_t *p) { return (int16_t)(((unsigned)p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* Dequantise, un-zig-zag and inverse-DCT one block. `coef`: 64 big-endian s16 in zig-zag order; `q`: 64 host u16 in zig-zag order.
 * Result: 64 samples, level shifted (+128) and clamped to 0..255, natural order. */
static void idct_block(const uint8_t *coef, const uint16_t *q, float out[64])
{
    float f[64], tmp[64];
    int k, x, y, u, v;

    memset(f, 0, sizeof f);
    for (k = 0; k < 64; k++) {
        f[ZIGZAG[k]] = (float)rd16(coef + 2 * k) * (float)q[k];
    }
    for (v = 0; v < 8; v++) {                       /* columns: tmp[v][x] = sum_u f[v][u] * cos(x,u) */
        for (x = 0; x < 8; x++) {
            float s = 0;
            for (u = 0; u < 8; u++) s += f[v * 8 + u] * sCos[x][u];
            tmp[v * 8 + x] = s;
        }
    }
    for (x = 0; x < 8; x++) {                       /* rows */
        for (y = 0; y < 8; y++) {
            float s = 0;
            for (v = 0; v < 8; v++) s += tmp[v * 8 + x] * sCos[y][v];
            s += 128.0f;
            out[y * 8 + x] = s < 0 ? 0 : (s > 255 ? 255 : s);
        }
    }
}

static uint8_t clamp8(float v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)(v + 0.5f)); }

void jpeg_mcu_to_rgba5551(const uint8_t *coef, const uint16_t *qy, const uint16_t *qc_u, const uint16_t *qc_v, uint8_t *out)
{
    float blk[6][64];
    int b, x, y;

    if (!sCosReady) init_cos();
    for (b = 0; b < 4; b++) idct_block(coef + b * 128, qy, blk[b]);
    idct_block(coef + 4 * 128, qc_u, blk[4]);
    idct_block(coef + 5 * 128, qc_v, blk[5]);

    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            float Y = blk[(y >> 3) * 2 + (x >> 3)][(y & 7) * 8 + (x & 7)];
            float cb = blk[4][(y >> 1) * 8 + (x >> 1)] - 128.0f;     /* 4:2:0, each chroma sample covers 2x2 pixels */
            float cr = blk[5][(y >> 1) * 8 + (x >> 1)] - 128.0f;
            /* JFIF colour conversion (JFIF 1.02, "Conversion to and from RGB") */
            uint8_t r = clamp8(Y + 1.402f * cr);
            uint8_t g = clamp8(Y - 0.344136f * cb - 0.714136f * cr);
            uint8_t bl = clamp8(Y + 1.772f * cb);
            uint16_t px = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((bl >> 3) << 1) | 1);
            out[(y * 16 + x) * 2] = (uint8_t)(px >> 8);
            out[(y * 16 + x) * 2 + 1] = (uint8_t)px;
        }
    }
}

/* Reads a 64-entry big-endian u16 table from game memory. */
static void read_qtable(uint32_t phys, uint16_t q[64])
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)(phys & 0x1FFFFFFFu);
    int i;
    for (i = 0; i < 64; i++) q[i] = (uint16_t)(((unsigned)p[2 * i] << 8) | p[2 * i + 1]);
}

unsigned int host_jpeg_task(unsigned int data_addr)
{
    static int warned;
    const uint8_t *d = (const uint8_t *)(uintptr_t)(data_addr & 0x1FFFFFFFu);
    uint32_t addr = rd32(d + 0x00) & 0x1FFFFFFFu, count = rd32(d + 0x04), mode = rd32(d + 0x08);
    uint16_t qy[64], qu[64], qv[64];
    uint32_t i;

    if (mode != 2 || count != 1) {
        if (!warned) {
            warned = 1;
            fprintf(stderr, "kyujo: JPEG task with mode %u, count %u is not implemented (only mode 2, count 1 is known to be used)\n",
                    (unsigned)mode, (unsigned)count);
        }
        return 0;
    }
    read_qtable(rd32(d + 0x0C), qy);
    read_qtable(rd32(d + 0x10), qu);
    read_qtable(rd32(d + 0x14), qv);
    for (i = 0; i < count; i++) {
        /* In place: decode into a temporary because the output (0x200 bytes) overlaps the input (0x300 bytes). */
        uint8_t px[0x200];
        uint8_t *mcu = (uint8_t *)(uintptr_t)(addr + i * 0x300u);
        jpeg_mcu_to_rgba5551(mcu, qy, qu, qv, px);
        memcpy((uint8_t *)(uintptr_t)(addr + i * 0x200u), px, sizeof px);
    }
    return 0;
}
