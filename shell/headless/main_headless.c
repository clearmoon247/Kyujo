/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * main_headless.c - a test shell for the sandboxed core on a desktop Linux machine: no window, no sound device.
 * It exists to prove that the WebAssembly core boots and plays, and to compare its pictures with the other builds.
 *
 *   core_headless [--gpu] [--root DIR] [--home PATH] [--frames DIR] [--every N] [--] [game arguments]
 *
 *   --gpu         draw with the OpenGL ES renderer (Mesa llvmpipe on a machine without a GPU) instead of the software renderer
 *
 *   --root DIR    the host folder that is "/" for the game (default: the current folder)
 *   --home PATH   KYUJO_HOME inside the game (default: /)
 *   --frames DIR  write every N-th picture as DIR/frame_NNNNNN.ppm   (--every, default 30)
 * Environment variables for the game (KYUJO_ROM ...) are passed on as they are when named in KYUJO_PASS (comma separated),
 * and the ones that start with KYUJO_ are always passed on.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/core_host.h"

extern char **environ;
#ifdef USE_GPU
int egl_headless_start(void);
#endif
static const char *sFramesDir;
static unsigned sEvery = 30, sCount, sSaved;
static int sGpu;

extern unsigned char *gRdpMem;
extern unsigned gRdpMemSize;
/* Development: KYUJO_PEEK="addr:len,addr:len" (hex) prints those game-memory ranges as hex lines (to stderr) every KYUJO_PEEK_EVERY pictures (default 60). */
static void peek_memory(void)
{
    const char *spec = getenv("KYUJO_PEEK");
    char buf[512], *tok, *save = NULL;
    if (!spec || !gRdpMem) return;
    snprintf(buf, sizeof buf, "%s", spec);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        unsigned long a = 0, n = 16, i;
        sscanf(tok, "%lx:%lx", &a, &n);
        if (a + n > gRdpMemSize) continue;
        fprintf(stderr, "peek %08lx:", a);
        for (i = 0; i < n; i++) fprintf(stderr, " %02x", gRdpMem[a + i]);
        fprintf(stderr, "\n");
    }
}

static void on_present(const unsigned char *rgb, unsigned w, unsigned h, void *user)
{
    static unsigned peeks;
    const char *ev = getenv("KYUJO_PEEK_EVERY");
    (void)user;
    {   /* Development: KYUJO_DUMP_AT="<picture number>:<file>,..." writes all game memory to <file> when that picture is shown */
        static unsigned shown;
        const char *spec = getenv("KYUJO_DUMP_AT");
        char buf[1024], *tok, *save = NULL;
        shown++;
        if (spec && gRdpMem) {
            snprintf(buf, sizeof buf, "%s", spec);
            for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                unsigned n = (unsigned)strtoul(tok, NULL, 10);
                char *colon = strchr(tok, ':');
                if (n == shown && colon) {
                    FILE *f = fopen(colon + 1, "wb");
                    if (f) { fwrite(gRdpMem, 1, gRdpMemSize, f); fclose(f); fprintf(stderr, "dumped game memory at picture %u to %s\n", n, colon + 1); }
                }
            }
        }
    }
    if (getenv("KYUJO_PEEK") && (peeks++ % (ev ? (unsigned)atoi(ev) : 60u)) == 0) peek_memory();
    if (sFramesDir && (sCount++ % sEvery) == 0) {
        char path[1024];
        FILE *f;
        snprintf(path, sizeof path, "%s/frame_%06u.ppm", sFramesDir, sSaved++);
        f = fopen(path, "wb");
        if (f) { fprintf(f, "P6\n%u %u\n255\n", w, h); fwrite(rgb, 3, (size_t)w * h, f); fclose(f); }
    }
}

/* development: KYUJO_AUDIO_HASH=1 prints a running hash of the sound the game made (every 2000 buffers), to compare two runs of the same script. */
static void on_audio(const void *pcm, unsigned bytes, void *user)
{
    static unsigned long long h = 1469598103934665603ULL, buffers, total;
    static int on = -1;
    const unsigned char *p = (const unsigned char *)pcm;
    unsigned i;
    (void)user;
    if (on < 0) on = getenv("KYUJO_AUDIO_HASH") != NULL;
    if (!on) return;
    for (i = 0; i < bytes; i++) h = (h ^ p[i]) * 1099511628211ULL;
    total += bytes;
    if (++buffers % 2000 == 0) fprintf(stderr, "audio hash: %llu buffers, %llu bytes, %016llx\n", buffers, total, h);
}

int main(int argc, char **argv)
{
    CoreFront fr;
    const char *root = ".", *home = "/";
    int i, gargc = 1;
    char **gargv = calloc((size_t)argc + 2, sizeof *gargv);
    gargv[0] = "kyujo";
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--home") && i + 1 < argc) home = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) sFramesDir = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) sEvery = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gpu")) sGpu = 1;
        else if (!strcmp(argv[i], "--")) { while (++i < argc) gargv[gargc++] = argv[i]; }
        else gargv[gargc++] = argv[i];
    }
    memset(&fr, 0, sizeof fr);
    fr.present = on_present;
    fr.audio = on_audio;
    fr.root_dir = root;
    fr.home = home;
    if (core_init(&fr) != 0) { fprintf(stderr, "core_headless: cannot start the core\n"); return 1; }
    core_pass_environment(environ);
#ifdef USE_GPU
    if (sGpu && egl_headless_start() != 0) { fprintf(stderr, "core_headless: the GPU renderer could not be started\n"); return 2; }
#else
    if (sGpu) { fprintf(stderr, "core_headless: built without --gpu support\n"); return 2; }
#endif
    return core_run(gargc, gargv);
}
