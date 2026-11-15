/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * os_sched.c - replacements for the MIPS assembly that sits underneath libultra's thread scheduler.
 *
 * libultra's thread, message-queue and timer code (the .c files in lib/ultralib/src/os) is ordinary C and is compiled unchanged. It relies on a
 * handful of routines that are written in MIPS assembly because they save and restore CPU registers and handle CPU interrupts
 * (lib/ultralib/src/os/exceptasm.s, interrupt.s, setintmask.s, getcount.s ...). This file provides those routines natively.
 * Each function below says which assembly routine it stands for and where its behaviour was taken from.
 *
 * How a "context switch" works here: every N64 thread has a host thread (platform.h). Only the thread that holds the baton runs.
 * Switching to thread T means: pass T the baton, then sleep until someone passes it back. Since only one thread ever runs, the
 * "CPU registers" need no saving: they live on each host thread's own stack.
 *
 * Interrupts: devices raise events on their own host threads (host_event_post). The running thread notices pending events
 * at the points where a real CPU would take an interrupt: when interrupts are (re-)enabled in __osRestoreInt, and in HostIdle().
 * Code that never calls into the OS therefore cannot be preempted; this differs from hardware, which can interrupt anywhere, and
 * is the main deliberate approximation of this layer.
 *
 * GAME-side file: includes libultra headers, never the C library's (see src/platform/platform.h).
 */
#include "PR/os_internal.h"
#include "PR/R4300.h"
#include "osint.h"
#include "platform.h"

#define HOST_STACK_BYTES (1024 * 1024)    /* native code with stack frames larger than MIPS needs more room than the game provides */
/* Slots are never reused: a thread destroyed while another runs leaves its host thread parked for good (it cannot be woken to
 * exit, since only the baton holder may run), so its slot must stay valid. The game creates well under this many in total. */
#define MAX_THREADS 256

/* The interrupt-mask bits a new thread starts with (createthread.c: OS_IM_ALL) without the global-enable bit, which is tracked
 * separately in sIE. Bits 8..15 are the CPU's IM0..IM7, bits 16..21 the RCP sources (SP, SI, AI, VI, PI, DP). */
#define IM_BITS (OS_IM_ALL & ~1u)

/* One entry per N64 thread that has been dispatched at least once, plus one for the boot context. */
typedef struct {
    OSThread   *os;        /* NULL for the boot context (the code that runs Main() before any thread exists) */
    HostThread *host;
    u32         ie;        /* this context's interrupt-enable flag, saved while it does not run (hardware: SR.IE in the saved context) */
    u32         mask;      /* its interrupt mask bits, likewise (hardware: SR.IM and the saved RCP mask) */
    int         dead;      /* osDestroyThread has been called on it: its host thread ends instead of waiting */
} ThreadSlot;

static ThreadSlot  sSlots[MAX_THREADS];
static int         sSlotCount;
static ThreadSlot  sBootSlot;
static ThreadSlot *sCurrent;          /* the slot whose host thread holds the baton */
static u32         sIE;               /* interrupt-enable flag of the running context (SR.IE) */
static u32         sMask = IM_BITS;   /* interrupt mask bits of the running context */
static int         sInServicing;      /* nonzero while service_events() is running: events are not nested */

_Static_assert(sizeof(void *) == 4, "this port assumes 32-bit pointers (see docs/architecture.md, decision D4)");

static ThreadSlot *current_slot(void)
{
    if (sCurrent == NULL) {
        sBootSlot.os = NULL;
        sBootSlot.host = host_thread_boot();
        sBootSlot.ie = 0;                 /* the boot context starts with interrupts off, as the CPU does after reset */
        sBootSlot.mask = IM_BITS;
        sCurrent = &sBootSlot;
        sIE = 0;
    }
    return sCurrent;
}

static void thread_entry(void *p);

