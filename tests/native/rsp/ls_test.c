/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * ls_test.c - hand-computed tests of the vector loads and stores that the ROM's audio microcode uses (found by tracing it:
 * LDV e0/e8, LQV e0, LRV e0, LSV e14, SDV e0, SQV e0, SSV e0..e14). Expected bytes are worked out from the instruction definitions
 * (a quad load takes bytes up to the next 16-byte boundary, the reverse load the bytes before the address, ...) and agree with
 * the way cxd4's su.c (LQV, LRV, SSV, SQV) is written; they are not a measurement on a console.
 * Build: cc -I src/platform -o ls_test tests/native/rsp/ls_test.c && ./ls_test      Exit status 0 = all passed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/rsp/rsp_cpu.c"

void host_fatal(const char *msg) { fprintf(stderr, "FATAL: %s\n", msg); exit(3); }
void host_log(const char *msg) { fprintf(stderr, "%s\n", msg); }
int host_game_range_ok(unsigned int a, unsigned int l) { (void)a; (void)l; return 0; }   /* these tests do no DMA */

static unsigned char dmem[4096], imem[4096];
static int failures;

/* runs one LWC2 (store = 0) or SWC2 (store = 1) instruction: op = the instruction's own operation number, e = element, off = signed 7-bit offset (in units of the access size), with $1 = base */
static void run(int store, int op, int vt, int e, int off, uint32_t base)
{
    uint32_t w = ((store ? 58u : 50u) << 26) | (1u << 21) | ((uint32_t)vt << 16) | ((uint32_t)op << 11) | ((uint32_t)e << 7) | ((uint32_t)off & 0x7F);
    memset(imem, 0, sizeof imem);
    imem[0] = w >> 24; imem[1] = w >> 16; imem[2] = w >> 8; imem[3] = w; imem[7] = 13;      /* BREAK */
    host_rsp_start(dmem, imem, 0, 0, 0, 0, 1, 0);
    R.gpr[1] = base;
    host_rsp_run();
}

static void check(const char *what, const unsigned char *got, const unsigned char *want, int n)
{
    if (memcmp(got, want, (size_t)n) != 0) {
        int i;
        failures++;
        printf("FAIL %s\n  got  ", what);
        for (i = 0; i < n; i++) printf("%02X ", got[i]);
        printf("\n  want ");
        for (i = 0; i < n; i++) printf("%02X ", want[i]);
        printf("\n");
    } else {
        printf("ok   %s\n", what);
    }
}

static void fill_dmem(void) { int i; for (i = 0; i < 4096; i++) dmem[i] = (unsigned char)(i * 7 + 3); }   /* a pattern that is not periodic over 16 */
static void fill_reg(int v, unsigned char start) { int i; for (i = 0; i < 16; i++) R.vpr[v][i] = (unsigned char)(start + i); }

