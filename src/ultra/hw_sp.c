/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * hw_sp.c - the RSP's registers and memories, and the RDP's command registers, as libultra uses them (lib/ultralib/src/io/sp*.c,
 * sptask.c, dp*.c). Register names and bit meanings are from lib/ultralib/include/PR/rcp.h; the DMA length format (length, count, skip) is
 * from n64brew's "Reality Signal Processor" page.
 *
 * When libultra starts the RSP (osSpTaskStartGo clears the HALT bit) there are two cases:
 *   - AUDIO tasks (type M_AUDTASK) run the real microcode: the boot code that libultra loaded into IMEM and the microcode it loads are
 *     executed by the interpreter in src/rsp/rsp_cpu.c, on the memories and registers kept here, until the microcode executes BREAK.
 *   - every other task is done at a higher level by rsp_hle.c (graphics: display list interpreter and software RDP; JPEG). A JPEG task is
 *     performed at once. A graphics task runs on a worker thread (or in slices, see "graphics tasks" below) and may be yielded at a command
 *     boundary. When a task ends, this file does what the microcode would have done: it sets BROKE and HALT, sets the "task done" signal
 *     (SP_STATUS_TASKDONE) and raises the SP interrupt if libultra asked for one (SP_SET_INTR_BREAK, which osSpTaskLoad and
 *     osSpTaskStartGo always do). A graphics task that ended in a full sync also raises the DP interrupt.
 *
 * Simplifications, all visible here: an AUDIO or JPEG task finishes before the register write returns (the interrupts are still delivered
 * asynchronously by the scheduler layer) - except that an AUDIO-type task whose microcode waits for the CPU is suspended there and
 * continued by the CPU's next access to SP_STATUS (the Game Boy Tower's microcodes do this, see host_rsp_run); audio tasks are never
 * yielded (SP_STATUS_YIELDED is only set for graphics tasks); the DMA engine is always idle.
 *
 * GAME-side file.
 */
#include "PR/os_internal.h"
#include "PR/rcp.h"
#include "PR/mbi.h"
#include "platform.h"
#include "hw_internal.h"
#include "sp_shared.h"

/* sp_shared.h repeats the SP_STATUS bit numbers (it cannot include rcp.h: the interpreter that shares it is host-side). Check them. */
_Static_assert(SPS_CLR_HALT == SP_CLR_HALT && SPS_SET_HALT == SP_SET_HALT && SPS_CLR_BROKE == SP_CLR_BROKE && SPS_CLR_INTR == SP_CLR_INTR &&
               SPS_SET_INTR == SP_SET_INTR && SPS_CLR_SSTEP == SP_CLR_SSTEP && SPS_SET_SSTEP == SP_SET_SSTEP &&
               SPS_CLR_INTR_BREAK == SP_CLR_INTR_BREAK && SPS_SET_INTR_BREAK == SP_SET_INTR_BREAK, "SP_STATUS write bits");
_Static_assert(SPS_HALT == SP_STATUS_HALT && SPS_BROKE == SP_STATUS_BROKE && SPS_SSTEP == SP_STATUS_SSTEP && SPS_INTR_BREAK == SP_STATUS_INTR_BREAK &&
               SPS_SIG0 == SP_STATUS_SIG0 && (SPS_SIG0 << 7) == SP_STATUS_SIG7, "SP_STATUS read bits");
_Static_assert(SP_CLR_SIG0 == (1u << 9) && SP_SET_SIG0 == (1u << 10) && SP_CLR_SIG7 == (1u << 23) && SP_SET_SIG7 == (1u << 24), "signal bit pairs");

unsigned int HostRspRunTask(const unsigned char *dmem);          /* rsp_hle.c */
void HostRspGfxBegin(const unsigned char *dmem);                 /* rsp_hle.c */

static u8 sDmem[0x1000];            /* SP_DMEM_START.. : bytes exactly as DMA left them (big-endian words) */
static u8 sImem[0x1000];
static u32 sSpMemAddr, sSpDramAddr, sSpPc;
static u32 sSpStatus = SP_STATUS_HALT;      /* power-up: halted */
static u32 sSpSemaphore;
static u32 sDpcStart, sDpcEnd, sDpcStatus;

/* ---- DMA ----------------------------------------------------------------------------------------------------------------------- */

/* One SP DMA (see sp_shared.h, which the interpreter shares). */
static void sp_dma(int to_sp, u32 lenreg)
{
    sp_dma_copy(sDmem, sImem, &sSpMemAddr, &sSpDramAddr, lenreg, to_sp);
}