/* For the stall report (platform.h, host_dbg): what each thread is, its state and the queue it waits on. Written at thread switches and polls. */
static void dbg_threads(void)
{
    int i, n = sSlotCount < 24 ? sSlotCount : 24, first = sSlotCount - n;      /* the newest 24 (slots of destroyed threads stay in the table) */
    for (i = 0; i < n; i++) {
        const OSThread *t = sSlots[first + i].os;
        if (!t || sSlots[first + i].dead) { host_dbg[32 + i * 3] = 0; host_dbg[33 + i * 3] = 0xDEAD0000u; continue; }
        host_dbg[32 + i * 3] = (unsigned)t->id << 16 | ((unsigned)t->priority & 0xFFFFu);
        host_dbg[33 + i * 3] = (unsigned)t->state << 16 | ((unsigned)(unsigned long)t->queue & 0xFFFFu);
        host_dbg[34 + i * 3] = (unsigned)t->context.pc;
    }
    host_dbg[10] = (unsigned)n;
}

/* Finds the live slot of `t`, creating it (and its host thread, which does not start until it gets the baton) on first use. */
static ThreadSlot *slot_for(OSThread *t)
{
    int i;
    ThreadSlot *s;
    for (i = 0; i < sSlotCount; i++) {
        if (sSlots[i].os == t && !sSlots[i].dead) return &sSlots[i];
    }
    if (sSlotCount == MAX_THREADS) host_fatal("more than 256 game threads created (see MAX_THREADS in os_sched.c)");
    s = &sSlots[sSlotCount++];
    s->os = t;
    s->dead = 0;
    s->ie = 1;                                          /* createthread.c gives new threads OS_IM_ALL, which includes SR_IE */
    s->mask = IM_BITS;
    s->host = host_thread_new(thread_entry, s, HOST_STACK_BYTES);
    return s;
}

/* First code run by the host thread of an N64 thread: calls the thread's entry function with its argument, as the CPU does when the
 * thread is first dispatched (createthread.c stored entry in context.pc and the argument in context.a0), then ends the thread
 * the way the original does when the entry function returns (it "returns" into __osCleanupThread). */
static void thread_entry(void *p)
{
    ThreadSlot *s = p;
    void (*fn)(void *) = (void (*)(void *))(unsigned long)s->os->context.pc;
    void *arg = (void *)(unsigned long)s->os->context.a0;
    sIE = s->ie;
    sMask = s->mask;
    fn(arg);
    __osCleanupThread();
    host_fatal("a thread was resumed after osDestroyThread");
}

/* Hands the baton to thread `t` and sleeps until it comes back. If `t` is the thread already running here there is nothing to do.
 * If the calling thread has been destroyed (osDestroyThread on itself) it never comes back: its host thread ends. */
static void transfer_to(OSThread *t)
{
    ThreadSlot *self = current_slot();
    ThreadSlot *to = slot_for(t);
    if (to == self) return;
    if (self->dead) {
        HostThread *h = self->host;
        self->host = NULL;
        sCurrent = to;
        host_thread_end(h, to->host);
    }
    self->ie = sIE;
    self->mask = sMask;
    sCurrent = to;
    dbg_threads();
    host_baton_pass(to->host);
    host_baton_wait(self->host);
    /* Back: whoever woke us has already set sCurrent to us. Restore the interrupt state we left with. */
    sIE = self->ie;
    sMask = self->mask;
}

/* An identity for the thread that is running now (the slot that holds the baton), stable for as long as the thread exists. Host code
 * that needs per-thread state (the machine-code interpreter's stacks, host_mips.c) keys it on this. */
const void *HostCurrentThreadId(void)
{
    return current_slot();
}

/* Called from osDestroyThread (patch 0003) once the thread is off all queues, before the scheduler picks the next thread. */
void __osHostThreadDestroyed(OSThread *t)
{
    int i;
    for (i = 0; i < sSlotCount; i++) {
        if (sSlots[i].os == t) sSlots[i].dead = 1;
    }
}

/* ---- queue primitives (exceptasm.s: __osEnqueueThread, __osPopThread) ---------------------------------------------------- */

/* Inserts `t` into the priority-ordered list `*queue`: before every thread of LOWER priority, after threads of equal or higher
 * priority (so equal priorities are served first-in first-out). The list ends in a sentinel with priority -1 (__osThreadTail).
 * `queue` is treated as a pointer to an OSThread whose first member is `next`, exactly as the assembly does. */
