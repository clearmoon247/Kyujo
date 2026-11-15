/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_sched.c - host threads, the "baton", hardware events and the timer (everything the libultra scheduler needs from the host).
 *
 * Model: every N64 thread runs on its own host thread, but only the thread holding the baton executes game code. The others sleep
 * in host_baton_wait(). That keeps the console's single-CPU behaviour (no races in game code) and means the libultra scheduler
 * logic can be the real libultra C code; this file only supplies "switch to that thread" and "wake me when something happens".
 *
 * One mutex (sLock) protects every shared variable here; there is no other locking. Device threads (the timer below; video and
 * audio later) touch nothing except through host_event_post().
 *
 * Two operating-system back ends are in this file: POSIX threads (Linux) and the Win32 API (Windows). The scheduling logic is
 * shared; only the few primitives in the "portability layer" section differ. The Win32 back end is compiled and checked
 * statically on the development machine, but can only be run on Windows.
 */
#include <stdlib.h>
#include <string.h>

#include "platform.h"

/* ------------------------------------------------------------------------------------------------------------------------ */
/* Portability layer: mutex, condition variable, thread start, clock.                                                          */
/* ------------------------------------------------------------------------------------------------------------------------ */
#ifdef _WIN32
#include <windows.h>

typedef CRITICAL_SECTION  OsMutex;
typedef CONDITION_VARIABLE OsCond;

static void os_mutex_init(OsMutex *m)  { InitializeCriticalSection(m); }
static void os_mutex_lock(OsMutex *m)  { EnterCriticalSection(m); }
static void os_mutex_unlock(OsMutex *m){ LeaveCriticalSection(m); }
static void os_cond_init(OsCond *c)    { InitializeConditionVariable(c); }
static void os_cond_wait(OsCond *c, OsMutex *m) { SleepConditionVariableCS(c, m, INFINITE); }
/* Waits at most `ns` nanoseconds (rounded up to whole milliseconds, the granularity of this API). */
static void os_cond_wait_ns(OsCond *c, OsMutex *m, unsigned long long ns)
{
    unsigned long long ms = (ns + 999999ULL) / 1000000ULL;
    SleepConditionVariableCS(c, m, ms > 0x7FFFFFFFULL ? 0x7FFFFFFFUL : (DWORD)ms);
}
static void os_cond_signal(OsCond *c)  { WakeConditionVariable(c); }
static void os_cond_broadcast(OsCond *c) { WakeAllConditionVariable(c); }