/* ---- running a task --------------------------------------------------------------------------------------------------------------- */

/* An audio-type task whose microcode is waiting for the CPU (see host_rsp_run in src/rsp/rsp_cpu.c): the interpreter is suspended inside it
 * and sSpStatus has HALT clear, as on the console where the RSP would be running (spinning). */
static int sSpSuspended;

/* What happens when the microcode has executed BREAK: the interpreter has set HALT|BROKE in the status. */
static void task_finished(void)
{
    sSpPc = 0x04001000;                                                      /* the boot code starts again at IMEM's start */
    if (sSpStatus & SP_STATUS_INTR_BREAK) HostHwRaiseSpIrq();
}

/* Copies the interpreter's registers back after it stopped (BREAK, or suspended waiting for the CPU) and passes on its interrupt requests. */
static void rsp_stopped(unsigned int steps)
{
    u32 req;
    sSpMemAddr = host_rsp_get(0);
    sSpDramAddr = host_rsp_get(1);
    sSpStatus = host_rsp_get(2);
    sSpSemaphore = host_rsp_get(3);
    host_trace("rsp microcode steps / SP_STATUS afterwards", steps, sSpStatus);
    req = host_rsp_get(5);                                   /* interrupt requests the microcode made through SP_STATUS */
    if (req & 1) HostHwAckSpIrq();
    if (req & 2) HostHwRaiseSpIrq();
    sSpSuspended = (int)host_rsp_get(6);
}

/* Runs the microcode that libultra loaded (boot code in IMEM, task description in DMEM) on the interpreter. It runs until BREAK or until
 * it waits for the CPU (then sSpSuspended is set and the CPU's next access to SP_STATUS continues it: resume_microcode). */
static unsigned sAudJobSeq, sAudDone;         /* audio microcode runs started / collected so far (the trace of hw_ai.c) */
static unsigned long long sAudStartNs, sAudEndNs;
static int sAudBusy;                /* an audio microcode run is on the second worker thread (host_rsp_run_async); until it is collected SP_STATUS says "running" */

/* The audio run on the worker has ended (or, with `wait`, is waited for): the result is taken over exactly as the synchronous run did it. Does nothing when none runs or it has not ended yet. */
static void audio_collect(int wait)
{
    unsigned int steps;
    if (!sAudBusy) return;
    if (wait) host_rsp_async_wait(); else if (host_rsp_async_running()) return;
    sAudBusy = 0;
    sAudDone++;
    sAudEndNs = host_now_ns();
    HostTraceEv("aud-run-collected", sAudDone);
    host_dbg[3]++;
    steps = host_rsp_async_result();
    host_job_taken(1);
    host_perf_add(PERF_AUDIO, host_rsp_async_ns());
    rsp_stopped(steps);
    if (!sSpSuspended) {
        host_trace("sp task end", 1, sSpStatus);
        task_finished();
    }
}

/* The game side is about to touch SP registers or memory that the running microcode owns: let it finish first (the console would not allow it either). */
static void audio_settle(void) { if (sAudBusy) audio_collect(1); }

static void run_microcode(void)
{
    host_rsp_start(sDmem, sImem, sSpPc, sSpMemAddr, sSpDramAddr, sSpStatus, sSpSemaphore, sDpcStatus);
    if (host_audio_async_enabled()) {
        /* The microcode runs on its own thread while the game goes on, as the RSP runs beside the CPU on the console; its end (SP_STATUS, interrupt) is reported by audio_collect
         * from the places that look: SP_STATUS reads, the idle loop and the preemption points. */
        sAudBusy = 1;
        sAudJobSeq++;
        sAudStartNs = host_now_ns();
        HostTraceEv("aud-run-start", sAudJobSeq);
        host_rsp_run_async();
        return;
    }
    {
        unsigned long long t0 = host_now_ns();
        rsp_stopped(host_rsp_run());
        host_perf_add(PERF_AUDIO, host_now_ns() - t0);
    }
}

/* Called when the CPU reads or writes SP_STATUS while a task is suspended: the answer the microcode waits for may have changed. */
static void resume_microcode(void)
{
    if (!sSpSuspended) return;
    host_rsp_resume(sSpStatus, sSpSemaphore, sDpcStatus);
    rsp_stopped(host_rsp_run());
    if (!sSpSuspended) {
        host_trace("sp task end (after waiting for the CPU)", 0, sSpStatus);
        task_finished();
    }
}

