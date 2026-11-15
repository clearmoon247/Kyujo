/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * rsp_cpu.c - an interpreter for the console's signal processor (RSP): runs the microcode that the game hands to it, instruction by
 * instruction. Used for the AUDIO task, whose microcode (aspMain, in the ROM) is the only authority on how the game's audio command list
 * turns into samples. (Graphics tasks are done at a higher level, src/render/; the JPEG task likewise.)
 *
 * HOST-side file (plain C, no game headers). The RSP's memories are the arrays owned by src/ultra/hw_sp.c; this file is given pointers
 * to them. Both memories are BYTE arrays in the console's big-endian order, exactly as the DMA engine left them.
 *
 * WHAT IS IMPLEMENTED, AND WHY EXACTLY THIS SET
 *   tools/rsp/rsp_dis.py counted the instructions of the ROM's aspMain (US ROM, 0xC60 bytes of text): 24 scalar instructions and 20
 *   vector-unit ones, all listed below. The Game Boy Tower emulator's two microcodes (decomp: D_812297C0 and D_8122A260, scanned by opcode in
 *   this session) add XORI, SLTI/SLTIU (SLTI/SLTIU/XORI are plain MIPS and not covered by a test yet), VEQ, VNE, VMRG, VOR (all four
 *   checked against cxd4 in tests/native/rsp) and LPV/SPV (element 0 only, any alignment as in cxd4; hand-computed test in ls_test.c). Every instruction outside that set stops the program with a message naming it. Nothing else is
 *   guessed: if another microcode is ever run through this interpreter, the first instruction that is missing is reported, and has to be
 *   added together with a check.
 *
 * SOURCES AND CONFIDENCE
 *   - Scalar unit: MIPS I semantics without exceptions, 12-bit PC, 12-bit data addresses with wrap-around, unaligned loads and stores
 *     allowed (n64brew, "Reality Signal Processor/CPU Core": "scalar loads and stores work at any byte address"; "only the low 12 bits ... of a
 *     load/store address are used"; BREAK special). No likely branches, no multiply/divide.
 *   - Vector loads/stores: n64brew's table gives the opcode numbers and the sizes (LSV 2 bytes, LDV 8, LQV up to the next 16-byte boundary,
 *     LRV from the previous boundary up to the address; SSV/SDV/SQV the stores, "element wrap: SLV with element=15 stores bytes 15,0,1,2").
 *     That page does not spell out the LQV/LRV arithmetic for a non-zero element; the code below does what the pair is for (together they load
 *     an unaligned quad) and marks the element != 0 case.
 *   - Vector arithmetic: the products, accumulator updates and clamping rules of VMULF/VMACF/VMUDL/VMUDM/VMUDN/VMUDH/VMADL/VMADM/VMADN/VMADH,
 *     VADD/VSUB/VADDC, VAND/VXOR, VLT/VGE and VSAR were first written from memory of the published RSP descriptions. They are now CHECKED
 *     by tests/native/rsp/diff_vs_cxd4.c: 100000 random trials per instruction (random registers and accumulators with edge values, all 16
 *     element fields, random VCO/VCC/VCE) compared lane by lane, accumulator and flags, with the vector unit of mupen64plus-rsp-cxd4 (CC0).
 *     Result: identical, after two corrections found by that test: VLT/VGE leave VCE alone (cxd4 and ares agree), and the VMADL/VMADN result
 *     clamp follows ares' accumulatorSaturate (cxd4's own macro disagrees with ares in a corner case that needs the middle accumulator word
 *     to be exactly 0x7FFF/0x8000; this interpreter and ares agree). Neither reference is the console; they are independent emulators.
 *   - Vector loads/stores: the forms the ROM's microcode really uses (found by tracing it) are covered by tests/native/rsp/ls_test.c
 *     (hand-computed expectations); forms it does not use (e.g. LRV with element != 0, SSV element 15) are marked in the code as unchecked.
 *   - Scalar unit: ordinary MIPS; exercised by the microcode itself (a wrong branch or address would derail the task, which ends within
 *     about 20000 instructions every time).
 *
 * NOT MODELLED: the RSP's timing (a task runs to its BREAK at once), DMA timing (a DMA completes immediately), the display-list/RDP side
 * (COP0 registers 8..15), vector instructions that need the reciprocal ROM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifdef RDP_SHELL_NATIVE
/* In the sandboxed core this file is compiled into the native shell and runs outside the wasm module and outside its lock (shell/core/rdp_glue.c):
 * the microcode's memories and the game's RAM are all inside the module's linear memory. */
#include "rdram.h"
void host_log(const char *message);
void host_fatal(const char *message) __attribute__((noreturn));
#else
#include "platform.h"
#endif
#include "sp_shared.h"

typedef struct {
    uint32_t gpr[32];
    _Alignas(16) uint8_t vpr[32][16];            /* vector registers as bytes in big-endian order: lane i is bytes 2i (high) and 2i+1 (low) */
    int64_t acc[8];                 /* the 48-bit accumulator of each lane, kept sign-extended to 64 bits */
    uint16_t vco, vcc, vce;         /* vector control registers: VCO = carry (bits 0..7) and not-equal (bits 8..15) per lane */
    uint32_t pc;                    /* offset in IMEM, 0..0xFFC */
    /* the SP registers that COP0 reads and writes (c0 0..7) */
    uint32_t mem_addr, dram_addr, status, semaphore;
    uint32_t dpc_status;            /* read-only copy of DPC_STATUS taken when the task starts */
    uint32_t irq_req;               /* bit 0: acknowledge the SP interrupt, bit 1: raise it (applied by the caller after the run) */
    uint8_t *dmem, *imem;
    unsigned long long steps;
    /* Cooperative yielding (see host_rsp_run): the microcode polled SP_STATUS twice and saw the same value, so only the CPU can change what
     * it waits for. poll_valid/poll_value remember the last SP_STATUS value a poll saw; any MTC0 forgets them. */
    int yielded, poll_valid;
    uint32_t poll_value;
} Rsp;

static Rsp R;
/* DPC_CLOCK (COP0 register 12): RSP instructions executed by all tasks before the current one. See cop0_read. */
static unsigned long long sClockBase;
static int sTraceLeft;           /* instructions still to print (KYUJO_RSP_TRACE) */

/* ---- helpers ---------------------------------------------------------------------------------------------------------------------- */

static void rsp_unsupported(const char *what, uint32_t pc, uint32_t word)
{
    char msg[200];
    snprintf(msg, sizeof msg, "RSP interpreter: unsupported %s at IMEM 0x%03X (instruction word 0x%08X)", what, pc, word);
    host_fatal(msg);
}

/* Vector registers hold their bytes in the RSP's (big-endian) order; a lane is two bytes, high byte first. SPEED: on a little-endian host
 * a lane is read and written with one 16-bit access and a byte swap instead of two byte accesses (the lane helpers were about a fifth of the
 * interpreter's time). Same values either way. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && (defined(__GNUC__) || defined(__clang__))
static uint16_t lane_u(int v, int i)
{
    uint16_t x;
    memcpy(&x, &R.vpr[v][2 * i], 2);
    x = __builtin_bswap16(x);
#ifdef PS_RSP_VERIFY      /* development (-DPS_RSP_VERIFY): the fast access must equal the plain byte composition */
    if (x != (uint16_t)(((uint16_t)R.vpr[v][2 * i] << 8) | R.vpr[v][2 * i + 1])) { fprintf(stderr, "RSP VERIFY: lane mismatch\n"); abort(); }
#endif
    return x;
}
static int16_t lane_s(int v, int i) { return (int16_t)lane_u(v, i); }
static void set_lane(int v, int i, uint16_t x)
{
    uint16_t sw = __builtin_bswap16(x);
    memcpy(&R.vpr[v][2 * i], &sw, 2);
#ifdef PS_RSP_VERIFY      /* development: the two bytes must be the high byte first, as the plain version writes them */
    if (R.vpr[v][2 * i] != (uint8_t)(x >> 8) || R.vpr[v][2 * i + 1] != (uint8_t)x) { fprintf(stderr, "RSP VERIFY: set_lane mismatch\n"); abort(); }
#endif
}
#else
static uint16_t lane_u(int v, int i) { return (uint16_t)(((uint16_t)R.vpr[v][2 * i] << 8) | R.vpr[v][2 * i + 1]); }
static int16_t lane_s(int v, int i) { return (int16_t)lane_u(v, i); }
static void set_lane(int v, int i, uint16_t x) { R.vpr[v][2 * i] = (uint8_t)(x >> 8); R.vpr[v][2 * i + 1] = (uint8_t)x; }
#endif

static int16_t clamp_s16(int64_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : (int16_t)x); }

/* The accumulator is 48 bits: keep it sign-extended after every update. */
static int64_t wrap48(int64_t x) { return (x << 16) >> 16; }

