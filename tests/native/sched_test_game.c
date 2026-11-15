/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * sched_test_game.c - self-test of the native libultra scheduler layer (src/ultra/os_sched.c + src/platform/host_sched.c).
 *
 * It runs REAL libultra code (osCreateThread, osStartThread, osSendMesg, osRecvMesg, osYieldThread, osSetThreadPri, osDestroyThread)
 * on top of the replacement assembly routines, and checks behaviour that the N64 Programming Manual / libultra source defines:
 *   - the highest-priority runnable thread runs; equal priorities run first-in first-out;
 *   - a blocked osRecvMesg / osSendMesg wakes when the partner acts, and message order is preserved;
 *   - osYieldThread lets an equal-priority thread run;
 *   - a hardware event (CPU Count/Compare "counter" interrupt) is turned into a message for osSetEventMesg's queue, wakes the
 *     waiting thread and preempts a lower-priority thread that is running;
 *   - osSetIntMask(OS_IM_NONE) holds the event back until the mask is lifted;
 *   - a thread that returns from its entry function is destroyed, and its OSThread can be created again.
 * Each check prints "ok" or "FAIL" with a reason. The program's exit status is 0 only if all checks passed.
 *
 * GAME-side file (libultra headers only). Output goes through host_log, which takes plain strings.
 */
#include "PR/os_internal.h"
#include "PR/ultratypes.h"
#include "platform.h"

#define STACK_WORDS 16
#define NO_SP(name) (&name[STACK_WORDS])        /* the stack pointer is ignored by this port; any address will do */

static OSThread  sIdleThread, sDriverThread, sW[4];
static u64       sStacks[8][STACK_WORDS];
static int       sFailures, sChecks;

/* ---- tiny reporting helpers (no C library on the game side) ------------------------------------------------------------------ */

static void put_int(char *dst, int v)
{
    char tmp[12];
    int n = 0, i = 0;
    unsigned u = v < 0 ? -v : v;
    if (v < 0) dst[i++] = '-';
    do { tmp[n++] = '0' + u % 10; u /= 10; } while (u);
    while (n) dst[i++] = tmp[--n];
    dst[i] = 0;
}

static void say(const char *prefix, const char *what, int a, int b)
{
    char line[160];
    int i = 0, k;
    char num[14];
    for (k = 0; prefix[k]; k++) line[i++] = prefix[k];
    for (k = 0; what[k] && i < 120; k++) line[i++] = what[k];
    line[i++] = ' '; line[i++] = '('; line[i++] = 'g'; line[i++] = 'o'; line[i++] = 't'; line[i++] = ' ';
    put_int(num, a); for (k = 0; num[k]; k++) line[i++] = num[k];
    line[i++] = ','; line[i++] = ' '; line[i++] = 'w'; line[i++] = 'a'; line[i++] = 'n'; line[i++] = 't'; line[i++] = ' ';
    put_int(num, b); for (k = 0; num[k]; k++) line[i++] = num[k];
    line[i++] = ')'; line[i] = 0;
    host_log(line);
}

static void check_eq(const char *what, int got, int want)
{
    sChecks++;
    if (got == want) { host_log(what); }
    else { sFailures++; say("FAIL: ", what, got, want); }
}

static void check(const char *what, int cond)
{
    sChecks++;
    if (cond) host_log(what);
    else { sFailures++; host_log("FAIL: condition false:"); host_log(what); }
}

/* A thread that wakes a higher-priority thread inside osSendMesg is put back on the run queue before it has returned from that
 * call (libultra's own rule). So after a scenario its helper threads may still be alive. Dropping the driver below them lets
 * them run to their end (each is destroyed when its entry function returns) before the next scenario reuses the OSThreads. */
static void let_lower_threads_finish(void)
{
    osSetThreadPri(NULL, 2);
    osSetThreadPri(NULL, 5);
}

/* ---- scenario 1: priorities and FIFO order ------------------------------------------------------------------------------------ */

static int         sLog[16], sLogN;
static OSMesgQueue sDoneQ;
static OSMesg      sDoneBuf[8];

static void logger_thread(void *arg)
{
    sLog[sLogN++] = (int)(unsigned long)arg;
    osSendMesg(&sDoneQ, (OSMesg)0, OS_MESG_BLOCK);
}

