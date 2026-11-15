/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_mips.c - an interpreter for MIPS III integer machine code, used for the game's hand-written assembly.
 *
 * Why this exists. The GB Tower (Game Boy) emulator in the game is a code overlay ("fragment 1") whose CPU core is 4192
 * hand-written MIPS instructions with no C source (rom.yaml: `hasm fragments/gb_tower_emulator/gb_tower_emulator_code_asm`). It
 * cannot be compiled for x86. host_mips_stub.c runs the few-instruction entry stubs of raw-binary fragments; this file is the
 * general version: every integer instruction that code of that kind can contain, with calls, a stack, multiply and divide.
 * The code is read from the game's memory pool (where the game's loader has copied and relocated it), never from a file of this
 * repository: the instructions are the player's, from their ROM, and nothing derived from it is stored here.
 *
 * What is interpreted. Exactly the integer instructions below (MIPS III, 64-bit registers because the emulator core moves
 * doublewords with ld/sd). Anything else - floating point, system registers, unaligned loads, "likely" branches, traps,
 * 64-bit shifts - stops the program with the address and the instruction word, so unsupported code can never run silently wrong.
 *
 * References for the semantics: "MIPS IV Instruction Set" (Price, 1995) and "See MIPS Run" (Sweetman), same as host_mips_stub.c;
 * and the instruction list actually present in the emulator core, measured from the ROM with llvm-mc (mnemonic: count):
 *   andi 598, j 524, addiu 413, sw 226, or 193, srl 173, ori 164, lw 164, sll 161, lbu 161, move 149, sb 144, addu 134, nop 117,
 *   jal 111, beqz 89, lui 87, sh 70, jr 64, bnez 61, lhu 40, sd 37, and 36, beq 34, ld 32, bne 28, xori 27, sltiu 23, subu 19,
 *   xor 16, sltu 15, bgtz 15, jalr 12, bltz 12, sllv 10, lb 8, bgezal 6, bgez 6, srlv 4, negu 3, lh 3, sra 1, mflo 1, divu 1.
 * The interpreter is tested against clang-compiled MIPS code (tests/native/mips) and by running the real core (see docs).
 *
 * Memory. Game memory is big-endian and a native pointer equals the console's physical address (docs/architecture.md), so a
 * register holding an address is used as a native pointer after a range check (addresses 0x1000 .. 0x00FFFFFF only: the whole
 * program lives below 16 MiB, see the address-range constraint in the docs). Code is fetched from the memory pool only.
 *
 * Calls. `jal`, `jalr` and `bgezal` to code that has a native version (a fragment's C functions, found through host_fragment_native)
 * call that native function with a0..a3 and continue after the call with its result in v0. The one call into the main program that
 * the emulator core makes (rmonPrintf, which in this ROM only spills its arguments and returns) is answered by doing nothing.
 *
 * HOST-side file (plain C).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "mips_shared.h"
#ifdef __wasm__
#include "host_ui.h"                                              /* host_settings(): gb_fast_cpu */
#endif

extern unsigned char gPool[];                                   /* the game's memory pool (src/heap.c) */
extern const unsigned int gHostPoolBytes;

typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;

#define RETURN_SENTINEL 0xFFFFFFF0u                             /* ra at entry: `jr ra` to it ends the run */
#define STACK_BYTES 32768u
#define MAX_THREADS 4u                                          /* game threads that may be inside interpreted code at the same time */

/* Preemption: the interpreted code's loops must let a pending device event take effect, like the loops of the compiled game code do
 * (tools/native/be_memory.py rule 5). Same two names as in the generated code; see os_sched.c. */
extern volatile unsigned int host_preempt_flag;
volatile unsigned int host_dbg[112];
__attribute__((export_name("core_dbg_addr"))) unsigned core_dbg_addr(void) { return (unsigned)(unsigned long)host_dbg; }
void HostPreemptPoll(void);
/* The RCP registers: libultra reaches them through these (port patch 0001); the interpreted code reads and writes them directly. */
unsigned int HostHwRead32(unsigned int addr);
void HostHwWrite32(unsigned int addr, unsigned int data);

/* One game thread's use of the interpreter. A thread can be switched out while it is inside interpreted code (at a preemption poll, or inside
 * a native function that blocks), and then another thread may enter the interpreter: each needs its own stack and its own record of where the
 * stack stands. The stacks are static, so they lie inside the program image (below 16 MiB like all game memory). */
typedef struct ThreadCtx {
    const void *id;                                             /* HostCurrentThreadId() of the owner */
    unsigned depth;                                             /* host_mips_call nesting of the owner (0 = free) */
    unsigned char *stack;
    MipsShared sh;                                              /* nested_sp (sp while the owner is inside a native call made from interpreted code, 0 = not inside) and the counters of the
                                                                 * current outermost call (PERF_MIPSYIELD...; instr counts branches); shared with the native interpreter (shell/core/mips_native.c) */
} ThreadCtx;
/* The STACKS are static in every build (128 KiB): the interpreted code's stack pointer is the address of one of them, and the interpreter only accepts
 * load/store addresses inside game memory (0x1000..0xFFFFFF, below 16 MiB, like all game memory), so they must lie inside the program image.
 * (Allocating them at run time put them at a heap address above 16 MiB: GB Tower stopped with "a load or store outside game memory", 2026-11-12.)
 * The big tables below (about 2.5 MiB) are host data that the game never sees: in the native programs, whose whole image must stay below 16 MiB
 * (tools/native/imagelimit.py), they are allocated on first use; in the WebAssembly build they stay plain static arrays (the shell lays out that memory). */
static unsigned char sStacks[MAX_THREADS][STACK_BYTES] __attribute__((aligned(16)));
#if defined(__wasm__)
#define MIPS_STATIC_TABLES 1
#endif
static ThreadCtx sCtx[MAX_THREADS];

static void mips_mem(void);

/* The calling thread's context: the one it already owns, else a free one. Only the thread holding the baton runs, so no lock is needed. */
static ThreadCtx *ctx_enter(void)
{
    const void *id = HostCurrentThreadId();
    unsigned i;
    mips_mem();
    for (i = 0; i < MAX_THREADS; i++) {
        if (sCtx[i].depth && sCtx[i].id == id) { sCtx[i].depth++; return &sCtx[i]; }
    }
    for (i = 0; i < MAX_THREADS; i++) {
        if (sCtx[i].depth == 0) {
            sCtx[i].id = id; sCtx[i].depth = 1; sCtx[i].sh.nested_sp = 0; sCtx[i].stack = sStacks[i];
            return &sCtx[i];
        }
    }
    host_fatal("more game threads are inside interpreted machine code at once than the interpreter has stacks for (host_mips.c MAX_THREADS)");
    return NULL;
}

typedef struct Cpu {
    u64 r[33];                                                  /* r[32] is a sink: the fast decoder redirects writes to $zero there */
    u64 hi, lo;
    u32 pc;                                                     /* address of the instruction being executed */
    unsigned long long steps;                                   /* instructions executed (for the optional profile) */
    ThreadCtx *ctx;                                             /* the owning thread's stack record */
} Cpu;

static void bad(const Cpu *c, const char *why, u32 pc, u32 inst, u32 extra)
{
    (void)c;
    fprintf(stderr, "kyujo: machine code at 0x%08X [console address 0x%08X] (word 0x%08X, value 0x%08X): %s\n", pc,
            host_fragment_link_addr(pc), inst, extra, why);
    host_fatal("the MIPS interpreter (host_mips.c) met something it does not support");
}

/* ---- memory -------------------------------------------------------------------------------------------------------------- */

/* A 32-bit load or store to the RCP's registers (0xA4xxxxxx, the uncached view of physical 0x04xxxxxx: SP, DP, MI, VI, AI, PI, RI, SI).
 * They go to the hardware model (hw_regs.c), exactly as libultra's IO_READ / IO_WRITE do. Other widths are refused. */
static int is_rcp_register(u32 a) { return (a >> 24) == 0xA4u; }

static unsigned char *mem(const Cpu *c, u32 a, u32 n, u32 pc, u32 inst)
{
    if (a < 0x1000u || a > 0x01000000u - n) bad(c, "a load or store outside game memory (0x1000..0x00FFFFFF)", pc, inst, a);
    if (a & (n - 1u)) bad(c, "an unaligned load or store", pc, inst, a);
    return (unsigned char *)(unsigned long)a;
}

static u32 fetch(const Cpu *c, u32 pc)
{
    const unsigned char *b = (const unsigned char *)(unsigned long)pc;
    unsigned long base = (unsigned long)gPool;
    if ((pc & 3u) || (unsigned long)pc < base || (unsigned long)pc - base > gHostPoolBytes - 4u) {
        bad(c, "the program counter left the memory pool", pc, 0, pc);
    }
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static u64 sext32(u32 v) { return (u64)(s64)(s32)v; }
static u32 sext16(u32 v) { return (u32)(s32)(short)(v & 0xFFFFu); }
static u64 sext16_64(u32 v) { return (u64)(s64)(short)(v & 0xFFFFu); }

/* Development aid: KYUJO_MIPS_WATCH=<hex offset> prints every store of the interpreted code that covers that offset from gHostMipsWatchBase
 * (the Game Boy emulator's state, set by host_gbemu.c): the program counter, the instruction, the stored value. Used to find which instruction
 * of the Game Boy core sets a flag of the emulator state. */
unsigned long gHostMipsWatchBase;
static unsigned long sWatch;
static int sWatchRead;
static void watch_store(u32 pc, u32 inst, u32 a, int size, u64 value)
{
    if (!sWatchRead) {
        const char *e = getenv("KYUJO_MIPS_WATCH");
        sWatchRead = 1;
        sWatch = e ? strtoul(e, NULL, 16) : 0;
    }
    if (sWatch && gHostMipsWatchBase && (unsigned long)a <= gHostMipsWatchBase + sWatch && gHostMipsWatchBase + sWatch < (unsigned long)a + (unsigned)size)
        fprintf(stderr, "kyujo: mips watch: pc 0x%08X (console address 0x%08X) inst 0x%08X stores %d byte(s) at 0x%08X, value 0x%llX\n", pc, host_fragment_link_addr(pc), inst, size, a, (unsigned long long)value);
}

/* The reference interpreter below (step, ref_iter) is the original, slow and simple one. The fast interpreter (DInst, fast_iter) must do exactly
 * what it does; KYUJO_MIPS_CHECK=1 runs both in lockstep (one instruction, or one branch with its delay slot, at a time) and stops at the
 * first difference. In that mode the reference skips accesses to RCP registers (they have side effects) and native calls (the fast side made them). */
static int sRefMode;                    /* the reference is running as the checker of the fast interpreter */
static int sRefSkipped;                 /* ... and it skipped an RCP access in this iteration */
static int sRefCallNative;              /* ... and the fast side made a native call in this iteration */

/* ---- one non-branch instruction ----------------------------------------------------------------------------------------- */
static void step(Cpu *c, u32 pc, u32 inst)
{
    u32 op = inst >> 26, rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, sh = (inst >> 6) & 31;
    u32 imm = inst & 0xFFFFu, funct = inst & 63u;
    u64 *r = c->r;
    u64 v;
    unsigned char *p;
    u32 a;

    if (op == 0) {                                              /* SPECIAL */
        switch (funct) {
        case 0x00: v = sext32((u32)r[rt] << sh); break;                              /* sll (nop = sll 0,0,0) */
        case 0x02: v = sext32((u32)r[rt] >> sh); break;                              /* srl */
        case 0x03: v = sext32((u32)((s32)(u32)r[rt] >> sh)); break;                  /* sra */
        case 0x04: v = sext32((u32)r[rt] << (r[rs] & 31)); break;                    /* sllv */
        case 0x06: v = sext32((u32)r[rt] >> (r[rs] & 31)); break;                    /* srlv */
        case 0x07: v = sext32((u32)((s32)(u32)r[rt] >> (r[rs] & 31))); break;        /* srav */
        case 0x10: v = c->hi; break;                                                 /* mfhi */
        case 0x12: v = c->lo; break;                                                 /* mflo */
        case 0x11: c->hi = r[rs]; return;                                            /* mthi */
        case 0x13: c->lo = r[rs]; return;                                            /* mtlo */
        case 0x18: {                                                                 /* mult: 32 x 32 signed -> hi:lo */
            s64 m = (s64)(s32)(u32)r[rs] * (s64)(s32)(u32)r[rt];
            c->lo = sext32((u32)m); c->hi = sext32((u32)((u64)m >> 32));
            return;
        }
        case 0x19: {                                                                 /* multu */
            u64 m = (u64)(u32)r[rs] * (u64)(u32)r[rt];
            c->lo = sext32((u32)m); c->hi = sext32((u32)(m >> 32));
            return;
        }
        case 0x1A: {                                                                 /* div (a divide by zero is "unpredictable": refused) */
            s32 x = (s32)(u32)r[rs], y = (s32)(u32)r[rt];
            if (y == 0 || (x == (s32)0x80000000 && y == -1)) bad(c, "division by zero or overflow", pc, inst, (u32)x);
            c->lo = sext32((u32)(x / y)); c->hi = sext32((u32)(x % y));
            return;
        }
        case 0x1B: {                                                                 /* divu */
            u32 x = (u32)r[rs], y = (u32)r[rt];
            if (y == 0) bad(c, "division by zero", pc, inst, x);
            c->lo = sext32(x / y); c->hi = sext32(x % y);
            return;
        }
        case 0x21: v = sext32((u32)r[rs] + (u32)r[rt]); break;                       /* addu (also `move`) */
        case 0x23: v = sext32((u32)r[rs] - (u32)r[rt]); break;                       /* subu (also `negu`) */
        case 0x24: v = r[rs] & r[rt]; break;                                         /* and */
        case 0x25: v = r[rs] | r[rt]; break;                                         /* or (also `move`) */
        case 0x26: v = r[rs] ^ r[rt]; break;                                         /* xor */
        case 0x27: v = ~(r[rs] | r[rt]); break;                                      /* nor */
        case 0x2A: v = (s64)r[rs] < (s64)r[rt]; break;                               /* slt */
        case 0x2B: v = r[rs] < r[rt]; break;                                         /* sltu */
        default: bad(c, "unsupported register instruction", pc, inst, funct); return;
        }
        if (rd != 0) r[rd] = v;
        return;
    }
    switch (op) {
    case 0x09: v = sext32((u32)r[rs] + sext16(imm)); break;                          /* addiu */
    case 0x0A: v = (s64)r[rs] < (s64)sext16_64(imm); break;                          /* slti */
    case 0x0B: v = r[rs] < sext16_64(imm); break;                                    /* sltiu: immediate sign-extended, compared unsigned */
    case 0x0C: v = r[rs] & imm; break;                                               /* andi */
    case 0x0D: v = r[rs] | imm; break;                                               /* ori */
    case 0x0E: v = r[rs] ^ imm; break;                                               /* xori */
    case 0x0F: v = sext32(imm << 16); break;                                         /* lui */
    case 0x20: case 0x24:                                                            /* lb, lbu */
        a = (u32)r[rs] + sext16(imm); p = mem(c, a, 1, pc, inst);
        v = (op == 0x20) ? (u64)(s64)(signed char)p[0] : (u64)p[0]; break;
    case 0x21: case 0x25:                                                            /* lh, lhu */
        a = (u32)r[rs] + sext16(imm); p = mem(c, a, 2, pc, inst);
        v = (u32)((u32)p[0] << 8 | p[1]); if (op == 0x21) v = sext16_64((u32)v); break;
    case 0x23: case 0x27:                                                            /* lw, lwu */
        a = (u32)r[rs] + sext16(imm);
        if (is_rcp_register(a)) {
            if (a & 3u) bad(c, "an unaligned load from an RCP register", pc, inst, a);
            if (sRefMode) { sRefSkipped = 1; return; }
            v = HostHwRead32(a); if (op == 0x23) v = sext32((u32)v);
            break;
        }
        p = mem(c, a, 4, pc, inst);
        v = (u32)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]); if (op == 0x23) v = sext32((u32)v); break;
    case 0x37:                                                                       /* ld */
        a = (u32)r[rs] + sext16(imm); p = mem(c, a, 8, pc, inst);
        v = (u64)p[0] << 56 | (u64)p[1] << 48 | (u64)p[2] << 40 | (u64)p[3] << 32 | (u64)p[4] << 24 | (u64)p[5] << 16 | (u64)p[6] << 8 | p[7];
        break;
    case 0x28:                                                                       /* sb */
        a = (u32)r[rs] + sext16(imm); p = mem(c, a, 1, pc, inst); watch_store(pc, inst, a, 1, r[rt] & 0xFF); p[0] = (unsigned char)r[rt]; return;
    case 0x29:                                                                       /* sh */
        a = (u32)r[rs] + sext16(imm); p = mem(c, a, 2, pc, inst);
        watch_store(pc, inst, a, 2, r[rt] & 0xFFFF);
        p[0] = (unsigned char)(r[rt] >> 8); p[1] = (unsigned char)r[rt]; return;
    case 0x2B:                                                                       /* sw */
        a = (u32)r[rs] + sext16(imm);
        if (is_rcp_register(a)) {
            if (a & 3u) bad(c, "an unaligned store to an RCP register", pc, inst, a);
            if (sRefMode) { sRefSkipped = 1; return; }
            HostHwWrite32(a, (u32)r[rt]);
            return;
        }
        p = mem(c, a, 4, pc, inst);
        watch_store(pc, inst, a, 4, r[rt] & 0xFFFFFFFFu);
        p[0] = (unsigned char)(r[rt] >> 24); p[1] = (unsigned char)(r[rt] >> 16); p[2] = (unsigned char)(r[rt] >> 8); p[3] = (unsigned char)r[rt];
        return;
    case 0x3F:                                                                       /* sd */
        a = (u32)r[rs] + sext16(imm); p = mem(c, a, 8, pc, inst);
        p[0] = (unsigned char)(r[rt] >> 56); p[1] = (unsigned char)(r[rt] >> 48); p[2] = (unsigned char)(r[rt] >> 40); p[3] = (unsigned char)(r[rt] >> 32);
        p[4] = (unsigned char)(r[rt] >> 24); p[5] = (unsigned char)(r[rt] >> 16); p[6] = (unsigned char)(r[rt] >> 8); p[7] = (unsigned char)r[rt];
        return;
    default:
        bad(c, "unsupported instruction", pc, inst, op);
        return;
    }
    if (rt != 0) r[rt] = v;
}

