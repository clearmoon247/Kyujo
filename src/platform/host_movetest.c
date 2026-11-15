/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_movetest.c - the "Move animation test": watch any move, used by any Pokemon, and write a log of the game functions and particle effects it ran.
 *
 * What the player does: opens the menu (F1), chooses "Move animation test", picks a Pokemon, a move and (optionally) the opponent's Pokemon, and starts it.
 * What happens: the program restarts itself with KYUJO_MOVETEST="<species>,<move>,<opponent species>" in its environment. The new copy
 *   1. is "armed": patch 0029 (the game's Memmap_SetFragmentMap) asks __osHostMoveTestValue for the species and move when the battle engine is loaded and
 *      writes them into the first player's lead Pokemon (and the opponent's lead species), so the game itself builds a battle with them;
 *   2. plays the game's menus by itself with a fixed list of button presses (kRoute: title screen -> Stadium -> Poke Cup -> three rental Pokemon ->
 *      battle) - host_movetest_tick is called once per picture and sets the live controller state;
 *   3. in the battle presses the buttons that choose the first command and the move, with the call trace (host_trace.c) recording;
 *   4. closes the trace, which is written to movefx-<pokemon>-<move>.log in the program's folder, and tells the player.
 * The log lists, in order of first call, the game functions that ran with their picture numbers, so the particle and effect functions of the move
 * (ParticleEffect_*, Gfx_*Particle*, BattleAnim_*) are visible. That needs a program built with --trace-calls (docs/move-trace.md); an ordinary
 * program still runs the battle and says in the log that no call was recorded.
 *
 * The same thing can be started from a shell: KYUJO_MOVETEST=94,101,0 ./kyujo  (Gengar, Night Shade, opponent unchanged);
 * with KYUJO_MOVETEST_QUIT=1 the program ends when the test is over (tools/native/movetest_batch.py runs many tests that way).
 * Limits: the route assumes the game's default menus (a new or ordinary save, no Game Boy cartridge screens in the way); if the menus differ the
 * route goes wrong and the log says "never reached the battle" after a time-out. Only the first player's lead Pokemon is changed.
 * HOST-side file (C library only). Not instrumented.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "host_ui.h"

extern const char *const gMoveTestSpeciesName[152];     /* generated into the build folder by tools/native/gen_movetest_names.py */
extern const char *const gMoveTestMoveName[166];

enum { BTN_A = 0x8000, BTN_B = 0x4000, BTN_START = 0x1000, BTN_UP = 0x0800, BTN_DOWN = 0x0400, BTN_LEFT = 0x0200, BTN_RIGHT = 0x0100,
       BTN_CUP = 0x0008, BTN_CDOWN = 0x0004, BTN_CLEFT = 0x0002, BTN_CRIGHT = 0x0001 };
typedef struct { unsigned short buttons; unsigned hold, after; } Step;

/* The route from the title screen to the first command menu of a Poke Cup battle (generated from the scripted route that was proved on Linux, see docs/move-trace.md).
 * { buttons, frames held, frames waited after releasing }. */
static const Step kRoute[] = {
    { 0, 0, 1500 },
    { BTN_START, 18, 315 },
    { BTN_A, 18, 735 },
    { BTN_A, 18, 555 },
    { BTN_A, 18, 375 },
    { BTN_A, 18, 315 },
    { BTN_A, 18, 315 },
    { BTN_A, 18, 435 },
    { BTN_A, 18, 435 },
    { BTN_A, 18, 375 },
    { BTN_B, 18, 75 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_DOWN, 9, 27 },
    { BTN_RIGHT, 9, 45 },
    { BTN_A, 18, 135 },
    { BTN_A, 18, 105 },
    { BTN_RIGHT, 9, 40 },
    { BTN_A, 18, 90 },
    { BTN_A, 18, 105 },
    { BTN_RIGHT, 9, 40 },
    { BTN_A, 18, 90 },
    { BTN_A, 18, 105 },
    { BTN_DOWN, 9, 40 },
    { BTN_A, 18, 90 },
    { BTN_A, 18, 105 },
    { BTN_LEFT, 9, 40 },
    { BTN_A, 18, 90 },
    { BTN_A, 18, 105 },
    { BTN_LEFT, 9, 40 },
    { BTN_A, 18, 90 },
    { BTN_A, 18, 205 },
    { BTN_A, 18, 375 },
    { BTN_A, 18, 915 },
    { BTN_RIGHT, 9, 40 },
    { BTN_A, 18, 90 },
    { BTN_A, 18, 205 },
    { BTN_A, 18, 1015 },
    { BTN_B, 18, 105 },
    { BTN_CLEFT, 18, 105 },
    { BTN_CUP, 18, 165 },
    { BTN_A, 18, 1515 },
};

