/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * test_gbjp.c - checks src/platform/host_gbjp.c (Japanese Pokemon Green -> what the US Stadium reads).
 *
 *   cc -I src/platform -o test_gbjp tests/native/gbjp/test_gbjp.c src/platform/host_gbjp.c
 *   ./test_gbjp [real-midori.sav [real-midori.gb]]       (the real files are optional: they are the user's own and never stored in this repository)
 *
 * The synthetic save is built here from the layout the decomp documents; the real files, when given, are checked against the same rules. The checks
 * use the Stadium's own rules written out again here (GbSave_VerifyChecksum, the offsets of src/gb_data.h), not the code under test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_gbjp.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else printf("ok   " __VA_ARGS__), printf("\n"); } while (0)

/* the Stadium's rule for the international main data (src/pokemon_stats.c GbSave_VerifyChecksum): a 0x50 in the 11-byte name, and the bytes from the name
 * to the checksum byte (0xFA3 into the block at 0x2580), that byte included, add up to 0xFF modulo 256 */
static int us_main_valid(const unsigned char *view)
{
    const unsigned char *m = view + 0x2580;
    unsigned sum = 0, i;
    int marker = 0;
    for (i = 0; i < 11; i++) if (m[0x18 + i] == 0x50) marker = 1;
    for (i = 0x18; i <= 0xFA3; i++) sum += m[i];
    return marker && (sum & 0xFF) == 0xFF;
}

static void fix_jp_checksum(unsigned char *jp)
{
    unsigned char *m = jp + 0x2580;
    unsigned sum = 0, i;
    for (i = 0x18; i < 0x1014; i++) sum += m[i];
    m[0x1014] = (unsigned char)(0xFF - (sum & 0xFF));
}

static void names(void)
{
    unsigned char out[11];
    /* codes from the Japanese table (gbjp_names.inc): ラ=A5? no: ラ=0xA5 プ=0x42 ラ ス=0x8C. (see the table: 0xA5 ラ, 0x42 プ, 0x8C ス) */
    static const unsigned char lapras[6] = { 0xA5, 0x42, 0xA5, 0x8C, 0x50, 0x50 };
    static const unsigned char alakazam[6] = { 0x9B, 0xE3, 0x12, 0xB0, 0xAB, 0x50 };       /* フ ー デ ィ ン */
    static const unsigned char golem[6] = { 0x09, 0xA8, 0xE3, 0x95, 0xAD, 0x50 };          /* ゴ ロ ー ニ ャ */
    static const unsigned char raichu[6] = { 0xA5, 0x81, 0x90, 0xAE, 0x82, 0x50 };         /* ラ イ チ ュ ウ */
    static const unsigned char victreebel[6] = { 0x82, 0x91, 0x1C, 0xAC, 0x93, 0x50 };     /* ウ ツ ボ ッ ト */
    static const unsigned char charizard[6] = { 0xD8, 0x0A, 0xE3, 0x13, 0xAB, 0x50 };      /* リ ザ ー ド ン */
    static const unsigned char green[6] = { 0x07, 0xD8, 0xE3, 0xAB, 0x50, 0x8B };          /* グ リ ー ン (the byte after the terminator is ignored) */
    char text[16];
    int i;
#define AS_TEXT(src) do { gbjp_name_to_us(src, out); for (i = 0; i < 11 && out[i] != 0x50; i++) text[i] = out[i] >= 0x80 && out[i] <= 0x99 ? (char)('A' + out[i] - 0x80) : out[i] == 0xE3 ? '-' : '?'; text[i] = 0; } while (0)
    AS_TEXT(lapras);    CHECK(strcmp(text, "RAPURASU") == 0, "name: ラプラス -> %s", text);
    AS_TEXT(alakazam);  CHECK(strcmp(text, "FU-DIN") == 0, "name: フーディン -> %s (small i replaces the E of DE)", text);
    AS_TEXT(golem);     CHECK(strcmp(text, "GORO-NYA") == 0, "name: ゴローニャ -> %s", text);
    AS_TEXT(raichu);    CHECK(strcmp(text, "RAICHUU") == 0, "name: ライチュウ -> %s", text);
    AS_TEXT(victreebel);CHECK(strcmp(text, "UTSUBOTTO") == 0, "name: ウツボット -> %s (small tsu doubles the T)", text);
    AS_TEXT(charizard); CHECK(strcmp(text, "RIZA-DON") == 0, "name: リザードン -> %s", text);
    AS_TEXT(green);     CHECK(strcmp(text, "GURI-N") == 0, "name: グリーン -> %s", text);
    gbjp_name_to_us(green, out);
    CHECK(out[6] == 0x50 && out[7] == 0 && out[10] == 0, "name ends with the terminator and zeros");
}

static void synthetic(void)
{
    unsigned char jp[GBJP_SRAM_SIZE], *v = (unsigned char *)malloc(GBJP_SRAM_SIZE);
    unsigned char *m = jp + 0x2580;
    int i;
    memset(jp, 0, sizeof jp);
    m[0x18] = 0x07; m[0x19] = 0xD8; m[0x1A] = 0xE3; m[0x1B] = 0xAB; m[0x1C] = 0x50;    /* グリーン */
    m[0x1E] = 0xAA; m[0x31] = 0xBB;                                                      /* Pokedex owned / seen first byte */
    m[0x44] = 2; m[0x45] = 6; m[0x46] = 1; m[0x47] = 0xFF;                               /* bag: 2 items */
    m[0x71] = 0x8B; m[0x72] = 0x50;                                                      /* rival's name */
    m[0x7B] = 0x12; m[0x7C] = 0x34;                                                      /* trainer ID */
    m[0x2C2] = 0x80 | 2;                                                                 /* boxes in use, current box 3 (index 2) */
    m[0x955] = 1; m[0x956] = 0x54; m[0x957] = 0xFF;                                      /* party of one */
    m[0x955 + 8] = 0x54;                                                                 /* its record: species */
    memset(m + 0x955 + 0x110, 0x50, 6); memset(m + 0x955 + 0x134, 0x50, 6);
    /* boxes: JP box 1 (bank 0, index 0): 25 Pokemon, JP box 3 (current, in the main data): 5 Pokemon, others empty */
    {
        unsigned char *b = jp + 0x4000;
        b[0] = 25;
        for (i = 0; i < 25; i++) { b[1 + i] = (unsigned char)(10 + i); b[0x20 + i * 33] = (unsigned char)(10 + i); b[0x3FE + i * 6] = 0x8B; b[0x3FE + i * 6 + 1] = 0x50; b[0x4B2 + i * 6] = 0x80; b[0x4B2 + i * 6 + 1] = 0x50; }
        b = m + 0xAAD;
        b[0] = 5;
        for (i = 0; i < 5; i++) { b[1 + i] = (unsigned char)(100 + i); b[0x20 + i * 33] = (unsigned char)(100 + i); b[0x3FE + i * 6] = 0x8B; b[0x3FE + i * 6 + 1] = 0x50; b[0x4B2 + i * 6] = 0x80; b[0x4B2 + i * 6 + 1] = 0x50; }
    }
    fix_jp_checksum(jp);
    CHECK(gbjp_save_to_us_layout(jp, v) == 1, "a valid synthetic Japanese save converts");
    CHECK(us_main_valid(v), "the converted main data passes the Stadium's checksum rule");
    {
        unsigned char *u = v + 0x2580;
        CHECK(u[0x23] == 0xAA && u[0x36] == 0xBB, "Pokedex flags moved from 0x1E/0x31 to 0x23/0x36");
        CHECK(u[0x49] == 2 && u[0x4A] == 6 && u[0x4B] == 1, "bag moved from 0x44 to 0x49");
        CHECK(u[0x85] == 0x12 && u[0x86] == 0x34, "trainer ID moved from 0x7B to 0x85");
        CHECK(u[0x2CC] == 0x80, "boxes in use, current box 1");
        CHECK(u[0x9AC] == 1 && u[0x9AD] == 0x54 && u[0x9AC + 8] == 0x54, "party moved from 0x955 to 0x9AC");
        CHECK(u[0xB40] == 20 && u[0xB41] == 10 && u[0xB41 + 19] == 29, "current box = the first 20 Pokemon (JP box 1, in order)");
        CHECK(v[0x4000 + 0x462 + 0] == 10 && v[0x4000 + 0x462 + 1] == 30 && v[0x4000 + 0x462 + 6] == 100, "box 2 = the rest of JP box 1 (5), then the 5 of JP box 3 (the current box)");
        CHECK(v[0x4000 + 0x462 + 0x16 + 5 * 33] == 100, "its fifth record is a Pokemon of the current box");
        CHECK(v[0x4000 + 2 * 0x462] == 0 && v[0x4000 + 2 * 0x462 + 1] == 0xFF, "box 3 is empty");
        CHECK(v[0x6000 + 5 * 0x462] == 0, "box 12 is empty");
    }
    /* a damaged checksum -> not converted, all 0xFF */
    jp[0x2580 + 0x30] ^= 1;
    CHECK(gbjp_save_to_us_layout(jp, v) == 0 && v[0x2580 + 0x18] == 0xFF && v[0] == 0xFF, "a Japanese save with a bad checksum gives an all-0xFF view");
    free(v);
}

static void rom(const unsigned char *real, unsigned long size)
{
    unsigned char *r = NULL;
    unsigned char fake[0x80000];
    unsigned long i, sum = 0;
    memset(fake, 0, sizeof fake);
    memcpy(fake + 0x134, "POKEMON GREEN", 13);
    fake[0x147] = 3; fake[0x148] = 4; fake[0x149] = 3; fake[0x14A] = 0;
    { unsigned char x = 0; for (i = 0x134; i <= 0x14C; i++) x = (unsigned char)(x - fake[i] - 1); fake[0x14D] = x; }
    CHECK(gbjp_is_supported_rom(fake, sizeof fake), "a Japanese Green header is recognised");
    fake[0x14A] = 1;
    CHECK(!gbjp_is_supported_rom(fake, sizeof fake), "an international header is not");
    fake[0x14A] = 0;
    if (real) { CHECK(gbjp_is_supported_rom(real, size), "the real Midori ROM is recognised"); memcpy(fake, real, size); }
    r = gbjp_make_us_rom(fake, sizeof fake);
    CHECK(r != NULL && r[0x148] == 5 && r[0x14A] == 1 && r[0x147] == 0x13, "US header values: size code 5, destination 1, chip type MBC3 (0x13)");
    { unsigned char x = 0; for (i = 0x134; i <= 0x14C; i++) x = (unsigned char)(x - r[i] - 1); CHECK(x == r[0x14D], "header checksum is right"); }
    for (i = 0; i < 0x100000; i++) if (i != 0x14E && i != 0x14F) sum += r[i];
    CHECK(((r[0x14E] << 8) | r[0x14F]) == (int)(sum & 0xFFFF), "the global checksum fits the Stadium's rule (all bytes but the two)");
    CHECK(memcmp(r + 0x150, fake + 0x150, 0x80000 - 0x150) == 0 && r[0x80000] == 0 && r[0xFFFFF] == 0, "the game's bytes are unchanged, the upper half is zero");
    free(r);
}

int main(int argc, char **argv)
{
    unsigned char *real_rom = NULL, *sav = NULL;
    unsigned long rom_size = 0, sav_size = 0;
    FILE *f;
    names();
    synthetic();
    if (argc > 2 && (f = fopen(argv[2], "rb"))) { real_rom = malloc(0x100000); rom_size = fread(real_rom, 1, 0x100000, f); fclose(f); }
    rom(real_rom, rom_size);
    if (argc > 1 && (f = fopen(argv[1], "rb"))) {
        sav = malloc(0x10000); sav_size = fread(sav, 1, 0x10000, f); fclose(f);
        CHECK(sav_size == GBJP_SRAM_SIZE, "real save is 32 KiB");
        if (sav_size == GBJP_SRAM_SIZE) {
            unsigned char *v = malloc(GBJP_SRAM_SIZE);
            int i, mons = 0;
            CHECK(gbjp_save_to_us_layout(sav, v) == 1, "the real Midori save converts");
            CHECK(us_main_valid(v), "and passes the Stadium's checksum rule");
            printf("     party count %d, player name codes:", v[0x2580 + 0x9AC]);
            for (i = 0; i < 11; i++) printf(" %02X", v[0x2580 + 0x18 + i]);
            printf("\n     boxes (count of each, 12):");
            for (i = 0; i < 12; i++) { int c = v[(i < 6 ? 0x4000 : 0x6000) + (i % 6) * 0x462]; mons += c; printf(" %d", c); }
            printf("   total %d\n", mons);
            free(v);
        }
    }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