/* "clamp signed": the result of the multiply-accumulate instructions that return the upper two thirds of the accumulator: acc[47:16],
 * saturated to a signed 16-bit value. */
static uint16_t acc_clamp_signed(int i) { return (uint16_t)clamp_s16(R.acc[i] >> 16); }

/* "clamp low": the result of VMUDN/VMADN/VMADL-style instructions: the low 16 bits of the accumulator, unless the accumulator no longer fits
 * in a signed 32-bit value, in which case it saturates to 0xFFFF (positive) or 0x0000 (negative). */
static uint16_t acc_clamp_low(int i)
{
    int64_t a = R.acc[i];
    if (a >= -(int64_t)0x80000000LL && a <= 0x7FFFFFFFLL) return (uint16_t)a;
    return a > 0 ? 0xFFFF : 0x0000;
}

/* Which lane of VT the lane `i` uses for the element specifier `e` (broadcast modifier): 0,1 none; 2,3 pairs; 4..7 quarters; 8..15 one lane. */
static int vt_lane(int e, int i)
{
    switch (e) {
    case 0: case 1: return i;
    case 2: return i & ~1;
    case 3: return (i & ~1) | 1;
    case 4: case 5: case 6: case 7: return (i & ~3) | (e - 4);
    default: return e - 8;
    }
}

/* ---- COP0: the SP registers ---------------------------------------------------------------------------------------------------- */

static uint32_t cop0_read(int reg, uint32_t pc, uint32_t word)
{
    switch (reg) {
    case 0: return R.mem_addr;
    case 1: return R.dram_addr;
    case 2: case 3: return 0;                       /* DMA length registers: a DMA completes at once, nothing is left */
    case 4: return R.status;
    case 5: case 6: return 0;                       /* DMA full / busy: never */
    case 7: { uint32_t v = R.semaphore; R.semaphore = 1; return v; }
    case 11: return R.dpc_status;                   /* DPC_STATUS: only READ here (the audio microcode looks at it at its start); see hw_sp.c */
    /* DPC_CLOCK: ASSUMPTION. What the register counts is not documented where it was looked up (n64brew's RDP interface page lists
     * "CLOCK[23:0]: ?"; it is 24 bits wide and read-only). The GB Tower's two microcodes (ROM 0xA9138 and 0xA9BD8) read it as their
     * first action and one of them subtracts it from a later value (a self-timing measurement, as far as the instruction stream
     * shows; not traced further). It is modelled as a free-running counter of executed RSP instructions, which is deterministic and
     * makes such a difference a plausible cycle count. Whether anything depends on the exact numbers is NOT known.
     * Registers 13..15 (BUFBUSY, PIPEBUSY, TMEM) are still refused: nothing known reads them. */
    case 12: return (uint32_t)(sClockBase + R.steps) & 0x00FFFFFFu;
    default: rsp_unsupported("read of COP0 register (RDP command registers other than DPC_STATUS and DPC_CLOCK)", pc, word);
    }
}

static void cop0_write(int reg, uint32_t v, uint32_t pc, uint32_t word)
{
    switch (reg) {
    case 0: R.mem_addr = v; break;
    case 1: R.dram_addr = v; break;
    case 2: sp_dma_copy(R.dmem, R.imem, &R.mem_addr, &R.dram_addr, v, 1); break;
    case 3: sp_dma_copy(R.dmem, R.imem, &R.mem_addr, &R.dram_addr, v, 0); break;
    case 4: {
        uint32_t req;
        R.status = sp_status_write(R.status, v, &req);
        R.irq_req |= req;
        break;
    }
    case 7: R.semaphore = 0; break;
    default: rsp_unsupported("write of COP0 register (RDP command registers)", pc, word);
    }
}

/* ---- scalar memory access (12-bit addresses, wrapping, any alignment) ----------------------------------------------------------- */

static uint32_t ld8(uint32_t a) { return R.dmem[a & 0xFFF]; }
static void st8(uint32_t a, uint32_t v) { R.dmem[a & 0xFFF] = (uint8_t)v; }
/* SPEED: an access that does not cross the end of DMEM (the address wraps at 4096) is one memory access and a byte swap; the byte-by-byte forms stay for the
 * rare access that wraps, and for hosts that are not little-endian. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && (defined(__GNUC__) || defined(__clang__))
static uint32_t ld16(uint32_t a)
{
    a &= 0xFFF;
    if (a <= 0xFFE) { uint16_t x; memcpy(&x, R.dmem + a, 2); return __builtin_bswap16(x); }
    return ((uint32_t)R.dmem[a] << 8) | R.dmem[0];
}
static uint32_t ld32(uint32_t a)
{
    a &= 0xFFF;
    if (a <= 0xFFC) { uint32_t x; memcpy(&x, R.dmem + a, 4); return __builtin_bswap32(x); }
    return ((uint32_t)R.dmem[a] << 24) | ((uint32_t)R.dmem[(a + 1) & 0xFFF] << 16) | ((uint32_t)R.dmem[(a + 2) & 0xFFF] << 8) | R.dmem[(a + 3) & 0xFFF];
}
static void st16(uint32_t a, uint32_t v)
{
    a &= 0xFFF;
    if (a <= 0xFFE) { uint16_t x = __builtin_bswap16((uint16_t)v); memcpy(R.dmem + a, &x, 2); return; }
    R.dmem[a] = (uint8_t)(v >> 8); R.dmem[0] = (uint8_t)v;
}
static void st32(uint32_t a, uint32_t v)
{
    a &= 0xFFF;
    if (a <= 0xFFC) { uint32_t x = __builtin_bswap32(v); memcpy(R.dmem + a, &x, 4); return; }
    st8(a, v >> 24); st8(a + 1, v >> 16); st8(a + 2, v >> 8); st8(a + 3, v);
}
#else
static uint32_t ld16(uint32_t a) { return (ld8(a) << 8) | ld8(a + 1); }
static uint32_t ld32(uint32_t a) { return (ld16(a) << 16) | ld16(a + 2); }
static void st16(uint32_t a, uint32_t v) { st8(a, v >> 8); st8(a + 1, v); }
static void st32(uint32_t a, uint32_t v) { st16(a, v >> 16); st16(a + 2, v); }
#endif

/* ---- vector loads and stores (LWC2 / SWC2) -------------------------------------------------------------------------------------- */

/* `e` is the element field (bits 10..7): the first byte of the vector register that the access touches. */
static void vload(uint32_t word, uint32_t pc)
{
    int base = (word >> 21) & 31, vt = (word >> 16) & 31, op = (word >> 11) & 31, e = (word >> 7) & 15;
    int32_t off = (int32_t)((word & 0x7F) << 25) >> 25;            /* signed 7-bit, scaled by the access size */
    uint32_t addr;
    int i, n;

    switch (op) {
    case 1:  /* LSV: 2 bytes -> bytes e, e+1 (bytes past 15 are dropped, as n64brew says for the scalar loads) */
        addr = R.gpr[base] + off * 2;
        for (i = 0; i < 2; i++) if (e + i < 16) R.vpr[vt][e + i] = (uint8_t)ld8(addr + i);
        break;
    case 3:  /* LDV: 8 bytes -> bytes e .. e+7 */
        addr = R.gpr[base] + off * 8;
        if (e <= 8 && (addr & 0xFFF) <= 0xFF8) { memcpy(&R.vpr[vt][e], R.dmem + (addr & 0xFFF), 8); break; }      /* the usual case: everything fits, no wrap */
        for (i = 0; i < 8; i++) if (e + i < 16) R.vpr[vt][e + i] = (uint8_t)ld8(addr + i);
        break;
    case 4:  /* LQV: from the address up to the next 16-byte boundary, into bytes e .. 15 (whichever ends first) */
        addr = R.gpr[base] + off * 16;
        n = 16 - (int)(addr & 15);
        if (n > 16 - e) n = 16 - e;
        if ((addr & 0xFFF) + (uint32_t)n <= 0x1000) { memcpy(&R.vpr[vt][e], R.dmem + (addr & 0xFFF), (size_t)n); break; }   /* no wrap at the end of DMEM */
        for (i = 0; i < n; i++) R.vpr[vt][e + i] = (uint8_t)ld8(addr + i);
        break;
    case 5: {/* LRV: from the previous 16-byte boundary up to the address (exclusive), right-aligned: it fills the bytes that LQV (at the same
              * address) left alone. n bytes land in register bytes 16-n .. 15. With e != 0 the destination moves up by e (bytes past 15 are
              * dropped): this is how the pair is used for unaligned quads and is NOT checked against hardware; the ROM's microcode is
              * expected to use e = 0 (the self-test that runs the microcode reports if it does not). */
        uint32_t start;
        addr = R.gpr[base] + off * 16;
        n = (int)(addr & 15);
        start = addr & ~15u;
        for (i = 0; i < n; i++) {
            int dst = 16 - n + i + e;
            if (dst < 16) R.vpr[vt][dst] = (uint8_t)ld8(start + i);
        }
        break;
    }
    case 6:  /* LPV: 8 bytes -> the HIGH byte of lanes 0..7 (low bytes zero), address = base + offset*8 (Dillonb/n64-resources rsp.md: "Signed
              * opcodes (LPV, SPV) map the value to bits (15..8)"). Only element 0 is done (cxd4 also refuses other elements: "Illegal
              * element"). The address need NOT be 8-byte aligned: cxd4 (su.c, LPV) handles every alignment 0..7 by reading the eight bytes
              * one after another from the unaligned address, wrapping at the end of DMEM (its case comments name commercial games that do
              * this); that is what is done here. NOT checked against hardware by me, only against that reference. */
        addr = R.gpr[base] + off * 8;
        if (e != 0) rsp_unsupported("LPV with a non-zero element", pc, word);
        for (i = 0; i < 8; i++) { R.vpr[vt][2 * i] = (uint8_t)ld8(addr + i); R.vpr[vt][2 * i + 1] = 0; }
        break;
    default:
        rsp_unsupported("vector load (LWC2) variant", pc, word);
    }
}

