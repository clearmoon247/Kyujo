/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * rdp_gbi_check.c - end-to-end checks of the display-list interpreter (src/render/f3dex2.c) and the software RDP (src/render/rdp.c).
 *
 * HOW: a display list is built with the macros of the decomp's own gbi.h (so every command ENCODING comes from the documentation header,
 * not from this port's reading of it), run through gfx_run_display_list, and ONE pixel of the finished RGBA5551 picture is compared with
 * a value computed here by the documented formulas. It must be built for a 32-bit target (the renderer treats native pointers as game
 * addresses); tests/native/render/run_render_checks.py does that.
 *
 * WHAT IS CHECKED
 *   1. Colour combiner: the A/B/C/D field decoding, through gsDPSetCombineLERP / the G_CC_* modes of gbi.h, with SHADE, PRIMITIVE and
 *      ENVIRONMENT as inputs (the combiner equation is (A - B) * C + D).
 *   2. Vertex lighting (the "dark Pokemon" regression, see docs/known-decomp-issues.md): the lit colour of a vertex must not depend on a scale
 *      in the modelview matrix, because the microcode normalises the light direction (Nintendo programming manual 11.7; F3DEX3 porting
 *      guide: "F3D through F3DEX2 normalize light directions in the microcode"). The game draws the Pokemon with a 0.1 scale.
 *   3. Texture rectangle drawn 1:1 shows the texels exactly, point-sampled and bilinear (regression for the blurred battle text).
 * WHAT IS NOT CHECKED: the exact fixed-point rounding of the real microcode (no reference was available); the tolerance below (8 of 255,
 * more than the 5-bit quantisation of the picture) says how loose the comparison is.
 *
 * Exit status 0 = all checks passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <PR/ultratypes.h>
#include <PR/mbi.h>
#include <PR/gbi.h>

#include "gfx.h"
#include "rdp.h"

/* ---- what the renderer expects from the rest of the program ------------------------------------------------------------------- */
int host_game_range_ok(unsigned int addr, unsigned int len) { (void)addr; (void)len; return 1; }     /* the test's memory is all valid */
void host_game_range_bounds(unsigned int *lo, unsigned int *hi) { *lo = 0x1000u; *hi = 0xFFFF0000u; }     /* every address the test can have */
unsigned long long host_now_ns(void) { return 0; }     /* the RDP speed profile reads the clock; the test does not use it */
void host_fatal(const char *msg) { fprintf(stderr, "host_fatal: %s\n", msg); exit(3); }

/* ---- memory the display list refers to ------------------------------------------------------------------------------------------- */
#define W 64
static uint8_t sColorImage[W * W * 2] __attribute__((aligned(64)));
static Gfx sDl[256] __attribute__((aligned(16)));
static uint8_t sMtxP[64] __attribute__((aligned(16))), sMtxM[64] __attribute__((aligned(16)));
static uint8_t sLight[2][16] __attribute__((aligned(16)));
static uint8_t sVp[16] __attribute__((aligned(16)));
static uint8_t sVtx[3 * 16] __attribute__((aligned(16)));
static int sN;                                      /* number of commands in sDl */

static void be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* Appends the commands that `macro` expands to. The macros here are the `gs` forms: braces holding one command each. */
#define ADD(...) do { static const Gfx _c[] = { __VA_ARGS__ }; unsigned _i; for (_i = 0; _i < sizeof _c / sizeof _c[0]; _i++) sDl[sN++] = _c[_i]; } while (0)

/* The renderer reads the display list as the console does: big-endian words. Our Gfx words are native (little-endian), so swap. */
static void swap_dl(void)
{
    int i;
    for (i = 0; i < sN; i++) {
        uint32_t a = sDl[i].words.w0, b = sDl[i].words.w1;
        sDl[i].words.w0 = __builtin_bswap32(a);
        sDl[i].words.w1 = __builtin_bswap32(b);
    }
}

/* Mtx (gbi.h): 16 integer halves then 16 fraction halves, big-endian, row-major; value = int + frac / 65536. */
static void diag_mtx(uint8_t *m, double s)
{
    int i;
    memset(m, 0, 64);
    for (i = 0; i < 4; i++) {
        double v = (i == 3) ? 1.0 : s;
        int ip = (int)floor(v);
        be16(m + (i * 4 + i) * 2, (unsigned)ip & 0xFFFF);
        be16(m + 32 + (i * 4 + i) * 2, (unsigned)((v - ip) * 65536.0 + 0.5) & 0xFFFF);
    }
}