/* ---- state ------------------------------------------------------------------------------------------------------------------------ */
static int sArmed;                         /* patch 0029 asks: change the battle's Pokemon */
static int sSpecies, sMove, sFoe;          /* 1..151, 1..165, 0..151 (0 = the opponent's own Pokemon) */
static unsigned sTick;                     /* pictures committed since the program started */
static unsigned sStep;                     /* index into kRoute, then kRouteLen + 0..2 for the battle steps */
static unsigned sHeld, sAfter;             /* frames left of the press, then of the wait that follows it */
static int sRunning, sDone, sStepStarted, sBattleSeen;
static char sLogPath[1024];

#define ROUTE_LEN (sizeof kRoute / sizeof kRoute[0])
#define BATTLE_STEPS 6
#define MOVE_FRAMES 600                    /* pictures recorded after the move is chosen: a long move animation is about 300 */

/* ---- what the game asks (patch 0029, memmap.c) ---------------------------------------------------------------------------------------- */
int __osHostMoveTestValue(int which)
{
    switch (which) {
    case 0: return sArmed;
    case 1: return sSpecies;
    case 2: return sMove;
    case 3: return sFoe;
    }
    return 0;
}

void __osHostMoveTestNotice(int applied)
{
    if (!applied) return;
    sBattleSeen = 1;                       /* the battle engine has been loaded with the test's Pokemon: the route got there */
    fprintf(stderr, "kyujo: move test: battle set up: %s uses %s%s%s\n", gMoveTestSpeciesName[sSpecies], gMoveTestMoveName[sMove],
            sFoe ? " against " : "", sFoe ? gMoveTestSpeciesName[sFoe] : "");
}

/* ---- starting --------------------------------------------------------------------------------------------------------------------- */
static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s=%s", name, value);
    _putenv(tmp);
#else
    if (value[0]) setenv(name, value, 1); else unsetenv(name);
#endif
}

/* The menu page's "Start": restarts the program with the test in its environment. Returns 0 if the restart failed. */
int host_movetest_launch(int species, int move, int foe)
{
    char v[64];
    snprintf(v, sizeof v, "%d,%d,%d", species, move, foe);
    set_env("KYUJO_MOVETEST", v);
    return host_restart();
}

const char *host_movetest_species_name(int id) { return id >= 1 && id <= 151 ? gMoveTestSpeciesName[id] : "?"; }
const char *host_movetest_move_name(int id) { return id >= 1 && id <= 165 ? gMoveTestMoveName[id] : "?"; }