static void vstore(uint32_t word, uint32_t pc)
{
    int base = (word >> 21) & 31, vt = (word >> 16) & 31, op = (word >> 11) & 31, e = (word >> 7) & 15;
    int32_t off = (int32_t)((word & 0x7F) << 25) >> 25;
    uint32_t addr;
    int i, n;

    switch (op) {
    case 1:  /* SSV: bytes e, e+1 (wrapping inside the register, n64brew: "SLV with element=15 stores VPR bytes 15, 0, 1, 2") */
        addr = R.gpr[base] + off * 2;
        for (i = 0; i < 2; i++) st8(addr + i, R.vpr[vt][(e + i) & 15]);
        break;
    case 3:  /* SDV: 8 bytes from byte e, wrapping */
        addr = R.gpr[base] + off * 8;
        if (e <= 8 && (addr & 0xFFF) <= 0xFF8) { memcpy(R.dmem + (addr & 0xFFF), &R.vpr[vt][e], 8); break; }      /* no wrap in the register or in DMEM */
        for (i = 0; i < 8; i++) st8(addr + i, R.vpr[vt][(e + i) & 15]);
        break;
    case 4:  /* SQV: from register byte e (wrapping) to the next 16-byte boundary of the address */
        addr = R.gpr[base] + off * 16;
        n = 16 - (int)(addr & 15);
        if (e == 0 && (addr & 0xFFF) + (uint32_t)n <= 0x1000) { memcpy(R.dmem + (addr & 0xFFF), R.vpr[vt], (size_t)n); break; }
        for (i = 0; i < n; i++) st8(addr + i, R.vpr[vt][(e + i) & 15]);
        break;
    case 6:  /* SPV: the HIGH byte of lanes 0..7 -> 8 bytes (the low bytes are ignored). Same limits as LPV: element 0 only; any alignment,
              * written byte after byte from the unaligned address with wrap-around (cxd4 su.c, SPV). */
        addr = R.gpr[base] + off * 8;
        if (e != 0) rsp_unsupported("SPV with a non-zero element", pc, word);
        for (i = 0; i < 8; i++) st8(addr + i, R.vpr[vt][2 * i]);
        break;
    default:
        rsp_unsupported("vector store (SWC2) variant", pc, word);
    }
}

/* ---- vector arithmetic (COP2 with bit 25 set) ------------------------------------------------------------------------------------ */

__attribute__((unused)) static void vector_op_scalar(uint32_t word, uint32_t pc)
{
    int e = (word >> 21) & 15, vt = (word >> 16) & 31, vs = (word >> 11) & 31, vd = (word >> 6) & 31, op = word & 63;
    int i;
    uint16_t res[8];
    int16_t tmp_s[8];
    uint16_t tmp_u[8];

    /* Snapshot of VT as seen through the element specifier (VD may be the same register as VS or VT). */
    for (i = 0; i < 8; i++) { tmp_u[i] = lane_u(vt, vt_lane(e, i)); tmp_s[i] = (int16_t)tmp_u[i]; }

    switch (op) {
    case 0:  /* VMULF: acc = 2*vs*vt + 0x8000; result = saturate(acc >> 16) */
        for (i = 0; i < 8; i++) { R.acc[i] = wrap48((int64_t)lane_s(vs, i) * tmp_s[i] * 2 + 0x8000); res[i] = acc_clamp_signed(i); }
        break;
    case 8:  /* VMACF: acc += 2*vs*vt; result = saturate(acc >> 16) */
        for (i = 0; i < 8; i++) { R.acc[i] = wrap48(R.acc[i] + (int64_t)lane_s(vs, i) * tmp_s[i] * 2); res[i] = acc_clamp_signed(i); }
        break;
    case 4:  /* VMUDL: acc = (vs_unsigned * vt_unsigned) >> 16; result = low accumulator */
        for (i = 0; i < 8; i++) { R.acc[i] = (int64_t)(((uint32_t)lane_u(vs, i) * tmp_u[i]) >> 16); res[i] = acc_clamp_low(i); }
        break;
    case 5:  /* VMUDM: acc = vs_signed * vt_unsigned; result = saturate(acc >> 16) */
        for (i = 0; i < 8; i++) { R.acc[i] = (int64_t)lane_s(vs, i) * tmp_u[i]; res[i] = acc_clamp_signed(i); }
        break;
    case 6:  /* VMUDN: acc = vs_unsigned * vt_signed; result = low accumulator */
        for (i = 0; i < 8; i++) { R.acc[i] = (int64_t)lane_u(vs, i) * tmp_s[i]; res[i] = acc_clamp_low(i); }
        break;
    case 7:  /* VMUDH: acc = (vs_signed * vt_signed) << 16; result = saturate(acc >> 16) */
        for (i = 0; i < 8; i++) { R.acc[i] = wrap48(((int64_t)lane_s(vs, i) * tmp_s[i]) << 16); res[i] = acc_clamp_signed(i); }
        break;
    case 12: /* VMADL: acc += (vs_unsigned * vt_unsigned) >> 16; result = clamp low */
        for (i = 0; i < 8; i++) { R.acc[i] = wrap48(R.acc[i] + (int64_t)(((uint32_t)lane_u(vs, i) * tmp_u[i]) >> 16)); res[i] = acc_clamp_low(i); }
        break;
    case 13: /* VMADM: acc += vs_signed * vt_unsigned; result = saturate(acc >> 16) */
        for (i = 0; i < 8; i++) { R.acc[i] = wrap48(R.acc[i] + (int64_t)lane_s(vs, i) * tmp_u[i]); res[i] = acc_clamp_signed(i); }
        break;
    case 14: /* VMADN: acc += vs_unsigned * vt_signed; result = clamp low */
        for (i = 0; i < 8; i++) { R.acc[i] = wrap48(R.acc[i] + (int64_t)lane_u(vs, i) * tmp_s[i]); res[i] = acc_clamp_low(i); }
        break;
    case 15: /* VMADH: acc += (vs_signed * vt_signed) << 16; result = saturate(acc >> 16) */
        for (i = 0; i < 8; i++) { R.acc[i] = wrap48(R.acc[i] + (((int64_t)lane_s(vs, i) * tmp_s[i]) << 16)); res[i] = acc_clamp_signed(i); }
        break;
    case 16: /* VADD: result = saturate(vs + vt + carry); the low accumulator gets the unsaturated sum; VCO is cleared */
        for (i = 0; i < 8; i++) {
            int64_t sum = (int64_t)lane_s(vs, i) + tmp_s[i] + ((R.vco >> i) & 1);
            R.acc[i] = (R.acc[i] & ~(int64_t)0xFFFF) | (uint16_t)sum;
            res[i] = (uint16_t)clamp_s16(sum);
        }
        R.vco = 0;
        break;
    case 17: /* VSUB: result = saturate(vs - vt - carry); low accumulator = the unsaturated difference; VCO cleared */
        for (i = 0; i < 8; i++) {
            int64_t d = (int64_t)lane_s(vs, i) - tmp_s[i] - ((R.vco >> i) & 1);
            R.acc[i] = (R.acc[i] & ~(int64_t)0xFFFF) | (uint16_t)d;
            res[i] = (uint16_t)clamp_s16(d);
        }
        R.vco = 0;
        break;
    case 20: /* VADDC: unsigned add, carry out into VCO bit i (not-equal bits cleared); result = low 16 bits */
        R.vco = 0;
        for (i = 0; i < 8; i++) {
            uint32_t sum = (uint32_t)lane_u(vs, i) + tmp_u[i];
            R.acc[i] = (R.acc[i] & ~(int64_t)0xFFFF) | (uint16_t)sum;
            res[i] = (uint16_t)sum;
            if (sum >> 16) R.vco |= (uint16_t)(1u << i);
        }
        break;
    case 29: /* VSAR: vd = one third of the accumulator, chosen by the element field: 8 = bits 47:32, 9 = 31:16, 10 = 15:0.
              * The accumulator is not changed. (Other element values: not used by the ROM's microcode.) */
        if (e < 8 || e > 10) rsp_unsupported("VSAR with an element outside 8..10", pc, word);
        for (i = 0; i < 8; i++) {
            int shift = (e == 8) ? 32 : (e == 9 ? 16 : 0);
            res[i] = (uint16_t)(R.acc[i] >> shift);
        }
        break;
    case 32: /* VLT: lanes where vs < vt (or equal while VCO says carry and not-equal... see below) take vs, the others vt */
        {
            uint16_t cc = 0;
            for (i = 0; i < 8; i++) {
                int16_t a = lane_s(vs, i), b = tmp_s[i];
                int eq_special = ((R.vco >> i) & 1) && ((R.vco >> (8 + i)) & 1);      /* carry AND not-equal */
                int lt = (a < b) || (a == b && eq_special);
                res[i] = (uint16_t)(lt ? a : b);
                if (lt) cc |= (uint16_t)(1u << i);
                R.acc[i] = (R.acc[i] & ~(int64_t)0xFFFF) | res[i];
            }
            R.vcc = cc; R.vco = 0;                 /* VCE is left alone (checked against two independent emulators: tests/native/rsp) */
        }
        break;
    case 35: /* VGE: lanes where vs > vt (or equal, unless VCO says carry and not-equal) take vs, the others vt */
        {
            uint16_t cc = 0;
            for (i = 0; i < 8; i++) {
                int16_t a = lane_s(vs, i), b = tmp_s[i];
                int eq_special = ((R.vco >> i) & 1) && ((R.vco >> (8 + i)) & 1);
                int ge = (a > b) || (a == b && !eq_special);
                res[i] = (uint16_t)(ge ? a : b);
                if (ge) cc |= (uint16_t)(1u << i);
                R.acc[i] = (R.acc[i] & ~(int64_t)0xFFFF) | res[i];
            }
            R.vcc = cc; R.vco = 0;                 /* VCE is left alone (checked against two independent emulators: tests/native/rsp) */
        }
        break;
    case 33: /* VEQ: lane is "selected" when vs == vt and the not-equal flag is clear; selected lanes take vs, the others vt (vt == vs where
              * the values are equal, so the result is vt wherever the lane is not selected); VCC = the selection (clip half cleared), VCO cleared */
    case 34: /* VNE: selected when vs != vt OR the not-equal flag is set (so selected lanes take vs); same result and flag rules as VEQ */
        {
            uint16_t cc = 0;
            for (i = 0; i < 8; i++) {
                int16_t a = lane_s(vs, i), b = tmp_s[i];
                int ne_flag = (R.vco >> (8 + i)) & 1;
                int sel = (op == 33) ? (a == b && !ne_flag) : (a != b || ne_flag);
                res[i] = (uint16_t)(sel ? a : b);
                if (sel) cc |= (uint16_t)(1u << i);
                R.acc[i] = (R.acc[i] & ~(int64_t)0xFFFF) | res[i];
            }
            R.vcc = cc; R.vco = 0;
        }
        break;
    case 39: /* VMRG: lanes selected in VCC (low half) take vs, the others vt; VCC, VCO and VCE are all left alone (github.com/Dillonb/n64-resources
              * rsp.md: "The values of VCC, VCO, and VCE remain unchanged"; mupen64plus-rsp-cxd4 agrees, see tests/native/rsp/diff_vs_cxd4.c) */
        for (i = 0; i < 8; i++) {
            res[i] = (uint16_t)(((R.vcc >> i) & 1) ? lane_u(vs, i) : tmp_u[i]);
            R.acc[i] = (R.acc[i] & ~(int64_t)0xFFFF) | res[i];
        }
        break;
    case 42: /* VOR */
        for (i = 0; i < 8; i++) { res[i] = (uint16_t)(lane_u(vs, i) | tmp_u[i]); R.acc[i] = (R.acc[i] & ~(int64_t)0xFFFF) | res[i]; }
        break;
    case 40: /* VAND */
        for (i = 0; i < 8; i++) { res[i] = (uint16_t)(lane_u(vs, i) & tmp_u[i]); R.acc[i] = (R.acc[i] & ~(int64_t)0xFFFF) | res[i]; }
        break;
    case 44: /* VXOR */
        for (i = 0; i < 8; i++) { res[i] = (uint16_t)(lane_u(vs, i) ^ tmp_u[i]); R.acc[i] = (R.acc[i] & ~(int64_t)0xFFFF) | res[i]; }
        break;
    default:
        rsp_unsupported("vector operation", pc, word);
    }
    for (i = 0; i < 8; i++) set_lane(vd, i, res[i]);
}


