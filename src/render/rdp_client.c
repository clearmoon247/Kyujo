/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * rdp_client.c - the RDP and the video output as seen from INSIDE the sandboxed core (WebAssembly; docs/switch-android-plan.md, section 11).
 *
 * In the native builds rdp.c and vi_out.c are linked into the program. In the sandboxed core they are not: they run in the native shell (shell/core/rdp_glue.c),
 * which reads the game's memory directly and can draw with the GPU. This file implements the same interface (rdp.h, gfx.h's vi_scanout) by calling the shell.
 * Pointers passed (vertices, register block, picture buffer, result variables) are addresses in the core's memory; the shell reads and writes them.
 */
#if defined(__wasm__)

#include <stdint.h>

#include "rdp.h"
#include "gfx.h"

#define SHELL_IMPORT(name) __attribute__((import_module("shell"), import_name(#name)))

SHELL_IMPORT(rdp_reset)               extern void shell_rdp_reset(void);
SHELL_IMPORT(rdp_command)             extern void shell_rdp_command(uint32_t hi, uint32_t lo);
SHELL_IMPORT(rdp_set_other_modes)     extern void shell_rdp_set_other_modes(uint32_t hi, uint32_t lo);
SHELL_IMPORT(rdp_texture_rectangle)   extern void shell_rdp_texture_rectangle(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, int flip);
SHELL_IMPORT(rdp_triangle)            extern void shell_rdp_triangle(const RdpVert *a, const RdpVert *b, const RdpVert *c, int tile, int level, int has_shade, int has_tex, int has_z);
SHELL_IMPORT(rdp_memory_reused)      extern void shell_rdp_memory_reused(uint32_t addr, uint32_t len);
SHELL_IMPORT(rdp_full_syncs)          extern uint32_t shell_rdp_full_syncs(void);
SHELL_IMPORT(rdp_color_image_addr)    extern uint32_t shell_rdp_color_image_addr(void);
SHELL_IMPORT(rdp_cimg_log_reset)      extern void shell_rdp_cimg_log_reset(void);
SHELL_IMPORT(rdp_task_may_draw_to)    extern int shell_rdp_task_may_draw_to(uint32_t origin);
SHELL_IMPORT(rdp_dev_color_image_hash) extern uint64_t shell_rdp_dev_color_image_hash(void);
SHELL_IMPORT(rdp_report_unimplemented) extern void shell_rdp_report_unimplemented(void);
SHELL_IMPORT(rdp_profile_report)      extern void shell_rdp_profile_report(uint64_t task_ns);
SHELL_IMPORT(rdp_stats)               extern void shell_rdp_stats(uint32_t *triangles, uint32_t *rects, uint64_t *pixels);
SHELL_IMPORT(rdp_trace)               extern void shell_rdp_trace(int on);
SHELL_IMPORT(vi_present)              extern int shell_vi_present(const ViRegs *regs);
SHELL_IMPORT(vi_scanout)              extern int shell_vi_scanout(const ViRegs *regs, uint8_t *rgb, unsigned max_w, unsigned max_h, unsigned *out_w, unsigned *out_h);

void rdp_reset(void) { shell_rdp_reset(); }
void rdp_command(uint32_t hi, uint32_t lo) { shell_rdp_command(hi, lo); }
void rdp_set_other_modes(uint32_t hi, uint32_t lo) { shell_rdp_set_other_modes(hi, lo); }
void rdp_texture_rectangle(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, int flip) { shell_rdp_texture_rectangle(w0, w1, half1, half2, flip); }
void rdp_triangle(const RdpVert *a, const RdpVert *b, const RdpVert *c, int tile, int level, int has_shade, int has_tex, int has_z)
{
    shell_rdp_triangle(a, b, c, tile, level, has_shade, has_tex, has_z);
}
uint32_t rdp_full_syncs(void) { return shell_rdp_full_syncs(); }
void rdp_memory_reused(uint32_t addr, uint32_t len) { shell_rdp_memory_reused(addr, len); }
uint32_t rdp_color_image_addr(void) { return shell_rdp_color_image_addr(); }
void rdp_cimg_log_reset(void) { shell_rdp_cimg_log_reset(); }
int rdp_task_may_draw_to(uint32_t origin) { return shell_rdp_task_may_draw_to(origin); }
uint64_t rdp_dev_color_image_hash(void) { return shell_rdp_dev_color_image_hash(); }
void rdp_report_unimplemented(void) { shell_rdp_report_unimplemented(); }
void rdp_profile_report(unsigned long long task_ns) { shell_rdp_profile_report(task_ns); }
void rdp_stats(uint32_t *triangles, uint32_t *rects, uint64_t *pixels) { shell_rdp_stats(triangles, rects, pixels); }
void host_rdp_trace(int on) { shell_rdp_trace(on); }
int vi_present(const ViRegs *regs) { return shell_vi_present(regs); }
int vi_scanout(const ViRegs *regs, uint8_t *rgb, unsigned max_w, unsigned max_h, unsigned *out_w, unsigned *out_h)
{
    return shell_vi_scanout(regs, rgb, max_w, max_h, out_w, out_h);
}

#endif
