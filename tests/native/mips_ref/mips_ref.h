/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * mips_ref.h - a small MIPS III integer interpreter used ONLY BY TESTS, to run the console's original hand-written machine code and
 * compare its results with the C code that replaces it (src/game_asm/*.c).
 *
 * It is deliberately separate from src/platform/host_mips_stub.c (which runs tiny entry stubs inside the game and refuses calls):
 * this one is for checking, handles everything the original assembly of this game uses so far, and stops with a message on anything
 * else. It is written from the instruction descriptions in "MIPS IV Instruction Set" (Price, 1995) and the VR4300 user's manual; the
 * semantics used here are the integer ones of MIPS I/III that do not differ between those documents.
 *
 * Memory model: ONE flat block standing for the console's RDRAM. Every address is reduced with `& 0x1FFFFFFF` (KSEG0/KSEG1 -> physical),
 * must then lie inside the block, and the bytes are BIG-endian like the console's.
 */
#ifndef MIPS_REF_H
#define MIPS_REF_H

typedef struct MipsRef {
    unsigned int r[32];
    unsigned int hi, lo;
    unsigned int pc;
    unsigned char *mem;            /* the block, big-endian bytes */
    unsigned int mem_size;
    unsigned long long steps;      /* instructions executed so far (also counts delay slots) */
    unsigned long long max_steps;  /* run stops with an error beyond this */
    char error[160];               /* empty, or the reason the last run stopped */
} MipsRef;

/* Initialises `m` around the block `mem` of `size` bytes (the caller owns it). */
void mips_ref_init(MipsRef *m, unsigned char *mem, unsigned int size);

/* Calls the function at virtual address `entry` with four argument registers and `sp` as stack pointer, exactly like a C call, until it
 * returns to a sentinel address. Returns 0 on success and stores v0 in *v0 (may be NULL); returns -1 and fills m->error otherwise. */
int mips_ref_call(MipsRef *m, unsigned int entry, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3,
                  unsigned int sp, unsigned int *v0);

/* Big-endian accessors on the block, for test code that prepares inputs and reads outputs by console address. */
unsigned int mips_ref_read32(const MipsRef *m, unsigned int addr);
void mips_ref_write32(MipsRef *m, unsigned int addr, unsigned int value);
unsigned int mips_ref_read16(const MipsRef *m, unsigned int addr);
void mips_ref_write16(MipsRef *m, unsigned int addr, unsigned int value);

#endif
