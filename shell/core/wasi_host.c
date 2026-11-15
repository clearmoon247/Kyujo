/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * wasi_host.c - the WASI system calls ("wasi_snapshot_preview1") that the core's C library (wasi-libc) makes, over plain POSIX calls.
 *
 * Why not an existing implementation: wabt's wasm2c ships none; the one it uses for its interpreter (uvwasi) is built on libuv, which has no Horizon
 * (Switch) port. This file is the small subset a game needs (files, directories, clock, arguments, environment, random, sleep), written against the
 * WASI preview-1 specification and nothing but POSIX (no openat/fstatat: the Switch's SD card driver does not have them, so paths are joined by hand).
 *
 * The game sees ONE pre-opened folder, "/", which is Core.front.root_dir on the host (Linux test shell: a directory; Switch: "sdmc:/"). A path that
 * would leave it with ".." is refused. Everything else a WASI program may ask for (sockets, links, rights, advice) answers "not supported" (52) so the
 * C library reports an ordinary error.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "core_host.h"
#include "wasm-rt.h"
#include "kyujo.h"
#include "core_internal.h"

enum { W_OK_ = 0, W_ACCES = 2, W_BADF = 8, W_EXIST = 20, W_FAULT = 21, W_INVAL = 28, W_IO = 29, W_ISDIR = 31, W_MFILE = 33, W_NAMETOOLONG = 37,
       W_NOENT = 44, W_NOMEM = 48, W_NOSPC = 51, W_NOSYS = 52, W_NOTDIR = 54, W_NOTEMPTY = 55, W_PERM = 63, W_SPIPE = 70, W_NOTCAPABLE = 76 };

static uint32_t wasi_errno(int e)
{
    switch (e) {
    case 0: return W_OK_;
    case EACCES: return W_ACCES;
    case EBADF: return W_BADF;
    case EEXIST: return W_EXIST;
    case EINVAL: return W_INVAL;
    case EISDIR: return W_ISDIR;
    case EMFILE: return W_MFILE;
    case ENAMETOOLONG: return W_NAMETOOLONG;
    case ENOENT: return W_NOENT;
    case ENOMEM: return W_NOMEM;
    case ENOSPC: return W_NOSPC;
    case ENOTDIR: return W_NOTDIR;
    case ENOTEMPTY: return W_NOTEMPTY;
    case EPERM: return W_PERM;
    case ESPIPE: return W_SPIPE;
    case EROFS: return 69;
    case EXDEV: return 75;
    default: return W_IO;
    }
}

/* ---- the file descriptor table ------------------------------------------------------------------------------------------------------ */
typedef struct {
    int used;
    int fd;              /* host descriptor, -1 for a folder (opened by path) */
    int is_dir;
    int is_tty;          /* the standard streams */
    char *path;          /* full host path (folders and files) */
} WFd;

#define MAX_FD 128
static WFd sFd[MAX_FD];
static char sRoot[1024];
static char **sEnv;
static unsigned sEnvN;

/* ---- access to the module's memory ------------------------------------------------------------------------------------------------------ */
static uint8_t *at(uint32_t addr, uint64_t len)
{
    wasm_rt_memory_t *m = w2c_kyujo_memory(&gCore.inst);
    if ((uint64_t)addr + len > m->size) return NULL;
    return m->data + addr;
}
static int put32(uint32_t addr, uint32_t v) { uint8_t *p = at(addr, 4); if (!p) return 0; memcpy(p, &v, 4); return 1; }
static int put64(uint32_t addr, uint64_t v) { uint8_t *p = at(addr, 8); if (!p) return 0; memcpy(p, &v, 8); return 1; }

int wasi_init(Core *c, const char *root_dir)
{
    size_t n;
    (void)c;
    snprintf(sRoot, sizeof sRoot, "%s", root_dir && root_dir[0] ? root_dir : ".");
    n = strlen(sRoot);
    while (n > 1 && sRoot[n - 1] == '/' && sRoot[n - 2] != ':') sRoot[--n] = 0;       /* "sdmc:/" keeps its slash, "/a/b/" loses it */
    sFd[0] = (WFd){1, 0, 0, 1, NULL};
    sFd[1] = (WFd){1, 1, 0, 1, NULL};
    sFd[2] = (WFd){1, 2, 0, 1, NULL};
    sFd[3] = (WFd){1, -1, 1, 0, strdup(sRoot)};                 /* the pre-opened folder "/" */
    return 0;
}

/* The game is being started again inside the same process (core_reset): its open files are closed, so the table is as after wasi_init. The environment stays. */
void wasi_reset(Core *c, const char *root_dir)
{
    int i;
    for (i = 4; i < MAX_FD; i++) {
        if (sFd[i].used && sFd[i].fd >= 0) close(sFd[i].fd);
        free(sFd[i].path);
        memset(&sFd[i], 0, sizeof sFd[i]);
    }
    free(sFd[3].path);
    wasi_init(c, root_dir);
}

void core_setenv(const char *name, const char *value)
{
    char *s = malloc(strlen(name) + strlen(value) + 2);
    sprintf(s, "%s=%s", name, value);
    sEnv = realloc(sEnv, (sEnvN + 1) * sizeof *sEnv);
    sEnv[sEnvN++] = s;
}

/* Host path of `rel` (as the game gives it) below folder descriptor `dirfd`. Refuses ".." and fills `out`. Returns a WASI errno. */
static uint32_t join_path(uint32_t dirfd, uint32_t ptr, uint32_t len, char *out, size_t size)
{
    const uint8_t *p = at(ptr, len);
    char rel[1024];
    size_t i, j = 0;
    const char *base;
    if (!p || len >= sizeof rel) return len >= sizeof rel ? W_NAMETOOLONG : W_FAULT;
    if (dirfd >= MAX_FD || !sFd[dirfd].used || !sFd[dirfd].is_dir || !sFd[dirfd].path) return W_BADF;
    for (i = 0; i < len; i++) rel[j++] = (char)p[i];
    rel[j] = 0;
    for (i = 0; rel[i];) {                                       /* component by component: no ".." (nothing may leave the pre-opened folder) */
        size_t k = i;
        while (rel[k] && rel[k] != '/') k++;
        if (k - i == 2 && rel[i] == '.' && rel[i + 1] == '.') return W_NOTCAPABLE;
        i = rel[k] ? k + 1 : k;
    }
    base = sFd[dirfd].path;
    {
        size_t bl = strlen(base);
        const char *r = rel;
        while (*r == '/') r++;
        if (snprintf(out, size, "%s%s%s", base, (bl && base[bl - 1] != '/') ? "/" : "", r) >= (int)size) return W_NAMETOOLONG;
        bl = strlen(out);
        while (bl > 1 && out[bl - 1] == '/' && out[bl - 2] != ':') out[--bl] = 0;
        if (out[0] == 0) strcpy(out, ".");
    }
    return W_OK_;
}

static int alloc_fd(void)
{
    int i;
    for (i = 4; i < MAX_FD; i++) if (!sFd[i].used) return i;
    return -1;
}

static uint8_t filetype_of(const struct stat *st)
{
    if (S_ISDIR(st->st_mode)) return 3;
    if (S_ISREG(st->st_mode)) return 4;
    if (S_ISCHR(st->st_mode)) return 2;
    if (S_ISBLK(st->st_mode)) return 1;
#ifdef S_ISLNK
    if (S_ISLNK(st->st_mode)) return 7;
#endif
    return 0;
}

static void write_filestat(uint32_t out, const struct stat *st)
{
    uint8_t *p = at(out, 64);
    uint64_t v;
    if (!p) return;
    memset(p, 0, 64);
    v = (uint64_t)st->st_dev;   memcpy(p + 0, &v, 8);
    v = (uint64_t)st->st_ino;   memcpy(p + 8, &v, 8);
    p[16] = filetype_of(st);
    v = (uint64_t)st->st_nlink; memcpy(p + 24, &v, 8);
    v = (uint64_t)st->st_size;  memcpy(p + 32, &v, 8);
    v = (uint64_t)st->st_atime * 1000000000ULL; memcpy(p + 40, &v, 8);
    v = (uint64_t)st->st_mtime * 1000000000ULL; memcpy(p + 48, &v, 8);
    v = (uint64_t)st->st_ctime * 1000000000ULL; memcpy(p + 56, &v, 8);
}

/* ---- arguments and environment ------------------------------------------------------------------------------------------------------ */
static uint32_t strings_get(char **list, unsigned n, uint32_t ptrs, uint32_t buf)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        size_t l = strlen(list[i]) + 1;
        uint8_t *d = at(buf, l);
        if (!d || !put32(ptrs + 4 * i, buf)) return W_FAULT;
        memcpy(d, list[i], l);
        buf += (uint32_t)l;
    }
    return W_OK_;
}
static uint32_t strings_sizes(char **list, unsigned n, uint32_t count_out, uint32_t size_out)
{
    unsigned i, total = 0;
    for (i = 0; i < n; i++) total += (unsigned)strlen(list[i]) + 1;
    return put32(count_out, n) && put32(size_out, total) ? W_OK_ : W_FAULT;
}

