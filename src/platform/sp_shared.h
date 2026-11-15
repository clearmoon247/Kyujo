/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * sp_shared.h - the two pieces of RSP behaviour that the CPU-side register model (src/ultra/hw_sp.c, GAME-side) and the RSP interpreter
 * (src/rsp/rsp_cpu.c, HOST-side) must do identically: the DMA engine and the writes to SP_STATUS. They are written once, here, as static
 * inline functions with plain C types, and included by both files, so the two cannot drift apart.
 *
 * Only plain C types are used so that both kinds of file (see platform.h) can include this header.
 *
 * Sources: bit meanings of SP_STATUS from lib/ultralib/include/PR/rcp.h (hw_sp.c checks the numbers below against it at compile time);
 * the DMA length format (length-1 in bits 11:0, row count-1 in bits 19:12, DRAM skip in bits 31:20) from n64brew's "Reality Signal
 * Processor" page.
 */
#ifndef SP_SHARED_H
#define SP_SHARED_H

#ifdef RDP_SHELL_NATIVE
/* The interpreter inside the native shell (shell/core/rdp_glue.c): a DRAM address is an offset into the core's memory (rdram.h) */
#include "rdram.h"
void host_log(const char *message);
#define SP_DRAM_PTR(a) RDRAM_PTR(a)
#else
#include "platform.h"          /* host_game_range_ok, host_log */
#endif
#ifndef SP_DRAM_PTR
#define SP_DRAM_PTR(a) ((unsigned char *)(unsigned long)(a))     /* game side and native builds: an address is a pointer */
#endif

/* SP_STATUS write bits (names are this port's; values from rcp.h). */
#define SPS_CLR_HALT       (1u << 0)
#define SPS_SET_HALT       (1u << 1)
#define SPS_CLR_BROKE      (1u << 2)
#define SPS_CLR_INTR       (1u << 3)
#define SPS_SET_INTR       (1u << 4)
#define SPS_CLR_SSTEP      (1u << 5)
#define SPS_SET_SSTEP      (1u << 6)
#define SPS_CLR_INTR_BREAK (1u << 7)
#define SPS_SET_INTR_BREAK (1u << 8)
/* signal n (0..7): clear = bit 9 + 2n, set = bit 10 + 2n */

/* SP_STATUS read bits. */
#define SPS_HALT       (1u << 0)
#define SPS_BROKE      (1u << 1)
#define SPS_SSTEP      (1u << 5)
#define SPS_INTR_BREAK (1u << 6)
#define SPS_SIG0       (1u << 7)       /* signal n is bit 7 + n */

/* Applies a write of `v` to SP_STATUS to `status` and returns the new value, except that the requests that need more than the register are
 * returned in *req: bit 0 = "acknowledge the SP interrupt" (CLR_INTR), bit 1 = "raise the SP interrupt" (SET_INTR). HALT is applied to the value like
 * the other bits; starting the RSP (CLR_HALT) is the caller's business. */
static inline unsigned int sp_status_write(unsigned int status, unsigned int v, unsigned int *req)
{
    unsigned int n;
    *req = 0;
    if (v & SPS_CLR_BROKE) status &= ~SPS_BROKE;
    if (v & SPS_CLR_INTR) *req |= 1u;
    if (v & SPS_SET_INTR) *req |= 2u;
    if (v & SPS_CLR_SSTEP) status &= ~SPS_SSTEP;
    if (v & SPS_SET_SSTEP) status |= SPS_SSTEP;
    if (v & SPS_CLR_INTR_BREAK) status &= ~SPS_INTR_BREAK;
    if (v & SPS_SET_INTR_BREAK) status |= SPS_INTR_BREAK;
    for (n = 0; n < 8; n++) {
        if (v & (1u << (9 + 2 * n))) status &= ~(SPS_SIG0 << n);
        if (v & (1u << (10 + 2 * n))) status |= (SPS_SIG0 << n);
    }
    if (v & SPS_SET_HALT) status |= SPS_HALT;
    if (v & SPS_CLR_HALT) status &= ~SPS_HALT;
    return status;
}

/* One SP DMA. `to_sp`: DRAM -> DMEM/IMEM (a write to SP_RD_LEN), otherwise the reverse (SP_WR_LEN). `lenreg` is the register value.
 * *mem_addr and *dram_addr are the SP_MEM_ADDR / SP_DRAM_ADDR registers (bit 12 of the first picks IMEM; the second is a host pointer
 * in this port). Memory wraps at 4 KB. Only *dram_addr is advanced (as before this file existed); whether the hardware also advances
 * SP_MEM_ADDR is not established here - libultra and the microcode load both registers before every DMA, so nothing observable depends on it. */
static inline void sp_dma_copy(unsigned char *dmem, unsigned char *imem, unsigned int *mem_addr, unsigned int *dram_addr,
                               unsigned int lenreg, int to_sp)
{
    unsigned char *mem = (*mem_addr & 0x1000) ? imem : dmem;
    unsigned int mem_off = *mem_addr & 0xFF8;               /* bit 12 picks IMEM, bits 11:3 the 8-byte-aligned offset */
    unsigned int row_len = ((lenreg & 0xFFF) + 8) & ~7u;    /* length-1 rounded up to a multiple of 8 */
    unsigned int rows = ((lenreg >> 12) & 0xFF) + 1;
    unsigned int skip = (lenreg >> 20) & 0xFF8;
    unsigned int da = *dram_addr & ~7u;                     /* the DRAM address of the current row */
    unsigned char *dram;
    unsigned int r, i;

    for (r = 0; r < rows; r++) {
        if (!host_game_range_ok(da, row_len)) {   /* garbage DRAM address: skip this row (reported once) */
            static int reported;
            if (!reported) {
                /* host_log takes a finished string (no printf on the game side), so the address is written as hex by hand */
                char msg[96] = "RSP DMA skipped, DRAM address outside game memory: 0x";
                unsigned int k, n = 0, a = da;
                while (msg[n]) n++;
                for (k = 0; k < 8; k++) msg[n++] = "0123456789ABCDEF"[(a >> (28 - 4 * k)) & 15];
                msg[n] = 0;
                reported = 1;
                host_log(msg);
            }
            da += row_len + skip;
            mem_off = (mem_off + row_len) & 0xFFF;
            continue;
        }
        dram = SP_DRAM_PTR(da);
        for (i = 0; i < row_len; i++) {
            unsigned char *m = &mem[(mem_off + i) & 0xFFF];
            if (to_sp) *m = dram[i]; else dram[i] = *m;
        }
        mem_off = (mem_off + row_len) & 0xFFF;
        da += row_len + skip;
    }
    *dram_addr = da;
}

#endif
