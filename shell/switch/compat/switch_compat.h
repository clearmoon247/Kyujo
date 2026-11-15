/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * switch_compat.h - force-included (-include) into every C file of the Switch build, before anything else.
 * devkitA64's newlib has no sigsetjmp/siglongjmp (wabt's wasm-rt.h uses them on every non-Windows system); there are no signal masks to save on Horizon,
 * so the plain setjmp family is the same thing.
 */
#ifndef KYUJO_SWITCH_COMPAT_H
#define KYUJO_SWITCH_COMPAT_H
#include <setjmp.h>
#ifndef sigsetjmp
#define sigjmp_buf jmp_buf
#define sigsetjmp(buf, mask) setjmp(buf)
#define siglongjmp(buf, val) longjmp(buf, val)
#endif
#endif