/* ---- calls out of the interpreter ---------------------------------------------------------------------------------------- */

/* main-program functions that interpreted code may call, by the console's physical address (the `jal` field of code that the loader
 * does not relocate). Only functions whose behaviour is known are listed. */
#define MAIN_RMONPRINTF 0x0005E22Cu      /* 0x8005E22C: addiu sp,-8; sw a0..a3 to the caller's save area; jr ra; addiu sp,8 (read from the ROM): a no-op */

/* Runs `entry` with the C calling convention (host_mips_call, below). */
static u32 run(Cpu *c, u32 entry);

/* Calls `target` as a function if it is not interpreted code. Returns 1 when it did (result in v0, execution continues after the call),
 * 0 when `target` is code to interpret. */
static int try_native_call(Cpu *c, u32 target, u32 pc, u32 inst)
{
    unsigned long base = (unsigned long)gPool;
    void *fn;
    u32 a0 = (u32)c->r[4], a1 = (u32)c->r[5], a2 = (u32)c->r[6], a3 = (u32)c->r[7], res;

    if ((unsigned long)target >= base && (unsigned long)target - base < gHostPoolBytes) {
        fn = host_fragment_native(target);
        if (!fn) return 0;                                      /* machine code in the pool with no native version: interpret it */
    } else if (target == MAIN_RMONPRINTF) {
        return 1;                                               /* a no-op (see above); v0 is left alone */
    } else {
        bad(c, "a call outside the memory pool to a function that is not known", pc, inst, target);
        return 0;
    }
    c->ctx->sh.nested_sp = (u32)c->r[29];                          /* a native function that calls interpreted code again continues below this stack */
    {
        unsigned long long t0 = host_now_ns();
        res = ((u32(*)(u32, u32, u32, u32))fn)(a0, a1, a2, a3);
        c->ctx->sh.ns_native += host_now_ns() - t0;
    }
    c->ctx->sh.nested_sp = 0;
    c->r[2] = sext32(res);
    return 1;
}

