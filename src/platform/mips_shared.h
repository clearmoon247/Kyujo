/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * mips_shared.h - the record that the wasm side (src/platform/host_mips.c) and the native side (shell/core/mips_native.c) of the MIPS interpreter share.
 * It lies in the core's memory (little endian, plain fields only, the same layout in both compilers: the 64-bit counters first).
 */
#ifndef MIPS_SHARED_H
#define MIPS_SHARED_H
typedef struct MipsShared {
    unsigned long long ns_yield, ns_native, instr;   /* of the current outermost call: time switched out at a preemption poll, time in native functions, branches run */
    unsigned int nested_sp;                          /* sp while the owner is inside a native function called from interpreted code (0 = not inside) */
    unsigned int pool_base, pool_bytes;              /* the memory pool the interpreted code lives in */
    unsigned int preempt_flag;                       /* the core-memory address of host_preempt_flag */
    unsigned int dbg_addr;                           /* the core-memory address of host_dbg (stall report) */
} MipsShared;
#endif
