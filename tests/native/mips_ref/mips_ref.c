/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/* mips_ref.c - see mips_ref.h. TEST-ONLY code. */
#include <stdio.h>
#include <string.h>

#include "mips_ref.h"

#define SENTINEL 0xFFFFFFF0u        /* return address given to the called function; `jr ra` to it ends the call */

void mips_ref_init(MipsRef *m, unsigned char *mem, unsigned int size)
{
    memset(m, 0, sizeof *m);
    m->mem = mem;
    m->mem_size = size;
    m->max_steps = 50000000ull;
}

static int fail(MipsRef *m, const char *why, unsigned int pc, unsigned int word)
{
    if (!m->error[0]) {
        snprintf(m->error, sizeof m->error, "%s (pc 0x%08X, word 0x%08X)", why, pc, word);
    }
    return -1;
}

/* Returns a pointer to `bytes` bytes at console address `addr`, or NULL if outside the block. */
static unsigned char *at(MipsRef *m, unsigned int addr, unsigned int bytes)
{
    unsigned int p = addr & 0x1FFFFFFFu;
    if (p > m->mem_size || bytes > m->mem_size - p) {
        return NULL;
    }
    return m->mem + p;
}

unsigned int mips_ref_read32(const MipsRef *m, unsigned int addr)
{
    const unsigned char *p = m->mem + (addr & 0x1FFFFFFFu);
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

void mips_ref_write32(MipsRef *m, unsigned int addr, unsigned int v)
{
    unsigned char *p = m->mem + (addr & 0x1FFFFFFFu);
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

unsigned int mips_ref_read16(const MipsRef *m, unsigned int addr)
{
    const unsigned char *p = m->mem + (addr & 0x1FFFFFFFu);
    return ((unsigned int)p[0] << 8) | p[1];
}

void mips_ref_write16(MipsRef *m, unsigned int addr, unsigned int v)
{
    unsigned char *p = m->mem + (addr & 0x1FFFFFFFu);
    p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v;
}

static unsigned int sx16(unsigned int v) { return (unsigned int)(int)(short)(v & 0xFFFFu); }

/* Executes the instruction `w` found at `pc` that is NOT a jump or branch (the delay-slot instruction and ordinary ones). */
static int exec_plain(MipsRef *m, unsigned int pc, unsigned int w)
{
    unsigned int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sh = (w >> 6) & 31, fn = w & 63;
    unsigned int imm = w & 0xFFFFu;
    unsigned int *r = m->r;
    unsigned int a, v;
    unsigned char *p;
    long long prod;

    if (op == 0) {
        switch (fn) {
        case 0x00: v = r[rt] << sh; break;                                     /* sll */
        case 0x02: v = r[rt] >> sh; break;                                     /* srl */
        case 0x03: v = (unsigned int)((int)r[rt] >> sh); break;                /* sra */
        case 0x04: v = r[rt] << (r[rs] & 31); break;                           /* sllv */
        case 0x06: v = r[rt] >> (r[rs] & 31); break;                           /* srlv */
        case 0x07: v = (unsigned int)((int)r[rt] >> (r[rs] & 31)); break;      /* srav */
        case 0x10: v = m->hi; break;                                           /* mfhi */
        case 0x12: v = m->lo; break;                                           /* mflo */
        case 0x11: m->hi = r[rs]; return 0;                                    /* mthi */
        case 0x13: m->lo = r[rs]; return 0;                                    /* mtlo */
        case 0x18:                                                             /* mult: 32x32 -> 64, signed */
            prod = (long long)(int)r[rs] * (long long)(int)r[rt];
            m->lo = (unsigned int)prod; m->hi = (unsigned int)((unsigned long long)prod >> 32);
            return 0;
        case 0x19: {                                                           /* multu */
            unsigned long long up = (unsigned long long)r[rs] * r[rt];
            m->lo = (unsigned int)up; m->hi = (unsigned int)(up >> 32);
            return 0;
        }
        case 0x1A:                                                             /* div (quotient lo, remainder hi) */
            if (r[rt] == 0 || ((int)r[rs] == (int)0x80000000 && (int)r[rt] == -1)) { return fail(m, "div by zero or overflow", pc, w); }
            m->lo = (unsigned int)((int)r[rs] / (int)r[rt]); m->hi = (unsigned int)((int)r[rs] % (int)r[rt]);
            return 0;
        case 0x1B:                                                             /* divu */
            if (r[rt] == 0) { return fail(m, "divu by zero", pc, w); }
            m->lo = r[rs] / r[rt]; m->hi = r[rs] % r[rt];
            return 0;
        case 0x20: case 0x21: v = r[rs] + r[rt]; break;                        /* add, addu (add's overflow trap is not modelled) */
        case 0x22: case 0x23: v = r[rs] - r[rt]; break;                        /* sub, subu */
        case 0x24: v = r[rs] & r[rt]; break;                                   /* and */
        case 0x25: v = r[rs] | r[rt]; break;                                   /* or */
        case 0x26: v = r[rs] ^ r[rt]; break;                                   /* xor */
        case 0x27: v = ~(r[rs] | r[rt]); break;                                /* nor */
        case 0x2A: v = (int)r[rs] < (int)r[rt]; break;                         /* slt */
        case 0x2B: v = r[rs] < r[rt]; break;                                   /* sltu */
        default: return fail(m, "unsupported SPECIAL instruction", pc, w);
        }
        if (rd) { r[rd] = v; }
        return 0;
    }
    switch (op) {
    case 0x08: case 0x09: v = r[rs] + sx16(imm); break;                        /* addi, addiu */
    case 0x0A: v = (int)r[rs] < (int)sx16(imm); break;                         /* slti */
    case 0x0B: v = r[rs] < sx16(imm); break;                                   /* sltiu */
    case 0x0C: v = r[rs] & imm; break;                                         /* andi */
    case 0x0D: v = r[rs] | imm; break;                                         /* ori */
    case 0x0E: v = r[rs] ^ imm; break;                                         /* xori */
    case 0x0F: v = imm << 16; break;                                           /* lui */
    case 0x20: a = r[rs] + sx16(imm); p = at(m, a, 1); if (!p) { return fail(m, "lb outside memory", pc, w); }
        v = (unsigned int)(int)(signed char)p[0]; break;                       /* lb */
    case 0x24: a = r[rs] + sx16(imm); p = at(m, a, 1); if (!p) { return fail(m, "lbu outside memory", pc, w); }
        v = p[0]; break;                                                       /* lbu */
    case 0x21: case 0x25: a = r[rs] + sx16(imm); if (a & 1) { return fail(m, "unaligned lh/lhu", pc, w); }
        p = at(m, a, 2); if (!p) { return fail(m, "lh outside memory", pc, w); }
        v = ((unsigned int)p[0] << 8) | p[1]; if (op == 0x21) { v = sx16(v); } break;
    case 0x23: a = r[rs] + sx16(imm); if (a & 3) { return fail(m, "unaligned lw", pc, w); }
        p = at(m, a, 4); if (!p) { return fail(m, "lw outside memory", pc, w); }
        v = ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3]; break;
    case 0x28: a = r[rs] + sx16(imm); p = at(m, a, 1); if (!p) { return fail(m, "sb outside memory", pc, w); }
        p[0] = (unsigned char)r[rt]; return 0;
    case 0x29: a = r[rs] + sx16(imm); if (a & 1) { return fail(m, "unaligned sh", pc, w); }
        p = at(m, a, 2); if (!p) { return fail(m, "sh outside memory", pc, w); }
        p[0] = (unsigned char)(r[rt] >> 8); p[1] = (unsigned char)r[rt]; return 0;
    case 0x2B: a = r[rs] + sx16(imm); if (a & 3) { return fail(m, "unaligned sw", pc, w); }
        p = at(m, a, 4); if (!p) { return fail(m, "sw outside memory", pc, w); }
        p[0] = (unsigned char)(r[rt] >> 24); p[1] = (unsigned char)(r[rt] >> 16); p[2] = (unsigned char)(r[rt] >> 8); p[3] = (unsigned char)r[rt];
        return 0;
    default: return fail(m, "unsupported instruction", pc, w);
    }
    if (rt) { r[rt] = v; }
    return 0;
}

int mips_ref_call(MipsRef *m, unsigned int entry, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3,
                  unsigned int sp, unsigned int *v0)
{
    m->error[0] = 0;
    m->r[4] = a0; m->r[5] = a1; m->r[6] = a2; m->r[7] = a3;
    m->r[29] = sp;
    m->r[31] = SENTINEL;
    m->pc = entry;

    while (m->steps < m->max_steps) {
        unsigned int pc = m->pc, w, op, rs, rt, fn;
        unsigned char *p = at(m, pc, 4);
        int taken = 0, link = 0, likely = 0, is_jump = 0;
        unsigned int target = 0;

        if (!p || (pc & 3)) { return fail(m, "pc outside memory or unaligned", pc, 0); }
        w = ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
        op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; fn = w & 63;
        m->steps++;

        if (op == 2 || op == 3) {                                                  /* j, jal */
            is_jump = 1; taken = 1; link = (op == 3);
            target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
        } else if (op == 0 && (fn == 8 || fn == 9)) {                              /* jr, jalr */
            is_jump = 1; taken = 1; link = (fn == 9);
            target = m->r[rs];
        } else if (op == 4 || op == 5 || op == 0x14 || op == 0x15) {               /* beq, bne, beql, bnel */
            is_jump = 1; likely = op >= 0x14;
            taken = ((op & 1) ? (m->r[rs] != m->r[rt]) : (m->r[rs] == m->r[rt]));
            target = pc + 4 + (sx16(w) << 2);
        } else if (op == 6 || op == 7 || op == 0x16 || op == 0x17) {               /* blez, bgtz, blezl, bgtzl */
            is_jump = 1; likely = op >= 0x14;
            taken = ((op & 1) ? ((int)m->r[rs] > 0) : ((int)m->r[rs] <= 0));
            target = pc + 4 + (sx16(w) << 2);
        } else if (op == 1) {                                                      /* REGIMM: bltz bgez bltzl bgezl bltzal bgezal */
            is_jump = 1;
            switch (rt) {
            case 0x00: case 0x02: taken = (int)m->r[rs] < 0; likely = (rt == 2); break;
            case 0x01: case 0x03: taken = (int)m->r[rs] >= 0; likely = (rt == 3); break;
            case 0x10: taken = (int)m->r[rs] < 0; link = 1; break;
            case 0x11: taken = (int)m->r[rs] >= 0; link = 1; break;
            default: return fail(m, "unsupported REGIMM instruction", pc, w);
            }
            target = pc + 4 + (sx16(w) << 2);
        }

        if (!is_jump) {
            if (exec_plain(m, pc, w)) { return -1; }
            m->pc = pc + 4;
            continue;
        }
        {
            unsigned int slot_pc = pc + 4, slot_w, saved_ra_value = pc + 8;
            unsigned char *sp_ = at(m, slot_pc, 4);
            if (!sp_) { return fail(m, "delay slot outside memory", slot_pc, 0); }
            slot_w = ((unsigned int)sp_[0] << 24) | ((unsigned int)sp_[1] << 16) | ((unsigned int)sp_[2] << 8) | sp_[3];
            /* The value a branch reads from a register is read BEFORE the delay slot runs (jalr/jr use rs of the jump itself); `target`
             * above was computed first, so a delay slot that changes rs does not alter it. The link register is written before the
             * delay slot runs, as on the hardware. */
            if (link) { m->r[(op == 0) ? ((w >> 11) & 31) ? ((w >> 11) & 31) : 31 : 31] = saved_ra_value; }
            if (likely && !taken) {                                                /* branch-likely: delay slot skipped when not taken */
                m->pc = pc + 8;
                continue;
            }
            {
                unsigned int sop = slot_w >> 26, sfn = slot_w & 63;
                if ((sop >= 1 && sop <= 7) || (sop >= 0x14 && sop <= 0x17) || (sop == 0 && (sfn == 8 || sfn == 9))) {
                    return fail(m, "a jump or branch in a delay slot", slot_pc, slot_w);
                }
            }
            m->steps++;
            if (exec_plain(m, slot_pc, slot_w)) { return -1; }
            if (!taken) { m->pc = pc + 8; continue; }
            if (target == SENTINEL) {
                if (v0) { *v0 = m->r[2]; }
                return 0;
            }
            m->pc = target;
        }
    }
    return fail(m, "step limit reached", m->pc, 0);
}
