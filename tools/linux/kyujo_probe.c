/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * kyujo_probe.c - a small test program for the Linux version of the port. It checks, one part at a time, what the real game will need:
 *
 *     kyujo_probe x11      a window with a test picture; prints the keys you press, focus changes and size changes
 *     kyujo_probe pads     lists the gamepads it finds and prints the buttons and sticks while you press them
 *     kyujo_probe sound    plays a two-second test tone through PulseAudio / PipeWire
 *     kyujo_probe          all three, one after the other
 *
 * It uses the SAME source files as the game (host_x11.c, host_evdev.c, host_pulse.c), so what works here works in the game.
 * It contains no game code and needs no ROM. Every step prints what it did and, on failure, why.
 * Options: -t SECONDS (how long the x11 and pads checks run, default 10), -f (x11: start in full screen), -s WIDTHxHEIGHT (x11: the window size).
 *
 * HOST-side tool (Linux, 32-bit static).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <math.h>

#include "host_ui.h"
#include "host_x11.h"
#include "host_evdev.h"
#include "host_pulse.h"

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void env_report(void)
{
    const char *names[] = {"DISPLAY", "XAUTHORITY", "HOME", "XDG_RUNTIME_DIR", "PULSE_SERVER", "PULSE_RUNTIME_PATH", "WAYLAND_DISPLAY", "USER"};
    unsigned i;
    printf("environment (uid %d):\n", (int)getuid());
    for (i = 0; i < sizeof names / sizeof names[0]; i++) printf("  %-18s = %s\n", names[i], getenv(names[i]) ? getenv(names[i]) : "(not set)");
}

/* ---- x11 ---- */

/* Fills a w x h picture (B,G,R,X): eight colour bars, a grey ramp under them, and a white box that moves with the time. */
static void draw_test_picture(unsigned char *p, unsigned w, unsigned h, double t)
{
    static const unsigned char bars[8][3] = {{255,255,255},{255,255,0},{0,255,255},{0,255,0},{255,0,255},{255,0,0},{0,0,255},{20,20,20}};
    unsigned x, y, bx = (unsigned)((sin(t * 2.0) * 0.5 + 0.5) * (double)(w > 60 ? w - 60 : 1));
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned char *q = p + ((size_t)y * w + x) * 4;
            unsigned char r, g, b;
            if (y < h * 2 / 3) {
                const unsigned char *c = bars[x * 8 / w];
                r = c[0]; g = c[1]; b = c[2];
            } else {
                r = g = b = (unsigned char)(x * 255 / (w ? w : 1));
            }
            if (x >= bx && x < bx + 60 && y > h / 2 - 30 && y < h / 2 + 30) r = g = b = 255;
            if (x < 4 || y < 4 || x + 4 >= w || y + 4 >= h) { r = 255; g = 0; b = 0; }       /* a red frame: the picture reaches all four edges */
            q[0] = b; q[1] = g; q[2] = r; q[3] = 0;
        }
    }
}

static unsigned sWinW = 640, sWinH = 480;                  /* -s WxH: the window size to test */

static int probe_x11(double seconds, int fullscreen)
{
    char err[256];
    X11Win *x;
    unsigned char *pic = NULL, keys[256], last[256];
    unsigned pw = 0, ph = 0, cw, ch, k;
    double t0 = now_s(), next = 0, send_total = 0;
    unsigned sends = 0;
    int focus = -1, full = -1;
    printf("\n== x11: window and keyboard ==\n");
    x = x11win_open("Kyujo probe", sWinW, sWinH, fullscreen, err, sizeof err);
    if (!x) { printf("FAILED: %s\n", err); return 1; }
    printf("connected. authorization: %s\n", x11win_auth_source());
    printf("screen %ux%u, picture sent with %s\n", x11win_screen_width(x), x11win_screen_height(x), x11win_transport(x));
    printf("A window is open. Press keys (they are printed), F11 = full screen on/off, Esc = end. Running %.0f seconds.\n", seconds);
    memset(last, 0, sizeof last);
    for (;;) {
        double t = now_s() - t0;
        int ev, repaint = 0;
        struct pollfd pf;
        if (t > seconds) break;
        pf.fd = x11win_fd(x); pf.events = POLLIN;
        poll(&pf, 1, 20);
        ev = x11win_pump(x);
        if (ev & X11W_CLOSE) { printf("the window was closed\n"); break; }
        if (ev & X11W_REDRAW) repaint = 1;
        if (x11win_has_focus(x) != focus) { focus = x11win_has_focus(x); printf("keyboard focus: %s\n", focus ? "yes" : "no"); }
        if (x11win_is_fullscreen(x) != full) { full = x11win_is_fullscreen(x); printf("full screen: %s\n", full ? "on" : "off"); }
        memset(keys, 0, sizeof keys);
        if (focus) x11win_keys(x, keys);
        for (k = 1; k < 256; k++) {
            if (keys[k] != last[k]) {
                const char *n = host_key_name((int)k);
                printf("key %-10s %s\n", n ? n : "?", keys[k] ? "down" : "up");
                if (keys[k] && k == HK_F11) x11win_set_fullscreen(x, !x11win_is_fullscreen(x));
                if (keys[k] && k == HK_ESC) seconds = 0;
            }
            last[k] = keys[k];
        }
        x11win_size(x, &cw, &ch);
        if (cw != pw || ch != ph) {
            printf("window size now %ux%u\n", cw, ch);
            free(pic);
            pic = (unsigned char *)malloc((size_t)cw * ch * 4);
            pw = cw; ph = ch;
        }
        if (pic && (repaint || t >= next)) {
            double a;
            draw_test_picture(pic, pw, ph, t);
            a = now_s();
            x11win_present(x, pic, pw, ph);
            send_total += now_s() - a;
            sends++;
            next = t + 0.033;
        }
    }
    x11win_close(x);
    free(pic);
    if (sends) printf("sending a %ux%u picture took %.1f ms on average (%u pictures; about 30 per second are needed, so it must stay well under 33 ms)\n", pw, ph, send_total * 1000.0 / sends, sends);
    printf("x11 check finished\n");
    return 0;
}

