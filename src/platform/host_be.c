/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_be.c - start-up reversal of initialised game data (see tools/native/be_memory.py, rule 2).
 *
 * Every game-side object file that has initialised data containing multi-byte values carries one small function (`__be_fix`) that
 * reverses those values in place, and a pointer to that function in a special section. This file finds all those pointers and
 * calls each function once.
 *
 * Finding them without a registry that the linker would have to complete:
 *   ELF (Linux)  : the section is called `be_fixups`; the linker provides __start_be_fixups / __stop_be_fixups.
 *   COFF (Windows): the sections are `.befix$a` (our begin marker), `.befix$m` (the entries), `.befix$z` (our end marker). The
 *                  linker sorts the part after `$` alphabetically, so the entries land between the two markers. This is the same
 *                  mechanism the C runtime uses for its start-up tables. tools/native/pe_check.py verifies it on the finished .exe.
 * Only object files that are actually linked contribute, which matters for the libultra archive (members are pulled in on demand).
 */
#include "platform.h"

typedef void (*be_fix_fn)(void);

#if defined(_WIN32)
static const be_fix_fn be_begin __attribute__((section(".befix$a"), used)) = 0;
static const be_fix_fn be_end __attribute__((section(".befix$z"), used)) = 0;
#define BE_FIRST (&be_begin + 1)
#define BE_LIMIT (&be_end)
#elif defined(__wasm__)
/* WebAssembly: wasm-ld makes no __start_/__stop_ symbols. src/platform/wasm_markers/be_begin.c and be_end.c are linked first and last; the
 * sections called `be_fixups` are joined in link order, so the entries lie between the two markers (checked by tools/native/wasm_check.py). */
extern const be_fix_fn gBeFixupsBegin, gBeFixupsEnd;
#define BE_FIRST (&gBeFixupsBegin + 1)
#define BE_LIMIT (&gBeFixupsEnd)
#else
extern const be_fix_fn __start_be_fixups[] __attribute__((weak));
extern const be_fix_fn __stop_be_fixups[] __attribute__((weak));
#define BE_FIRST __start_be_fixups
#define BE_LIMIT __stop_be_fixups
#endif

unsigned int host_swap_game_data(void)
{
    static int done;
    static unsigned int count;
    const be_fix_fn *p;

    if (done) {
        return count;                    /* reversing twice would restore the host's order: never do it */
    }
    done = 1;
    if (BE_FIRST == 0) {                 /* no game object carries data (possible in tiny tests) */
        return 0;
    }
    for (p = BE_FIRST; p < BE_LIMIT; p++) {
        if (*p) {                        /* the linker may pad the section with zeros */
            (*p)();
            count++;
        }
    }
    return count;
}
