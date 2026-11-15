/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * rsp_client.c - the RSP interpreter as seen from inside the sandboxed core (docs/switch-android-plan.md).
 *
 * The interpreter itself (rsp_cpu.c) is compiled into the native shell: run as WebAssembly it spent most of the game's processor time, and held the
 * core's lock while it did. The core reaches it through the "shell" imports below, with the same functions as before (platform.h); the memories are
 * passed as addresses in the core's memory, which the shell turns into pointers (shell/core/rdp_glue.c).
 *
 * HOST-side, only for the wasm32 profile.
 */
#if defined(__wasm__)

#include "platform.h"

#define SHELL_IMPORT(name) __attribute__((import_module("shell"), import_name(#name)))
SHELL_IMPORT(rsp_start)   extern void shell_rsp_start(unsigned int dmem, unsigned int imem, unsigned int pc, unsigned int mem_addr, unsigned int dram_addr,
                                                      unsigned int status, unsigned int semaphore, unsigned int dpc_status);
SHELL_IMPORT(rsp_run)     extern unsigned int shell_rsp_run(void);
SHELL_IMPORT(rsp_resume)  extern void shell_rsp_resume(unsigned int status, unsigned int semaphore, unsigned int dpc_status);
SHELL_IMPORT(rsp_get)     extern unsigned int shell_rsp_get(unsigned int which);

void host_rsp_start(unsigned char *dmem, unsigned char *imem, unsigned int pc, unsigned int mem_addr, unsigned int dram_addr,
                    unsigned int status, unsigned int semaphore, unsigned int dpc_status)
{
    shell_rsp_start((unsigned int)(unsigned long)dmem, (unsigned int)(unsigned long)imem, pc, mem_addr, dram_addr, status, semaphore, dpc_status);
}
unsigned int host_rsp_run(void) { return shell_rsp_run(); }
void host_rsp_resume(unsigned int status, unsigned int semaphore, unsigned int dpc_status) { shell_rsp_resume(status, semaphore, dpc_status); }
unsigned int host_rsp_get(unsigned int which) { return shell_rsp_get(which); }

#endif
