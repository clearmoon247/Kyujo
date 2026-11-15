/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_x11.h - a small X11 client that speaks the X protocol directly (no Xlib, no SDL): one window, a picture to show in it, the keyboard.
 *
 * WHY: the Linux build is one static 32-bit program (musl) so that it runs on a 64-bit Linux that has no 32-bit graphics libraries installed
 * (Batocera is such a system: it has the 32-bit loader but no 32-bit Xlib/SDL for a static program to use). The X protocol is a documented byte
 * stream on a socket, so the program talks to the X server itself. Only the few requests below are used.
 *
 * Threading: one X11Win belongs to ONE thread (the window thread in host_window_linux.c). Nothing here locks.
 * HOST-side file (Linux). Tested against Xvfb; see tests/native/x11/.
 */
#ifndef HOST_X11_H
#define HOST_X11_H

typedef struct X11Win X11Win;

/* Connects to the display named by $DISPLAY (using the X authority cookie), creates a window with a client area of w x h pixels, and shows it.
 * fullscreen != 0 asks the window manager for full screen before the window is shown. Returns NULL on failure with a one-line reason in err. */
X11Win *x11win_open(const char *title, unsigned w, unsigned h, int fullscreen, char *err, unsigned errsz);
void x11win_close(X11Win *x);

int x11win_fd(const X11Win *x);                       /* the socket, for poll(): readable when events wait */

/* Handles every event that has arrived (does not block). Returns a set of these bits: */
#define X11W_CLOSE 1                                  /* the window manager asked to close the window, or the connection was lost */
#define X11W_REDRAW 2                                 /* part of the window must be painted again (exposed, resized) */
int x11win_pump(X11Win *x);

void x11win_size(const X11Win *x, unsigned *w, unsigned *h);   /* the current client area */
int x11win_has_focus(const X11Win *x);                /* 1 while the window has the keyboard focus */
/* Sets key[HK_*] = 1 (host_ui.h numbers) for every key that is down right now. Asks the server (one round trip). Meant to be called only while
 * the window has the focus; the caller clears the array first. */
void x11win_keys(X11Win *x, unsigned char key[256]);

/* Paints a finished picture: bgrx is w x h pixels, 4 bytes each (blue, green, red, unused), top row first. It is drawn at the window's top left,
 * 1:1; the caller has already scaled it to the client area (src/render/scale.c). Large pictures are sent in several requests. */
void x11win_present(X11Win *x, const unsigned char *bgrx, unsigned w, unsigned h);

void x11win_set_title(X11Win *x, const char *title);
void x11win_set_fullscreen(X11Win *x, int on);        /* asks the window manager; the state is known once the manager has done it */
int x11win_is_fullscreen(const X11Win *x);
unsigned x11win_screen_width(const X11Win *x);
unsigned x11win_screen_height(const X11Win *x);

/* Where the X authority cookie that was accepted came from (for the log); "" before a connection exists. */
const char *x11win_auth_source(void);

/* How the picture is sent ("PutImage" or "MIT-SHM"), for the log. */
const char *x11win_transport(const X11Win *x);

#endif
