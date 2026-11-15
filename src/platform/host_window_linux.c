/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_window_linux.c - the player's window on Linux: shows the picture, reads the keyboard and the gamepads.
 *
 * STATUS: tested here only against a virtual X server (Xvfb: window, picture, keys, login by cookie; tests/native/x11) and with fake key presses.
 * Not yet run on a real desktop or on Batocera; the first run there is its first test. Full screen needs a window manager and has not been
 * seen working yet.
 *
 * Same job and the same structure as the Windows version (host_window.c): the window lives on its OWN thread, which is the only user of the X
 * connection; the game thread hands over a finished picture with host_window_present(), the window thread scales it to the window and paints it.
 * Every 10 ms the window thread reads the keyboard (host_x11.c) and the gamepads (host_evdev.c) and publishes the snapshot (host_raw_publish).
 * What a key or button does is decided by the settings and bindings in host_input.c / host_menu.c, exactly as on Windows.
 *
 * When there is no X display (DISPLAY not set, or the connection fails, or KYUJO_HEADLESS is set) the window is simply not there: the
 * program runs headless as before (development frame dump, test scripts). One line on stderr says why.
 *
 * HOST-side file (Linux only; the Windows window is host_window.c).
 */
#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#include "host_ui.h"
#include "host_x11.h"
#include "host_evdev.h"
#include "scale.h"

static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;   /* protects sPixels, sW, sH, sFramePending, sTitleFps - nothing that takes long */
static unsigned char *sPixels;          /* the latest picture as RGB, 3 bytes per pixel, top row first (exactly what vi_scanout produced) */
static unsigned sW, sH, sCap;
static int sFramePending;
static unsigned sTitleFps;
static int sStarted;
static volatile int sState;             /* 0 = not tried, 1 = the window is open, -1 = no window (reason printed) */
static int sWake[2] = {-1, -1};         /* pipe: the game thread writes a byte to wake the window thread */
static volatile int sReqFull = -1;      /* a full screen request from another thread: -1 none, 0 off, 1 on */
static volatile int sIsFull;
static pthread_mutex_t sReadyLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sReady = PTHREAD_COND_INITIALIZER;
static int sReadyFlag;

static unsigned long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

/* ---- devices (window thread only) ---- */

static void poll_devices(X11Win *win)
{
    static HostRawInput raw;            /* static: it is large, and the window thread is the only user */
    static int f11_was, enter_was;
    int alt;
    memset(&raw, 0, sizeof raw);
    if (x11win_has_focus(win)) {        /* keys count only while the window is in front */
        x11win_keys(win, raw.key);
        alt = raw.key[HK_LALT] || raw.key[HK_RALT];
        if (raw.key[HK_F11] && !f11_was) x11win_set_fullscreen(win, !x11win_is_fullscreen(win));
        if (raw.key[HK_ENTER] && alt && !enter_was) x11win_set_fullscreen(win, !x11win_is_fullscreen(win));      /* Alt+Enter */
        f11_was = raw.key[HK_F11];
        enter_was = raw.key[HK_ENTER] && alt;
    } else {
        f11_was = enter_was = 0;
    }
    evdev_poll(&raw, (unsigned)mono_ms());
    host_raw_publish(&raw);
}

/* ---- painting (window thread only) ---- */

static unsigned char *sDraw;            /* private copy of the picture being painted */
static unsigned sDrawCap;
static unsigned char *sOut;             /* the finished window contents, BGRX */
static size_t sOutCap;
static unsigned char *sWork;            /* scratch rows for scale_picture */
static size_t sWorkCap;

/* How long painting takes (scaling to the window + sending it to the X server), so that a slow big screen shows up in the log. Window thread only. */
static unsigned long long sPaintNs, sPaintMaxNs, sPaintCount, sStatsAt;

static void paint_stats(unsigned long long ns, unsigned cw, unsigned ch)
{
    unsigned long long now = host_wall_ns();
    sPaintNs += ns;
    if (ns > sPaintMaxNs) sPaintMaxNs = ns;
    sPaintCount++;
    if (sStatsAt == 0) sStatsAt = now;
    if (now - sStatsAt >= 10000000000ULL) {              /* every 10 s: said only when painting is slow enough to matter (average over 8 ms) */
        if (sPaintCount && sPaintNs / sPaintCount > 8000000ULL)
            fprintf(stderr, "kyujo: window: painting is slow: %llu paints of %ux%u in 10 s, %.1f ms on average, %.1f ms at most\n", sPaintCount, cw, ch,
                    (double)sPaintNs / (double)sPaintCount / 1e6, (double)sPaintMaxNs / 1e6);
        sPaintNs = sPaintMaxNs = sPaintCount = 0;
        sStatsAt = now;
    }
}

