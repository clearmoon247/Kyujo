/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * be_test_host.c - HOST-side driver of the big-endian rewrite test (see tests/native/be/be_cases.c and tools/native/test_be_memory.py).
 * Runs the game-side checks and, with --dump, prints the bytes of the initialised test data so the Python driver can compare them
 * with a big-endian MIPS compile of the same source.
 */
#include <stdio.h>
#include <string.h>

#include "platform.h"

extern int be_run_tests(void);
extern void be_dump(void);

static int g_checks, g_failures;

/* Called by the game-side tests. `name` points to game data: plain bytes, so it is an ordinary C string. */
void be_check(const char *name, int ok)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("FAIL %s\n", name);
    }
}

/* The hook the rewrite puts in front of every indirect call (in the real program it is src/game_asm/fragment_native.c). */
static const void *sRedirectFrom, *sRedirectTo;
static unsigned int sHookCalls;

void be_set_redirect(const void *from, const void *to)
{
    sRedirectFrom = from;
    sRedirectTo = to;
}

unsigned int be_hook_calls(void)
{
    return sHookCalls;
}

void *HostResolveCallTarget(void *target)
{
    sHookCalls++;
    return (target == sRedirectFrom && target != NULL) ? (void *)sRedirectTo : target;
}

/* Called by be_dump(): one line per global, hexadecimal bytes in memory order. */
void be_emit(const char *name, const void *data, unsigned int size)
{
    const unsigned char *p = data;
    unsigned int i;
    printf("DATA %s %u ", name, size);
    for (i = 0; i < size; i++) {
        printf("%02x", p[i]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    unsigned int fixed = host_swap_game_data();
    printf("INFO %u object files had initialised data to reverse\n", fixed);
    be_run_tests();
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        be_dump();
    }
    printf("RESULT %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
