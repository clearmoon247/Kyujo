/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_window.c - the player's window: shows the picture, reads the keyboard and a gamepad, asks for the ROM file.
 *
 * *** STATUS: this file was only COMPILED (for 32-bit Windows, with clang/mingw-w64 headers) on the development machine. No Windows or
 * *** Wine is available there, so none of it has been run. The first run on Windows is its first test.
 *
 * On Windows (_WIN32) the window lives on its OWN thread (a Win32 window must be created and pumped by one thread), so the game thread
 * never waits for the message loop. The game thread hands over a finished picture with host_window_present(); the window thread paints it.
 * On Linux the window is host_window_linux.c (X11, written for the Batocera port). On other systems the functions below do nothing
 * (the development frame dump in src/render/gfx_host.c is the output there).
 *
 * INPUT: this file only READS the devices and publishes a snapshot (host_ui.h: HostRawInput): which keyboard keys are down (our own key numbers, HK_*;
 * only while the window is in front) and the state of the gamepads: up to four Xbox-layout pads through XInput (PB_* buttons, sticks, triggers) and
 * up to four other pads through the Windows joystick API (winmm: DualShock, DualSense, Switch Pro and most other pads, as "generic" devices: numbered
 * buttons, a hat and six axes). A pad that XInput already reports is skipped in the joystick list (same USB vendor/product, found through Raw Input).
 * What a key or button does is decided by the settings and bindings in host_input.c / host_menu.c (the menu: F1, Esc or the pad's menu button).
 * XInput is loaded at run time (xinput1_4.dll, else xinput9_1_0.dll); without it the keyboard and generic pads still work.
 * The window also has a menu bar (File, Controllers, Game Paks, Options, Help). Its items open the portable in-game menu at the right screen
 * (host_menu_request), so every function exists once, in host_menu.c; a Switch version has no menu bar and uses the in-game menu only.
 * Full screen (F11, Alt+Enter, Options, or fullscreen=1 in the settings) is a borderless window over the monitor.
 *
 * HOST-side file (uses the Windows API on Windows, nothing else).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "host_ui.h"
#include "scale.h"

#ifdef _WIN32

#include <windows.h>
#include <commdlg.h>
#include <xinput.h>
#include <mmsystem.h>

#define WM_PS_FRAME (WM_APP + 1)        /* the game thread has stored a new picture */
#define WM_PS_TITLE (WM_APP + 2)        /* wParam = frames presented in the last second */
#define WM_PS_FULLSCREEN (WM_APP + 3)   /* wParam = 1 on, 0 off (from host_window_set_fullscreen) */

/* Our key numbers (host_ui.h) and the Windows virtual-key codes they come from. Letters and digits are the same number in both. */
static const struct { int hk; int vk; } kKeys[] = {
    {HK_BACKSPACE, VK_BACK}, {HK_TAB, VK_TAB}, {HK_ENTER, VK_RETURN}, {HK_ESC, VK_ESCAPE}, {HK_SPACE, VK_SPACE},
    {HK_UP, VK_UP}, {HK_DOWN, VK_DOWN}, {HK_LEFT, VK_LEFT}, {HK_RIGHT, VK_RIGHT},
    {HK_LSHIFT, VK_LSHIFT}, {HK_RSHIFT, VK_RSHIFT}, {HK_LCTRL, VK_LCONTROL}, {HK_RCTRL, VK_RCONTROL}, {HK_LALT, VK_LMENU}, {HK_RALT, VK_RMENU},
    {HK_INSERT, VK_INSERT}, {HK_DELETE, VK_DELETE}, {HK_HOME, VK_HOME}, {HK_END, VK_END}, {HK_PGUP, VK_PRIOR}, {HK_PGDN, VK_NEXT},
    {HK_F1, VK_F1}, {HK_F2, VK_F2}, {HK_F3, VK_F3}, {HK_F4, VK_F4}, {HK_F5, VK_F5}, {HK_F6, VK_F6},
    {HK_F7, VK_F7}, {HK_F8, VK_F8}, {HK_F9, VK_F9}, {HK_F10, VK_F10}, {HK_F11, VK_F11}, {HK_F12, VK_F12},
    {HK_NUM0, VK_NUMPAD0}, {HK_NUM1, VK_NUMPAD1}, {HK_NUM2, VK_NUMPAD2}, {HK_NUM3, VK_NUMPAD3}, {HK_NUM4, VK_NUMPAD4},
    {HK_NUM5, VK_NUMPAD5}, {HK_NUM6, VK_NUMPAD6}, {HK_NUM7, VK_NUMPAD7}, {HK_NUM8, VK_NUMPAD8}, {HK_NUM9, VK_NUMPAD9},
    {HK_NUMADD, VK_ADD}, {HK_NUMSUB, VK_SUBTRACT}, {HK_NUMMUL, VK_MULTIPLY}, {HK_NUMDIV, VK_DIVIDE}, {HK_NUMDOT, VK_DECIMAL},
    /* HK_NUMENTER has no virtual-key code of its own (Windows reports Return with a flag); it is never seen as pressed here */
    {HK_SEMICOLON, VK_OEM_1}, {HK_QUOTE, VK_OEM_7}, {HK_COMMA, VK_OEM_COMMA}, {HK_PERIOD, VK_OEM_PERIOD}, {HK_SLASH, VK_OEM_2},
    {HK_LBRACKET, VK_OEM_4}, {HK_RBRACKET, VK_OEM_6}, {HK_BACKSLASH, VK_OEM_5}, {HK_MINUS, VK_OEM_MINUS}, {HK_EQUALS, VK_OEM_PLUS},
    {HK_BACKQUOTE, VK_OEM_3},
};

static CRITICAL_SECTION sLock;          /* protects sPixels, sW, sH - and NOTHING that takes long (see paint) */
static unsigned char *sPixels;          /* the latest picture as RGB, 3 bytes per pixel, top row first (exactly what vi_scanout produced) */
static unsigned sW, sH, sCap;
static HWND sWnd;
static HANDLE sReady;
static int sStarted;

static void poll_devices(void);

/* ---- XInput, looked up at run time ---- */
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);
static XInputGetStateFn sXInputGetState;
static int sXInputTried;