/* ---- graphics tasks ---------------------------------------------------------------------------------------------------------------
 * On the console the RSP runs a graphics task while the CPU carries on, and the game's scheduler can stop it at a command boundary
 * (SP_SET_YIELD) so that an audio task can run first. Here the task is started by gfx_start and then followed by gfx_pump.
 * Default (host_gfx_async_enabled): the task runs on a worker thread (src/render/gfx_host.c, host_job_start), so the game carries on
 * meanwhile as it does on the console. gfx_pump(1) only looks whether the worker has ended (HostIdle, HostPreemptPoll and SP_STATUS reads
 * do this); gfx_pump(0) waits for it (before a field is shown from the buffer the task draws into - HostHwGfxCompleteFor - and before pool
 * memory is given back - __osHostGfxIdleBeforeReuse). When the game asks for a yield (SP_SET_YIELD) it is told "yielded" at once and the worker
 * is NOT stopped (gfx_virtual_yield: on the console only the RSP's list reading pauses, the RDP goes on drawing).
 * KYUJO_GFX_THREAD=0: no thread; the task is advanced in slices of `budget` commands from HostIdle and HostPreemptPoll
 * (os_sched.c), before a field is shown, and at once when the game asks for a yield.
 * In that single-thread mode a yield stops the task at a command boundary, so an audio task that waits for it starts after one command
 * instead of after the whole list. Completion and yield set SP_STATUS the way the microcode does, and libultra reads them back (osSpTaskYielded). */
static int sGfxBusy;                /* a graphics task has started and has neither ended nor yielded */
static int sGfxYielded;             /* the task yielded: the next graphics start resumes it instead of starting a new one */
static volatile int sGfxYieldReq;   /* the game asked for a yield (SP_SET_YIELD) and the task has not reached a command boundary yet */
static unsigned int sGfxSyncs;      /* RDP full syncs run so far in this task: the DP interrupt is owed when the task ends with any */
static int sGfxVirtYield;           /* thread mode: the game was told the task yielded, but the worker thread keeps drawing (see gfx_virtual_yield) */
static int sGfxHeldDone;            /* ... and the worker ended before the game restarted the task: the end is reported at the restart */

/* The task has ended and the game is told: SP_STATUS says "done" and the interrupts it asked for come. */
static void gfx_report_end(void)
{
    sGfxYielded = 0;
    sSpStatus |= SP_STATUS_HALT | SP_STATUS_BROKE | SP_STATUS_SIG2;     /* SIG2 = TASKDONE */
    host_trace("gfx task end", sGfxSyncs, sSpStatus);
    task_finished();
    if (sGfxSyncs) HostHwRaiseDpIrq();
    sGfxSyncs = 0;
}

/* Thread mode only: the game asks the task to yield (SP_SET_YIELD) so that an audio task can use the RSP. The worker thread is NOT stopped.
 * On the console a yield stops only the RSP side (the display list reading); the RDP goes on drawing what it was already given, in parallel
 * with the audio task. The software renderer is one piece, and one command (a big triangle or texture rectangle) takes 10 to 60 ms of
 * drawing, so stopping it "at the next command" held the audio task up by that long, and the game then built too little audio (measured:
 * SP_SET_YIELD at +0 ms, yielded at +8..12 ms; 75 % of the audio tasks on time in the menus). Instead the game is told at once that the task yielded
 * (SP_STATUS and interrupt as in a real yield) while the worker keeps drawing, and the audio task runs on this thread meanwhile (the
 * audio interpreter shares nothing with the renderer). When the game restarts the task, the worker is still drawing (nothing to do) or
 * has ended (the end is reported then, gfx_start). */
static void gfx_virtual_yield(void)
{
    sGfxVirtYield = 1;
    sGfxYielded = 1;
    sSpStatus |= SP_STATUS_HALT | SP_STATUS_BROKE | SP_STATUS_SIG1;
    host_trace("gfx task yielded (virtually: the worker goes on drawing)", 0, sSpStatus);
    task_finished();
}

/* Advances the busy graphics task (single-thread mode: by at most `budget` commands; thread mode: collects it if it ended, or for
 * budget 0 waits for it). Budget 0 means: to its end or its yield. */
