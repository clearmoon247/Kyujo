#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""rsp_dis.py - a small disassembler for RSP microcode, written to find out WHICH instructions a microcode uses before the interpreter
(src/rsp/rsp_cpu.c) is trusted with it.

Usage: rsp_dis.py <rom> <rom offset of the text> <byte length> [--list]
       --list  print every instruction; without it only the histogram of mnemonics.

The opcode tables are the standard MIPS ones for the scalar unit and the RSP vector unit's (COP2) function and LWC2/SWC2 tables
(n64brew "Reality Signal Processor/CPU Core" and "/Vector Unit"). An encoding that is not in the tables prints as `?` so that a
wrong table shows up as a count of `?`.
"""
import sys, collections

SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr", 13: "break", 32: "add", 33: "addu",
           34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor", 42: "slt", 43: "sltu"}
REGIMM = {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}
MAIN = {2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz", 8: "addi", 9: "addiu", 10: "slti", 11: "sltiu", 12: "andi",
        13: "ori", 14: "xori", 15: "lui", 32: "lb", 33: "lh", 35: "lw", 36: "lbu", 37: "lhu", 40: "sb", 41: "sh", 43: "sw"}
VU = {0: "vmulf", 1: "vmulu", 2: "vrndp", 3: "vmulq", 4: "vmudl", 5: "vmudm", 6: "vmudn", 7: "vmudh", 8: "vmacf", 9: "vmacu",
      10: "vrndn", 11: "vmacq", 12: "vmadl", 13: "vmadm", 14: "vmadn", 15: "vmadh", 16: "vadd", 17: "vsub", 19: "vabs", 20: "vaddc",
      21: "vsubc", 29: "vsar", 32: "vlt", 33: "veq", 34: "vne", 35: "vge", 36: "vcl", 37: "vch", 38: "vcr", 39: "vmrg", 40: "vand",
      41: "vnand", 42: "vor", 43: "vnor", 44: "vxor", 45: "vnxor", 48: "vrcp", 49: "vrcpl", 50: "vrcph", 51: "vmov", 52: "vrsq",
      53: "vrsql", 54: "vrsqh", 55: "vnop"}
LWC2 = {0: "lbv", 1: "lsv", 2: "llv", 3: "ldv", 4: "lqv", 5: "lrv", 6: "lpv", 7: "luv", 8: "lhv", 9: "lfv", 10: "lwv", 11: "ltv"}
SWC2 = {0: "sbv", 1: "ssv", 2: "slv", 3: "sdv", 4: "sqv", 5: "srv", 6: "spv", 7: "suv", 8: "shv", 9: "sfv", 10: "swv", 11: "stv"}


def mnemonic(w):
    op = w >> 26
    if op == 0:
        return SPECIAL.get(w & 63, "?")
    if op == 1:
        return REGIMM.get((w >> 16) & 31, "?")
    if op == 16:
        return {0: "mfc0", 4: "mtc0"}.get((w >> 21) & 31, "?")
    if op == 18:
        if (w >> 25) & 1:
            return VU.get(w & 63, "?")
        return {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}.get((w >> 21) & 31, "?")
    if op == 50:
        return LWC2.get((w >> 11) & 31, "?")
    if op == 58:
        return SWC2.get((w >> 11) & 31, "?")
    return MAIN.get(op, "?")


def main():
    rom = open(sys.argv[1], "rb").read()
    off, n = int(sys.argv[2], 0), int(sys.argv[3], 0)
    hist = collections.Counter()
    for i in range(0, n, 4):
        w = int.from_bytes(rom[off + i:off + i + 4], "big")
        m = mnemonic(w)
        hist[m] += 1
        if "--list" in sys.argv:
            print("%04X: %08X  %s" % (i, w, m))
    for m, c in sorted(hist.items(), key=lambda x: (-x[1], x[0])):
        print("%-8s %d" % (m, c))


main()
