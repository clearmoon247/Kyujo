/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * core_host.h - runs the WebAssembly core (build/kyujo.wasm, translated to C by wasm2c) inside a native program.
 *
 * This is the part every 64-bit shell shares (Linux test shell, Nintendo Switch, Android): the instance, the WASI system calls the C library of the
 * core uses (files, clock, arguments), the calls the core makes into the shell (threads, locks, time), and the one lock that keeps game code on one
 * thread at a time. What differs per system is the CoreFront below: how a picture is shown, how sound is played, which folder is the game's "/".
 * See docs/switch-android-plan.md.
 */
#ifndef CORE_HOST_H
#define CORE_HOST_H

#include <stddef.h>
#include "../../src/platform/host_ui.h"

typedef struct CoreFront {
    /* A finished picture, 8-bit RGB, `w` x `h`, top row first. Called by the game's thread; copy it, do not keep the pointer. */
    void (*present)(const unsigned char *rgb, unsigned w, unsigned h, void *user);
    /* `bytes` of 48000 Hz 16-bit little-endian stereo. Called by the game's thread; must not block (drop what cannot be taken). */
    void (*audio)(const void *pcm, unsigned bytes, void *user);
    void (*set_fullscreen)(int on, void *user);                 /* may be NULL */
    void (*set_widescreen)(int on, void *user);                 /* may be NULL: the picture fills the window (1) or keeps 4:3 (0) */
    void (*graphics_go)(void *user);                            /* may be NULL: the game's first-start page is over (or was not needed): the graphics set-up (shader warm-up) may begin */
    void (*set_backdrop)(const unsigned char *rgb, unsigned w, unsigned h, void *user);   /* may be NULL: the game's menu wallpaper tile (RGB; copy it) for the start-up picture */
    /* The system's own file picker for Game Boy files (Android); both may be NULL (= none: the game uses its in-window file browser). pick_gb starts it (kind 0: a ROM and
     * its save, kind 1: a save for the ROM `rom`; port -1 only asks whether there is one) and returns 1 if it is there; pick_gb_poll returns 0 while it is open and 1 when it is
     * over, with the game's path of the file now in use ("" if none) and a short line in words. */
    int (*pick_gb)(int port, int kind, const char *rom, void *user);
    int (*pick_gb_poll)(int *port, int *kind, char *path, unsigned path_size, char *msg, unsigned msg_size, void *user);
    /* The game ended (exit code). If NULL the process exits. A shell that must return to a launcher (Switch: hbmenu) sets this and does not return. */
    void (*exit_hook)(int code, void *user);
    /* "Restart game" (the game has already written its files). Returns 0 if this system cannot start the program again; otherwise it may return 1 or
     * not return at all (the shell ends the game's thread and starts the new copy once it has cleaned up). May be NULL (= not supported). */
    int (*restart)(void *user);
    void *user;
    const char *root_dir;                                       /* the host folder that is "/" for the game (Switch: "sdmc:/") */
    const char *home;                                           /* KYUJO_HOME inside the game, e.g. "/switch/kyujo" */
} CoreFront;

/* Creates the instance. 0 on success. */
int core_init(const CoreFront *front);
/* Runs the game's start-up code and main() on the CALLING thread; returns the exit code when the game ends (or never, if the front's exit_hook does not return). */
int core_run(int argc, char **argv);
/* Throws the instance away and makes it again (the game boots from nothing); every core thread must be gone (core_threads_stop returned 0). 0 on success. */
int core_reset(void);
/* Size of the core's memory in bytes (fixed). */
unsigned long long core_memory_bytes(void);
/* Gives the game a new snapshot of keyboard and gamepads (any thread). */
void mips_native_describe(char *buf, size_t n);     /* where the native MIPS interpreter is (stall report) */
void core_dbg_describe(char *buf, size_t n);        /* the game's scheduler state: signal wait, threads and what each waits for (stall report) */
unsigned core_dbg_progress(void);                   /* a number that changes while the game runs (events delivered to it) */
void core_fn_history(char *buf, size_t n);          /* indexes of the functions the game entered last, newest first (needs --trace-fn) */
void core_profile_start(void);                    /* starts the sampler (a --trace-fn build) */
void core_profile_report(char *buf, size_t n);    /* the most frequent game functions since the last call, "index:per-mille" */
int core_lock_is_busy(void);                      /* for stall reports */
int core_threads_stop(unsigned timeout_ms);      /* the program is leaving: every thread the core started ends; returns how many did not in time */
void core_note(const char *what);                  /* remembers the last notable host call (a string that stays valid) */
const char *core_last_note(unsigned *age_ms);
int core_push_input(const HostRawInput *in);     /* 1 when delivered; 0 when the core was busy (try again later) */
/* A string for an environment variable of the game (before core_run). */
void core_setenv(const char *name, const char *value);
/* Gives the game every KYUJO_* variable of the host environment (`environ`). */
void core_pass_environment(char **env);

#endif
