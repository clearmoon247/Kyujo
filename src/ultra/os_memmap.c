/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * os_memmap.c - address translation (lib/ultralib/src/os/virtualtophysical.c, physicaltovirtual.c).
 *
 * On the N64, game pointers are KSEG0 virtual addresses (0x80xxxxxx) and the hardware wants the physical address (0x00xxxxxx).
 * On the PC a game pointer is a host pointer and the emulated hardware (hw_regs.c, later the RSP/VI/AI models) dereferences the
 * very same value, so the translation is the identity. The original functions would mask or probe the TLB and give wrong answers
 * for host addresses.
 *
 * Consequence (docs/architecture.md, memory model): host pointers must not collide with the ranges the game uses as other kinds of
 * address - segment numbers 0x01..0x0F in the top byte.
 *
 * GAME-side file: libultra headers only.
 */
#include "PR/os_internal.h"

u32 osVirtualToPhysical(void *addr)
{
    return (u32)(unsigned long)addr;
}

void *osPhysicalToVirtual(u32 addr)
{
    return (void *)(unsigned long)addr;
}
