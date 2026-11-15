/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * test_evdev.c - tests the part of src/platform/host_evdev.c that does not need a real input device: decoding of the 16-byte kernel events and
 * the translation of buttons and axes into the HostRawPad of the port, for an Xbox-style pad, a Sony-style pad and a generic joystick.
 *
 * The device descriptions (which buttons and axes exist, their ranges) are what the kernel would report; they are filled in by hand here because
 * this sandbox has no input devices. What this does NOT test: opening /dev/input/event*, the ioctl calls, hot-plugging. Those are tested on
 * the player's machine with the probe program (tools/linux/kyujo_probe.c, "pads").
 *
 * Build and run:  cc -std=gnu11 -I src/platform -o test_evdev tests/native/evdev/test_evdev.c src/platform/host_keys.c && ./test_evdev
 * (host_evdev.c is #included so that the test can reach its static state.)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../../src/platform/host_evdev.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL  " __VA_ARGS__); printf("\n"); } else { printf("PASS  " __VA_ARGS__); printf("\n"); } } while (0)

/* Writes one kernel event (16 bytes: time 8 bytes, type, code, value) into the pipe that stands for the device. */
static void ev(int fd, unsigned type, unsigned code, int value)
{
    unsigned char b[16] = {0};
    b[8] = (unsigned char)type; b[9] = (unsigned char)(type >> 8);
    b[10] = (unsigned char)code; b[11] = (unsigned char)(code >> 8);
    b[12] = (unsigned char)value; b[13] = (unsigned char)(value >> 8); b[14] = (unsigned char)(value >> 16); b[15] = (unsigned char)(value >> 24);
    if (write(fd, b, 16) != 16) { perror("write"); failures++; }
}

/* A device whose events come from a pipe: returns the write end. */
static int add_dev(const char *name, unsigned vendor, int standard)
{
    int p[2];
    EvDev *d = (EvDev *)calloc(1, sizeof *d);
    if (pipe(p) != 0) { perror("pipe"); exit(2); }
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
    d->fd = p[0];
    snprintf(d->path, sizeof d->path, "/dev/input/fake%d", sCount);
    snprintf(d->name, sizeof d->name, "%s", name);
    d->vendor = vendor;
    d->standard = standard;
    sDev[sCount++] = d;
    return p[1];
}

static void axis(EvDev *d, unsigned a, int lo, int hi) { d->has_abs[a] = 1; d->amin[a] = lo; d->amax[a] = hi; d->abs[a] = (lo + hi) / 2; }

int main(void)
{
    static HostRawInput raw;
    int xw, sw, gw;
    EvDev *x, *s, *g;
    unsigned c;

    /* ---- an Xbox-style pad: sticks ABS_X/Y and ABS_RX/RY, triggers ABS_Z / ABS_RZ, D-pad on the hat ---- */
    xw = add_dev("Microsoft X-Box 360 pad", 0x045e, 1);
    x = sDev[0];
    axis(x, 0, -32768, 32767); axis(x, 1, -32768, 32767); axis(x, 3, -32768, 32767); axis(x, 4, -32768, 32767);
    axis(x, 2, 0, 255); axis(x, 5, 0, 255); axis(x, ABS_HAT0X, -1, 1); axis(x, ABS_HAT0Y, -1, 1);
    x->abs[2] = x->abs[5] = 0;
    ev(xw, EV_KEY, BTN_SOUTH, 1); ev(xw, EV_KEY, BTN_X, 1); ev(xw, EV_KEY, BTN_START, 1); ev(xw, EV_KEY, BTN_TR, 1); ev(xw, EV_KEY, BTN_THUMBL, 1);
    ev(xw, EV_ABS, 0, 16384);             /* left stick right, half way */
    ev(xw, EV_ABS, 1, -32768);            /* left stick fully UP (the kernel counts Y downwards) */
    ev(xw, EV_ABS, 3, -32768);            /* right stick fully left */
    ev(xw, EV_ABS, 2, 255);               /* left trigger fully pressed */
    ev(xw, EV_ABS, ABS_HAT0X, -1);        /* D-pad left */
    ev(xw, EV_ABS, ABS_HAT0Y, 1);         /* D-pad down */
    ev(xw, EV_SYN, 0, 0);
    memset(&raw, 0, sizeof raw);
    evdev_poll(&raw, 0);
    CHECK(raw.pad[0].connected && !raw.pad[0].generic, "xbox pad is slot 0, Xbox layout");
    CHECK(strcmp(raw.pad[0].name, "Microsoft X-Box 360 pad") == 0, "name kept: '%s'", raw.pad[0].name);
    CHECK(raw.pad[0].down[PB_A] && raw.pad[0].down[PB_X] && raw.pad[0].down[PB_START] && raw.pad[0].down[PB_RB] && raw.pad[0].down[PB_LSTICK], "buttons A X Start RB L-stick-click down");
    CHECK(!raw.pad[0].down[PB_B] && !raw.pad[0].down[PB_Y] && !raw.pad[0].down[PB_LB], "buttons B Y LB up");
    CHECK(raw.pad[0].lx > 16000 && raw.pad[0].lx < 16500, "left stick x half right: %d", raw.pad[0].lx);
    CHECK(raw.pad[0].ly == 32767, "left stick fully up is +32767 (the kernel's -32768 inverted): %d", raw.pad[0].ly);
    CHECK(raw.pad[0].rx == -32767, "right stick fully left: %d", raw.pad[0].rx);
    CHECK(raw.pad[0].lt == 255 && raw.pad[0].rt == 0, "triggers: left %u right %u", raw.pad[0].lt, raw.pad[0].rt);
    CHECK(raw.pad[0].down[PB_DLEFT] && raw.pad[0].down[PB_DDOWN] && !raw.pad[0].down[PB_DUP] && !raw.pad[0].down[PB_DRIGHT], "hat gives D-pad left and down");
    ev(xw, EV_KEY, BTN_SOUTH, 0); ev(xw, EV_ABS, ABS_HAT0X, 0); ev(xw, EV_ABS, ABS_HAT0Y, 0);
    memset(&raw, 0, sizeof raw);
    evdev_poll(&raw, 1);
    CHECK(!raw.pad[0].down[PB_A] && !raw.pad[0].down[PB_DLEFT] && raw.pad[0].down[PB_X], "release is seen; other buttons stay down");
    CHECK(evdev_events_read() == 15, "events counted: %u", evdev_events_read());

    /* ---- a Sony-style pad: right stick ABS_Z/RZ, triggers ABS_RX/RY, D-pad buttons as hat too ---- */
    sw = add_dev("Wireless Controller", 0x054c, 1);
    s = sDev[1];
    axis(s, 0, 0, 255); axis(s, 1, 0, 255); axis(s, 2, 0, 255); axis(s, 5, 0, 255); axis(s, 3, 0, 255); axis(s, 4, 0, 255);
    s->abs[3] = s->abs[4] = 0;
    ev(sw, EV_ABS, 2, 255);                /* right stick fully right */
    ev(sw, EV_ABS, 4, 255);                /* right trigger fully pressed */
    ev(sw, EV_ABS, 0, 0);                  /* left stick fully left */
    memset(&raw, 0, sizeof raw);
    evdev_poll(&raw, 2);
    CHECK(raw.pad[1].connected && !raw.pad[1].generic, "sony pad is slot 1");
    CHECK(raw.pad[1].rx == 32767, "sony right stick x comes from ABS_Z: %d", raw.pad[1].rx);
    CHECK(raw.pad[1].rt == 255 && raw.pad[1].lt == 0, "sony triggers come from ABS_RX / ABS_RY: left %u right %u", raw.pad[1].lt, raw.pad[1].rt);
    CHECK(raw.pad[1].lx == -32767, "0..255 stick range is centred: left = %d", raw.pad[1].lx);

    /* ---- a pad with no analog triggers: the TL2 / TR2 buttons make the trigger full ---- */
    ev(sw, EV_KEY, BTN_TL2, 1);
    s->has_abs[3] = 0;
    memset(&raw, 0, sizeof raw);
    evdev_poll(&raw, 3);
    CHECK(raw.pad[1].lt == 255, "TL2 button gives a full left trigger when there is no analog one: %u", raw.pad[1].lt);

    /* ---- a generic joystick: buttons numbered in code order, first axes X Y Z, hat ---- */
    gw = add_dev("Generic USB Joystick", 0x1234, 0);
    g = sDev[2];
    g->has_key[0x120] = g->has_key[0x121] = g->has_key[0x123] = 1;       /* trigger, thumb, top: three buttons */
    axis(g, 0, 0, 255); axis(g, 1, 0, 255); axis(g, 2, 0, 255); axis(g, ABS_HAT0X, -1, 1); axis(g, ABS_HAT0Y, -1, 1);
    ev(gw, EV_KEY, 0x123, 1);              /* the third button */
    ev(gw, EV_ABS, 0, 255);                /* X fully right */
    ev(gw, EV_ABS, 1, 0);                  /* Y fully up (kernel: small = up) */
    ev(gw, EV_ABS, ABS_HAT0Y, -1);         /* hat up */
    memset(&raw, 0, sizeof raw);
    evdev_poll(&raw, 4);
    CHECK(raw.pad[4].connected && raw.pad[4].generic, "generic joystick is slot 4, generic layout");
    CHECK(raw.pad[4].buttons_total == 3, "three buttons counted: %u", raw.pad[4].buttons_total);
    CHECK(raw.pad[4].down[PB_G_BTN1 + 2] && !raw.pad[4].down[PB_G_BTN1] && !raw.pad[4].down[PB_G_BTN1 + 1], "code 0x123 is button 3");
    CHECK(raw.pad[4].axis[0] == 32767 && raw.pad[4].axis[1] == -32767, "axes X / Y: %d %d", raw.pad[4].axis[0], raw.pad[4].axis[1]);
    CHECK(raw.pad[4].lx == 32767 && raw.pad[4].ly == 32767, "lx / ly: Y is inverted (up is positive): %d %d", raw.pad[4].lx, raw.pad[4].ly);
    CHECK(raw.pad[4].down[PB_G_HAT_UP] && !raw.pad[4].down[PB_G_HAT_DOWN], "hat up");

    /* ---- two pads with the same name ---- */
    add_dev("Microsoft X-Box 360 pad", 0x045e, 1);
    axis(sDev[3], 0, -32768, 32767);
    memset(&raw, 0, sizeof raw);
    evdev_poll(&raw, 5);
    CHECK(raw.pad[2].connected && strcmp(raw.pad[2].name, "Microsoft X-Box 360 pad (2)") == 0, "second pad with the same name gets (2): '%s'", raw.pad[2].name);

    /* ---- a device that went away ---- */
    close(xw);
    memset(&raw, 0, sizeof raw);
    evdev_poll(&raw, 6);
    CHECK(evdev_count() == 3, "a pad whose device closed (read returns 0) is dropped: %d left", evdev_count());
    (void)c;
    printf("%s\n", failures ? "SOME FAILED" : "ALL PASSED");
    return failures != 0;
}
