/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * mips_native.c - the MIPS interpreter for the Game Boy emulator's hand-written core, compiled natively (the module's own copy, src/platform/host_mips.c, runs as
 * translated C and is no faster with blocks than without). The code, the data and the block cache logic are those of host_mips.c's block engine (see the long
 * comment there for how blocks, delay slots and validity work); what differs is where memory is: a guest address is an offset into the core's linear memory
 * (gM), and everything only the module can do is a call into it (core_mips_native_call: a fragment's C function; core_mips_preempt_poll: a thread switch at a
 * loop; core_mips_hw_read / _write: an RCP register).
 *
 * Differential-tested against the module's reference interpreter on random programs (tests/native/mips_fuzz.c).
 * NATIVE-side file (the shell). Called with the wasm lock held, by the game thread that called host_mips_call.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef MIPS_NATIVE_TEST
#include "core_host.h"
#include "wasm-rt.h"
#include "wasm-rt-impl.h"
#include "kyujo.h"
#include "core_internal.h"
#include "../../src/platform/mips_shared.h"
/* what only the module can do */
#define MIPS_HW_READ(a) w2c_kyujo_core_mips_hw_read(&gCore.inst, (a))
#define MIPS_HW_WRITE(a, v) w2c_kyujo_core_mips_hw_write(&gCore.inst, (a), (v))
#define MIPS_NATIVE_CALL(t, a0, a1, a2, a3) w2c_kyujo_core_mips_native_call(&gCore.inst, (t), (a0), (a1), (a2), (a3))
#define MIPS_PREEMPT() w2c_kyujo_core_mips_preempt_poll(&gCore.inst)
/* Another thread (a timer, the audio worker...) that wants the core's lock asks the holder to hand it over at its next loop header (core_host.c). Code
 * run here has no such headers, so a loop of machine code (the Game Boy core waiting on an RSP signal) would keep the lock for ever, and the video
 * retrace that would end the wait could never be posted: a back branch has to honour the request as well. */
extern volatile int gYieldReq;
void core_yield_point(void);
#define MIPS_YIELD() do { if (gYieldReq) core_yield_point(); } while (0)
#else
#define MIPS_YIELD() ((void)0)
#include "mips_shared.h"                                         /* the differential test (tests/native/mips_fuzz_native.c) supplies these */
#endif

typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

unsigned long long host_now_ns(void);
void host_fatal(const char *message) __attribute__((noreturn));

#define RETURN_SENTINEL 0xFFFFFFF0u
#define MAIN_RMONPRINTF 0x0005E22Cu      /* a no-op (see host_mips.c) */

typedef struct Cpu {
    u64 r[33];                                                  /* r[32] is a sink: decoded instructions write there instead of $zero */
    u64 hi, lo;
    u32 pc;
    MipsShared *sh;
} Cpu;

static uint8_t *gM;                                             /* the core's linear memory: guest address a is gM + a */
static u32 sPoolBase, sPoolBytes;
static const int sWatchOn = 0;
static void watch_store(u32 pc, u32 inst, u32 a, int size, u64 value) { (void)pc; (void)inst; (void)a; (void)size; (void)value; }

static void bad(const Cpu *c, const char *why, u32 pc, u32 inst, u32 extra)
{
    (void)c;
    fprintf(stderr, "kyujo: machine code at 0x%08X (word 0x%08X, value 0x%08X): %s\n", pc, inst, extra, why);
    host_fatal("the MIPS interpreter (mips_native.c) met something it does not support");
}

