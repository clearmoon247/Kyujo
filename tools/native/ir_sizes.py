#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Sizes of the global variables named after console addresses (`D_8xxxxxxx`), computed from LLVM IR text.

    ir_sizes.py <file.ll> [<file.ll> ...]        prints "name size" lines

WHY this exists: gen_platform_symbols.py has to know how big each such variable is (to decide which variable a linker-script symbol
points into). For ELF objects `llvm-nm -S` says so. COFF objects (the Windows target) do not record symbol sizes at all, so on Windows
every alias silently fell back to a copy of the ROM's data (found by comparing the alias counts of the two targets: 16 against 2).
The IR is the one place that has the type of each variable for BOTH targets, so the sizes are computed from it, with the data layout the
IR itself declares (`target datalayout = ...`), i.e. the layout the code generator of that target will use.

HOW (the rules, as LLVM's language reference states them):
  - scalar types: iN, float, double, x86_fp80, ptr: size = ceil(bits/8) rounded up to the ABI alignment of the type; alignment from the
    data layout string (defaults of LLVM when the string does not mention the type: i1/i8 1 byte, i16 2, i32 4, i64 4 (!), float 4,
    double 8, ptr as in `p:` entry).
  - array [N x T]: N * size_of_element_with_padding (alloc size of T).
  - struct { ... }: members in order, each aligned to its own ABI alignment; the size is rounded up to the struct's alignment (the largest
    member alignment, at least the `a:` aggregate entry of the layout). `<{ ... }>` is packed: alignment 1, no padding.
  - named struct %name = type {...} is looked up from its definition in the same file.