static int is_branch(u32 inst)
{
    u32 op = inst >> 26, funct = inst & 63u;
    return op == 1 || op == 2 || op == 3 || (op >= 4 && op <= 7) || (op == 0 && (funct == 8 || funct == 9));
}

/* Development: KYUJO_MIPS_SAMPLE=<n> prints the program counter every 2^n instructions of interpreted code (n = 10..40), to see
 * where a program that does not finish is spending its time. */
static unsigned long long sSampleMask;
static int sSampleRead;
static int sTraceLeft;                  /* instructions still to print (host_mips_trace_next) */

/* Development: prints the next n interpreted instructions (console address, word, the registers $15, $22 and $25 that the Game Boy core
 * uses for its cycle counter, PC and flags - see host_gbemu.c). */
void host_mips_trace_next(int n) { sTraceLeft = n; }

/* ---- the reference loop: one instruction (or one branch with its delay slot) per call ------------------------------------- */
static u64 sRefCallResult;              /* checker: the result register after the fast side's native call */

/* Returns 1 when the interpreted function returned (value in *ret), else 0. */
static int ref_iter(Cpu *c, u32 *ret)
{
    u32 pc = c->pc, inst = fetch(c, pc);
    u32 op, rs, rt, rd;
    u32 target = 0;
    int taken = 0, link = 0, is_return = 0;

    if (sSampleMask && (c->steps & sSampleMask) == 0 && c->steps) {
        fprintf(stderr, "mips sample: pc 0x%08X (console address 0x%08X) after %llu instructions\n", pc, host_fragment_link_addr(pc), c->steps);
    }
    op = inst >> 26; rs = (inst >> 21) & 31; rt = (inst >> 16) & 31; rd = (inst >> 11) & 31;
    if (sTraceLeft > 0) {
        sTraceLeft--;
        fprintf(stderr, "mips %08X: %08X   r15 %08X r22 %08X r25 %08X\n", host_fragment_link_addr(pc), inst, (u32)c->r[15], (u32)c->r[22], (u32)c->r[25]);
    }
    c->steps++;
    if (op == 2 || op == 3) {                               /* j, jal: top 4 bits of the delay slot's address, 26-bit field << 2 */
        taken = 1; link = (op == 3);
        target = ((pc + 4) & 0xF0000000u) | ((inst & 0x03FFFFFFu) << 2);
    } else if (op == 0 && (inst & 63u) == 8) {              /* jr */
        taken = 1; target = (u32)c->r[rs];
        if (target == RETURN_SENTINEL) is_return = 1;
    } else if (op == 0 && (inst & 63u) == 9) {              /* jalr rd, rs (rd = ra when written `jalr rs`) */
        taken = 1; link = 1; target = (u32)c->r[rs];
    } else if (op == 4 || op == 5) {                        /* beq, bne */
        taken = (op == 4) ? (c->r[rs] == c->r[rt]) : (c->r[rs] != c->r[rt]);
        target = pc + 4 + (sext16(inst) << 2);
    } else if (op == 6 || op == 7) {                        /* blez, bgtz */
        taken = (op == 6) ? ((s64)c->r[rs] <= 0) : ((s64)c->r[rs] > 0);
        target = pc + 4 + (sext16(inst) << 2);
    } else if (op == 1) {                                   /* REGIMM: bltz, bgez, bltzal, bgezal */
        switch (rt) {
        case 0x00: taken = (s64)c->r[rs] < 0; break;
        case 0x01: taken = (s64)c->r[rs] >= 0; break;
        case 0x10: taken = (s64)c->r[rs] < 0; link = 1; break;
        case 0x11: taken = (s64)c->r[rs] >= 0; link = 1; break;
        default: bad(c, "unsupported branch (likely forms are not interpreted)", pc, inst, rt);
        }
        target = pc + 4 + (sext16(inst) << 2);
    } else {                                                /* an ordinary instruction */
        step(c, pc, inst);
        c->pc = pc + 4;
        return 0;
    }
    /* Branches and jumps: the delay slot (the next instruction) always runs. A linking branch writes ra (or rd) BEFORE the slot runs
     * but after the target register was read; a conditional linking branch (bgezal) writes it even when not taken (See MIPS Run). */
    if (link) {
        if (op == 0) { if (rd != 0) c->r[rd] = sext32(pc + 8); }
        else c->r[31] = sext32(pc + 8);
    }
    {
        u32 slot = fetch(c, pc + 4);
        if (is_branch(slot)) bad(c, "a branch in a branch delay slot", pc + 4, slot, 0);
        step(c, pc + 4, slot);
        c->steps++;
    }
    if (is_return) { *ret = (u32)c->r[2]; return 1; }
    if (!taken) { c->pc = pc + 8; return 0; }
    if (link) {                                             /* a call: native code is called here, interpreted code is entered */
        int native;
        if (sRefMode) { native = sRefCallNative; if (native) c->r[2] = sRefCallResult; }
        else native = try_native_call(c, target, pc, inst);
        if (native) { c->pc = pc + 8; return 0; }
    }
    if (!sRefMode && target <= pc && host_preempt_flag) HostPreemptPoll();   /* a loop: let a pending device event take effect (may switch threads) */
    c->pc = target;
    return 0;
}

/* ---- the fast interpreter: instructions decoded once, kept in a cache -------------------------------------------------------- */

/* A decoded instruction. The cache is direct-mapped by address; an entry is valid for the address `pc` and for the exact instruction word
 * `raw` (the native-endian word read from memory), so code that the game's loader replaces (another overlay at the same address, relocation)
 * is simply decoded again. The destination register field already holds 32 (the sink) where the instruction names $zero, so a write to $zero
 * needs no test, and $zero itself is never written. */
typedef struct DInst {
    u32 pc, raw, imm;
    unsigned char op, rs, rt, rd;
} DInst;

enum {
    OP_NOP, OP_SLL, OP_SRL, OP_SRA, OP_SLLV, OP_SRLV, OP_SRAV, OP_MFHI, OP_MFLO, OP_MTHI, OP_MTLO, OP_MULT, OP_MULTU, OP_DIV, OP_DIVU,
    OP_ADDU, OP_SUBU, OP_AND, OP_OR, OP_XOR, OP_NOR, OP_SLT, OP_SLTU,
    OP_ADDIU, OP_SLTI, OP_SLTIU, OP_ANDI, OP_ORI, OP_XORI, OP_LUI,
    OP_LB, OP_LBU, OP_LH, OP_LHU, OP_LW, OP_LWU, OP_LD, OP_SB, OP_SH, OP_SW, OP_SD,
    OP_BAD_SPECIAL, OP_BAD_OP,
    OP_BR_FIRST,                                                /* from here on: branches and jumps (they own a delay slot) */
    OP_J = OP_BR_FIRST, OP_JAL, OP_JR, OP_JALR, OP_BEQ, OP_BNE, OP_BLEZ, OP_BGTZ, OP_BLTZ, OP_BGEZ, OP_BLTZAL, OP_BGEZAL, OP_BAD_REGIMM
};

#define DEC_N 16384u                                            /* entries (16 bytes each): covers a 64 KiB stretch of code without collisions */
#ifdef MIPS_STATIC_TABLES
static DInst sDec[DEC_N];
#else
static DInst *sDec;                                             /* DEC_N entries, allocated by mips_mem() */
#endif
static int sMode;                                               /* 0 fast, 1 reference only, 2 both in lockstep (KYUJO_MIPS_CHECK) */
static int sWatchOn;                                            /* KYUJO_MIPS_WATCH is set */

