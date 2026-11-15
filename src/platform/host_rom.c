/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_rom.c - loads the player's cartridge ROM into memory and checks that it is the expected game.
 *
 * Formats: the three byte orders in circulation are told apart by the first four bytes of the file
 *   80 37 12 40  .z64  big-endian, the order of the N64's own memory      (kept as is)
 *   37 80 40 12  .v64  every pair of bytes swapped                         (swapped back)
 *   40 12 37 80  .n64  every 4-byte word reversed                          (reversed back)
 * (these are the three encodings of the header's first word 0x80371240, the PI configuration word every N64 ROM starts with).
 *
 * Identity: the SHA-1 digest (FIPS 180-4) of the converted image must be the one the decomp records for the US ROM in
 * yamls/us/header.yaml. All offsets used elsewhere in this port (segment tables, DMA sources) are those of that exact image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

/* Copied from the decomp: yamls/us/header.yaml, key `sha1`. */
#define EXPECTED_SHA1 "ed7bef5a306f88c0a6e96b15e71fee2ef32058f3"

/* Largest cartridge the format allows for this game's era; guards against reading an unrelated huge file into memory. */
#define MAX_ROM_BYTES (64u * 1024u * 1024u)

static unsigned char *sRom;
static unsigned int   sRomSize;

/* ---- SHA-1 (FIPS 180-4, section 6.1.2) --------------------------------------------------------------------------------------------- */

typedef struct { unsigned int h[5]; } Sha1State;

static unsigned int rol(unsigned int v, int n) { return (v << n) | (v >> (32 - n)); }

/* Processes one 64-byte block. */
static void sha1_block(Sha1State *s, const unsigned char *p)
{
    unsigned int w[80], a, b, c, d, e, t;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned int)p[4 * i] << 24) | ((unsigned int)p[4 * i + 1] << 16) | ((unsigned int)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        unsigned int f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

/* Hex digest of `len` bytes at `data` (40 lowercase characters + NUL) written to `out`. */
static void sha1_hex(const unsigned char *data, size_t len, char out[41])
{
    Sha1State s = {{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u}};
    unsigned char tail[128];
    size_t full = len / 64, rem = len % 64, i, tail_len;
    unsigned long long bits = (unsigned long long)len * 8;
    for (i = 0; i < full; i++) sha1_block(&s, data + 64 * i);
    memset(tail, 0, sizeof tail);
    memcpy(tail, data + 64 * full, rem);
    tail[rem] = 0x80;                                   /* padding: a 1 bit, zeros, then the 64-bit length in bits, big-endian */
    tail_len = (rem < 56) ? 64 : 128;
    for (i = 0; i < 8; i++) tail[tail_len - 1 - i] = (unsigned char)(bits >> (8 * i));
    sha1_block(&s, tail);
    if (tail_len == 128) sha1_block(&s, tail + 64);
    for (i = 0; i < 5; i++) snprintf(out + 8 * i, 9, "%08x", s.h[i]);
}

/* ---- loading --------------------------------------------------------------------------------------------------------------------- */

/* Reads `path`, converts it to the big-endian order and checks it is the supported ROM. On success *out / *out_size hold a new buffer (the
 * caller owns it); on failure they are untouched and the result is the message. Nothing global is changed, so this serves both the loading
 * at start-up and the menu's check of a ROM the player picked. */
static const char *read_and_check(const char *path, unsigned char **out, unsigned int *out_size)
{
    static char msg[300];
    FILE *f = fopen(path, "rb");
    long size;
    unsigned int i;
    unsigned char *rom;
    char digest[41];

    if (!f) { snprintf(msg, sizeof msg, "cannot open the ROM file '%s'", path); return msg; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0x1000 || (unsigned long)size > MAX_ROM_BYTES || (size & 3)) {
        fclose(f);
        snprintf(msg, sizeof msg, "'%s' does not look like an N64 ROM (size %ld bytes)", path, size);
        return msg;
    }
    rom = (unsigned char *)malloc((size_t)size);
    if (!rom || fread(rom, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(rom);
        snprintf(msg, sizeof msg, "could not read the ROM file '%s'", path);
        return msg;
    }
    fclose(f);

    if (rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40) {
        /* .z64: already big-endian */
    } else if (rom[0] == 0x37 && rom[1] == 0x80 && rom[2] == 0x40 && rom[3] == 0x12) {
        for (i = 0; i < (unsigned)size; i += 2) { unsigned char t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t; }
    } else if (rom[0] == 0x40 && rom[1] == 0x12 && rom[2] == 0x37 && rom[3] == 0x80) {
        for (i = 0; i < (unsigned)size; i += 4) {
            unsigned char a = rom[i], b = rom[i + 1];
            rom[i] = rom[i + 3]; rom[i + 1] = rom[i + 2]; rom[i + 2] = b; rom[i + 3] = a;
        }
    } else {
        free(rom);
        snprintf(msg, sizeof msg, "'%s' does not start with an N64 ROM header (first word is not 0x80371240 in any byte order)", path);
        return msg;
    }

    sha1_hex(rom, (size_t)size, digest);
    if (strcmp(digest, EXPECTED_SHA1) != 0) {
        free(rom);
        snprintf(msg, sizeof msg, "this is not the supported ROM (Pokemon Stadium, USA): SHA-1 is %s, expected %s", digest, EXPECTED_SHA1);
        return msg;
    }
    *out = rom;
    *out_size = (unsigned int)size;
    return NULL;
}

const char *host_rom_load(const char *path)
{
    unsigned char *rom = NULL;
    unsigned int size = 0;
    const char *err = read_and_check(path, &rom, &size);
    if (err) return err;
    free(sRom);
    sRom = rom;
    sRomSize = size;
    return NULL;
}

const char *host_rom_check(const char *path)
{
    unsigned char *rom = NULL;
    unsigned int size = 0;
    const char *err = read_and_check(path, &rom, &size);
    if (err) return err;
    free(rom);
    return NULL;
}

const unsigned char *host_rom_data(void) { return sRom; }
unsigned int host_rom_size(void) { return sRomSize; }

void host_copy_from_rom(unsigned int offset, void *dst, unsigned int len)
{
    if (!sRom || offset > sRomSize || len > sRomSize - offset) host_fatal("read outside the loaded ROM");
    memcpy(dst, sRom + offset, len);
}
