/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_range.c - "is this address inside the program image?" (host_game_range_ok, declared in platform.h).
 *
 * Why: the game's memory is the program's own image (its .data/.bss, below 16 MiB, see tools/native/imagelimit.py) and a game pointer is
 * the native address. The renderer, the video output and the RSP DMA follow pointers that come out of the game's data. If such a pointer is
 * garbage (found with the bounds-checking build: a texture address of 0x6150, 0xFFFFFF, 0xFFDB4B) the console would have read some
 * wrapped-around RDRAM; here it would crash the program with a segmentation fault. Asking this function first lets the caller skip the
 * access and report it.
 *
 * How the image is found (not with the linker's `_end`/`__executable_start`: the stub generator, tools/native/gen_platform_symbols.py,
 * turns such "undefined" names into placeholder variables, so they would hold nonsense - found the hard way):
 *   Linux   - dl_iterate_phdr lists the program headers of the running program; the PT_LOAD segments span the image.
 *   Windows - the PE header of the running program: base address and SizeOfImage. UNVERIFIED: never run on Windows.
 */
#include <stdint.h>

#include "platform.h"

#ifdef _WIN32
#include <windows.h>

static void image_bounds(uint32_t *start, uint32_t *end)
{
    const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    *start = (uint32_t)(UINT_PTR)base;
    *end = (uint32_t)((UINT_PTR)base + nt->OptionalHeader.SizeOfImage);
}
#elif defined(__wasm__)
/* WebAssembly: the game's memory is the module's linear memory from the start of its data to the end of its static data (wasm-ld defines these
 * two symbols itself; the stub generator is not involved on this target). */
extern unsigned char __global_base, __heap_base;

static void image_bounds(uint32_t *start, uint32_t *end)
{
    *start = (uint32_t)(uintptr_t)&__global_base;
    *end = (uint32_t)(uintptr_t)&__heap_base;
}
#else
#define _GNU_SOURCE
#include <link.h>

struct Bounds { uintptr_t lo, hi; int found; };

/* Called once for the program itself (the first object; a static program has no others). */
static int phdr_callback(struct dl_phdr_info *info, size_t size, void *arg)
{
    struct Bounds *b = (struct Bounds *)arg;
    unsigned i;
    (void)size;
    if (b->found) return 1;
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t lo, hi;
        if (ph->p_type != PT_LOAD) continue;
        lo = info->dlpi_addr + ph->p_vaddr;
        hi = lo + ph->p_memsz;
        if (!b->found || lo < b->lo) b->lo = lo;
        if (!b->found || hi > b->hi) b->hi = hi;
        b->found = 1;
    }
    return 1;                                          /* stop after the first object */
}

static void image_bounds(uint32_t *start, uint32_t *end)
{
    struct Bounds b = { 0, 0, 0 };
    dl_iterate_phdr(phdr_callback, &b);
    *start = (uint32_t)b.lo;
    *end = (uint32_t)b.hi;                             /* if nothing was found both are 0 and every range is refused: loud, not silent */
}
#endif

int host_game_range_ok(unsigned int addr, unsigned int len)
{
    static uint32_t lo, hi;                            /* looked up on first use (from several threads, but the result is always the same) */
    if (!hi) image_bounds(&lo, &hi);
    return len <= hi - lo && addr >= lo && addr <= hi - len;
}

/* The same bounds, for callers that check many small ranges (the software renderer checks every pixel it writes): they fetch the bounds once
 * and compare inline instead of calling host_game_range_ok for each access. lo == hi == 0 means "unknown": every range is refused. */
void host_game_range_bounds(unsigned int *lo, unsigned int *hi)
{
    uint32_t a = 0, b = 0;
    image_bounds(&a, &b);
    *lo = a;
    *hi = b;
}
