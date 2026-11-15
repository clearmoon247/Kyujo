/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * core_host.c - the native side of the sandboxed core: instance, the calls the core makes into the shell, and the lock.
 * See core_host.h. The WASI system calls are in wasi_host.c.
 *
 * THREADS. The game has several threads (the libultra scheduler's), each of them an operating-system thread here, but only one runs game code at a
 * time (the "baton", src/platform/host_sched.c). WebAssembly keeps its call stack on the native stack of the thread that runs it, but also a second
 * stack in its memory (the "shadow stack", pointer in the global __stack_pointer) that belongs to the module instance, not to the thread. So:
 *   - ONE lock (the "wasm lock") is held by whichever thread runs code of the module. Every call below that can wait releases it first and takes it
 *     back afterwards.
 *   - Each thread keeps its own shadow-stack pointer: saved when it releases the lock, restored when it takes the lock again.
 * A thread started by the core gets a shadow stack that the core allocated (stack_top in thread_start).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core_host.h"
#include "wasm-rt.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"
#include "kyujo.h"
#include "core_internal.h"

Core gCore;

static pthread_mutex_t sWasmLock = PTHREAD_MUTEX_INITIALIZER;
static __thread uint32_t tSavedSp;          /* this thread's shadow stack pointer while it does not hold the lock */
static __thread int tHasLock;

/* ---- fairness of the lock ------------------------------------------------------------------------------------------------------------------------
 * The game's threads run one at a time (the baton, src/ultra/os_sched.c) and the console's interrupts are noticed at "scheduling points": when
 * the running code calls the OS, or at the loop back-edges that look at a flag that device threads set (be_memory.py rule 5). But the device threads
 * (the video timer, the audio device, ...) run wasm code too, and so need this lock to set that flag at all. A game thread that spins waiting for the
 * audio to go quiet (Audio_WaitForIdle: a loop of function calls that never calls the OS) kept the lock for ever and the audio could not run. The
 * generated code therefore asks at every loop header whether another thread has waited for the lock for a while (YIELD_POINT in build_shell.py), and
 * the holder then hands the lock over and takes it back. A thread that has to wait for the lock raises the request at once. */
volatile int gYieldReq;
static atomic_int sWaiters;
static atomic_uint sLockGen;                      /* counts acquisitions (changed by whoever takes the lock) */
static atomic_int sHeld;                          /* the lock is held (for the sampler below) */
static unsigned long long sHeldSinceUs;           /* when the holder took it (written by the holder) */
static unsigned long long now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000ull + (unsigned long long)t.tv_nsec / 1000u;
}

/* ---- sampling of where the game's code spends its time (needs --trace-fn: the ring of functions entered last) ----------------------------------------
 * A thread samples the newest ring entry every millisecond, together with whether the lock was held. core_profile_report prints the top entries. */
extern volatile unsigned int gFnRing[64], gFnPos;
#define PROF_N 8192
static unsigned sProfFn[PROF_N], sProfCnt[PROF_N], sProfFree, sProfTotal;
static pthread_mutex_t sProfMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t sTickerOnce = PTHREAD_ONCE_INIT;

/* ---- leaving: the game's threads must all be gone before the process ends -------------------------------------------------------------------------
 * On the Switch the .nro runs inside hbmenu's loader process. When the program returns, the loader unmaps its code and loads the next .nro (hbmenu, or this
 * program again after "Restart game") into the same process. A thread of the old program that is still alive (asleep in a timed wait, or woken by a signal on
 * a reused heap address) then runs unmapped code and the console shows a crash. core_threads_stop() therefore makes every thread the core started leave on its
 * own: each one checks sShutdown wherever it comes back from the host into the game, and exits there. */
static atomic_int sShutdown;
static atomic_uint sGeneration = 1;                /* counts the instances made by core_init and core_reset (a thread's shadow stack belongs to one of them) */
static atomic_int sLive;                          /* threads started by the core (game threads, device threads, the sampler) that have not left yet */
static __thread int tCounted;
static void thread_die(void)                      /* called without the wasm lock */
{
    tHasLock = 0;
    if (tCounted) { tCounted = 0; atomic_fetch_sub(&sLive, 1); }
    pthread_exit(NULL);
}

