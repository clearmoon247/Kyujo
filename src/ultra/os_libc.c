/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * os_libc.c - libultra's memory routines (lib/ultralib/src/libc/bcopy.s, bzero.s, bcmp.s) on top of the host C library.
 *
 * The original library defines each name twice, with and without a leading underscore (weak aliases): the game calls _bcopy,
 * libultra itself calls bcopy/bzero/bcmp. Windows' C runtime has none of these BSD names, so they are defined here for every target.
 *
 * Behaviour taken from the assembly:
 *   bcopy(src, dst, n)  copies n bytes, correctly even when the ranges overlap (it picks the direction); returns nothing useful
 *   bzero(p, n)         sets n bytes to 0
 *   bcmp(a, b, n)       returns 0 if the n bytes are equal, otherwise 1 (not the byte difference)
 * A count <= 0 does nothing. Argument order of bcopy is (SOURCE, destination) - the opposite of memcpy.
 *
 * GAME-side file: no C library headers; the three C library functions are declared by hand with the 32-bit size type.
 */
typedef unsigned int host_size_t;                  /* size_t on both 32-bit targets (checked by the static assert below) */
_Static_assert(sizeof(host_size_t) == sizeof(void *), "this port is 32-bit");

extern void *memmove(void *, const void *, host_size_t);
extern void *memset(void *, int, host_size_t);
extern int   memcmp(const void *, const void *, host_size_t);

void bcopy(const void *src, void *dst, int n)    { if (n > 0) memmove(dst, src, (host_size_t)n); }
void _bcopy(const void *src, void *dst, int n)   { bcopy(src, dst, n); }

void bzero(void *p, int n)                       { if (n > 0) memset(p, 0, (host_size_t)n); }
void _bzero(void *p, int n)                      { bzero(p, n); }
void blkclr(void *p, int n)                      { bzero(p, n); }
void _blkclr(void *p, int n)                     { bzero(p, n); }

int bcmp(const void *a, const void *b, int n)    { return (n > 0 && memcmp(a, b, (host_size_t)n) != 0) ? 1 : 0; }
int _bcmp(const void *a, const void *b, int n)   { return bcmp(a, b, n); }