typedef struct { void (*fn)(void *); void *arg; } OsStart;
static DWORD WINAPI os_thread_main(LPVOID p)
{
    OsStart s = *(OsStart *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
/* Windows' default timer tick is about 15.6 ms, which would make 16.7 ms video fields jitter badly. winmm's timeBeginPeriod(1) asks
 * for 1 ms. It is looked up at run time (LoadLibrary) so the program's import table stays limited to KERNEL32 and the UCRT. */
static void os_request_fine_timers(void)
{
    HMODULE m = LoadLibraryA("winmm.dll");
    if (m) {
        typedef UINT (WINAPI *TimeBeginPeriodFn)(UINT);
        TimeBeginPeriodFn fn = (TimeBeginPeriodFn)(void *)GetProcAddress(m, "timeBeginPeriod");
        if (fn) fn(1);
    }
}
static void os_thread_exit(void) { ExitThread(0); }
/* Precise short waits.  The cond-var timeout above has a granularity of one millisecond AT BEST (rounded UP), and Windows wakes a sleeping
 * thread only at its next clock tick (1 ms after timeBeginPeriod(1), 15.6 ms otherwise, and timeBeginPeriod is not honoured for a window
 * that is in the background on recent Windows 11). The Game Boy Tower's "Loading..." is made of tens of thousands of Joybus transfers of
 * about 1.15 ms each (see hw_pif.c); each wait was rounded up to 2 ms or more, which is the most likely reason the first Windows test took
 * about 2 minutes where Linux takes about 40 s (NOT measured on Windows: this is the explanation that fits the numbers).
 * So the last OS_SPIN_NS before a device deadline are waited for by spinning on the performance counter instead of sleeping. */
#define OS_SPIN_NS 2500000ULL
static void os_spin_until(unsigned long long deadline_ns)
{
    while (host_now_ns() < deadline_ns) YieldProcessor();
}
static void os_sleep_ns(unsigned long long ns) { Sleep((DWORD)((ns + 999999ULL) / 1000000ULL)); }
static void os_thread_start(void (*fn)(void *), void *arg, unsigned int stack_bytes)
{
    OsStart *s = malloc(sizeof *s);
    HANDLE h;
    if (!s) host_fatal("out of memory starting a thread");
    s->fn = fn;
    s->arg = arg;
    /* STACK_SIZE_PARAM_IS_A_RESERVATION: reserve address space only; pages are committed as the stack grows. */
    h = CreateThread(NULL, stack_bytes, os_thread_main, s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) host_fatal("CreateThread failed");
    CloseHandle(h);                       /* the thread keeps running; we never join it */
}

unsigned long long host_wall_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    /* split to avoid overflowing 64 bits when multiplying by 1e9 */
    return (unsigned long long)(now.QuadPart / freq.QuadPart) * 1000000000ULL
         + (unsigned long long)(now.QuadPart % freq.QuadPart) * 1000000000ULL / (unsigned long long)freq.QuadPart;
}

#elif defined(__wasm__)
/* The game running as WebAssembly inside the native shell of the Switch / Android port (docs/switch-android-plan.md). WebAssembly has no threads
 * of its own that fit this design, so every primitive here is a call into the shell, which keeps the real threads, mutexes and condition variables.
 * The shell serialises all wasm code with one lock (every blocking call below releases it while it waits), so game code still never runs on two
 * threads at once, exactly as the baton intends. A mutex or condition variable is a small number the shell gave out (a handle). */
#define SHELL_IMPORT(name) __attribute__((import_module("shell"), import_name(#name)))
SHELL_IMPORT(mutex_new)       unsigned int shell_mutex_new(void);
SHELL_IMPORT(mutex_lock)      void shell_mutex_lock(unsigned int m);
SHELL_IMPORT(mutex_unlock)    void shell_mutex_unlock(unsigned int m);
SHELL_IMPORT(cond_new)        unsigned int shell_cond_new(void);
SHELL_IMPORT(cond_wait)       void shell_cond_wait(unsigned int c, unsigned int m);
SHELL_IMPORT(cond_wait_ns)    void shell_cond_wait_ns(unsigned int c, unsigned int m, unsigned long long ns);
SHELL_IMPORT(cond_signal)     void shell_cond_signal(unsigned int c);
SHELL_IMPORT(cond_broadcast)  void shell_cond_broadcast(unsigned int c);
SHELL_IMPORT(thread_start)    void shell_thread_start(void (*entry)(void *), void *arg, void *stack_top);
SHELL_IMPORT(thread_exit)     void shell_thread_exit(void);
SHELL_IMPORT(sleep_ns)        void shell_sleep_ns(unsigned long long ns);
SHELL_IMPORT(yield)           void shell_yield(void);
SHELL_IMPORT(wall_ns)         unsigned long long shell_wall_ns(void);

typedef unsigned int OsMutex;
typedef unsigned int OsCond;

static void os_mutex_init(OsMutex *m)  { *m = shell_mutex_new(); }
static void os_mutex_lock(OsMutex *m)  { shell_mutex_lock(*m); }
static void os_mutex_unlock(OsMutex *m){ shell_mutex_unlock(*m); }
static void os_cond_init(OsCond *c)    { *c = shell_cond_new(); }
static void os_cond_wait(OsCond *c, OsMutex *m) { shell_cond_wait(*c, *m); }
static void os_cond_wait_ns(OsCond *c, OsMutex *m, unsigned long long ns) { shell_cond_wait_ns(*c, *m, ns); }
static void os_cond_signal(OsCond *c)  { shell_cond_signal(*c); }
static void os_cond_broadcast(OsCond *c) { shell_cond_broadcast(*c); }

/* A thread needs its own stack for the compiler's "shadow stack" (the part of the call stack that lives in memory); the shell switches the
 * stack pointer when it hands the wasm lock from one thread to another. The native stack is the shell's business. */
typedef struct { void (*fn)(void *); void *arg; } OsStart;
static void os_thread_start(void (*fn)(void *), void *arg, unsigned int stack_bytes)
{
    OsStart *s = malloc(sizeof *s);
    unsigned char *stack = malloc(stack_bytes);
    if (!s || !stack) host_fatal("out of memory starting a thread");
    s->fn = fn;
    s->arg = arg;
    shell_thread_start(fn, arg, stack + stack_bytes);
    free(s);
}
/* What the shell calls to run a thread's function on the thread it started (a function pointer is a table index here, so the shell cannot call it directly). */
__attribute__((export_name("core_thread_entry"))) void core_thread_entry(void (*fn)(void *), void *arg) { fn(arg); }
static void os_request_fine_timers(void) { }
#ifndef OS_SPIN_NS
#define OS_SPIN_NS 0ULL
#endif
static void os_spin_until(unsigned long long deadline_ns) { while (host_now_ns() < deadline_ns) shell_yield(); }
static void os_thread_exit(void) { shell_thread_exit(); }
static void os_sleep_ns(unsigned long long ns) { shell_sleep_ns(ns); }
unsigned long long host_wall_ns(void) { return shell_wall_ns(); }

#else /* POSIX */
#include <pthread.h>
#include <time.h>

typedef pthread_mutex_t OsMutex;
typedef pthread_cond_t  OsCond;

static void os_mutex_init(OsMutex *m)  { pthread_mutex_init(m, NULL); }
static void os_mutex_lock(OsMutex *m)  { pthread_mutex_lock(m); }
static void os_mutex_unlock(OsMutex *m){ pthread_mutex_unlock(m); }
static void os_cond_init(OsCond *c)
{
    /* Timed waits must use the monotonic clock, or a change of the wall clock would shorten or lengthen them. */
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}
static void os_cond_wait(OsCond *c, OsMutex *m) { pthread_cond_wait(c, m); }
static void os_cond_wait_ns(OsCond *c, OsMutex *m, unsigned long long ns)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    ns += (unsigned long long)t.tv_nsec;
    t.tv_sec += (time_t)(ns / 1000000000ULL);
    t.tv_nsec = (long)(ns % 1000000000ULL);
    pthread_cond_timedwait(c, m, &t);
}
static void os_cond_signal(OsCond *c)  { pthread_cond_signal(c); }
static void os_cond_broadcast(OsCond *c) { pthread_cond_broadcast(c); }