/* ---- the vector unit, SIMD form ---------------------------------------------------------------------------------------------------- */
/*
 * SPEED.  The Switch tester's log put the audio task at about 13 ms per video field on the console's Cortex-A57 cores: 60 % of the game thread's time, and
 * the picture and sound then missed their deadlines.  A count of the instructions of the ROM's audio microcode (PS_RSP_STATS) showed 35 % vector ALU
 * instructions, and the scalar version above (a function call, a lane-by-lane loop with 64-bit arithmetic and a byte swap on every lane access) spent most of
 * the interpreter's time in them.  The version below does the same arithmetic on whole vectors, written with the compilers' portable vector types (GCC and
 * clang lower them to SSE on a PC and to NEON on the Switch; nothing in the source names an instruction set).  The results are the same bit for bit:
 * tests/native/rsp/diff_vs_cxd4.c compares this version with the reference, and a build with -DPS_RSP_CROSSCHECK runs both versions on every vector
 * instruction of a real run of the game and stops at the first difference (build with -DPS_RSP_SCALAR to use the scalar version above instead).
 *
 * Why 128-bit pieces: GCC (the Switch compiler) turns operations on vectors wider than the hardware's 128 bits into element-by-element code with compares
 * and conditional moves (seen in the disassembly of a first version that used 256- and 512-bit types).  Every type below is at most 128 bits, so each
 * operation is one NEON (or SSE) instruction.  The eight lanes are handled as two "quads" of four; a quad's accumulators are two 2 x 64-bit vectors.
 *
 * Representation: vpr stays big-endian bytes (the loads and stores work on bytes); a register is turned into native lanes with one byte shuffle and
 * back with another.  The accumulator stays eight sign-extended 64-bit values in R.acc (48 significant bits).
 */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && (defined(__GNUC__) || defined(__clang__)) && !defined(PS_RSP_SCALAR)
#define PS_RSP_SIMD 1
typedef int16_t  v8h  __attribute__((vector_size(16)));
typedef uint16_t v8uh __attribute__((vector_size(16)));
typedef int16_t  v4h  __attribute__((vector_size(8)));
typedef uint16_t v4uh __attribute__((vector_size(8)));
typedef int32_t  v4w  __attribute__((vector_size(16)));
typedef uint32_t v4uw __attribute__((vector_size(16)));
typedef int32_t  v2w  __attribute__((vector_size(8)));
typedef int64_t  v2d  __attribute__((vector_size(16)));
typedef uint64_t v2ud __attribute__((vector_size(16)));
typedef uint8_t  v16b __attribute__((vector_size(16)));
typedef struct { v2d l, h; } q4;                       /* the accumulators of four lanes */

#define PS_AI static inline __attribute__((always_inline))
#define PS_SHUF8(v, a, b, c, d, e, f, g, h) ((v8uh)__builtin_shufflevector((v8uh)(v), (v8uh)(v), a, b, c, d, e, f, g, h))
#define PS_SHUF16(v) ((v16b)__builtin_shufflevector((v16b)(v), (v16b)(v), 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14))

PS_AI v8uh sv_load(int r) { v16b b; memcpy(&b, R.vpr[r], 16); return (v8uh)PS_SHUF16(b); }
PS_AI void sv_store(int r, v8uh x) { v16b b = PS_SHUF16((v16b)x); memcpy(R.vpr[r], &b, 16); }