int main(void)
{
    unsigned char want[16];
    int i;

    /* LQV e0 at an address 5 bytes into a quad: loads dmem[0x105..0x10F] (11 bytes) into register bytes 0..10, the rest unchanged */
    fill_dmem();
    memset(R.vpr, 0xEE, sizeof R.vpr);
    run(0, 4, 2, 0, 0, 0x105);
    for (i = 0; i < 16; i++) want[i] = i < 11 ? dmem[0x105 + i] : 0xEE;
    check("LQV e0 unaligned (to the 16-byte boundary)", R.vpr[2], want, 16);

    /* LQV e0, aligned: all 16 bytes */
    run(0, 4, 2, 0, 0, 0x110);
    for (i = 0; i < 16; i++) want[i] = dmem[0x110 + i];
    check("LQV e0 aligned", R.vpr[2], want, 16);

    /* LRV e0 at 0x105: the 5 bytes before the address (dmem[0x100..0x104]) go to register bytes 11..15 (right-aligned) */
    memset(R.vpr, 0xEE, sizeof R.vpr);
    run(0, 5, 3, 0, 0, 0x105);
    for (i = 0; i < 16; i++) want[i] = i >= 11 ? dmem[0x100 + (i - 11)] : 0xEE;
    check("LRV e0 (right-aligned remainder)", R.vpr[3], want, 16);

    /* LQV + LRV together at the same unaligned address give the 16 bytes starting there, rotated by the misalignment:
     * register = dmem[0x105..0x10F] then dmem[0x100..0x104]. */
    memset(R.vpr, 0, sizeof R.vpr);
    run(0, 4, 4, 0, 0, 0x105);          /* run() keeps the register file, so the LRV below completes the same register */
    run(0, 5, 4, 0, 0, 0x105);
    for (i = 0; i < 16; i++) want[i] = i < 11 ? dmem[0x105 + i] : dmem[0x100 + (i - 11)];
    check("LQV then LRV into one register (unaligned quad)", R.vpr[4], want, 16);

    /* LDV e0: 8 bytes into register bytes 0..7;  LDV e8: into bytes 8..15 */
    memset(R.vpr, 0xEE, sizeof R.vpr);
    run(0, 3, 6, 0, 0, 0x200);
    for (i = 0; i < 16; i++) want[i] = i < 8 ? dmem[0x200 + i] : 0xEE;
    check("LDV e0", R.vpr[6], want, 16);
    memset(R.vpr, 0xEE, sizeof R.vpr);
    run(0, 3, 6, 8, 0, 0x208);
    for (i = 0; i < 16; i++) want[i] = i >= 8 ? dmem[0x208 + (i - 8)] : 0xEE;
    check("LDV e8", R.vpr[6], want, 16);

    /* LSV e14: 2 bytes into register bytes 14, 15 */
    memset(R.vpr, 0xEE, sizeof R.vpr);
    run(0, 1, 7, 14, 0, 0x300);
    for (i = 0; i < 16; i++) want[i] = i >= 14 ? dmem[0x300 + (i - 14)] : 0xEE;
    check("LSV e14", R.vpr[7], want, 16);

    /* the offset field is scaled by the access size: LDV with offset 3 reads at base + 24, LQV with offset -1 at base - 16 */
    memset(R.vpr, 0xEE, sizeof R.vpr);
    run(0, 3, 8, 0, 3, 0x400);
    for (i = 0; i < 8; i++) want[i] = dmem[0x400 + 24 + i];
    check("LDV offset 3 (scaled by 8)", R.vpr[8], want, 8);
    run(0, 4, 8, 0, -1, 0x410);
    for (i = 0; i < 16; i++) want[i] = dmem[0x400 + i];
    check("LQV offset -1 (scaled by 16)", R.vpr[8], want, 16);

    /* SQV e0 at 0x505: stores register bytes 0..10 to dmem[0x505..0x50F] (to the quad boundary) */
    fill_dmem(); fill_reg(9, 0x40);
    { unsigned char before[4096]; memcpy(before, dmem, sizeof before);
      run(1, 4, 9, 0, 0, 0x505);
      for (i = 0; i < 16; i++) want[i] = (i >= 5) ? (unsigned char)(0x40 + (i - 5)) : before[0x500 + i];
      check("SQV e0 unaligned (to the boundary)", dmem + 0x500, want, 16);
      check("SQV e0 leaves the next quad alone", dmem + 0x510, before + 0x510, 16); }

    /* SDV e0: register bytes 0..7 to 8 bytes */
    fill_dmem(); fill_reg(9, 0x40);
    { unsigned char before[4096]; memcpy(before, dmem, sizeof before);
      run(1, 3, 9, 0, 0, 0x600);
      for (i = 0; i < 16; i++) want[i] = (i < 8) ? (unsigned char)(0x40 + i) : before[0x600 + i];
      check("SDV e0", dmem + 0x600, want, 16); }

    /* SSV, every element the microcode uses (0..14 even, and 7): register bytes e, e+1 to 2 bytes */
    {   int es[] = {0, 2, 4, 6, 7, 8, 10, 12, 14}, k;
        for (k = 0; k < 9; k++) {
            char name[40];
            unsigned char before[16];
            fill_dmem(); fill_reg(9, 0x40);
            memcpy(before, dmem + 0x700, 16);
            run(1, 1, 9, es[k], 0, 0x700);
            memcpy(want, before, 16);
            want[0] = (unsigned char)(0x40 + es[k]); want[1] = (unsigned char)(0x40 + es[k] + 1);
            snprintf(name, sizeof name, "SSV e%d", es[k]);
            check(name, dmem + 0x700, want, 16);
        }
    }

    /* LPV e0 (used by the Game Boy microcodes): 8 bytes into the HIGH byte of lanes 0..7, low bytes zero; offset scaled by 8 */
    fill_dmem();
    memset(R.vpr, 0xEE, sizeof R.vpr);
    run(0, 6, 10, 0, 0, 0x800);
    for (i = 0; i < 8; i++) { want[2 * i] = dmem[0x800 + i]; want[2 * i + 1] = 0; }
    check("LPV e0", R.vpr[10], want, 16);
    run(0, 6, 10, 0, 2, 0x800);
    for (i = 0; i < 8; i++) { want[2 * i] = dmem[0x800 + 16 + i]; want[2 * i + 1] = 0; }
    check("LPV e0 offset 2 (scaled by 8)", R.vpr[10], want, 16);

    /* SPV e0: the HIGH byte of lanes 0..7 to 8 bytes (the low bytes are not stored); offset -1 = base - 8 */
    fill_dmem(); fill_reg(11, 0x40);
    { unsigned char before[4096]; memcpy(before, dmem, sizeof before);
      run(1, 6, 11, 0, -1, 0x908);
      for (i = 0; i < 16; i++) want[i] = (i < 8) ? (unsigned char)(0x40 + 2 * i) : before[0x900 + i];
      check("SPV e0 offset -1", dmem + 0x900, want, 16); }

    /* LPV / SPV at addresses that are not multiples of 8: the eight bytes are read / written one after the other from the unaligned address
     * (cxd4 su.c, LPV and SPV, cases 01..07, each case is "the next 8 bytes, continuing into the following group"), wrapping at 0xFFF. */
    fill_dmem();
    memset(R.vpr, 0xEE, sizeof R.vpr);
    run(0, 6, 10, 0, 0, 0x803);
    for (i = 0; i < 8; i++) { want[2 * i] = dmem[0x803 + i]; want[2 * i + 1] = 0; }
    check("LPV e0 at an address = 3 (mod 8)", R.vpr[10], want, 16);
    run(0, 6, 10, 0, 0, 0xFFD);
    for (i = 0; i < 8; i++) { want[2 * i] = dmem[(0xFFD + i) & 0xFFF]; want[2 * i + 1] = 0; }
    check("LPV e0 wrapping at the end of DMEM", R.vpr[10], want, 16);
    fill_dmem(); fill_reg(11, 0x40);
    { unsigned char before[4096]; memcpy(before, dmem, sizeof before);
      run(1, 6, 11, 0, 0, 0x905);
      for (i = 0; i < 16; i++) want[i] = (i >= 5 && i < 13) ? (unsigned char)(0x40 + 2 * (i - 5)) : before[0x900 + i];
      check("SPV e0 at an address = 5 (mod 8)", dmem + 0x900, want, 16); }

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
