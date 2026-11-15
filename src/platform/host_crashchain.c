/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/* host_crashchain.c - see host_crashchain.h. Tested by tests/native/crash/test_crashchain.c. */
#include <stdint.h>
#include <stdio.h>

#include "host_crashchain.h"

int host_format_frame_chain(char *out, unsigned size, unsigned long ebp, unsigned long lo, unsigned long hi, int max)
{
    unsigned n = 0;
    int frames = 0;
    if (size == 0) return 0;
    out[0] = 0;
    while (frames < max) {
        const uintptr_t *frame;
        unsigned long next, ret;
        int w;
        /* a frame is two words: [ebp] = caller's ebp, [ebp + one word] = return address; both must lie inside the stack */
        if (ebp % sizeof(uintptr_t) != 0 || ebp < lo || ebp + 2 * sizeof(uintptr_t) > hi) break;
        frame = (const uintptr_t *)ebp;
        next = (unsigned long)frame[0];
        ret = (unsigned long)frame[1];
        w = snprintf(out + n, size - n, "  caller 0x%08lX\n", ret);
        if (w < 0 || (unsigned)w >= size - n) break;
        n += (unsigned)w;
        frames++;
        if (next <= ebp) break;                  /* the chain must climb */
        ebp = next;
    }
    return frames;
}