static void gfx_pump(unsigned int budget)
{
    unsigned int syncs = 0;
    int r;

    if (!sGfxBusy) return;
    if (host_gfx_async_enabled()) {
        /* The task runs on its own thread: a slice request only looks whether it has ended; budget 0 (finish it / stop it) waits for it. */
        r = host_gfx_task_collect(budget == 0, &syncs);
        if (r == HOST_GFX_PAUSED) return;
    } else {
        r = host_gfx_task_pump(budget, &sGfxYieldReq, &syncs);
    }
    sGfxSyncs += syncs;
    if (r == HOST_GFX_PAUSED) return;                   /* still running: the next slice continues it */
    sGfxBusy = 0;
    sGfxYieldReq = 0;
    if (r == HOST_GFX_YIELDED) {
        sGfxYielded = 1;
        /* SP_STATUS_YIELD (SIG0, the request) stays set: osSpTaskYielded reads it together with SP_STATUS_YIELDED (SIG1) */
        sSpStatus |= SP_STATUS_HALT | SP_STATUS_BROKE | SP_STATUS_SIG1;
        host_trace("gfx task yielded", 0, sSpStatus);
        task_finished();                                 /* a yield raises the SP interrupt like a BREAK, when the task asked for it */
    } else if (sGfxVirtYield) {
        sGfxHeldDone = 1;                                /* the game believes the task is yielded: it hears of the end when it restarts it */
        host_trace("gfx task ended while yielded (reported at its restart)", sGfxSyncs, sSpStatus);
    } else {
        gfx_report_end();
    }
}

/* Starts the graphics task that the task block in DMEM describes, or continues a yielded one (the game restarts it with OS_TASK_YIELDED
 * after the audio task). The task does not run to its end here: it is on the worker thread, or gfx_pump runs it in slices. */
static void gfx_start(void)
{
    if (sGfxVirtYield) {                                /* restart after a virtual yield: the same task, which has gone on drawing */
        sGfxVirtYield = 0;
        sGfxYielded = 0;
        sGfxYieldReq = 0;
        host_trace(sGfxHeldDone ? "gfx task resumed after a virtual yield (had ended)" : "gfx task resumed after a virtual yield", 0, sSpStatus);
        if (sGfxHeldDone) { sGfxHeldDone = 0; gfx_report_end(); }
        return;                                         /* otherwise the worker is still on it; gfx_pump reports the end */
    }
    if (sGfxBusy) gfx_pump(0);                          /* not expected: the game starts the next task only after the last ended or yielded */
    if (!sGfxYielded) HostRspGfxBegin(sDmem);           /* a yielded task keeps its state (the microcode would reload it from the yield buffer) */
    host_trace(sGfxYielded ? "gfx task resumed after a yield" : "gfx task started", 0, sSpStatus);
    sGfxBusy = 1;
    sGfxYieldReq = 0;
    if (host_gfx_async_enabled()) host_gfx_task_run_async(&sGfxYieldReq);        /* runs on the worker thread from here on */
}

void HostHwGfxPump(unsigned int budget) { gfx_pump(budget); audio_collect(0); }
void HostHwRspPump(void) { audio_collect(0); }
int HostHwRspBusy(void) { return sAudBusy; }
void HostHwRspSettle(void) { audio_settle(); }
unsigned HostHwRspJobSeq(void) { return sAudJobSeq; }
unsigned HostHwRspJobsDone(void) { return sAudDone; }
unsigned long long HostHwRspSinceStartNs(void) { unsigned long long n = host_now_ns(); return n > sAudStartNs ? n - sAudStartNs : 0; }
unsigned long long HostHwRspSinceEndNs(void) { unsigned long long n = host_now_ns(); return n > sAudEndNs ? n - sAudEndNs : 0; }
int HostHwGfxBusy(void) { return sGfxBusy; }
void HostHwGfxComplete(void) { gfx_pump(0); }

/* Before a picture is shown (VI commit with frame buffer `origin`): the picture must not be half drawn, so a graphics task that is drawing
 * into that buffer is finished first. A task that draws into another buffer (the usual double or triple buffering: the next frame is drawn
 * while the finished one is shown) is NOT waited for - waiting for it would stop the whole game, including the audio thread, for up to a
 * full drawing task every field (measured: the game then built only 2/3 of the audio it should, which sounded slow). It is only looked at,
 * so that its end is reported when it has ended. KYUJO_GFX_THREAD=0 keeps the old behaviour (always finish it). */
void HostHwGfxCompleteFor(unsigned int origin)
{
    unsigned long long t0;
    if (!sGfxBusy) return;
    if (host_gfx_async_enabled() && !host_gfx_task_may_draw_to(origin)) { gfx_pump(1); return; }
    t0 = host_now_ns();
    gfx_pump(0);
    host_perf_add(PERF_GFXWAIT, host_now_ns() - t0);
}

