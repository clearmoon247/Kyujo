/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_cheat.c - the host's side of the debug cheat in port patch 0026 (L+R: knock out the opponent's Pokemon, L+R+Z: win the battle).
 *
 * The cheat itself runs inside the game's battle code (port/patches/0026-debug-knockout-cheat.patch). The host only says whether it is switched on
 * and receives notices, to write them to the log and to show a short message on the picture (host_menu.c, when the menus exist).
 * HOST-side file: may use the C library.
 */
#include <stdio.h>
#include <stdlib.h>

#include "platform.h"
#include "host_ui.h"

/* 1 = on. The setting (menu, kyujo.cfg) decides; KYUJO_CHEATS=0 or =1 overrides it for one run (tests). */
int __osHostCheatsEnabled(void)
{
    const char *env = getenv("KYUJO_CHEATS");
    if (env && env[0] == '0') return 0;
    if (env && env[0] == '1') return 1;
    return host_settings()->cheats;
}

/* The two menu-driven cheats (port patches 0039 and 0040; both off by default): which = 1: the Hall of Fame is shown complete, 2: Kids Club computer against
 * computer. The setting (Options menu, kyujo.cfg) decides; KYUJO_CHEAT_HOF / KYUJO_CHEAT_CPUVS = 0 or 1 override it for one run (tests).
 * The game asks often (every time it reads a Hall of Fame record), so the first "yes" of a run is logged and shown once. */
int __osHostCheatOption(int which)
{
    static int told[3];
    const char *env = getenv(which == 1 ? "KYUJO_CHEAT_HOF" : "KYUJO_CHEAT_CPUVS");
    int on;
    if (which != 1 && which != 2) return 0;
    if (env && env[0] == '0') on = 0;
    else if (env && env[0] == '1') on = 1;
    else on = which == 1 ? host_settings()->cheat_hof : host_settings()->cheat_cpuvs;
    if (on && !told[which]) {
        told[which] = 1;
        fprintf(stderr, "kyujo: cheat: %s\n", which == 1 ? "Hall of Fame shown complete (records made up in memory, the save is not changed)" : "Kids Club: Ready! can be chosen with nobody playing (computer against computer)");
        host_menu_notice(which == 1 ? "Cheat: Hall of Fame complete" : "Cheat: Kids Club computer against computer");
    }
    return on;
}

/* Development only (port patch 0030): 1 once per run when KYUJO_FORCE_PIKACHU_REWARD=1, so that the Pikachu reward scene (and its save to the
 * Game Boy cartridge) is shown after the next battle. */
int __osHostForcePikachuReward(void)
{
    static int done;
    const char *env = getenv("KYUJO_FORCE_PIKACHU_REWARD");
    if (done || !env || env[0] != '1') return 0;
    done = 1;
    fprintf(stderr, "kyujo: development: the Pikachu reward scene is forced after this battle\n");
    return 1;
}

/* what: 1 = knock out the opponent's active Pokemon, 2 = win the battle. applied: 0 = armed (the buttons were pressed), 1 = carried out. */
void __osHostCheatNotice(int what, int applied)
{
    fprintf(stderr, "kyujo: cheat: %s %s\n", what == 2 ? "win the battle" : "knock out the opponent",
            applied ? "carried out (the opponent faints now)" : "armed (takes effect when this turn is played: choose a move)");
    if (applied) host_menu_notice(what == 2 ? "Cheat: you win this battle" : "Cheat: opponent's Pokemon knocked out");
    else host_menu_notice(what == 2 ? "Cheat armed: win the battle (choose a move)" : "Cheat armed: knock out the opponent (choose a move)");
}

/* The cheat's keys as they are bound now, for the menu: "Q + E knocks out, Q + E + C wins" (the first key of L, R and Z). The cheat chord is
 * the N64 buttons L + R (+ Z) of the first controller, so it follows whatever the keyboard and gamepads are bound to. */
/* A console's own names for the gamepad inputs of the cheat: the shoulder buttons are L and R, the triggers ZL and ZR. */
static const char *console_name(int pb)
{
    switch (pb) {
    case PB_LB: return "L";
    case PB_RB: return "R";
    case PB_LT: return "ZL";
    case PB_RT: return "ZR";
    default: return host_pad_button_label(pb);
    }
}

void host_cheat_help(const HostSettings *s, char *out, unsigned size)
{
    HostRawInput raw;
    host_raw_get(&raw);
    if (raw.console) {                                     /* no keys on a console: the gamepad's buttons, as the first connected gamepad (or the default layout) has them bound */
        HostPadProfile def;
        const HostPadProfile *pr = &def;
        int i;
        host_profile_set_defaults(&def, 0);
        for (i = 0; i < HOST_PADS; i++) {
            int k;
            if (!raw.pad[i].connected) continue;
            k = host_profile_find(s, &raw.pad[i]);
            if (k >= 0) pr = &s->profile[k];
            break;
        }
        {
            int li = pr->pad[ACT_L][0] >= 0 ? pr->pad[ACT_L][0] : pr->pad[ACT_L][1], ri = pr->pad[ACT_R][0] >= 0 ? pr->pad[ACT_R][0] : pr->pad[ACT_R][1], zi = pr->pad[ACT_Z][0] >= 0 ? pr->pad[ACT_Z][0] : pr->pad[ACT_Z][1];
            const char *l = li >= 0 ? console_name(li) : NULL, *r = ri >= 0 ? console_name(ri) : NULL, *z = zi >= 0 ? console_name(zi) : NULL;
            if (l && r && z) snprintf(out, size, "%s + %s knocks out, %s + %s + %s wins", l, r, l, r, z);
            else if (l && r) snprintf(out, size, "%s + %s knocks out", l, r);
            else snprintf(out, size, "Hold the N64 buttons L + R (+ Z to win)");
        }
        return;
    }
    const char *l = host_key_name(s->key[ACT_L][0] ? s->key[ACT_L][0] : s->key[ACT_L][1]);
    const char *r = host_key_name(s->key[ACT_R][0] ? s->key[ACT_R][0] : s->key[ACT_R][1]);
    const char *z = host_key_name(s->key[ACT_Z][0] ? s->key[ACT_Z][0] : s->key[ACT_Z][1]);
    if (l && r && z) snprintf(out, size, "Keys: %s + %s knocks out, %s + %s + %s wins", l, r, l, r, z);
    else if (l && r) snprintf(out, size, "Keys: %s + %s knocks out (no key for Z)", l, r);
    else snprintf(out, size, "Hold the N64 buttons L + R (+ Z to win)");
}