static void *sampler_main(void *arg)
{
    (void)arg;
    tCounted = 1;
    for (;;) {
        struct timespec t = { 0, 1000000 };
        unsigned pos, fn, h;
        nanosleep(&t, NULL);
        if (atomic_load(&sShutdown)) thread_die();
        pos = gFnPos;
        if (!pos) continue;
        fn = gFnRing[(pos - 1) & 63u];
        pthread_mutex_lock(&sProfMu);
        sProfTotal++;
        if (!atomic_load(&sHeld)) sProfFree++;
        for (h = fn * 2654435761u >> 19; ; h = (h + 1) & (PROF_N - 1)) {
            h &= PROF_N - 1;
            if (sProfCnt[h] == 0) { sProfFn[h] = fn; sProfCnt[h] = 1; break; }
            if (sProfFn[h] == fn) { sProfCnt[h]++; break; }
        }
        pthread_mutex_unlock(&sProfMu);
    }
    return NULL;
}
static void sampler_start(void)
{
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    atomic_fetch_add(&sLive, 1);
    if (pthread_create(&th, &at, sampler_main, NULL) == 0) pthread_detach(th); else atomic_fetch_sub(&sLive, 1);
}
void core_profile_start(void) { pthread_once(&sTickerOnce, sampler_start); }

/* The ten most frequent functions since the last report ("index:per-mille"), and the share of samples in which nobody held the lock; then the counters restart. */
void core_profile_report(char *buf, size_t n)
{
    unsigned i, k, used = 0, total, freecnt;
    unsigned bestc[10] = {0}, bestf[10] = {0};
    buf[0] = 0;
    pthread_mutex_lock(&sProfMu);
    total = sProfTotal; freecnt = sProfFree;
    for (i = 0; i < PROF_N; i++) {
        unsigned c = sProfCnt[i];
        if (!c) continue;
        for (k = 0; k < 10; k++) {
            if (c > bestc[k]) { unsigned j; for (j = 9; j > k; j--) { bestc[j] = bestc[j - 1]; bestf[j] = bestf[j - 1]; } bestc[k] = c; bestf[k] = sProfFn[i]; break; }
        }
    }
    memset(sProfCnt, 0, sizeof sProfCnt); sProfTotal = 0; sProfFree = 0;
    pthread_mutex_unlock(&sProfMu);
    if (!total) return;
    used += (unsigned)snprintf(buf, n, "%u samples, lock free %u%%:", total, 100u * freecnt / total);
    for (k = 0; k < 10 && bestc[k] && used + 16 < n; k++) used += (unsigned)snprintf(buf + used, n - used, " %u:%u", bestf[k], 1000u * bestc[k] / total);
}

void core_lock_take(void)
{
    if (pthread_mutex_trylock(&sWasmLock) != 0) {
        atomic_fetch_add(&sWaiters, 1);
        gYieldReq = 1;                            /* the holder hands the lock over at its next loop header (core_yield_point) */
        pthread_mutex_lock(&sWasmLock);
        atomic_fetch_sub(&sWaiters, 1);
    }
    if (atomic_load_explicit(&sShutdown, memory_order_relaxed) && tCounted) {      /* the program is leaving: this thread ends here instead of going on in the game */
        pthread_mutex_unlock(&sWasmLock);
        thread_die();
    }
    atomic_store_explicit(&sHeld, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&sLockGen, 1, memory_order_relaxed);
    sHeldSinceUs = now_us();
    if (atomic_load_explicit(&sWaiters, memory_order_relaxed) > 0) gYieldReq = 1;      /* others still wait: this holder hands over in turn */
    gCore.inst.w2c_0x5F_stack_pointer = tSavedSp;
    tHasLock = 1;
}
void core_lock_drop(void)
{
    tSavedSp = gCore.inst.w2c_0x5F_stack_pointer;
    tHasLock = 0;
    atomic_store_explicit(&sHeld, 0, memory_order_relaxed);
    pthread_mutex_unlock(&sWasmLock);
}

