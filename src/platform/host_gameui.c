/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_gameui.c - the game's own letters and the Game Pak Check page's background, read from the player's ROM, for the menus.
 *
 * The menus (host_menu.c) should look like the game: the same font, and the background of the game's Game Pak Check page (the page with the
 * pak icons and the party list; tiled "the game" logo in olive and gold). Nothing of that is stored in this program: both are read from
 * the ROM that is loaded anyway, the first time the menu needs them.
 *
 * Both live inside Yay0 compressed ROM assets ("PERS-SZP": a header whose first fields are the header size at +8 and the decompressed
 * size at +0x10, then the Yay0 data at +header size; decompressed with the game's own Yay0_Decompress).
 *
 *   Font       ROM archive `fonts` (rom.yaml 0x3BA190). Six files, one per letter size, each such an asset.
 *              Decompressed, a file is   0x00..0x8F  the advance width of each of the 144 glyph slots (one byte each)
 *                                        0x90..      the glyph bitmaps: slot n at 0x90 + n*W*H, W*H bytes, 8-bit "IA": high nibble = intensity,
 *                                                    low nibble = alpha.
 *              (src/graphics_textures.c: Font_LoadSet, Font_DrawChar.) The sizes (W x H) of the six files are those of Font_LoadSizes:
 *              10 high, 12, 16, 20, 24 and 32 (the first two only 16 wide, then 16, 24, 24, 32).
 *              Which slot a character code uses is the game's own table, reached through the game's function Font_MapGlyphCode.
 *              Drawing follows Font_DrawChar: a glyph is shown `advance` pixels wide (not its whole cell), and the next one starts
 *              advance-2 pixels further (sizes 16 and up), so that the outlines of neighbours overlap.
 *   Background The Game Pak Check page (src/fragments/transfer_pak_check, PakUi_DrawMenuBackdrop) fills the whole 640x480 picture with one
 *              128x64 texture, 4-bit intensity ("I4", 2 pixels per byte, high nibble first), repeated, with the colour combiner
 *                 colour = (ENV - PRIM) * texel + PRIM,   PRIM = (120,100,40), ENV = (180,180,100)
 *              (gDPSetCombineLERP(ENVIRONMENT, PRIMITIVE, TEXEL0, PRIMITIVE, ...), gDPSetPrimColor 120,100,40, gDPSetEnvColor 180,180,100).
 *              The texture is at offset 0xF780 of the asset `menu_select_ui` (ROM 0x4EB0C0, the game's segment 3: D_300F780).
 *              A 4-bit intensity i is the 8-bit value i*17, as the RDP expands it.
 * HOST-side file. The two game-side functions it calls (Yay0_Decompress, Font_MapGlyphCode) are pure functions of their arguments.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "host_ui.h"

/* Game-side (compiled through the big-endian rewrite, see tests/native/yay0): called with plain byte buffers. */
extern void Yay0_Decompress(void *src, void *dst);
extern unsigned char Font_MapGlyphCode(unsigned short code);

#define FONTS_ROM_START 0x3BA190u
#define FONTS_ROM_END   0x3CB130u
#define MENU_SELECT_ROM_START 0x4EB0C0u        /* menu_select_ui */
#define MENU_SELECT_ROM_END   0x5046B0u
#define BG_TEXTURE_OFFSET     0xF780u          /* D_300F780 */

#define GLYPH_SLOTS 145                    /* 0x90 bytes of widths cover 144; the files hold 145 bitmaps (see the checks in tests/native/ui) */
#define HEADER_BYTES 0x90u

static const struct { int w, h; } kSets[HUI_SIZES] = { {16, 10}, {16, 12}, {16, 16}, {24, 20}, {24, 24}, {32, 32} };

typedef struct Font {
    unsigned char *file;                   /* the decompressed file, or NULL if it could not be read */
    unsigned size;
} Font;

static Font sFont[HUI_SIZES];
static int sFontTried;
static unsigned char *sBg;                 /* 640 x 480 x 3 */
static int sBgTried;

static unsigned be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }
static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* ---- drawing on a canvas ----------------------------------------------------------------------------------------------------------- */

void host_canvas_blend(HostCanvas *cv, int x, int y, unsigned rgb, unsigned alpha)
{
    unsigned fx = cv->fx ? cv->fx : 65536u, fy = cv->fy ? cv->fy : 65536u;
    long px0, px1, py0, py1, px, py;
    unsigned inv;
    if (x < 0 || y < 0 || alpha == 0) return;
    if (alpha > 255) alpha = 255;
    inv = 255 - alpha;
    px0 = (long)(((unsigned long long)x * fx) >> 16);
    px1 = (long)(((unsigned long long)(x + 1) * fx) >> 16);
    py0 = (long)(((unsigned long long)y * fy) >> 16);
    py1 = (long)(((unsigned long long)(y + 1) * fy) >> 16);
    if (px1 <= px0) px1 = px0 + 1;                                 /* a shrunk picture still shows every drawing pixel, once or overlapped */
    if (py1 <= py0) py1 = py0 + 1;
    for (py = py0; py < py1 && py < cv->h; py++) {
        for (px = px0; px < px1 && px < cv->w; px++) {
            unsigned char *p = cv->rgb + ((size_t)py * (size_t)cv->w + (size_t)px) * 3u;
            p[0] = (unsigned char)((p[0] * inv + (rgb >> 16 & 255) * alpha + 127) / 255);
            p[1] = (unsigned char)((p[1] * inv + (rgb >> 8 & 255) * alpha + 127) / 255);
            p[2] = (unsigned char)((p[2] * inv + (rgb & 255) * alpha + 127) / 255);
        }
    }
}

void host_canvas_fill(HostCanvas *cv, int x, int y, int w, int h, unsigned rgb, unsigned alpha)
{
    int i, j;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) host_canvas_blend(cv, x + i, y + j, rgb, alpha);
}

