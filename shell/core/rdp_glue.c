/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * rdp_glue.c - the RDP and the video output on the native side of the sandboxed core (docs/switch-android-plan.md, section 11).
 *
 * src/render/rdp.c and vi_out.c are compiled into the shell (-DRDP_SHELL_NATIVE, src/render/rdram.h) and read the game's memory directly: an address is
 * an offset into the module's linear memory, gRdpMem is its start. The core reaches them through the "shell" imports below (src/render/rdp_client.c).
 *
 * LOCKING. The drawing calls run WITHOUT the wasm lock: the graphics task is a game thread of its own, and while it draws the other game threads must go on
 * (as on the console and in the native builds). They copy their arguments first, drop the lock, draw, and take it again. The renderer's own state is touched
 * by that one thread only (rdp_task_may_draw_to is the exception: it reads atomics).
 * MEMORY. The renderer keeps pointing at the memory start while it draws, so the memory must not move: it never grows after start-up (see build_native.py,
 * initial = maximum), checked here by comparing the start with the one seen at the first call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core_host.h"
#include "wasm-rt.h"
#include "wasm-rt-impl.h"
#include "kyujo.h"
#include "core_internal.h"
#include "rdram.h"
#include "rdp.h"
#include "gfx.h"

uint8_t *gRdpMem;
uint32_t gRdpMemSize;

unsigned long long host_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ULL + (unsigned long long)t.tv_nsec;
}

void host_game_range_bounds(unsigned int *lo, unsigned int *hi) { *lo = 0; *hi = gRdpMemSize; }
int host_game_range_ok(unsigned int addr, unsigned int len) { return len <= gRdpMemSize && addr <= gRdpMemSize - len; }

/* Makes the renderer's view of the memory current. Called with the wasm lock held. */
static uint8_t *sFirstMem;

/* The instance was thrown away (core_reset): its memory is gone and the next one may sit at another address. Nothing is read through gRdpMem until the new game's first rdp call. */
void rdp_glue_memory_replaced(void)
{
    sFirstMem = NULL;
    gRdpMem = NULL;
    gRdpMemSize = 0;
}

static void sync_mem(void)
{
    wasm_rt_memory_t *m = w2c_kyujo_memory(&gCore.inst);
    uint8_t *first = sFirstMem;
    if (!first) sFirstMem = m->data;
    else if (m->data != first) { fprintf(stderr, "core: the core's memory moved (it grew): the renderer would read stale addresses\n"); abort(); }
    gRdpMem = m->data;
    gRdpMemSize = (uint32_t)m->size;
}

static int valid(uint32_t p, uint32_t len) { return (uint64_t)p + len <= gRdpMemSize; }

void w2c_shell_rdp_reset(struct w2c_shell *s) { (void)s; sync_mem(); rdp_reset(); }
void w2c_shell_rdp_command(struct w2c_shell *s, uint32_t hi, uint32_t lo)
{
    (void)s; sync_mem();
    core_lock_drop(); rdp_command(hi, lo); core_lock_take();
}
void w2c_shell_rdp_set_other_modes(struct w2c_shell *s, uint32_t hi, uint32_t lo) { (void)s; sync_mem(); rdp_set_other_modes(hi, lo); }
void w2c_shell_rdp_texture_rectangle(struct w2c_shell *s, uint32_t w0, uint32_t w1, uint32_t h1, uint32_t h2, uint32_t flip)
{
    (void)s; sync_mem();
    core_lock_drop(); rdp_texture_rectangle(w0, w1, h1, h2, (int)flip); core_lock_take();
}
void w2c_shell_rdp_triangle(struct w2c_shell *s, uint32_t a, uint32_t b, uint32_t c, uint32_t tile, uint32_t level, uint32_t sh, uint32_t tex, uint32_t z)
{
    RdpVert v[3];
    (void)s; sync_mem();
    if (!valid(a, sizeof(RdpVert)) || !valid(b, sizeof(RdpVert)) || !valid(c, sizeof(RdpVert))) return;
    memcpy(&v[0], gRdpMem + a, sizeof v[0]);
    memcpy(&v[1], gRdpMem + b, sizeof v[1]);
    memcpy(&v[2], gRdpMem + c, sizeof v[2]);
    core_lock_drop(); rdp_triangle(&v[0], &v[1], &v[2], (int)tile, (int)level, (int)sh, (int)tex, (int)z); core_lock_take();
}
uint32_t w2c_shell_rdp_full_syncs(struct w2c_shell *s) { (void)s; return rdp_full_syncs(); }
void w2c_shell_rdp_memory_reused(struct w2c_shell *s, uint32_t addr, uint32_t len) { (void)s; rdp_memory_reused(addr, len); }
uint32_t w2c_shell_rdp_color_image_addr(struct w2c_shell *s) { (void)s; return rdp_color_image_addr(); }
void w2c_shell_rdp_cimg_log_reset(struct w2c_shell *s) { (void)s; rdp_cimg_log_reset(); }
uint32_t w2c_shell_rdp_task_may_draw_to(struct w2c_shell *s, uint32_t origin) { (void)s; return (uint32_t)rdp_task_may_draw_to(origin); }
uint64_t w2c_shell_rdp_dev_color_image_hash(struct w2c_shell *s) { uint64_t h; (void)s; sync_mem(); core_lock_drop(); h = rdp_dev_color_image_hash(); core_lock_take(); return h; }
void w2c_shell_rdp_report_unimplemented(struct w2c_shell *s) { (void)s; rdp_report_unimplemented(); }
void w2c_shell_rdp_profile_report(struct w2c_shell *s, uint64_t ns) { (void)s; rdp_profile_report(ns); }
void w2c_shell_rdp_stats(struct w2c_shell *s, uint32_t tp, uint32_t rp, uint32_t pp)
{
    uint32_t t, r; uint64_t p;
    (void)s; sync_mem();
    rdp_stats(&t, &r, &p);
    if (valid(tp, 4)) memcpy(gRdpMem + tp, &t, 4);
    if (valid(rp, 4)) memcpy(gRdpMem + rp, &r, 4);
    if (valid(pp, 8)) memcpy(gRdpMem + pp, &p, 8);
}
void host_rdp_trace(int on);
void w2c_shell_rdp_trace(struct w2c_shell *s, uint32_t on) { (void)s; host_rdp_trace((int)on); }