uint32_t w2c_wasi__snapshot__preview1_args_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b) { (void)w; return strings_get(gCore.argv, (unsigned)gCore.argc, a, b); }
uint32_t w2c_wasi__snapshot__preview1_args_sizes_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b) { (void)w; return strings_sizes(gCore.argv, (unsigned)gCore.argc, a, b); }
uint32_t w2c_wasi__snapshot__preview1_environ_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b) { (void)w; return strings_get(sEnv, sEnvN, a, b); }
uint32_t w2c_wasi__snapshot__preview1_environ_sizes_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b) { (void)w; return strings_sizes(sEnv, sEnvN, a, b); }

/* ---- clock, random, sleep, exit ----------------------------------------------------------------------------------------------------- */
static clockid_t clock_of(uint32_t id)
{
    switch (id) {
    case 0: return CLOCK_REALTIME;
    case 1: return CLOCK_MONOTONIC;
    default: return CLOCK_MONOTONIC;                             /* process/thread CPU time: not meaningful here */
    }
}

uint32_t w2c_wasi__snapshot__preview1_clock_res_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t id, uint32_t out)
{
    (void)w; (void)id;
    return put64(out, 1000) ? W_OK_ : W_FAULT;
}

uint32_t w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t id, uint64_t precision, uint32_t out)
{
    struct timespec t;
    (void)w; (void)precision;
    clock_gettime(clock_of(id), &t);
    return put64(out, (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec) ? W_OK_ : W_FAULT;
}

uint32_t w2c_wasi__snapshot__preview1_random_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t buf, uint32_t len)
{
    uint8_t *p = at(buf, len);
    FILE *f;
    (void)w;
    if (!p) return W_FAULT;
    f = fopen("/dev/urandom", "rb");
    if (f) { size_t n = fread(p, 1, len, f); fclose(f); if (n == len) return W_OK_; }
    { uint32_t i, x = (uint32_t)time(NULL) ^ 0x9E3779B9u; for (i = 0; i < len; i++) { x = x * 1664525u + 1013904223u; p[i] = (uint8_t)(x >> 24); } }   /* last resort: not secure; the game needs no secrets */
    return W_OK_;
}