/* Called from the generated code at loop headers when gYieldReq is set. */
void core_yield_point(void)
{
    unsigned gen;
    int i;
    if (!tHasLock || atomic_load(&sWaiters) == 0) { gYieldReq = 0; return; }
    if (now_us() - sHeldSinceUs < 150) return;     /* has only just taken it: the request stays up and is looked at again at the next loop header */
    gYieldReq = 0;
    gen = atomic_load_explicit(&sLockGen, memory_order_relaxed);
    core_lock_drop();
    for (i = 0; i < 100 && atomic_load_explicit(&sLockGen, memory_order_relaxed) == gen && atomic_load(&sWaiters) > 0; i++) {
        struct timespec t = { 0, 20000 };
        nanosleep(&t, NULL);                       /* the waiter wakes with the unlock; this gives it the few microseconds to get the lock first */
    }
    core_lock_take();
}

int core_has_lock(void) { return tHasLock; }

/* For a stall report: is the core's lock held right now, and what was the last file or timing call the game made. */
/* The last functions the game entered, newest last (filled by the generated code when it is built with --trace-fn; otherwise it stays empty). */
volatile unsigned int gFnRing[64], gFnPos;
void core_fn_history(char *buf, size_t n)
{
    unsigned int pos = gFnPos, i;
    size_t used = 0;
    buf[0] = 0;
    if (!pos) return;
    for (i = 0; i < 32 && used + 8 < n; i++) used += (size_t)snprintf(buf + used, n - used, "%u ", gFnRing[(pos - 1 - i) & 63u]);
}
int core_lock_is_busy(void)
{
    if (pthread_mutex_trylock(&sWasmLock) != 0) return 1;
    pthread_mutex_unlock(&sWasmLock);
    return 0;
}
static const char *volatile sNote = "(none)";
static volatile unsigned long long sNoteNs;
static unsigned long long mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
}
void core_note(const char *what) { sNote = what; sNoteNs = mono_ms(); }
const char *core_last_note(unsigned *age_ms) { *age_ms = (unsigned)(mono_ms() - sNoteNs); return sNote; }

/* ---- handles for mutexes and condition variables ------------------------------------------------------------------------------ */
#define MAX_SYNC 256
static pthread_mutex_t *sMutex[MAX_SYNC];
static pthread_cond_t *sCond[MAX_SYNC];
static unsigned sMutexN = 1, sCondN = 1;                 /* handle 0 is never used: a zero-filled game variable is not a valid handle */
static pthread_mutex_t sTableLock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *mtx(uint32_t h) { return h < MAX_SYNC ? sMutex[h] : NULL; }
static pthread_cond_t *cnd(uint32_t h) { return h < MAX_SYNC ? sCond[h] : NULL; }

uint32_t w2c_shell_mutex_new(struct w2c_shell *s)
{
    uint32_t h;
    (void)s;
    pthread_mutex_lock(&sTableLock);
    h = sMutexN < MAX_SYNC ? sMutexN++ : 0;
    pthread_mutex_unlock(&sTableLock);
    if (!h) { fprintf(stderr, "core: out of mutex handles\n"); abort(); }
    sMutex[h] = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(sMutex[h], NULL);
    return h;
}

void w2c_shell_mutex_lock(struct w2c_shell *s, uint32_t h)
{
    pthread_mutex_t *m = mtx(h);
    (void)s;
    if (pthread_mutex_trylock(m) == 0) return;
    core_lock_drop();                                      /* someone else has it: do not hold the wasm lock while waiting */
    pthread_mutex_lock(m);
    if (atomic_load(&sShutdown) && tCounted) { pthread_mutex_unlock(m); thread_die(); }
    core_lock_take();
}

void w2c_shell_mutex_unlock(struct w2c_shell *s, uint32_t h) { (void)s; pthread_mutex_unlock(mtx(h)); }

uint32_t w2c_shell_cond_new(struct w2c_shell *s)
{
    uint32_t h;
    pthread_condattr_t a;
    (void)s;
    pthread_mutex_lock(&sTableLock);
    h = sCondN < MAX_SYNC ? sCondN++ : 0;
    pthread_mutex_unlock(&sTableLock);
    if (!h) { fprintf(stderr, "core: out of condition-variable handles\n"); abort(); }
    sCond[h] = malloc(sizeof(pthread_cond_t));
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);          /* timed waits use the monotonic clock, see wall_ns */
    pthread_cond_init(sCond[h], &a);
    pthread_condattr_destroy(&a);
    return h;
}