/* Called by the patched main pool (port patch 0025) before pool memory is freed or popped, i.e. before it can be handed out again: the
 * graphics task that is still running may read that memory (display lists, textures, vertices of the last frame), so it is finished first.
 * Costs nothing when no task runs. The time spent waiting is counted as gfx-wait in the 5 s speed line. */
void __osHostGfxIdleBeforeReuse(void)
{
    unsigned long long t0;
    if (!sGfxBusy) return;
    t0 = host_now_ns();
    host_trace("pool reuse: waiting for the graphics task", 0, 0);
    gfx_pump(0);
    host_perf_add(PERF_GFXWAIT, host_now_ns() - t0);
}

/* Port patch 0035 (src/memory_main.c): the pool handed out [addr, addr+size). The renderer drops what it still holds over that range (rdp.h). */
extern void rdp_memory_reused(unsigned int addr, unsigned int len);
void __osHostPoolMemoryReused(void *addr, unsigned int size)
{
    rdp_memory_reused((unsigned int)(unsigned long)addr, size);
}

static void sp_start(void)
{
    unsigned int owed = 1;
    u32 type = ((u32)sDmem[0xFC0] << 24) | ((u32)sDmem[0xFC1] << 16) | ((u32)sDmem[0xFC2] << 8) | sDmem[0xFC3];     /* OSTask.type */

    sSpStatus &= ~(SP_STATUS_HALT);
    host_trace("sp task start", type, 0);
    host_dbg[type == M_AUDTASK ? 2 : 9]++;
    if (type == M_AUDTASK) {
        /* The real microcode runs. It halts with BREAK (the interpreter sets HALT|BROKE) and sets TASKDONE (SIG2) itself: seen in the
         * running game, SP_STATUS after an audio task is 0x243 = HALT|BROKE|INTR_BREAK|SIG2 (the INTR_BREAK bit was set by libultra). */
        run_microcode();
        if (sAudBusy) return;                                                /* on the worker: audio_collect reports the end */
        if (sSpSuspended) return;                                            /* it waits for the CPU: the rest happens in resume_microcode */
        host_trace("sp task end", owed, sSpStatus);
        task_finished();
        return;
    }
    if (type == M_GFXTASK) {
        gfx_start();                                                        /* runs on the worker (or in slices) from now on; its end is in gfx_pump */
        return;
    }
    /* JPEG tasks are done at a higher level: their end state is what the microcode would have left. */
    owed = HostRspRunTask(sDmem);
    host_trace("sp task end", owed, sSpStatus);
    sSpStatus |= SP_STATUS_HALT | SP_STATUS_BROKE | SP_STATUS_SIG2;         /* SIG2 = SP_STATUS_TASKDONE (rcp.h) */
    task_finished();
    if (owed & 2) HostHwRaiseDpIrq();
}

/* ---- registers ------------------------------------------------------------------------------------------------------------------ */

int HostSpOwns(u32 a)
{
    return (a >= SP_DMEM_START && a <= SP_IMEM_END) || (a >= SP_BASE_REG && a <= SP_SEMAPHORE_REG) || a == SP_PC_REG ||
           (a >= DPC_BASE_REG && a <= DPC_TMEM_REG);
}

u32 HostSpRead32(u32 a)
{
    if (a >= SP_DMEM_START && a <= SP_IMEM_END) {
        const u8 *m = (a >= SP_IMEM_START) ? sImem : sDmem;
        u32 o = a & 0xFFC;
        audio_settle();                                     /* the microcode's memories: after its end */
        return ((u32)m[o] << 24) | ((u32)m[o + 1] << 16) | ((u32)m[o + 2] << 8) | m[o + 3];
    }
    switch (a) {
    case SP_MEM_ADDR_REG:  audio_settle(); return sSpMemAddr;
    case SP_DRAM_ADDR_REG: audio_settle(); return sSpDramAddr;
    case SP_RD_LEN_REG:
    case SP_WR_LEN_REG:    return 0;                       /* a DMA completes at once: nothing left to transfer */
    case SP_STATUS_REG:
        audio_collect(0);                                   /* an audio run on the worker may have ended: report it */
        resume_microcode();                                 /* the microcode (if suspended) first sees what the CPU changed, and may run on */
        if (sGfxBusy && host_gfx_async_enabled()) gfx_pump(1);      /* a graphics task on its worker thread may have ended: show it */
        host_dbg[0] = sSpStatus; host_dbg[1] = (unsigned)sAudBusy | (unsigned)sSpSuspended << 1 | (unsigned)sGfxBusy << 2 | (unsigned)sGfxVirtYield << 3;
        return sSpStatus;
    case SP_DMA_FULL_REG:
    case SP_DMA_BUSY_REG:  return 0;
    case SP_SEMAPHORE_REG: { u32 v; audio_settle(); v = sSpSemaphore; sSpSemaphore = 1; return v; }    /* read-and-lock (n64brew) */
    case SP_PC_REG:        audio_settle(); return sSpPc;
    case DPC_START_REG:    return sDpcStart;
    case DPC_END_REG:      return sDpcEnd;
    case DPC_CURRENT_REG:  return sDpcEnd;                  /* the RDP has consumed everything it was given */
    case DPC_STATUS_REG:   return sDpcStatus;
    case DPC_CLOCK_REG:
    case DPC_BUFBUSY_REG:
    case DPC_PIPEBUSY_REG:
    case DPC_TMEM_REG:     return 0;
    default:               host_missing_register("SP/DP register read", a);
    }
}