void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1 *w, uint32_t code)
{
    (void)w;
    gCore.exit_code = (int)code;
    core_exit((int)code);
}

uint32_t w2c_wasi__snapshot__preview1_sched_yield(struct w2c_wasi__snapshot__preview1 *w)
{
    (void)w;
    core_lock_drop();
    sched_yield();
    core_lock_take();
    return W_OK_;
}

/* poll_oneoff: only clock subscriptions are supported (that is what sleep() and nanosleep() use); the game's files are never waited on. */
uint32_t w2c_wasi__snapshot__preview1_poll_oneoff(struct w2c_wasi__snapshot__preview1 *w, uint32_t in, uint32_t out, uint32_t n, uint32_t nevents)
{
    uint32_t i;
    uint64_t wait_ns = ~0ULL;
    struct timespec now;
    (void)w;
    if (n == 0) return W_INVAL;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (i = 0; i < n; i++) {
        const uint8_t *s = at(in + 48 * i, 48);
        uint64_t timeout, ns;
        uint16_t flags;
        uint32_t id;
        if (!s) return W_FAULT;
        if (s[8] != 0) return W_NOSYS;                              /* not a clock subscription */
        memcpy(&id, s + 16, 4); memcpy(&timeout, s + 24, 8); memcpy(&flags, s + 40, 2);
        ns = timeout;
        if (flags & 1) {                                            /* absolute time */
            struct timespec c;
            uint64_t cur;
            clock_gettime(clock_of(id), &c);
            cur = (uint64_t)c.tv_sec * 1000000000ULL + (uint64_t)c.tv_nsec;
            ns = timeout > cur ? timeout - cur : 0;
        }
        if (ns < wait_ns) wait_ns = ns;
    }
    if (wait_ns) {
        struct timespec t;
        t.tv_sec = (time_t)(wait_ns / 1000000000ULL);
        t.tv_nsec = (long)(wait_ns % 1000000000ULL);
        core_lock_drop();
        nanosleep(&t, NULL);
        core_lock_take();
    }
    {
        uint8_t *e = at(out, 32);
        const uint8_t *s0 = at(in, 48);
        if (!e || !s0) return W_FAULT;
        memset(e, 0, 32);
        memcpy(e, s0, 8);                                           /* userdata */
        e[10] = 0;                                                  /* type: clock */
    }
    return put32(nevents, 1) ? W_OK_ : W_FAULT;
}