VALIDATION: tools/native/test_ir_sizes.py compares these numbers with llvm-nm -S for every object of an ELF build (they must be equal).
Only variables DEFINED in the file are reported (not `external` declarations, not aliases). Standard library only.
"""
import re
import sys

# LLVM's default layout entries (language reference, "Data Layout"): used when the module's string does not say otherwise.
DEFAULT_INT = {1: 1, 8: 1, 16: 2, 32: 4, 64: 4}        # bits -> ABI alignment in bytes (i64 is 4 by default: "i64:32:64")


class IrError(Exception):
    pass


def parse_layout(text):
    """Returns ({int bits: abi align bytes}, {'float':..,'double':..,'x86_fp80':..}, ptr_size, ptr_align, aggregate_min_align)."""
    ints = dict(DEFAULT_INT)
    fl = {"float": 4, "double": 8, "x86_fp80": 16}          # LLVM defaults; the data layout string normally overrides them
    ptr_size = ptr_align = 4
    agg = 1
    m = re.search(r'^target datalayout = "([^"]*)"', text, re.M)
    if not m:
        raise IrError("no target datalayout in the IR")
    for part in m.group(1).split("-"):
        f = part.split(":")
        if re.match(r"^i\d+$", f[0]) and len(f) >= 2:
            ints[int(f[0][1:])] = max(1, int(f[1]) // 8) if int(f[1]) else 1
        elif f[0] == "f32":
            fl["float"] = int(f[1]) // 8
        elif f[0] == "f64":
            fl["double"] = int(f[1]) // 8
        elif f[0] == "f80":
            fl["x86_fp80"] = int(f[1]) // 8
        elif f[0] == "p" and len(f) >= 3:
            ptr_size, ptr_align = int(f[1]) // 8, int(f[2]) // 8
        elif f[0] == "a" and len(f) >= 2:
            agg = max(1, int(f[1]) // 8)
    return ints, fl, ptr_size, ptr_align, agg


class Sizer:
    def __init__(self, text):
        self.text = text
        self.ints, self.fl, self.ptr_size, self.ptr_align, self.agg = parse_layout(text)
        self.named = {}
        for m in re.finditer(r"^(%[\w.$\-]+) = type (.*)$", text, re.M):
            self.named[m.group(1)] = m.group(2)
        self.cache = {}

    # Each parse function takes the text and a position and returns (size, align, new position), where `size` is the type's
    # store size WITHOUT trailing padding for scalars (they have none) and the full size for aggregates.
    def parse_type(self, s, i):
        while s[i] == " ":
            i += 1
        c = s[i]
        if c == "[":
            m = re.compile(r"\[\s*(\d+)\s*x\s*").match(s, i)
            if not m:
                raise IrError("bad array type near %r" % s[i:i + 40])
            n = int(m.group(1))
            size, align, j = self.parse_type(s, m.end())
            while s[j] == " ":
                j += 1
            if s[j] != "]":
                raise IrError("array type not closed near %r" % s[j:j + 40])
            alloc = (size + align - 1) // align * align
            return n * alloc, align, j + 1
        if c == "{" or s.startswith("<{", i):
            packed = c == "<"
            i += 2 if packed else 1
            off, al = 0, 1
            while True:
                while s[i] == " ":
                    i += 1
                if s[i] == "}":
                    i += 2 if packed else 1
                    break
                size, align, i = self.parse_type(s, i)
                if packed:
                    align = 1
                off = (off + align - 1) // align * align + size
                al = max(al, align)
                while s[i] == " ":
                    i += 1
                if s[i] == ",":
                    i += 1
            if not packed:
                al = max(al, self.agg)
            return (off + al - 1) // al * al, al, i
        if c == "%":
            m = re.compile(r"%[\w.$\-]+|%\"[^\"]*\"").match(s, i)
            name = m.group(0)
            if name not in self.cache:
                body = self.named.get(name)
                if body is None or body.strip() == "opaque":
                    raise IrError("type %s has no definition" % name)
                self.cache[name] = self.parse_type(body, 0)[:2]
            size, align = self.cache[name]
            return size, align, m.end()
        m = re.compile(r"i(\d+)").match(s, i)
        if m:
            bits = int(m.group(1))
            align = self.ints.get(bits)
            if align is None:                      # LLVM: the next larger integer entry, else the largest
                bigger = [b for b in sorted(self.ints) if b >= bits]
                align = self.ints[bigger[0]] if bigger else self.ints[max(self.ints)]
            return (bits + 7) // 8, align, m.end()
        for name, size in (("float", 4), ("double", 8), ("x86_fp80", 10)):
            if s.startswith(name, i):
                return size, self.fl[name], i + len(name)
        if s.startswith("ptr", i):
            return self.ptr_size, self.ptr_align, i + 3
        raise IrError("unknown type near %r" % s[i:i + 40])

    def global_sizes(self):
        """{name: size in bytes} of the defined globals called D_8xxxxxxx."""
        out = {}
        pat = re.compile(r"^@(D_8[0-9A-Fa-f]{7}) = (?!external)((?:[a-z_]+ )*?)(?:global|constant) ", re.M)
        for m in pat.finditer(self.text):
            size, align, _ = self.parse_type(self.text, m.end())
            out[m.group(1)] = size                   # the variable's size is the type's size (padding included for aggregates)
        return out


def ir_global_sizes(text):
    return Sizer(text).global_sizes()


def raise_alignment(text, min_size=8, align=8):
    """Gives every defined global variable of at least `min_size` bytes an alignment of at least `align` bytes. Returns (new text, count).

    WHY: the console's data is laid out by a linker that happened to align large objects, and the game relies on it where hardware
    needs it: the RCP's DMA engines ignore the low three bits of a RAM address (SP_DRAM_ADDR "8-byte aligned", n64brew; libultra's
    own checks demand 8 or 16), so a table that the game hands to a DMA (a microcode, a texture, an audio buffer) must start on an
    8-byte boundary. A table that the native linker placed on a 4-byte boundary made the DMA start 4 bytes early (found by running the GB
    Tower: its RSP microcode, a `const u32[]` of the decomp, was loaded shifted by one word and the microcode ran into a BREAK).
    Raising the alignment of every object of 8 bytes or more cannot hurt: nothing in the port depends on the addresses of variables
    being close together (aliases are `variable + offset`, not absolute).

    HOW: unoptimised clang IR prints each global definition on one line, ending in `, align N` (see the LLVM language reference,
    "Global Variables"); that N is rewritten, or added when missing. Declarations (`external`), aliases and LLVM's own `@llvm.*` are
    left alone. Names, types and values are untouched."""
    sizer = Sizer(text)
    pat = re.compile(r"^(@[^\s=]+) = (?!external)((?:[a-z_]+ )*?)(global|constant) ", re.M)
    out, last, count = [], 0, 0
    for m in pat.finditer(text):
        name = m.group(1)
        if name.startswith("@llvm."):
            continue
        eol = text.find("\n", m.end())
        eol = len(text) if eol < 0 else eol
        size, _, _ = sizer.parse_type(text, m.end())
        if size < min_size:
            continue
        line = text[m.start():eol]
        am = re.search(r", align (\d+)\s*$", line)
        if am and int(am.group(1)) >= align:
            continue
        new_line = (line[:am.start()] if am else line) + ", align %d" % align
        out.append(text[last:m.start()])
        out.append(new_line)
        last = eol
        count += 1
    out.append(text[last:])
    return "".join(out), count


if __name__ == "__main__":
    for path in sys.argv[1:]:
        for name, size in sorted(ir_global_sizes(open(path).read()).items()):
            print(name, size)
