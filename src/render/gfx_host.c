/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * gfx_host.c - the entry points the game-side hardware model calls: run a graphics task, tell the video output about a new frame.
 * Also the development frame dump:
 *   KYUJO_DUMP_DIR=<dir>      write each new frame as <dir>/frame_NNNNN.png (frames are numbered by the order they appear)
 *   KYUJO_DUMP_EVERY=<n>      only every n-th new frame (default 1)
 *   KYUJO_DUMP_MAX=<n>        stop after n files (default 200)
 *   KYUJO_DUMP_FROM=<n>       skip the frames numbered below n (default 0), so a late scene can be sampled finely
 *   KYUJO_GFX_BENCH=<t1,t2,...>[:<reps>]   BENCHMARK of the renderer on real frames (development). At the first graphics task that
 *                                   starts <t> seconds or more after the first graphics task of the run, the same display list is run <reps> times
 *                                   in a row (default 30) and each run is timed. One summary line goes to stderr: fastest and median run in ms,
 *                                   pixels written, and a hash of the colour image after the first and after the last run (equal = the list
 *                                   redraws everything it reads, so repeating it is a fair benchmark). The game then continues. Combine with
 *                                   KYUJO_RDP_PROFILE=1 for a per-primitive table of every run. Hashes of different game runs cannot be
 *                                   compared (the scene at <t> seconds differs); tests/native/render/make_rdp_ab_tree.py makes a copy of the tree that compares
 *                                   a changed rdp.c against an old one on the SAME frame inside one run.
 * HOST-side.
 */
#include <stdio.h>
#include <stdlib.h>

#include "platform.h"
#include "host_ui.h"
#include "gfx.h"
#include "rdp.h"

static int sRdpReady;
static int sDirty;                      /* a graphics task has drawn since the last picture was shown */

static unsigned long long sTaskNs;      /* host time the current task has used so far (it may run in several pumps) */

/* ---- development benchmark (KYUJO_GFX_BENCH, see the header) ------------------------------------------------------------------- */

#define BENCH_MAX_POINTS 16

static int cmp_u64(const void *a, const void *b)
{
    unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Runs the display list at dl_addr `reps` times and prints the summary. Called only from host_gfx_task_begin, before the real run of that
 * task. The RDP is reset first (the game's own state is lost; frames set up everything they use). The display list is expected to set up everything it uses (a frame normally clears and
 * redraws its buffers); the two hashes in the output show whether that held (equal = the repeated runs drew the same picture). */
static void gfx_bench_run(unsigned int dl_addr, double at_s, int reps)
{
    unsigned long long *ns = (unsigned long long *)malloc((size_t)reps * sizeof *ns);
    unsigned long long first_hash = 0, last_hash = 0, pix_first = 0;
    uint32_t tri0, rect0, tri1, rect1;
    uint64_t pix0, pix1;
    int i;
    if (!ns) return;
    rdp_reset();                        /* every run starts from the power-up state, so the result does not depend on what was drawn before */
    for (i = 0; i < reps; i++) {
        unsigned int syncs = 0;
        unsigned long long t0;
        rdp_stats(&tri0, &rect0, &pix0);
        gfx_reset_rsp();
        gfx_task_begin(dl_addr);
        t0 = host_now_ns();
        gfx_task_run(0, NULL, &syncs);
        ns[i] = host_now_ns() - t0;
        rdp_stats(&tri1, &rect1, &pix1);
        rdp_profile_report(ns[i]);
        if (i == 0) { first_hash = rdp_dev_color_image_hash(); pix_first = pix1 - pix0; }
        last_hash = rdp_dev_color_image_hash();
    }
    qsort(ns, (size_t)reps, sizeof *ns, cmp_u64);
    fprintf(stderr, "gfx-bench: at %.0f s, display list %08X, %d runs: fastest %.2f ms, median %.2f ms, %llu pixels written, %u triangles, %u rectangles, "
            "picture hash first %016llX last %016llX%s\n", at_s, dl_addr, reps, (double)ns[0] / 1e6, (double)ns[reps / 2] / 1e6, pix_first,
            tri1 - tri0, rect1 - rect0, first_hash, last_hash, first_hash == last_hash ? "" : "  (DIFFERENT: the list does not redraw everything it reads)");
    free(ns);
}

/* Called at every task start; cheap when the variable is not set. */
static void gfx_bench_poll(unsigned int dl_addr)
{
    static int parsed;
    static double at[BENCH_MAX_POINTS];
    static int n_at, done, reps = 30;
    static unsigned long long start_ns;
    double now_s;
    if (!parsed) {
        const char *e = getenv("KYUJO_GFX_BENCH");
        parsed = 1;
        start_ns = host_now_ns();
        while (e && *e && n_at < BENCH_MAX_POINTS) {
            char *end;
            double v = strtod(e, &end);
            if (end == e) break;
            at[n_at++] = v;
            e = end;
            if (*e == ',') e++;
            else break;
        }
        if (e && *e == ':' && atoi(e + 1) > 0) reps = atoi(e + 1);
        if (n_at == 0) n_at = -1;                                  /* the variable is not set (or unusable): never look again */
    }
    if (n_at <= 0 || done >= n_at) return;
    now_s = (double)(host_now_ns() - start_ns) / 1e9;
    if (now_s >= at[done]) {
        double due = at[done++];
        gfx_bench_run(dl_addr, due, reps);
    }
}

void host_gfx_task_begin(unsigned int dl_addr)
{
    gfx_bench_poll(dl_addr);
    if (!sRdpReady) {
        sRdpReady = 1;
        rdp_reset();
    }
    sDirty = 1;
    gfx_reset_rsp();                    /* RSP-side state is fresh at each task start; the RDP's state persists (it is not reloaded) */
    sTaskNs = 0;
    rdp_cimg_log_reset();               /* which frame buffers this task draws into is learnt from its G_SETCIMG commands */
    gfx_task_begin(dl_addr);
}

int host_gfx_task_may_draw_to(unsigned int origin) { return rdp_task_may_draw_to(origin); }

int host_gfx_task_pump(unsigned int budget, const volatile int *yield_req, unsigned int *syncs)
{
    unsigned long long t0 = host_now_ns();
    int r = gfx_task_run(budget, yield_req, syncs);
    unsigned long long dt = host_now_ns() - t0;
    sTaskNs += dt;
    host_perf_add(PERF_GFX, dt);
    if (r == HOST_GFX_DONE) rdp_profile_report(sTaskNs);   /* development: KYUJO_RDP_PROFILE=<min ms> prints which drawing commands cost the time */
    return r;
}

/* ---- the task on a worker thread ------------------------------------------------------------------------------------------------------
 * On the console the RDP draws while the CPU goes on. The task (gfx_task_run) is handed to a worker thread (host_job_start); the game side
 * (src/ultra/hw_sp.c) collects the result when it needs it: before the picture is shown, when it asks for a yield, and by polling at its
 * scheduling points. Only this worker touches the display list interpreter's and the rasteriser's state while a task runs. */
typedef struct GfxJob {
    unsigned int budget;
    const volatile int *yield_req;
    unsigned int syncs;
    int result;
    unsigned long long ns;
} GfxJob;
static GfxJob sJob;
static int sJobActive;                  /* started and not yet collected (touched by the game side only) */

int host_gfx_async_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("KYUJO_GFX_THREAD"); on = !(e && e[0] == '0'); }
    return on;
}