static void load_xinput(void)
{
    HMODULE m;
    sXInputTried = 1;
    m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    if (m) sXInputGetState = (XInputGetStateFn)(void *)GetProcAddress(m, "XInputGetState");
    fprintf(stderr, "kyujo: gamepads: XInput %s\n", sXInputGetState ? "is available" : "could not be loaded (Xbox-layout pads are not read)");
}

/* ---- which USB devices XInput already reports, and how many keyboards there are (Raw Input; looked at every 2 seconds) ---- */
static struct { unsigned vid, pid; } sXiIds[32];
static int sXiIdCount;
static unsigned char sKeyboards;
static DWORD sNextHidScan;

static void scan_hid(DWORD now)
{
    static RAWINPUTDEVICELIST list[256];
    UINT n = 256, i, got;
    int kb = 0;
    if ((LONG)(now - sNextHidScan) < 0) return;
    sNextHidScan = now + 2000;
    sXiIdCount = 0;
    got = GetRawInputDeviceList(list, &n, sizeof list[0]);
    if (got == (UINT)-1) return;
    for (i = 0; i < got; i++) {
        char name[256];
        UINT len = sizeof name;
        const char *v, *pd;
        if (list[i].dwType == RIM_TYPEKEYBOARD) { kb++; continue; }
        if (list[i].dwType != RIM_TYPEHID) continue;
        if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICENAME, name, &len) == (UINT)-1) continue;
        if (!strstr(name, "IG_")) continue;                 /* Microsoft's marker for a device that XInput drives */
        v = strstr(name, "VID_");
        pd = strstr(name, "PID_");
        if (v && pd && sXiIdCount < 32) {
            sXiIds[sXiIdCount].vid = (unsigned)strtoul(v + 4, NULL, 16);
            sXiIds[sXiIdCount].pid = (unsigned)strtoul(pd + 4, NULL, 16);
            sXiIdCount++;
        }
    }
    sKeyboards = (unsigned char)(kb > 255 ? 255 : kb);
}

static int is_xinput_device(unsigned vid, unsigned pid)
{
    int i;
    for (i = 0; i < sXiIdCount; i++) if (sXiIds[i].vid == vid && sXiIds[i].pid == pid) return 1;
    return 0;
}

