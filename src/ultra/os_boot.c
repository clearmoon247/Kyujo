/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * os_boot.c - the "boot parameters" the N64's start-up firmware (IPL) leaves in low memory for the game.
 *
 * libultra declares them as absolute addresses (lib/ultralib/src/os/parameters.s: osTvType at 0x80000300, osRomBase at 0x80000308,
 * ...). On a PC there is no memory at those addresses, so they are ordinary variables here, with the values the firmware would
 * have stored for this game on a console. Types are those of lib/ultralib/include/PR/os_system.h.
 *
 * GAME-side file: libultra headers only.
 */
#include "PR/os_internal.h"
#include "PR/leo.h"

s32   osRomType = 0;                                   /* 0 = cartridge (os_system.h) */
void *osRomBase = (void *)0xB0000000;                  /* where the cartridge appears in KSEG1: 0x10000000 physical */
s32   osTvType = OS_TV_NTSC;                           /* US release: NTSC (os_system.h: 0 PAL, 1 NTSC, 2 MPAL) */
s32   osResetType = 0;                                 /* 0 = cold reset */
s32   osCicId = 0;                                     /* identifies the cartridge's lock-out chip; nothing in the game or libultra reads it */
s32   osVersion = 0;                                   /* OS version stamp left by the IPL; unused */
u32   osMemSize = 0x400000;                            /* 4 MB: console without Expansion Pak (the game sizes its pool for 4 MB first) */
s32   osAppNMIBuffer[OS_APP_NMI_BUFSIZE / sizeof(s32)];/* 64 bytes kept across a soft reset */

/* 64DD boot disk ID area (parameters.s: leoBootID at 0x800001A0). The game copies the disk ID here before booting a disk. */
LEODiskID leoBootID;