typedef struct { void (*fn)(void *); void *arg; } OsStart;
static void *os_thread_main(void *p)
{
    OsStart s = *(OsStart *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
static void os_request_fine_timers(void) { }           /* POSIX timers are already fine-grained */
/* ... so no spinning is needed (nanosleep/timedwait are precise to microseconds). The spin code is still compiled, so that its logic can be
 * exercised on Linux by building with -DOS_SPIN_NS=2500000ULL (done once during development; see docs/known-decomp-issues.md). */
#ifndef OS_SPIN_NS
#define OS_SPIN_NS 0ULL
#endif
static void os_spin_until(unsigned long long deadline_ns)
{
    while (host_now_ns() < deadline_ns) __builtin_ia32_pause();
}
static void os_thread_exit(void) { pthread_exit(NULL); }
static void os_sleep_ns(unsigned long long ns)
{
    struct timespec t;
    t.tv_sec = (time_t)(ns / 1000000000ULL);
    t.tv_nsec = (long)(ns % 1000000000ULL);
    nanosleep(&t, NULL);
}
static void os_thread_start(void (*fn)(void *), void *arg, unsigned int stack_bytes)
{
    OsStart *s = malloc(sizeof *s);
    pthread_attr_t a;
    pthread_t t;
    if (!s) host_fatal("out of memory starting a thread");
    s->fn = fn;
    s->arg = arg;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_bytes);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, os_thread_main, s) != 0) host_fatal("pthread_create failed");
    pthread_attr_destroy(&a);
}

unsigned long long host_wall_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ULL + (unsigned long long)t.tv_nsec;
}
#endif

