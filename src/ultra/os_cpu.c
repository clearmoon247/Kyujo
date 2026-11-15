/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * os_cpu.c - stand-ins for the parts of the N64's CPU that a PC does not have: status/cause registers, the FPU control register,
 * caches, the TLB and the debug watch register. libultra reaches these through small assembly routines (lib/ultralib/src/os/*.s,
 * named in the comments below); here they are plain C.
 *
 * Interrupt enable and mask are NOT here: they are tied to the scheduler and live in os_sched.c.
 * GAME-side file: libultra headers only.
 */
#include "PR/os_internal.h"
#include "platform.h"

/* ---- CP0 Status / Cause (getsr.s setsr.s getcause.s setcause.s) ---------------------------------------------------------------------- */
/* Only osInitialize touches them (it sets the "FPU usable" bit). The stored values are not interpreted: whether interrupts are
 * enabled is tracked by the scheduler layer instead. */
static u32 sStatus, sCause, sFpcCsr;

void __osSetSR(u32 v)    { sStatus = v; }
u32  __osGetSR(void)     { return sStatus; }
void __osSetCause(u32 v) { sCause = v; }
u32  __osGetCause(void)  { return sCause; }

/* ---- FPU control/status (setfpccsr.s getfpccsr.s) -------------------------------------------------------------------------------------- */
/* On the N64 this selects rounding mode and flush-denormals-to-zero. The host FPU is NOT reprogrammed: results may differ from the
 * console in rare denormal cases (docs/native-port-feasibility.md, "float behaviour"). The value is kept so a read returns what
 * was written. Returns the previous value, as the assembly does. */
u32 __osSetFpcCsr(u32 v) { u32 old = sFpcCsr; sFpcCsr = v; return old; }
u32 __osGetFpcCsr(void)  { return sFpcCsr; }

/* ---- caches (invaldcache.s invalicache.s writebackdcache.s) --------------------------------------------------------------------------- */
/* The host's caches are coherent with its memory (and RCP emulation reads the same memory), so there is nothing to flush. */
void osInvalDCache(void *a, s32 n)     { (void)a; (void)n; }
void osInvalICache(void *a, s32 n)     { (void)a; (void)n; }
void osWritebackDCache(void *a, s32 n) { (void)a; (void)n; }
void osWritebackDCacheAll(void)        { }

/* ---- TLB (maptlbrdb.s unmaptlball.s) ------------------------------------------------------------------------------------------------------ */
/* osInitialize maps the debugger (RDB) window; the host has no TLB and no debugger hardware. */
void osMapTLBRdb(void)   { }
void osUnmapTLBAll(void) { }

/* ---- WatchLo (setwatchlo.s) --------------------------------------------------------------------------------------------------------------- */
/* A CPU debug register that traps accesses to one address. Not emulated. `set_watch_lohi` is the game's own wrapper, called from Main(). */
void __osSetWatchLo(u32 v) { (void)v; }
void set_watch_lohi(s32 v) { (void)v; }
