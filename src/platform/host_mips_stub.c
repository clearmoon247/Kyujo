/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_mips_stub.c - runs the tiny pieces of MIPS machine code that the game loads from the ROM and that have no C source.
 *
 * Why this exists. Most code overlays ("fragments") are compiled from the decomp's C files, and host_fragment.c maps a call into one
 * to the native function. A few fragments are raw data files that the decomp keeps as binary (rom.yaml segments of type `bin`, for
 * example the model archives `late_assets_*`). Each of them still starts with the usual fragment header, and its "main function" (the
 * entry that the game calls with (0, 0), see Fragment_ProcessGeoLayout in src/display_object_textures.c) is a few instructions made by
 * the original build tools. Example, found at the start of such a fragment (annotated by hand from the bytes):
 *
 *      sw    a1, 4(sp)          ; AFA50004
 *      bnez  a0, +3             ; 14800003   (if the argument is not 0 ... )
 *      move  v1, zero           ; 00001825   (... the result is 0)
 *      lui   v1, 0x82           ; 3C030082   (otherwise the result is the address of a table inside the fragment)
 *      addiu v1, v1, -0x3FA4    ; 2463C05C
 *      jr    ra                 ; 03E00008
 *      move  v0, v1             ; 00601025
 *
 * Rather than guess what each such entry returns, this file EXECUTES it with a minimal interpreter. The interpreter knows only the
 * ordinary integer instructions listed in step() (no calls, no multiply/divide, no floating point, no system registers). Anything else
 * stops the program with the address and the instruction word, so a fragment with real code but no C source cannot go unnoticed.
 *
 * The code is read from game memory (big-endian, as the console had it; the loader has already relocated its addresses), and the
 * addresses it computes are the native addresses of that memory, because this port maps the console's physical addresses to the
 * same numbers (see docs/architecture.md, "Memory model").
 *
 * Reference for the instruction encodings: "MIPS IV Instruction Set" (Price, 1995) and "See MIPS Run" (Sweetman); the field layout
 * used below is op[31:26] rs[25:21] rt[20:16] rd[15:11] shamt[10:6] funct[5:0], immediate = low 16 bits.
 *
 * HOST-side file (plain C types only).
 */
#include <stdio.h>

#include "platform.h"

extern unsigned char gPool[];                                   /* the game's memory pool (src/heap.c) */
extern const unsigned int gHostPoolBytes;

#define MAX_STEPS 4096u                                         /* a stub that runs longer than this is not a stub */
#define STACK_BYTES 256u
#define RETURN_SENTINEL 0xFFFFFFF0u                             /* value of `ra` at entry: `jr ra` to it ends the run */

/* Everything a run needs. One run executes at a time (the baton design runs one game thread at once) but the structure is kept local so
 * a run could be re-entered safely. */
struct Cpu {
    unsigned int r[32];
    unsigned int pc;
    unsigned char stack[STACK_BYTES];                           /* the stub's private stack (sp points into it) */
};

static void bad(const struct Cpu *c, const char *why, unsigned int pc, unsigned int inst)
{
    (void)c;
    fprintf(stderr, "kyujo: machine code at 0x%08X (word 0x%08X) cannot be run by the stub interpreter: %s\n", pc, inst, why);
    host_fatal("unsupported machine code in a fragment that has no C source");
}

/* ---- memory access, big-endian. Only the stub's stack and the memory pool may be touched. ---------------------------------------- */
static unsigned char *mem(struct Cpu *c, unsigned int addr, unsigned int bytes, unsigned int pc, unsigned int inst)
{
    unsigned long base = (unsigned long)c->stack;
    if (addr >= base && addr - base <= STACK_BYTES - bytes) {
        return (unsigned char *)(unsigned long)addr;
    }
    base = (unsigned long)gPool;
    if (addr >= base && addr - base <= gHostPoolBytes - bytes) {
        return (unsigned char *)(unsigned long)addr;
    }
    bad(c, "a load or store outside the stub's stack and the memory pool", pc, inst);
    return NULL;
}

static unsigned int fetch(const struct Cpu *c, unsigned int pc)
{
    const unsigned char *b = (const unsigned char *)(unsigned long)pc;
    unsigned long base = (unsigned long)gPool;
    if (pc & 3u || pc < base || pc - base > gHostPoolBytes - 4u) {
        bad(c, "the program counter left the memory pool", pc, 0);
    }
    return ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) | ((unsigned int)b[2] << 8) | b[3];
}

static unsigned int sext16(unsigned int v) { return (unsigned int)(int)(short)(v & 0xFFFFu); }