void __osEnqueueThread(OSThread **queue, OSThread *t)
{
    OSThread *pred = (OSThread *)queue;
    OSThread *succ = *queue;
    if (succ->priority >= t->priority) {            /* the assembly's first test: "blt head, new -> insert at front" */
        while (succ->priority >= t->priority) {
            pred = succ;
            succ = succ->next;
        }
    }
    t->next = pred->next;
    pred->next = t;
    t->queue = queue;
}

/* Removes and returns the first thread of `*queue`. */
OSThread *__osPopThread(OSThread **queue)
{
    OSThread *t = *queue;
    *queue = t->next;
    return t;
}

/* ---- dispatching (exceptasm.s: __osDispatchThread, __osEnqueueAndYield, __osCleanupThread) -------------------------------- */

/* Runs the highest-priority runnable thread. On the CPU this never returns to its caller; here it returns when the caller's own
 * thread is dispatched again (callers that are being destroyed or are the boot context simply never get the baton back). */
void __osDispatchThread(void)
{
    OSThread *t = __osPopThread(&__osRunQueue);
    if (t == (OSThread *)&__osThreadTail) host_fatal("no runnable thread (the game's idle thread is missing)");
    __osRunningThread = t;
    t->state = OS_STATE_RUNNING;
    transfer_to(t);
}

/* Saves the running thread, puts it on `queue` (unless queue is NULL) and dispatches the next thread. Returns when the saved
 * thread is dispatched again. */
void __osEnqueueAndYield(OSThread **queue)
{
    OSThread *self = __osRunningThread;
    if (queue != NULL) __osEnqueueThread(queue, self);
    __osDispatchThread();
}

/* Where a thread's entry function "returns to". */
void __osCleanupThread(void)
{
    osDestroyThread(NULL);
}

/* ---- interrupts ------------------------------------------------------------------------------------------------------------- */

/* Interrupt-mask bit that enables each event (os_exception.h: OS_IM_*), or 0 for events that cannot be masked. */
static u32 event_mask_bit(int ev)
{
    switch (ev) {
    case OS_EVENT_SW1:     return 0x00000100;
    case OS_EVENT_SW2:     return 0x00000200;
    case OS_EVENT_CART:    return 0x00000800;
    case OS_EVENT_PRENMI:  return 0x00001000;
    case OS_EVENT_COUNTER: return 0x00008000;
    case OS_EVENT_SP:      return 0x00010000 | 0x400;      /* RCP sources also need the RCP line (IM2) enabled */
    case OS_EVENT_SP_BREAK:return 0x00010000 | 0x400;
    case OS_EVENT_SI:      return 0x00020000 | 0x400;
    case OS_EVENT_AI:      return 0x00040000 | 0x400;
    case OS_EVENT_VI:      return 0x00080000 | 0x400;
    case OS_EVENT_PI:      return 0x00100000 | 0x400;
    case OS_EVENT_DP:      return 0x00200000 | 0x400;
    default:               return 0;
    }
}

/* hw_regs.c: clears the device-side interrupt state, as the console's exception handler does when it takes the interrupt. */
extern void HostHwAckEvent(int event);

/* exceptasm.s: send_mesg. Delivers one event: stores the message registered with osSetEventMesg in its queue (never blocking, and
 * silently dropping it if the queue is full or nothing is registered) and makes a thread waiting for it runnable. */
static void send_event_message(int ev)
{
    __OSEventState *es = &__osEventStateTab[ev];
    OSMesgQueue *mq = es->messageQueue;
    if (mq == NULL) return;
    if (mq->validCount >= mq->msgCount) return;
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = es->message;
    mq->validCount++;
    if (mq->mtqueue->next != NULL) {
        OSThread *waiter = __osPopThread(&mq->mtqueue);
        __osEnqueueThread(&__osRunQueue, waiter);          /* note: its state stays OS_STATE_WAITING until dispatched, as in the original */
    }
}

/* The interrupt entry (exceptasm.s: __osException) for the events pending right now. The running thread is interrupted, every
 * deliverable event is turned into a message, and then the scheduler re-evaluates who runs: if the interrupted thread still has
 * the highest priority it goes back to the FRONT of the run queue (it resumes before other threads of equal priority); otherwise
 * it is queued by priority and a more important thread runs first. */