/* ---- file operations ---------------------------------------------------------------------------------------------------------------- */
#define GETFD(var, num) WFd *var; if ((num) >= MAX_FD || !sFd[(num)].used) return W_BADF; var = &sFd[(num)]

static uint32_t io_vec(uint32_t fdn, uint32_t iovs, uint32_t iovs_len, uint32_t nout, int writing, int64_t offset)
{
    uint32_t i;
    uint64_t total = 0;
    GETFD(f, fdn);
    if (f->is_dir) return W_ISDIR;
    for (i = 0; i < iovs_len; i++) {
        uint32_t base, len;
        uint8_t *p, *iov = at(iovs + 8 * i, 8);
        ssize_t r;
        if (!iov) return W_FAULT;
        memcpy(&base, iov, 4); memcpy(&len, iov + 4, 4);
        if (len == 0) continue;
        p = at(base, len);
        if (!p) return W_FAULT;
        core_note(writing ? "file write" : "file read");
        if (offset >= 0) r = writing ? pwrite(f->fd, p, len, (off_t)(offset + (int64_t)total)) : pread(f->fd, p, len, (off_t)(offset + (int64_t)total));
        else r = writing ? write(f->fd, p, len) : read(f->fd, p, len);
        if (r < 0) { if (total) break; return wasi_errno(errno); }
        total += (uint64_t)r;
        if ((uint32_t)r < len) break;
    }
    return put32(nout, (uint32_t)total) ? W_OK_ : W_FAULT;
}

uint32_t w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t iovs, uint32_t n, uint32_t out) { (void)w; return io_vec(fd, iovs, n, out, 0, -1); }
uint32_t w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t iovs, uint32_t n, uint32_t out) { (void)w; return io_vec(fd, iovs, n, out, 1, -1); }
uint32_t w2c_wasi__snapshot__preview1_fd_pread(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t iovs, uint32_t n, uint64_t off, uint32_t out) { (void)w; return io_vec(fd, iovs, n, out, 0, (int64_t)off); }
uint32_t w2c_wasi__snapshot__preview1_fd_pwrite(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t iovs, uint32_t n, uint64_t off, uint32_t out) { (void)w; return io_vec(fd, iovs, n, out, 1, (int64_t)off); }

