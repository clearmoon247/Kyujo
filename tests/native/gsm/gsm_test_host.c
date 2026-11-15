/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * gsm_test_host.c - HOST-side driver that compares src/game_asm/libgsm.c (compiled through the big-endian rewrite, i.e. exactly as the
 * game uses it) with the console's ORIGINAL machine code, run in the reference interpreter tests/native/mips_ref.
 *
 * Usage: gsm_test_host <rom file>
 * The 0x600 bytes of machine code are taken from ROM offset 0x49190 and placed at console address 0x80048590, where the game had them.
 * Every case fills a region of memory with random bytes, copies it, runs the original code on one copy and the C function on the other, and
 * compares the whole region (inputs that must stay unchanged included). The random generator is fixed, so a failure is reproducible.
 * Prints one line per function:  <name> <cases> <mismatches>   and exits with status 1 if any mismatch (details on stderr).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mips_ref.h"

/* GAME side (src/game_asm/libgsm.c). Plain-integer signatures; the pointers are native addresses of big-endian buffers. */
extern void Gsm_RPE_Decoding(int xmaxcr, int Mcr, short *xMcr, short *erp);
extern void Gsm_Long_term_synthesis_filtering(void *S, int Ncr, int bcr, short *erp);
extern void Gsm_Short_Term_Synthesis_Filter(void *S, short *in, short *out);
extern unsigned int host_swap_game_data(void);

void host_fatal(const char *message)
{
    fprintf(stderr, "FATAL (game side): %s\n", message);
    exit(2);
}

#define MEM_SIZE 0x100000u
#define CODE_PHYS 0x48590u            /* console address 0x80048590 */
#define CODE_ROM 0x49190u
#define CODE_BYTES 0x600u
#define VIRT(p) (0x80000000u | (p))

/* Fixed layout of the test region (physical addresses): */
#define STATE 0x80000u                /* 0x18A bytes */
#define BUF_A 0x81000u                /* up to 0x400 bytes each */
#define BUF_B 0x81400u
#define BUF_C 0x81800u
#define REGION_START STATE
#define REGION_END 0x82000u
#define STACK_TOP 0xF0000u

static unsigned long long rng_state = 0x9E3779B97F4A7C15ull;
static unsigned int rnd(void)                       /* xorshift64*, deterministic */
{
    rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
    return (unsigned int)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}

static unsigned char *memA, *memB;
static MipsRef ref;
static unsigned int total_mismatches;

static void put16(unsigned char *mem, unsigned int phys, unsigned int v) { mem[phys] = (unsigned char)(v >> 8); mem[phys + 1] = (unsigned char)v; }
static void put32(unsigned char *mem, unsigned int phys, unsigned int v)
{
    mem[phys] = (unsigned char)(v >> 24); mem[phys + 1] = (unsigned char)(v >> 16); mem[phys + 2] = (unsigned char)(v >> 8); mem[phys + 3] = (unsigned char)v;
}

/* A random halfword with a mix of magnitudes, so that both small (typical) and extreme (wrap-around) values are exercised. */
static unsigned int rnd16(int mode)
{
    unsigned int r = rnd();
    if (mode == 0) { return r & 0xFFFF; }                                   /* anything */
    if (mode == 1) { return (unsigned int)((int)(r % 2001) - 1000) & 0xFFFF; }   /* small */
    return (unsigned int)((int)(r % 16001) - 8000) & 0xFFFF;                /* medium */
}

static void fill_region_random(void)
{
    unsigned int i;
    for (i = REGION_START; i < REGION_END; i++) {
        memA[i] = (unsigned char)(rnd() >> 7);
    }
}

/* Compares the region of both memories; reports the first difference. Returns 1 if equal. */
static int same_region(const char *what, unsigned int case_no)
{
    unsigned int i;
    for (i = REGION_START; i < REGION_END; i++) {
        if (memA[i] != memB[i]) {
            fprintf(stderr, "MISMATCH %s case %u at phys 0x%05X (offset 0x%X from STATE): original 0x%02X, C 0x%02X\n",
                    what, case_no, i, i - STATE, memA[i], memB[i]);
            return 0;
        }
    }
    return 1;
}