void w2c_shell_cond_wait(struct w2c_shell *s, uint32_t c, uint32_t m)
{
    (void)s;
    core_lock_drop();
    pthread_cond_wait(cnd(c), mtx(m));
    if (atomic_load(&sShutdown) && tCounted) { pthread_mutex_unlock(mtx(m)); thread_die(); }
    core_lock_take();
}

void w2c_shell_cond_wait_ns(struct w2c_shell *s, uint32_t c, uint32_t m, uint64_t ns)
{
    struct timespec t;
    (void)s;
    clock_gettime(CLOCK_MONOTONIC, &t);
    ns += (uint64_t)t.tv_nsec;
    t.tv_sec += (time_t)(ns / 1000000000ULL);
    t.tv_nsec = (long)(ns % 1000000000ULL);
    core_lock_drop();
    pthread_cond_timedwait(cnd(c), mtx(m), &t);
    if (atomic_load(&sShutdown) && tCounted) { pthread_mutex_unlock(mtx(m)); thread_die(); }
    core_lock_take();
}

void w2c_shell_cond_signal(struct w2c_shell *s, uint32_t c) { (void)s; pthread_cond_signal(cnd(c)); }
void w2c_shell_cond_broadcast(struct w2c_shell *s, uint32_t c) { (void)s; pthread_cond_broadcast(cnd(c)); }

/* ---- threads ---------------------------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t fn, arg, stack_top; } ThreadStart;

void core_trap_report(const char *where, wasm_rt_trap_t code)
{
    fprintf(stderr, "core: the game trapped in %s: %s (%d)\n", where, wasm_rt_strerror(code), (int)code);
    fflush(stderr);
    {   /* development: KYUJO_TRAP_DUMP=<file> writes the game's RAM (the first 16 MiB) at the moment of a trap, for looking at what was corrupted */
        const char *path = getenv("KYUJO_TRAP_DUMP");
        extern unsigned char *gRdpMem;
        if (path && gRdpMem) { FILE *f = fopen(path, "wb"); if (f) { fwrite(gRdpMem, 1, 16u << 20, f); fclose(f); fprintf(stderr, "core: game RAM written to %s\n", path); } }
    }
}

static void *thread_main(void *p)
{
    ThreadStart s = *(ThreadStart *)p;
    wasm_rt_trap_t trap;
    free(p);
    tCounted = 1;                                            /* counted in sLive by w2c_shell_thread_start */
    wasm_rt_init_thread();
    tSavedSp = s.stack_top;
    core_lock_take();
    trap = (wasm_rt_trap_t)wasm_rt_impl_try();
    if (trap) {
        core_trap_report("a game thread", trap);
        core_exit(70);
    } else {
        w2c_kyujo_core_thread_entry(&gCore.inst, s.fn, s.arg);
    }
    core_lock_drop();
    if (tCounted) { tCounted = 0; atomic_fetch_sub(&sLive, 1); }
    return NULL;
}

void w2c_shell_thread_start(struct w2c_shell *sh, uint32_t entry, uint32_t arg, uint32_t stack_top)
{
    ThreadStart *s = malloc(sizeof *s);
    pthread_attr_t a;
    pthread_t t;
    (void)sh;
    s->fn = entry;
    s->arg = arg;
    s->stack_top = (stack_top + 15u) & ~15u;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 2u << 20);                 /* native stack: wasm2c code recurses like the original, with larger frames */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    atomic_fetch_add(&sLive, 1);
    if (pthread_create(&t, &a, thread_main, s) != 0) { fprintf(stderr, "core: pthread_create failed\n"); abort(); }
    pthread_attr_destroy(&a);
}

void w2c_shell_thread_exit(struct w2c_shell *sh)
{
    (void)sh;
    core_lock_drop();
    thread_die();
}

void w2c_shell_sleep_ns(struct w2c_shell *sh, uint64_t ns)
{
    struct timespec t;
    (void)sh;
    t.tv_sec = (time_t)(ns / 1000000000ULL);
    t.tv_nsec = (long)(ns % 1000000000ULL);
    core_lock_drop();
    nanosleep(&t, NULL);
    if (atomic_load(&sShutdown) && tCounted) thread_die();
    core_lock_take();
}

