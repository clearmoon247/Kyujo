/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/* jpeg_task.h - host-side JPEG microcode replacement (see jpeg_task.c). */
#ifndef JPEG_TASK_H
#define JPEG_TASK_H
#include <stdint.h>

/* Converts one 16x16 mode-2 MCU: `coef` = 6 blocks of 64 big-endian s16 (zig-zag order), q* = 64 host u16 each (zig-zag order).
 * `out` receives 16x16 big-endian RGBA5551 pixels (0x200 bytes). Exposed so tests can drive it without game memory. */
void jpeg_mcu_to_rgba5551(const uint8_t *coef, const uint16_t *qy, const uint16_t *qc_u, const uint16_t *qc_v, uint8_t *out);

/* Runs the task whose 0x20-byte parameter block is at `data_addr`. Returns the interrupts owed beyond the SP one (always 0). */
unsigned int host_jpeg_task(unsigned int data_addr);
#endif
