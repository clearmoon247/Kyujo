/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_control.c - a DEVELOPMENT aid: drive the running game from another program (or from a shell), one command at a time, and take
 * screenshots on request. Not used by players; it exists so that the game can be played step by step without a window and without
 * guessing frame numbers in advance (see KYUJO_INPUT in host_input.c for the blind alternative).
 *
 * Enable:   KYUJO_CONTROL=<file>     The program reads commands, one per line, from <file> as they are appended to it
 *           (e.g. `echo "press A" >> <file>`; the file may be a regular file, created by the caller beforehand), and writes one
 *           result line per command to <file>.out ("ok <n>" when command number n has been carried out, "error ..." otherwise).
 * Commands: press <BUTTON[+BUTTON...]> [ms]   hold the buttons for ms milliseconds (default 200), then release and wait 250 ms
 *           hold <BUTTON[+...]>               keep holding until the next `hold` or `release`
 *           release                           release everything
 *           stick <x> <y> [ms]                tilt the stick (-128..127) for ms (default 300), then centre it
 *           wait <ms>                         do nothing for ms of wall-clock time
 *           trace <file> | trace stop         record which game functions run (host_trace.c; only in a build made for it); mark <word> labels a point
 *           waitf <n>                         do nothing until n more pictures have been committed (game time: steady even when the game
 *                                             runs slower than real time, which is when `wait` scripts drift out of step)
 *           shot <path.png>                   save the picture the video output shows now
 *           rdptrace on|off                  print every texture rectangle the renderer draws (development; to see how one picture is made)
 *           quit                              end the program
 *           Button names are those of host_input.c (A B Z START UP DOWN LEFT RIGHT L R CUP CDOWN CLEFT CRIGHT).
 * Commands are carried out in order, each only after the previous one has finished. The game runs on its own clock meanwhile.
 * The live controller state set here is used only while no KYUJO_INPUT script is active.
 * Linux/POSIX only (a plain host thread). HOST-side file.
 */
#ifndef _WIN32
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "platform.h"

static const struct { const char *name; unsigned short bit; } kBtn[] = {
    {"A", 0x8000}, {"B", 0x4000}, {"Z", 0x2000}, {"START", 0x1000}, {"UP", 0x0800}, {"DOWN", 0x0400}, {"LEFT", 0x0200},
    {"RIGHT", 0x0100}, {"L", 0x0020}, {"R", 0x0010}, {"CUP", 0x0008}, {"CDOWN", 0x0004}, {"CLEFT", 0x0002}, {"CRIGHT", 0x0001},
};

/* ---- screenshot hand-off between the control thread and the game thread (host_vi_commit) ---- */
static pthread_mutex_t sShotLock = PTHREAD_MUTEX_INITIALIZER;
static char sShotPath[1024];
static int sShotPending, sShotResult;           /* result: 0 not done, 1 ok, -1 failed */
static unsigned sCommits;                       /* video commits so far (for `waitf`); written by the game thread, read by the control thread */

/* Called by the video output (game thread) at every commit: returns the path of a requested screenshot or NULL. Copies into `buf`. */
const char *host_control_take_shot(char *buf, unsigned size)
{
    const char *r = NULL;
    __atomic_add_fetch(&sCommits, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&sShotLock);
    if (sShotPending) {
        snprintf(buf, size, "%s", sShotPath);
        sShotPending = 0;
        r = buf;
    }
    pthread_mutex_unlock(&sShotLock);
    return r;
}
void host_control_shot_done(int ok)
{
    pthread_mutex_lock(&sShotLock);
    sShotResult = ok ? 1 : -1;
    pthread_mutex_unlock(&sShotLock);
}

static int parse_buttons(const char *s, unsigned short *out)
{
    unsigned short v = 0;
    char tmp[128], *p, *save = NULL;
    snprintf(tmp, sizeof tmp, "%s", s);
    for (p = strtok_r(tmp, "+", &save); p; p = strtok_r(NULL, "+", &save)) {
        unsigned k;
        int found = 0;
        for (k = 0; k < sizeof kBtn / sizeof kBtn[0]; k++) if (strcmp(kBtn[k].name, p) == 0) { v |= kBtn[k].bit; found = 1; }
        if (!found) return 0;
    }
    *out = v;
    return 1;
}

static void sleep_ms(unsigned ms) { host_sleep_ns((unsigned long long)ms * 1000000ULL); }

