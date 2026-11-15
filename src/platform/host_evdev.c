/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_evdev.c - gamepads from the Linux event devices. See host_evdev.h.
 *
 * The kernel gives every input device a node /dev/input/eventN. Reading it returns 16-byte events (32-bit program; the kernel uses this size
 * for a 32-bit program also on a 64-bit kernel): seconds, microseconds, type (16 bit), code (16 bit), value (32 bit). The types used:
 *     EV_KEY (1)  a button: value 1 = pressed, 0 = released, 2 = auto-repeat (ignored)
 *     EV_ABS (3)  an axis: value in the range the device reports (EVIOCGABS gives minimum and maximum)
 *     EV_SYN (0)  code SYN_DROPPED (3): the program was too slow and events were lost; the state is read again from the device
 * Codes (linux/input-event-codes.h; copied here because the C library of this build has no Linux kernel headers):
 *   buttons 0x130.. are the "gamepad" buttons: SOUTH(A) EAST(B) NORTH(X) WEST(Y) TL TR TL2 TR2 SELECT START MODE THUMBL THUMBR.
 *   axes: ABS_X 0, ABS_Y 1, ABS_Z 2, ABS_RX 3, ABS_RY 4, ABS_RZ 5, ABS_HAT0X 0x10, ABS_HAT0Y 0x11.
 *
 * Layout choices (verified only against the kernel documentation, not against every pad):
 *   - A device with the A, B, X, Y, TL and TR buttons is a "standard" pad. Its face buttons are reported as Xbox A B X Y by the kernel's own
 *     names (BTN_A, BTN_B, BTN_X, BTN_Y), the D-pad comes from the hat axes or from the BTN_DPAD_* buttons.
 *   - Xbox-style pads: left stick ABS_X/Y, right stick ABS_RX/RY, triggers ABS_Z / ABS_RZ.
 *     Sony pads (USB vendor 054c): right stick ABS_Z/RZ, triggers ABS_RX/RY. (The kernel's drivers report them that way.)
 *   - If the pad has no analog triggers, its TL2/TR2 buttons make the trigger full.
 *   - Anything else with some buttons and two axes is a "generic" device: buttons are numbered in the order of their codes (Button 1 ...),
 *     the first six non-hat axes are X Y Z R U V, the hat axis pair gives the hat.
 * Devices that are not pads (keyboards, mice, touchpads, accelerometers) are recognised by their capability bits and ignored.
 */
#if defined(__linux__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "host_evdev.h"

/* ioctl request numbers: direction (read = 2) in bits 30-31, size in 16-29, type in 8-15, number in 0-7 (x86 layout; _IOC in <asm/ioctl.h>). */
#define IOC_R(type, nr, size) ((int)((2u << 30) | ((unsigned)(size) << 16) | ((unsigned)(type) << 8) | (unsigned)(nr)))
#define EVIOCGID IOC_R('E', 0x02, 8)
#define EVIOCGNAME(len) IOC_R('E', 0x06, len)
#define EVIOCGKEY(len) IOC_R('E', 0x18, len)
#define EVIOCGBIT(ev, len) IOC_R('E', 0x20 + (ev), len)
#define EVIOCGABS(abs) IOC_R('E', 0x40 + (abs), 24)

#define EV_SYN 0
#define EV_KEY 1
#define EV_ABS 3
#define SYN_DROPPED 3

#define BTN_JOYSTICK 0x120
#define BTN_MOUSE 0x110
#define BTN_SOUTH 0x130
#define BTN_EAST 0x131
#define BTN_X 0x133
#define BTN_Y 0x134
#define BTN_TL 0x136
#define BTN_TR 0x137
#define BTN_TL2 0x138
#define BTN_TR2 0x139
#define BTN_SELECT 0x13a
#define BTN_START 0x13b
#define BTN_THUMBL 0x13d
#define BTN_THUMBR 0x13e
#define BTN_DPAD_UP 0x220
#define BTN_DPAD_DOWN 0x221
#define BTN_DPAD_LEFT 0x222
#define BTN_DPAD_RIGHT 0x223
#define KEYS 0x300                                    /* button codes below this are kept */
#define ABS_N 0x18                                    /* axes 0 .. 0x17 are kept (the hat axes are 0x10..0x17) */
#define ABS_HAT0X 0x10
#define ABS_HAT0Y 0x11

#define MAX_DEV 16

typedef struct EvDev {
    int fd;
    char path[40];
    char name[48];
    unsigned vendor;
    int standard;                                     /* 1: the kernel's standard gamepad layout; 0: generic */
    unsigned char key[KEYS];
    unsigned char has_key[KEYS];
    unsigned char has_abs[ABS_N];
    int abs[ABS_N], amin[ABS_N], amax[ABS_N];
} EvDev;

static EvDev *sDev[MAX_DEV];                          /* on the heap: the program image must stay small (tools/native/imagelimit.py) */
static int sCount;
static unsigned sNextScan;
static unsigned sEvents;
static char sRefused[MAX_DEV * 4][40];                /* devices that could not be opened or are not pads: not tried again */
static int sRefusedCount;

static int test_bit(const unsigned char *bits, unsigned n) { return (bits[n >> 3] >> (n & 7)) & 1; }

static int refused_before(const char *path)
{
    int i;
    for (i = 0; i < sRefusedCount; i++) if (strcmp(sRefused[i], path) == 0) return 1;
    return 0;
}

static void refuse(const char *path)
{
    if (sRefusedCount < (int)(sizeof sRefused / sizeof sRefused[0])) snprintf(sRefused[sRefusedCount++], sizeof sRefused[0], "%s", path);
}

/* Reads the current state of all buttons and axes from the device (at open, and after SYN_DROPPED). */
static void read_state(EvDev *d)
{
    unsigned char keys[KEYS / 8];
    unsigned a;
    memset(keys, 0, sizeof keys);
    if (ioctl(d->fd, EVIOCGKEY(sizeof keys), keys) >= 0) {
        unsigned c;
        for (c = 0; c < KEYS; c++) d->key[c] = (unsigned char)test_bit(keys, c);
    }
    for (a = 0; a < ABS_N; a++) {
        int info[6];
        if (!d->has_abs[a]) continue;
        if (ioctl(d->fd, EVIOCGABS(a), info) >= 0) { d->abs[a] = info[0]; d->amin[a] = info[1]; d->amax[a] = info[2]; }
    }
}

static void clean_name(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (; *in && n + 1 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 32 || c > 126 || c == '=' || c == '#') c = ' ';
        if (c == ' ' && (n == 0 || out[n - 1] == ' ')) continue;
        out[n++] = (char)c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
    if (n == 0) snprintf(out, size, "Game controller");
}

/* Opens one event device if it is a gamepad. Returns 1 when added. */
static int try_open(const char *path)
{
    EvDev *d;
    int fd;
    unsigned char evbits[8], keybits[KEYS / 8 + 8], absbits[8];
    char name[128];
    unsigned short id[4];
    unsigned c, a;
    if (sCount >= MAX_DEV) return 0;
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (!refused_before(path)) { fprintf(stderr, "kyujo: gamepads: cannot open %s: %s\n", path, strerror(errno)); refuse(path); }
        return 0;
    }
    memset(evbits, 0, sizeof evbits); memset(keybits, 0, sizeof keybits); memset(absbits, 0, sizeof absbits);
    ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits);
    if (!test_bit(evbits, EV_KEY) || !test_bit(evbits, EV_ABS)) { close(fd); refuse(path); return 0; }
    ioctl(fd, EVIOCGBIT(EV_KEY, KEYS / 8), keybits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
    /* a pad has the gamepad buttons (0x130 ..) or the joystick buttons (0x120 ..), and a first axis pair or a hat; a mouse (BTN_MOUSE) is not one */
    if ((!test_bit(keybits, BTN_SOUTH) && !test_bit(keybits, BTN_JOYSTICK)) || test_bit(keybits, BTN_MOUSE) ||
        (!(test_bit(absbits, 0) && test_bit(absbits, 1)) && !test_bit(absbits, ABS_HAT0X))) {
        close(fd);
        refuse(path);
        return 0;
    }
    d = (EvDev *)calloc(1, sizeof *d);
    if (!d) { close(fd); return 0; }
    sDev[sCount] = d;
    d->fd = fd;
    snprintf(d->path, sizeof d->path, "%s", path);
    memset(name, 0, sizeof name);
    ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
    clean_name(d->name, sizeof d->name, name);
    memset(id, 0, sizeof id);
    ioctl(fd, EVIOCGID, id);
    d->vendor = id[1];
    for (c = 0; c < KEYS; c++) d->has_key[c] = (unsigned char)test_bit(keybits, c);
    for (a = 0; a < ABS_N; a++) d->has_abs[a] = (unsigned char)test_bit(absbits, a);
    d->standard = d->has_key[BTN_SOUTH] && d->has_key[BTN_EAST] && d->has_key[BTN_X] && d->has_key[BTN_Y] && d->has_key[BTN_TL] && d->has_key[BTN_TR];
    read_state(d);
    sCount++;
    fprintf(stderr, "kyujo: gamepads: %s \"%s\" (USB %04x:%04x) connected as a %s pad\n", path, d->name, d->vendor, id[2], d->standard ? "standard (Xbox layout)" : "generic");
    return 1;
}

