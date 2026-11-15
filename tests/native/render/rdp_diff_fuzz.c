/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * rdp_diff_fuzz.c - differential test of the software RDP (src/render/rdp.c): "the same picture before and after a change".
 *
 * WHY: the battle screen is too slow on a PC (about 35-40 fields per second, see docs/known-decomp-issues.md and the investigation log), and
 * most of the time goes into rdp.c. Speeding it up must not change a single pixel, and a picture compared by eye is no proof. This program
 * drives rdp.c with many random but valid RDP command sequences (textures of every format, wrap / mirror / clamp / mask / shift, point and
 * bilinear filter, 1 and 2 cycle modes, random combiner words, render modes with z buffer / blending / alpha compare, triangles and texture
 * rectangles, 16 and 32 bit colour images) and prints, after every case, a checksum of the colour image and the z buffer.
 *
 * HOW IT IS USED: build it twice, once against the rdp.c that is trusted (a copy of the file before the change) and once against the changed
 * rdp.c; run both with the same seed; the two outputs must be IDENTICAL, line by line. tests/native/render/run_rdp_diff.py does all of this.
 * The sequence of commands depends only on the seed (a fixed xorshift generator), never on rdp.c, so both builds receive the same input.
 * The program also reports, on stderr, how many pixels were written overall, so a run that draws nothing cannot pass unnoticed.
 *
 * WHAT THIS PROVES: that two versions of rdp.c agree on the cases generated here. It does not prove either version right (the
 * rdp_gbi_check.c tests and the comparison with the documentation do that), and it covers only what the generator produces (listed above).
 * Not generated: fill rectangles / copy mode, CI textures from a 32-bit image, shifted LOD tiles, the key and convert registers beyond K4/K5.
 *
 * Built for a 32-bit target like the other renderer tests (the renderer treats native pointers as game addresses).
 * usage: rdp_diff_fuzz <seed> <number of cases>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <PR/ultratypes.h>
#include <PR/mbi.h>
#include <PR/gbi.h>

#include "rdp.h"

/* ---- what the renderer expects from the rest of the program (same stand-ins as rdp_gbi_check.c) ---------------------------------- */
int host_game_range_ok(unsigned int addr, unsigned int len) { (void)addr; (void)len; return 1; }
void host_game_range_bounds(unsigned int *lo, unsigned int *hi) { *lo = 0x1000u; *hi = 0xFFFF0000u; }
unsigned long long host_now_ns(void) { return 0; }
void host_fatal(const char *msg) { fprintf(stderr, "host_fatal: %s\n", msg); exit(3); }

/* ---- the generator ---------------------------------------------------------------------------------------------------------------- */
static uint32_t sSeed;
static uint32_t rnd(void) { sSeed ^= sSeed << 13; sSeed ^= sSeed >> 17; sSeed ^= sSeed << 5; return sSeed; }       /* xorshift32 */
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }                            /* inclusive */
static float rf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }
static int pct(int p) { return (int)(rnd() % 100u) < p; }

#define FB_W 96
#define FB_H 64
static uint8_t sCol[FB_W * FB_H * 4] __attribute__((aligned(64)));      /* colour image (up to 32 bit pixels) */
static uint8_t sZ[FB_W * FB_H * 2] __attribute__((aligned(64)));        /* z buffer, 16 bit */
static uint8_t sTex[4096] __attribute__((aligned(64)));                 /* texture data in the game's memory */
static uint8_t sTlut[512] __attribute__((aligned(64)));                 /* palette: 256 entries of 16 bit */
static Gfx sDl[2048];
static int sN;

/* ---- helpers that append commands. The g-form macros of gbi.h write through a Gfx pointer; `P` is a post-incremented cursor. ------------- */
#define P (&sDl[sN++])