static void decode(DInst *e, u32 pc, u32 inst, u32 nraw)
{
    u32 op = inst >> 26, rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, sh = (inst >> 6) & 31, funct = inst & 63u;
    u32 simm = sext16(inst), dt = rt ? rt : 32, dd = rd ? rd : 32;

    e->pc = pc; e->raw = nraw; e->rs = (unsigned char)rs; e->rt = (unsigned char)rt; e->rd = (unsigned char)dd; e->imm = 0;
    switch (op) {
    case 0x00:
        switch (funct) {
        case 0x00: e->op = inst == 0 ? OP_NOP : OP_SLL; e->imm = sh; break;
        case 0x02: e->op = OP_SRL; e->imm = sh; break;
        case 0x03: e->op = OP_SRA; e->imm = sh; break;
        case 0x04: e->op = OP_SLLV; break;
        case 0x06: e->op = OP_SRLV; break;
        case 0x07: e->op = OP_SRAV; break;
        case 0x08: e->op = OP_JR; break;
        case 0x09: e->op = OP_JALR; break;
        case 0x10: e->op = OP_MFHI; break;
        case 0x11: e->op = OP_MTHI; break;
        case 0x12: e->op = OP_MFLO; break;
        case 0x13: e->op = OP_MTLO; break;
        case 0x18: e->op = OP_MULT; break;
        case 0x19: e->op = OP_MULTU; break;
        case 0x1A: e->op = OP_DIV; break;
        case 0x1B: e->op = OP_DIVU; break;
        case 0x21: e->op = OP_ADDU; break;
        case 0x23: e->op = OP_SUBU; break;
        case 0x24: e->op = OP_AND; break;
        case 0x25: e->op = OP_OR; break;
        case 0x26: e->op = OP_XOR; break;
        case 0x27: e->op = OP_NOR; break;
        case 0x2A: e->op = OP_SLT; break;
        case 0x2B: e->op = OP_SLTU; break;
        default: e->op = OP_BAD_SPECIAL; e->imm = funct; break;
        }
        break;
    case 0x01:
        e->imm = pc + 4 + (simm << 2);
        switch (rt) {
        case 0x00: e->op = OP_BLTZ; break;
        case 0x01: e->op = OP_BGEZ; break;
        case 0x10: e->op = OP_BLTZAL; break;
        case 0x11: e->op = OP_BGEZAL; break;
        default: e->op = OP_BAD_REGIMM; e->imm = rt; break;
        }
        break;
    case 0x02: case 0x03:
        e->op = op == 2 ? OP_J : OP_JAL;
        e->imm = ((pc + 4) & 0xF0000000u) | ((inst & 0x03FFFFFFu) << 2);
        break;
    case 0x04: e->op = OP_BEQ; e->imm = pc + 4 + (simm << 2); break;
    case 0x05: e->op = OP_BNE; e->imm = pc + 4 + (simm << 2); break;
    case 0x06: e->op = OP_BLEZ; e->imm = pc + 4 + (simm << 2); break;
    case 0x07: e->op = OP_BGTZ; e->imm = pc + 4 + (simm << 2); break;
    case 0x09: e->op = OP_ADDIU; e->rd = (unsigned char)dt; e->imm = simm; break;
    case 0x0A: e->op = OP_SLTI; e->rd = (unsigned char)dt; e->imm = simm; break;
    case 0x0B: e->op = OP_SLTIU; e->rd = (unsigned char)dt; e->imm = simm; break;
    case 0x0C: e->op = OP_ANDI; e->rd = (unsigned char)dt; e->imm = inst & 0xFFFFu; break;
    case 0x0D: e->op = OP_ORI; e->rd = (unsigned char)dt; e->imm = inst & 0xFFFFu; break;
    case 0x0E: e->op = OP_XORI; e->rd = (unsigned char)dt; e->imm = inst & 0xFFFFu; break;
    case 0x0F: e->op = OP_LUI; e->rd = (unsigned char)dt; e->imm = (inst & 0xFFFFu) << 16; break;
    case 0x20: e->op = OP_LB; e->rd = (unsigned char)dt; e->imm = simm; break;
    case 0x21: e->op = OP_LH; e->rd = (unsigned char)dt; e->imm = simm; break;
    case 0x23: e->op = OP_LW; e->rd = (unsigned char)dt; e->imm = simm; break;
    case 0x24: e->op = OP_LBU; e->rd = (unsigned char)dt; e->imm = simm; break;
    case 0x25: e->op = OP_LHU; e->rd = (unsigned char)dt; e->imm = simm; break;
    case 0x27: e->op = OP_LWU; e->rd = (unsigned char)dt; e->imm = simm; break;
    case 0x37: e->op = OP_LD; e->rd = (unsigned char)dt; e->imm = simm; break;
    case 0x28: e->op = OP_SB; e->imm = simm; break;
    case 0x29: e->op = OP_SH; e->imm = simm; break;
    case 0x2B: e->op = OP_SW; e->imm = simm; break;
    case 0x3F: e->op = OP_SD; e->imm = simm; break;
    default: e->op = OP_BAD_OP; e->imm = op; break;
    }
}

static const DInst *dfill(Cpu *c, DInst *e, u32 pc)
{
    u32 inst = fetch(c, pc);                                    /* checks that pc is a word address inside the memory pool */
    decode(e, pc, inst, *(const u32 *)(unsigned long)pc);
    return e;
}

static inline const DInst *dlookup(Cpu *c, u32 pc)
{
    DInst *e = &sDec[(pc >> 2) & (DEC_N - 1u)];
    if (__builtin_expect(e->pc == pc && e->raw == *(const u32 *)(unsigned long)pc, 1)) return e;
    return dfill(c, e, pc);
}

#define IN_RANGE(a, n) ((((a) - 0x1000u) <= (0x01000000u - (n) - 0x1000u)) && !((a) & ((n) - 1u)))
#define PTR(a) ((const unsigned char *)(unsigned long)(a))
#define PTRW(a) ((unsigned char *)(unsigned long)(a))

/* A 32-bit access outside the plain range: the RCP registers, or an error. */
static __attribute__((noinline)) u64 lw_slow(Cpu *c, u32 a, u32 pc, int sign)
{
    u32 inst = fetch(c, pc), w;
    if (is_rcp_register(a)) {
        if (a & 3u) bad(c, "an unaligned load from an RCP register", pc, inst, a);
        w = HostHwRead32(a);
    } else {
        const unsigned char *p = mem(c, a, 4, pc, inst);
        w = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
    }
    return sign ? sext32(w) : (u64)w;
}
static __attribute__((noinline)) void sw_slow(Cpu *c, u32 a, u32 pc, u32 value)
{
    u32 inst = fetch(c, pc);
    if (is_rcp_register(a)) {
        if (a & 3u) bad(c, "an unaligned store to an RCP register", pc, inst, a);
        HostHwWrite32(a, value);
        return;
    }
    {
        unsigned char *p = mem(c, a, 4, pc, inst);
        p[0] = (unsigned char)(value >> 24); p[1] = (unsigned char)(value >> 16); p[2] = (unsigned char)(value >> 8); p[3] = (unsigned char)value;
    }
}
static __attribute__((noinline)) void mem_bad(Cpu *c, u32 a, u32 n, u32 pc) { mem(c, a, n, pc, fetch(c, pc)); }