/* ---- the Windows joystick API (winmm), for the pads that are not Xbox layout ---- */
#define JOY_MAX 16
typedef struct JoyState {
    int present;                    /* reported and used */
    int ignored;                    /* XInput already reports it */
    DWORD next_probe;
    char name[48];
    JOYCAPSA caps;
    long rest[6];                   /* the position of every axis when the pad was found (a stick in the middle, a trigger at its end) */
    int calibrated;
} JoyState;
static JoyState sJoy[JOY_MAX];

/* An axis value relative to its rest position, stretched so that the longer side reaches +-32767. */
static short axis_value(DWORD v, long rest, long lo, long hi)
{
    long side = (rest - lo > hi - rest) ? rest - lo : hi - rest, d = (long)v - rest;
    if (side <= 0) return 0;
    d = d * 32767 / side;
    if (d > 32767) d = 32767;
    if (d < -32767) d = -32767;
    return (short)d;
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

/* Reads one joystick id into `p`; returns 1 when the pad is connected and usable. */
static int poll_joystick(int id, DWORD now, HostRawPad *p)
{
    JoyState *j = &sJoy[id];
    JOYINFOEX ji;
    int i;
    if (!j->present && (LONG)(now - j->next_probe) < 0) return 0;
    memset(&ji, 0, sizeof ji);
    ji.dwSize = sizeof ji;
    ji.dwFlags = JOY_RETURNALL;
    if (joyGetPosEx((UINT)id, &ji) != JOYERR_NOERROR) {
        if (j->present) fprintf(stderr, "kyujo: gamepads: '%s' (joystick %d) was disconnected\n", j->name, id);
        j->present = 0;
        j->calibrated = 0;
        j->next_probe = now + 1000;
        return 0;
    }
    if (!j->present) {
        if (joyGetDevCapsA((UINT)id, &j->caps, sizeof j->caps) != JOYERR_NOERROR) { j->next_probe = now + 1000; return 0; }
        if (is_xinput_device(j->caps.wMid, j->caps.wPid)) {
            if (!j->ignored) fprintf(stderr, "kyujo: gamepads: joystick %d '%s' (USB %04X:%04X) is an XInput device and is read through XInput\n", id, j->caps.szPname, j->caps.wMid, j->caps.wPid);
            j->ignored = 1;
            j->next_probe = now + 5000;
            return 0;
        }
        j->ignored = 0;
        clean_name(j->name, sizeof j->name, j->caps.szPname);
        j->rest[0] = ji.dwXpos; j->rest[1] = ji.dwYpos; j->rest[2] = ji.dwZpos; j->rest[3] = ji.dwRpos; j->rest[4] = ji.dwUpos; j->rest[5] = ji.dwVpos;
        j->calibrated = 1;
        j->present = 1;
        fprintf(stderr, "kyujo: gamepads: joystick %d '%s' (USB %04X:%04X) connected: %u buttons, %u axes%s\n", id, j->name, j->caps.wMid, j->caps.wPid,
                (unsigned)j->caps.wNumButtons, (unsigned)j->caps.wNumAxes, (j->caps.wCaps & JOYCAPS_HASPOV) ? ", hat switch" : "");
    }
    p->connected = 1;
    p->generic = 1;
    snprintf(p->name, sizeof p->name, "%s", j->name);
    p->buttons_total = (unsigned char)(j->caps.wNumButtons > 32 ? 32 : j->caps.wNumButtons);
    for (i = 0; i < 32; i++) if (ji.dwButtons >> i & 1u) p->down[PB_G_BTN1 + i] = 1;
    if ((j->caps.wCaps & JOYCAPS_HASPOV) && (ji.dwPOV & 0xFFFFu) != 0xFFFFu) {
        DWORD pov = ji.dwPOV & 0xFFFFu;                              /* hundredths of a degree, 0 = up, clockwise */
        if (pov >= 31500 || pov <= 4500) p->down[PB_G_HAT_UP] = 1;
        if (pov >= 4500 && pov <= 13500) p->down[PB_G_HAT_RIGHT] = 1;
        if (pov >= 13500 && pov <= 22500) p->down[PB_G_HAT_DOWN] = 1;
        if (pov >= 22500 && pov <= 31500) p->down[PB_G_HAT_LEFT] = 1;
    }
    p->axis[0] = axis_value(ji.dwXpos, j->rest[0], (long)j->caps.wXmin, (long)j->caps.wXmax);
    p->axis[1] = axis_value(ji.dwYpos, j->rest[1], (long)j->caps.wYmin, (long)j->caps.wYmax);
    if (j->caps.wCaps & JOYCAPS_HASZ) p->axis[2] = axis_value(ji.dwZpos, j->rest[2], (long)j->caps.wZmin, (long)j->caps.wZmax);
    if (j->caps.wCaps & JOYCAPS_HASR) p->axis[3] = axis_value(ji.dwRpos, j->rest[3], (long)j->caps.wRmin, (long)j->caps.wRmax);
    if (j->caps.wCaps & JOYCAPS_HASU) p->axis[4] = axis_value(ji.dwUpos, j->rest[4], (long)j->caps.wUmin, (long)j->caps.wUmax);
    if (j->caps.wCaps & JOYCAPS_HASV) p->axis[5] = axis_value(ji.dwVpos, j->rest[5], (long)j->caps.wVmin, (long)j->caps.wVmax);
    p->lx = p->axis[0];
    p->ly = (short)-p->axis[1];                                      /* the joystick API counts Y down; our sticks count up */
    return 1;
}

/* Reads the keyboard and the gamepads and publishes the snapshot. Called on the window thread only (every 10 ms). */
static void poll_devices(void)
{
    static HostRawInput raw;                        /* static: it is large, and the window thread is the only user */
    static DWORD next_probe[4];                     /* XInput is slow for a slot with no pad: such a slot is only looked at about once a second */
    static unsigned char was_connected[4];
    int i, generic_slot;
    UINT joys;
    DWORD now = GetTickCount();

    memset(&raw, 0, sizeof raw);
    scan_hid(now);
    raw.keyboards = sKeyboards;
    if (GetForegroundWindow() == sWnd) {            /* keys count only while the window is in front */
        for (i = 'A'; i <= 'Z'; i++) if (GetAsyncKeyState(i) & 0x8000) raw.key[i] = 1;
        for (i = '0'; i <= '9'; i++) if (GetAsyncKeyState(i) & 0x8000) raw.key[i] = 1;
        for (i = 0; i < (int)(sizeof kKeys / sizeof kKeys[0]); i++) if (GetAsyncKeyState(kKeys[i].vk) & 0x8000) raw.key[kKeys[i].hk] = 1;
    }
    if (!sXInputTried) load_xinput();
    for (i = 0; sXInputGetState && i < 4; i++) {
        XINPUT_STATE xs;
        const XINPUT_GAMEPAD *g;
        HostRawPad *p = &raw.pad[i];
        if (!was_connected[i] && (LONG)(now - next_probe[i]) < 0) continue;
        if (sXInputGetState((DWORD)i, &xs) != ERROR_SUCCESS) {
            if (was_connected[i]) fprintf(stderr, "kyujo: gamepads: XInput controller %d was disconnected\n", i + 1);
            was_connected[i] = 0;
            next_probe[i] = now + 1000;
            continue;
        }
        if (!was_connected[i]) fprintf(stderr, "kyujo: gamepads: XInput controller %d connected\n", i + 1);
        was_connected[i] = 1;
        g = &xs.Gamepad;
        p->connected = 1;
        snprintf(p->name, sizeof p->name, "XInput controller %d", i + 1);
        if (g->wButtons & XINPUT_GAMEPAD_A) p->down[PB_A] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_B) p->down[PB_B] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_X) p->down[PB_X] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_Y) p->down[PB_Y] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) p->down[PB_LB] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) p->down[PB_RB] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_BACK) p->down[PB_BACK] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_START) p->down[PB_START] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_LEFT_THUMB) p->down[PB_LSTICK] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) p->down[PB_RSTICK] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_DPAD_UP) p->down[PB_DUP] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) p->down[PB_DDOWN] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) p->down[PB_DLEFT] = 1;
        if (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) p->down[PB_DRIGHT] = 1;
        p->lx = g->sThumbLX; p->ly = g->sThumbLY; p->rx = g->sThumbRX; p->ry = g->sThumbRY;
        p->lt = g->bLeftTrigger; p->rt = g->bRightTrigger;
    }
    joys = joyGetNumDevs();
    if (joys > JOY_MAX) joys = JOY_MAX;
    generic_slot = 4;
    for (i = 0; i < (int)joys && generic_slot < HOST_PADS; i++) {
        HostRawPad *p = &raw.pad[generic_slot];
        int k;
        if (!poll_joystick(i, now, p)) continue;
        for (k = 4; k < generic_slot; k++) {                         /* two pads with the same name: the later one gets " (2)", " (3)" */
            if (strcmp(raw.pad[k].name, p->name) == 0) {
                char base[48];
                int n = 2;
                snprintf(base, sizeof base, "%.40s", p->name);
                do { snprintf(p->name, sizeof p->name, "%s (%d)", base, n++); } while (strcmp(raw.pad[k].name, p->name) == 0);
                k = 3;
            }
        }
        generic_slot++;
    }
    host_raw_publish(&raw);
}