/* ---- font ------------------------------------------------------------------------------------------------------------------------- */

/* Decompresses the "PERS-SZP" asset at ROM offset `start` (at most `max_size` bytes long). Returns a new buffer (caller frees) and its size, or NULL. */
static unsigned char *load_pers(unsigned start, unsigned max_size, unsigned min_out, unsigned *out_size)
{
    const unsigned char *rom = host_rom_data(), *p;
    unsigned header, out;
    unsigned char *src, *dst;
    if (!rom || start + 0x20 > host_rom_size() || start + max_size > host_rom_size()) return NULL;
    p = rom + start;
    if (memcmp(p, "PERS-SZP", 8) != 0) return NULL;
    header = be32(p + 8);                                          /* PERSSZP: header size, decompressed sizes (+0xC, +0x10) */
    out = be32(p + 0x10);
    if (header >= max_size || out < min_out || out > (1u << 20)) return NULL;
    if (be32(p + 0x14) != 0) return NULL;                          /* the game patches pointers into some assets; these have none, so this is not what we expect */
    if (memcmp(p + header, "Yay0", 4) != 0) return NULL;
    src = (unsigned char *)malloc(max_size - header);              /* a private copy: the decompressor reads 32-bit words */
    dst = (unsigned char *)malloc(out);
    if (!src || !dst) { free(src); free(dst); return NULL; }
    memcpy(src, p + header, max_size - header);
    Yay0_Decompress(src, dst);
    free(src);
    *out_size = out;
    return dst;
}

static void load_fonts(void)
{
    const unsigned char *rom = host_rom_data();
    const unsigned char *ar;
    unsigned n, i;
    if (sFontTried) return;
    sFontTried = 1;
    if (!rom || host_rom_size() < FONTS_ROM_END) return;
    ar = rom + FONTS_ROM_START;                                    /* BinArchive: 16-byte header (file count at +12), then 16 bytes per file */
    n = be32(ar + 12);
    if (n < HUI_SIZES) return;
    for (i = 0; i < HUI_SIZES; i++) {
        unsigned off = be32(ar + 16 + 16 * i), size = be32(ar + 20 + 16 * i);
        if (off + size > FONTS_ROM_END - FONTS_ROM_START) continue;
        sFont[i].file = load_pers(FONTS_ROM_START + off, size, HEADER_BYTES + (unsigned)(GLYPH_SLOTS * kSets[i].w * kSets[i].h), &sFont[i].size);
    }
}