static inline __attribute__((always_inline)) void exec_plain(Cpu *c, const DInst *d, u32 pc)
{
    u64 *r = c->r;
    u32 rs = d->rs, rt = d->rt, rd = d->rd, imm = d->imm, a;
    u64 v;

    switch (d->op) {
    case OP_NOP: return;
    case OP_SLL: r[rd] = sext32((u32)r[rt] << imm); return;
    case OP_SRL: r[rd] = sext32((u32)r[rt] >> imm); return;
    case OP_SRA: r[rd] = sext32((u32)((s32)(u32)r[rt] >> imm)); return;
    case OP_SLLV: r[rd] = sext32((u32)r[rt] << (r[rs] & 31)); return;
    case OP_SRLV: r[rd] = sext32((u32)r[rt] >> (r[rs] & 31)); return;
    case OP_SRAV: r[rd] = sext32((u32)((s32)(u32)r[rt] >> (r[rs] & 31))); return;
    case OP_MFHI: r[rd] = c->hi; return;
    case OP_MFLO: r[rd] = c->lo; return;
    case OP_MTHI: c->hi = r[rs]; return;
    case OP_MTLO: c->lo = r[rs]; return;
    case OP_MULT: {
        s64 m = (s64)(s32)(u32)r[rs] * (s64)(s32)(u32)r[rt];
        c->lo = sext32((u32)m); c->hi = sext32((u32)((u64)m >> 32));
        return;
    }
    case OP_MULTU: {
        u64 m = (u64)(u32)r[rs] * (u64)(u32)r[rt];
        c->lo = sext32((u32)m); c->hi = sext32((u32)(m >> 32));
        return;
    }
    case OP_DIV: {
        s32 x = (s32)(u32)r[rs], y = (s32)(u32)r[rt];
        if (y == 0 || (x == (s32)0x80000000 && y == -1)) bad(c, "division by zero or overflow", pc, fetch(c, pc), (u32)x);
        c->lo = sext32((u32)(x / y)); c->hi = sext32((u32)(x % y));
        return;
    }
    case OP_DIVU: {
        u32 x = (u32)r[rs], y = (u32)r[rt];
        if (y == 0) bad(c, "division by zero", pc, fetch(c, pc), x);
        c->lo = sext32(x / y); c->hi = sext32(x % y);
        return;
    }
    case OP_ADDU: r[rd] = sext32((u32)r[rs] + (u32)r[rt]); return;
    case OP_SUBU: r[rd] = sext32((u32)r[rs] - (u32)r[rt]); return;
    case OP_AND: r[rd] = r[rs] & r[rt]; return;
    case OP_OR: r[rd] = r[rs] | r[rt]; return;
    case OP_XOR: r[rd] = r[rs] ^ r[rt]; return;
    case OP_NOR: r[rd] = ~(r[rs] | r[rt]); return;
    case OP_SLT: r[rd] = (s64)r[rs] < (s64)r[rt]; return;
    case OP_SLTU: r[rd] = r[rs] < r[rt]; return;
    case OP_ADDIU: r[rd] = sext32((u32)r[rs] + imm); return;
    case OP_SLTI: r[rd] = (s64)r[rs] < (s64)(s32)imm; return;
    case OP_SLTIU: r[rd] = r[rs] < (u64)(s64)(s32)imm; return;
    case OP_ANDI: r[rd] = r[rs] & (u64)imm; return;
    case OP_ORI: r[rd] = r[rs] | (u64)imm; return;
    case OP_XORI: r[rd] = r[rs] ^ (u64)imm; return;
    case OP_LUI: r[rd] = sext32(imm); return;
    case OP_LB: a = (u32)r[rs] + imm; if (__builtin_expect(!IN_RANGE(a, 1u), 0)) mem_bad(c, a, 1, pc); r[rd] = (u64)(s64)(signed char)*PTR(a); return;
    case OP_LBU: a = (u32)r[rs] + imm; if (__builtin_expect(!IN_RANGE(a, 1u), 0)) mem_bad(c, a, 1, pc); r[rd] = *PTR(a); return;
    case OP_LH: case OP_LHU:
        a = (u32)r[rs] + imm; if (__builtin_expect(!IN_RANGE(a, 2u), 0)) mem_bad(c, a, 2, pc);
        v = __builtin_bswap16(*(const unsigned short *)PTR(a));
        r[rd] = d->op == OP_LH ? (u64)(s64)(short)(unsigned short)v : v;
        return;
    case OP_LW: case OP_LWU:
        a = (u32)r[rs] + imm;
        if (__builtin_expect(IN_RANGE(a, 4u), 1)) { u32 w = __builtin_bswap32(*(const u32 *)PTR(a)); r[rd] = d->op == OP_LW ? sext32(w) : (u64)w; }
        else r[rd] = lw_slow(c, a, pc, d->op == OP_LW);
        return;
    case OP_LD: a = (u32)r[rs] + imm; if (__builtin_expect(!IN_RANGE(a, 8u), 0)) mem_bad(c, a, 8, pc); r[rd] = __builtin_bswap64(*(const u64 *)PTR(a)); return;
    case OP_SB:
        a = (u32)r[rs] + imm; if (__builtin_expect(!IN_RANGE(a, 1u), 0)) mem_bad(c, a, 1, pc);
        if (__builtin_expect(sWatchOn, 0)) watch_store(pc, fetch(c, pc), a, 1, r[rt] & 0xFF);
        *PTRW(a) = (unsigned char)r[rt]; return;
    case OP_SH:
        a = (u32)r[rs] + imm; if (__builtin_expect(!IN_RANGE(a, 2u), 0)) mem_bad(c, a, 2, pc);
        if (__builtin_expect(sWatchOn, 0)) watch_store(pc, fetch(c, pc), a, 2, r[rt] & 0xFFFF);
        *(unsigned short *)PTRW(a) = __builtin_bswap16((unsigned short)r[rt]); return;
    case OP_SW:
        a = (u32)r[rs] + imm;
        if (__builtin_expect(IN_RANGE(a, 4u), 1)) {
            if (__builtin_expect(sWatchOn, 0)) watch_store(pc, fetch(c, pc), a, 4, r[rt] & 0xFFFFFFFFu);
            *(u32 *)PTRW(a) = __builtin_bswap32((u32)r[rt]);
        } else sw_slow(c, a, pc, (u32)r[rt]);
        return;
    case OP_SD:
        a = (u32)r[rs] + imm; if (__builtin_expect(!IN_RANGE(a, 8u), 0)) mem_bad(c, a, 8, pc);
        *(u64 *)PTRW(a) = __builtin_bswap64(r[rt]); return;
    case OP_BAD_SPECIAL: bad(c, "unsupported register instruction", pc, fetch(c, pc), imm); return;
    default: bad(c, "unsupported instruction", pc, fetch(c, pc), imm); return;
    }
}

/* Same contract as ref_iter. */
static inline __attribute__((always_inline)) int fast_iter(Cpu *c, u32 *ret)
{
    u32 pc = c->pc;
    const DInst *d = dlookup(c, pc);
    u64 *r = c->r;
    u32 target;
    int taken = 0, link = 0, is_return = 0;

    if (d->op < OP_BR_FIRST) { exec_plain(c, d, pc); c->pc = pc + 4; return 0; }
    c->ctx->sh.instr++;                                            /* branches only (blocks), a counter per instruction would cost too much */
    target = d->imm;
    switch (d->op) {
    case OP_J: taken = 1; break;
    case OP_JAL: taken = 1; link = 1; break;
    case OP_JR: taken = 1; target = (u32)r[d->rs]; is_return = target == RETURN_SENTINEL; break;
    case OP_JALR: taken = 1; link = 1; target = (u32)r[d->rs]; break;
    case OP_BEQ: taken = r[d->rs] == r[d->rt]; break;
    case OP_BNE: taken = r[d->rs] != r[d->rt]; break;
    case OP_BLEZ: taken = (s64)r[d->rs] <= 0; break;
    case OP_BGTZ: taken = (s64)r[d->rs] > 0; break;
    case OP_BLTZ: taken = (s64)r[d->rs] < 0; break;
    case OP_BGEZ: taken = (s64)r[d->rs] >= 0; break;
    case OP_BLTZAL: taken = (s64)r[d->rs] < 0; link = 1; break;
    case OP_BGEZAL: taken = (s64)r[d->rs] >= 0; link = 1; break;
    default: bad(c, "unsupported branch (likely forms are not interpreted)", pc, fetch(c, pc), d->imm); break;
    }
    if (link) r[d->op == OP_JALR ? d->rd : 31] = sext32(pc + 8);
    {
        const DInst *slot = dlookup(c, pc + 4);
        if (slot->op >= OP_BR_FIRST) bad(c, "a branch in a branch delay slot", pc + 4, fetch(c, pc + 4), 0);
        exec_plain(c, slot, pc + 4);
    }
    if (is_return) { *ret = (u32)r[2]; return 1; }
    if (!taken) { c->pc = pc + 8; return 0; }
    if (link) {
        int native = try_native_call(c, target, pc, fetch(c, pc));
        if (sMode == 2) sRefCallNative = native;
        if (native) { c->pc = pc + 8; return 0; }
    }
    if (target <= pc && host_preempt_flag) { unsigned long long t0 = host_now_ns(); HostPreemptPoll(); c->ctx->sh.ns_yield += host_now_ns() - t0; }
    c->pc = target;
    return 0;
}

/* KYUJO_MIPS_CHECK: the fast iteration, then the reference on the state from before it; any difference stops the program. */
static int check_iter(Cpu *c, u32 *ret)
{
    Cpu snap = *c;
    u32 ret2 = 0;
    int fin, fin2, i;

    sRefCallNative = 0; sRefSkipped = 0;
    fin = fast_iter(c, ret);
    sRefCallResult = c->r[2];
    sRefMode = 1;
    fin2 = ref_iter(&snap, &ret2);
    sRefMode = 0;
    if (fin != fin2 || (fin && *ret != ret2)) bad(c, "CHECK: the fast and the reference interpreter disagree on the end of the function", snap.pc, 0, fin);
    if (fin || sRefSkipped) return fin;
    if (snap.pc != c->pc) bad(c, "CHECK: the fast and the reference interpreter disagree on the next address", snap.pc, c->pc, snap.pc);
    for (i = 0; i < 32; i++) {
        if (snap.r[i] != c->r[i]) {
            fprintf(stderr, "kyujo: CHECK: register %d: reference 0x%llX, fast 0x%llX (iteration at 0x%08X)\n", i, (unsigned long long)snap.r[i], (unsigned long long)c->r[i], host_fragment_link_addr(snap.pc));
            bad(c, "CHECK: the fast and the reference interpreter disagree on a register", c->pc, 0, (u32)i);
        }
    }
    if (snap.hi != c->hi || snap.lo != c->lo) bad(c, "CHECK: the fast and the reference interpreter disagree on hi/lo", c->pc, 0, 0);
    return 0;
}


/* ---- the block interpreter: straight-line runs of instructions decoded once, run with threaded dispatch ------------------------ */