static void flush(void)
{
    /* Hands the collected commands to the RDP the way the display-list interpreter does (f3dex2.c): Set_Other_Modes through its own entry,
     * a texture rectangle with its two half words, everything else through rdp_command. */
    int i;
    for (i = 0; i < sN; i++) {
        uint32_t w0 = sDl[i].words.w0, w1 = sDl[i].words.w1;
        unsigned op = w0 >> 24;
        if (op == G_RDPSETOTHERMODE) {
            rdp_set_other_modes(w0 & 0x00FFFFFFu, w1);
        } else if (op == G_TEXRECT || op == G_TEXRECTFLIP) {
            rdp_texture_rectangle(w0, w1, sDl[i + 1].words.w1, sDl[i + 2].words.w1, op == G_TEXRECTFLIP);
            i += 2;
        } else {
            rdp_command(w0, w1);
        }
    }
    sN = 0;
}

static void fill_random(uint8_t *p, unsigned n)
{
    unsigned i;
    int mode = rr(0, 3);
    for (i = 0; i < n; i++) {
        uint32_t r = rnd();
        /* mode 0: pure noise; 1: smooth ramp with a little noise (so bilinear filtering has gradients to filter); 2: mostly 0x00/0xFF
         * (hard edges, alpha 0/255); 3: noise with the low bit set often (RGBA5551 alpha bit set) */
        p[i] = mode == 0 ? (uint8_t)r : mode == 1 ? (uint8_t)(i * 3 + (r & 7)) : mode == 2 ? ((r & 1) ? 0xFF : 0x00) : (uint8_t)(r | ((r >> 9) & 1));
    }
}

/* the render-mode pairs: (cycle 1 form, cycle 2 form) from gbi.h; the second entry of each pair is the "2" variant */
static const uint32_t kRm[][2] = {
    { G_RM_OPA_SURF, G_RM_OPA_SURF2 }, { G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2 }, { G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2 },
    { G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2 }, { G_RM_XLU_SURF, G_RM_XLU_SURF2 }, { G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2 },
    { G_RM_ZB_XLU_SURF, G_RM_ZB_XLU_SURF2 }, { G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2 }, { G_RM_ZB_OPA_DECAL, G_RM_ZB_OPA_DECAL2 },
    { G_RM_AA_ZB_OPA_DECAL, G_RM_AA_ZB_OPA_DECAL2 }, { G_RM_ZB_XLU_DECAL, G_RM_ZB_XLU_DECAL2 }, { G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2 },
    { G_RM_AA_ZB_XLU_INTER, G_RM_AA_ZB_XLU_INTER2 }, { G_RM_ZB_CLD_SURF, G_RM_ZB_CLD_SURF2 }, { G_RM_AA_ZB_OPA_INTER, G_RM_AA_ZB_OPA_INTER2 },
    { G_RM_FOG_SHADE_A, G_RM_AA_ZB_OPA_SURF2 }, { G_RM_FOG_PRIM_A, G_RM_AA_XLU_SURF2 }, { G_RM_PASS, G_RM_AA_ZB_OPA_SURF2 },
    { G_RM_PASS, G_RM_XLU_SURF2 }, { G_RM_CLD_SURF, G_RM_CLD_SURF2 }, { G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_XLU_SURF2 },
};

static uint32_t sOmh, sOml;      /* the other-modes words of the case being built */

static void emit_modes(int cyc2, int bilerp)
{
    int k = rr(0, (int)(sizeof kRm / sizeof kRm[0]) - 1);
    static const uint32_t ac[3] = { G_AC_NONE, G_AC_THRESHOLD, G_AC_DITHER };
    sOmh = (cyc2 ? G_CYC_2CYCLE : G_CYC_1CYCLE) | (bilerp ? G_TF_BILERP : G_TF_POINT) | (pct(70) ? G_TP_PERSP : G_TP_NONE) |
           (pct(15) ? G_PM_1PRIMITIVE : G_PM_NPRIMITIVE);
    sOml = kRm[k][0] | kRm[k][1] | ac[rr(0, 2)] | (pct(10) ? G_ZS_PRIM : G_ZS_PIXEL);
}