static void *control_thread(void *arg)
{
    const char *path = (const char *)arg;
    char outpath[1100], line[512];
    FILE *in, *out;
    long done = 0;
    unsigned short held = 0;

    snprintf(outpath, sizeof outpath, "%s.out", path);
    out = fopen(outpath, "w");
    in = fopen(path, "r");
    if (!in || !out) { fprintf(stderr, "kyujo: KYUJO_CONTROL: cannot open %s\n", path); return NULL; }
    setvbuf(out, NULL, _IOLBF, 0);
    for (;;) {
        char cmd[32] = "", a1[256] = "", a2[64] = "", a3[64] = "";
        int n;
        if (!fgets(line, sizeof line, in)) { clearerr(in); sleep_ms(30); continue; }     /* end of file: wait for more, like `tail -f` */
        n = sscanf(line, "%31s %255s %63s %63s", cmd, a1, a2, a3);
        if (n < 1 || cmd[0] == '#') continue;
        done++;
        if (strcmp(cmd, "press") == 0 && n >= 2) {
            unsigned short b;
            unsigned ms = n >= 3 ? (unsigned)atoi(a2) : 250;
            if (!parse_buttons(a1, &b)) { fprintf(out, "error %ld unknown button in '%s'\n", done, a1); continue; }
            host_input_set_live(b, 0, 0); sleep_ms(ms);
            host_input_set_live(held, 0, 0); sleep_ms(250);
        } else if (strcmp(cmd, "hold") == 0 && n >= 2) {
            if (!parse_buttons(a1, &held)) { fprintf(out, "error %ld unknown button in '%s'\n", done, a1); continue; }
            host_input_set_live(held, 0, 0);
        } else if (strcmp(cmd, "release") == 0) {
            held = 0; host_input_set_live(0, 0, 0);
        } else if (strcmp(cmd, "stick") == 0 && n >= 3) {
            int x = atoi(a1), y = atoi(a2);
            unsigned ms = n >= 4 ? (unsigned)atoi(a3) : 300;
            if (x < -128 || x > 127 || y < -128 || y > 127) { fprintf(out, "error %ld stick out of range\n", done); continue; }
            host_input_set_live(held, (signed char)x, (signed char)y); sleep_ms(ms);
            host_input_set_live(held, 0, 0); sleep_ms(250);
        } else if (strcmp(cmd, "wait") == 0 && n >= 2) {
            sleep_ms((unsigned)atoi(a1));
        } else if (strcmp(cmd, "waitf") == 0 && n >= 2) {
            unsigned target = __atomic_load_n(&sCommits, __ATOMIC_RELAXED) + (unsigned)atoi(a1);
            unsigned long long waited = 0;
            while ((int)(target - __atomic_load_n(&sCommits, __ATOMIC_RELAXED)) > 0 && waited < 300000) { sleep_ms(10); waited += 10; }    /* at most 5 minutes */
        } else if (strcmp(cmd, "shot") == 0 && n >= 2) {
            int waited = 0, res = 0;
            pthread_mutex_lock(&sShotLock);
            snprintf(sShotPath, sizeof sShotPath, "%s", a1);
            sShotResult = 0;
            sShotPending = 1;
            pthread_mutex_unlock(&sShotLock);
            while (waited < 20000 && res == 0) {                  /* the next video commit takes it */
                sleep_ms(20); waited += 20;
                pthread_mutex_lock(&sShotLock); res = sShotResult; pthread_mutex_unlock(&sShotLock);
            }
            if (res != 1) { fprintf(out, "error %ld screenshot not taken (%s)\n", done, res ? "write failed" : "no picture within 20 s"); continue; }
        } else if (strcmp(cmd, "rdptrace") == 0 && n >= 2) {
            host_rdp_trace(strcmp(a1, "on") == 0);
        } else if (strcmp(cmd, "trace") == 0 && n >= 2) {       /* `trace <file>` opens a call trace, `trace stop` closes it (host_trace.c; needs a trace build) */
            if (strcmp(a1, "stop") == 0) host_trace_stop();
            else if (host_trace_start(a1, NULL) != 0) { fprintf(out, "error %ld cannot open the trace\n", done); continue; }
        } else if (strcmp(cmd, "mark") == 0 && n >= 2) {        /* `mark <word>` writes a labelled line into the open trace */
            host_trace_mark(a1);
        } else if (strcmp(cmd, "quit") == 0) {
            fprintf(out, "ok %ld\n", done);
            host_exit(0);
        } else {
            fprintf(out, "error %ld unknown command: %s", done, line);
            continue;
        }
        fprintf(out, "ok %ld\n", done);
    }
}

void host_control_start(void)
{
    static char path[1024];
    const char *e = getenv("KYUJO_CONTROL");
    pthread_t t;
    if (!e || !*e) return;
    snprintf(path, sizeof path, "%s", e);
    if (pthread_create(&t, NULL, control_thread, path) != 0) fprintf(stderr, "kyujo: KYUJO_CONTROL: cannot start the control thread\n");
    else pthread_detach(t);
}
#else
#include "platform.h"
void host_control_start(void) {}
const char *host_control_take_shot(char *buf, unsigned size) { (void)buf; (void)size; return 0; }
void host_control_shot_done(int ok) { (void)ok; }
#endif
