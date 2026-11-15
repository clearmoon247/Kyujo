/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/* core_internal.h - shared by core_host.c and wasi_host.c (not for the fronts). */
#ifndef CORE_INTERNAL_H
#define CORE_INTERNAL_H

#include <stdint.h>

struct Core;
struct w2c_shell { struct Core *core; };
struct w2c_wasi__snapshot__preview1 { struct Core *core; };

typedef struct Core {
    w2c_kyujo inst;
    struct w2c_shell shell;
    struct w2c_wasi__snapshot__preview1 wasi;
    CoreFront front;
    int argc;
    char **argv;
    int exit_code;
} Core;

extern Core gCore;

void core_lock_take(void);               /* hold the wasm lock (restores this thread's shadow stack pointer) */
void core_lock_drop(void);               /* release it (saves the pointer); do this around anything that waits */
int core_has_lock(void);
void core_exit(int code);
void core_trap_report(const char *where, wasm_rt_trap_t code);
int wasi_init(Core *c, const char *root_dir);
void wasi_reset(Core *c, const char *root_dir);
void rdp_glue_memory_replaced(void);

#endif