static void emit_combine(void)
{
    /* random but valid multiplexer words: field ranges as in gbi.h (G_CCMUX_* / G_ACMUX_*). */
    int a0 = rr(0, 7), b0 = rr(0, 7), c0 = rr(0, 15), d0 = rr(0, 7), Aa0 = rr(0, 7), Ab0 = rr(0, 7), Ac0 = rr(0, 7), Ad0 = rr(0, 7);
    int a1 = rr(0, 7), b1 = rr(0, 7), c1 = rr(0, 15), d1 = rr(0, 7), Aa1 = rr(0, 7), Ab1 = rr(0, 7), Ac1 = rr(0, 7), Ad1 = rr(0, 7);
    Gfx *g = P;
    if (pct(60)) {         /* the common modes of the game keep their textbook shape */
        static const int common[][8] = {
            { G_CCMUX_TEXEL0, G_CCMUX_0, G_CCMUX_SHADE, G_CCMUX_0, G_ACMUX_TEXEL0, G_ACMUX_0, G_ACMUX_SHADE, G_ACMUX_0 },
            { G_CCMUX_TEXEL0, G_CCMUX_0, G_CCMUX_PRIMITIVE, G_CCMUX_0, G_ACMUX_TEXEL0, G_ACMUX_0, G_ACMUX_PRIMITIVE, G_ACMUX_0 },
            { G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_TEXEL0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_TEXEL0 },
            { G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_SHADE, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_SHADE },
            { G_CCMUX_TEXEL0, G_CCMUX_ENVIRONMENT, G_CCMUX_TEXEL0_ALPHA, G_CCMUX_ENVIRONMENT, G_ACMUX_TEXEL0, G_ACMUX_0, G_ACMUX_ENVIRONMENT, G_ACMUX_0 },
        };
        const int *c = common[rr(0, 4)];
        a0 = c[0]; b0 = c[1]; c0 = c[2]; d0 = c[3]; Aa0 = c[4]; Ab0 = c[5]; Ac0 = c[6]; Ad0 = c[7];
    }
    g->words.w0 = _SHIFTL(G_SETCOMBINE, 24, 8) | _SHIFTL(GCCc0w0(a0, c0, Aa0, Ac0) | GCCc1w0(a1, c1), 0, 24);
    g->words.w1 = (unsigned int)(GCCc0w1(b0, d0, Ab0, Ad0) | GCCc1w1(b1, Aa1, Ac1, d1, Ab1, Ad1));
}

/* Loads a texture of the given kind into TMEM and sets up tile 0 (the render tile) to show it. Returns 1 when the kind needs a palette.
 * The gbi.h macros use their `pkt` argument several times, so the argument must be a cursor that advances each time: P. */
static int emit_texture(int kind, int w, int h, int cms, int cmt, int masks, int maskt, int shifts, int shiftt)
{
    switch (kind) {
    case 0: gDPLoadTextureBlock(P, sTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, w, h, 0, cms, cmt, masks, maskt, shifts, shiftt); break;
    case 1: gDPLoadTextureBlock(P, sTex, G_IM_FMT_RGBA, G_IM_SIZ_32b, w, h, 0, cms, cmt, masks, maskt, shifts, shiftt); break;
    case 2: gDPLoadTextureBlock(P, sTex, G_IM_FMT_IA, G_IM_SIZ_8b, w, h, 0, cms, cmt, masks, maskt, shifts, shiftt); break;
    case 3: gDPLoadTextureBlock(P, sTex, G_IM_FMT_IA, G_IM_SIZ_16b, w, h, 0, cms, cmt, masks, maskt, shifts, shiftt); break;
    case 4: gDPLoadTextureBlock_4b(P, sTex, G_IM_FMT_IA, w, h, 0, cms, cmt, masks, maskt, shifts, shiftt); break;
    case 5: gDPLoadTextureBlock_4b(P, sTex, G_IM_FMT_I, w, h, 0, cms, cmt, masks, maskt, shifts, shiftt); break;
    case 6: gDPLoadTextureBlock(P, sTex, G_IM_FMT_I, G_IM_SIZ_8b, w, h, 0, cms, cmt, masks, maskt, shifts, shiftt); break;
    case 7:
        gDPLoadTLUT_pal256(P, sTlut);
        gDPLoadTextureBlock(P, sTex, G_IM_FMT_CI, G_IM_SIZ_8b, w, h, 0, cms, cmt, masks, maskt, shifts, shiftt);
        return 1;
    default:
        gDPLoadTLUT_pal16(P, 0, sTlut);
        gDPLoadTextureBlock_4b(P, sTex, G_IM_FMT_CI, w, h, 0, cms, cmt, masks, maskt, shifts, shiftt);
        return 1;
    }
    return 0;
}