static void scenario_priority_and_fifo(void)
{
    OSMesg m;
    int i;
    host_log("-- scenario 1: priority and first-in-first-out order");
    osCreateMesgQueue(&sDoneQ, sDoneBuf, 8);
    /* Driver runs at priority 5; the workers (priority 3) cannot run until the driver blocks. Started in the order 2,3,1. */
    osCreateThread(&sW[0], 20, logger_thread, (void *)2, NO_SP(sStacks[0]), 3);
    osCreateThread(&sW[1], 21, logger_thread, (void *)3, NO_SP(sStacks[1]), 3);
    osCreateThread(&sW[2], 22, logger_thread, (void *)1, NO_SP(sStacks[2]), 3);
    osStartThread(&sW[0]);
    osStartThread(&sW[1]);
    osStartThread(&sW[2]);
    check_eq("lower-priority workers did not run before the driver blocked", sLogN, 0);
    for (i = 0; i < 3; i++) osRecvMesg(&sDoneQ, &m, OS_MESG_BLOCK);
    check_eq("three workers ran", sLogN, 3);
    let_lower_threads_finish();
    check_eq("worker order 1st (started first)", sLog[0], 2);
    check_eq("worker order 2nd", sLog[1], 3);
    check_eq("worker order 3rd", sLog[2], 1);

    /* A higher-priority thread preempts at once when started. */
    sLogN = 0;
    osCreateThread(&sW[3], 23, logger_thread, (void *)9, NO_SP(sStacks[3]), 8);
    osStartThread(&sW[3]);
    check_eq("higher-priority thread ran immediately when started", sLogN, 1);
    osRecvMesg(&sDoneQ, &m, OS_MESG_BLOCK);
    let_lower_threads_finish();
}

/* ---- scenario 2: blocking producer / consumer, order preserved --------------------------------------------------------------- */

static OSMesgQueue sDataQ;
static OSMesg      sDataBuf[1];                     /* capacity 1: every send after the first must block until a receive */
static int         sSum, sOrderOk = 1;

static void producer_thread(void *arg)
{
    int i;
    (void)arg;
    for (i = 1; i <= 20; i++) osSendMesg(&sDataQ, (OSMesg)(unsigned long)i, OS_MESG_BLOCK);
}

static void scenario_producer_consumer(void)
{
    OSMesg m;
    int i, expect = 1;
    host_log("-- scenario 2: blocking producer/consumer through a 1-slot queue");
    osCreateMesgQueue(&sDataQ, sDataBuf, 1);
    osCreateThread(&sW[0], 30, producer_thread, 0, NO_SP(sStacks[0]), 3);
    osStartThread(&sW[0]);
    for (i = 0; i < 20; i++) {
        osRecvMesg(&sDataQ, &m, OS_MESG_BLOCK);
        if ((int)(unsigned long)m != expect++) sOrderOk = 0;
        sSum += (int)(unsigned long)m;
    }
    check("messages arrived in order", sOrderOk);
    check_eq("sum of 1..20", sSum, 210);
    let_lower_threads_finish();
}

/* ---- scenario 3: yield between equal priorities ------------------------------------------------------------------------------- */

static void yielder_thread(void *arg)
{
    int i;
    for (i = 0; i < 3; i++) {
        sLog[sLogN++] = (int)(unsigned long)arg;
        osYieldThread();
    }
    osSendMesg(&sDoneQ, (OSMesg)0, OS_MESG_BLOCK);
}

static void scenario_yield(void)
{
    OSMesg m;
    host_log("-- scenario 3: osYieldThread alternates equal-priority threads");
    sLogN = 0;
    osCreateThread(&sW[0], 40, yielder_thread, (void *)1, NO_SP(sStacks[0]), 3);
    osCreateThread(&sW[1], 41, yielder_thread, (void *)2, NO_SP(sStacks[1]), 3);
    osStartThread(&sW[0]);
    osStartThread(&sW[1]);
    osRecvMesg(&sDoneQ, &m, OS_MESG_BLOCK);
    osRecvMesg(&sDoneQ, &m, OS_MESG_BLOCK);
    let_lower_threads_finish();
    check_eq("yield log length", sLogN, 6);
    check("yield alternates 1,2,1,2,1,2", sLog[0] == 1 && sLog[1] == 2 && sLog[2] == 1 && sLog[3] == 2 && sLog[4] == 1 && sLog[5] == 2);
}

