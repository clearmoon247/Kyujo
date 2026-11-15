/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * ui_test_host.c - HOST-side checks of the menu's non-visual parts: the settings file (parse, format, round trip, bad input), the
 * key/gamepad bindings (host_input_map: default keys, rebinding, two alternatives, stick, dead zone, ports) and the path helpers of the file
 * browser. Linked with host_settings.c, host_keys.c, host_input.c, host_paths.c and host_cheat.c; the few things those need from the rest of the
 * program are stubbed below. Prints one line per check that fails and "UI TEST PASSED: checks=N" (exit 0) otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "host_ui.h"

/* ---- stubs for what the other parts of the program provide ---- */
void host_fatal(const char *m) { fprintf(stderr, "FATAL: %s\n", m); exit(2); }
int host_menu_is_open(void) { return 0; }
int host_gbpak_present(int channel) { (void)channel; return 0; }
unsigned long long host_now_ns(void) { return 0; }
unsigned long long host_wall_ns(void) { return 0; }
void host_exit(int status) { exit(status); }
void host_run_exit_hook(void) {}
void host_menu_notice(const char *text) { (void)text; }
unsigned int host_gfx_frame_count(void) { return 0; }

static int sChecks, sFailed;
#define CHECK(cond) do { sChecks++; if (!(cond)) { sFailed++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static unsigned short map_keys(const HostSettings *s, const int *keys, int n, signed char *sx, signed char *sy, int port)
{
    HostRawInput raw;
    unsigned short b;
    int conn, i;
    memset(&raw, 0, sizeof raw);
    for (i = 0; i < n; i++) raw.key[keys[i]] = 1;
    host_input_map(s, &raw, port, &b, sx, sy, &conn);
    return b;
}