static void emit_colors(void)
{
    gDPSetPrimColor(P, rr(0, 255), rr(0, 255), rr(0, 255), rr(0, 255), rr(0, 255), pct(50) ? 255 : rr(0, 255));
    gDPSetEnvColor(P, rr(0, 255), rr(0, 255), rr(0, 255), pct(50) ? 255 : rr(0, 255));
    gDPSetBlendColor(P, rr(0, 255), rr(0, 255), rr(0, 255), rr(0, 255));
    gDPSetFogColor(P, rr(0, 255), rr(0, 255), rr(0, 255), rr(0, 255));
    gDPSetConvert(P, rr(-256, 255), rr(-256, 255), rr(-256, 255), rr(-256, 255), rr(-256, 255), rr(-256, 255));
    gDPSetPrimDepth(P, rr(0, 0xFFFF), rr(0, 8));
}

/* One random primitive: a triangle (RdpVert coordinates, as the RSP side gives them) or a texture rectangle. */
static void draw_primitive(int use_tex)
{
    if (pct(55)) {
        RdpVert v[3];
        int i, tex = use_tex && pct(90);
        float cx = rf(-10.0f, FB_W + 10.0f), cy = rf(-10.0f, FB_H + 10.0f), sz = rf(2.0f, 70.0f), ts = pct(50) ? 1.0f : rf(0.25f, 3.0f);
        float ws = pct(70) ? 1.0f : rf(0.5f, 2.0f);
        /* A quarter of the triangles have their corners exactly on pixel centres (x.5, y.5): horizontal, vertical and 45 degree edges then pass
         * THROUGH pixel centres, which exercises the rule for pixels that lie exactly on an edge (random corners almost never do that). */
        int snap = pct(25);
        for (i = 0; i < 3; i++) {
            v[i].x = cx + rf(-sz, sz);
            v[i].y = cy + rf(-sz, sz);
            if (snap) { v[i].x = (float)(int)v[i].x + 0.5f; v[i].y = (float)(int)v[i].y + 0.5f; }
            v[i].z = rf(0.0f, 1023.0f);
            v[i].r = rf(0.0f, 255.0f); v[i].g = rf(0.0f, 255.0f); v[i].b = rf(0.0f, 255.0f); v[i].a = pct(60) ? 255.0f : rf(0.0f, 255.0f);
            v[i].s = rf(-8.0f, 40.0f) * ts; v[i].t = rf(-8.0f, 40.0f) * ts;
            v[i].invw = pct(70) ? 1.0f : rf(0.25f, 2.0f) * ws;
        }
        rdp_triangle(&v[0], &v[1], &v[2], 0, 0, pct(80), tex, pct(70));
    } else {
        int x0 = rr(-4, FB_W - 2) * 4, y0 = rr(-4, FB_H - 2) * 4, x1 = x0 + rr(4, 60 * 4), y1 = y0 + rr(4, 40 * 4);
        int s0 = pct(40) ? 0 : rr(-64 * 32, 64 * 32), t0 = pct(40) ? 0 : rr(-64 * 32, 64 * 32);
        int dsdx = pct(40) ? 1 << 10 : rr(-3 << 10, 3 << 10), dtdy = pct(40) ? 1 << 10 : rr(-3 << 10, 3 << 10);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        gSPTextureRectangle(P, x0, y0, x1, y1, 0, s0, t0, dsdx, dtdy);     /* (upper left, lower right) in 10.2 fixed point */
        (void)use_tex;
    }
}