void HostSpWrite32(u32 a, u32 v)
{
    audio_settle();                                         /* a write changes what the running microcode owns (SP_STATUS included: its end would overwrite the bits) */
    if (a >= SP_DMEM_START && a <= SP_IMEM_END) {
        u8 *m = (a >= SP_IMEM_START) ? sImem : sDmem;
        u32 o = a & 0xFFC;
        m[o] = (u8)(v >> 24); m[o + 1] = (u8)(v >> 16); m[o + 2] = (u8)(v >> 8); m[o + 3] = (u8)v;
        return;
    }
    switch (a) {
    case SP_MEM_ADDR_REG:  sSpMemAddr = v; break;
    case SP_DRAM_ADDR_REG: sSpDramAddr = v; break;
    case SP_RD_LEN_REG:    sp_dma(1, v); break;             /* DRAM -> SP */
    case SP_WR_LEN_REG:    sp_dma(0, v); break;             /* SP -> DRAM */
    case SP_SEMAPHORE_REG: sSpSemaphore = 0; break;
    case SP_PC_REG:        sSpPc = v & 0xFFC; break;
    case SP_STATUS_REG: {
        u32 req;
        sSpStatus = sp_status_write(sSpStatus, v, &req);
        if (req & 1) HostHwAckSpIrq();
        if (req & 2) HostHwRaiseSpIrq();
        if ((v & SP_SET_SIG0) && sGfxBusy && !sGfxVirtYield) {   /* SP_SET_YIELD (SIG0) while a graphics task runs */
            host_trace("SP_SET_YIELD while the graphics task runs", 0, sSpStatus);
            if (host_gfx_async_enabled()) {
                gfx_virtual_yield();                    /* the worker goes on; the game hears "yielded" now */
            } else {
                sGfxYieldReq = 1;                       /* stop it at its next command */
                gfx_pump(0);
            }
        }
        if (sSpSuspended) {
            /* A task is running and waiting for the CPU: this write is probably what it waits for. Halting it from outside is not modelled. */
            if (v & SP_SET_HALT) host_missing_register("halting an audio task that waits for the CPU:", v);
            resume_microcode();
        } else if (v & SP_CLR_HALT) {
            if (sSpStatus & SP_STATUS_SSTEP) host_missing_register("RSP single-step start:", v);
            sp_start();
        }
        break;
    }
    case DPC_START_REG:    sDpcStart = v; break;
    case DPC_END_REG:      sDpcEnd = v; break;              /* an RDP command list given directly: not used with F3DEX2 tasks */
    case DPC_STATUS_REG:
        if (v & DPC_CLR_XBUS_DMEM_DMA) sDpcStatus &= ~DPC_STATUS_XBUS_DMEM_DMA;
        if (v & DPC_SET_XBUS_DMEM_DMA) sDpcStatus |= DPC_STATUS_XBUS_DMEM_DMA;
        if (v & DPC_CLR_FREEZE) sDpcStatus &= ~DPC_STATUS_FREEZE;
        if (v & DPC_SET_FREEZE) sDpcStatus |= DPC_STATUS_FREEZE;
        if (v & DPC_CLR_FLUSH) sDpcStatus &= ~DPC_STATUS_FLUSH;
        if (v & DPC_SET_FLUSH) sDpcStatus |= DPC_STATUS_FLUSH;
        break;                                              /* the counter-clear bits have nothing to clear yet */
    default:               host_missing_register("SP/DP register write", a);
    }
}