/* The polling runs on its own thread. It used to be a 10 ms timer of the window thread, and one slow call there (XInput looking at the slots that have no pad,
 * the Raw Input device list) held every paint behind it: the first log with timings showed a 21 ms poll, longer than a video field, which is a late picture
 * (a visible hitch) about once a second. The thread is the only caller of poll_devices and of host_raw_publish (a single-writer seqlock). */
static volatile LONG sPollMaxUs;                    /* the slowest poll since the perf line was last written (reported with the paint timings) */
static DWORD WINAPI input_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        unsigned long long t0 = host_wall_ns();
        LONG us;
        poll_devices();
        us = (LONG)((host_wall_ns() - t0) / 1000ULL);
        if (us > sPollMaxUs) InterlockedExchange(&sPollMaxUs, us);
        Sleep(10);
    }
    return 0;
}
static void start_input_thread(void)
{
    HANDLE h = CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else fprintf(stderr, "kyujo: cannot start the input thread (error %lu)\n", GetLastError());
}

/* ---- painting ---- */
/*
 * What the first Windows test found, and what is done about it (all UNVERIFIED on Windows: only compiled here):
 *  - "visual blinking": the old paint() blacked out the client area on the window's DC and then stretched the picture over it, so the screen
 *    showed black for a moment on every picture. Now the complete window contents (black bars + picture) are built in ONE memory buffer and
 *    sent to the window with a single StretchDIBits call at 1:1 size, so the screen only ever receives finished pictures.
 *  - lag everywhere: the old paint() held the lock that the GAME thread needs to hand over its next picture while it ran a slow HALFTONE
 *    stretch, so a slow paint stalled the game. Now the lock is held only for a memory copy of the picture; scaling and drawing happen
 *    outside it.
 *  - blurry battle text: HALFTONE enlarges by plain interpolation. The picture is now scaled by src/render/scale.c ("sharp bilinear", tested
 *    on Linux by tests/native/render/scale_check.c), which keeps texels crisp.
 */