static void paint(X11Win *win)
{
    unsigned cw, ch, pw = 0, ph = 0;
    size_t need;
    x11win_size(win, &cw, &ch);
    if (cw == 0 || ch == 0) return;
    /* 1. take a private copy of the latest picture; the game thread is blocked only for this memcpy */
    pthread_mutex_lock(&sLock);
    if (sPixels && sW && sH) {
        size_t n = (size_t)sW * sH * 3u;
        if (n > sDrawCap) {
            free(sDraw);
            sDraw = (unsigned char *)malloc(n);
            sDrawCap = sDraw ? (unsigned)n : 0;
        }
        if (sDraw) { memcpy(sDraw, sPixels, n); pw = sW; ph = sH; }
    }
    pthread_mutex_unlock(&sLock);
    /* 2. build the whole window contents outside the lock */
    need = (size_t)cw * ch * 4u;
    if (need > sOutCap) { free(sOut); sOut = (unsigned char *)malloc(need); sOutCap = sOut ? need : 0; }
    if (!sOut) return;
    if (pw && ph) {
        size_t work = (size_t)cw * 8u;
        if (work > sWorkCap) { free(sWork); sWork = (unsigned char *)malloc(work); sWorkCap = sWork ? work : 0; }
        if (sWork && scale_picture_mode(sDraw, pw, ph, sOut, cw, ch, sWork, host_settings()->widescreen) == 0) {
            x11win_present(win, sOut, cw, ch);
            return;
        }
    }
    memset(sOut, 0, need);                                  /* no picture yet (or no memory): black */
    x11win_present(win, sOut, cw, ch);
}

/* ---- the window thread ---- */

static void signal_ready(int state)
{
    pthread_mutex_lock(&sReadyLock);
    sState = state;
    sReadyFlag = 1;
    pthread_cond_broadcast(&sReady);
    pthread_mutex_unlock(&sReadyLock);
}

static void *window_thread(void *unused)
{
    char err[300];
    X11Win *win;
    unsigned long long next_tick;
    unsigned shown_fps = (unsigned)-1;
    unsigned pw = 0, ph = 0;
    (void)unused;
    win = x11win_open("Pokemon Stadium", 640, 480, host_settings()->fullscreen, err, sizeof err);
    if (!win) {
        fprintf(stderr, "kyujo: window: no window (%s); running without one\n", err);
        signal_ready(-1);
        return NULL;
    }
    fprintf(stderr, "kyujo: window: open on DISPLAY=%s (screen %ux%u, %s, X authority from %s)\n", getenv("DISPLAY"), x11win_screen_width(win),
            x11win_screen_height(win), x11win_transport(win), x11win_auth_source());
    signal_ready(1);
    next_tick = mono_ms();
    for (;;) {
        struct pollfd pf[2];
        unsigned long long now = mono_ms();
        int ev, want_paint = 0, full_req;
        unsigned cw, ch, fps;
        pf[0].fd = x11win_fd(win); pf[0].events = POLLIN; pf[0].revents = 0;
        pf[1].fd = sWake[0]; pf[1].events = POLLIN; pf[1].revents = 0;
        poll(pf, 2, next_tick > now ? (int)(next_tick - now) : 0);
        if (pf[1].revents & POLLIN) { char junk[64]; while (read(sWake[0], junk, sizeof junk) > 0) {} }
        ev = x11win_pump(win);
        if (ev & X11W_CLOSE) host_exit(0);                  /* ends the whole program, game thread included */
        if (ev & X11W_REDRAW) want_paint = 1;
        sIsFull = x11win_is_fullscreen(win);
        full_req = sReqFull;
        if (full_req >= 0) { sReqFull = -1; if (full_req != x11win_is_fullscreen(win)) x11win_set_fullscreen(win, full_req); }
        now = mono_ms();
        if (now >= next_tick) {
            poll_devices(win);
            next_tick = now + 10;
        }
        pthread_mutex_lock(&sLock);
        if (sFramePending) { sFramePending = 0; want_paint = 1; }
        fps = sTitleFps;
        pthread_mutex_unlock(&sLock);
        x11win_size(win, &cw, &ch);
        if (cw != pw || ch != ph) { pw = cw; ph = ch; want_paint = 1; }
        if (want_paint) {
            unsigned long long t0 = host_wall_ns();
            paint(win);
            paint_stats(host_wall_ns() - t0, cw, ch);
        }
        if (fps != shown_fps) {
            char t[128];
            shown_fps = fps;
            snprintf(t, sizeof t, "Pokemon Stadium - %u pictures/s", fps);
            x11win_set_title(win, t);
        }
    }
    return NULL;
}