/* ---- pads ---- */

static int probe_pads(double seconds)
{
    static HostRawInput raw;
    static HostRawPad shown[HOST_PADS];              /* what has been printed so far, per slot: a change is printed when it differs from this */
    double t0 = now_s();
    unsigned i;
    int b;
    printf("\n== pads: gamepads (evdev) ==\n");
    printf("Press buttons and move sticks on your gamepad(s); changes are printed. Running %.0f seconds.\n", seconds);
    while (now_s() - t0 < seconds) {
        memset(&raw, 0, sizeof raw);
        evdev_poll(&raw, (unsigned)(now_s() * 1000.0));
        for (i = 0; i < HOST_PADS; i++) {
            HostRawPad *p = &raw.pad[i], *o = &shown[i];
            if (p->connected && !o->connected) printf("pad %u connected: \"%s\" (%s layout)\n", i + 1, p->name, p->generic ? "generic" : "Xbox");
            if (!p->connected && o->connected) printf("pad %u disconnected\n", i + 1);
            if (!p->connected) { memset(o, 0, sizeof *o); continue; }
            o->connected = 1;
            for (b = 0; b < PB_COUNT; b++) {
                if (p->down[b] != o->down[b]) { printf("pad %u  %-10s %s\n", i + 1, host_pad_button_name(b), p->down[b] ? "down" : "up"); o->down[b] = p->down[b]; }
            }
            if (abs(p->lx - o->lx) > 4000 || abs(p->ly - o->ly) > 4000 || abs(p->rx - o->rx) > 4000 || abs(p->ry - o->ry) > 4000 ||
                abs((int)p->lt - o->lt) > 40 || abs((int)p->rt - o->rt) > 40) {
                printf("pad %u  left stick %6d %6d   right stick %6d %6d   triggers %3u %3u\n", i + 1, p->lx, p->ly, p->rx, p->ry, p->lt, p->rt);
                o->lx = p->lx; o->ly = p->ly; o->rx = p->rx; o->ry = p->ry; o->lt = p->lt; o->rt = p->rt;
            }
        }
        usleep(10000);
    }
    printf("pads check finished: %d pad(s) open, %u input events read\n", evdev_count(), evdev_events_read());
    if (evdev_count() == 0) printf("(no gamepad was found: if one is plugged in, tell me what 'ls -l /dev/input/' and 'cat /proc/bus/input/devices' show)\n");
    return 0;
}

/* ---- sound ---- */

static int probe_sound(void)
{
    char err[256];
    Pulse *p;
    int16_t buf[2 * 960];
    unsigned frame = 0, sent = 0, dropped = 0;
    double t0, next;
    printf("\n== sound: PulseAudio / PipeWire ==\n");
    p = pulse_open("Kyujo probe", 48000, err, sizeof err);
    if (!p) { printf("FAILED: %s\n", err); return 1; }
    printf("connected to %s, protocol version %u, stream is open. Playing a 440 Hz tone for 2 seconds (you should hear it)...\n", pulse_server_path(p), pulse_version(p));
    t0 = now_s();
    next = t0;
    while (now_s() - t0 < 2.2) {
        unsigned i, n;
        if (now_s() < next) { usleep(2000); continue; }
        for (i = 0; i < 960; i++, frame++) {                              /* 20 ms per block */
            double v = (now_s() - t0 < 2.0) ? sin(2.0 * M_PI * 440.0 * (double)frame / 48000.0) * 9000.0 : 0.0;
            buf[2 * i] = buf[2 * i + 1] = (int16_t)v;
        }
        n = pulse_write(p, buf, sizeof buf);
        sent += n;
        dropped += (unsigned)sizeof buf - n;
        next += 0.020;
    }
    printf("accepted %u bytes, dropped %u bytes; server asked for data %u times; underruns %u\n", sent, dropped, pulse_requests(p), pulse_underruns(p));
    pulse_close(p);
    printf("sound check finished\n");
    return 0;
}

int main(int argc, char **argv)
{
    double seconds = 10;
    int fullscreen = 0, i, want_x = 0, want_pads = 0, want_sound = 0, rc = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "-f") == 0) fullscreen = 1;
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) { if (sscanf(argv[++i], "%ux%u", &sWinW, &sWinH) != 2 || sWinW < 64 || sWinH < 64 || sWinW > 8192 || sWinH > 8192) { fprintf(stderr, "-s needs WIDTHxHEIGHT\n"); return 2; } }
        else if (strcmp(argv[i], "x11") == 0) want_x = 1;
        else if (strcmp(argv[i], "pads") == 0) want_pads = 1;
        else if (strcmp(argv[i], "sound") == 0) want_sound = 1;
        else { fprintf(stderr, "usage: %s [x11] [pads] [sound] [-t SECONDS] [-f]\n", argv[0]); return 2; }
    }
    if (!want_x && !want_pads && !want_sound) want_x = want_pads = want_sound = 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Kyujo Linux probe\n");
    env_report();
    if (want_x) rc |= probe_x11(seconds, fullscreen);
    if (want_pads) rc |= probe_pads(seconds);
    if (want_sound) rc |= probe_sound();
    printf("\nDone. %s\n", rc ? "At least one part FAILED (see above)." : "All parts that ran worked.");
    return rc;
}