/* Trace (KYUJO_AUDIO_TRACE=1): the scheduler's task slots just before each video retrace is delivered, for the first 240 retraces:
 *   SCHEDTRACE retrace-number audio-runs-started audio-runs-done A14 A18 A1C A20 sp-events audio-frames-built t-ms
 * where each slot is 0 (empty) or the type of the task in it (1 graphics, 2 audio, 4 other). D_800A62E0 is the game's scheduler state (src/scheduler.h). */
extern unsigned char D_800A62E0[];
extern unsigned HostAiLenReads;
static unsigned slot_type(unsigned off)
{
    unsigned long p = (unsigned long)*(unsigned *)(D_800A62E0 + off);
    return p ? *(unsigned *)(p + 0x20) : 0;
}
static void trace_retrace(void)
{
    static unsigned n;
    unsigned vals[12], vi, d, pos = 0;
    char m[160];
    if (n >= 240) return;
    vals[0] = n++;
    vals[1] = HostHwRspJobSeq();
    vals[2] = HostHwRspJobsDone();
    vals[3] = slot_type(0xA14);
    vals[4] = slot_type(0xA18);
    vals[5] = slot_type(0xA1C);
    vals[6] = slot_type(0xA20);
    vals[7] = host_dbg[16 + 4];
    vals[8] = HostAiLenReads;
    vals[9] = (unsigned)(host_now_ns() / 1000000ull);
    for (vi = 0; vi < 10; vi++) {
        char t[12];
        unsigned tn = 0, v = vals[vi];
        if (vi == 0) { const char *l = "SCHEDTRACE"; while (*l) m[pos++] = *l++; }
        m[pos++] = ' ';
        if (v == 0) t[tn++] = '0';
        while (v) { t[tn++] = (char)('0' + v % 10); v /= 10; }
        for (d = tn; d > 0; d--) m[pos++] = t[d - 1];
    }
    m[pos] = 0;
    host_log(m);
}

/* The game's audio thread (src/DDC0.c: Audio_StartThread, thread id 4, priority 80). Each video field the scheduler thread wakes it with a message; it then builds the next audio
 * frame (about a millisecond of work on the console, tens of milliseconds on a starved host) and hands the task to the scheduler, which starts it at the NEXT retrace. The scheduler
 * keeps room for ONE such task: if a second retrace arrives while the audio thread is still at work the first task has not been handed over yet, the audio thread's next message
 * is already waiting, it builds again at once, and the second task replaces the first, which never runs: its output buffer is not rewritten and the sound output plays the
 * previous contents again (the garbled "Pika" voice). On the console the frame is always finished long before the next retrace. So a retrace is held back, here, for as long as the
 * audio thread is at work (it is runnable or running): the host being slow then costs a late field, as it does for the rest of the game, instead of a dropped audio task.
 * The hold is bounded (AUDIO_HOLD_MAX_NS) so that a thread that never comes back cannot stop the game. */
#define AUDIO_THREAD_ID   4
#define AUDIO_THREAD_PRI  80
#define AUDIO_HOLD_MAX_NS (100ULL * 1000000ULL)
static unsigned long long sViHeldSince;                    /* host clock when the first retrace was held back (0: none is) */

static int audio_thread_at_work(void)
{
    int i;
    for (i = 0; i < sSlotCount; i++) {
        const OSThread *t = sSlots[i].os;
        if (t && !sSlots[i].dead && t->id == AUDIO_THREAD_ID && t->priority == AUDIO_THREAD_PRI)
            return t->state == OS_STATE_RUNNABLE || t->state == OS_STATE_RUNNING;
    }
    return 0;
}

static int hold_retrace(void)
{
    unsigned long long now;
    if (!audio_thread_at_work()) { sViHeldSince = 0; return 0; }
    now = host_now_ns();
    if (sViHeldSince == 0) sViHeldSince = now;
    if (now - sViHeldSince > AUDIO_HOLD_MAX_NS) { sViHeldSince = 0; return 0; }
    return 1;
}

