/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * yay0_test_host.c - HOST-side driver that tests src/game_asm/yay0.c (compiled through the big-endian rewrite) on every Yay0
 * block found in a file (the player's ROM, in practice). For each block it prints one line
 *         Y <file offset> <decoded size> <FNV-1a 32-bit hash of the decoded bytes> <guard ok>
 * and tools/native/test_yay0.py compares these lines with a decoder written separately in Python.
 * The block is copied into a buffer of its own first, so a decoder that read past the block's end would be seen by the run-time
 * guard of the Python side only through a wrong hash; the output buffer has guard bytes after it, checked here.
 *
 * Usage: yay0_test_host <file>        (the file is read as plain bytes: no ROM check, no byte swapping)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void Yay0_Decompress(void *src, void *dst);        /* game side (src/game_asm/yay0.c) */
extern unsigned int host_swap_game_data(void);

/* The game side calls this on corrupt data. */
void host_fatal(const char *message)
{
    fprintf(stderr, "FATAL: %s\n", message);
    exit(2);
}

static unsigned int be32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *rom;
    long size, pos;
    unsigned int blocks = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
    }
    host_swap_game_data();
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    rom = malloc(size);
    if (!rom || fread(rom, 1, size, f) != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(f);

    for (pos = 0; pos + 16 <= size; pos++) {
        unsigned int out_size, i, hash = 2166136261u, guard_ok = 1;
        unsigned char *out;
        if (memcmp(rom + pos, "Yay0", 4) != 0) {
            continue;
        }
        out_size = be32(rom + pos + 4);
        out = malloc(out_size + 16);
        memset(out, 0xA5, out_size + 16);
        Yay0_Decompress(rom + pos, out);
        for (i = 0; i < 16; i++) {                        /* nothing may be written after the decoded size */
            guard_ok &= out[out_size + i] == 0xA5;
        }
        for (i = 0; i < out_size; i++) {
            hash = (hash ^ out[i]) * 16777619u;
        }
        printf("Y %ld %u %u %u\n", pos, out_size, hash, guard_ok);
        free(out);
        blocks++;
    }
    printf("DONE %u blocks\n", blocks);
    return 0;
}