uint32_t w2c_shell_vi_scanout(struct w2c_shell *s, uint32_t regs, uint32_t rgb, uint32_t max_w, uint32_t max_h, uint32_t pw, uint32_t ph)
{
    ViRegs r;
    unsigned w = 0, h = 0;
    int ok;
    (void)s; sync_mem();
    if (!valid(regs, sizeof r) || !valid(pw, 4) || !valid(ph, 4) || !valid(rgb, max_w * max_h * 3u)) return 0;
    memcpy(&r, gRdpMem + regs, sizeof r);
    core_lock_drop();                    /* waits for the GPU thread; nothing else of the game may be held up meanwhile */
    ok = vi_scanout(&r, gRdpMem + rgb, max_w, max_h, &w, &h);
    core_lock_take();
    memcpy(gRdpMem + pw, &w, 4);
    memcpy(gRdpMem + ph, &h, 4);
    return (uint32_t)ok;
}

/* The picture straight from the GPU's colour surface to the window: queued, the game does not wait for the GPU (gpu_show_picture). */
uint32_t w2c_shell_vi_present(struct w2c_shell *s, uint32_t regs)
{
    ViRegs r;
    int ok;
    (void)s; sync_mem();
    if (!valid(regs, sizeof r)) return 0;
    memcpy(&r, gRdpMem + regs, sizeof r);
    core_lock_drop();                    /* it can wait for the GPU thread when that is far behind */
    ok = vi_present(&r);
    core_lock_take();
    return (uint32_t)ok;
}


/* ---- the RSP interpreter (src/rsp/rsp_cpu.c, native side; src/rsp/rsp_client.c is the core's side) ---------------------------------------------------
 * The audio microcode runs here, outside the wasm module and WITHOUT its lock: it only touches its own state and the core's memory (the microcode's
 * DMEM/IMEM, which the core passes as addresses, and the game's RAM by DMA), as the RSP does on the console while the CPU carries on. The task's own thread
 * waits for it (the call returns when the microcode has hit BREAK or waits for the CPU), so nothing else of the core's RSP state is used meanwhile. */
unsigned int host_rsp_run(void);
void host_rsp_start(unsigned char *dmem, unsigned char *imem, unsigned int pc, unsigned int mem_addr, unsigned int dram_addr, unsigned int status, unsigned int semaphore, unsigned int dpc_status);
void host_rsp_resume(unsigned int status, unsigned int semaphore, unsigned int dpc_status);
unsigned int host_rsp_get(unsigned int which);

void host_log(const char *message) { fprintf(stderr, "kyujo: %s\n", message); }
void host_fatal(const char *message)
{
    fprintf(stderr, "kyujo: fatal: %s\n", message);
    fflush(stderr);
    core_exit(70);
    abort();
}

void w2c_shell_rsp_start(struct w2c_shell *s, uint32_t dmem, uint32_t imem, uint32_t pc, uint32_t mem_addr, uint32_t dram_addr, uint32_t status, uint32_t semaphore, uint32_t dpc_status)
{
    (void)s; sync_mem();
    if (!valid(dmem, 0x1000) || !valid(imem, 0x1000)) host_fatal("RSP memories outside the core's memory");
    host_rsp_start(gRdpMem + dmem, gRdpMem + imem, pc, mem_addr, dram_addr, status, semaphore, dpc_status);
}

uint32_t w2c_shell_rsp_run(struct w2c_shell *s)
{
    uint32_t steps;
    (void)s; sync_mem();
    core_lock_drop();
    steps = host_rsp_run();
    core_lock_take();
    return steps;
}

void w2c_shell_rsp_resume(struct w2c_shell *s, uint32_t status, uint32_t semaphore, uint32_t dpc_status) { (void)s; host_rsp_resume(status, semaphore, dpc_status); }
uint32_t w2c_shell_rsp_get(struct w2c_shell *s, uint32_t which) { (void)s; return host_rsp_get(which); }
