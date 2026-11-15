/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * diff_vs_cxd4.c - differential test of the vector unit of src/rsp/rsp_cpu.c against an independent implementation.
 *
 * WHY: the interpreter's multiply/accumulate and clamp rules were written from memory of the published RSP descriptions (see the
 * comment at the top of rsp_cpu.c). An instruction that is subtly wrong would still produce plausible-looking audio, so each vector
 * instruction the ROM's audio microcode uses is compared lane by lane against the vector unit of "mupen64plus-rsp-cxd4"
 * (https://github.com/mupen64plus/mupen64plus-rsp-cxd4, files vu/*.c; public domain, CC0 - see its COPYING). That source is NOT part of
 * this repository: tests/native/rsp/run_diff.sh clones it into a scratch directory, builds this file against it and runs it.
 * The two implementations share no code; where they disagree, one of them is wrong and a person has to decide which (the cxd4 code
 * is itself an emulator, so agreement is strong evidence, not proof about the console).
 *
 * WHAT: for each tested instruction, many random trials of: random source registers (with edge values 0, +-1, 0x7FFF, 0x8000 mixed in),
 * random accumulator, random VCO/VCC/VCE, random element field 0..15; execute the one instruction on both; compare the destination
 * register, all 8 accumulators and the three flag registers.
 *
 * The cxd4 element broadcast (su.c, the non-SSE2 branch, `shuffle_temporary`) is reproduced here by hand in `cxd4_vt_lane`: it is part of
 * what is compared (our vt_lane() against theirs).
 *
 * Build/run: see run_diff.sh. Exit status 0 = no difference found.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the interpreter under test, included so that its state `R` is reachable */
#include "../../../src/rsp/rsp_cpu.c"

/* ---- what the interpreter needs from the host (platform.h) ---- */
void host_fatal(const char *msg) { fprintf(stderr, "FATAL: %s\n", msg); exit(3); }
void host_log(const char *msg) { fprintf(stderr, "%s\n", msg); }
int host_game_range_ok(unsigned int a, unsigned int l) { (void)a; (void)l; return 0; }   /* these tests do no DMA */

/* ---- the reference (cxd4 vu/*.c, built without SSE2) ---- */
#include "vu/vu.h"
u32 inst_word;                     /* declared extern by the reference; its su.c (not built here) defines it */
extern i16 cf_ne[N], cf_co[N], cf_clip[N], cf_comp[N], cf_vce[N];
void message(const char *body) { fprintf(stderr, "cxd4 message: %s\n", body); }

static uint32_t sRng = 12345;
static uint32_t rnd(void) { sRng = sRng * 1664525u + 1013904223u; return sRng >> 8; }
static uint16_t rnd16(void)
{
    static const uint16_t edge[] = {0, 1, 0xFFFF, 0x7FFF, 0x8000, 0x8001, 0x00FF, 0xFF00, 0x4000, 0xC000};
    uint32_t r = rnd();
    if ((r & 7) == 0) return edge[(r >> 4) % (sizeof edge / sizeof edge[0])];
    return (uint16_t)(rnd());
}

/* which lane of VT lane i uses, as cxd4's su.c does it (non-SSE2 branch): copied by hand, with the case split of its switch */
static int cxd4_vt_lane(int e, int i)
{
    if (e < 2) return i;
    if (e < 4) return (i & 0xE) + (e & 1);
    if (e < 8) return (i & 0xC) + (e & 3);
    return e % N;
}

static const int kOps[] = {0, 4, 5, 6, 7, 8, 12, 13, 14, 15, 16, 17, 20, 29, 32, 33, 34, 35, 39, 40, 42, 44};
static const char *kNames[] = {"VMULF", "VMUDL", "VMUDM", "VMUDN", "VMUDH", "VMACF", "VMADL", "VMADM", "VMADN", "VMADH", "VADD",
                               "VSUB", "VADDC", "VSAR", "VLT", "VEQ", "VNE", "VGE", "VMRG", "VAND", "VOR", "VXOR"};

int main(int argc, char **argv)
{
    int trials = argc > 1 ? atoi(argv[1]) : 20000;
    int k, t, i, fails_total = 0;
    static unsigned char imem[4096], dmem[4096];

    for (k = 0; k < (int)(sizeof kOps / sizeof kOps[0]); k++) {
        int fails = 0;
        for (t = 0; t < trials; t++) {
            int funct = kOps[k], vd = rnd() % 32, vs = rnd() % 32, vt = rnd() % 32, e = rnd() % 16;
            uint32_t word;
            i16 tmp[N];
            uint16_t vco = (uint16_t)rnd(), vcc = (uint16_t)rnd(), vce = (uint16_t)(rnd() & 0xFF);
            int64_t acc[8];
            int bad = 0;

            if (funct == 29) e = 8 + (rnd() % 3);              /* VSAR: element 8,9,10 = high, middle, low part (others are not tested) */
            word = (0x12u << 26) | (1u << 25) | ((uint32_t)e << 21) | ((uint32_t)vt << 16) | ((uint32_t)vs << 11) | ((uint32_t)vd << 6) | (uint32_t)funct;

            /* the same random state into both */
            for (i = 0; i < 32; i++) {
                int l;
                for (l = 0; l < 8; l++) { uint16_t x = rnd16(); set_lane(i, l, x); VR[i][l] = (i16)x; }
            }
            for (i = 0; i < 8; i++) {
                uint16_t h = rnd16(), m = rnd16(), l = rnd16();
                VACC[0][i] = (i16)h; VACC[1][i] = (i16)m; VACC[2][i] = (i16)l;
                acc[i] = wrap48(((int64_t)(uint16_t)h << 32) | ((int64_t)m << 16) | l);
                R.acc[i] = acc[i];
                cf_co[i] = (vco >> i) & 1; cf_ne[i] = (vco >> (8 + i)) & 1;
                cf_comp[i] = (vcc >> i) & 1; cf_clip[i] = (vcc >> (8 + i)) & 1;
                cf_vce[i] = (vce >> i) & 1;
            }
            R.vco = vco; R.vcc = vcc; R.vce = vce;

            /* reference: broadcast VT as cxd4 does, run the operation, copy the result to VD */
            for (i = 0; i < N; i++) tmp[i] = VR[vt][cxd4_vt_lane(e, i)];
            inst_word = word;
            COP2_C2[funct](&VR[vs][0], &tmp[0]);
            for (i = 0; i < N; i++) VR[vd][i] = V_result[i];
            if (funct == 12 || funct == 14) {
                /* VMADL / VMADN result: cxd4's SIGNED_CLAMP_AL decides "clamped?" by comparing the middle accumulator word with its clamped
                 * value, which misses the case that the middle word happens to equal the clamp value (0x7FFF / 0x8000) while the high word
                 * says the accumulator overflowed. ares (ares/n64/rsp/interpreter-vpu.cpp, accumulatorSaturate(n, 0, 0x0000, 0xFFFF), the
                 * rule used for VMADL and VMADN) tests the high word too. Both agree everywhere else; the ares rule is used for the result
                 * here (the accumulator itself is still compared with cxd4's). */
                for (i = 0; i < N; i++) {
                    int16_t h = VACC[0][i], m = VACC[1][i];
                    uint16_t r;
                    if (h < 0) r = (h != -1 || m >= 0) ? 0x0000 : (uint16_t)VACC[2][i];
                    else       r = (h != 0 || m < 0) ? 0xFFFF : (uint16_t)VACC[2][i];
                    VR[vd][i] = (i16)r;
                }
            }

            /* interpreter: the instruction, then BREAK */
            memset(imem, 0, sizeof imem);
            imem[0] = word >> 24; imem[1] = word >> 16; imem[2] = word >> 8; imem[3] = word;
            imem[4] = 0; imem[5] = 0; imem[6] = 0; imem[7] = 13;           /* BREAK */
            host_rsp_start(dmem, imem, 0, 0, 0, 0, 1, 0);
            /* host_rsp_start leaves the vector state alone; re-apply what it may not keep */
            R.vco = vco; R.vcc = vcc; R.vce = vce;
            for (i = 0; i < 8; i++) R.acc[i] = acc[i];
            host_rsp_run();

            /* compare */
            for (i = 0; i < 8 && !bad; i++) {
                int64_t ref = wrap48((((int64_t)(uint16_t)VACC[0][i]) << 32) | ((int64_t)(uint16_t)VACC[1][i] << 16) | (uint16_t)VACC[2][i]);
                if (lane_u(vd, i) != (uint16_t)VR[vd][i]) { bad = 1; printf("  %s lane %d: result %04X, reference %04X\n", kNames[k], i, lane_u(vd, i), (uint16_t)VR[vd][i]); }
                else if (R.acc[i] != ref) { bad = 1; printf("  %s lane %d: acc %012llX, reference %012llX\n", kNames[k], i, (unsigned long long)(R.acc[i] & 0xFFFFFFFFFFFFLL), (unsigned long long)(ref & 0xFFFFFFFFFFFFLL)); }
            }
            if (!bad) {
                uint16_t rvco = 0, rvcc = 0, rvce = 0;
                for (i = 0; i < 8; i++) {
                    rvco |= (cf_co[i] ? 1 : 0) << i; rvco |= (cf_ne[i] ? 1 : 0) << (8 + i);
                    rvcc |= (cf_comp[i] ? 1 : 0) << i; rvcc |= (cf_clip[i] ? 1 : 0) << (8 + i);
                    rvce |= (cf_vce[i] ? 1 : 0) << i;
                }
                if (R.vco != rvco || R.vcc != rvcc || R.vce != rvce) {
                    bad = 1;
                    printf("  %s flags: vco %04X/%04X vcc %04X/%04X vce %02X/%02X (interpreter/reference)\n", kNames[k], R.vco, rvco, R.vcc, rvcc, R.vce, rvce);
                }
            }
            if (bad) {
                printf("    word %08X e=%d vd=%d vs=%d vt=%d\n", word, e, vd, vs, vt);
                if (++fails >= 3) break;
            }
        }
        printf("%-6s %s (%d trials%s)\n", kNames[k], fails ? "DIFFERS" : "ok", trials, fails ? ", first differences above" : "");
        fails_total += fails ? 1 : 0;
    }
    printf("%d of %d instructions differ from the reference\n", fails_total, (int)(sizeof kOps / sizeof kOps[0]));
    return fails_total ? 1 : 0;
}
