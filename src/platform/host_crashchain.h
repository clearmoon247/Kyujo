/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
#ifndef HOST_CRASHCHAIN_H
#define HOST_CRASHCHAIN_H

/* Writes into `out` (at most `size` bytes, always terminated) the return addresses found by following the saved-ebp chain that starts at `ebp`,
 * as "  caller 0x........" lines, at most `max` of them. The walk reads memory only inside [lo, hi) (the thread's stack), and stops at the first link
 * that is misaligned, outside that range, or not above the previous one. Code compiled without frame pointers ends the chain early; that is harmless.
 * Returns the number of frames written. Used by the crash report, see host_main.c. */
int host_format_frame_chain(char *out, unsigned size, unsigned long ebp, unsigned long lo, unsigned long hi, int max);

#endif
