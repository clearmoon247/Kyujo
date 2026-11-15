/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/* sched_test_host.c - HOST-side entry point of the scheduler self-test (see sched_test_game.c). */
#include "platform.h"

void sched_test_boot(void);

int main(void)
{
    host_swap_game_data();           /* game memory is big-endian: reverse the initialised data first (see platform.h) */
    sched_test_boot();
    return 0;
}

/* The test has no overlays (fragments) to load, so every indirect call target is already the real function: identity.
 * The real program's version is in src/platform/host_fragment.c. (Needed since game code compiled by be_memory.py sends its
 * indirect calls through this hook.) */
void *HostResolveCallTarget(void *target)
{
    return target;
}
