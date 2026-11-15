/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * scale.c - "sharp bilinear" scaling of the console picture to the window size.
 *
 * WHY NOT LET WINDOWS DO IT: the first Windows version stretched the picture with StretchDIBits in HALFTONE mode. The tester reported blurry,
 * horizontally "misaligned" battle text. The battle scenes are drawn by the game at 320 x ~237 pixels (the menus at 640 x ~474), so the
 * picture is enlarged by a factor that is usually NOT a whole number; plain interpolation then blurs every edge over about 2 output pixels
 * and gives neighbouring texels unequal widths. (That this is the cause is the explanation that fits; it has not been seen on Windows.)
 *
 * WHAT IS DONE INSTEAD: for an enlargement by factor s (> 1) every output pixel centre is mapped back into the picture (u = (x + 0.5) / s - 0.5),
 * i0 = floor(u) is the picture pixel to its left and f = u - i0 the fractional distance. Plain bilinear would blend with weight f. Here the weight is
 *     f' = clamp(0.5 + (f - 0.5) * s, 0, 1)
 * which is 0 or 1 everywhere except in a band one OUTPUT pixel wide around the boundary between two picture pixels: texels stay crisp, edges are only
 * anti-aliased by one output pixel (the "sharp bilinear" method known from emulator front ends). For a whole-number s it is exactly pixel
 * replication. For a reduction (s < 1) plain bilinear (f' = f) is used. The same is applied vertically.
 * 8-bit fixed-point weights (0..256). The work is split into one horizontal pass per needed picture row (kept in `work`) and a vertical blend; rows
 * whose vertical weight is 0 or 256 are copied, which is most rows when enlarging, so the cost is dominated by the horizontal pass.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "scale.h"

#define MAX_DIM 8192u

void scale_fit_rect_mode(unsigned cw, unsigned ch, unsigned *dx, unsigned *dy, unsigned *dw, unsigned *dh, int stretch)
{
    unsigned w = cw, h = cw * 3u / 4u;
    if (stretch) { *dw = cw; *dh = ch; *dx = 0; *dy = 0; return; }           /* widescreen: the picture fills the window */
    if (h > ch) { h = ch; w = ch * 4u / 3u; }
    *dw = w; *dh = h;
    *dx = (cw - w) / 2u;
    *dy = (ch - h) / 2u;
}

void scale_fit_rect(unsigned cw, unsigned ch, unsigned *dx, unsigned *dy, unsigned *dw, unsigned *dh)
{
    scale_fit_rect_mode(cw, ch, dx, dy, dw, dh, 0);
}

/* A picture that has a black frame on ALL four sides (Game Boy Tower: the game draws its page 14 pixels in from the left and right and 7 from the top and
 * bottom of the 283 x 237 picture, the rest is black) is cropped to what is inside the frame, so that the widescreen mode fills the window with the page and not
 * with the page plus a black border. Rules, chosen so that nothing else is touched: every one of the four margins must be at least 2 and at most 12 % of the
 * picture's size (a picture with black bars only above and below, like the battle introductions, or no margin at all, or a black screen is left alone), and
 * "black" is red, green and blue all at most 6. Only the picture's edges are examined, so the cost is a few hundred pixels. */
#define CROP_BLACK 6u
#define CROP_MIN_MARGIN 2u
static int px_black(const uint8_t *p) { return p[0] <= CROP_BLACK && p[1] <= CROP_BLACK && p[2] <= CROP_BLACK; }

static void crop_black_frame(const uint8_t *rgb, unsigned sw, unsigned sh, unsigned *x0, unsigned *y0, unsigned *w, unsigned *h)
{
    unsigned maxx = sw * 12u / 100u, maxy = sh * 12u / 100u, l = 0, r = 0, t = 0, b = 0, x, y;
    *x0 = 0; *y0 = 0; *w = sw; *h = sh;
    if (sw < 16 || sh < 16) return;
    for (t = 0; t <= maxy + 1; t++) {                                  /* rows from the top that are entirely black */
        const uint8_t *row = rgb + (size_t)t * sw * 3u;
        for (x = 0; x < sw && px_black(row + x * 3u); x++) {}
        if (x < sw) break;
    }
    for (b = 0; b <= maxy + 1; b++) {
        const uint8_t *row = rgb + (size_t)(sh - 1u - b) * sw * 3u;
        for (x = 0; x < sw && px_black(row + x * 3u); x++) {}
        if (x < sw) break;
    }
    for (l = 0; l <= maxx + 1; l++) {                                  /* columns from the left / right that are entirely black */
        for (y = 0; y < sh && px_black(rgb + ((size_t)y * sw + l) * 3u); y++) {}
        if (y < sh) break;
    }
    for (r = 0; r <= maxx + 1; r++) {
        for (y = 0; y < sh && px_black(rgb + ((size_t)y * sw + (sw - 1u - r)) * 3u); y++) {}
        if (y < sh) break;
    }
    if (l < CROP_MIN_MARGIN || r < CROP_MIN_MARGIN || t < CROP_MIN_MARGIN || b < CROP_MIN_MARGIN) return;
    if (l > maxx || r > maxx || t > maxy || b > maxy) return;
    *x0 = l; *y0 = t; *w = sw - l - r; *h = sh - t - b;
}

/* Source position (index of the left/upper picture pixel and the weight 0..256 of the next one) for output pixel `o` of `n_out`, picture size `n_in`. */
static void map_pixel(unsigned o, unsigned n_out, unsigned n_in, unsigned *i0, unsigned *i1, unsigned *w)
{
    double s = (double)n_out / (double)n_in;
    double u = ((double)o + 0.5) / s - 0.5;
    double fl = (double)(long)u;
    double f;
    long a;
    if (u < 0) { fl = -1.0; }                      /* (long) truncates toward zero; floor() is wanted */
    f = u - fl;
    a = (long)fl;
    if (s > 1.0) {
        f = 0.5 + (f - 0.5) * s;
        if (f < 0.0) f = 0.0;
        if (f > 1.0) f = 1.0;
    }
    {
        long b = a + 1;
        if (a < 0) a = 0;
        if (a > (long)n_in - 1) a = (long)n_in - 1;
        if (b < 0) b = 0;
        if (b > (long)n_in - 1) b = (long)n_in - 1;
        *i0 = (unsigned)a;
        *i1 = (unsigned)b;
        *w = (unsigned)(f * 256.0 + 0.5);
    }
}

/* One horizontally scaled output row (dw pixels, B,G,R,0) from picture row `src` (sw pixels, R,G,B). x0/x1/wx are the per-column tables. */
static void hrow(const uint8_t *src, const unsigned *x0, const unsigned *x1, const unsigned *wx, unsigned dw, uint8_t *out)
{
    unsigned x;
    for (x = 0; x < dw; x++) {
        const uint8_t *a = src + x0[x] * 3u, *b = src + x1[x] * 3u;
        unsigned w = wx[x];
        if (w == 0) {
            out[0] = a[2]; out[1] = a[1]; out[2] = a[0];
        } else if (w >= 256) {
            out[0] = b[2]; out[1] = b[1]; out[2] = b[0];
        } else {
            out[0] = (uint8_t)((a[2] * (256u - w) + b[2] * w + 128u) >> 8);
            out[1] = (uint8_t)((a[1] * (256u - w) + b[1] * w + 128u) >> 8);
            out[2] = (uint8_t)((a[0] * (256u - w) + b[0] * w + 128u) >> 8);
        }
        out[3] = 0;
        out += 4;
    }
}

int scale_picture(const uint8_t *rgb, unsigned sw, unsigned sh, uint8_t *dst, unsigned cw, unsigned ch, uint8_t *work)
{
    return scale_picture_mode(rgb, sw, sh, dst, cw, ch, work, 0);
}

int scale_picture_mode(const uint8_t *rgb, unsigned sw, unsigned sh, uint8_t *dst, unsigned cw, unsigned ch, uint8_t *work, int stretch)
{
    unsigned dx, dy, dw, dh, x, y;
    unsigned sx0 = 0, sy0 = 0, sw_in = sw, sh_in = sh;      /* the part of the picture that is scaled (all of it unless a black frame is cropped) */
    unsigned *tab;
    uint8_t *rowA = work, *rowB = work + (size_t)cw * 4u;
    long cacheA = -1, cacheB = -1;                  /* picture row held in rowA / rowB, -1 = none */
    if (!sw || !sh || !cw || !ch || sw > MAX_DIM || sh > MAX_DIM || cw > MAX_DIM || ch > MAX_DIM) return -1;
    scale_fit_rect_mode(cw, ch, &dx, &dy, &dw, &dh, stretch);
    if (stretch) crop_black_frame(rgb, sw, sh, &sx0, &sy0, &sw_in, &sh_in);   /* widescreen: a black frame round the picture would show as a border */
    memset(dst, 0, (size_t)cw * ch * 4u);           /* black bars */
    if (!dw || !dh) return 0;
    tab = (unsigned *)malloc((size_t)dw * 3u * sizeof(unsigned));
    if (!tab) return -1;
    for (x = 0; x < dw; x++) { map_pixel(x, dw, sw_in, &tab[x], &tab[dw + x], &tab[2u * dw + x]); tab[x] += sx0; tab[dw + x] += sx0; }
    for (y = 0; y < dh; y++) {
        unsigned y0, y1, wy;
        uint8_t *out = dst + ((size_t)(dy + y) * cw + dx) * 4u;
        const uint8_t *ra, *rb;
        map_pixel(y, dh, sh_in, &y0, &y1, &wy);
        y0 += sy0; y1 += sy0;
        /* make sure the horizontally scaled rows for y0 (and y1 if blended) are in the cache */
        if (cacheA != (long)y0 && cacheB == (long)y0) {            /* swap so that A is y0 */
            uint8_t *t = rowA; rowA = rowB; rowB = t;
            { long c = cacheA; cacheA = cacheB; cacheB = c; }
        }
        if (cacheA != (long)y0) { hrow(rgb + (size_t)y0 * sw * 3u, tab, tab + dw, tab + 2u * dw, dw, rowA); cacheA = (long)y0; }
        ra = rowA;
        if (wy == 0 || y1 == y0) {
            memcpy(out, ra, (size_t)dw * 4u);
            continue;
        }
        if (cacheB != (long)y1) { hrow(rgb + (size_t)y1 * sw * 3u, tab, tab + dw, tab + 2u * dw, dw, rowB); cacheB = (long)y1; }
        rb = rowB;
        if (wy >= 256) {
            memcpy(out, rb, (size_t)dw * 4u);
        } else {
            unsigned n = dw * 4u, i;
            for (i = 0; i < n; i++) out[i] = (uint8_t)((ra[i] * (256u - wy) + rb[i] * wy + 128u) >> 8);
        }
    }
    free(tab);
    return 0;
}