int host_gameui_font_ok(int size)
{
    load_fonts();
    return size >= 0 && size < HUI_SIZES && sFont[size].file != NULL;
}

int host_text_height(int size) { return size >= 0 && size < HUI_SIZES ? kSets[size].h : 0; }

static int glyph_slot(unsigned char c)
{
    int slot = Font_MapGlyphCode(c);
    return slot < GLYPH_SLOTS ? slot : 0;
}

static int advance(int size, unsigned char c)
{
    return sFont[size].file[glyph_slot(c)];
}

int host_text_width(int size, const char *s)
{
    int x = 0, widest = 0;
    if (!host_gameui_font_ok(size)) return 0;
    for (; *s; s++) {
        if (*s == '\n') { if (x > widest) widest = x; x = 0; continue; }
        x += advance(size, (unsigned char)*s) - (size >= 2 ? 2 : 1);
    }
    return x > widest ? x : widest;
}

void host_text_draw(HostCanvas *cv, int size, int x, int y, const char *s, unsigned rgb, int shadow)
{
    int x0 = x;
    if (!host_gameui_font_ok(size)) return;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        int adv, slot, gx, gy;
        const unsigned char *bm;
        if (c == '\n') { x = x0; y += kSets[size].h + 2; continue; }
        slot = glyph_slot(c);
        adv = sFont[size].file[slot];
        bm = sFont[size].file + HEADER_BYTES + (unsigned)slot * kSets[size].w * kSets[size].h;
        for (gy = 0; gy < kSets[size].h; gy++) {
            for (gx = 0; gx < adv && gx < kSets[size].w; gx++) {
                unsigned v = bm[gy * kSets[size].w + gx], inten = v >> 4, alpha = v & 15;
                if (alpha == 0) continue;
                if (shadow) host_canvas_blend(cv, x + gx + 1, y + gy + 1, 0x000000u, alpha * 17 / 2);       /* a soft dark edge, half as strong */
                host_canvas_blend(cv, x + gx, y + gy, ((rgb >> 16 & 255) * inten / 15) << 16 | ((rgb >> 8 & 255) * inten / 15) << 8 | ((rgb & 255) * inten / 15), alpha * 17);
            }
        }
        x += adv - (size >= 2 ? 2 : 1);
    }
}

/* ---- background ---------------------------------------------------------------------------------------------------------------- */

const unsigned char *host_gameui_background(void)
{
    static const int prim[3] = {120, 100, 40}, env[3] = {180, 180, 100};
    unsigned char *asset;
    unsigned size = 0;
    int x, y, c;
    if (sBgTried) return sBg;
    sBgTried = 1;
    asset = load_pers(MENU_SELECT_ROM_START, MENU_SELECT_ROM_END - MENU_SELECT_ROM_START, BG_TEXTURE_OFFSET + 128 * 64 / 2, &size);
    if (!asset) return NULL;
    sBg = (unsigned char *)malloc((size_t)HUI_W * HUI_H * 3);
    if (sBg) {
        for (y = 0; y < HUI_H; y++) {
            for (x = 0; x < HUI_W; x++) {
                unsigned tx = (unsigned)x & 127, ty = (unsigned)y & 63, idx = ty * 128 + tx;
                unsigned byte = asset[BG_TEXTURE_OFFSET + idx / 2];
                int t = (int)((idx & 1 ? byte & 15u : byte >> 4) * 17u);        /* I4 -> 0..255 */
                for (c = 0; c < 3; c++) sBg[((size_t)y * HUI_W + x) * 3 + c] = (unsigned char)(prim[c] + (env[c] - prim[c]) * t / 255);
            }
        }
    }
    free(asset);
    return sBg;
}