static void start_window(void)
{
    pthread_t th;
    pthread_attr_t attr;
    struct timespec until;
    const char *why = NULL;
    sStarted = 1;
    if (getenv("KYUJO_HEADLESS") && *getenv("KYUJO_HEADLESS") != '0') why = "KYUJO_HEADLESS is set";
    else if (!getenv("DISPLAY") || !*getenv("DISPLAY")) why = "DISPLAY is not set";
    if (why) {
        fprintf(stderr, "kyujo: window: none (%s); running without one\n", why);
        sState = -1;
        return;
    }
    if (pipe(sWake) == 0) {                                 /* both ends non-blocking: the writer never waits, the reader drains everything */
        int i;
        for (i = 0; i < 2; i++) fcntl(sWake[i], F_SETFL, fcntl(sWake[i], F_GETFL, 0) | O_NONBLOCK);
    } else {
        sWake[0] = sWake[1] = -1;                           /* then the window thread wakes up by its 10 ms timer only */
    }
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    if (pthread_create(&th, &attr, window_thread, NULL) != 0) {
        fprintf(stderr, "kyujo: window: cannot start the window thread (%s); running without a window\n", strerror(errno));
        sState = -1;
        return;
    }
    pthread_detach(th);
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 12;
    pthread_mutex_lock(&sReadyLock);
    while (!sReadyFlag) if (pthread_cond_timedwait(&sReady, &sReadyLock, &until) != 0) break;
    if (!sReadyFlag) { sState = -1; fprintf(stderr, "kyujo: window: the window did not open within 12 seconds; running without one\n"); }
    pthread_mutex_unlock(&sReadyLock);
}

int host_window_wanted(void)
{
    if (!sStarted) start_window();
    return sState == 1;
}

void host_window_prepare(void) { (void)host_window_wanted(); }     /* opens the window now rather than at the first picture */

void host_window_present(const unsigned char *rgb, unsigned w, unsigned h)
{
    static unsigned long long last_ns, count;
    unsigned long long now;
    if (!host_window_wanted() || w == 0 || h == 0) return;
    pthread_mutex_lock(&sLock);
    if ((size_t)w * h * 3 > sCap) {
        free(sPixels);
        sCap = w * h * 3;
        sPixels = (unsigned char *)malloc(sCap);
    }
    if (sPixels) {
        memcpy(sPixels, rgb, (size_t)w * h * 3u);          /* no per-pixel work on the game thread: scale_picture converts to BGRX on the window thread */
        sW = w;
        sH = h;
        sFramePending = 1;
    }
    pthread_mutex_unlock(&sLock);
    if (sWake[1] >= 0) { char b = 1; (void)!write(sWake[1], &b, 1); }
    count++;
    now = host_now_ns();
    if (now - last_ns >= 1000000000ULL) {
        pthread_mutex_lock(&sLock);
        sTitleFps = (unsigned)count;
        pthread_mutex_unlock(&sLock);
        count = 0;
        last_ns = now;
    }
}

void host_window_set_fullscreen(int on)
{
    sReqFull = on != 0;
    if (sWake[1] >= 0) { char b = 1; (void)!write(sWake[1], &b, 1); }
}

int host_window_is_fullscreen(void) { return sIsFull; }

/* No file dialog on Linux: the ROM comes from --rom, KYUJO_ROM or the settings, or from the in-game menu's file browser. */
const char *host_window_pick_rom(char *buf, unsigned size) { (void)buf; (void)size; return NULL; }

#endif /* __linux__ */