static int service_events(void)
{
    OSThread *cur = __osRunningThread;
    u32 savedIE = sIE, savedMask = sMask;
    int ev, handled = 0, skipped[32], nskipped = 0, i;

    if (cur == NULL || sInServicing || !host_event_pending()) return 0;
    /* The audio run that is still on its worker (about 1 ms of work, but on a loaded machine a starved thread may need tens of ms) ends first and is reported (event SP,
     * a lower number than VI, so it is taken first below): on the console it is long over when a retrace comes. If the scheduler thread gets the retrace first it finds the last
     * audio task still active, cannot start the next one in time and replaces it with the one after: an output buffer is not rewritten and is played again. */
    HostHwRspSettle();
    sInServicing = 1;
    sIE = 0;                                               /* interrupts are off while an interrupt is handled */
    cur->state = OS_STATE_RUNNABLE;
    while ((ev = host_event_take()) >= 0) {
        u32 bit = event_mask_bit(ev);
        if (bit != 0 && (savedMask & bit) != bit) {        /* masked: keep it pending for when the mask is lifted */
            skipped[nskipped++] = ev;
            continue;
        }
        if (ev == OS_EVENT_VI && hold_retrace()) {         /* see audio_thread_at_work: not while the audio thread is building its frame */
            skipped[nskipped++] = ev;
            continue;
        }
        if (ev == OS_EVENT_VI && sViHeldSince != 0) {      /* a retrace that was held back is delivered now: say so when it was long (at most 20 lines) */
            static unsigned reported;
            unsigned long long held = host_now_ns() - sViHeldSince;
            if (held > 20000000ULL && reported < 20) {
                char m[96];
                unsigned ms = (unsigned)(held / 1000000ULL), n = 0, d;
                char t[12];
                const char *l = "scheduler: a video retrace was held back for the audio thread, ms: ";
                while (*l) m[n++] = *l++;
                if (ms == 0) t[0] = '0';
                for (d = 0; ms; ms /= 10) t[d++] = (char)('0' + ms % 10);
                while (d) m[n++] = t[--d];
                m[n] = 0;
                host_log(m);
                reported++;
            }
        }
        if (ev == OS_EVENT_VI) sViHeldSince = 0;
        if (ev == OS_EVENT_VI && host_audio_trace_enabled()) trace_retrace();
        if (ev == OS_EVENT_VI || ev == OS_EVENT_SP) HostTraceEv(ev == OS_EVENT_VI ? "event-VI" : "event-SP", (unsigned)ev);
        host_trace("event delivered", (unsigned int)ev, 0);
        if (ev < 16) host_dbg[16 + ev]++;
        HostHwAckEvent(ev);
        send_event_message(ev);
        handled = 1;
    }
    for (i = 0; i < nskipped; i++) host_event_post(skipped[i]);
    if (!handled) {
        cur->state = OS_STATE_RUNNING;
        sIE = savedIE;
        sInServicing = 0;
        return 0;
    }
    if (cur->priority >= __osRunQueue->priority) {
        cur->next = __osRunQueue;                          /* exceptasm.s: enqueueRunning */
        __osRunQueue = cur;
        cur->queue = &__osRunQueue;
    } else {
        __osEnqueueThread(&__osRunQueue, cur);
    }
    sInServicing = 0;                                      /* allow the threads that run now to take interrupts */
    __osDispatchThread();                                  /* returns when `cur` runs again */
    sIE = savedIE;
    sMask = savedMask;
    return 1;
}

/* interrupt.s: __osDisableInt. Turns interrupts off and returns whether they were on (SR_IE, i.e. 1 or 0). */
u32 __osDisableInt(void)
{
    u32 old;
    (void)current_slot();
    old = sIE;
    sIE = 0;
    return old;
}

/* interrupt.s: __osRestoreInt. Turns interrupts back on if `mask` says they were on. Events that arrived meanwhile are taken now. */
void __osRestoreInt(u32 mask)
{
    (void)current_slot();
    if (mask) {
        sIE = 1;
        service_events();
    }
}

/* setintmask.s: osSetIntMask. Replaces the interrupt mask bits and returns the old ones. The global enable flag (bit 0) is NOT
 * changed by this call (the assembly masks it out when writing the status register) but is reported in the returned value. */
OSIntMask osSetIntMask(OSIntMask im)
{
    OSIntMask old;
    (void)current_slot();
    old = (sMask & ~1u) | (sIE & 1u);
    sMask = im & IM_BITS;
    if (sIE) service_events();                             /* unmasking may release a pending event */
    return old;
}