static void close_dev(int i)
{
    fprintf(stderr, "kyujo: gamepads: \"%s\" (%s) was disconnected\n", sDev[i]->name, sDev[i]->path);
    close(sDev[i]->fd);
    free(sDev[i]);
    sDev[i] = sDev[--sCount];
}

static void scan_dir(void)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *de;
    if (!dir) return;
    while ((de = readdir(dir)) != NULL) {
        char path[40];
        int i, known = 0;
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        snprintf(path, sizeof path, "/dev/input/%.20s", de->d_name);          /* event0 .. event999: far less than 20 characters */
        for (i = 0; i < sCount; i++) if (strcmp(sDev[i]->path, path) == 0) known = 1;
        if (known || refused_before(path)) continue;
        try_open(path);
    }
    closedir(dir);
}

/* Reads all waiting events of one device. Returns 0 if the device is gone. */
static int read_events(EvDev *d)
{
    for (;;) {
        unsigned char buf[16 * 64];
        ssize_t n = read(d->fd, buf, sizeof buf);
        ssize_t i;
        if (n < 0) return errno == EAGAIN || errno == EINTR;
        if (n == 0) return 0;
        for (i = 0; i + 16 <= n; i += 16) {
            unsigned type = (unsigned)buf[i + 8] | ((unsigned)buf[i + 9] << 8), code = (unsigned)buf[i + 10] | ((unsigned)buf[i + 11] << 8);
            int value = (int)((unsigned)buf[i + 12] | ((unsigned)buf[i + 13] << 8) | ((unsigned)buf[i + 14] << 16) | ((unsigned)buf[i + 15] << 24));
            sEvents++;
            if (type == EV_KEY && code < KEYS) d->key[code] = value != 0;
            else if (type == EV_ABS && code < ABS_N) d->abs[code] = value;
            else if (type == EV_SYN && code == SYN_DROPPED) read_state(d);
        }
    }
}