/* Runs the list; returns the picture's centre pixel as 8-bit r,g,b (each 5-bit value expanded as v * 8 + 4, the middle of its step). */
static void run_and_read(int rgb[3])
{
    unsigned v;
    swap_dl();
    memset(sColorImage, 0, sizeof sColorImage);
    rdp_reset();
    gfx_reset_rsp();
    gfx_run_display_list((uint32_t)(uintptr_t)sDl);
    v = ((unsigned)sColorImage[(W / 2 * W + W / 2) * 2] << 8) | sColorImage[(W / 2 * W + W / 2) * 2 + 1];
    rgb[0] = (int)((v >> 11) & 31) * 8 + 4; rgb[1] = (int)((v >> 6) & 31) * 8 + 4; rgb[2] = (int)((v >> 1) & 31) * 8 + 4;
}

/* One vertex triple (all three the same colour / normal) covering the middle of the picture. `k` = coordinate size: clip = k * modelview
 * scale * projection scale (1/32) must come to 0.5, so the caller passes k = 16 / scale. */
static void make_vertices(int k, const int8_t rgb[3])
{
    static const int sx[3] = {-1, 1, 0}, sy[3] = {-1, -1, 1};
    int i;
    memset(sVtx, 0, sizeof sVtx);
    for (i = 0; i < 3; i++) {
        uint8_t *v = sVtx + i * 16;
        be16(v + 0, (unsigned)(sx[i] * k) & 0xFFFF); be16(v + 2, (unsigned)(sy[i] * k) & 0xFFFF); be16(v + 4, 0);
        v[12] = (uint8_t)rgb[0]; v[13] = (uint8_t)rgb[1]; v[14] = (uint8_t)rgb[2]; v[15] = 255;
    }
}

/* The commands every test starts with: colour image, scissor, 1-cycle mode, no z, no texture; viewport 64 x 64 and a projection that maps
 * clip -1..1 to the whole picture. The combiner and the rest are added by the caller. */
static void common_setup(void)
{
    sN = 0;
    ADD(gsDPSetColorImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, W, sColorImage));
    ADD(gsDPSetScissor(G_SC_NON_INTERLACE, 0, 0, W, W));
    ADD(gsDPSetCycleType(G_CYC_1CYCLE));
    ADD(gsDPSetRenderMode(G_RM_OPA_SURF, G_RM_OPA_SURF2));
    ADD(gsDPSetPrimColor(0, 0, 200, 100, 50, 255));
    ADD(gsDPSetEnvColor(20, 220, 120, 255));
    be16(sVp + 0, 32 * 4); be16(sVp + 2, 32 * 4); be16(sVp + 4, 511); be16(sVp + 6, 0);        /* scale: half the picture, 2 fraction bits */
    be16(sVp + 8, 32 * 4); be16(sVp + 10, 32 * 4); be16(sVp + 12, 511); be16(sVp + 14, 0);     /* translate: the middle */
    ADD(gsSPViewport(sVp));
    diag_mtx(sMtxP, 1.0 / 32.0);
    ADD(gsSPMatrix(sMtxP, G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH));
}

static int sFailures;
static void check(const char *what, int got[3], const int want[3], int tol)
{
    int ok = abs(got[0] - want[0]) <= tol && abs(got[1] - want[1]) <= tol && abs(got[2] - want[2]) <= tol;
    printf("%s %-46s got (%3d,%3d,%3d) want (%3d,%3d,%3d)\n", ok ? "ok  " : "FAIL", what, got[0], got[1], got[2], want[0], want[1], want[2]);
    if (!ok) sFailures++;
}

/* ---- 1. combiner ------------------------------------------------------------------------------------------------------------------ */