static unsigned char *sDraw;            /* private copy of the picture being painted (window thread only) */
static unsigned sDrawCap;
static unsigned char *sOut;             /* the finished window contents, BGRX (window thread only) */
static size_t sOutCap;
static unsigned char *sWork;            /* scratch rows for scale_picture */
static size_t sWorkCap;

/* How long the window thread's paints take (reported in the perf line, host_perf.c): a slow paint or a long wait for one shows as stutter that the game's own
 * numbers do not see. Written by the window thread, read and reset by the game thread. */
static volatile LONG sPaintCount, sPaintSumUs, sPaintMaxUs;
int host_window_paint_stats(unsigned *count, unsigned *sum_us, unsigned *max_us, unsigned *poll_max_us)
{
    *poll_max_us = (unsigned)InterlockedExchange(&sPollMaxUs, 0);
    *count = (unsigned)InterlockedExchange(&sPaintCount, 0);
    *sum_us = (unsigned)InterlockedExchange(&sPaintSumUs, 0);
    *max_us = (unsigned)InterlockedExchange(&sPaintMaxUs, 0);
    return 1;
}

static void paint_untimed(HWND hwnd);
static void paint(HWND hwnd)
{
    unsigned long long t0 = host_wall_ns();
    LONG us;
    paint_untimed(hwnd);
    us = (LONG)((host_wall_ns() - t0) / 1000ULL);
    InterlockedIncrement(&sPaintCount);
    InterlockedExchangeAdd(&sPaintSumUs, us);
    if (us > sPaintMaxUs) InterlockedExchange(&sPaintMaxUs, us);
}

