/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * hw_internal.h - what the pieces of the hardware emulation (hw_regs.c, hw_flash.c, ...) share. Not for game or libultra code.
 * GAME-side header: libultra types only.
 */
#ifndef HW_INTERNAL_H
#define HW_INTERNAL_H

#include "PR/ultratypes.h"

/* ---- the cartridge's flash save memory (hw_flash.c) ---- */
/* True if physical address `a` lies in the flash chip's window (domain 2, address 2, first 128 KB + command register). */
int  flash_owns(u32 a);
u32  flash_read(u32 a);                         /* CPU/PI word read from the window */
void flash_write(u32 a, u32 v);                 /* CPU/PI word write to the window: commands */
void flash_dma_to_dram(u32 a, void *dst, u32 len);        /* PI DMA from the chip */
void flash_dma_from_dram(u32 a, const void *src, u32 len); /* PI DMA to the chip */

/* ---- the PIF / controllers (hw_pif.c) ---- */
#define CONT_CMD_EXE_BIT 0x01        /* bit 0 of the last PIF RAM byte: "run the command block" (libultra: CONT_CMD_EXE in controller.h) */
u32  HostPifRead32(u32 offset);                 /* CPU word access to PIF RAM, offset 0..60 from PIF_RAM_START */
void HostPifWrite32(u32 offset, u32 v);
void HostPifDmaFromDram(const void *dram);      /* SI DMA memory -> PIF RAM (runs the block when bit 0 of the last byte is set) */
unsigned long long HostPifDmaToDram(void *dram); /* SI DMA PIF RAM -> memory (runs the block's commands); returns the Joybus busy time in ns */

/* ---- the audio interface (hw_ai.c) ---- */
u32  HostAiRead32(u32 reg);                     /* reg: a physical register address, AI_DRAM_ADDR_REG .. AI_BITRATE_REG */
void HostAiWrite32(u32 reg, u32 v);
/* Provided by hw_regs.c for the AI: raise / clear the AI line of the interrupt controller (MI). */
void HostHwRaiseAiIrq(void);
void HostHwAckAiIrq(void);

/* ---- the RSP and RDP (hw_sp.c, rsp_hle.c) ---- */
int  HostSpOwns(u32 a);                         /* true for DMEM/IMEM, the SP registers (SP_PC_REG included) and the DPC registers */
u32  HostSpRead32(u32 a);
void HostSpWrite32(u32 a, u32 v);
void HostHwRaiseSpIrq(void);                    /* provided by hw_regs.c: the SP and DP lines of the interrupt controller */
void HostHwAckSpIrq(void);
void HostHwRaiseDpIrq(void);

/* Diagnostic: logs "<label> 0x<8 hex digits>" (the game side has no printf). */
void HostHwLogHex(const char *label, u32 v);

#endif