static void gfx_job_main(void *p)
{
    GfxJob *j = (GfxJob *)p;
    unsigned long long t0 = host_now_ns();
    j->result = gfx_task_run(j->budget, j->yield_req, &j->syncs);
    j->ns = host_now_ns() - t0;
}

void host_gfx_task_run_async(const volatile int *yield_req)
{
    sJob.budget = 0;
    sJob.yield_req = yield_req;
    sJob.syncs = 0;
    sJob.result = HOST_GFX_DONE;
    sJob.ns = 0;
    sJobActive = 1;
    host_job_start(gfx_job_main, &sJob);
}

int host_gfx_task_collect(int wait, unsigned int *syncs)
{
    if (!sJobActive) return HOST_GFX_DONE;
    if (wait) host_job_wait();
    else if (host_job_running()) return HOST_GFX_PAUSED;
    sJobActive = 0;
    host_job_taken(0);
    *syncs += sJob.syncs;
    sTaskNs += sJob.ns;
    host_perf_add(PERF_GFX, sJob.ns);
    if (sJob.result == HOST_GFX_DONE) rdp_profile_report(sTaskNs);
    return sJob.result;
}

/* The whole task at once (used where nothing needs to run in between). */
unsigned int host_gfx_task(unsigned int dl_addr)
{
    unsigned int syncs = 0;
    host_gfx_task_begin(dl_addr);
    host_gfx_task_pump(0, NULL, &syncs);
    return syncs;
}

unsigned int host_gfx_frame_count(void);

static ViRegs sLast;
static int sHaveLast;
static unsigned sFrameNo, sDumped;

unsigned int host_gfx_frame_count(void) { return sFrameNo; }