static void run_ref(const char *what, unsigned int entry, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3)
{
    mips_ref_init(&ref, memA, MEM_SIZE);
    ref.steps = 0;
    if (mips_ref_call(&ref, entry, a0, a1, a2, a3, VIRT(STACK_TOP), NULL)) {
        fprintf(stderr, "reference interpreter stopped in %s: %s\n", what, ref.error);
        exit(3);
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *rom;
    unsigned int n, i, cases, bad;

    if (argc != 2) { fprintf(stderr, "usage: %s <rom>\n", argv[0]); return 2; }
    host_swap_game_data();
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    rom = malloc(0x2000000);
    n = (unsigned int)fread(rom, 1, 0x2000000, f);
    fclose(f);
    if (n < CODE_ROM + CODE_BYTES) { fprintf(stderr, "file too small\n"); return 2; }

    memA = calloc(1, MEM_SIZE);
    memB = calloc(1, MEM_SIZE);
    memcpy(memA + CODE_PHYS, rom + CODE_ROM, CODE_BYTES);        /* the original code, where the game had it */
    memcpy(memB + CODE_PHYS, rom + CODE_ROM, CODE_BYTES);

    /* ---- Gsm_RPE_Decoding: every xmaxcr (0..63) and Mcr (0..3), 12 random inputs each, three input styles ---- */
    cases = bad = 0;
    for (n = 0; n < 64 * 4 * 12; n++) {
        unsigned int xmaxcr = (n / 48) & 63, Mcr = (n / 12) & 3, style = n % 3;
        fill_region_random();
        for (i = 0; i < 13; i++) {                                                /* xMcr at BUF_A: realistic 3-bit values or anything */
            put16(memA, BUF_A + 2 * i, style == 0 ? (rnd() & 7) : rnd16(style == 1 ? 0 : 1));
        }
        memcpy(memB + REGION_START, memA + REGION_START, REGION_END - REGION_START);
        run_ref("RPE", VIRT(0x48590), xmaxcr, Mcr, VIRT(BUF_A), VIRT(BUF_B));
        Gsm_RPE_Decoding((int)xmaxcr, (int)Mcr, (short *)(memB + BUF_A), (short *)(memB + BUF_B));
        cases++;
        if (!same_region("Gsm_RPE_Decoding", cases)) { bad++; }
    }
    printf("Gsm_RPE_Decoding %u %u\n", cases, bad);
    total_mismatches += bad;

    /* ---- Gsm_Long_term_synthesis_filtering: Ncr inside and outside 40..120, all four gains, history and residual random ---- */
    cases = bad = 0;
    for (n = 0; n < 4000; n++) {
        static const int special[] = { 0, 39, 40, 41, 80, 119, 120, 121, 200, 255, -1 };
        int Ncr = (n % 3 == 0) ? special[(n / 3) % 11] : (int)(rnd() % 100) + 30;
        int bcr = (int)(rnd() & 3), style = (int)(n % 3);
        fill_region_random();
        for (i = 0; i < 160; i++) { put16(memA, STATE + 2 * i, rnd16(style)); }            /* history */
        put16(memA, STATE + 0x166, rnd() % 121);                                          /* nrp: a lag the game could have stored */
        for (i = 0; i < 40; i++) { put16(memA, BUF_A + 2 * i, rnd16(style)); }              /* erp */
        memcpy(memB + REGION_START, memA + REGION_START, REGION_END - REGION_START);
        run_ref("long-term", VIRT(0x48684), VIRT(STATE), (unsigned int)Ncr, (unsigned int)bcr, VIRT(BUF_A));
        Gsm_Long_term_synthesis_filtering(memB + STATE, Ncr, bcr, (short *)(memB + BUF_A));
        cases++;
        if (!same_region("Gsm_Long_term_synthesis_filtering", cases)) { bad++; }
    }
    printf("Gsm_Long_term_synthesis_filtering %u %u\n", cases, bad);
    total_mismatches += bad;

    /* ---- Gsm_Short_Term_Synthesis_Filter: random filter state, coefficients of both frames, frame index 0/1/other, input of 3 styles ---- */
    cases = bad = 0;
    for (n = 0; n < 3000; n++) {
        int style = (int)(n % 3);
        fill_region_random();
        for (i = 0; i < 8; i++) {                                                          /* v[]: 32-bit values, small or anything */
            put32(memA, STATE + 0x140 + 4 * i, style == 0 ? rnd() : (unsigned int)((int)(rnd() % 40001) - 20000));
        }
        put16(memA, STATE + 0x164, rnd16(style));                                          /* msr */
        for (i = 0; i < 16; i++) { put16(memA, STATE + 0x168 + 2 * i, rnd16(style)); }     /* LARpp[2][8] */
        put16(memA, STATE + 0x188, (n % 7 == 0) ? (rnd() & 0xFFFF) : (n & 1));            /* frame_index */
        for (i = 0; i < 160; i++) { put16(memA, BUF_A + 2 * i, rnd16(style == 2 ? 1 : style)); }   /* input */
        memcpy(memB + REGION_START, memA + REGION_START, REGION_END - REGION_START);
        run_ref("short-term", VIRT(0x48904), VIRT(STATE), VIRT(BUF_A), VIRT(BUF_B), 0);
        Gsm_Short_Term_Synthesis_Filter(memB + STATE, (short *)(memB + BUF_A), (short *)(memB + BUF_B));
        cases++;
        if (!same_region("Gsm_Short_Term_Synthesis_Filter", cases)) { bad++; }
    }
    printf("Gsm_Short_Term_Synthesis_Filter %u %u\n", cases, bad);
    total_mismatches += bad;

    return total_mismatches ? 1 : 0;
}
