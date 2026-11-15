/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * switch_compat.c - the three C library functions that devkitA64's newlib declares or that the shared shell code uses, but that libnx does not provide:
 * sysconf (wabt's runtime asks for the page size once), pread and pwrite (WASI positioned reads and writes in shell/core/wasi_host.c).
 * pread/pwrite are lseek + read/write + lseek back under one lock: the file position of a descriptor is shared, and game threads may use one file at a time.
 */
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE: return 4096;
    case _SC_NPROCESSORS_ONLN: return 4;
    default: errno = EINVAL; return -1;
    }
}

static pthread_mutex_t sPosLock = PTHREAD_MUTEX_INITIALIZER;

ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    off_t old;
    ssize_t r;
    pthread_mutex_lock(&sPosLock);
    old = lseek(fd, 0, SEEK_CUR);
    if (old < 0 || lseek(fd, off, SEEK_SET) < 0) { pthread_mutex_unlock(&sPosLock); return -1; }
    r = read(fd, buf, n);
    lseek(fd, old, SEEK_SET);
    pthread_mutex_unlock(&sPosLock);
    return r;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    off_t old;
    ssize_t r;
    pthread_mutex_lock(&sPosLock);
    old = lseek(fd, 0, SEEK_CUR);
    if (old < 0 || lseek(fd, off, SEEK_SET) < 0) { pthread_mutex_unlock(&sPosLock); return -1; }
    r = write(fd, buf, n);
    lseek(fd, old, SEEK_SET);
    pthread_mutex_unlock(&sPosLock);
    return r;
}
