/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * sys/mman.h - devkitA64's C library has none, and wabt's wasm-rt-impl.c includes it. The core is built with WASM_RT_USE_MMAP=0 (explicit bounds checks, memory
 * from malloc), so nothing from this header is used: this empty file only lets the include succeed.
 */
#ifndef KYUJO_COMPAT_SYS_MMAN_H
#define KYUJO_COMPAT_SYS_MMAN_H
#endif