static int clampi(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* The rest of a combiner test, after the combine command has been added by COMBINER_CASE. */
static void combiner_finish(const char *name, const int want[3])
{
    static const int8_t shade[3] = {100, -56, 30};          /* vertex colour bytes: 100, 200 (= -56 as a signed byte), 30 */
    int got[3];
    ADD(gsSPClearGeometryMode(0xFFFFFF));
    ADD(gsSPSetGeometryMode(G_SHADE | G_SHADING_SMOOTH));
    diag_mtx(sMtxM, 1.0);
    ADD(gsSPMatrix(sMtxM, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH));
    make_vertices(16, shade);
    ADD(gsSPVertex(sVtx, 3, 0));
    ADD(gsSP1Triangle(0, 1, 2, 0));
    ADD(gsDPFullSync());
    ADD(gsSPEndDisplayList());
    run_and_read(got);
    check(name, got, want, 9);
}
/* A macro (not a function) because the gbi.h macros expand to brace initialisers that only ADD can take. */
#define COMBINER_CASE(NAME, COMB, WANT) do { common_setup(); ADD(COMB); combiner_finish(NAME, WANT); } while (0)

static void test_combiner(void)
{
    const int sh[3] = {100, 200, 30}, pr[3] = {200, 100, 50}, en[3] = {20, 220, 120};
    int want[3], c;
    COMBINER_CASE("G_CC_SHADE", gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE), sh);
    COMBINER_CASE("G_CC_PRIMITIVE", gsDPSetCombineMode(G_CC_PRIMITIVE, G_CC_PRIMITIVE), pr);
    /* (PRIMITIVE - 0) * SHADE + 0 */
    for (c = 0; c < 3; c++) want[c] = clampi(pr[c] * sh[c] / 255);
    COMBINER_CASE("(PRIM - 0) * SHADE + 0", gsDPSetCombineLERP(PRIMITIVE, 0, SHADE, 0, 0, 0, 0, 1, PRIMITIVE, 0, SHADE, 0, 0, 0, 0, 1), want);
    /* (PRIMITIVE - ENVIRONMENT) * SHADE + ENVIRONMENT */
    for (c = 0; c < 3; c++) want[c] = clampi((pr[c] - en[c]) * sh[c] / 255 + en[c]);
    COMBINER_CASE("(PRIM - ENV) * SHADE + ENV", gsDPSetCombineLERP(PRIMITIVE, ENVIRONMENT, SHADE, ENVIRONMENT, 0, 0, 0, 1, PRIMITIVE, ENVIRONMENT, SHADE, ENVIRONMENT, 0, 0, 0, 1), want);
    /* (SHADE - ENVIRONMENT) * PRIMITIVE + 0: B and C fields in the other positions */
    for (c = 0; c < 3; c++) want[c] = clampi((sh[c] - en[c]) * pr[c] / 255);
    COMBINER_CASE("(SHADE - ENV) * PRIM + 0", gsDPSetCombineLERP(SHADE, ENVIRONMENT, PRIMITIVE, 0, 0, 0, 0, 1, SHADE, ENVIRONMENT, PRIMITIVE, 0, 0, 0, 0, 1), want);
}

/* ---- 2. lighting ------------------------------------------------------------------------------------------------------------------ */

/* The documented result: ambient + colour * max(0, normal . unit(light direction) / 127), per channel, clamped. (Rotation is the identity
 * here, the matrix is a pure scale, so the transformed light direction is the light direction times the scale, and normalising removes it.) */
static void lighting_case(const char *name, double scale, const int8_t n[3], const int8_t ld[3], int lc, int amb)
{
    static const double ignore = 0;
    int got[3], want[3], c;
    double len = sqrt((double)ld[0] * ld[0] + (double)ld[1] * ld[1] + (double)ld[2] * ld[2]);
    double d = ((double)n[0] * ld[0] + (double)n[1] * ld[1] + (double)n[2] * ld[2]) / len / 127.0;
    (void)ignore;
    if (d < 0) d = 0;
    for (c = 0; c < 3; c++) want[c] = clampi((int)(amb + lc * d + 0.5));

    common_setup();
    ADD(gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE));
    ADD(gsSPClearGeometryMode(0xFFFFFF));
    ADD(gsSPSetGeometryMode(G_SHADE | G_SHADING_SMOOTH | G_LIGHTING));
    diag_mtx(sMtxM, scale);
    ADD(gsSPMatrix(sMtxM, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH));
    memset(sLight, 0, sizeof sLight);
    sLight[0][0] = sLight[0][1] = sLight[0][2] = (uint8_t)lc; sLight[0][4] = sLight[0][5] = sLight[0][6] = (uint8_t)lc;
    sLight[0][8] = (uint8_t)ld[0]; sLight[0][9] = (uint8_t)ld[1]; sLight[0][10] = (uint8_t)ld[2];
    sLight[1][0] = sLight[1][1] = sLight[1][2] = (uint8_t)amb; sLight[1][4] = sLight[1][5] = sLight[1][6] = (uint8_t)amb;
    ADD(gsSPNumLights(NUMLIGHTS_1));
    ADD(gsSPLight(sLight[0], 1));
    ADD(gsSPLight(sLight[1], 2));
    make_vertices((int)(16.0 / scale + 0.5), n);
    ADD(gsSPVertex(sVtx, 3, 0));
    ADD(gsSP1Triangle(0, 1, 2, 0));
    ADD(gsDPFullSync());
    ADD(gsSPEndDisplayList());
    run_and_read(got);
    check(name, got, want, 9);
}