static void paint_untimed(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    unsigned cw, ch, pw = 0, ph = 0;

    GetClientRect(hwnd, &rc);
    cw = (unsigned)(rc.right - rc.left);
    ch = (unsigned)(rc.bottom - rc.top);
    if (cw == 0 || ch == 0) { EndPaint(hwnd, &ps); return; }

    /* 1. take a private copy of the latest picture; the game thread is blocked only for this memcpy */
    EnterCriticalSection(&sLock);
    if (sPixels && sW && sH) {
        size_t n = (size_t)sW * sH * 3u;
        if (n > sDrawCap) {
            free(sDraw);
            sDraw = (unsigned char *)malloc(n);
            sDrawCap = sDraw ? (unsigned)n : 0;
        }
        if (sDraw) { memcpy(sDraw, sPixels, n); pw = sW; ph = sH; }
    }
    LeaveCriticalSection(&sLock);

    /* 2. build the whole window contents outside the lock */
    if (pw && ph) {
        size_t need = (size_t)cw * ch * 4u, work = (size_t)cw * 8u;
        if (need > sOutCap) { free(sOut); sOut = (unsigned char *)malloc(need); sOutCap = sOut ? need : 0; }
        if (work > sWorkCap) { free(sWork); sWork = (unsigned char *)malloc(work); sWorkCap = sWork ? work : 0; }
        if (sOut && sWork && scale_picture_mode(sDraw, pw, ph, sOut, cw, ch, sWork, host_settings()->widescreen) == 0) {
            BITMAPINFO bi;
            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = (LONG)cw;
            bi.bmiHeader.biHeight = -(LONG)ch;          /* negative: the first row in memory is the top row */
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            /* 1:1 (no scaling by Windows): source and destination rectangles are the whole client area */
            StretchDIBits(dc, 0, 0, (int)cw, (int)ch, 0, 0, (int)cw, (int)ch, sOut, &bi, DIB_RGB_COLORS, SRCCOPY);
            EndPaint(hwnd, &ps);
            return;
        }
    }
    PatBlt(dc, 0, 0, (int)cw, (int)ch, BLACKNESS);      /* no picture yet (or no memory): black */
    EndPaint(hwnd, &ps);
}

/* ---- the menu bar and full screen ---- */
enum {                                                  /* menu item ids (WM_COMMAND) */
    IDM_FIRST = 100,
    IDM_GAMEROM = IDM_FIRST, IDM_RESTART, IDM_EXIT,
    IDM_CONTROLLERS,
    IDM_PAK1, IDM_PAK2, IDM_PAK3, IDM_PAK4,
    IDM_FULLSCREEN, IDM_OPTIONS,
    IDM_ABOUT
};
static HMENU sMenuBar;
static volatile LONG sFull;                              /* 1 while in full screen */
static WINDOWPLACEMENT sSavedPlace;
static LONG sSavedStyle;

static HMENU build_menu_bar(void)
{
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), ctl = CreatePopupMenu(), paks = CreatePopupMenu(), opt = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuA(file, MF_STRING, IDM_GAMEROM, "Pokemon Stadium &ROM and save file...");
    AppendMenuA(file, MF_STRING, IDM_RESTART, "&Restart game...");
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, IDM_EXIT, "E&xit");
    AppendMenuA(ctl, MF_STRING, IDM_CONTROLLERS, "&Controllers, gamepads and key assignment...");
    AppendMenuA(paks, MF_STRING, IDM_PAK1, "Controller &1 Transfer Pak...");
    AppendMenuA(paks, MF_STRING, IDM_PAK2, "Controller &2 Transfer Pak...");
    AppendMenuA(paks, MF_STRING, IDM_PAK3, "Controller &3 Transfer Pak...");
    AppendMenuA(paks, MF_STRING, IDM_PAK4, "Controller &4 Transfer Pak...");
    AppendMenuA(opt, MF_STRING, IDM_FULLSCREEN, "&Full screen\tF11");
    AppendMenuA(opt, MF_STRING, IDM_OPTIONS, "&Sound, cheat and other options...");
    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About and where the files are...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)file, "&File");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)ctl, "&Controllers");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)paks, "&Game Paks");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)opt, "&Options");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    return bar;
}

