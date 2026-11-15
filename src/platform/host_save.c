/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_save.c - the cartridge's save memory as a file on disk.
 *
 * The game keeps its save data in a flash chip on the cartridge (emulated by src/ultra/hw_flash.c). This file only stores and loads
 * the chip's bytes. The file is the player's data: it is never part of the repository (.gitignore lists *.sav).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#endif

static const char *sPath = "kyujo.sav";
static char sPathBuf[1024];

static void write_now(const void *src, unsigned int size);
static const void *sPendSrc;                          /* a store that is not on disk yet (write-behind, see host_save_store) */
static unsigned int sPendSize;
static unsigned long long sPendFirst, sPendLast;

void host_save_set_path(const char *path)
{
    host_save_flush();                                /* what is waiting belongs to the file in use now */
    if (strlen(path) >= sizeof sPathBuf) { fprintf(stderr, "kyujo: save path too long, keeping the default\n"); return; }
    strcpy(sPathBuf, path);
    sPath = sPathBuf;
}

const char *host_save_path(void) { return sPath; }

/* Can a save file be made at `path`? Tried the way host_save_store writes (a temporary neighbour, then removed). A settings file copied from
 * another computer names folders that do not exist here; the caller then falls back to the default place instead of failing at every save. */
int host_save_can_write(const char *path)
{
    char tmp[sizeof sPathBuf + 8];
    FILE *f;
    if (strlen(path) >= sizeof sPathBuf) return 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return 0;
    fclose(f);
    remove(tmp);
    return 1;
}

unsigned int host_save_load(void *dst, unsigned int size)
{
    FILE *f = fopen(sPath, "rb");
    long n;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n != (long)size) {
        fprintf(stderr, "kyujo: ignoring save file '%s': it is %ld bytes, expected %u\n", sPath, n, size);
        fclose(f);
        return 0;
    }
    if (fread(dst, 1, size, f) != size) {
        fprintf(stderr, "kyujo: could not read the save file '%s'; starting with blank save memory\n", sPath);
        fclose(f);
        return 0;
    }
    fclose(f);
    return size;
}

/*
 * Write-behind.  A save is made of hundreds of flash commands (one per 128-byte page and one per sector erase) and every one of them used to
 * write the whole 128 KiB file through a temporary neighbour and a rename. On a Switch's SD card that is about 200 ms each: the Switch tester's log
 * shows 40 and more such writes in a row and a picture that stood still for 11 seconds while the game "saved".  Now host_save_store only
 * remembers that the chip changed; the file is written when the game has stopped changing it for QUIET_NS, at the latest MAX_NS after the first
 * change (host_save_poll, once per video field), when the program ends or restarts (host_save_flush, via the exit hook) and when the file in use
 * changes.  The chip's bytes live in hw_flash.c's array for the whole run, so keeping the pointer is enough.
 */
#define SAVE_QUIET_NS 300000000ULL
#define SAVE_MAX_NS  2000000000ULL

void host_save_store(const void *src, unsigned int size)
{
    unsigned long long now = host_now_ns();
    if (!sPendSrc) sPendFirst = now;
    sPendSrc = src;
    sPendSize = size;
    sPendLast = now;
}

void host_save_flush(void)
{
    if (sPendSrc) {
        const void *src = sPendSrc;
        sPendSrc = NULL;
        write_now(src, sPendSize);
    }
}

void host_save_poll(void)
{
    if (sPendSrc) {
        unsigned long long now = host_now_ns();
        if (now - sPendLast >= SAVE_QUIET_NS || now - sPendFirst >= SAVE_MAX_NS) host_save_flush();
    }
}

static void write_now(const void *src, unsigned int size)
{
    char tmp[sizeof sPathBuf + 8];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", sPath);
    f = fopen(tmp, "wb");
    if (!f || fwrite(src, 1, size, f) != size || fflush(f) != 0) {
        static int sFailures;                          /* the game saves often; a bad folder must not fill the log (one test log had 250 identical lines) */
        if (sFailures++ < 3) fprintf(stderr, "kyujo: could not write the save file '%s'%s\n", tmp, sFailures == 3 ? " (further failures are not logged)" : "");
        if (f) fclose(f);
        return;
    }
    fclose(f);
#ifdef _WIN32
    /* C's rename() refuses to replace an existing file on Windows; MoveFileEx can, atomically on the same volume. */
    if (!MoveFileExA(tmp, sPath, MOVEFILE_REPLACE_EXISTING)) fprintf(stderr, "kyujo: could not replace the save file '%s'\n", sPath);
#else
    if (rename(tmp, sPath) != 0) fprintf(stderr, "kyujo: could not replace the save file '%s'\n", sPath);
#endif
}