/* Executes one instruction at `pc` (not a branch/jump: those are handled in run()). Returns nothing; changes c->r. */
static void step(struct Cpu *c, unsigned int pc, unsigned int inst)
{
    unsigned int op = inst >> 26, rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, sh = (inst >> 6) & 31;
    unsigned int imm = inst & 0xFFFFu, funct = inst & 63u;
    unsigned int *r = c->r;
    unsigned int a, v;
    unsigned char *p;

    if (op == 0) {                                               /* SPECIAL: register-register */
        switch (funct) {
        case 0x00: v = r[rt] << sh; break;                       /* sll (nop is sll zero,zero,0) */
        case 0x02: v = r[rt] >> sh; break;                       /* srl */
        case 0x03: v = (unsigned int)((int)r[rt] >> sh); break;  /* sra */
        case 0x21: v = r[rs] + r[rt]; break;                     /* addu (also `move`) */
        case 0x23: v = r[rs] - r[rt]; break;                     /* subu */
        case 0x24: v = r[rs] & r[rt]; break;                     /* and */
        case 0x25: v = r[rs] | r[rt]; break;                     /* or (also `move`) */
        case 0x26: v = r[rs] ^ r[rt]; break;                     /* xor */
        case 0x27: v = ~(r[rs] | r[rt]); break;                  /* nor */
        case 0x2A: v = (int)r[rs] < (int)r[rt]; break;           /* slt */
        case 0x2B: v = r[rs] < r[rt]; break;                     /* sltu */
        default: bad(c, "unsupported register instruction", pc, inst); return;
        }
        if (rd != 0) {
            r[rd] = v;
        }
        return;
    }
    switch (op) {
    case 0x09: v = r[rs] + sext16(imm); break;                   /* addiu */
    case 0x0A: v = (int)r[rs] < (int)sext16(imm); break;         /* slti */
    case 0x0B: v = r[rs] < sext16(imm); break;                   /* sltiu (the immediate is sign-extended, then compared unsigned) */
    case 0x0C: v = r[rs] & imm; break;                           /* andi */
    case 0x0D: v = r[rs] | imm; break;                           /* ori */
    case 0x0E: v = r[rs] ^ imm; break;                           /* xori */
    case 0x0F: v = imm << 16; break;                             /* lui */
    case 0x23: case 0x21: case 0x25: case 0x20: case 0x24: {     /* lw, lh, lhu, lb, lbu */
        unsigned int n = (op == 0x23) ? 4 : (op == 0x21 || op == 0x25) ? 2 : 1;
        a = r[rs] + sext16(imm);
        if (a % n) { bad(c, "unaligned load", pc, inst); }
        p = mem(c, a, n, pc, inst);
        v = (n == 4) ? ((unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 | (unsigned int)p[2] << 8 | p[3])
          : (n == 2) ? ((unsigned int)p[0] << 8 | p[1]) : p[0];
        if (op == 0x21) { v = sext16(v); }                       /* lh */
        if (op == 0x20) { v = (unsigned int)(int)(signed char)v; }   /* lb */
        break;
    }
    case 0x2B: case 0x29: case 0x28: {                           /* sw, sh, sb */
        unsigned int n = (op == 0x2B) ? 4 : (op == 0x29) ? 2 : 1;
        a = r[rs] + sext16(imm);
        if (a % n) { bad(c, "unaligned store", pc, inst); }
        p = mem(c, a, n, pc, inst);
        v = r[rt];
        if (n == 4) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
        else if (n == 2) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
        else { p[0] = (unsigned char)v; }
        return;
    }
    default:
        bad(c, "unsupported instruction (calls, multiply/divide, floating point and system instructions are not interpreted)", pc, inst);
        return;
    }
    if (rt != 0) {                                               /* immediate forms write rt; loads too */
        r[rt] = v;
    }
}

/* Runs the code at `entry` with the C calling convention's four argument registers and returns v0. */
static unsigned int run(unsigned int entry, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3)
{
    struct Cpu c = {{0}, 0, {0}};
    unsigned int steps;

    c.r[4] = a0; c.r[5] = a1; c.r[6] = a2; c.r[7] = a3;
    c.r[29] = (unsigned int)(unsigned long)(c.stack + STACK_BYTES - 16);   /* sp, keeping room for the caller-save area above it */
    c.r[31] = RETURN_SENTINEL;
    c.pc = entry;

    for (steps = 0; steps < MAX_STEPS; steps++) {
        unsigned int pc = c.pc, inst = fetch(&c, pc);
        unsigned int op = inst >> 26, rs = (inst >> 21) & 31, rt = (inst >> 16) & 31;
        int taken = 0;
        unsigned int target = 0;
        int is_return = 0;

        if (op == 2) {                                           /* j: top 4 bits of the delay-slot address, 26-bit field << 2 */
            taken = 1;
            target = ((pc + 4) & 0xF0000000u) | ((inst & 0x03FFFFFFu) << 2);
        } else if (op == 0 && (inst & 63u) == 8) {               /* jr */
            taken = 1;
            target = c.r[rs];
            if (target == RETURN_SENTINEL) { is_return = 1; }
        } else if (op == 4 || op == 5) {                         /* beq, bne */
            taken = (op == 4) ? (c.r[rs] == c.r[rt]) : (c.r[rs] != c.r[rt]);
            target = pc + 4 + (sext16(inst) << 2);
            if (!taken) { c.pc = pc + 4; step(&c, pc + 4, fetch(&c, pc + 4)); c.pc = pc + 8; continue; }
        } else if (op == 6 || op == 7) {                         /* blez, bgtz (rt must be 0) */
            taken = (op == 6) ? ((int)c.r[rs] <= 0) : ((int)c.r[rs] > 0);
            target = pc + 4 + (sext16(inst) << 2);
            if (!taken) { c.pc = pc + 4; step(&c, pc + 4, fetch(&c, pc + 4)); c.pc = pc + 8; continue; }
        } else if (op == 1 && (rt == 0 || rt == 1)) {            /* bltz, bgez (the non-linking forms only) */
            taken = (rt == 0) ? ((int)c.r[rs] < 0) : ((int)c.r[rs] >= 0);
            target = pc + 4 + (sext16(inst) << 2);
            if (!taken) { c.pc = pc + 4; step(&c, pc + 4, fetch(&c, pc + 4)); c.pc = pc + 8; continue; }
        } else if (op == 3 || (op == 0 && (inst & 63u) == 9) || op == 1) {
            bad(&c, "a call (jal/jalr) or linking branch: this stub calls other code", pc, inst);
        } else {                                                 /* an ordinary instruction */
            step(&c, pc, inst);
            c.pc = pc + 4;
            continue;
        }
        /* A taken branch or a jump: the instruction after it (its delay slot) runs first, then control moves. The delay slot must not
         * itself be a branch. */
        {
            unsigned int slot = fetch(&c, pc + 4);
            unsigned int sop = slot >> 26;
            if (sop == 1 || sop == 2 || sop == 3 || (sop >= 4 && sop <= 7) || (sop == 0 && ((slot & 63u) == 8 || (slot & 63u) == 9))) {
                bad(&c, "a branch in a branch delay slot", pc + 4, slot);
            }
            step(&c, pc + 4, slot);
        }
        if (is_return) {
            return c.r[2];                                       /* v0 */
        }
        c.pc = target;
    }
    bad(&c, "the stub did not finish within the step limit", c.pc, 0);
    return 0;
}

/* ---- native entry points -------------------------------------------------------------------------------------------------------
 * The game calls the stub through a plain function pointer, so each stub address needs its own native function that remembers the address.
 * A fixed set of such functions is generated by the macro below; host_mips_thunk() hands them out. The thunk reads the code from game
 * memory at every call, so if the pool address is later reused for another fragment the thunk runs whatever is there then.
 */
#define THUNK_COUNT 32
static unsigned int sThunkAddr[THUNK_COUNT];
static unsigned int sThunkUsed;

#define DEFINE_THUNK(n) \
    static unsigned int thunk_##n(unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3) { return run(sThunkAddr[n], a0, a1, a2, a3); }
DEFINE_THUNK(0)  DEFINE_THUNK(1)  DEFINE_THUNK(2)  DEFINE_THUNK(3)  DEFINE_THUNK(4)  DEFINE_THUNK(5)  DEFINE_THUNK(6)  DEFINE_THUNK(7)
DEFINE_THUNK(8)  DEFINE_THUNK(9)  DEFINE_THUNK(10) DEFINE_THUNK(11) DEFINE_THUNK(12) DEFINE_THUNK(13) DEFINE_THUNK(14) DEFINE_THUNK(15)
DEFINE_THUNK(16) DEFINE_THUNK(17) DEFINE_THUNK(18) DEFINE_THUNK(19) DEFINE_THUNK(20) DEFINE_THUNK(21) DEFINE_THUNK(22) DEFINE_THUNK(23)
DEFINE_THUNK(24) DEFINE_THUNK(25) DEFINE_THUNK(26) DEFINE_THUNK(27) DEFINE_THUNK(28) DEFINE_THUNK(29) DEFINE_THUNK(30) DEFINE_THUNK(31)

#define T(n) (void *)thunk_##n
static void *const sThunks[THUNK_COUNT] = {
    T(0),  T(1),  T(2),  T(3),  T(4),  T(5),  T(6),  T(7),  T(8),  T(9),  T(10), T(11), T(12), T(13), T(14), T(15),
    T(16), T(17), T(18), T(19), T(20), T(21), T(22), T(23), T(24), T(25), T(26), T(27), T(28), T(29), T(30), T(31),
};

void *host_mips_thunk(unsigned int pool_addr)
{
    unsigned int i;
    for (i = 0; i < sThunkUsed; i++) {
        if (sThunkAddr[i] == pool_addr) {
            return sThunks[i];
        }
    }
    if (sThunkUsed == THUNK_COUNT) {
        host_fatal("too many distinct machine-code entry points without C source (raise THUNK_COUNT in host_mips_stub.c)");
    }
    sThunkAddr[sThunkUsed] = pool_addr;
    return sThunks[sThunkUsed++];
}