/* Reads KYUJO_MOVETEST (then removes it, so that a later "Restart game" does not repeat the test). Called once at start-up. */
void host_movetest_init(void)
{
    const char *e = getenv("KYUJO_MOVETEST");
    int s = 0, m = 0, f = 0;
    char name[200];
    if (!e || !e[0]) return;
    if (sscanf(e, "%d,%d,%d", &s, &m, &f) < 2 || s < 1 || s > 151 || m < 1 || m > 165 || f < 0 || f > 151) {
        fprintf(stderr, "kyujo: move test: KYUJO_MOVETEST='%s' is not <species 1-151>,<move 1-165>[,<opponent 0-151>]\n", e);
        set_env("KYUJO_MOVETEST", "");
        return;
    }
    set_env("KYUJO_MOVETEST", "");
    sSpecies = s; sMove = m; sFoe = f;
    sArmed = sRunning = 1;
    snprintf(name, sizeof name, "movefx-%s-%s.log", gMoveTestSpeciesName[s], gMoveTestMoveName[m]);
    {
        char *p;                                           /* a file name that every system accepts: letters and digits, everything else becomes '-' */
        for (p = name; *p; p++) {
            int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.';
            if (!ok) *p = '-';
        }
    }
    host_app_path(sLogPath, sizeof sLogPath, name);
    fprintf(stderr, "kyujo: move test: %s uses %s; the route to the battle starts now; the log will be %s\n", gMoveTestSpeciesName[s],
            gMoveTestMoveName[m], sLogPath);
}

/* ---- the route, one picture at a time ------------------------------------------------------------------------------------------------------- */
static void finish(const char *how)
{
    host_input_set_live(0, 0, 0);
    host_trace_mark(how);
    host_trace_stop();
    sRunning = 0; sDone = 1;
    if (strcmp(how, "done") == 0) {
        char msg[1100];
        snprintf(msg, sizeof msg, "Move test finished. Log: %s", sLogPath);
        host_menu_notice(msg);
        fprintf(stderr, "kyujo: move test: finished, log %s\n", sLogPath);
    } else {
        host_menu_notice("Move test stopped: the battle was not reached (see the log)");
        fprintf(stderr, "kyujo: move test: stopped (%s)\n", how);
    }
    {   /* for scripts that run many tests one after the other (tools/native/movetest_batch.py): KYUJO_MOVETEST_QUIT=1 ends the program now */
        const char *q = getenv("KYUJO_MOVETEST_QUIT");
        if (q && q[0] == '1') host_exit(strcmp(how, "done") == 0 ? 0 : 3);
    }
}

/* The step to carry out now: a route step, or one of the three battle steps. */
static Step current(void)
{
    static const Step battle[BATTLE_STEPS] = {
        { BTN_A, 18, 120 },                /* the battle's command menu: A = Battle (shows the moves, on the C buttons) */
        /* The Pokemon knows one move (patch 0029), in the first slot. Which C button that slot is on is not looked up: all four are pressed one
         * after the other; the empty slots do nothing, the one with the move starts it. */
        { BTN_CUP, 18, 20 },
        { BTN_CLEFT, 18, 20 },
        { BTN_CRIGHT, 18, 20 },
        { BTN_CDOWN, 18, MOVE_FRAMES },    /* then the move plays: the trace keeps recording */
        { 0, 0, 0 },                       /* done */
    };
    return sStep < ROUTE_LEN ? kRoute[sStep] : battle[sStep - ROUTE_LEN];
}

void host_movetest_tick(void)
{
    sTick++;
    if (!sRunning) return;
    if (!sStepStarted) {                                   /* begin the step: press */
        Step st = current();
        if (sStep == ROUTE_LEN) {                          /* the battle's first command menu should be on the screen: start recording */
            if (!sBattleSeen) { finish("never_reached_the_battle"); return; }
            if (host_trace_start(sLogPath, NULL) == 0) host_trace_mark("battle_menu");
        } else if (sStep == ROUTE_LEN + 1) {
            host_trace_mark("move_buttons");
        } else if (sStep == ROUTE_LEN + BATTLE_STEPS - 1) {
            finish("done");
            return;
        }
        sStepStarted = 1; sHeld = st.hold; sAfter = st.after;
        host_input_set_live(st.buttons, 0, 0);
    }
    if (sHeld > 0) {
        if (--sHeld == 0) host_input_set_live(0, 0, 0);    /* released */
        return;
    }
    if (sAfter > 0) { sAfter--; if (sAfter > 0) return; }
    sStep++; sStepStarted = 0;
}