/* ---- scenario 4: a hardware event wakes a blocked thread (Count/Compare) ------------------------------------------------------- */

static OSMesgQueue sEvQ;
static OSMesg      sEvBuf[4];

static void scenario_timer_event(void)
{
    OSMesg m;
    u32 t0, dt;
    host_log("-- scenario 4: Count/Compare interrupt becomes a message for osSetEventMesg");
    osCreateMesgQueue(&sEvQ, sEvBuf, 4);
    osSetEventMesg(OS_EVENT_COUNTER, &sEvQ, (OSMesg)0x1234);
    {
        int attempt, early = 0, on_time = 0, delivered_ok = 1;
        /* 20 ms requested. A correct implementation never wakes EARLY (checked on every attempt). It wakes late only through host
         * scheduling jitter, which is negligible on an idle machine but can be tens of milliseconds on a busy one; so "on time"
         * (at most 20 ms * 1.5 + 5 ms = 35 ms) is required on at least one of up to 10 attempts. A timer that is really too slow
         * (checked by deliberately breaking the conversion) fails every attempt. */
        for (attempt = 0; attempt < 10 && !on_time; attempt++) {
            t0 = osGetCount();
            __osSetCompare(t0 + OS_CPU_COUNTER / 50);
            osRecvMesg(&sEvQ, &m, OS_MESG_BLOCK);
            dt = osGetCount() - t0;
            if ((int)(unsigned long)m != 0x1234) delivered_ok = 0;
            if (dt < OS_CPU_COUNTER / 1000 * 19) early = 1;
            if (dt <= OS_CPU_COUNTER / 1000 * 35) on_time = 1;
        }
        check("the event delivered the registered message", delivered_ok);
        check("never woke earlier than 19 ms", !early);
        check("woke within 35 ms (at least once in 10 attempts)", on_time);
    }
    __osSetCompare(0);
    {
        /* Count must advance at OS_CPU_COUNTER ticks per second: compare it with the host's clock over 50 ms (+-5 %). */
        unsigned long long h0 = host_now_ns(), h1;
        u32 c0 = osGetCount(), c1;
        while ((h1 = host_now_ns()) - h0 < 50000000ULL) { }
        c1 = osGetCount();
        {
            unsigned long long expect = (h1 - h0) / 1000 * (OS_CPU_COUNTER / 1000) / 1000;      /* ns -> us -> ticks */
            unsigned long long got = (u32)(c1 - c0);
            check("Count advances at 46.875 MHz (+-5%)", got * 100 >= expect * 95 && got * 100 <= expect * 105);
        }
    }
}

/* ---- scenario 5: the event preempts a lower-priority thread that is running ------------------------------------------------------ */

static volatile int sSpinCount, sSpinDone;

static void spinner_thread(void *arg)
{
    u32 end = osGetCount() + OS_CPU_COUNTER / 5;              /* spin up to 200 ms */
    (void)arg;
    while ((s32)(end - osGetCount()) > 0 && !sSpinDone) {
        sSpinCount++;
        __osRestoreInt(__osDisableInt());                     /* a point where a real CPU would take a pending interrupt */
    }
    osSendMesg(&sDoneQ, (OSMesg)0, OS_MESG_BLOCK);
}

static void scenario_preemption(void)
{
    OSMesg m;
    int spun;
    host_log("-- scenario 5: event preempts a running lower-priority thread");
    while (sEvQ.validCount) osRecvMesg(&sEvQ, &m, OS_MESG_NOBLOCK);
    osCreateThread(&sW[0], 50, spinner_thread, 0, NO_SP(sStacks[0]), 3);
    osStartThread(&sW[0]);                                    /* lower priority than the driver (5): does not run yet */
    __osSetCompare(osGetCount() + OS_CPU_COUNTER / 50);       /* 20 ms */
    osRecvMesg(&sEvQ, &m, OS_MESG_BLOCK);                     /* blocks: the spinner runs; the event must preempt it */
    spun = sSpinCount;
    check("spinner had been running when the event arrived", spun > 0);
    check("spinner was preempted before finishing", sDoneQ.validCount == 0);
    sSpinDone = 1;                                            /* let it finish */
    osRecvMesg(&sDoneQ, &m, OS_MESG_BLOCK);
    let_lower_threads_finish();
    __osSetCompare(0);
}

