/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * scale_check.c - checks of src/render/scale.c (the "sharp bilinear" scaler of the window picture).
 *
 * Each check computes the expected result here, independently of scale.c, from the documented behaviour in scale.c's header:
 *   1. a whole-number enlargement is exact pixel replication (and channel order R,G,B -> B,G,R,0),
 *   2. a flat colour stays flat at any size, and the 4:3 letterbox bars are exactly black,
 *   3. a one-pixel white line enlarged by a non-integer factor stays SHARP: the number of in-between grey values across an edge is at most
 *      2 columns (plain bilinear would give about the enlargement factor in-between columns),
 *   4. a reduction does not crash and keeps a flat picture flat (within 2 of 255),
 *   5. the placement rectangle is centred and 4:3.
 * Exit status 0 = all passed. Plain C, runs as a 64-bit or 32-bit program.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "scale.h"

static int sFail;
#define CHECK(cond, ...) do { if (cond) printf("ok   " __VA_ARGS__), printf("\n"); else { sFail++; printf("FAIL " __VA_ARGS__), printf("\n"); } } while (0)

int main(void)
{
    unsigned x, y;
    uint8_t *work = malloc(2 * 4096 * 4);

    /* 1. exact 2x replication of a 4 x 3 picture (4:3, so no bars) into 8 x 6 */
    {
        uint8_t src[4 * 3 * 3], dst[8 * 6 * 4];
        int bad = 0;
        for (y = 0; y < 3; y++) for (x = 0; x < 4; x++) { uint8_t *p = src + (y * 4 + x) * 3; p[0] = (uint8_t)(10 + x * 40); p[1] = (uint8_t)(20 + y * 70); p[2] = (uint8_t)(x * 13 + y * 7); }
        CHECK(scale_picture(src, 4, 3, dst, 8, 6, work) == 0, "2x scale returns success");
        for (y = 0; y < 6; y++) for (x = 0; x < 8; x++) {
            const uint8_t *s = src + ((y / 2) * 4 + (x / 2)) * 3, *d = dst + (y * 8 + x) * 4;
            if (d[0] != s[2] || d[1] != s[1] || d[2] != s[0] || d[3] != 0) bad++;
        }
        CHECK(bad == 0, "whole-number enlargement is exact replication, channels B,G,R,0 (%d wrong pixels)", bad);
    }

    /* 2. flat colour + bars: 320 x 240 flat (200,100,50) into 1000 x 700 (4:3 box is 933 x 700, centred) */
    {
        enum { SW = 320, SH = 240, CW = 1000, CH = 700 };
        uint8_t *src = malloc(SW * SH * 3), *dst = malloc((size_t)CW * CH * 4);
        unsigned dx, dy, dw, dh, bad_in = 0, bad_out = 0;
        for (x = 0; x < SW * SH; x++) { src[x * 3] = 200; src[x * 3 + 1] = 100; src[x * 3 + 2] = 50; }
        CHECK(scale_picture(src, SW, SH, dst, CW, CH, work) == 0, "flat picture, non-integer factor, returns success");
        scale_fit_rect(CW, CH, &dx, &dy, &dw, &dh);
        CHECK(dw == 933 && dh == 700 && dx == 33 && dy == 0, "fit rectangle %ux%u at %u,%u is 4:3 and centred (want 933x700 at 33,0)", dw, dh, dx, dy);
        for (y = 0; y < CH; y++) for (x = 0; x < CW; x++) {
            const uint8_t *d = dst + ((size_t)y * CW + x) * 4;
            int inside = x >= dx && x < dx + dw && y >= dy && y < dy + dh;
            if (inside) { if (d[0] != 50 || d[1] != 100 || d[2] != 200) bad_in++; }
            else if (d[0] || d[1] || d[2]) bad_out++;
        }
        CHECK(bad_in == 0, "flat colour stays flat inside the picture (%u wrong)", bad_in);
        CHECK(bad_out == 0, "letterbox bars are exactly black (%u wrong)", bad_out);
        free(src); free(dst);
    }

    /* 3. sharpness: 320 x 240 picture, black with ONE white column at x = 100, scaled to 1920 x 1440 (factor 6.0: whole) and 1900 x 1425 (5.9375) */
    {
        static const unsigned sizes[2][2] = { { 1920, 1440 }, { 1900, 1425 } };
        int k;
        for (k = 0; k < 2; k++) {
            unsigned cw = sizes[k][0], ch = sizes[k][1], mid = 0, full = 0;
            uint8_t *src = calloc(320 * 240, 3), *dst = malloc((size_t)cw * ch * 4);
            for (y = 0; y < 240; y++) { uint8_t *p = src + (y * 320 + 100) * 3; p[0] = p[1] = p[2] = 255; }
            if (cw * 2 > 4096) { free(work); work = malloc((size_t)cw * 2 * 4); }
            scale_picture(src, 320, 240, dst, cw, ch, work);
            for (x = 0; x < cw; x++) { unsigned v = dst[((size_t)(ch / 2) * cw + x) * 4]; if (v == 255) full++; else if (v > 0) mid++; }
            /* the line is 5.9..6 output pixels wide; at most 2 columns (one per edge) may be in-between, none for the whole-number factor */
            CHECK(full >= 5 && full <= 7 && mid <= (k == 0 ? 0u : 2u), "one-pixel line at factor %.4f: %u full columns, %u in-between (want 5..7 full, <= %u in-between)",
                  (double)cw / 320.0, full, mid, k == 0 ? 0u : 2u);
            free(src); free(dst);
        }
    }

    /* 4. reduction: 640 x 480 flat 255 into 400 x 300 and into 200 x 300 (letterboxed): average inside the box stays 255 */
    {
        uint8_t *src = malloc(640 * 480 * 3), *dst = malloc(400 * 300 * 4);
        unsigned dx, dy, dw, dh, lo = 0;
        memset(src, 255, 640 * 480 * 3);
        CHECK(scale_picture(src, 640, 480, dst, 400, 300, work) == 0, "reduction returns success");
        scale_fit_rect(400, 300, &dx, &dy, &dw, &dh);
        for (y = dy; y < dy + dh; y++) for (x = dx; x < dx + dw; x++) { const uint8_t *d = dst + ((size_t)y * 400 + x) * 4; if (d[0] < 253 || d[1] < 253 || d[2] < 253) lo++; }
        CHECK(lo == 0, "reduction of a flat picture stays flat (%u pixels below 253)", lo);
        CHECK(scale_picture(src, 640, 480, dst, 200, 300, work) == 0, "tall narrow window returns success");
        CHECK(scale_picture(src, 640, 480, dst, 0, 300, work) == -1, "zero-width window is refused");
        free(src); free(dst);
    }
    /* 6. widescreen (stretch = 1): the picture fills a 16:9 window, no black pixel anywhere, and a flat picture stays flat */
    {
        enum { CW = 160, CH = 90 };
        uint8_t src[320 * 240 * 3], *dst = malloc((size_t)CW * CH * 4);
        unsigned dx, dy, dw, dh, dark = 0;
        memset(src, 255, sizeof src);
        CHECK(scale_picture_mode(src, 320, 240, dst, CW, CH, work, 1) == 0, "stretch returns success");
        scale_fit_rect_mode(CW, CH, &dx, &dy, &dw, &dh, 1);
        CHECK(dx == 0 && dy == 0 && dw == CW && dh == CH, "stretch rectangle is the whole window (%u,%u %ux%u)", dx, dy, dw, dh);
        for (y = 0; y < CH; y++) for (x = 0; x < CW; x++) { const uint8_t *d = dst + ((size_t)y * CW + x) * 4; if (d[0] < 253 || d[1] < 253 || d[2] < 253) dark++; }
        CHECK(dark == 0, "stretched flat picture covers every pixel (%u dark)", dark);
        scale_fit_rect_mode(CW, CH, &dx, &dy, &dw, &dh, 0);
        CHECK(dx > 0 && dy == 0 && dh == CH && dw == CH * 4 / 3, "without stretch the 4:3 rectangle keeps side bars (%u,%u %ux%u)", dx, dy, dw, dh);
        free(dst);
    }
    /* 7. widescreen crops a black frame that is on all four sides (Game Boy Tower), but nothing else */
    {
        enum { SW = 100, SH = 75, CW = 160, CH = 90 };
        uint8_t src[SW * SH * 3], *dst = malloc((size_t)CW * CH * 4);
        unsigned dark;
        /* white page with a 5/4 pixel black frame: the stretched picture has no dark pixel at all */
        memset(src, 0, sizeof src);
        for (y = 4; y < SH - 4; y++) for (x = 5; x < SW - 5; x++) { uint8_t *p = src + ((size_t)y * SW + x) * 3; p[0] = p[1] = p[2] = 255; }
        CHECK(scale_picture_mode(src, SW, SH, dst, CW, CH, work, 1) == 0, "stretch of a framed picture returns success");
        dark = 0;
        for (y = 0; y < CH; y++) for (x = 0; x < CW; x++) { const uint8_t *d = dst + ((size_t)y * CW + x) * 4; if (d[0] < 200) dark++; }
        CHECK(dark == 0, "the black frame is cropped away (%u dark pixels)", dark);
        /* the same picture without widescreen keeps its frame (bars are part of the 4:3 picture) */
        CHECK(scale_picture_mode(src, SW, SH, dst, 100, 75, work, 0) == 0, "no stretch keeps the picture");
        CHECK(dst[(2 * 100 + 50) * 4] == 0 && dst[(37 * 100 + 50) * 4] == 255, "no stretch: frame still black, page white");
        /* black bars only above and below (a battle introduction): left alone, so the bars stay */
        memset(src, 255, sizeof src);
        for (y = 0; y < 10; y++) for (x = 0; x < SW; x++) { uint8_t *p = src + ((size_t)y * SW + x) * 3; p[0] = p[1] = p[2] = 0; p = src + ((size_t)(SH - 1 - y) * SW + x) * 3; p[0] = p[1] = p[2] = 0; }
        CHECK(scale_picture_mode(src, SW, SH, dst, CW, CH, work, 1) == 0, "stretch of a letterboxed picture returns success");
        CHECK(dst[(1 * CW + 80) * 4] == 0 && dst[(45 * CW + 80) * 4] == 255, "letterbox bars are kept (top row black, middle white)");
        /* an all black picture does not crash and stays black */
        memset(src, 0, sizeof src);
        CHECK(scale_picture_mode(src, SW, SH, dst, CW, CH, work, 1) == 0 && dst[(45 * CW + 80) * 4] == 0, "a black picture stays black");
        free(dst);
    }
    free(work);
    printf(sFail ? "%d check(s) FAILED\n" : "all checks passed\n", sFail);
    return sFail != 0;
}