/* VT as the element field shows it (the broadcast modifier); same table as vt_lane() above. */
PS_AI v8uh sv_vt(int vt, int e)
{
    v8uh b = sv_load(vt);
    switch (e) {
    case 0: case 1: return b;
    case 2:  return PS_SHUF8(b, 0, 0, 2, 2, 4, 4, 6, 6);
    case 3:  return PS_SHUF8(b, 1, 1, 3, 3, 5, 5, 7, 7);
    case 4:  return PS_SHUF8(b, 0, 0, 0, 0, 4, 4, 4, 4);
    case 5:  return PS_SHUF8(b, 1, 1, 1, 1, 5, 5, 5, 5);
    case 6:  return PS_SHUF8(b, 2, 2, 2, 2, 6, 6, 6, 6);
    case 7:  return PS_SHUF8(b, 3, 3, 3, 3, 7, 7, 7, 7);
    case 8:  return PS_SHUF8(b, 0, 0, 0, 0, 0, 0, 0, 0);
    case 9:  return PS_SHUF8(b, 1, 1, 1, 1, 1, 1, 1, 1);
    case 10: return PS_SHUF8(b, 2, 2, 2, 2, 2, 2, 2, 2);
    case 11: return PS_SHUF8(b, 3, 3, 3, 3, 3, 3, 3, 3);
    case 12: return PS_SHUF8(b, 4, 4, 4, 4, 4, 4, 4, 4);
    case 13: return PS_SHUF8(b, 5, 5, 5, 5, 5, 5, 5, 5);
    case 14: return PS_SHUF8(b, 6, 6, 6, 6, 6, 6, 6, 6);
    default: return PS_SHUF8(b, 7, 7, 7, 7, 7, 7, 7, 7);
    }
}

/* lanes 4q..4q+3 of a register, widened to 32 bits (signed / unsigned) */
PS_AI v4w q_sw(v8uh x, int q)
{
    v8h s = (v8h)x;
    v4h t = q ? __builtin_shufflevector(s, s, 4, 5, 6, 7) : __builtin_shufflevector(s, s, 0, 1, 2, 3);
    return __builtin_convertvector(t, v4w);
}
PS_AI v4w q_uw(v8uh x, int q)
{
    v4uh t = q ? __builtin_shufflevector(x, x, 4, 5, 6, 7) : __builtin_shufflevector(x, x, 0, 1, 2, 3);
    return (v4w)__builtin_convertvector(t, v4uw);
}
/* four 32-bit values -> four 64-bit accumulator lanes (sign-extended) */
PS_AI q4 q_widen(v4w p)
{
    q4 r;
    r.l = __builtin_convertvector(__builtin_shufflevector(p, p, 0, 1), v2d);
    r.h = __builtin_convertvector(__builtin_shufflevector(p, p, 2, 3), v2d);
    return r;
}
PS_AI q4 q_get(int q) { q4 r; memcpy(&r.l, &R.acc[4 * q], 16); memcpy(&r.h, &R.acc[4 * q + 2], 16); return r; }
PS_AI void q_put(int q, q4 a) { memcpy(&R.acc[4 * q], &a.l, 16); memcpy(&R.acc[4 * q + 2], &a.h, 16); }
PS_AI q4 q_add(q4 a, q4 b) { q4 r; r.l = a.l + b.l; r.h = a.h + b.h; return r; }
PS_AI q4 q_add_k(q4 a, int64_t k) { q4 r; r.l = a.l + k; r.h = a.h + k; return r; }
PS_AI q4 q_shl1(q4 a) { q4 r; r.l = (v2d)((v2ud)a.l << 1); r.h = (v2d)((v2ud)a.h << 1); return r; }
PS_AI q4 q_shl16(q4 a) { q4 r; r.l = (v2d)((v2ud)a.l << 16); r.h = (v2d)((v2ud)a.h << 16); return r; }
PS_AI q4 q_wrap48(q4 a) { q4 r; r.l = (v2d)((v2ud)a.l << 16) >> 16; r.h = (v2d)((v2ud)a.h << 16) >> 16; return r; }   /* keep 48 bits, sign-extended */
PS_AI v4w q_narrow(v2d l, v2d h)                                                      /* the low 32 bits of four 64-bit values */
{
    v2w nl = __builtin_convertvector(l, v2w), nh = __builtin_convertvector(h, v2w);
    return __builtin_shufflevector(nl, nh, 0, 1, 2, 3);
}
PS_AI v4w q_lo32(q4 a) { return q_narrow(a.l, a.h); }
PS_AI v4w q_hi32(q4 a) { return q_narrow(a.l >> 32, a.h >> 32); }                      /* bits 47..32, sign-extended */
PS_AI v4w q_sar16(q4 a) { return q_narrow(a.l >> 16, a.h >> 16); }                      /* acc >> 16: fits in 32 bits */
PS_AI v4uh q_sat16(v4w t)                                                              /* saturate to a signed 16-bit lane */
{
    v4w hi = t > 32767, lo = t < -32768;
    t = (t & ~hi) | (hi & 32767);
    t = (t & ~lo) | (lo & -32768);
    return (v4uh)__builtin_convertvector(t, v4h);
}
/* "clamp low": the low 16 bits unless the accumulator no longer fits in a signed 32-bit value (then 0xFFFF for positive, 0 for negative).
 * With hi = bits 47..32 and lo = bits 31..0: it fits exactly when hi equals the sign extension of lo; when it does not, hi >= 0 means positive. */
PS_AI v4uh q_clamp_low(q4 a)
{
    v4w hi = q_hi32(a), lo = q_lo32(a);
    v4w inrange = hi == (lo >> 31);
    v4w r = (lo & inrange) | (~inrange & (hi >= 0));
    return (v4uh)__builtin_convertvector((v4uw)(r & 0xFFFF), v4uh);
}
PS_AI q4 q_low_set(q4 a, v4uh low)                                                    /* replace bits 15..0 of each accumulator */
{
    v4uw w = __builtin_convertvector(low, v4uw);
    q4 r;
    r.l = (a.l & ~(int64_t)0xFFFF) | (v2d)__builtin_convertvector(__builtin_shufflevector(w, w, 0, 1), v2ud);
    r.h = (a.h & ~(int64_t)0xFFFF) | (v2d)__builtin_convertvector(__builtin_shufflevector(w, w, 2, 3), v2ud);
    return r;
}
PS_AI v8uh q_join(v4uh lo, v4uh hi) { return __builtin_shufflevector(lo, hi, 0, 1, 2, 3, 4, 5, 6, 7); }
/* the flag word's bits 4q+shift .. 4q+shift+3 as lane masks (-1 / 0) */
PS_AI v4w q_bits(unsigned m, int q, int shift)
{
    const v4w sel = {1, 2, 4, 8};
    v4w x = (v4w){0} + (int32_t)((m >> (4 * q + shift)) & 15);
    return (x & sel) != 0;
}
/* lanes of a 16-bit mask vector -> bits 0..7 */
PS_AI v8h sv_bits(unsigned m, int shift)
{
    const v8h sel = {1, 2, 4, 8, 16, 32, 64, 128};
    v8h x = (v8h){0} + (int16_t)((m >> shift) & 0xFF);
    return (x & sel) != 0;
}
PS_AI unsigned sv_mask_bits(v8h m)
{
    unsigned r = 0;
    int i;
    for (i = 0; i < 8; i++) r |= ((unsigned)(m[i] & 1)) << i;
    return r;
}

/* The multiply and multiply-accumulate instructions, one quad (lanes 4q..4q+3) at a time.  kind: signedness of VS and VT. */
#define PS_MULQ(q, A, B, EXPR_ACC, EXPR_RES) \
    do { v4w x_ = A, y_ = B; (void)x_; (void)y_; EXPR_ACC; EXPR_RES; } while (0)

PS_AI v4uh quad_arith(int op, int q, v8uh a, v8uh b)
{
    q4 acc;
    v4uh res;
    switch (op) {
    case 0:  /* VMULF: acc = 2*vs*vt + 0x8000; result = saturate(acc >> 16) */
        acc = q_add_k(q_shl1(q_widen(q_sw(a, q) * q_sw(b, q))), 0x8000);
        res = q_sat16(q_sar16(acc));
        break;
    case 8:  /* VMACF: acc += 2*vs*vt */
        acc = q_wrap48(q_add(q_get(q), q_shl1(q_widen(q_sw(a, q) * q_sw(b, q)))));
        res = q_sat16(q_sar16(acc));
        break;
    case 4:  /* VMUDL: acc = (vs_unsigned * vt_unsigned) >> 16; result = low accumulator */
        acc = q_widen((v4w)(((v4uw)q_uw(a, q) * (v4uw)q_uw(b, q)) >> 16));            /* the shifted product is < 65536: widening as signed is exact */
        res = q_clamp_low(acc);
        break;
    case 5:  /* VMUDM: acc = vs_signed * vt_unsigned */
        acc = q_widen(q_sw(a, q) * q_uw(b, q));
        res = q_sat16(q_sar16(acc));
        break;
    case 6:  /* VMUDN: acc = vs_unsigned * vt_signed */
        acc = q_widen(q_uw(a, q) * q_sw(b, q));
        res = q_clamp_low(acc);
        break;
    case 7:  /* VMUDH: acc = (vs_signed * vt_signed) << 16; result = saturate(acc >> 16) = saturate(the product) */
        {
            v4w p = q_sw(a, q) * q_sw(b, q);
            acc = q_shl16(q_widen(p));
            res = q_sat16(p);
        }
        break;
    case 12: /* VMADL */
        acc = q_wrap48(q_add(q_get(q), q_widen((v4w)(((v4uw)q_uw(a, q) * (v4uw)q_uw(b, q)) >> 16))));
        res = q_clamp_low(acc);
        break;
    case 13: /* VMADM */
        acc = q_wrap48(q_add(q_get(q), q_widen(q_sw(a, q) * q_uw(b, q))));
        res = q_sat16(q_sar16(acc));
        break;
    case 14: /* VMADN */
        acc = q_wrap48(q_add(q_get(q), q_widen(q_uw(a, q) * q_sw(b, q))));
        res = q_clamp_low(acc);
        break;
    default: /* 15: VMADH: acc += (vs_signed * vt_signed) << 16 */
        acc = q_wrap48(q_add(q_get(q), q_shl16(q_widen(q_sw(a, q) * q_sw(b, q)))));
        res = q_sat16(q_sar16(acc));
        break;
    }
    q_put(q, acc);
    return res;
}

