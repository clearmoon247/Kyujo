/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * test_crashchain.c - checks host_format_frame_chain (src/platform/host_crashchain.c) on a synthetic stack.
 *   cc -I src/platform -o test_crashchain tests/native/crash/test_crashchain.c src/platform/host_crashchain.c && ./test_crashchain
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "host_crashchain.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* a stack of 16 words; frames at word 2 -> word 6 -> word 10; the last link points up and outside */
    static uintptr_t stack[16];
    unsigned long lo = (unsigned long)(uintptr_t)stack, hi = lo + sizeof stack;
    char out[400];
    int n;
    memset(stack, 0, sizeof stack);
    stack[2] = (uintptr_t)&stack[6];  stack[3] = 0x401000;
    stack[6] = (uintptr_t)&stack[10]; stack[7] = 0x402000;
    stack[10] = (uintptr_t)(lo + 100000); stack[11] = 0x403000;
    n = host_format_frame_chain(out, sizeof out, (unsigned long)(uintptr_t)&stack[2], lo, hi, 20);
    CHECK(n == 3, "three frames found (%d)", n);
    CHECK(strstr(out, "0x00401000") && strstr(out, "0x00402000") && strstr(out, "0x00403000"), "the three return addresses are written");

    n = host_format_frame_chain(out, sizeof out, (unsigned long)(uintptr_t)&stack[2], lo, hi, 2);
    CHECK(n == 2 && !strstr(out, "0x00403000"), "max stops the walk");

    stack[6] = (uintptr_t)&stack[2];                      /* a link that goes back: stop, no loop */
    n = host_format_frame_chain(out, sizeof out, (unsigned long)(uintptr_t)&stack[2], lo, hi, 20);
    CHECK(n == 2, "a link that does not climb ends the walk (%d)", n);

    n = host_format_frame_chain(out, sizeof out, lo + 1, lo, hi, 20);
    CHECK(n == 0, "a misaligned ebp reads nothing");
    n = host_format_frame_chain(out, sizeof out, hi - sizeof(uintptr_t), lo, hi, 20);
    CHECK(n == 0, "a frame that would end outside the stack reads nothing");
    n = host_format_frame_chain(out, sizeof out, 0, lo, hi, 20);
    CHECK(n == 0, "ebp 0 reads nothing");
    n = host_format_frame_chain(out, 20, (unsigned long)(uintptr_t)&stack[2], lo, hi, 20);
    CHECK(n <= 1 && strlen(out) < 20, "a small buffer is respected (%d)", n);

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