void w2c_shell_yield(struct w2c_shell *sh)
{
    (void)sh;
    core_lock_drop();
    sched_yield();
    core_lock_take();
}

uint64_t w2c_shell_wall_ns(struct w2c_shell *sh)
{
    struct timespec t;
    (void)sh;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* ---- the front: picture, sound, restart -------------------------------------------------------------------------------------------- */
static const uint8_t *core_mem(uint32_t addr, uint32_t len)
{
    wasm_rt_memory_t *m = w2c_kyujo_memory(&gCore.inst);
    if ((uint64_t)addr + len > m->size) return NULL;
    return m->data + addr;
}

void w2c_shell_present(struct w2c_shell *sh, uint32_t rgb, uint32_t w, uint32_t h)
{
    const uint8_t *p = core_mem(rgb, w * h * 3);
    (void)sh;
    if (p && gCore.front.present) gCore.front.present(p, w, h, gCore.front.user);
}

void w2c_shell_audio_write(struct w2c_shell *sh, uint32_t ptr, uint32_t len)
{
    const uint8_t *p = core_mem(ptr, len);
    (void)sh;
    if (p && gCore.front.audio) gCore.front.audio(p, len, gCore.front.user);
}

void w2c_shell_set_widescreen(struct w2c_shell *sh, uint32_t on)
{
    (void)sh;
    if (gCore.front.set_widescreen) gCore.front.set_widescreen((int)on, gCore.front.user);
}

void w2c_shell_graphics_go(struct w2c_shell *sh)
{
    (void)sh;
    if (gCore.front.graphics_go) gCore.front.graphics_go(gCore.front.user);
}

void w2c_shell_set_backdrop(struct w2c_shell *sh, uint32_t rgb, uint32_t w, uint32_t h)
{
    const uint8_t *p = core_mem(rgb, w * h * 3);
    (void)sh;
    if (p && gCore.front.set_backdrop) gCore.front.set_backdrop(p, w, h, gCore.front.user);
}

/* The wasm memory for writing: the game's buffers that the shell fills. */
static uint8_t *core_mem_w(uint32_t addr, uint32_t len) { return (uint8_t *)core_mem(addr, len); }

uint32_t w2c_shell_pick_gb(struct w2c_shell *sh, uint32_t port, uint32_t kind, uint32_t rom)
{
    const uint8_t *p = NULL;
    (void)sh;
    if (!gCore.front.pick_gb) return 0;
    if (rom) {                                                    /* a C string in the game's memory: it must end inside it */
        wasm_rt_memory_t *m = w2c_kyujo_memory(&gCore.inst);
        uint32_t n = 0;
        if (rom >= m->size) return 0;
        while (rom + n < m->size && m->data[rom + n] && n < 4096) n++;
        if (rom + n >= m->size || n >= 4096) return 0;
        p = m->data + rom;
    }
    return (uint32_t)gCore.front.pick_gb((int)port, (int)kind, (const char *)p, gCore.front.user);
}

uint32_t w2c_shell_pick_gb_poll(struct w2c_shell *sh, uint32_t port, uint32_t kind, uint32_t path, uint32_t path_size, uint32_t msg, uint32_t msg_size)
{
    int pp = -1, pk = 0, r;
    char pth[1100], pmsg[400];
    uint8_t *wp = core_mem_w(port, 4), *wk = core_mem_w(kind, 4), *wpath = core_mem_w(path, path_size), *wmsg = core_mem_w(msg, msg_size);
    (void)sh;
    if (!gCore.front.pick_gb_poll || !wp || !wk || !wpath || !wmsg || !path_size || !msg_size) return 0;
    pth[0] = pmsg[0] = 0;
    r = gCore.front.pick_gb_poll(&pp, &pk, pth, sizeof pth, pmsg, sizeof pmsg, gCore.front.user);
    if (!r) return 0;
    memcpy(wp, &pp, 4);                                           /* ints of the game's memory are little-endian, as the host's */
    memcpy(wk, &pk, 4);
    snprintf((char *)wpath, path_size, "%s", pth);
    snprintf((char *)wmsg, msg_size, "%s", pmsg);
    return 1;
}

void w2c_shell_set_fullscreen(struct w2c_shell *sh, uint32_t on)
{
    (void)sh;
    if (gCore.front.set_fullscreen) gCore.front.set_fullscreen((int)on, gCore.front.user);
}

uint32_t w2c_shell_restart(struct w2c_shell *sh)
{
    int r, was_counted;
    (void)sh;
    if (!gCore.front.restart) return 0;
    /* On success the front ends this thread without returning. It must not take the wasm lock with it (every other game thread would then wait for ever and
     * could not leave, see core_threads_stop), and it is no longer one of the threads to wait for. */
    core_lock_drop();
    was_counted = tCounted;
    if (was_counted) { tCounted = 0; atomic_fetch_sub(&sLive, 1); }
    r = gCore.front.restart(gCore.front.user);
    if (was_counted) { tCounted = 1; atomic_fetch_add(&sLive, 1); }
    core_lock_take();
    return (uint32_t)r;                                                 /* 0: the game tells the player to start it again by hand */
}

void core_exit(int code)
{
    if (tHasLock) core_lock_drop();                          /* the hook may end this thread: it must not take the lock with it */
    if (tCounted) { tCounted = 0; atomic_fetch_sub(&sLive, 1); }     /* it is on its way out whatever the hook does */
    if (gCore.front.exit_hook) gCore.front.exit_hook(code, gCore.front.user);
    exit(code);
}

/* Makes every thread the core started leave (see above) and waits for them, at most timeout_ms. Returns how many are still there. Call from the thread that
 * runs the program's event loop, after the game's own thread has ended and before anything the game threads might be waiting for is torn down. */
int core_threads_stop(unsigned timeout_ms)
{
    unsigned waited = 0;
    atomic_store(&sShutdown, 1);
    for (;;) {
        unsigned i;
        if (atomic_load(&sLive) <= 0 || waited >= timeout_ms) break;
        pthread_mutex_lock(&sTableLock);
        for (i = 1; i < sCondN; i++) if (sCond[i]) pthread_cond_broadcast(sCond[i]);
        pthread_mutex_unlock(&sTableLock);
        { struct timespec t = { 0, 5000000 }; nanosleep(&t, NULL); }
        waited += 5;
    }
    return atomic_load(&sLive);
}

/* ---- the public functions --------------------------------------------------------------------------------------------------------- */
int core_init(const CoreFront *front)
{
    gCore.front = *front;
    gCore.shell.core = &gCore;
    gCore.wasi.core = &gCore;
    wasm_rt_init();
    wasm2c_kyujo_instantiate(&gCore.inst, &gCore.shell, &gCore.wasi);
    if (wasi_init(&gCore, front->root_dir) != 0) return -1;
    if (front->home) core_setenv("KYUJO_HOME", front->home);
    return 0;
}

void core_pass_environment(char **env)
{
    for (; env && *env; env++) {
        if (!strncmp(*env, "KYUJO_", 6)) {
            const char *eq = strchr(*env, '=');
            if (eq) {
                char name[128];
                size_t n = (size_t)(eq - *env);
                if (n < sizeof name) { memcpy(name, *env, n); name[n] = 0; core_setenv(name, eq + 1); }
            }
        }
    }
}

/* Starts the game again inside the same process ("Restart game"): the instance is thrown away and made again, so the game boots from nothing. Needs every thread of
 * the core gone (core_threads_stop returned 0) and the renderer idle with its caches dropped. Returns 0 on success. Used because a chain-loaded copy of the program on
 * the Switch crashed in the graphics driver whatever was closed before. */
int core_reset(void)
{
    unsigned i;
    if (atomic_load(&sLive) > 0) return -1;
    wasm2c_kyujo_free(&gCore.inst);
    memset(&gCore.inst, 0, sizeof gCore.inst);
    rdp_glue_memory_replaced();
    if (getenv("KYUJO_TEST_MOVE_MEMORY")) (void)malloc(160u << 20);          /* development: the new instance must not get the old address (it does not on Linux, it did not on the Switch) */
    pthread_mutex_lock(&sTableLock);
    for (i = 1; i < sMutexN; i++) if (sMutex[i]) { pthread_mutex_destroy(sMutex[i]); free(sMutex[i]); sMutex[i] = NULL; }
    for (i = 1; i < sCondN; i++) if (sCond[i]) { pthread_cond_destroy(sCond[i]); free(sCond[i]); sCond[i] = NULL; }
    sMutexN = sCondN = 1;
    pthread_mutex_unlock(&sTableLock);
    atomic_store(&sShutdown, 0);
    atomic_store(&sHeld, 0);
    sTickerOnce = (pthread_once_t)PTHREAD_ONCE_INIT;
    atomic_fetch_add(&sGeneration, 1);
    wasm2c_kyujo_instantiate(&gCore.inst, &gCore.shell, &gCore.wasi);
    wasi_reset(&gCore, gCore.front.root_dir);
    gCore.exit_code = 0;
    return 0;
}

/* Size of the core's memory in bytes (it is fixed: the renderer keeps a pointer into it, see rdp_glue.c). */
unsigned long long core_memory_bytes(void) { return w2c_kyujo_memory(&gCore.inst)->size; }

int core_run(int argc, char **argv)
{
    wasm_rt_trap_t trap;
    gCore.argc = argc;
    gCore.argv = argv;
    tSavedSp = gCore.inst.w2c_0x5F_stack_pointer;           /* the main thread starts on the stack the linker gave the module */
    atomic_fetch_add(&sLive, 1);
    tCounted = 1;
    core_lock_take();
    trap = (wasm_rt_trap_t)wasm_rt_impl_try();
    if (trap) {
        core_trap_report("the game", trap);
        core_lock_drop();
        if (tCounted) { tCounted = 0; atomic_fetch_sub(&sLive, 1); }
        return 70;
    }
    w2c_kyujo_0x5Fstart(&gCore.inst);
    core_lock_drop();
    if (tCounted) { tCounted = 0; atomic_fetch_sub(&sLive, 1); }
    return gCore.exit_code;
}

int core_push_input(const HostRawInput *in)
{
    uint32_t ptr;
    wasm_rt_memory_t *m;
    static __thread unsigned tStackGen;
    if (pthread_mutex_trylock(&sWasmLock) != 0) {
        /* The game holds the core. A busy game (Game Boy Tower at speed 3, a battle) holds it nearly all the time, so a plain try-again could miss a short
         * button press altogether. Ask the holder to hand it over (the same request a waiting game thread makes) and wait a few milliseconds for it; the
         * event loop must not wait longer than that, the caller tries again. */
        int i, got = 0;
        atomic_fetch_add(&sWaiters, 1);
        gYieldReq = 1;
        for (i = 0; i < 100 && !got; i++) {
            struct timespec t = { 0, 50000 };
            nanosleep(&t, NULL);
            got = pthread_mutex_trylock(&sWasmLock) == 0;
        }
        atomic_fetch_sub(&sWaiters, 1);
        if (!got) return 0;
    }
    if (tStackGen != atomic_load(&sGeneration)) {
        /* This thread has never run core code: give it the shadow stack the core reserved for the shell (core_shell_stack_top is a leaf function that
         * does not touch the stack, so it is safe to call before the pointer is set). */
        gCore.inst.w2c_0x5F_stack_pointer = w2c_kyujo_core_shell_stack_top(&gCore.inst);
        tSavedSp = gCore.inst.w2c_0x5F_stack_pointer;
        tStackGen = atomic_load(&sGeneration);
    } else {
        gCore.inst.w2c_0x5F_stack_pointer = tSavedSp;
    }
    atomic_store_explicit(&sHeld, 1, memory_order_relaxed);
    sHeldSinceUs = now_us();
    tHasLock = 1;
    ptr = w2c_kyujo_core_raw_input_ptr(&gCore.inst);
    m = w2c_kyujo_memory(&gCore.inst);
    if ((uint64_t)ptr + sizeof *in <= m->size) {
        memcpy(m->data + ptr, in, sizeof *in);
        w2c_kyujo_core_raw_input_commit(&gCore.inst);
    }
    core_lock_drop();
    return 1;
}