PS_AI v4uh quad_addsub(int op, int q, v8uh a, v8uh b)                 /* VADD, VSUB: result = saturate(vs +- vt -+ carry); the low accumulator gets the unsaturated value */
{
    v4w c = q_bits(R.vco, q, 0) & 1;
    v4w sum = (op == 16) ? q_sw(a, q) + q_sw(b, q) + c : q_sw(a, q) - q_sw(b, q) - c;
    q_put(q, q_low_set(q_get(q), __builtin_convertvector((v4uw)sum, v4uh)));
    return q_sat16(sum);
}

static void vector_op_simd(uint32_t word, uint32_t pc)
{
    int e = (word >> 21) & 15, vt = (word >> 16) & 31, vs = (word >> 11) & 31, vd = (word >> 6) & 31, op = word & 63;
    v8uh a = sv_load(vs), b = sv_vt(vt, e), res;

    switch (op) {
    case 0: case 4: case 5: case 6: case 7: case 8: case 12: case 13: case 14: case 15:
        res = q_join(quad_arith(op, 0, a, b), quad_arith(op, 1, a, b));
        break;
    case 16: /* VADD (VCO is cleared) */
    case 17: /* VSUB */
        res = q_join(quad_addsub(op, 0, a, b), quad_addsub(op, 1, a, b));
        R.vco = 0;
        break;
    case 20: /* VADDC: unsigned add, carry out into VCO bit i (not-equal bits cleared); result = low 16 bits */
        {
            v8uh lowv = a + b;                                                               /* 16-bit wrap-around: the low half of the sum */
            v8h carry = (v8h)(lowv < a);                                                      /* carry out: the wrapped sum is smaller (unsigned compare) */
            res = lowv;
            R.vco = (uint16_t)sv_mask_bits(carry);
            q_put(0, q_low_set(q_get(0), __builtin_shufflevector(lowv, lowv, 0, 1, 2, 3)));
            q_put(1, q_low_set(q_get(1), __builtin_shufflevector(lowv, lowv, 4, 5, 6, 7)));
        }
        break;
    case 29: /* VSAR: vd = one third of the accumulator, chosen by the element field: 8 = bits 47:32, 9 = 31:16, 10 = 15:0. The accumulator is not changed. */
        {
            q4 a0 = q_get(0), a1 = q_get(1);
            v4w r0, r1;
            if (e == 8) { r0 = q_hi32(a0); r1 = q_hi32(a1); }
            else if (e == 9) { r0 = q_sar16(a0); r1 = q_sar16(a1); }
            else if (e == 10) { r0 = q_lo32(a0); r1 = q_lo32(a1); }
            else { rsp_unsupported("VSAR with an element outside 8..10", pc, word); return; }
            res = q_join(__builtin_convertvector((v4uw)r0, v4uh), __builtin_convertvector((v4uw)r1, v4uh));
        }
        break;
    case 40: res = a & b; goto logic;                                  /* VAND */
    case 42: res = a | b; goto logic;                                  /* VOR */
    case 44: res = a ^ b;                                              /* VXOR */
    logic:
        q_put(0, q_low_set(q_get(0), __builtin_shufflevector(res, res, 0, 1, 2, 3)));
        q_put(1, q_low_set(q_get(1), __builtin_shufflevector(res, res, 4, 5, 6, 7)));
        break;
    case 32: case 33: case 34: case 35: case 39:                     /* VLT, VEQ, VNE, VGE, VMRG: lane-wise decisions with flag bits */
        {
            v8h sa = (v8h)a, sb = (v8h)b;
            v8h carry = sv_bits(R.vco, 0), ne = sv_bits(R.vco, 8);
            v8h sel;
            switch (op) {
            case 32: sel = (sa < sb) | ((sa == sb) & carry & ne); break;                       /* VLT: less, or equal while carry and not-equal */
            case 35: sel = (sa > sb) | ((sa == sb) & ~(carry & ne)); break;                     /* VGE: greater, or equal unless carry and not-equal */
            case 33: sel = (sa == sb) & ~ne; break;                                              /* VEQ */
            case 34: sel = (sa != sb) | ne; break;                                               /* VNE */
            default: sel = sv_bits(R.vcc, 0); break;                                             /* VMRG: lanes selected in VCC (low half) */
            }
            res = (v8uh)((sel & sa) | (~sel & sb));
            q_put(0, q_low_set(q_get(0), __builtin_shufflevector(res, res, 0, 1, 2, 3)));
            q_put(1, q_low_set(q_get(1), __builtin_shufflevector(res, res, 4, 5, 6, 7)));
            if (op != 39) {
                R.vcc = (uint16_t)sv_mask_bits(sel);                                             /* VCC = the selection (the clip half is cleared) */
                R.vco = 0;                                                                       /* VCE is left alone (checked against two independent emulators) */
            }
        }
        break;
    default:
        rsp_unsupported("vector operation", pc, word);
    }
    sv_store(vd, res);
}

#ifdef PS_RSP_CROSSCHECK
/* development (-DPS_RSP_CROSSCHECK): every vector instruction is run twice, by the SIMD and by the scalar version, from the same state; any difference in the
 * registers, the accumulators or the flags stops the program.  Used on a real run of the game (the audio microcode with the game's own data). */
static unsigned long long sCrossCount;
static void vector_op(uint32_t word, uint32_t pc)
{
    Rsp save = R, after;
    vector_op_simd(word, pc);
    after = R;
    R = save;
    vector_op_scalar(word, pc);
    if (memcmp(R.vpr, after.vpr, sizeof R.vpr) || memcmp(R.acc, after.acc, sizeof R.acc) || R.vco != after.vco || R.vcc != after.vcc || R.vce != after.vce) {
        fprintf(stderr, "RSP CROSSCHECK: SIMD and scalar differ at IMEM %03X, word %08X\n", (unsigned)pc, (unsigned)word);
        abort();
    }
    if (++sCrossCount % 5000000 == 0) fprintf(stderr, "rsp crosscheck: %llu vector instructions identical\n", sCrossCount);
}
#else
#define vector_op vector_op_simd
#endif
#else
#define vector_op vector_op_scalar
#endif

/* ---- the main loop --------------------------------------------------------------------------------------------------------------- */

void host_rsp_start(unsigned char *dmem, unsigned char *imem, unsigned int pc, unsigned int mem_addr, unsigned int dram_addr,
                    unsigned int status, unsigned int semaphore, unsigned int dpc_status)
{
    static int env_read;
    static int trace_n;
    static unsigned long trace_match;
    if (!env_read) {
        const char *e = getenv("KYUJO_RSP_TRACE");
        env_read = 1;
        if (e && atoi(e) > 0) { trace_n = atoi(e); sTraceLeft = trace_n; }
        /* KYUJO_RSP_TRACE_MATCH=<hex>: instead of tracing the first <n> instructions of the whole run, trace the first <n> of EVERY task
         * whose word at IMEM offset 8 equals <hex> (to pick one microcode out of the audio and game ones). */
        e = getenv("KYUJO_RSP_TRACE_MATCH");
        if (e && *e) { trace_match = strtoul(e, NULL, 16); sTraceLeft = 0; }
    }
    if (trace_match && trace_n > 0) {
        const unsigned long w8 = ((unsigned long)imem[8] << 24) | ((unsigned long)imem[9] << 16) | ((unsigned long)imem[10] << 8) | imem[11];
        fprintf(stderr, "rsp task: IMEM word at 8 = %08lX, DMEM task type %lu%s\n", w8, ((unsigned long)dmem[0xFC0] << 24 | (unsigned long)dmem[0xFC1] << 16 | (unsigned long)dmem[0xFC2] << 8 | dmem[0xFC3]), w8 == trace_match ? " (traced)" : "");
        if (w8 == trace_match) sTraceLeft = trace_n;
    }
    R.dmem = dmem;
    R.imem = imem;
    R.pc = pc & 0xFFC;
    R.mem_addr = mem_addr;
    R.dram_addr = dram_addr;
    R.status = status;
    R.semaphore = semaphore;
    R.dpc_status = dpc_status;
    R.irq_req = 0;
    R.yielded = 0;
    R.poll_valid = 0;
}