/* The game's clock. It is the wall clock minus the time the menu had the game paused (host_clock_pause), so that the game's timers, the video
 * retraces and the audio see no gap after the pause. host_wall_ns is the plain clock (test scripts, the menu's own timing). */
static volatile unsigned long long sPausedTotalNs;      /* the finished pauses */
static volatile unsigned long long sPauseStartNs;       /* wall time at which the current pause began */
static volatile int sPaused;

void host_clock_pause(void)
{
    if (sPaused) return;
    sPauseStartNs = host_wall_ns();
    __atomic_store_n(&sPaused, 1, __ATOMIC_SEQ_CST);
}

void host_clock_resume(void)
{
    if (!sPaused) return;
    sPausedTotalNs += host_wall_ns() - sPauseStartNs;
    __atomic_store_n(&sPaused, 0, __ATOMIC_SEQ_CST);
}

unsigned long long host_now_ns(void)
{
    if (__atomic_load_n(&sPaused, __ATOMIC_SEQ_CST)) return sPauseStartNs - sPausedTotalNs;      /* stands still while paused */
    return host_wall_ns() - sPausedTotalNs;
}


/* ------------------------------------------------------------------------------------------------------------------------ */
/* Shared logic                                                                                                                */
/* ------------------------------------------------------------------------------------------------------------------------ */

struct HostThread {
    OsCond cv;                         /* the owner sleeps here while it does not hold the baton */
    int go;                            /* set by host_baton_pass, consumed by the owner when it wakes */
    int started;                       /* has the host thread been created yet? */
    void (*entry)(void *);
    void *arg;
    unsigned int stack_bytes;
};

static OsMutex sLock;                  /* protects every variable below and every HostThread's go/started */
static OsCond  sEventCv;               /* signalled whenever sPending changes (and by host_job_start's worker when its job ends) */
static OsCond  sJobCv;                 /* wakes the background-job worker when a job is queued */
static OsCond  sJobDoneCv;             /* wakes a host_job_wait when the job has ended */
static void  (*sJobFn)(void *);        /* the queued job (NULL = none queued) */
static void   *sJobArg;
static int     sJobBusy;               /* a job is queued or running */
static int     sJobWorkerStarted;
static OsCond  sJob2Cv, sJob2DoneCv;   /* the second worker (the audio microcode, see host_job2_start) */
static void  (*sJob2Fn)(void *);
static void   *sJob2Arg;
static int     sJob2Busy, sJob2WorkerStarted;
static int     sJobEnded[2];           /* a job (slot 0: graphics, 1: audio) has ended and its result has not been taken: host_preempt_flag must stay raised */
static int     sWakeFlag;              /* a job ended: host_event_wait returns once even though no event is pending */
static OsCond  sTimerCv;               /* wakes the timer thread when it is (re)armed */
static int     sInitDone;
static HostThread sBoot;
static unsigned int sPending;          /* bit n set = event n pending */

static int sTimerThreadStarted;
static int sTimerArmed;
static unsigned long long sTimerDeadline;
static int sTimerEvent;

/* Delayed device events (host_event_post_after): one deadline per event number, 0 = none armed. Used for devices whose operation takes
 * measurable time on the console (the SI / Joybus transfers). Event numbers are bit positions in sPending, so they are below 32. */
static unsigned long long sLaterDeadline[32];

/* video field clock (host_vi_set_timing) */
static int sViRunning;
static int sViEvent = -1;
static int sViFired;                           /* has this field's interrupt been raised already? */
static unsigned long long sViFieldStart, sViPeriod, sViIntrOffset, sViFieldIndex;

/* Only the boot thread calls this, before any other thread exists. */
static void init_once(void)
{
    if (sInitDone) return;
    os_mutex_init(&sLock);
    os_cond_init(&sEventCv);
    os_cond_init(&sTimerCv);
    os_cond_init(&sJobCv);
    os_cond_init(&sJobDoneCv);
    os_cond_init(&sJob2Cv);
    os_cond_init(&sJob2DoneCv);
    os_cond_init(&sBoot.cv);
    sBoot.started = 1;                 /* the boot thread already exists */
    os_request_fine_timers();
    sInitDone = 1;
}