/* The per-instruction cache above still pays, for every instruction, a hash lookup, a check of the instruction word in memory, one shared
 * indirect jump and a store of the program counter. On the Switch the Game Boy core (about 300000 interpreted instructions per emulated
 * frame) took 15 ms of the 16.7 ms frame with that. Here the code is cut into blocks: from an address up to and including the next branch and
 * its delay slot. A block is found once (a direct-mapped table), its instructions run one after the other through computed gotos (every
 * handler ends with its own indirect jump, which predicts much better than one shared jump) with no per-instruction checks, and the program
 * counter is only set when the block ends. A delay slot that cannot change what the branch reads or writes is moved in front of the branch, so
 * it runs as an ordinary instruction.
 *
 * Validity: a block is used while its generation is the current one and the first instruction word at its address is the one it was built from.
 * host_mips_flush() (called when the Game Boy emulator is set up) starts a new generation. Everything else behaves as in fast_iter (see there);
 * KYUJO_MIPS_BLOCKS=0 turns this engine off, and the reference modes (KYUJO_MIPS_REF / _CHECK, trace, sampling) never use it. */
enum { OP_FALL = OP_BAD_REGIMM + 1, OP_BADSLOT, OP_COUNT };

typedef struct Block {
    u32 pc, raw0, gen, first;                                   /* address, its instruction word when built, generation, index of the first op in sArena */
} Block;
#define BLK_N 8192u
#define ARENA_N 131072u
#define BLK_MAXOPS 48u                                          /* plain instructions before the block is cut without a branch */
#ifdef MIPS_STATIC_TABLES
static Block sBlk[BLK_N];
static DInst sArena[ARENA_N];
#else
static Block *sBlk;                                             /* BLK_N blocks and ARENA_N ops, allocated by mips_mem() */
static DInst *sArena;
#endif
static u32 sArenaUsed, sGen = 1, sArenaGen = 1, sActive;

/* The interpreter's big tables are allocated once, on first use (they are host data: the game never sees these addresses). */
static void mips_mem(void)
{
#ifndef __wasm__
    if ((unsigned long)&sStacks[0][0] + sizeof sStacks > 0x01000000ul) host_fatal("the MIPS interpreter's stacks lie above 16 MiB (host_mips.c): the program image is too large");
#endif
#ifndef MIPS_STATIC_TABLES
    if (sArena) return;
    sDec = calloc(DEC_N, sizeof *sDec);
    sBlk = calloc(BLK_N, sizeof *sBlk);
    sArena = calloc(ARENA_N, sizeof *sArena);
    if (!sDec || !sBlk || !sArena) host_fatal("out of memory for the MIPS interpreter's tables (host_mips.c)");
#endif
}
static int sBlocksOn = -1;

#ifdef __wasm__
static void native_flush(void);
#endif
void host_mips_flush(void)
{
    sGen++;
#ifdef __wasm__
    native_flush();
#endif
}

static int dest_reg(const DInst *d)                             /* the general register an instruction writes (32 = none that matters) */
{
    switch (d->op) {
    case OP_SLL: case OP_SRL: case OP_SRA: case OP_SLLV: case OP_SRLV: case OP_SRAV: case OP_MFHI: case OP_MFLO:
    case OP_ADDU: case OP_SUBU: case OP_AND: case OP_OR: case OP_XOR: case OP_NOR: case OP_SLT: case OP_SLTU:
    case OP_ADDIU: case OP_SLTI: case OP_SLTIU: case OP_ANDI: case OP_ORI: case OP_XORI: case OP_LUI:
    case OP_LB: case OP_LBU: case OP_LH: case OP_LHU: case OP_LW: case OP_LWU: case OP_LD:
        return d->rd;
    default: return 32;
    }
}
static unsigned src_mask(const DInst *d)                        /* the general registers an instruction reads (bit n = $n) */
{
    unsigned rs = 1u << d->rs, rt = 1u << d->rt;
    switch (d->op) {
    case OP_NOP: case OP_MFHI: case OP_MFLO: case OP_LUI: return 0;
    case OP_SLL: case OP_SRL: case OP_SRA: return rt;
    case OP_MTHI: case OP_MTLO: return rs;
    case OP_ADDIU: case OP_SLTI: case OP_SLTIU: case OP_ANDI: case OP_ORI: case OP_XORI:
    case OP_LB: case OP_LBU: case OP_LH: case OP_LHU: case OP_LW: case OP_LWU: case OP_LD: return rs;
    default: return rs | rt;                                    /* ALU register forms, multiply/divide, stores */
    }
}
static unsigned br_src_mask(const DInst *d)
{
    switch (d->op) {
    case OP_J: case OP_JAL: return 0;
    case OP_JR: case OP_JALR: case OP_BLEZ: case OP_BGTZ: case OP_BLTZ: case OP_BGEZ: case OP_BLTZAL: case OP_BGEZAL: return 1u << d->rs;
    default: return (1u << d->rs) | (1u << d->rt);
    }
}
static int br_link_reg(const DInst *d)                          /* the register a branch writes with its return address, 0 = none */
{
    switch (d->op) {
    case OP_JAL: case OP_BLTZAL: case OP_BGEZAL: return 31;
    case OP_JALR: return d->rd == 32 ? 0 : d->rd;               /* (a write to $zero is a write to nothing) */
    default: return 0;
    }
}

/* Builds the block at pc into the arena. Layout of its ops: the plain instructions, then the terminator (a branch, or OP_FALL when the block was cut),
 * then, for a branch whose delay slot was not moved in front of it, the slot's instruction (an OP_NOP when it was moved). */
static const Block *build_block(Cpu *c, Block *b, u32 pc)
{
    u32 n = 0, at = pc, need;
    DInst *ops;
    if (sArenaUsed + BLK_MAXOPS + 4 > ARENA_N) {
        if (sActive > 1) host_fatal("the MIPS interpreter's block cache is full while code is running in it (host_mips.c ARENA_N)");
        sArenaUsed = 0; sGen++; sArenaGen = sGen;
    }
    ops = &sArena[sArenaUsed];
    for (;;) {
        DInst *e = &ops[n];
        u32 inst = fetch(c, at);                                /* checks that the address is a word address inside the memory pool */
        decode(e, at, inst, *(const u32 *)(unsigned long)at);
        if (e->op >= OP_BR_FIRST) {
            DInst *slot = &ops[n + 1];
            u32 sinst = fetch(c, at + 4);
            decode(slot, at + 4, sinst, *(const u32 *)(unsigned long)(at + 4));
            if (slot->op >= OP_BR_FIRST) { e->op = OP_BADSLOT; ops[n + 1].op = OP_NOP; n++; break; }
            if (slot->op == OP_NOP) { n++; break; }               /* an empty slot stays where it is (L_BR skips it) */
            {   /* may the slot run before the branch? */
                int dst = dest_reg(slot), link = br_link_reg(e);
                unsigned reads = src_mask(slot);
                if ((dst == 32 || !(br_src_mask(e) & (1u << dst))) && (!link || (dst != link && !(reads & (1u << link))))) {
                    DInst br = *e, sl = *slot;
                    ops[n] = sl;                                /* the slot becomes an ordinary instruction ... */
                    ops[n + 1] = br;                            /* ... followed by the branch */
                    memset(&ops[n + 2], 0, sizeof ops[n + 2]);  /* (no slot instruction left: OP_NOP is 0) */
                    n++;
                    break;
                }
            }
            n++;
            break;                                              /* the branch is ops[n], its slot ops[n + 1] */
        }
        n++; at += 4;
        if (n >= BLK_MAXOPS) { e = &ops[n]; memset(e, 0, sizeof *e); e->op = OP_FALL; e->pc = at; e->imm = at; ops[n + 1].op = OP_NOP; break; }
    }
    need = n + 2;
    b->pc = pc; b->raw0 = *(const u32 *)(unsigned long)pc; b->gen = sGen; b->first = sArenaUsed;
    sArenaUsed += need;
    return b;
}

static inline const Block *blk_lookup(Cpu *c, u32 pc)
{
    Block *b = &sBlk[(pc >> 2) & (BLK_N - 1u)];
    if (__builtin_expect(b->pc == pc && b->gen == sGen && b->raw0 == *(const u32 *)(unsigned long)pc && b->first < sArenaUsed, 1)) return b;
    return build_block(c, b, pc);
}