/* Continues a task that host_rsp_run suspended (host_rsp_get(6) != 0). The CPU may have written SP_STATUS (and DPC_STATUS, the semaphore)
 * in the meantime, so the current values are handed in; everything else (PC, registers, memories, SP_MEM_ADDR/SP_DRAM_ADDR) is the
 * microcode's own. Call host_rsp_run again afterwards. */
void host_rsp_resume(unsigned int status, unsigned int semaphore, unsigned int dpc_status)
{
    R.status = status;
    R.semaphore = semaphore;
    R.dpc_status = dpc_status;
    R.irq_req = 0;
    R.yielded = 0;
}

/* Runs until BREAK, or until the microcode waits for the CPU. Returns the number of instructions executed (truncated to 32 bits).
 *
 * WAITING FOR THE CPU: the Game Boy Tower's microcodes end a frame by setting SP_STATUS signal 3, then spinning on `mfc0 SP_STATUS` until
 * the CPU has cleared it (the CPU side: `while (!(SP_STATUS & SIG3)) {} SP_STATUS = SP_CLR_SIG3`, gb_tower_emulator_7F9A0.c). On the
 * console the two run side by side. Here the interpreter runs inside the CPU's own register access, so it cannot spin: when an `mfc0` of
 * SP_STATUS returns exactly what the previous one returned and nothing was written to a COP0 register in between, nothing but the CPU
 * can change the next answer, so the run is SUSPENDED at that instruction (PC left on it, host_rsp_get(6) = 1) and the caller continues the
 * task (host_rsp_resume + host_rsp_run) when the CPU next touches SP_STATUS. The same instruction is then executed again with the new value.
 * A poll that sits in a branch delay slot is not a yield point (the delay state is not saved); the next poll will be. */
/* The instruction word at IMEM offset pc (big-endian in memory; pc is a multiple of 4 and below 0x1000, so the four bytes never wrap). */
static uint32_t rsp_fetch(uint32_t pc)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && (defined(__GNUC__) || defined(__clang__))
    uint32_t w;
    memcpy(&w, R.imem + (pc & 0xFFC), 4);
    w = __builtin_bswap32(w);
#ifdef PS_RSP_VERIFY      /* development (-DPS_RSP_VERIFY): must equal the plain byte composition */
    if (w != (((uint32_t)R.imem[pc & 0xFFF] << 24) | ((uint32_t)R.imem[(pc + 1) & 0xFFF] << 16) | ((uint32_t)R.imem[(pc + 2) & 0xFFF] << 8) | R.imem[(pc + 3) & 0xFFF])) {
        fprintf(stderr, "RSP VERIFY: instruction fetch mismatch at %03X\n", (unsigned)pc); abort();
    }
#endif
    return w;
#else
    return ((uint32_t)R.imem[pc & 0xFFF] << 24) | ((uint32_t)R.imem[(pc + 1) & 0xFFF] << 16) | ((uint32_t)R.imem[(pc + 2) & 0xFFF] << 8) | R.imem[(pc + 3) & 0xFFF];
#endif
}