HostThread *host_thread_boot(void)
{
    init_once();
    return &sBoot;
}

HostThread *host_thread_new(void (*entry)(void *), void *arg, unsigned int stack_bytes)
{
    HostThread *t;
    init_once();
    t = calloc(1, sizeof *t);
    if (!t) host_fatal("out of memory creating a thread");
    os_cond_init(&t->cv);
    t->entry = entry;
    t->arg = arg;
    t->stack_bytes = stack_bytes;
    return t;
}

/* First code run by every host thread created by host_thread_new: wait for the baton, then run the entry function. */
static void thread_trampoline(void *p)
{
    HostThread *t = p;
    host_baton_wait(t);
    t->entry(t->arg);
    host_fatal("a game thread's entry function returned (it must end through osDestroyThread)");
}

void host_baton_pass(HostThread *to)
{
    os_mutex_lock(&sLock);
    to->go = 1;
    if (!to->started) {
        to->started = 1;
        os_thread_start(thread_trampoline, to, to->stack_bytes);   /* it will find go == 1 and run at once */
    } else {
        os_cond_signal(&to->cv);
    }
    os_mutex_unlock(&sLock);
}

void host_baton_wait(HostThread *self)
{
    os_mutex_lock(&sLock);
    while (!self->go) os_cond_wait(&self->cv, &sLock);
    self->go = 0;
    os_mutex_unlock(&sLock);
}

void host_thread_end(HostThread *self, HostThread *to)
{
    host_baton_pass(to);
    free(self);                        /* nobody else holds a pointer to it: the owning slot is marked dead */
    os_thread_exit();
    host_fatal("thread exit returned");
}

void host_sleep_ns(unsigned long long ns)
{
    os_sleep_ns(ns);
}

/* Read by the scheduling points that tools/native/be_memory.py (rule 5) puts at the end of every loop iteration of the game's code:
 * nonzero while an event is pending. A host-order variable, written and read as a plain int (volatile: other threads set it). */
volatile unsigned int host_preempt_flag;

void host_event_post(int event)
{
    os_mutex_lock(&sLock);
    sPending |= 1u << event;
    host_preempt_flag = 1;
    os_cond_broadcast(&sEventCv);
    os_mutex_unlock(&sLock);
}

int host_event_take(void)
{
    int n = -1, i;
    os_mutex_lock(&sLock);
    for (i = 0; i < 32; i++) {
        if (sPending & (1u << i)) {
            n = i;
            sPending &= ~(1u << i);
            break;
        }
    }
    host_preempt_flag = sPending != 0;
    os_mutex_unlock(&sLock);
    return n;
}

int host_event_pending(void)
{
    int p;
    os_mutex_lock(&sLock);
    p = sPending != 0;
    os_mutex_unlock(&sLock);
    return p;
}

int host_event_wait(unsigned long long timeout_ns)
{
    int p;
    os_mutex_lock(&sLock);
    if (!sPending && !sWakeFlag) {
        if (timeout_ns) os_cond_wait_ns(&sEventCv, &sLock, timeout_ns);
        else while (!sPending && !sWakeFlag) os_cond_wait(&sEventCv, &sLock);
    }
    sWakeFlag = 0;                     /* a job-end wake-up is used up by this return (the caller looks at the job itself) */
    p = sPending != 0;
    os_mutex_unlock(&sLock);
    return p;
}

/* ---- one background job at a time (the graphics worker, see src/render/gfx_host.c) --------------------------------------------------
 * host_job_start hands a function to a worker thread (created on first use) and returns at once. When the function returns, the worker
 * wakes whoever sleeps in host_event_wait (the idle thread) and raises host_preempt_flag, so that a busy game thread notices at its next
 * scheduling point. Only the game side starts jobs, never two at once (it waits for or polls the first one before starting the next). */