/* ---- scenario 6: interrupt mask holds an event back ------------------------------------------------------------------------------ */

static void scenario_mask(void)
{
    OSMesg m;
    OSIntMask old;
    u32 end;
    host_log("-- scenario 6: osSetIntMask(OS_IM_NONE) holds the event until the mask is lifted");
    while (sEvQ.validCount) osRecvMesg(&sEvQ, &m, OS_MESG_NOBLOCK);
    old = osSetIntMask(OS_IM_NONE);
    __osSetCompare(osGetCount() + OS_CPU_COUNTER / 100);      /* 10 ms */
    end = osGetCount() + OS_CPU_COUNTER / 20;                 /* busy for 50 ms */
    while ((s32)(end - osGetCount()) > 0) __osRestoreInt(__osDisableInt());
    check_eq("no message while masked", sEvQ.validCount, 0);
    osSetIntMask(old);
    check_eq("message delivered once unmasked", sEvQ.validCount, 1);
    osRecvMesg(&sEvQ, &m, OS_MESG_NOBLOCK);
    __osSetCompare(0);
}

/* ---- scenario 7: a thread that returns is destroyed and its OSThread can be reused --------------------------------------------------- */

static void scenario_reuse(void)
{
    OSMesg m;
    int round;
    host_log("-- scenario 7: finished thread is destroyed; the same OSThread can be created again");
    for (round = 0; round < 3; round++) {
        sLogN = 0;
        osCreateThread(&sW[0], 60, logger_thread, (void *)(round + 7), NO_SP(sStacks[0]), 3);
        osStartThread(&sW[0]);
        osRecvMesg(&sDoneQ, &m, OS_MESG_BLOCK);
        let_lower_threads_finish();
        check_eq("reused thread ran with its new argument", sLog[0], round + 7);
    }
    /* destroy a thread that is blocked */
    osCreateThread(&sW[1], 61, producer_thread, 0, NO_SP(sStacks[1]), 3);
    osCreateMesgQueue(&sDataQ, sDataBuf, 1);
    osStartThread(&sW[1]);
    osSetThreadPri(NULL, 2);                                  /* below the producer (3): it runs now, fills the queue, blocks */
    osSetThreadPri(NULL, 5);                                  /* back to the driver's own priority */
    osDestroyThread(&sW[1]);
    check("destroying a blocked thread did not crash", 1);
}

/* ---- thread bodies ----------------------------------------------------------------------------------------------------------------- */

static void driver_thread(void *arg)
{
    (void)arg;
    osStartThread(&sIdleThread);                              /* lowest priority: runs only when the driver blocks */
    scenario_priority_and_fifo();
    scenario_producer_consumer();
    scenario_yield();
    scenario_timer_event();
    scenario_preemption();
    scenario_mask();
    scenario_reuse();
    {
        char line[64];
        int i = 0, k;
        const char *a = sFailures ? "SCHEDULER SELF-TEST FAILED: failures=" : "SCHEDULER SELF-TEST PASSED: checks=";
        char num[14];
        for (k = 0; a[k]; k++) line[i++] = a[k];
        put_int(num, sFailures ? sFailures : sChecks);
        for (k = 0; num[k]; k++) line[i++] = num[k];
        line[i] = 0;
        host_log(line);
    }
    host_exit(sFailures ? 1 : 0);
}

extern void HostIdle(void);

static void idle_thread(void *arg)
{
    (void)arg;
    for (;;) HostIdle();
}

/* Called by the host-side main(): runs on the boot context, which is abandoned as soon as the first thread is started. */
void sched_test_boot(void)
{
    osCreateThread(&sIdleThread, 1, idle_thread, 0, NO_SP(sStacks[6]), 1);
    osCreateThread(&sDriverThread, 2, driver_thread, 0, NO_SP(sStacks[7]), 5);
    osStartThread(&sDriverThread);
    host_fatal("osStartThread returned to the boot context");
}