int main(void)
{
    HostSettings s, t;
    char text[65536], text2[65536];
    signed char sx, sy;
    int keys[4];

    /* defaults: the documented keys (README) */
    host_settings_defaults(&s);
    CHECK(s.cheats == 1);
    CHECK(s.cheat_hof == 0 && s.cheat_cpuvs == 0);                                       /* the two menu cheats (patches 0039, 0040) are off by default */
    CHECK(s.keyboard_port == 1);
    keys[0] = 'X';  CHECK(map_keys(&s, keys, 1, &sx, &sy, 1) == 0x8000);                /* X = A */
    keys[0] = HK_SPACE; CHECK(map_keys(&s, keys, 1, &sx, &sy, 1) == 0x8000);            /* Space = A too */
    keys[0] = 'Q'; keys[1] = 'E'; CHECK(map_keys(&s, keys, 2, &sx, &sy, 1) == 0x0030);   /* Q + E = L + R: the cheat chord */
    keys[0] = HK_ENTER; CHECK(map_keys(&s, keys, 1, &sx, &sy, 1) == 0x1000);            /* Enter = START */
    keys[0] = HK_UP; CHECK(map_keys(&s, keys, 1, &sx, &sy, 1) == 0x0800 && sy > 0 && sx == 0);  /* arrow up = stick up, which also presses D-up (stick_dpad) */
    keys[0] = HK_LEFT; keys[1] = HK_UP; map_keys(&s, keys, 2, &sx, &sy, 1); CHECK(sx < 0 && sy > 0);
    keys[0] = 'X'; CHECK(map_keys(&s, keys, 1, &sx, &sy, 2) == 0);                      /* the keyboard drives port 1 only */

    /* rebinding and two alternatives */
    s.key[ACT_A][0] = 'M'; s.key[ACT_A][1] = HK_NONE;
    keys[0] = 'M'; CHECK(map_keys(&s, keys, 1, &sx, &sy, 1) == 0x8000);
    keys[0] = 'X'; CHECK(map_keys(&s, keys, 1, &sx, &sy, 1) == 0);
    s.keyboard_port = 3;
    keys[0] = 'M'; CHECK(map_keys(&s, keys, 1, &sx, &sy, 3) == 0x8000 && map_keys(&s, keys, 1, &sx, &sy, 1) == 0);

    /* settings text: round trip, with changed values and paths that contain spaces and '=' */
    host_settings_defaults(&s);
    s.cheats = 0;
    snprintf(s.rom, sizeof s.rom, "C:\\Games\\My Roms\\stadium=1.z64");
    snprintf(s.gb_rom[1], sizeof s.gb_rom[1], "/home/x y/red.gb");
    snprintf(s.gb_save[1], sizeof s.gb_save[1], "/home/x y/red.sav");
    s.key[ACT_START][0] = HK_F5; s.key[ACT_START][1] = 'P';
    s.fullscreen = 1; s.volume = 40; s.deadzone = 33; s.keyboard_port = 0;
    snprintf(s.profile[0].name, sizeof s.profile[0].name, "XInput controller 1");
    host_profile_set_defaults(&s.profile[0], 0);
    s.profile[0].port = 2;
    s.profile[0].pad[ACT_L][0] = PB_LT; s.profile[0].pad[ACT_L][1] = -1;
    snprintf(s.profile[3].name, sizeof s.profile[3].name, "Wireless Controller (2)");     /* a name with spaces and brackets */
    host_profile_set_defaults(&s.profile[3], 1);
    s.profile[3].port = 3;
    s.profile[3].pad[ACT_A][0] = PB_G_BTN1 + 11; s.profile[3].pad[ACT_A][1] = PB_G_AX_X_NEG + 5;
    s.profile[3].pad[ACT_MENU][0] = PB_G_HAT_LEFT;
    s.cheat_hof = 1; s.cheat_cpuvs = 1;                                                   /* they are saved and read back */
    CHECK(host_settings_format(&s, text, sizeof text) > 0);
    host_settings_defaults(&t);
    CHECK(host_settings_parse(&t, text) == 0);
    CHECK(memcmp(&s, &t, sizeof s) == 0);
    CHECK(t.cheat_hof == 1 && t.cheat_cpuvs == 1);
    CHECK(host_settings_format(&t, text2, sizeof text2) > 0 && strcmp(text, text2) == 0);
    CHECK(host_settings_format(&s, text2, 20) == -1);                                    /* too small: reported, not cut */

    /* bad input is counted, skipped, and does not damage the rest */
    host_settings_defaults(&t);
    CHECK(host_settings_parse(&t, "cheats=maybe\nnonsense=1\ndeadzone=99\nkeyboard_port=7\nkey_a=NOSUCHKEY\ncheats=0\n") >= 4);
    CHECK(t.cheats == 0);                                                                /* the valid line after the bad ones still counts */
    CHECK(t.deadzone <= 50 && t.keyboard_port >= 0 && t.keyboard_port <= 4);

    /* gamepads: a pad drives the N64 controller its profile names; no profile yet = no input; buttons, trigger, dead zone, the stick */
    host_settings_defaults(&s);
    {
        HostRawInput raw;
        unsigned short b;
        int conn;
        memset(&raw, 0, sizeof raw);
        s.keyboard_port = 0;
        raw.pad[0].connected = 1;
        snprintf(raw.pad[0].name, sizeof raw.pad[0].name, "XInput controller 1");
        host_input_map(&s, &raw, 1, &b, &sx, &sy, &conn);
        CHECK(conn == 0);                                                                /* no profile yet: drives nothing */
        CHECK(host_profiles_sync(&s, &raw) == 1);                                        /* the sync makes the profile ... */
        CHECK(host_profiles_sync(&s, &raw) == 0);                                        /* ... once */
        CHECK(host_profile_find(&s, &raw.pad[0]) == 0 && s.profile[0].port == 1);        /* the lowest free N64 controller */
        host_input_map(&s, &raw, 1, &b, &sx, &sy, &conn);
        CHECK(conn == 1 && b == 0 && sx == 0 && sy == 0);
        raw.pad[0].lx = 3000;                                                            /* inside the default dead zone */
        host_input_map(&s, &raw, 1, &b, &sx, &sy, &conn);
        CHECK(sx == 0);
        raw.pad[0].lx = 12000;                                                           /* each gamepad has its own dead zone */
        s.profile[0].deadzone = 30; host_input_map(&s, &raw, 1, &b, &sx, &sy, &conn); CHECK(sx > 0);
        s.profile[0].deadzone = 40; host_input_map(&s, &raw, 1, &b, &sx, &sy, &conn); CHECK(sx == 0);
        s.profile[0].deadzone = 24;
        raw.pad[0].lx = 32767; raw.pad[0].ly = -32767;                                   /* full right/down: clamped to the console's range */
        host_input_map(&s, &raw, 1, &b, &sx, &sy, &conn);
        CHECK(sx > 0 && sx <= 80 && sy < 0 && sy >= -80);
        CHECK((b & 0x0100) && (b & 0x0400) && !(b & 0x0800) && !(b & 0x0200));            /* the stick tilted far also presses D-right and D-down */
        s.stick_dpad = 0; host_input_map(&s, &raw, 1, &b, &sx, &sy, &conn);
        CHECK(b == 0 && sx > 0);                                                         /* ... only while the option is on */
        s.stick_dpad = 1;
        raw.pad[0].lx = 12000; raw.pad[0].ly = 0; s.profile[0].deadzone = 10;             /* a light tilt moves the stick but is no D-pad press */
        host_input_map(&s, &raw, 1, &b, &sx, &sy, &conn);
        CHECK(sx > 0 && sx < 32 && b == 0);
        s.profile[0].deadzone = 24; raw.pad[0].lx = 32767; raw.pad[0].ly = -32767;
        raw.pad[0].lx = raw.pad[0].ly = 0;
        raw.pad[0].down[PB_LB] = 1;
        raw.pad[0].rt = 255;
        host_input_map(&s, &raw, 1, &b, &sx, &sy, &conn);
        CHECK((b & 0x0020) != 0);                                                        /* LB = L */
        CHECK((b & 0x2000) != 0);                                                        /* the right trigger = Z (the cheat's third key) */
        host_input_map(&s, &raw, 2, &b, &sx, &sy, &conn);
        CHECK(conn == 0);                                                                /* a port nobody drives is not connected */
        raw.pad[0].connected = 0;
        host_input_map(&s, &raw, 1, &b, &sx, &sy, &conn);
        CHECK(conn == 0);                                                                /* an unplugged pad: the port disappears */
    }

    /* a generic pad (a Bluetooth DualShock, a Switch Pro pad ...) is found by name and kind, gets the second free controller, and its numbered buttons,
     * hat and axes are bound like the Xbox pad's buttons */
    host_settings_defaults(&s);
    {
        HostRawInput raw;
        unsigned short b;
        int conn, p0, p1;
        memset(&raw, 0, sizeof raw);
        s.keyboard_port = 0;
        raw.pad[0].connected = 1;
        snprintf(raw.pad[0].name, sizeof raw.pad[0].name, "XInput controller 1");
        raw.pad[4].connected = 1; raw.pad[4].generic = 1;
        snprintf(raw.pad[4].name, sizeof raw.pad[4].name, "Wireless Controller");
        CHECK(host_profiles_sync(&s, &raw) == 2);
        p0 = host_profile_find(&s, &raw.pad[0]); p1 = host_profile_find(&s, &raw.pad[4]);
        CHECK(p0 >= 0 && p1 >= 0 && p0 != p1);
        CHECK(s.profile[p0].port == 1 && s.profile[p1].port == 2);                       /* each connected pad takes the next free controller */
        CHECK(s.profile[p1].generic == 1);
        raw.pad[4].down[PB_G_BTN1 + 1] = 1;                                              /* button 2 = A in the generic defaults */
        host_input_map(&s, &raw, 2, &b, &sx, &sy, &conn);
        CHECK(conn == 1 && b == 0x8000);
        host_input_map(&s, &raw, 1, &b, &sx, &sy, &conn);
        CHECK(conn == 1 && b == 0);                                                      /* it does not leak into controller 1 */
        raw.pad[4].down[PB_G_BTN1 + 1] = 0;
        raw.pad[4].down[PB_G_HAT_UP] = 1;
        host_input_map(&s, &raw, 2, &b, &sx, &sy, &conn);
        CHECK(b == 0x0800);                                                              /* hat up = D-pad up */
        raw.pad[4].down[PB_G_HAT_UP] = 0;
        raw.pad[4].lx = 32767;                                                           /* the X axis is the left stick */
        host_input_map(&s, &raw, 2, &b, &sx, &sy, &conn);
        CHECK(sx > 0 && sy == 0);
        raw.pad[4].lx = 0;
        raw.pad[4].axis[2] = 30000;                                                      /* axis Z+ = C right */
        host_input_map(&s, &raw, 2, &b, &sx, &sy, &conn);
        CHECK(b == 0x0001);
        raw.pad[4].axis[2] = 0;
        s.profile[p1].pad[ACT_B][0] = PB_G_AX_X_NEG + 7; s.profile[p1].pad[ACT_B][1] = -1;     /* rebinding: axis R+ = B */
        raw.pad[4].axis[3] = 30000;
        host_input_map(&s, &raw, 2, &b, &sx, &sy, &conn);
        CHECK((b & 0x4000) != 0);
        raw.pad[4].axis[3] = 0;
        raw.pad[4].connected = 0;                                                        /* unplugged: its controller disappears, the profile stays */
        host_input_map(&s, &raw, 2, &b, &sx, &sy, &conn);
        CHECK(conn == 0 && host_profile_find(&s, &raw.pad[4]) == p1);
        CHECK(strcmp(host_pad_button_name(PB_G_BTN1 + 2), "BTN3") == 0 && host_pad_button_from_name("btn12") == PB_G_BTN1 + 11);
        CHECK(host_pad_button_from_name("HAT_UP") == PB_G_HAT_UP && host_pad_button_from_name("AXZ+") == PB_G_AX_X_NEG + 5);
        CHECK(host_pad_button_from_name("BTN33") == -1 && host_pad_button_from_name("A") == PB_A);
    }

    /* the cheat chord is described with the keys that are really bound */
    host_settings_defaults(&s);
    {
        char line[200];
        host_cheat_help(&s, line, sizeof line);
        CHECK(strstr(line, "Q") && strstr(line, "E") && strstr(line, "C"));
        s.key[ACT_L][0] = 'U'; s.key[ACT_L][1] = HK_NONE;
        host_cheat_help(&s, line, sizeof line);
        CHECK(strstr(line, "U") != NULL);
    }

    /* paths */
    {
        char o[300];
        host_path_with_ext("/a/b.c/game.gb", ".sav", o, sizeof o); CHECK(strcmp(o, "/a/b.c/game.sav") == 0);
        host_path_with_ext("/a/b.c/game", ".sav", o, sizeof o);    CHECK(strcmp(o, "/a/b.c/game.sav") == 0);   /* a dot in a folder name is not an extension */
        host_path_with_ext("C:\\x\\red.GBC", ".sav", o, sizeof o); CHECK(strcmp(o, "C:\\x\\red.sav") == 0);
        CHECK(strcmp(host_path_basename("/a/b/c.gb"), "c.gb") == 0);
        CHECK(strcmp(host_path_basename("C:\\a\\c.gb"), "c.gb") == 0);
        CHECK(strcmp(host_path_basename("c.gb"), "c.gb") == 0);
        host_path_dirname("/a/b/c.gb", o, sizeof o); CHECK(strcmp(o, "/a/b") == 0);
        host_path_parent("/a/b", o, sizeof o); CHECK(strcmp(o, "/a") == 0);
    }

    if (sFailed) { printf("UI TEST FAILED: %d of %d checks\n", sFailed, sChecks); return 1; }
    printf("UI TEST PASSED: checks=%d\n", sChecks);
    return 0;
}