static void job_thread(void *unused)
{
    (void)unused;
    os_mutex_lock(&sLock);
    for (;;) {
        void (*fn)(void *);
        void *arg;
        while (!sJobFn) os_cond_wait(&sJobCv, &sLock);
        fn = sJobFn;
        arg = sJobArg;
        sJobFn = NULL;
        os_mutex_unlock(&sLock);
        fn(arg);
        os_mutex_lock(&sLock);
        sJobBusy = 0;
        sJobEnded[0] = 1;
        sWakeFlag = 1;
        host_preempt_flag = 1;
        os_cond_broadcast(&sEventCv);
        os_cond_broadcast(&sJobDoneCv);
    }
}

void host_job_start(void (*fn)(void *), void *arg)
{
    os_mutex_lock(&sLock);
    if (sJobBusy) { os_mutex_unlock(&sLock); host_fatal("host_job_start: the previous job has not ended"); }
    if (!sJobWorkerStarted) {
        sJobWorkerStarted = 1;
        os_thread_start(job_thread, NULL, 1024 * 1024);
    }
    sJobFn = fn;
    sJobArg = arg;
    sJobBusy = 1;
    os_cond_signal(&sJobCv);
    os_mutex_unlock(&sLock);
}

int host_job_running(void)
{
    int r;
    os_mutex_lock(&sLock);
    r = sJobBusy;
    os_mutex_unlock(&sLock);
    return r;
}

void host_job_wait(void)
{
    os_mutex_lock(&sLock);
    while (sJobBusy) os_cond_wait(&sJobDoneCv, &sLock);
    os_mutex_unlock(&sLock);
}

/* A second background job slot with its own worker thread, for the RSP audio microcode (src/ultra/hw_sp.c), which runs at the same time as the graphics worker
 * and the game. Same behaviour as the first slot: one job at a time, the worker wakes host_event_wait and raises host_preempt_flag when it ends. */
static void job2_thread(void *unused)
{
    (void)unused;
    os_mutex_lock(&sLock);
    for (;;) {
        void (*fn)(void *);
        void *arg;
        while (!sJob2Fn) os_cond_wait(&sJob2Cv, &sLock);
        fn = sJob2Fn;
        arg = sJob2Arg;
        sJob2Fn = NULL;
        os_mutex_unlock(&sLock);
        fn(arg);
        os_mutex_lock(&sLock);
        sJob2Busy = 0;
        sJobEnded[1] = 1;
        sWakeFlag = 1;
        host_preempt_flag = 1;
        os_cond_broadcast(&sEventCv);
        os_cond_broadcast(&sJob2DoneCv);
    }
}

/* The RSP audio microcode on the second worker (hw_sp.c): the interpreter itself is shell code behind the import host_rsp_run; the worker only calls it. Its state belongs to the
 * worker from host_rsp_run_async until the run has ended and been collected; the game side does not touch the interpreter in between. */
static unsigned int sRspSteps;
static unsigned long long sRspNs;
static void rsp_job(void *unused)
{
    unsigned long long t0;
    static int lag_ms = -1;
    (void)unused;
    if (lag_ms < 0) { const char *e = getenv("KYUJO_DEBUG_AUDIO_LAG_MS"); lag_ms = e ? atoi(e) : 0; }   /* development: a slow audio worker (a slow wake-up, a busy machine) */
    if (lag_ms > 0) host_sleep_ns((unsigned long long)lag_ms * 1000000ull);
    t0 = host_now_ns();
    sRspSteps = host_rsp_run();
    sRspNs = host_now_ns() - t0;
}
void host_rsp_run_async(void) { host_job2_start(rsp_job, NULL); }
int host_rsp_async_running(void) { return host_job2_running(); }
void host_rsp_async_wait(void) { host_job2_wait(); }
unsigned int host_rsp_async_result(void) { return sRspSteps; }
unsigned long long host_rsp_async_ns(void) { return sRspNs; }   /* (by value: game code cannot read a host-written 64-bit variable through a pointer, platform.h byte-order rule) */

int host_audio_async_enabled(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("KYUJO_AUDIO_THREAD"); v = !(e && e[0] == '0'); }
    return v;
}

