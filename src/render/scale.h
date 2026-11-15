/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * scale.h - scaling the finished picture to the size of the player's window ("sharp bilinear"). HOST-side, plain C, no operating-system calls,
 * so it can be tested on Linux (tests/native/render/scale_check.c) although only the Windows window uses it.
 */
#ifndef PS_SCALE_H
#define PS_SCALE_H

#include <stdint.h>

/* Scales the picture `rgb` (sw x sh, 3 bytes per pixel R,G,B, rows top to bottom) into `dst` (cw x ch pixels, 4 bytes per pixel B,G,R,0, which is what
 * Windows' 32-bit DIBs hold). The picture is shown at the 4:3 shape of the console's screen, centred, whatever its pixel size; the rest of `dst` is black.
 * `work` must hold at least 2 * cw * 4 bytes (two horizontally scaled rows are kept between output rows).
 * Returns 0 on success, -1 if a size is zero or too large (the picture sizes in this program are at most 1024 x 768). */
int scale_picture(const uint8_t *rgb, unsigned sw, unsigned sh, uint8_t *dst, unsigned cw, unsigned ch, uint8_t *work);

/* The same, with the choice of shape: stretch = 0 keeps the console's 4:3 (black bars), stretch = 1 fills the whole window (the picture is drawn wider or
 * taller than it was made; the game does not draw a wider view). scale_picture() is the stretch = 0 case. */
int scale_picture_mode(const uint8_t *rgb, unsigned sw, unsigned sh, uint8_t *dst, unsigned cw, unsigned ch, uint8_t *work, int stretch);

/* The rectangle the picture occupies inside a cw x ch window (the same arithmetic as scale_picture uses). */
void scale_fit_rect(unsigned cw, unsigned ch, unsigned *dx, unsigned *dy, unsigned *dw, unsigned *dh);
void scale_fit_rect_mode(unsigned cw, unsigned ch, unsigned *dx, unsigned *dy, unsigned *dw, unsigned *dh, int stretch);

#endif