void host_vi_commit(unsigned int control, unsigned int origin, unsigned int width, unsigned int h_video, unsigned int v_video,
                    unsigned int x_scale, unsigned int y_scale)
{
    static int env_read;
    static const char *dir;
    static unsigned every = 1, max = 200, from = 0;
    ViRegs r;
    int same;

    host_perf_add(PERF_FIELDS, 0);                            /* one call per retrace the game handles: the speed monitor counts them */
    host_window_sync_view();                                  /* the shell build: the Widescreen option (host_window_shell.c) */
    host_save_poll();                                         /* writes the cartridge save when the game has stopped changing it (host_save.c) */
    host_movetest_tick();                                     /* the move tester's scripted buttons (host_movetest.c); does nothing unless it is running */
    host_menu_poll();                                         /* opens the menu on F1 / Esc / gamepad Back; while it is open this call does not return (the game is paused) */
    {   /* development experiment (KYUJO_SLOW_MS=<n>): make every field take n ms longer, to test whether a slow host triggers the bad lists */
        static int slow = -1;
        if (slow < 0) { const char *e = getenv("KYUJO_SLOW_MS"); slow = e ? atoi(e) : 0; }
        if (slow > 0) host_sleep_ns((unsigned long long)slow * 1000000ULL);
    }
    if (!env_read) {
        const char *e;
        env_read = 1;
        dir = getenv("KYUJO_DUMP_DIR");
        if ((e = getenv("KYUJO_DUMP_EVERY")) != NULL && atoi(e) > 0) every = (unsigned)atoi(e);
        if ((e = getenv("KYUJO_DUMP_MAX")) != NULL && atoi(e) > 0) max = (unsigned)atoi(e);
        if ((e = getenv("KYUJO_DUMP_FROM")) != NULL && atoi(e) > 0) from = (unsigned)atoi(e);
    }
    r.control = control; r.origin = origin & 0x00FFFFFFu; r.width = width; r.h_video = h_video; r.v_video = v_video; r.x_scale = x_scale; r.y_scale = y_scale;
    {   /* development screenshot requested through KYUJO_CONTROL (host_control.c): the picture shown right now */
        char shot[1024];
        if (host_control_take_shot(shot, sizeof shot) != NULL) {
            static uint8_t *srgb;
            unsigned sw, sh;
            if (!srgb) srgb = (uint8_t *)malloc(1024u * 768u * 3u);
            host_control_shot_done(srgb && vi_scanout(&r, srgb, 1024, 768, &sw, &sh) && png_write_rgb(shot, srgb, sw, sh) == 0);
        }
    }
    same = sHaveLast && sLast.origin == r.origin && sLast.control == r.control;
    if (same && !sDirty) {
        return;                                              /* same frame buffer as before and nothing drawn since: not a new picture */
    }
    sDirty = 0;
    if (getenv("KYUJO_VI_TRACE")) {
        fprintf(stderr, "vi: control %08X origin %06X width %u h_video %08X v_video %08X xscale %08X yscale %08X\n", control, r.origin, width, h_video, v_video, x_scale, y_scale);
    }
    sLast = r;
    sHaveLast = 1;
    if (!same) sFrameNo++;                                   /* the frame dump numbers distinct frame buffers; a redraw into the same one is not renumbered */
    /* One conversion of the frame serves both outputs: the player's window (Windows only) and the development frame dump. */
    {
        int want_dump = !same && dir && sDumped < max && sFrameNo >= from && (sFrameNo % every) == 0, have_window;
        /* The player's window (Windows: always; Linux: when an X display could be opened; elsewhere none). Without one,
         * KYUJO_PERF_SCANOUT=1 (development) still converts every picture, so the speed monitor (host_perf.c) includes the cost
         * the window build pays for it (the window hand-over itself is not included). */
        static int perf_scanout = -1;
        int want_window = host_window_wanted();
        if (perf_scanout < 0) perf_scanout = getenv("KYUJO_PERF_SCANOUT") != NULL;
        have_window = want_window;
        want_window = want_window || perf_scanout;
        if (have_window && !want_dump && !perf_scanout && !host_menu_overlay_active()) {
            unsigned long long t1 = host_now_ns();
            if (vi_present(&r)) {                             /* the GPU shows its colour surface itself: no read-back, no waiting for it */
                host_perf_add(PERF_PICTURE, host_now_ns() - t1);
                return;
            }
        }
        if (want_dump || want_window) {
            /* heap, not a static array: the program image must stay small (all of it lies below 0x01000000, see the memory model) */
            static uint8_t *rgb;
            unsigned w, h;
            if (!rgb) rgb = (uint8_t *)malloc(1024u * 768u * 3u);
            unsigned long long t0 = host_now_ns();
            if (rgb && vi_scanout(&r, rgb, 1024, 768, &w, &h)) {
                host_menu_overlay(rgb, w, h);                 /* short messages (cheat, gamepad plugged in ...) over the picture */
                if (have_window) host_window_present(rgb, w, h);
                if (want_dump) {
                    char path[1024];
                    snprintf(path, sizeof path, "%s/frame_%05u.png", dir, sFrameNo);
                    if (png_write_rgb(path, rgb, w, h) == 0) {
                        sDumped++;
                    } else {
                        fprintf(stderr, "kyujo: cannot write %s\n", path);
                    }
                }
                host_perf_add(PERF_PICTURE, host_now_ns() - t0);
            }
        }
    }
}