void host_job2_start(void (*fn)(void *), void *arg)
{
    os_mutex_lock(&sLock);
    if (sJob2Busy) { os_mutex_unlock(&sLock); host_fatal("host_job2_start: the previous job has not ended"); }
    if (!sJob2WorkerStarted) {
        sJob2WorkerStarted = 1;
        os_thread_start(job2_thread, NULL, 1024 * 1024);
    }
    sJob2Fn = fn;
    sJob2Arg = arg;
    sJob2Busy = 1;
    os_cond_signal(&sJob2Cv);
    os_mutex_unlock(&sLock);
}

int host_job2_running(void)
{
    int r;
    os_mutex_lock(&sLock);
    r = sJob2Busy;
    os_mutex_unlock(&sLock);
    return r;
}

void host_job2_wait(void)
{
    os_mutex_lock(&sLock);
    while (sJob2Busy) os_cond_wait(&sJob2DoneCv, &sLock);
    os_mutex_unlock(&sLock);
}

/* After a job's result has been taken: lower host_preempt_flag again unless an event is pending (the job's end raised it). */
void host_preempt_ack(void)
{
    os_mutex_lock(&sLock);
    host_preempt_flag = sPending != 0 || sJobEnded[0] || sJobEnded[1];
    os_mutex_unlock(&sLock);
}

/* The result of the job in `slot` (0 graphics, 1 audio) has been taken. The flag stays raised while the other job has ended and waits to be collected. */
void host_job_taken(int slot)
{
    os_mutex_lock(&sLock);
    sJobEnded[slot] = 0;
    host_preempt_flag = sPending != 0 || sJobEnded[0] || sJobEnded[1];
    os_mutex_unlock(&sLock);
}

/* The timer thread serves two clocks: the one-shot Count/Compare timer and the video field clock. It sleeps until the earliest
 * deadline, posts what is due, and repeats. */
static void timer_thread(void *unused)
{
    (void)unused;
    os_mutex_lock(&sLock);
    for (;;) {
        unsigned long long now = host_now_ns(), next = 0;      /* 0 = nothing scheduled */
        int next_precise = 0;                                  /* is `next` a device deadline (Joybus transfer, CPU timer) rather than the video clock? */

        if (sTimerArmed) {
            if (now >= sTimerDeadline) {
                sTimerArmed = 0;
                sPending |= 1u << sTimerEvent;
                host_preempt_flag = 1;
                os_cond_broadcast(&sEventCv);
            } else {
                next = sTimerDeadline;
                next_precise = 1;
            }
        }
        {   /* delayed device events: post those that are due, remember the earliest one that is not */
            int ev;
            for (ev = 0; ev < 32; ev++) {
                if (sLaterDeadline[ev] == 0) continue;
                if (now >= sLaterDeadline[ev]) {
                    sLaterDeadline[ev] = 0;
                    sPending |= 1u << ev;
                    host_preempt_flag = 1;
                    os_cond_broadcast(&sEventCv);
                } else if (next == 0 || sLaterDeadline[ev] < next) {
                    next = sLaterDeadline[ev];
                    next_precise = 1;
                }
            }
        }
        if (sViRunning) {
            while (now >= sViFieldStart + sViPeriod) {          /* a new field began (more than one if we were late) */
                sViFieldStart += sViPeriod;
                sViFieldIndex++;
                sViFired = 0;
            }
            if (!sViFired && sViEvent >= 0 && now >= sViFieldStart + sViIntrOffset) {
                sViFired = 1;
                sPending |= 1u << sViEvent;
                host_preempt_flag = 1;
                os_cond_broadcast(&sEventCv);
            }
            {
                unsigned long long vi_next = (sViFired || sViEvent < 0) ? sViFieldStart + sViPeriod : sViFieldStart + sViIntrOffset;
                if (next == 0 || vi_next < next) { next = vi_next; next_precise = 0; }
            }
        }
        if (next == 0) {
            os_cond_wait(&sTimerCv, &sLock);
        } else if (next > now) {
            unsigned long long wait = next - now;
            if (next_precise && OS_SPIN_NS && wait <= OS_SPIN_NS) {
                /* close to a device deadline: spin instead of sleeping (see OS_SPIN_NS). The lock is released meanwhile so the game thread can
                 * arm or post events; after the spin the loop starts again and recomputes every deadline, so nothing is lost. */
                os_mutex_unlock(&sLock);
                os_spin_until(next);
                os_mutex_lock(&sLock);
            } else if (next_precise && OS_SPIN_NS && wait > OS_SPIN_NS) {
                os_cond_wait_ns(&sTimerCv, &sLock, wait - OS_SPIN_NS);       /* sleep up to the spin window; may return early, then the loop re-checks */
            } else {
                os_cond_wait_ns(&sTimerCv, &sLock, wait);
            }
        }
    }
}

