/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * test_blend_div.c - proves the reciprocal-multiply division used by the software RDP's blender (rdp.c, blend_div) gives exactly n / d for
 * EVERY n and d the blender can produce: d = 1..510 (a + b with a, b in 0..255, not both 0) and n = 0..255 * 510 + 255 (the largest numerator,
 * with both colours 255). About 66 million pairs; takes a second or two.
 *
 * build and run (host compiler, any platform):   cc -O2 -o test_blend_div test_blend_div.c && ./test_blend_div
 * The two functions below are COPIES of the ones in rdp.c (keep them identical; the test says so in its output so that a reviewer compares).
 */
#include <stdint.h>
#include <stdio.h>

static uint32_t sDivRecip[511];
static void div_tables(void)
{
    uint32_t d;
    for (d = 2; d <= 510; d++) sDivRecip[d] = (uint32_t)(((1ULL << 32) + d - 1) / d);
}
static int blend_div(int n, int d)
{
    if (d == 1) return n;
    return (int)(((uint64_t)(uint32_t)n * sDivRecip[d]) >> 32);
}

int main(void)
{
    long long checked = 0, bad = 0;
    int d, n;
    div_tables();
    for (d = 1; d <= 510; d++) {
        for (n = 0; n <= 255 * 510 + 255; n++) {
            checked++;
            if (blend_div(n, d) != n / d) {
                if (bad++ < 5) printf("MISMATCH n=%d d=%d: got %d, expected %d\n", n, d, blend_div(n, d), n / d);
            }
        }
    }
    printf("test_blend_div: %lld pairs checked, %lld mismatches\n", checked, bad);
    return bad ? 1 : 0;
}