/* An axis value as -32767..32767 around the middle of its range. */
static short stick(const EvDev *d, unsigned a)
{
    int lo = d->amin[a], hi = d->amax[a];
    long mid2 = (long)lo + hi, half2 = (long)hi - lo, v;
    if (half2 <= 0) return 0;
    v = ((long)d->abs[a] * 2 - mid2) * 32767 / half2;
    if (v > 32767) v = 32767;
    if (v < -32767) v = -32767;
    return (short)v;
}

/* An axis that rests at its minimum (a trigger) as 0..255. */
static unsigned char trigger(const EvDev *d, unsigned a)
{
    long span = (long)d->amax[a] - d->amin[a], v;
    if (span <= 0) return 0;
    v = ((long)d->abs[a] - d->amin[a]) * 255 / span;
    return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void fill_standard(const EvDev *d, HostRawPad *p)
{
    int sony = d->vendor == 0x054c;
    unsigned rxa = sony ? 2 : 3, rya = sony ? 5 : 4, lta = sony ? 3 : 2, rta = sony ? 4 : 5;     /* ABS_Z=2 ABS_RX=3 ABS_RY=4 ABS_RZ=5 */
    p->down[PB_A] = d->key[BTN_SOUTH];
    p->down[PB_B] = d->key[BTN_EAST];
    p->down[PB_X] = d->key[BTN_X];
    p->down[PB_Y] = d->key[BTN_Y];
    p->down[PB_LB] = d->key[BTN_TL];
    p->down[PB_RB] = d->key[BTN_TR];
    p->down[PB_BACK] = d->key[BTN_SELECT];
    p->down[PB_START] = d->key[BTN_START];
    p->down[PB_LSTICK] = d->key[BTN_THUMBL];
    p->down[PB_RSTICK] = d->key[BTN_THUMBR];
    p->down[PB_DUP] = d->key[BTN_DPAD_UP] || (d->has_abs[ABS_HAT0Y] && d->abs[ABS_HAT0Y] < 0);
    p->down[PB_DDOWN] = d->key[BTN_DPAD_DOWN] || (d->has_abs[ABS_HAT0Y] && d->abs[ABS_HAT0Y] > 0);
    p->down[PB_DLEFT] = d->key[BTN_DPAD_LEFT] || (d->has_abs[ABS_HAT0X] && d->abs[ABS_HAT0X] < 0);
    p->down[PB_DRIGHT] = d->key[BTN_DPAD_RIGHT] || (d->has_abs[ABS_HAT0X] && d->abs[ABS_HAT0X] > 0);
    if (d->has_abs[0]) p->lx = stick(d, 0);
    if (d->has_abs[1]) p->ly = (short)-stick(d, 1);                 /* the kernel counts Y down; our sticks count up */
    if (d->has_abs[rxa]) p->rx = stick(d, rxa);
    if (d->has_abs[rya]) p->ry = (short)-stick(d, rya);
    if (d->has_abs[lta]) p->lt = trigger(d, lta); else if (d->key[BTN_TL2]) p->lt = 255;
    if (d->has_abs[rta]) p->rt = trigger(d, rta); else if (d->key[BTN_TR2]) p->rt = 255;
}

static void fill_generic(const EvDev *d, HostRawPad *p)
{
    unsigned c, n = 0, a, axes = 0;
    for (c = 0x100; c < KEYS && n < 32; c++) {                     /* the buttons, numbered in the order of their codes */
        if (!d->has_key[c]) continue;
        if (d->key[c]) p->down[PB_G_BTN1 + n] = 1;
        n++;
    }
    p->buttons_total = (unsigned char)n;
    for (a = 0; a < ABS_N && axes < 6; a++) {                      /* the first six axes that are not the hat: X Y Z R U V */
        if (!d->has_abs[a] || (a >= ABS_HAT0X)) continue;
        p->axis[axes++] = stick(d, a);
    }
    if (d->has_abs[ABS_HAT0Y]) { if (d->abs[ABS_HAT0Y] < 0) p->down[PB_G_HAT_UP] = 1; if (d->abs[ABS_HAT0Y] > 0) p->down[PB_G_HAT_DOWN] = 1; }
    if (d->has_abs[ABS_HAT0X]) { if (d->abs[ABS_HAT0X] < 0) p->down[PB_G_HAT_LEFT] = 1; if (d->abs[ABS_HAT0X] > 0) p->down[PB_G_HAT_RIGHT] = 1; }
    p->lx = p->axis[0];
    p->ly = (short)-p->axis[1];
}

void evdev_poll(HostRawInput *raw, unsigned now_ms)
{
    int i, std_slot = 0, gen_slot = 4;
    if ((int)(now_ms - sNextScan) >= 0) { sNextScan = now_ms + 2000; scan_dir(); }
    for (i = 0; i < sCount; ) {
        if (!read_events(sDev[i])) { close_dev(i); continue; }
        i++;
    }
    for (i = 0; i < sCount; i++) {
        const EvDev *d = sDev[i];
        int slot = d->standard ? std_slot : gen_slot, k;
        HostRawPad *p;
        if (slot >= (d->standard ? 4 : HOST_PADS)) continue;       /* more pads than slots: the rest are ignored */
        p = &raw->pad[slot];
        if (d->standard) std_slot++; else gen_slot++;
        p->connected = 1;
        p->generic = d->standard ? 0 : 1;
        snprintf(p->name, sizeof p->name, "%s", d->name);
        for (k = 0; k < slot; k++) {                               /* two pads with the same name: the later one gets " (2)", " (3)" */
            if (raw->pad[k].connected && strcmp(raw->pad[k].name, p->name) == 0) {
                char base[48];
                int n = 2;
                snprintf(base, sizeof base, "%.40s", d->name);
                do { snprintf(p->name, sizeof p->name, "%.40s (%d)", base, n++ % 100); } while (strcmp(raw->pad[k].name, p->name) == 0);
                k = -1;
            }
        }
        if (d->standard) fill_standard(d, p); else fill_generic(d, p);
    }
}

int evdev_count(void) { return sCount; }
unsigned evdev_events_read(void) { return sEvents; }

#endif /* __linux__ */