static void test_lighting(void)
{
    static const double scales[3] = {1.0, 0.1, 0.5};
    static const int8_t normals[4][3] = {{0, -127, 0}, {57, -97, 39}, {-87, -16, -80}, {40, -30, 110}};
    static const int8_t light[3] = {13, -112, 38};          /* a direction the game really uses (seen in a debug trace of the intro) */
    char name[96];
    unsigned s, k;
    for (k = 0; k < 4; k++) {
        for (s = 0; s < 3; s++) {
            snprintf(name, sizeof name, "lighting normal %u, modelview scale %.1f", k, scales[s]);
            lighting_case(name, scales[s], normals[k], light, 120, 40);     /* light 120 + ambient 40: no clamping, so partial lighting shows */
        }
    }
}

/* ---- 3. texture rectangle sampling (the "fuzzy battle text" regression) --------------------------------------------------------- */

static uint8_t sTex[8 * 8 * 2] __attribute__((aligned(16)));     /* 8 x 8 RGBA5551: column 3 white, everything else black (big-endian halves) */

/* The pixel (x, y) of the finished picture as the 5-bit red value times 8 (white = 248, black = 0). */
static int red_at(int x, int y)
{
    unsigned v = ((unsigned)sColorImage[(y * W + x) * 2] << 8) | sColorImage[(y * W + x) * 2 + 1];
    return (int)((v >> 11) & 31) * 8;
}

/* A texture rectangle drawn 1:1 (s0 = t0 = 0, dsdx = dtdy = 1.0 texel per pixel) must show the texels EXACTLY, also with the bilinear
 * filter on: the texel (3, y) lands on the screen column rx + 3 and the neighbours stay black. Basis: Nintendo N64 Programming Manual 14.1,
 * where the exact four-texel average needs the start point 0.5 ("set the start point to 0.5 and then step by 1 texel per pixel"), i.e.
 * texel i is AT coordinate i. The previous renderer shifted bilinear samples by half a texel: column 3 came out half grey, columns 2 and 4
 * half grey as well (the blurred battle text). */
static void texrect_finish(const char *name)
{
    const int rx = 20, ry = 20;
    int x, bad = 0, px[8];
    ADD(gsDPSetCombineMode(G_CC_DECALRGB, G_CC_DECALRGB));
    ADD(gsDPLoadTextureBlock(sTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, 8, 0, G_TX_CLAMP, G_TX_CLAMP, 3, 3, G_TX_NOLOD, G_TX_NOLOD));
    ADD(gsSPTextureRectangle(20 << 2, 20 << 2, 28 << 2, 28 << 2, G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10));
    ADD(gsDPFullSync());
    ADD(gsSPEndDisplayList());
    swap_dl();
    memset(sColorImage, 0, sizeof sColorImage);
    rdp_reset();
    gfx_reset_rsp();
    gfx_run_display_list((uint32_t)(uintptr_t)sDl);
    for (x = 0; x < 8; x++) {
        px[x] = red_at(rx + x, ry + 4);
        if (px[x] != (x == 3 ? 248 : 0)) bad++;
    }
    printf("%s %-46s columns %3d %3d %3d %3d %3d %3d %3d %3d (want 0 0 0 248 0 0 0 0)\n", bad ? "FAIL" : "ok  ", name, px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7]);
    if (bad) sFailures++;
}
/* A macro for the same reason as COMBINER_CASE: the filter command is a brace initialiser. */
#define TEXRECT_CASE(NAME, FILTER) do { texrect_prepare(); common_setup(); ADD(gsDPSetTextureFilter(FILTER)); texrect_finish(NAME); } while (0)

/* Fills sTex: black everywhere (alpha bit set), column 3 white. Called before each case. */
static void texrect_prepare(void)
{
    int x;
    memset(sTex, 0, sizeof sTex);
    for (x = 0; x < 64; x++) { sTex[x * 2] = 0x00; sTex[x * 2 + 1] = 0x01; }                       /* black, alpha bit set */
    for (x = 0; x < 8; x++) { sTex[(x * 8 + 3) * 2] = 0xFF; sTex[(x * 8 + 3) * 2 + 1] = 0xFF; }    /* column 3 white */
}

int main(void)
{
    test_combiner();
    test_lighting();
    TEXRECT_CASE("texture rectangle 1:1, point sampling", G_TF_POINT);
    TEXRECT_CASE("texture rectangle 1:1, bilinear", G_TF_BILERP);
    printf(sFailures ? "%d CHECK(S) FAILED\n" : "all checks passed\n", sFailures);
    return sFailures ? 1 : 0;
}