/* FNV-1a checksum of a byte range. */
static uint32_t fnv(const uint8_t *p, unsigned n, uint32_t h)
{
    unsigned i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

int main(int argc, char **argv)
{
    unsigned seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 1u;
    int cases = argc > 2 ? atoi(argv[2]) : 1000, c, drawn_cases = 0;
    uint64_t pixels_total = 0;
    sSeed = seed ? seed : 1u;
    rdp_reset();
    for (c = 0; c < cases; c++) {
        int siz32 = pct(25), segs = rr(1, 4), s;
        unsigned cbytes = FB_W * FB_H * (siz32 ? 4u : 2u);
        uint32_t h;
        uint32_t t0, t1;
        uint64_t p0, p1;
        if (pct(50)) rdp_reset();                    /* half of the cases start from clean state, half from what the last case left */
        rdp_stats(&t0, &t1, &p0);                    /* after the reset: the counters restart at 0 */
        fill_random(sCol, sizeof sCol);
        fill_random(sZ, sizeof sZ);
        if (pct(40)) memset(sZ, 0xFF, sizeof sZ);    /* a far-away depth buffer: most z tests pass */
        fill_random(sTex, sizeof sTex);
        fill_random(sTlut, sizeof sTlut);
        sN = 0;
        gDPSetColorImage(P, G_IM_FMT_RGBA, siz32 ? G_IM_SIZ_32b : G_IM_SIZ_16b, FB_W, sCol);
        gDPSetDepthImage(P, sZ);
        if (pct(70)) { gDPSetScissor(P, G_SC_NON_INTERLACE, 0, 0, FB_W, FB_H); }       /* braces: gbi.h's gDP macros are blocks, not statements */
        else { gDPSetScissor(P, G_SC_NON_INTERLACE, rr(0, 30), rr(0, 20), rr(40, FB_W), rr(30, FB_H)); }
        for (s = 0; s < segs; s++) {
            /* A segment: new modes / colours / texture (not always), then a few primitives. Changing only part of the state between
             * segments exercises the renderer's caches (a texture that changed, a tile description that changed, a palette that changed). */
            static const int kSz[] = { 4, 8, 16, 32 };
            int cyc2 = pct(40), bilerp = pct(60), kind = rr(0, 8), np = rr(1, 5), k, tlut = 0;
            if (s == 0 || pct(50)) { emit_modes(cyc2, bilerp); emit_combine(); emit_colors(); }
            if (s == 0 || pct(60)) {
                int w = kSz[rr(0, 3)], h = kSz[rr(0, 3)];
                int cms = pct(50) ? G_TX_WRAP : (pct(50) ? G_TX_CLAMP : (pct(50) ? G_TX_MIRROR : (G_TX_MIRROR | G_TX_CLAMP)));
                int cmt = pct(50) ? G_TX_WRAP : (pct(50) ? G_TX_CLAMP : (pct(50) ? G_TX_MIRROR : (G_TX_MIRROR | G_TX_CLAMP)));
                int masks = pct(50) ? 0 : rr(0, 6), maskt = pct(50) ? 0 : rr(0, 6), shifts = pct(80) ? 0 : rr(0, 15), shiftt = pct(80) ? 0 : rr(0, 15);
                if (kind == 1 && w * h > 512) h = 8;                 /* 32-bit texels use both halves of TMEM */
                if (kind == 7 || kind == 8) tlut = pct(50) ? G_TT_RGBA16 : G_TT_IA16;
                emit_texture(kind, w, h, cms, cmt, masks, maskt, shifts, shiftt);
                if (kind >= 7) sOmh = (sOmh & ~(3u << G_MDSFT_TEXTLUT)) | (uint32_t)tlut;
                else sOmh &= ~(3u << G_MDSFT_TEXTLUT);
                gDPSetOtherMode(P, sOmh, sOml);
            } else {
                gDPSetOtherMode(P, sOmh, sOml);
            }
            for (k = 0; k < np; k++) draw_primitive(1);
        }
        flush();
        rdp_stats(&t0, &t1, &p1);
        pixels_total += p1 - p0;
        if (p1 != p0) drawn_cases++;
        h = fnv(sCol, cbytes, 2166136261u);
        h = fnv(sZ, sizeof sZ, h);
        printf("%d %08x\n", c, (unsigned)h);
    }
    fprintf(stderr, "rdp_diff_fuzz: seed %u, %d cases, %d drew pixels, %llu pixels written in total\n", seed, cases, drawn_cases,
            (unsigned long long)pixels_total);
    return 0;
}
