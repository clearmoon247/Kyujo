/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_window_shell.c - the "window" of the game when it runs as WebAssembly inside the native shell (Switch, Android; docs/switch-android-plan.md).
 *
 * Same job as host_window.c (Windows) and host_window_linux.c (X11), different mechanism: the shell owns the real window, the GPU context and the
 * gamepads (SDL2 there). This file only passes the finished picture to the shell and takes the shell's input snapshot.
 *
 *   picture: host_window_present() calls the shell, which copies the RGB bytes at once (it must not keep the pointer) and shows them when its
 *            display loop is ready.
 *   input:   the shell fills the HostRawInput that core_raw_input_ptr() returns (core memory) and then calls core_raw_input_commit(); that
 *            publishes it exactly as the Windows and Linux window threads do. Both calls are made by the shell's own thread while it holds the wasm lock.
 *
 * STATUS: written, compiled for wasm32; the shell side is not written yet.
 */
#if defined(__wasm__)

#include <string.h>

#include "platform.h"
#include "host_ui.h"

#define SHELL_IMPORT(name) __attribute__((import_module("shell"), import_name(#name)))
SHELL_IMPORT(present)         extern void shell_present(const unsigned char *rgb, unsigned int w, unsigned int h);
SHELL_IMPORT(set_fullscreen)  extern void shell_set_fullscreen(int on);
SHELL_IMPORT(set_widescreen)  extern void shell_set_widescreen(int on);
SHELL_IMPORT(graphics_go)     extern void shell_graphics_go(void);
SHELL_IMPORT(set_backdrop)    extern void shell_set_backdrop(const unsigned char *rgb, unsigned int w, unsigned int h);
#ifdef HOST_SHELL_PICKER
SHELL_IMPORT(pick_gb)         extern int shell_pick_gb(int port, int kind, const char *rom);
SHELL_IMPORT(pick_gb_poll)    extern int shell_pick_gb_poll(int *port, int *kind, char *path, unsigned int path_size, char *msg, unsigned int msg_size);
int host_pick_gb_start(int port, int kind, const char *rom) { return shell_pick_gb(port, kind, rom); }
int host_pick_gb_poll(int *port, int *kind, char *path, unsigned int path_size, char *msg, unsigned int msg_size) { return shell_pick_gb_poll(port, kind, path, path_size, msg, msg_size); }
#endif

static HostRawInput sRaw;
static int sFull;

void host_window_present(const unsigned char *rgb, unsigned int w, unsigned int h) { shell_present(rgb, w, h); }
void host_window_graphics_go(void) { shell_graphics_go(); }
void host_window_set_backdrop(const unsigned char *rgb, unsigned int w, unsigned int h) { shell_set_backdrop(rgb, w, h); }
int host_window_wanted(void) { return 1; }
void host_window_prepare(void) {}                             /* the shell made the window before the game started */
const char *host_window_pick_rom(char *buf, unsigned int size) { (void)buf; (void)size; return NULL; }   /* the in-game file browser is used */
void host_window_set_fullscreen(int on) { sFull = on; shell_set_fullscreen(on); }
int host_window_is_fullscreen(void) { return sFull; }

/* Tells the shell about the Widescreen option when it changes (and once at the start). */
void host_window_sync_view(void)
{
    static int sent = -1;
    int w = (host_settings()->widescreen != 0) | (host_settings()->scale_style & 3) << 1;      /* bit 0: widescreen; bits 1-2: picture scaling style */
    if (w != sent) { sent = w; shell_set_widescreen(w); }
}

__attribute__((export_name("core_raw_input_ptr"))) HostRawInput *core_raw_input_ptr(void) { return &sRaw; }
__attribute__((export_name("core_raw_input_commit"))) void core_raw_input_commit(void) { host_raw_publish(&sRaw); }

/* The shell's own thread (the one that reads the input) enters the core too, and every thread that runs core code needs a shadow stack of its own (see
 * shell/core/core_host.c). This is that stack: the shell asks for its top once, before it calls core_raw_input_commit. */
static unsigned char sShellStack[16384] __attribute__((aligned(16)));
__attribute__((export_name("core_shell_stack_top"))) unsigned int core_shell_stack_top(void) { return (unsigned int)(unsigned long)(sShellStack + sizeof sShellStack); }

#endif
