/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * missing.c - reporting: unimplemented functions, unimplemented hardware registers, fatal errors, log lines.
 *
 * tools/native/gen_platform_symbols.py generates one tiny function per not-yet-implemented symbol; each calls host_missing() with
 * its own name. Stopping at the first such call (instead of continuing with wrong data) means the program never "half works":
 * the console shows exactly which piece is needed next, and the number of these stubs only ever goes down as real code replaces them.
 */
#include <stdio.h>
#include <stdlib.h>

#include "platform.h"

void host_missing(const char *symbol)
{
    fprintf(stderr, "kyujo: '%s' is called but is not implemented yet\n", symbol);
    fflush(stderr);
    /* 70 (EX_SOFTWARE in sysexits.h) is used on purpose: a test script can tell "ran until the first gap" (70) from a crash (a
     * signal / 128+n) and from success (0). _Exit skips atexit handlers, which could themselves hit more missing code. */
    _Exit(HOST_EXIT_MISSING);
}

void host_missing_register(const char *access, unsigned int phys_addr)
{
    fprintf(stderr, "kyujo: hardware register access not implemented yet: %s of physical address 0x%08X\n", access, phys_addr);
    fflush(stderr);
    _Exit(HOST_EXIT_MISSING);
}

void host_log(const char *message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

#ifdef _WIN32
#include <windows.h>
/* A program started by double-click owns a console window that disappears the moment the program ends, taking the error text with it.
 * So on Windows the message is ALSO shown in a message box (user32 is looked up at run time, so no link-time dependency is added to the
 * small test programs that use this file). NOT run on Windows yet. */
void host_error_box(const char *text)
{
    HMODULE u = LoadLibraryA("user32.dll");
    if (u) {
        typedef int (WINAPI *MessageBoxAFn)(HWND, LPCSTR, LPCSTR, UINT);
        MessageBoxAFn mb = (MessageBoxAFn)(void *)GetProcAddress(u, "MessageBoxA");
        if (mb) mb(NULL, text, "Pokemon Stadium - stopped", 0x10 /* MB_ICONERROR */);
    }
}
#else
void host_error_box(const char *text) { (void)text; }       /* other systems: the message on stderr is enough */
#endif

void host_fatal(const char *message)
{
    fprintf(stderr, "kyujo: FATAL: %s\n", message);
    fflush(stderr);
#ifdef _WIN32
    host_error_box(message);
#endif
    _Exit(1);
}

/* _Exit() skips atexit handlers, but closing the window must not lose what the game wrote to a Game Boy cartridge's save RAM. The Transfer Pak
 * code therefore registers its flush function here (a hook, not a direct call, so that small test programs that link this file but not the
 * Transfer Pak still link). The N64 save itself is written at once by host_save_store. */
static void (*sExitHook)(void);
void host_set_exit_hook(void (*fn)(void)) { sExitHook = fn; }


void host_run_exit_hook(void) { host_save_flush(); if (sExitHook) sExitHook(); }

void host_exit(int status)
{
    host_save_flush();
    if (sExitHook) sExitHook();
    fflush(stderr);
    _Exit(status);
}

/* ---- development trace (KYUJO_TRACE) ---------------------------------------------------------------------------------------- */
void host_trace(const char *what, unsigned int a, unsigned int b)
{
    static int checked, on;
    static unsigned long long start;
    if (!checked) {
        checked = 1;
        on = getenv("KYUJO_TRACE") != NULL;
        start = host_now_ns();
    }
    if (!on) {
        return;
    }
    fprintf(stderr, "[+%9.3f ms] %s 0x%X 0x%X\n", (double)(host_now_ns() - start) / 1e6, what, a, b);
}