static u32 run_blocks(Cpu *c, u32 entry)
{
    static const void *const T[OP_COUNT] = {
        [OP_NOP] = &&L_NOP, [OP_SLL] = &&L_SLL, [OP_SRL] = &&L_SRL, [OP_SRA] = &&L_SRA, [OP_SLLV] = &&L_SLLV, [OP_SRLV] = &&L_SRLV, [OP_SRAV] = &&L_SRAV,
        [OP_MFHI] = &&L_MFHI, [OP_MFLO] = &&L_MFLO, [OP_MTHI] = &&L_MTHI, [OP_MTLO] = &&L_MTLO, [OP_MULT] = &&L_MULT, [OP_MULTU] = &&L_MULTU,
        [OP_DIV] = &&L_DIV, [OP_DIVU] = &&L_DIVU, [OP_ADDU] = &&L_ADDU, [OP_SUBU] = &&L_SUBU, [OP_AND] = &&L_AND, [OP_OR] = &&L_OR, [OP_XOR] = &&L_XOR,
        [OP_NOR] = &&L_NOR, [OP_SLT] = &&L_SLT, [OP_SLTU] = &&L_SLTU, [OP_ADDIU] = &&L_ADDIU, [OP_SLTI] = &&L_SLTI, [OP_SLTIU] = &&L_SLTIU,
        [OP_ANDI] = &&L_ANDI, [OP_ORI] = &&L_ORI, [OP_XORI] = &&L_XORI, [OP_LUI] = &&L_LUI,
        [OP_LB] = &&L_LB, [OP_LBU] = &&L_LBU, [OP_LH] = &&L_LH, [OP_LHU] = &&L_LHU, [OP_LW] = &&L_LW, [OP_LWU] = &&L_LWU, [OP_LD] = &&L_LD,
        [OP_SB] = &&L_SB, [OP_SH] = &&L_SH, [OP_SW] = &&L_SW, [OP_SD] = &&L_SD,
        [OP_BAD_SPECIAL] = &&L_BAD, [OP_BAD_OP] = &&L_BAD,
        [OP_J] = &&L_BR, [OP_JAL] = &&L_BR, [OP_JR] = &&L_BR, [OP_JALR] = &&L_BR, [OP_BEQ] = &&L_BR, [OP_BNE] = &&L_BR, [OP_BLEZ] = &&L_BR,
        [OP_BGTZ] = &&L_BR, [OP_BLTZ] = &&L_BR, [OP_BGEZ] = &&L_BR, [OP_BLTZAL] = &&L_BR, [OP_BGEZAL] = &&L_BR, [OP_BAD_REGIMM] = &&L_BAD,
        [OP_FALL] = &&L_FALL, [OP_BADSLOT] = &&L_BADSLOT,
    };
    u64 *r = c->r;
    const DInst *p;
    u32 pc = entry;
    const DInst *slot;
    u32 target;
    int taken, link;

    if (sActive == 0 && sArenaGen != sGen) { sArenaUsed = 0; sArenaGen = sGen; }
    sActive++;
#define NEXT() do { p++; goto *T[p->op]; } while (0)
#define START_BLOCK() do { p = &sArena[blk_lookup(c, pc)->first]; goto *T[p->op]; } while (0)
    START_BLOCK();

L_NOP: NEXT();
L_SLL: r[p->rd] = sext32((u32)r[p->rt] << p->imm); NEXT();
L_SRL: r[p->rd] = sext32((u32)r[p->rt] >> p->imm); NEXT();
L_SRA: r[p->rd] = sext32((u32)((s32)(u32)r[p->rt] >> p->imm)); NEXT();
L_SLLV: r[p->rd] = sext32((u32)r[p->rt] << (r[p->rs] & 31)); NEXT();
L_SRLV: r[p->rd] = sext32((u32)r[p->rt] >> (r[p->rs] & 31)); NEXT();
L_SRAV: r[p->rd] = sext32((u32)((s32)(u32)r[p->rt] >> (r[p->rs] & 31))); NEXT();
L_MFHI: r[p->rd] = c->hi; NEXT();
L_MFLO: r[p->rd] = c->lo; NEXT();
L_MTHI: c->hi = r[p->rs]; NEXT();
L_MTLO: c->lo = r[p->rs]; NEXT();
L_MULT: { s64 m = (s64)(s32)(u32)r[p->rs] * (s64)(s32)(u32)r[p->rt]; c->lo = sext32((u32)m); c->hi = sext32((u32)((u64)m >> 32)); } NEXT();
L_MULTU: { u64 m = (u64)(u32)r[p->rs] * (u64)(u32)r[p->rt]; c->lo = sext32((u32)m); c->hi = sext32((u32)(m >> 32)); } NEXT();
L_DIV: {
        s32 x = (s32)(u32)r[p->rs], y = (s32)(u32)r[p->rt];
        if (y == 0 || (x == (s32)0x80000000 && y == -1)) bad(c, "division by zero or overflow", p->pc, fetch(c, p->pc), (u32)x);
        c->lo = sext32((u32)(x / y)); c->hi = sext32((u32)(x % y));
    } NEXT();
L_DIVU: {
        u32 x = (u32)r[p->rs], y = (u32)r[p->rt];
        if (y == 0) bad(c, "division by zero", p->pc, fetch(c, p->pc), x);
        c->lo = sext32(x / y); c->hi = sext32(x % y);
    } NEXT();
L_ADDU: r[p->rd] = sext32((u32)r[p->rs] + (u32)r[p->rt]); NEXT();
L_SUBU: r[p->rd] = sext32((u32)r[p->rs] - (u32)r[p->rt]); NEXT();
L_AND: r[p->rd] = r[p->rs] & r[p->rt]; NEXT();
L_OR: r[p->rd] = r[p->rs] | r[p->rt]; NEXT();
L_XOR: r[p->rd] = r[p->rs] ^ r[p->rt]; NEXT();
L_NOR: r[p->rd] = ~(r[p->rs] | r[p->rt]); NEXT();
L_SLT: r[p->rd] = (s64)r[p->rs] < (s64)r[p->rt]; NEXT();
L_SLTU: r[p->rd] = r[p->rs] < r[p->rt]; NEXT();
L_ADDIU: r[p->rd] = sext32((u32)r[p->rs] + p->imm); NEXT();
L_SLTI: r[p->rd] = (s64)r[p->rs] < (s64)(s32)p->imm; NEXT();
L_SLTIU: r[p->rd] = r[p->rs] < (u64)(s64)(s32)p->imm; NEXT();
L_ANDI: r[p->rd] = r[p->rs] & (u64)p->imm; NEXT();
L_ORI: r[p->rd] = r[p->rs] | (u64)p->imm; NEXT();
L_XORI: r[p->rd] = r[p->rs] ^ (u64)p->imm; NEXT();
L_LUI: r[p->rd] = sext32(p->imm); NEXT();
L_LB: { u32 a = (u32)r[p->rs] + p->imm; if (__builtin_expect(!IN_RANGE(a, 1u), 0)) mem_bad(c, a, 1, p->pc); r[p->rd] = (u64)(s64)(signed char)*PTR(a); } NEXT();
L_LBU: { u32 a = (u32)r[p->rs] + p->imm; if (__builtin_expect(!IN_RANGE(a, 1u), 0)) mem_bad(c, a, 1, p->pc); r[p->rd] = *PTR(a); } NEXT();
L_LH: { u32 a = (u32)r[p->rs] + p->imm; if (__builtin_expect(!IN_RANGE(a, 2u), 0)) mem_bad(c, a, 2, p->pc); r[p->rd] = (u64)(s64)(short)__builtin_bswap16(*(const unsigned short *)PTR(a)); } NEXT();
L_LHU: { u32 a = (u32)r[p->rs] + p->imm; if (__builtin_expect(!IN_RANGE(a, 2u), 0)) mem_bad(c, a, 2, p->pc); r[p->rd] = __builtin_bswap16(*(const unsigned short *)PTR(a)); } NEXT();
L_LW: {
        u32 a = (u32)r[p->rs] + p->imm;
        if (__builtin_expect(IN_RANGE(a, 4u), 1)) r[p->rd] = sext32(__builtin_bswap32(*(const u32 *)PTR(a)));
        else r[p->rd] = lw_slow(c, a, p->pc, 1);
    } NEXT();
L_LWU: {
        u32 a = (u32)r[p->rs] + p->imm;
        if (__builtin_expect(IN_RANGE(a, 4u), 1)) r[p->rd] = (u64)__builtin_bswap32(*(const u32 *)PTR(a));
        else r[p->rd] = lw_slow(c, a, p->pc, 0);
    } NEXT();
L_LD: { u32 a = (u32)r[p->rs] + p->imm; if (__builtin_expect(!IN_RANGE(a, 8u), 0)) mem_bad(c, a, 8, p->pc); r[p->rd] = __builtin_bswap64(*(const u64 *)PTR(a)); } NEXT();
L_SB: {
        u32 a = (u32)r[p->rs] + p->imm; if (__builtin_expect(!IN_RANGE(a, 1u), 0)) mem_bad(c, a, 1, p->pc);
        if (__builtin_expect(sWatchOn, 0)) watch_store(p->pc, fetch(c, p->pc), a, 1, r[p->rt] & 0xFF);
        *PTRW(a) = (unsigned char)r[p->rt];
    } NEXT();
L_SH: {
        u32 a = (u32)r[p->rs] + p->imm; if (__builtin_expect(!IN_RANGE(a, 2u), 0)) mem_bad(c, a, 2, p->pc);
        if (__builtin_expect(sWatchOn, 0)) watch_store(p->pc, fetch(c, p->pc), a, 2, r[p->rt] & 0xFFFF);
        *(unsigned short *)PTRW(a) = __builtin_bswap16((unsigned short)r[p->rt]);
    } NEXT();
L_SW: {
        u32 a = (u32)r[p->rs] + p->imm;
        if (__builtin_expect(IN_RANGE(a, 4u), 1)) {
            if (__builtin_expect(sWatchOn, 0)) watch_store(p->pc, fetch(c, p->pc), a, 4, r[p->rt] & 0xFFFFFFFFu);
            *(u32 *)PTRW(a) = __builtin_bswap32((u32)r[p->rt]);
        } else sw_slow(c, a, p->pc, (u32)r[p->rt]);
    } NEXT();
L_SD: { u32 a = (u32)r[p->rs] + p->imm; if (__builtin_expect(!IN_RANGE(a, 8u), 0)) mem_bad(c, a, 8, p->pc); *(u64 *)PTRW(a) = __builtin_bswap64(r[p->rt]); } NEXT();
L_BAD: bad(c, p->op == OP_BAD_SPECIAL ? "unsupported register instruction" : "unsupported instruction", p->pc, fetch(c, p->pc), p->imm); NEXT();
L_BADSLOT: bad(c, "a branch in a branch delay slot", p->pc + 4, fetch(c, p->pc + 4), 0); NEXT();
L_FALL: pc = p->imm; START_BLOCK();

L_BR:
    {
        u32 bpc = p->pc;
        c->ctx->sh.instr++;                                        /* branches only (blocks), a counter per instruction would cost too much */
        slot = p + 1;
        target = p->imm; taken = 0; link = 0;
        switch (p->op) {
        case OP_J: taken = 1; break;
        case OP_JAL: taken = 1; link = 1; break;
        case OP_JR: taken = 1; target = (u32)r[p->rs]; if (target == RETURN_SENTINEL) slot = NULL; break;
        case OP_JALR: taken = 1; link = 1; target = (u32)r[p->rs]; break;
        case OP_BEQ: taken = r[p->rs] == r[p->rt]; break;
        case OP_BNE: taken = r[p->rs] != r[p->rt]; break;
        case OP_BLEZ: taken = (s64)r[p->rs] <= 0; break;
        case OP_BGTZ: taken = (s64)r[p->rs] > 0; break;
        case OP_BLTZ: taken = (s64)r[p->rs] < 0; break;
        case OP_BGEZ: taken = (s64)r[p->rs] >= 0; break;
        case OP_BLTZAL: taken = (s64)r[p->rs] < 0; link = 1; break;
        case OP_BGEZAL: taken = (s64)r[p->rs] >= 0; link = 1; break;
        default: bad(c, "unsupported branch (likely forms are not interpreted)", bpc, fetch(c, bpc), p->imm); break;
        }
        if (link) r[p->op == OP_JALR ? p->rd : 31] = sext32(bpc + 8);
        if (slot && slot->op != OP_NOP) {                       /* a delay slot that was not moved in front of the branch: exec_plain's way */
            exec_plain(c, slot, bpc + 4);
        } else if (!slot) {                                     /* the return to the caller of host_mips_call (its delay slot still runs, as in fast_iter) */
            exec_plain(c, p + 1, bpc + 4);
            sActive--;
            return (u32)r[2];
        }
        if (!taken) { pc = bpc + 8; START_BLOCK(); }
        if (link && try_native_call(c, target, bpc, fetch(c, bpc))) { pc = bpc + 8; START_BLOCK(); }
        if (target <= bpc && host_preempt_flag) { unsigned long long t0 = host_now_ns(); HostPreemptPoll(); c->ctx->sh.ns_yield += host_now_ns() - t0; }
        pc = target;
        START_BLOCK();
    }
    __builtin_unreachable();
}

