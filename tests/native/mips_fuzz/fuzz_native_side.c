// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Dan B. Cooper
/* differential fuzz of shell/core/mips_native.c (tests/native/mips_fuzz_native.c) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define MIPS_NATIVE_TEST 1
unsigned char gPool[1 << 20] __attribute__((section(".pool"), aligned(4096)));
unsigned char gData[1024] __attribute__((section(".dat"), aligned(64)));
static unsigned char sTestStack[32768] __attribute__((section(".stk"), aligned(16)));
static volatile unsigned int sFlag;
unsigned long long host_now_ns(void) { return 0; }
void host_fatal(const char *m) { fprintf(stderr, "FATAL: %s\n", m); exit(3); }
static unsigned int hw_read_stub(unsigned int a) { (void)a; return 0; }
static void hw_write_stub(unsigned int a, unsigned int d) { (void)a; (void)d; }
static unsigned long long native_call_stub(unsigned int t, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3) { (void)t; (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
static void preempt_stub(void) {}
#define MIPS_HW_READ(a) hw_read_stub(a)
#define MIPS_HW_WRITE(a, v) hw_write_stub(a, v)
#define MIPS_NATIVE_CALL(t, a0, a1, a2, a3) native_call_stub(t, a0, a1, a2, a3)
#define MIPS_PREEMPT() preempt_stub()
#include "mips_native.c"

static void host_mips_flush(void) { sGen++; memset(sNoNative, 0, sizeof sNoNative); }
static unsigned int host_mips_call(unsigned int entry, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3)
{
    static MipsShared sh;
    Cpu c;
    gM = 0;
    sh.pool_base = (unsigned)(unsigned long)gPool; sh.pool_bytes = sizeof gPool; sh.preempt_flag = (unsigned)(unsigned long)&sFlag;
    sPoolBase = sh.pool_base; sPoolBytes = sh.pool_bytes;
    memset(&c, 0, sizeof c);
    c.sh = &sh;
    c.r[4] = sext32(a0); c.r[5] = sext32(a1); c.r[6] = sext32(a2); c.r[7] = sext32(a3);
    c.r[29] = sext32((unsigned)(unsigned long)(sTestStack + sizeof sTestStack - 16));
    c.r[31] = sext32(RETURN_SENTINEL);
    return run_blocks(&c, entry);
}


static uint64_t rs_state;
static uint32_t rnd(void) { rs_state = rs_state * 6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(rs_state >> 33); }
static unsigned ru(unsigned n) { return rnd() % n; }

static uint32_t prog[4096]; static unsigned np;
static unsigned dreg(void) { static const unsigned pool[] = {1,2,3,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,0}; return pool[ru(sizeof pool / sizeof pool[0])]; }
static unsigned sreg(void) { static const unsigned pool[] = {0,1,2,3,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,4}; return pool[ru(sizeof pool / sizeof pool[0])]; }
static uint32_t R(unsigned op, unsigned rs, unsigned rt, unsigned rd, unsigned sh, unsigned fn) { return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | fn; }
static uint32_t I(unsigned op, unsigned rs, unsigned rt, unsigned imm) { return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF); }

static uint32_t plain(void)
{
    static const unsigned fn3[] = {0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B};
    switch (ru(12)) {
    case 0: case 1: return R(0, sreg(), sreg(), dreg(), 0, fn3[ru(8)]);
    case 2: { static const unsigned f[] = {0x00, 0x02, 0x03}; return R(0, 0, sreg(), dreg(), ru(32), f[ru(3)]); }
    case 3: { static const unsigned f[] = {0x04, 0x06, 0x07}; return R(0, sreg(), sreg(), dreg(), 0, f[ru(3)]); }
    case 4: case 5: { static const unsigned o[] = {0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E}; return I(o[ru(6)], sreg(), dreg(), rnd()); }
    case 6: return I(0x0F, 0, dreg(), rnd());
    case 7: { static const unsigned o[] = {0x20, 0x24, 0x21, 0x25, 0x23, 0x27, 0x37}; unsigned k = ru(7); unsigned sz = k < 2 ? 1 : k < 4 ? 2 : k < 6 ? 4 : 8; return I(o[k], 4, dreg(), (ru(128) & ~(sz - 1)) | 0); }
    case 8: { static const unsigned o[] = {0x28, 0x29, 0x2B, 0x3F}; unsigned k = ru(4); unsigned sz = 1u << k; return I(o[k], 4, sreg(), (ru(128) & ~(sz - 1)) + 256); }
    case 9: { static const unsigned f[] = {0x10, 0x12}; return R(0, 0, 0, dreg(), 0, f[ru(2)]); }
    case 10: { unsigned k = ru(4); static const unsigned f[] = {0x11, 0x13, 0x18, 0x19}; return k < 2 ? R(0, sreg(), 0, 0, 0, f[k]) : R(0, sreg(), sreg(), 0, 0, f[k]); }
    default: return 0; /* nop */
    }
}