static void start_timer_thread_locked(void)
{
    if (!sTimerThreadStarted) {
        sTimerThreadStarted = 1;
        os_thread_start(timer_thread, NULL, 64 * 1024);
    }
}

void host_timer_arm(unsigned long long delay_ns, int event)
{
    init_once();
    os_mutex_lock(&sLock);
    sTimerDeadline = host_now_ns() + delay_ns;
    sTimerEvent = event;
    sTimerArmed = 1;
    start_timer_thread_locked();
    os_cond_signal(&sTimerCv);
    os_mutex_unlock(&sLock);
}

void host_event_post_after(unsigned long long delay_ns, int event)
{
    if (event < 0 || event >= 32) host_fatal("host_event_post_after: event number out of range");
    init_once();
    os_mutex_lock(&sLock);
    sLaterDeadline[event] = host_now_ns() + delay_ns + 1;      /* +1: 0 is the "not armed" marker */
    start_timer_thread_locked();
    os_cond_signal(&sTimerCv);
    os_mutex_unlock(&sLock);
}

void host_timer_disarm(void)
{
    init_once();
    os_mutex_lock(&sLock);
    sTimerArmed = 0;
    os_cond_signal(&sTimerCv);
    os_mutex_unlock(&sLock);
}

void host_vi_set_timing(unsigned long long field_period_ns, unsigned long long intr_offset_ns, int event)
{
    if (field_period_ns == 0) host_fatal("video field period of zero");
    init_once();
    os_mutex_lock(&sLock);
    if (!sViRunning) {
        sViRunning = 1;
        sViFieldStart = host_now_ns();
        sViFieldIndex = 0;
        sViFired = 0;
    }
    sViPeriod = field_period_ns;
    sViIntrOffset = intr_offset_ns;
    if (event != sViEvent) sViFired = 0;
    sViEvent = event;
    start_timer_thread_locked();
    os_cond_signal(&sTimerCv);
    os_mutex_unlock(&sLock);
}

static unsigned long long vi_phase(unsigned long long *field_period_ns, unsigned long long *field_index)
{
    unsigned long long now = host_now_ns(), phase = 0;
    os_mutex_lock(&sLock);
    if (sViRunning) {
        /* The timer thread advances the field start lazily; compute the same result here so the answer is exact even if it is late. */
        unsigned long long start = sViFieldStart, idx = sViFieldIndex;
        if (now >= start + sViPeriod) {
            unsigned long long n = (now - start) / sViPeriod;
            start += n * sViPeriod;
            idx += n;
        }
        phase = now - start;
        *field_period_ns = sViPeriod;
        *field_index = idx;
    } else {
        *field_period_ns = 0;
        *field_index = 0;
    }
    os_mutex_unlock(&sLock);
    return phase;
}

/* See platform.h. The division is done here, host-side, so no multi-byte value crosses the game/host boundary through a pointer. */
unsigned int host_vi_position(unsigned int v_sync)
{
    unsigned long long period, field, phase = vi_phase(&period, &field);
    unsigned int half_line;
    if (period == 0 || v_sync == 0) return 0;
    half_line = (unsigned int)(phase * v_sync / period);
    if (half_line >= v_sync) half_line = v_sync - 1;
    return (half_line << 1) | (unsigned int)(field & 1);
}

/* Nonzero when a graphics task may be run in idle time (os_sched.c HostIdle). KYUJO_GFX_NO_IDLE=1 turns that off: the task then
 * stays running until the retrace, which exercises the game's yield to audio (development only). */
int host_gfx_idle_slices(void)
{
    static int checked, on = 1;
    if (!checked) {
        checked = 1;
        on = getenv("KYUJO_GFX_NO_IDLE") == NULL;
    }
    return on;
}