#ifdef PS_RSP_STATS      /* development (-DPS_RSP_STATS): instructions and time per call and per kind of instruction, printed every 300 calls */
#include <time.h>
static unsigned long long sStKind[4], sStNs[4], sStCalls, sStSteps, sStTime;
static unsigned long long st_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000000000ULL + (unsigned long long)t.tv_nsec; }
#endif
static unsigned int host_rsp_run_inner(void);
#ifdef PS_RSP_STATS
#include <signal.h>
#include <sys/time.h>
static volatile int sCls = -1;                         /* the kind of instruction being executed (-1: not in the interpreter) */
static unsigned long long sClsHist[128];
static void cls_tick(int sig) { (void)sig; if (sCls >= 0) sClsHist[sCls]++; }
static void cls_start(void)
{
    static int done;
    struct itimerval it;
    if (done) return;
    done = 1;
    signal(SIGPROF, cls_tick);
    it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 500; it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, NULL);
}
#endif
unsigned int host_rsp_run(void)
{
#ifdef PS_RSP_STATS
    unsigned long long t0 = st_now();
    cls_start();
    unsigned int n = host_rsp_run_inner();
    sStTime += st_now() - t0; sStSteps += n; sStCalls++;
    { static unsigned long long h = 1469598103934665603ULL; int k; for (k = 0; k < 4096; k++) h = (h ^ R.dmem[k]) * 1099511628211ULL;
      if (sStCalls % 100 == 0) fprintf(stderr, "rsp hash after %llu calls: %016llx\n", sStCalls, h); }
    sCls = -1;
    if (sStCalls % 1500 == 0) { int k; unsigned long long tot = 0; for (k = 0; k < 128; k++) tot += sClsHist[k]; fprintf(stderr, "rsp time by kind (samples %llu):", tot); for (k = 0; k < 128; k++) if (sClsHist[k] * 200 > tot) fprintf(stderr, " %s%d=%.1f%%", k < 64 ? "vec" : (k == 64 ? "scalar" : k == 65 ? "vload" : k == 66 ? "vstore" : k == 67 ? "cop0" : k == 68 ? "mfc2/mtc2" : "other"), k < 64 ? k : 0, 100.0 * (double)sClsHist[k] / (double)tot); fprintf(stderr, "\n"); }
    if (sStCalls % 300 == 0) { fprintf(stderr, "rsp stats: %llu calls, %.0f steps/call, %.1f ns/step, %.2f ms/call\n", sStCalls, (double)sStSteps / 300, (double)sStTime / (double)sStSteps, (double)sStTime / 300e6); sStSteps = sStTime = 0; }
    return n;
#else
    return host_rsp_run_inner();
#endif
}
static unsigned int host_rsp_run_inner(void)
{
    const unsigned long long limit = 200ull * 1000 * 1000;     /* a microcode that does not break within this is considered lost */
    unsigned long long steps = 0;
    int delay = 0;                                              /* the next instruction is a delay slot... */
    uint32_t delay_target = 0;                                  /* ...and then control goes here */
    int branch_pending = 0;

    R.yielded = 0;
    for (;;) {
        uint32_t pc = R.pc;
        uint32_t w = rsp_fetch(pc);
        uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sh = (w >> 6) & 31;
        int32_t simm = (int16_t)w;
        uint32_t uimm = w & 0xFFFF;
        uint32_t next = (pc + 4) & 0xFFF;
        int take = 0;
        uint32_t target = 0;
        uint32_t ea;

        if (++steps > limit) {
            char msg[160];
            snprintf(msg, sizeof msg, "RSP interpreter: the microcode did not reach BREAK (IMEM word at 8 is %08X, now at IMEM 0x%03X, instruction %08X)",
                     (unsigned)(((uint32_t)R.imem[8] << 24) | ((uint32_t)R.imem[9] << 16) | ((uint32_t)R.imem[10] << 8) | R.imem[11]), (unsigned)pc, (unsigned)w);
            host_fatal(msg);
        }
        if (sTraceLeft > 0) {                                    /* development: KYUJO_RSP_TRACE=<n> prints the first n instructions (address, word) */
            sTraceLeft--;
            fprintf(stderr, "rsp %03X: %08X\n", (unsigned)pc, (unsigned)w);
        }
#ifdef PS_RSP_STATS
        sCls = (op == 18 && (w & (1u << 25))) ? (int)(w & 63) : op == 50 ? 65 : op == 58 ? 66 : op == 16 ? 67 : op == 18 ? 68 : 64;
        { static unsigned long long hist[64], vl, vs_, tot, next_print; tot++; if (op == 18 && (w & (1u << 25))) hist[w & 63]++; else if (op == 50) vl++; else if (op == 58) vs_++;
          if (tot >= next_print + 40000000ULL) { int k; next_print = tot; fprintf(stderr, "rsp mix of %llu: vload %llu vstore %llu vector:", tot, vl, vs_); for (k = 0; k < 64; k++) if (hist[k]) fprintf(stderr, " %d=%llu", k, hist[k]); fprintf(stderr, "\n"); } }
#endif
        branch_pending = delay;                                  /* if the previous instruction was a branch, this one is its delay slot */
        delay = 0;

        switch (op) {
        case 0:
            switch (w & 63) {
            case 0:  R.gpr[rd] = R.gpr[rt] << sh; break;                            /* SLL */
            case 2:  R.gpr[rd] = R.gpr[rt] >> sh; break;                            /* SRL */
            case 3:  R.gpr[rd] = (uint32_t)((int32_t)R.gpr[rt] >> sh); break;       /* SRA */
            case 4:  R.gpr[rd] = R.gpr[rt] << (R.gpr[rs] & 31); break;              /* SLLV */
            case 6:  R.gpr[rd] = R.gpr[rt] >> (R.gpr[rs] & 31); break;              /* SRLV */
            case 7:  R.gpr[rd] = (uint32_t)((int32_t)R.gpr[rt] >> (R.gpr[rs] & 31)); break;   /* SRAV */
            case 8:  take = 1; target = R.gpr[rs] & 0xFFC; break;                   /* JR */
            case 9:  take = 1; target = R.gpr[rs] & 0xFFC; R.gpr[rd] = (pc + 8) & 0xFFF; break;   /* JALR */
            case 13: goto broke;                                                    /* BREAK */
            case 32: case 33: R.gpr[rd] = R.gpr[rs] + R.gpr[rt]; break;             /* ADD, ADDU: no overflow trap on the RSP */
            case 34: case 35: R.gpr[rd] = R.gpr[rs] - R.gpr[rt]; break;             /* SUB, SUBU */
            case 36: R.gpr[rd] = R.gpr[rs] & R.gpr[rt]; break;                      /* AND */
            case 37: R.gpr[rd] = R.gpr[rs] | R.gpr[rt]; break;                      /* OR */
            case 38: R.gpr[rd] = R.gpr[rs] ^ R.gpr[rt]; break;                      /* XOR */
            case 39: R.gpr[rd] = ~(R.gpr[rs] | R.gpr[rt]); break;                   /* NOR */
            case 42: R.gpr[rd] = ((int32_t)R.gpr[rs] < (int32_t)R.gpr[rt]); break;  /* SLT */
            case 43: R.gpr[rd] = (R.gpr[rs] < R.gpr[rt]); break;                    /* SLTU */
            default: rsp_unsupported("SPECIAL instruction", pc, w);
            }
            break;
        case 1:
            switch (rt) {
            case 0:  take = ((int32_t)R.gpr[rs] < 0);  target = (pc + 4 + (simm << 2)) & 0xFFC; break;                       /* BLTZ */
            case 1:  take = ((int32_t)R.gpr[rs] >= 0); target = (pc + 4 + (simm << 2)) & 0xFFC; break;                      /* BGEZ */
            case 16: take = ((int32_t)R.gpr[rs] < 0);  target = (pc + 4 + (simm << 2)) & 0xFFC; R.gpr[31] = (pc + 8) & 0xFFF; break;   /* BLTZAL: the test reads rs first, the link is written whether or not it branches */
            case 17: take = ((int32_t)R.gpr[rs] >= 0); target = (pc + 4 + (simm << 2)) & 0xFFC; R.gpr[31] = (pc + 8) & 0xFFF; break;   /* BGEZAL: same */
            default: rsp_unsupported("REGIMM instruction", pc, w);
            }
            break;
        case 2:  take = 1; target = (w << 2) & 0xFFC; break;                                                                 /* J */
        case 3:  take = 1; target = (w << 2) & 0xFFC; R.gpr[31] = (pc + 8) & 0xFFF; break;                                  /* JAL */
        case 4:  take = (R.gpr[rs] == R.gpr[rt]); target = (pc + 4 + (simm << 2)) & 0xFFC; break;                            /* BEQ */
        case 5:  take = (R.gpr[rs] != R.gpr[rt]); target = (pc + 4 + (simm << 2)) & 0xFFC; break;                            /* BNE */
        case 6:  take = ((int32_t)R.gpr[rs] <= 0); target = (pc + 4 + (simm << 2)) & 0xFFC; break;                           /* BLEZ */
        case 7:  take = ((int32_t)R.gpr[rs] > 0);  target = (pc + 4 + (simm << 2)) & 0xFFC; break;                           /* BGTZ */
        case 8: case 9: R.gpr[rt] = R.gpr[rs] + (uint32_t)simm; break;                                                       /* ADDI, ADDIU */
        case 13: R.gpr[rt] = R.gpr[rs] | uimm; break;                                                                        /* ORI */
        case 14: R.gpr[rt] = R.gpr[rs] ^ uimm; break;                                                                        /* XORI (zero-extended immediate, like ORI) */
        case 10: R.gpr[rt] = ((int32_t)R.gpr[rs] < (int32_t)simm); break;                                                    /* SLTI: signed compare with the sign-extended immediate */
        case 11: R.gpr[rt] = (R.gpr[rs] < (uint32_t)simm); break;                                                            /* SLTIU: unsigned compare with the SIGN-extended immediate (as on every MIPS) */
        case 12: R.gpr[rt] = R.gpr[rs] & uimm; break;                                                                        /* ANDI */
        case 15: R.gpr[rt] = uimm << 16; break;                                                                              /* LUI */
        case 16:
            if (rs == 0) {                                                                                                   /* MFC0 */
                if (rd == 4 && !branch_pending) {
                    if (R.poll_valid && R.status == R.poll_value) {          /* the same answer twice with no write in between: wait for the CPU */
                        R.yielded = 1;
                        R.steps = 0;
                        sClockBase += steps - 1;                             /* this instruction did not execute */
                        return (unsigned int)(steps - 1);
                    }
                    R.poll_valid = 1;
                    R.poll_value = R.status;
                }
                R.steps = steps;
                R.gpr[rt] = cop0_read((int)rd, pc, w);
            } else if (rs == 4) {                                                                                            /* MTC0 */
                R.poll_valid = 0;
                cop0_write((int)rd, R.gpr[rt], pc, w);
            }
            else rsp_unsupported("COP0 instruction", pc, w);
            break;
        case 18:
            if (w & (1u << 25)) {
                vector_op(w, pc);
            } else if (rs == 0) {                                                       /* MFC2 rt, vd[element]: a signed halfword */
                int e = (int)((w >> 7) & 15);
                R.gpr[rt] = (uint32_t)(int32_t)(int16_t)(((uint16_t)R.vpr[rd][e] << 8) | R.vpr[rd][(e + 1) & 15]);
            } else if (rs == 4) {                                                       /* MTC2 rt, vd[element]: low halfword; at e = 15 only the last byte fits */
                int e = (int)((w >> 7) & 15);
                R.vpr[rd][e] = (uint8_t)(R.gpr[rt] >> 8);
                if (e < 15) R.vpr[rd][e + 1] = (uint8_t)R.gpr[rt];
            } else {
                rsp_unsupported("COP2 move instruction", pc, w);
            }
            break;
        case 32: ea = R.gpr[rs] + (uint32_t)simm; R.gpr[rt] = (uint32_t)(int32_t)(int8_t)ld8(ea); break;                     /* LB */
        case 33: ea = R.gpr[rs] + (uint32_t)simm; R.gpr[rt] = (uint32_t)(int32_t)(int16_t)ld16(ea); break;                   /* LH */
        case 35: ea = R.gpr[rs] + (uint32_t)simm; R.gpr[rt] = ld32(ea); break;                                               /* LW */
        case 36: ea = R.gpr[rs] + (uint32_t)simm; R.gpr[rt] = ld8(ea); break;                                                /* LBU */
        case 37: ea = R.gpr[rs] + (uint32_t)simm; R.gpr[rt] = ld16(ea); break;                                               /* LHU */
        case 40: ea = R.gpr[rs] + (uint32_t)simm; st8(ea, R.gpr[rt]); break;                                                 /* SB */
        case 41: ea = R.gpr[rs] + (uint32_t)simm; st16(ea, R.gpr[rt]); break;                                                /* SH */
        case 43: ea = R.gpr[rs] + (uint32_t)simm; st32(ea, R.gpr[rt]); break;                                                /* SW */
        case 50: vload(w, pc); break;                                                                                         /* LWC2 */
        case 58: vstore(w, pc); break;                                                                                        /* SWC2 */
        default:
            rsp_unsupported("instruction", pc, w);
        }
        R.gpr[0] = 0;

        /* Control transfer: a taken branch executes its delay slot and then jumps. */
        if (branch_pending) {
            R.pc = delay_target;                                  /* the slot just executed: go to the recorded target */
            (void)take;
            if (take) {
                /* a branch inside a delay slot: not used by the microcode; refuse rather than guess */
                rsp_unsupported("branch in a branch delay slot", pc, w);
            }
        } else if (take) {
            delay = 1;
            delay_target = target;
            R.pc = next;                                           /* the delay slot comes first */
        } else {
            R.pc = next;
        }
        continue;
broke:
        R.status |= SPS_HALT | SPS_BROKE;
        R.pc = (pc + 4) & 0xFFC;
        R.steps = steps;
        sClockBase += steps;                                      /* the clock keeps counting across tasks */
        R.steps = 0;
        return (unsigned int)steps;
    }
}

unsigned int host_rsp_get(unsigned int which)
{
    switch (which) {
    case 0: return R.mem_addr;
    case 1: return R.dram_addr;
    case 2: return R.status;
    case 3: return R.semaphore;
    case 4: return R.pc;
    case 5: return R.irq_req;
    case 6: return (unsigned int)R.yielded;                 /* 1: the task is suspended, waiting for the CPU (see host_rsp_run) */
    default: return 0;
    }
}