uint32_t w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint64_t off, uint32_t whence, uint32_t out)
{
    off_t r;
    int wh = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END;
    (void)w;
    { GETFD(f, fd); if (f->is_dir || f->is_tty) return W_SPIPE; r = lseek(f->fd, (off_t)(int64_t)off, wh); }
    if (r < 0) return wasi_errno(errno);
    return put64(out, (uint64_t)r) ? W_OK_ : W_FAULT;
}

uint32_t w2c_wasi__snapshot__preview1_fd_tell(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t out)
{
    return w2c_wasi__snapshot__preview1_fd_seek(w, fd, 0, 1, out);
}

uint32_t w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd)
{
    (void)w;
    { GETFD(f, fd);
      if (fd < 4) return W_OK_;                                     /* the standard streams and the pre-opened folder stay */
      if (f->fd >= 0) close(f->fd);
      free(f->path);
      memset(f, 0, sizeof *f); }
    return W_OK_;
}

uint32_t w2c_wasi__snapshot__preview1_fd_sync(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd)
{
    (void)w;
    { GETFD(f, fd); if (f->fd >= 0) fsync(f->fd); }
    return W_OK_;
}
uint32_t w2c_wasi__snapshot__preview1_fd_datasync(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd) { return w2c_wasi__snapshot__preview1_fd_sync(w, fd); }

uint32_t w2c_wasi__snapshot__preview1_fd_fdstat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t out)
{
    uint8_t *p = at(out, 24);
    uint64_t all = 0x1FFFFFFFULL;
    (void)w;
    { GETFD(f, fd);
      if (!p) return W_FAULT;
      memset(p, 0, 24);
      p[0] = f->is_dir ? 3 : f->is_tty ? 2 : 4;
      memcpy(p + 8, &all, 8);
      memcpy(p + 16, &all, 8); }
    return W_OK_;
}

uint32_t w2c_wasi__snapshot__preview1_fd_fdstat_set_flags(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t flags) { (void)w; (void)fd; (void)flags; return W_OK_; }

uint32_t w2c_wasi__snapshot__preview1_fd_filestat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t out)
{
    struct stat st;
    int r;
    (void)w;
    { GETFD(f, fd); r = f->fd >= 0 ? fstat(f->fd, &st) : stat(f->path, &st); }
    if (r != 0) return wasi_errno(errno);
    write_filestat(out, &st);
    return W_OK_;
}

uint32_t w2c_wasi__snapshot__preview1_fd_filestat_set_size(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint64_t size)
{
    (void)w;
    { GETFD(f, fd); if (f->fd < 0 || ftruncate(f->fd, (off_t)size) != 0) return wasi_errno(errno); }
    return W_OK_;
}

uint32_t w2c_wasi__snapshot__preview1_fd_prestat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t out)
{
    uint8_t *p = at(out, 8);
    (void)w;
    if (fd != 3) return W_BADF;                                     /* ends the library's search for pre-opened folders */
    if (!p) return W_FAULT;
    memset(p, 0, 8);
    p[0] = 0;                                                       /* tag: directory */
    { uint32_t l = 1; memcpy(p + 4, &l, 4); }                       /* the name is "/" */
    return W_OK_;
}

uint32_t w2c_wasi__snapshot__preview1_fd_prestat_dir_name(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t buf, uint32_t len)
{
    uint8_t *p = at(buf, len);
    (void)w;
    if (fd != 3) return W_BADF;
    if (!p || len < 1) return W_FAULT;
    p[0] = '/';
    return W_OK_;
}

