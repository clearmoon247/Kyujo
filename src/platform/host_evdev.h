/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_evdev.h - gamepads on Linux, read from the kernel's event devices (/dev/input/event*). No library is needed.
 *
 * Each call to evdev_poll() reads whatever the pads sent since the last call and fills the pad slots of a HostRawInput (host_ui.h):
 * pads that follow the kernel's standard "gamepad" layout (Xbox, PlayStation, Switch Pro, 8BitDo ...) are reported like Xbox pads in slots 0..3;
 * other joysticks are reported as generic devices (numbered buttons, a hat and axes) in slots 4..7, like the Windows joystick API does.
 * HOST-side file (Linux).
 */
#ifndef HOST_EVDEV_H
#define HOST_EVDEV_H

#include "host_ui.h"

/* Fills raw->pad[] (the caller has cleared it) with the pads that are connected. now_ms is any millisecond clock; the device directory is
 * looked at again about every 2 seconds, so a pad plugged in later is found. Cheap to call every 10 ms. */
void evdev_poll(HostRawInput *raw, unsigned now_ms);

int evdev_count(void);                                /* pads open right now */
unsigned evdev_events_read(void);                     /* input events read so far (a sign that the device and the event layout work) */

#endif