/* Window thread only. Full screen is a borderless window over the whole monitor the window is on; the menu bar is hidden meanwhile. */
static void set_fullscreen_now(int on)
{
    if (!sWnd || (on != 0) == (sFull != 0)) return;
    if (on) {
        MONITORINFO mi;
        HMONITOR mon = MonitorFromWindow(sWnd, MONITOR_DEFAULTTONEAREST);
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        sSavedPlace.length = sizeof sSavedPlace;
        GetWindowPlacement(sWnd, &sSavedPlace);
        sSavedStyle = GetWindowLongA(sWnd, GWL_STYLE);
        if (!GetMonitorInfoA(mon, &mi)) return;
        SetMenu(sWnd, NULL);
        SetWindowLongA(sWnd, GWL_STYLE, (LONG)(WS_POPUP | WS_VISIBLE));
        SetWindowPos(sWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        InterlockedExchange(&sFull, 1);
    } else {
        InterlockedExchange(&sFull, 0);
        SetWindowLongA(sWnd, GWL_STYLE, sSavedStyle ? sSavedStyle : (LONG)(WS_OVERLAPPEDWINDOW | WS_VISIBLE));
        SetMenu(sWnd, sMenuBar);
        if (sSavedPlace.length) SetWindowPlacement(sWnd, &sSavedPlace);
        SetWindowPos(sWnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    InvalidateRect(sWnd, NULL, FALSE);
}

void host_window_set_fullscreen(int on) { if (sWnd) PostMessageA(sWnd, WM_PS_FULLSCREEN, (WPARAM)(on != 0), 0); }
int host_window_is_fullscreen(void) { return sFull != 0; }

static void menu_command(int id)
{
    switch (id) {
    case IDM_GAMEROM: host_menu_request(HM_GAMEROM); break;
    case IDM_RESTART: host_menu_request(HM_RESTART); break;
    case IDM_EXIT: PostMessageA(sWnd, WM_CLOSE, 0, 0); break;
    case IDM_CONTROLLERS: host_menu_request(HM_CONTROLLERS); break;
    case IDM_PAK1: case IDM_PAK2: case IDM_PAK3: case IDM_PAK4: host_menu_request(HM_PAK1 + (id - IDM_PAK1)); break;
    case IDM_FULLSCREEN: set_fullscreen_now(!sFull); break;
    case IDM_OPTIONS: host_menu_request(HM_OPTIONS); break;
    case IDM_ABOUT: host_menu_request(HM_ABOUT); break;
    default: break;
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;                                   /* paint() draws everything; avoids flicker */
    case WM_PS_FRAME:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PS_FULLSCREEN:
        set_fullscreen_now((int)wp);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == 0) menu_command((int)LOWORD(wp));
        return 0;
    case WM_INITMENUPOPUP:                          /* the check mark of "Full screen" */
        CheckMenuItem(sMenuBar, IDM_FULLSCREEN, MF_BYCOMMAND | (sFull ? MF_CHECKED : MF_UNCHECKED));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F11) { set_fullscreen_now(!sFull); return 0; }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { set_fullscreen_now(!sFull); return 0; }       /* Alt+Enter */
        break;
    case WM_SETCURSOR:
        if (sFull && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }                  /* no mouse pointer over the full screen picture */
        break;
    case WM_PS_TITLE: {
        char t[128];
        snprintf(t, sizeof t, "Pokemon Stadium - %u pictures/s", (unsigned)wp);
        SetWindowTextA(hwnd, t);
        return 0;
    }
    case WM_KILLFOCUS:
        return 0;                                   /* the input thread sees at its next poll (10 ms) that the window is no longer in front and releases all keys */
    case WM_CLOSE:
        host_exit(0);                               /* ends the whole program, game thread included */
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD WINAPI window_thread(LPVOID unused)
{
    WNDCLASSA wc;
    RECT r = {0, 0, 640, 480};
    MSG m;
    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "KyujoSourcePort";
    if (!RegisterClassA(&wc)) {
        fprintf(stderr, "kyujo: RegisterClass failed (error %lu); running without a window\n", GetLastError());
        SetEvent(sReady);
        return 1;
    }
    sMenuBar = build_menu_bar();
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);                 /* TRUE: the window has a menu bar, the picture area stays 640 x 480 */
    sWnd = CreateWindowExA(0, wc.lpszClassName, "Pokemon Stadium", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, sMenuBar, wc.hInstance, NULL);
    if (!sWnd) fprintf(stderr, "kyujo: CreateWindow failed (error %lu); running without a window\n", GetLastError());
    else {
        start_input_thread();                       /* keyboard and pad polling every 10 ms, off the window thread */
        /* A copy started by "Restart game" is allowed to take the front (host_restart called AllowSetForegroundWindow); a copy started by double click
         * is in front anyway. If Windows refuses, the window is at least raised above the others for a moment. */
        BringWindowToTop(sWnd);
        if (!SetForegroundWindow(sWnd)) {
            SetWindowPos(sWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetWindowPos(sWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            fprintf(stderr, "kyujo: window: Windows did not let the window take the front\n");
        }
        if (host_settings()->fullscreen) set_fullscreen_now(1);
    }
    SetEvent(sReady);
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}

static void start_window(void)
{
    sStarted = 1;
    InitializeCriticalSection(&sLock);
    sReady = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!CreateThread(NULL, 0, window_thread, NULL, 0, NULL)) {
        fprintf(stderr, "kyujo: cannot start the window thread (error %lu)\n", GetLastError());
        return;
    }
    WaitForSingleObject(sReady, 10000);             /* the first picture is stored only after the window exists */
}

int host_window_wanted(void) { return 1; }
void host_window_prepare(void)
{
    unsigned long long t0;
    if (sStarted) return;
    t0 = host_wall_ns();
    start_window();
    fprintf(stderr, "kyujo: window: created in %u ms (before the game starts, so the first picture does not wait for it)\n", (unsigned)((host_wall_ns() - t0) / 1000000ull));
}

void host_window_present(const unsigned char *rgb, unsigned w, unsigned h)
{
    static unsigned long long last_ns, count;
    unsigned long long now;
    if (!sStarted) start_window();
    if (!sWnd || w == 0 || h == 0) return;
    EnterCriticalSection(&sLock);
    if (w * h * 3 > sCap) {
        free(sPixels);
        sCap = w * h * 3;
        sPixels = (unsigned char *)malloc(sCap);
    }
    if (sPixels) {
        memcpy(sPixels, rgb, (size_t)w * h * 3u);          /* no per-pixel work on the game thread: scale_picture converts to BGRX on the window thread */
        sW = w;
        sH = h;
    }
    LeaveCriticalSection(&sLock);
    PostMessageA(sWnd, WM_PS_FRAME, 0, 0);
    count++;
    now = host_now_ns();
    if (now - last_ns >= 1000000000ULL) {
        PostMessageA(sWnd, WM_PS_TITLE, (WPARAM)count, 0);
        count = 0;
        last_ns = now;
    }
}

/* Asks for the ROM with the standard "Open" dialog. Returns `buf` or NULL when cancelled. */
const char *host_window_pick_rom(char *buf, unsigned size)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    buf[0] = 0;
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "N64 ROM (*.z64;*.n64;*.v64)\0*.z64;*.n64;*.v64\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = size;
    ofn.lpstrTitle = "Select your Pokemon Stadium (USA) ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? buf : NULL;
}

#elif !defined(__linux__)   /* neither Windows nor Linux: no window (Linux: host_window_linux.c) */

void host_window_present(const unsigned char *rgb, unsigned w, unsigned h) { (void)rgb; (void)w; (void)h; }
void host_window_prepare(void) {}
const char *host_window_pick_rom(char *buf, unsigned size) { (void)buf; (void)size; return NULL; }
void host_window_set_fullscreen(int on) { (void)on; }
int host_window_is_fullscreen(void) { return 0; }
int host_window_wanted(void) { return 0; }

#endif