/* WASI directory entry: d_next u64, d_ino u64, d_namlen u32, d_type u8, 3 bytes padding (24 bytes), then the name (not terminated). */
uint32_t w2c_wasi__snapshot__preview1_fd_readdir(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t buf, uint32_t buf_len, uint64_t cookie, uint32_t out)
{
    DIR *d;
    struct dirent *e;
    uint64_t index = 0;
    uint32_t used = 0;
    uint8_t *dst = at(buf, buf_len);
    (void)w;
    { GETFD(f, fd); if (!f->is_dir) return W_NOTDIR; d = opendir(f->path); }
    if (!d) return wasi_errno(errno);
    if (!dst) { closedir(d); return W_FAULT; }
    while ((e = readdir(d)) != NULL) {
        uint8_t ent[24];
        uint32_t nl = (uint32_t)strlen(e->d_name), room;
        uint64_t next = index + 1, ino = 0;
        uint8_t type = 0;
        if (index++ < cookie) continue;
#ifdef DT_DIR
        type = e->d_type == DT_DIR ? 3 : e->d_type == DT_REG ? 4 : e->d_type == DT_LNK ? 7 : e->d_type == DT_CHR ? 2 : e->d_type == DT_BLK ? 1 : 0;
#endif
        memset(ent, 0, sizeof ent);
        memcpy(ent, &next, 8);
        memcpy(ent + 8, &ino, 8);
        memcpy(ent + 16, &nl, 4);
        ent[20] = type;
        room = buf_len - used;
        { uint32_t c = room < 24 ? room : 24; memcpy(dst + used, ent, c); used += c; room -= c; }
        if (room < nl) { uint32_t c = room; memcpy(dst + used, e->d_name, c); used += c; break; }     /* a cut name tells the library to call again with a bigger buffer */
        memcpy(dst + used, e->d_name, nl);
        used += nl;
        if (used >= buf_len) break;
    }
    closedir(d);
    return put32(out, used) ? W_OK_ : W_FAULT;
}

uint32_t w2c_wasi__snapshot__preview1_path_open(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd, uint32_t dirflags, uint32_t path, uint32_t plen,
                                                uint32_t oflags, uint64_t rights_base, uint64_t rights_inh, uint32_t fdflags, uint32_t out)
{
    char full[1100];
    uint32_t e;
    struct stat st;
    int fl = 0, hfd, n;
    int want_dir = (oflags & 2) != 0;
    (void)w; (void)dirflags; (void)rights_inh;
    e = join_path(dirfd, path, plen, full, sizeof full);
    if (e) return e;
    n = alloc_fd();
    if (n < 0) return W_MFILE;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {              /* a folder: opened by path, there is no descriptor to keep */
        if ((oflags & 1) && (oflags & 4)) return W_EXIST;
        sFd[n] = (WFd){1, -1, 1, 0, strdup(full)};
        return put32(out, (uint32_t)n) ? W_OK_ : W_FAULT;
    }
    if (want_dir) return stat(full, &st) == 0 ? W_NOTDIR : W_NOENT;
    {
        int rd = (rights_base & (1u << 1)) != 0, wr = (rights_base & ((1u << 6) | (1u << 22))) != 0;     /* FD_READ; FD_WRITE or FD_FILESTAT_SET_SIZE */
        fl = rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    }
    if (oflags & 1) fl |= O_CREAT;
    if (oflags & 4) fl |= O_EXCL;
    if (oflags & 8) fl |= O_TRUNC;
    if (fdflags & 1) fl |= O_APPEND;
#ifdef O_BINARY
    fl |= O_BINARY;
#endif
    core_note("file open");
    hfd = open(full, fl, 0644);
    {
        static int logged;
        if (logged < 60) { logged++; fprintf(stderr, "wasi: open '%s' flags 0x%X -> %s\n", full, (unsigned)fl, hfd < 0 ? strerror(errno) : "ok"); }
    }
    if (hfd < 0) return wasi_errno(errno);
    sFd[n] = (WFd){1, hfd, 0, 0, strdup(full)};
    return put32(out, (uint32_t)n) ? W_OK_ : W_FAULT;
}

uint32_t w2c_wasi__snapshot__preview1_path_filestat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd, uint32_t flags, uint32_t path, uint32_t plen, uint32_t out)
{
    char full[1100];
    struct stat st;
    uint32_t e = join_path(dirfd, path, plen, full, sizeof full);
    (void)w; (void)flags;
    if (e) return e;
    if (stat(full, &st) != 0) return wasi_errno(errno);
    write_filestat(out, &st);
    return W_OK_;
}

uint32_t w2c_wasi__snapshot__preview1_path_create_directory(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd, uint32_t path, uint32_t plen)
{
    char full[1100];
    uint32_t e = join_path(dirfd, path, plen, full, sizeof full);
    (void)w;
    if (e) return e;
    return mkdir(full, 0755) == 0 ? W_OK_ : wasi_errno(errno);
}