/* program: leaf at word 0..; main at LEAF_N */
static unsigned build(void)
{
    unsigned i, leaf_start, main_start, n;
    np = 0;
    /* leaf routine at the start */
    leaf_start = np;
    n = 1 + ru(5);
    for (i = 0; i < n; i++) prog[np++] = plain();
    prog[np++] = R(0, 31, 0, 0, 0, 0x08);           /* jr ra */
    prog[np++] = plain();                            /* delay slot */
    main_start = np;
    prog[np++] = R(0, 31, 0, 30, 0, 0x25);           /* or r30, ra, zero */
    n = 40 + ru(120);
    while (np < main_start + n) {
        unsigned k = ru(14);
        if (k < 7) prog[np++] = plain();
        else if (k < 11) {            /* forward conditional branch skipping 0..3 words after the delay slot */
            static const unsigned ops[] = {4, 5, 6, 7, 1, 1};
            unsigned o = ops[ru(6)], skip = ru(4);
            unsigned off = skip + 1;           /* words from the delay slot to the target */
            uint32_t b;
            if (o == 1) b = I(1, sreg(), ru(2), off);
            else if (o == 4 || o == 5) b = I(o, sreg(), sreg(), off);
            else b = I(o, sreg(), 0, off);
            if (o == 1 && ru(4) == 0) b = I(1, sreg(), ru(2) ? 0x11 : 0x10, off);   /* bgezal / bltzal */
            prog[np++] = b;
            prog[np++] = plain();
            for (i = 0; i < skip; i++) prog[np++] = plain();
        } else if (k == 11) {          /* jal leaf (+ delay slot) */
            prog[np++] = (3u << 26) | ((((unsigned)(unsigned long)gPool) + leaf_start * 4) >> 2);
            prog[np++] = plain();
        } else if (k == 12) {          /* jalr via a register holding the leaf address */
            unsigned rr = 24;
            prog[np++] = I(0x0F, 0, rr, (((unsigned)(unsigned long)gPool) >> 16));                              /* lui */
            prog[np++] = I(0x0D, rr, rr, ((((unsigned)(unsigned long)gPool) + leaf_start * 4)) & 0xFFFF);        /* ori */
            prog[np++] = R(0, rr, 0, 31, 0, 0x09);                          /* jalr ra, rr */
            prog[np++] = plain();
        } else {                       /* a small counted loop */
            unsigned body = 1 + ru(4), j;
            prog[np++] = I(0x0D, 0, 26, 2 + ru(5));                         /* ori r26, zero, count */
            for (j = 0; j < body; j++) { uint32_t p = plain(); while (((p >> 11) & 31) == 26 && (p >> 26) == 0 && (p & 63) != 0x08) p = plain(); prog[np++] = p; }
            prog[np++] = I(9, 26, 26, 0xFFFF);                              /* addiu r26, r26, -1 */
            prog[np++] = I(7, 26, 0, (unsigned)(-(int)(body + 2)) & 0xFFFF);/* bgtz r26, back to the first body word (offset from delay slot) */
            prog[np++] = plain();
        }
    }
    /* make r26 a never-written destination elsewhere: dreg() never returns it; r30 neither. Store registers and hi/lo */
    for (i = 1; i < 26; i++) if (i != 4) prog[np++] = I(0x2B, 4, i, 512 + i * 4);
    prog[np++] = R(0, 0, 0, 8, 0, 0x10); prog[np++] = I(0x2B, 4, 8, 640);
    prog[np++] = R(0, 0, 0, 8, 0, 0x12); prog[np++] = I(0x2B, 4, 8, 644);
    prog[np++] = R(0, 30, 0, 0, 0, 0x08);            /* jr r30 */
    prog[np++] = 0;
    return main_start;
}

int main(int argc, char **argv)
{
    unsigned seeds = argc > 1 ? (unsigned)atoi(argv[1]) : 1000, s, i;
    for (s = 0; s < seeds; s++) {
        unsigned main_start;
        uint64_t h = 1469598103934665603ULL;
        rs_state = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)s * 0x100000001B3ULL);
        main_start = build();
        memset(gPool, 0, 4096 * 4);
        for (i = 0; i < np; i++) { gPool[i * 4] = prog[i] >> 24; gPool[i * 4 + 1] = prog[i] >> 16; gPool[i * 4 + 2] = prog[i] >> 8; gPool[i * 4 + 3] = prog[i]; }
        for (i = 0; i < 256; i++) gData[i] = (unsigned char)rnd();
        memset(gData + 256, 0, 768);
        host_mips_flush();
        {
            unsigned int v = host_mips_call((unsigned int)(unsigned long)gPool + main_start * 4, (unsigned int)(unsigned long)gData, 0x12345678u, 0xFFFFFFF0u, 7);
            for (i = 0; i < 1024; i++) { h ^= gData[i]; h *= 1099511628211ULL; }
            h ^= v; h *= 1099511628211ULL;
        }
        printf("%u %016llx\n", s, (unsigned long long)h);
    }
    return 0;
}