static u32 fetch(const Cpu *c, u32 pc)
{
    const unsigned char *b = gM + pc;
    if ((pc & 3u) || pc < sPoolBase || pc - sPoolBase > sPoolBytes - 4u) bad(c, "the program counter left the memory pool", pc, 0, pc);
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static u64 sext32(u32 v) { return (u64)(s64)(s32)v; }
static u32 sext16(u32 v) { return (u32)(s32)(short)(v & 0xFFFFu); }

#define IN_RANGE(a, n) ((((a) - 0x1000u) <= (0x01000000u - (n) - 0x1000u)) && !((a) & ((n) - 1u)))
#define PTR(a) ((const unsigned char *)(gM + (a)))
#define PTRW(a) ((unsigned char *)(gM + (a)))

static int is_rcp_register(u32 a) { return (a >> 24) == 0xA4u; }

/* What a stall report (main_sdl.c) can say about the interpreter without disturbing it: the branches run last, a copy of the registers taken every 256 branches
 * (nothing here points into a thread's state: a thread can be switched out at any time), the last RCP access, and whether a thread is inside the interpreter. */
static volatile u32 sRing[16], sRingN, sHwAddr, sHwVal, sHwKind;
static volatile unsigned long long sSnap[7], sHwCount;
static volatile int sRunning;                                   /* runs entered and not yet left (all threads) */
void mips_native_describe(char *buf, size_t n)
{
    size_t k = 0;
    u32 i, rn = sRingN;
    int w;
    if (!n) return;
    buf[0] = 0;
    if (!sRingN) { snprintf(buf, n, "never used"); return; }
    w = snprintf(buf + k, n - k, "%s; branches (newest first):", sRunning > 0 ? "inside the interpreter" : "not inside the interpreter now"); if (w > 0) k += (size_t)w;
    for (i = 0; i < 12 && k < n; i++) { w = snprintf(buf + k, n - k, " %06X", sRing[(rn - 1 - i) & 15]); if (w > 0) k += (size_t)w; }
    if (k < n)
        snprintf(buf + k, n - k, "; regs (up to 256 branches old) v0 %llX a0 %llX t0 %llX t1 %llX s0 %llX sp %llX ra %llX; last RCP access: %s %08X value %08X (%llu in all)",
                 sSnap[0], sSnap[1], sSnap[2], sSnap[3], sSnap[4], sSnap[5], sSnap[6], sHwKind ? "write" : "read", (unsigned)sHwAddr, (unsigned)sHwVal, sHwCount);
}
static unsigned sDbgAddr;
static const volatile u32 *dbg_ptr(void)
{
    if (!sDbgAddr) sDbgAddr = w2c_kyujo_core_dbg_addr(&gCore.inst);
    return (const volatile u32 *)(w2c_kyujo_memory(&gCore.inst)->data + sDbgAddr);
}
/* The scheduler's view (host_dbg in the core, filled by hw_sp.c / os_sched.c): where the signal wait stands, which threads exist and what each waits for. */
void core_dbg_describe(char *buf, size_t n)
{
    const volatile u32 *d = dbg_ptr();
    size_t k = 0;
    unsigned i, nt = d[10] < 24 ? d[10] : 24;
    int w;
    if (!n) return;
    w = snprintf(buf, n, "SP status %X flags %X; audio tasks started %u ended %u, others started %u; preempt polls %u (delivering %u); IE/mask %X; running thread %X; run queue top priority %u; events delivered (kind:count):",
                 d[0], d[1], d[2], d[3], d[9], d[4], d[5], d[6], d[7], d[8]);
    if (w > 0) k = (size_t)w < n ? (size_t)w : n - 1;
    for (i = 0; i < 10 && k < n; i++) if (d[16 + i]) { w = snprintf(buf + k, n - k, " %u:%u", i, d[16 + i]); if (w > 0) k += (size_t)w; }
    if (k < n) { w = snprintf(buf + k, n - k, "; threads (id/priority state queue pc):"); if (w > 0) k += (size_t)w; }
    for (i = 0; i < nt && k < n; i++) {
        u32 a = d[32 + i * 3], b = d[33 + i * 3], pc = d[34 + i * 3];
        w = snprintf(buf + k, n - k, " %u/%u s%u q%04X pc%08X;", a >> 16, a & 0xFFFF, b >> 16, b & 0xFFFF, pc);
        if (w > 0) k += (size_t)w;
    }
}
/* A number that changes while the game runs: video retraces delivered to the game plus the branches counted by the interpreter. */
unsigned core_dbg_progress(void)
{
    const volatile u32 *d = dbg_ptr();
    { unsigned i, t = 0; for (i = 0; i < 10; i++) t += d[16 + i]; return t; }     /* (every event kind: any event delivered is progress) */
}
static __attribute__((noinline)) void mem_bad(Cpu *c, u32 a, u32 n, u32 pc)
{
    u32 inst = fetch(c, pc);
    if (a < 0x1000u || a > 0x01000000u - n) bad(c, "a load or store outside game memory (0x1000..0x00FFFFFF)", pc, inst, a);
    bad(c, "an unaligned load or store", pc, inst, a);
}
static __attribute__((noinline)) u64 lw_slow(Cpu *c, u32 a, u32 pc, int sign)
{
    u32 inst = fetch(c, pc), w;
    if (is_rcp_register(a)) {
        if (a & 3u) bad(c, "an unaligned load from an RCP register", pc, inst, a);
        w = MIPS_HW_READ(a);
        sHwKind = 0; sHwAddr = a; sHwVal = w; sHwCount++;
    } else {
        if (a < 0x1000u || a > 0x01000000u - 4u || (a & 3u)) mem_bad(c, a, 4, pc);
        w = (u32)gM[a] << 24 | (u32)gM[a + 1] << 16 | (u32)gM[a + 2] << 8 | gM[a + 3];
    }
    return sign ? sext32(w) : (u64)w;
}
static __attribute__((noinline)) void sw_slow(Cpu *c, u32 a, u32 pc, u32 value)
{
    u32 inst = fetch(c, pc);
    if (is_rcp_register(a)) {
        if (a & 3u) bad(c, "an unaligned store to an RCP register", pc, inst, a);
        sHwKind = 1; sHwAddr = a; sHwVal = value; sHwCount++;
        MIPS_HW_WRITE(a, value);
        return;
    }
    if (a < 0x1000u || a > 0x01000000u - 4u || (a & 3u)) mem_bad(c, a, 4, pc);
    gM[a] = (unsigned char)(value >> 24); gM[a + 1] = (unsigned char)(value >> 16); gM[a + 2] = (unsigned char)(value >> 8); gM[a + 3] = (unsigned char)value;
}

/* A call target inside the pool: a fragment's C function runs natively (inside the module), anything else is interpreted. Targets known to have no native version
 * are remembered (the table is dropped by a flush). */
static u32 sNoNative[1024];
static int native_call(Cpu *c, u32 target, u32 pc)
{
    (void)pc;
    if (target >= sPoolBase && target - sPoolBase < sPoolBytes) {
        u64 r;
        unsigned long long t0;
        u32 *slot = &sNoNative[(target >> 2) & 1023u];
        if (*slot == target) return 0;
        t0 = host_now_ns();
        c->sh->nested_sp = (u32)c->r[29];                       /* a native function that calls interpreted code again continues below this stack */
        r = MIPS_NATIVE_CALL(target, (u32)c->r[4], (u32)c->r[5], (u32)c->r[6], (u32)c->r[7]);
        c->sh->nested_sp = 0;
        if (!(r >> 32)) { *slot = target; return 0; }
        c->sh->ns_native += host_now_ns() - t0;
        c->r[2] = sext32((u32)r);
        return 1;
    }
    if (target == MAIN_RMONPRINTF) return 1;
    bad(c, "a call outside the memory pool to a function that is not known", pc, 0, target);
    return 0;
}

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


enum { OP_DUMMY_END_MARK = 0 };
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
        if (__builtin_expect(sWatchOn, 0)) watch_store(pc, 0, a, 1, r[rt] & 0xFF);
        *PTRW(a) = (unsigned char)r[rt]; return;
    case OP_SH:
        a = (u32)r[rs] + imm; if (__builtin_expect(!IN_RANGE(a, 2u), 0)) mem_bad(c, a, 2, pc);
        if (__builtin_expect(sWatchOn, 0)) watch_store(pc, 0, a, 2, r[rt] & 0xFFFF);
        *(unsigned short *)PTRW(a) = __builtin_bswap16((unsigned short)r[rt]); return;
    case OP_SW:
        a = (u32)r[rs] + imm;
        if (__builtin_expect(IN_RANGE(a, 4u), 1)) {
            if (__builtin_expect(sWatchOn, 0)) watch_store(pc, 0, a, 4, r[rt] & 0xFFFFFFFFu);
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


enum { OP_FALL = OP_BAD_REGIMM + 1, OP_BADSLOT, OP_COUNT };

typedef struct Block {
    u32 pc, raw0, gen, first;                                   /* address, its instruction word when built, generation, index of the first op in sArena */
} Block;
#define BLK_N 8192u
#define ARENA_N 131072u
#define BLK_MAXOPS 48u                                          /* plain instructions before the block is cut without a branch */
static Block sBlk[BLK_N];
static DInst sArena[ARENA_N];
static u32 sArenaUsed, sGen = 1, sArenaGen = 1, sActive;
static int sBlocksOn = -1;


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
        decode(e, at, inst, *(const u32 *)(gM + at));
        if (e->op >= OP_BR_FIRST) {
            DInst *slot = &ops[n + 1];
            u32 sinst = fetch(c, at + 4);
            decode(slot, at + 4, sinst, *(const u32 *)(gM + (at + 4)));
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
    b->pc = pc; b->raw0 = *(const u32 *)(gM + pc); b->gen = sGen; b->first = sArenaUsed;
    sArenaUsed += need;
    return b;
}

static inline const Block *blk_lookup(Cpu *c, u32 pc)
{
    Block *b = &sBlk[(pc >> 2) & (BLK_N - 1u)];
    if (__builtin_expect(b->pc == pc && b->gen == sGen && b->raw0 == *(const u32 *)(gM + pc) && b->first < sArenaUsed, 1)) return b;
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
    sRunning++;
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
        if (!(sRingN & 255u)) { sSnap[0] = r[2]; sSnap[1] = r[4]; sSnap[2] = r[8]; sSnap[3] = r[9]; sSnap[4] = r[16]; sSnap[5] = r[29]; sSnap[6] = r[31]; }
        sRing[sRingN++ & 15] = bpc;
        c->sh->instr++;                                                  /* branches (blocks) executed, for the statistics; one counter per instruction would cost too much */
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
            sRunning--;
            return (u32)r[2];
        }
        if (!taken) { pc = bpc + 8; START_BLOCK(); }
        if (link && native_call(c, target, bpc)) { pc = bpc + 8; START_BLOCK(); }
        if (target <= bpc && (*(volatile u32 *)(gM + c->sh->preempt_flag))) { unsigned long long t0 = host_now_ns(); MIPS_PREEMPT(); c->sh->ns_yield += host_now_ns() - t0; }
        if (target <= bpc) MIPS_YIELD();
        pc = target;
        START_BLOCK();
    }
    __builtin_unreachable();
}



#ifndef MIPS_NATIVE_TEST
/* ---- entry from the module ---------------------------------------------------------------------------------------------------- */
uint32_t w2c_shell_mips_run(struct w2c_shell *s, uint32_t entry, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t sp, uint32_t sh_addr)
{
    Cpu c;
    (void)s;
    gM = w2c_kyujo_memory(&gCore.inst)->data;
    c.sh = (MipsShared *)(gM + sh_addr);
    sPoolBase = c.sh->pool_base; sPoolBytes = c.sh->pool_bytes;
    memset(&c, 0, offsetof(Cpu, sh));
    c.r[4] = sext32(a0); c.r[5] = sext32(a1); c.r[6] = sext32(a2); c.r[7] = sext32(a3);
    c.r[29] = sext32(sp);
    c.r[31] = sext32(RETURN_SENTINEL);
    return run_blocks(&c, entry);
}

void w2c_shell_mips_flush(struct w2c_shell *s)
{
    (void)s;
    sGen++;
    memset(sNoNative, 0, sizeof sNoNative);
}
#endif