/* Called by the scheduling points at the end of every loop iteration of the game's compiled code while an event is pending
 * (tools/native/be_memory.py rule 5, host_preempt_flag): lets the running thread take the "interrupt" at that point, like HostIdle does.
 * Interrupts that are disabled or masked stay pending; then the flag stays set, so after a poll that delivers nothing the next polls
 * are skipped for a while instead of taking the lock every iteration. */
#define GFX_PREEMPT_SLICE 128     /* graphics commands run at a preemption point (small: the game thread goes on afterwards) */
#define GFX_IDLE_SLICE    512     /* graphics commands run per idle pass (the idle thread has nothing else to do) */

void HostPreemptPoll(void)
{
    static unsigned skip;
    HostHwGfxPump(GFX_PREEMPT_SLICE);      /* a graphics task still running gets a slice here too (hw_sp.c) */
    /* An audio microcode run that has ended on its worker thread is reported (SP interrupt) BEFORE a video retrace is delivered below, as on the console, where the run
     * (about 1 ms) ends long before the next retrace. Without this the end was only noticed in the idle loop or when the game read SP_STATUS; a game thread that was busy
     * for a frame delivered the retrace first, the scheduler then found the audio task of the last frame still "running", did not start the new one in time, and replaced
     * it with the next one: one of the three output buffers was not rewritten and was played again (the garbled "Pika" voice on Windows). */
    HostHwRspPump();
    host_dbg[4]++;
    host_dbg[6] = sIE | sMask << 8;
    if (skip) { skip--; return; }
    dbg_threads();
    (void)current_slot();
    if (!sIE || !service_events()) skip = 255;
    else host_dbg[5]++;
    if (__osRunningThread) host_dbg[7] = (unsigned)__osRunningThread->id << 16 | (unsigned)__osRunningThread->priority;
    if (__osRunQueue) host_dbg[8] = (unsigned)__osRunQueue->priority;
}

/* Called from the idle thread's endless loop (patch 0004) in place of an empty loop: the CPU would be interrupted there, we sleep
 * until a device event arrives and then take it. An event that is pending but masked stays pending, so waiting for "an event" would
 * return at once forever; in that case sleep briefly instead of spinning. */
void HostIdle(void)
{
    int handled = 0;
    HostHwRspPump();                                       /* an audio microcode run on its worker thread may have ended (hw_sp.c) */
    if (HostHwGfxBusy() && host_gfx_async_enabled()) {
        HostHwGfxPump(1);                                  /* the task runs on its worker thread: collect it if it has ended, else sleep (its end wakes us) */
    } else if (host_gfx_idle_slices() && HostHwGfxBusy()) {     /* work is waiting for this idle time: run a slice of it, then take what arrived */
        HostHwGfxPump(GFX_IDLE_SLICE);
        if (sIE) service_events();
        return;
    }
    (void)host_event_wait(0);
    HostHwRspPump();                                       /* ... it may be what woke us */
    if (sIE) handled = service_events();
    if (!handled && host_event_pending()) host_sleep_ns(100000);      /* 0.1 ms */
}

/* ---- CP0 Count / Compare (getcount.s, setcount.s, setcompare.s) ------------------------------------------------------------- */

static u32 sCountOffset;                                   /* Count = time-derived ticks + offset */

/* Count register: increments at OS_CPU_COUNTER (46,875,000) Hz. 46,875,000 / 1e9 = 3 / 64, exactly. */
static u32 count_now(void)
{
    return (u32)((host_now_ns() * 3ULL) / 64ULL) + sCountOffset;
}

u32 osGetCount(void)
{
    return count_now();
}

void __osSetCount(u32 v)
{
    sCountOffset = v - (u32)((host_now_ns() * 3ULL) / 64ULL);
}

/* Compare register: the "counter" interrupt fires when Count reaches Compare. A value of 0 is used by libultra to mean "no timer is
 * pending" (timerintr.c), so it disarms here instead of firing when the 32-bit counter wraps (about every 91 seconds). */
void __osSetCompare(u32 v)
{
    u32 delta;
    if (v == 0) {
        host_timer_disarm();
        return;
    }
    delta = v - count_now();
    if (delta == 0) delta = 1;
    host_timer_arm(((unsigned long long)delta * 64ULL) / 3ULL, OS_EVENT_COUNTER);
}