uint32_t w2c_wasi__snapshot__preview1_path_remove_directory(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd, uint32_t path, uint32_t plen)
{
    char full[1100];
    uint32_t e = join_path(dirfd, path, plen, full, sizeof full);
    (void)w;
    if (e) return e;
    return rmdir(full) == 0 ? W_OK_ : wasi_errno(errno);
}

uint32_t w2c_wasi__snapshot__preview1_path_unlink_file(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd, uint32_t path, uint32_t plen)
{
    char full[1100];
    uint32_t e = join_path(dirfd, path, plen, full, sizeof full);
    (void)w;
    if (e) return e;
    return unlink(full) == 0 ? W_OK_ : wasi_errno(errno);
}

uint32_t w2c_wasi__snapshot__preview1_path_rename(struct w2c_wasi__snapshot__preview1 *w, uint32_t odir, uint32_t opath, uint32_t olen, uint32_t ndir, uint32_t npath, uint32_t nlen)
{
    char a[1100], b[1100];
    uint32_t e = join_path(odir, opath, olen, a, sizeof a);
    (void)w;
    if (e) return e;
    e = join_path(ndir, npath, nlen, b, sizeof b);
    if (e) return e;
    if (rename(a, b) == 0) return W_OK_;
    if (errno == EEXIST || errno == EACCES) {                       /* some SD card drivers refuse to replace a file: the game's save writer relies on replacing */
        if (unlink(b) == 0 && rename(a, b) == 0) return W_OK_;
    }
    return wasi_errno(errno);
}

/* ---- everything else: "not supported" ------------------------------------------------------------------------------------------------ */
#define NOSYS0(name) uint32_t w2c_wasi__snapshot__preview1_##name(struct w2c_wasi__snapshot__preview1 *w) { (void)w; return W_NOSYS; }
uint32_t w2c_wasi__snapshot__preview1_fd_advise(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint64_t b, uint64_t c, uint32_t d) { (void)w; (void)a; (void)b; (void)c; (void)d; return W_OK_; }
uint32_t w2c_wasi__snapshot__preview1_fd_allocate(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint64_t b, uint64_t c) { (void)w; (void)a; (void)b; (void)c; return W_NOSYS; }
uint32_t w2c_wasi__snapshot__preview1_fd_fdstat_set_rights(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint64_t b, uint64_t c) { (void)w; (void)a; (void)b; (void)c; return W_OK_; }
uint32_t w2c_wasi__snapshot__preview1_fd_filestat_set_times(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint64_t b, uint64_t c, uint32_t d) { (void)w; (void)a; (void)b; (void)c; (void)d; return W_OK_; }
uint32_t w2c_wasi__snapshot__preview1_fd_renumber(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b) { (void)w; (void)a; (void)b; return W_NOSYS; }
uint32_t w2c_wasi__snapshot__preview1_path_filestat_set_times(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint64_t e, uint64_t f, uint32_t g) { (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return W_OK_; }
uint32_t w2c_wasi__snapshot__preview1_path_link(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g) { (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return W_NOSYS; }
uint32_t w2c_wasi__snapshot__preview1_path_readlink(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) { (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return W_INVAL; }
uint32_t w2c_wasi__snapshot__preview1_path_symlink(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) { (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; return W_NOSYS; }
uint32_t w2c_wasi__snapshot__preview1_sock_accept(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b, uint32_t c) { (void)w; (void)a; (void)b; (void)c; return W_NOSYS; }
uint32_t w2c_wasi__snapshot__preview1_sock_recv(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) { (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return W_NOSYS; }
uint32_t w2c_wasi__snapshot__preview1_sock_send(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) { (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; return W_NOSYS; }
uint32_t w2c_wasi__snapshot__preview1_sock_shutdown(struct w2c_wasi__snapshot__preview1 *w, uint32_t a, uint32_t b) { (void)w; (void)a; (void)b; return W_NOSYS; }