static u32 run(Cpu *c, u32 entry)
{
    static int sInit;
    int mode;
    u32 ret = 0;

    if (!sInit) {
        const char *e = getenv("KYUJO_MIPS_SAMPLE");
        unsigned i;
        sInit = 1;
        if (e && atoi(e) >= 10 && atoi(e) <= 40) sSampleMask = (1ull << atoi(e)) - 1u;
        e = getenv("KYUJO_MIPS_WATCH");
        sWatchOn = e && *e && strtoul(e, NULL, 16);
        if (getenv("KYUJO_MIPS_REF")) sMode = 1;
        else if (getenv("KYUJO_MIPS_CHECK")) sMode = 2;
        mips_mem();
        for (i = 0; i < DEC_N; i++) sDec[i].pc = 1;             /* an odd address: matches no instruction */
        { const char *b = getenv("KYUJO_MIPS_BLOCKS"); sBlocksOn = !(b && b[0] == '0'); }
        sSampleRead = 1;
    }
    c->pc = entry;
    mode = (sSampleMask || sTraceLeft > 0) ? 1 : sMode;
    if (mode == 0 && sBlocksOn) {
        c->pc = entry;
        return run_blocks(c, entry);
    }
    if (mode == 0) {
        for (;;) if (fast_iter(c, &ret)) return ret;
    } else if (mode == 1) {
        for (;;) if (ref_iter(c, &ret)) return ret;
    } else {
        for (;;) if (check_iter(c, &ret)) return ret;
    }
}


/* ---- the native interpreter (shell/core/mips_native.c) -------------------------------------------------------------------------------- */
/* In the wasm build this file's interpreter runs as translated C, which was no faster with blocks than without them (measured: the Game Boy core's frame took
 * the same time); the same block engine compiled natively is about three times faster than the old per-instruction one. So the shell runs it: it reads and
 * writes the core's memory directly, and comes back into the module (the exports below) for the things only the module can do: call a fragment's C function,
 * take a pending device event at a loop (a thread switch), and read or write an RCP register. The reference modes (KYUJO_MIPS_REF / _CHECK / _SAMPLE /
 * _WATCH, the trace) stay here; KYUJO_MIPS_NATIVE=0 turns the native interpreter off. */
#ifdef __wasm__
__attribute__((import_module("shell"), import_name("mips_run"))) extern unsigned int shell_mips_run(unsigned int entry, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3, unsigned int sp, MipsShared *sh);
__attribute__((import_module("shell"), import_name("mips_flush"))) extern void shell_mips_flush(void);

/* The native interpreter asks: is there a native function for this call target? 0 = no (interpret it); else bit 32 set and the function's result in the low word. */
__attribute__((export_name("core_mips_native_call"))) unsigned long long core_mips_native_call(unsigned int target, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3)
{
    void *fn = host_fragment_native(target);
    if (!fn) return 0;
    return (1ULL << 32) | (unsigned long long)((u32(*)(u32, u32, u32, u32))fn)(a0, a1, a2, a3);
}
__attribute__((export_name("core_mips_preempt_poll"))) void core_mips_preempt_poll(void) { HostPreemptPoll(); }
__attribute__((export_name("core_mips_hw_read"))) unsigned int core_mips_hw_read(unsigned int addr) { return HostHwRead32(addr); }
__attribute__((export_name("core_mips_hw_write"))) void core_mips_hw_write(unsigned int addr, unsigned int data) { HostHwWrite32(addr, data); }

static void native_flush(void) { shell_mips_flush(); }
static int native_wanted(void)
{
    static int on = -1, env = -1;           /* env: KYUJO_MIPS_NATIVE 1 = on, 0 = off, unset = the setting (gb_fast_cpu) */
    if (env < 0) {
        const char *e = getenv("KYUJO_MIPS_NATIVE");
        env = (e && e[0] == '0') ? 0 : (e && e[0]) ? 1 : 2;
    }
    if (on < 0) {
        on = 1;
        if (getenv("KYUJO_MIPS_REF") || getenv("KYUJO_MIPS_CHECK") || getenv("KYUJO_MIPS_SAMPLE") || (getenv("KYUJO_MIPS_WATCH") && *getenv("KYUJO_MIPS_WATCH"))) on = 0;
    }
    return on && sTraceLeft <= 0 && (env == 2 ? host_settings()->gb_fast_cpu : env);
}
#endif

/* ---- entry from native code ----------------------------------------------------------------------------------------------- */
unsigned int host_mips_call(unsigned int entry, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3)
{
    Cpu c;
    ThreadCtx *ctx = ctx_enter();
    u32 sp = ctx->sh.nested_sp ? ctx->sh.nested_sp - 64u : (u32)(unsigned long)(ctx->stack + STACK_BYTES - 16);
    u32 saved_nested = ctx->sh.nested_sp, res;

    if (ctx->sh.nested_sp && (unsigned long)sp < (unsigned long)ctx->stack + 1024u) host_fatal("the interpreted code's stack is exhausted");
    memset(&c, 0, sizeof c);
    c.ctx = ctx;
    c.r[4] = sext32(a0); c.r[5] = sext32(a1); c.r[6] = sext32(a2); c.r[7] = sext32(a3);
    c.r[29] = sext32(sp);
    c.r[31] = sext32(RETURN_SENTINEL);
    ctx->sh.nested_sp = 0;
    {
        unsigned long long t0 = host_now_ns();
        if (saved_nested == 0) { ctx->sh.ns_yield = 0; ctx->sh.ns_native = 0; ctx->sh.instr = 0; }
#ifdef __wasm__
        if (native_wanted()) {
            static int said_native;
            if (!said_native) { said_native = 1; fprintf(stderr, "kyujo: MIPS engine: the native interpreter runs machine code from now on (first call at 0x%08X)\n", entry); }
            ctx->sh.pool_base = (u32)(unsigned long)gPool; ctx->sh.pool_bytes = gHostPoolBytes; ctx->sh.preempt_flag = (u32)(unsigned long)&host_preempt_flag; ctx->sh.dbg_addr = (u32)(unsigned long)host_dbg;
            res = shell_mips_run(entry, a0, a1, a2, a3, sp, &ctx->sh);
        } else
#endif
        {
            static int said_old;
            if (!said_old) { said_old = 1; fprintf(stderr, "kyujo: MIPS engine: the module interpreter runs machine code (first call at 0x%08X)\n", entry); }
            res = run(&c, entry);
        }
        if (saved_nested == 0) {                                                   /* outermost calls only: nested ones are inside this time */
            host_perf_add(PERF_MIPSYIELD, ctx->sh.ns_yield);
            host_perf_add(PERF_MIPSNATIVE, ctx->sh.ns_native);
            host_perf_add(PERF_MIPSKINSTR, ctx->sh.instr * 1000ull);                     /* thousands of branches, shown as "ms" */
            host_perf_add(PERF_MIPS, host_now_ns() - t0);
        }
    }
    ctx->sh.nested_sp = saved_nested;
    ctx->depth--;
    return res;
}
