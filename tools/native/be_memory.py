#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""be_memory.py - makes compiled game code see memory the way the Nintendo 64 did: big-endian.

WHY THIS EXISTS
  The decompiled game was written for a big-endian CPU (MIPS). Its data comes straight from the cartridge as big-endian bytes, and
  the code reads that data through ordinary C types: `*(u32 *)p`, `((u16 *)p)[i]`, unions of a float and four bytes, display lists
  made of 32-bit words, and so on. A little-endian PC reads all of those wrongly. Converting every kind of data by hand would
  need the layout of every asset format, and a missed case would be a silent error.

  This tool removes the whole problem at the source: it rewrites the compiler's intermediate code (LLVM IR) so that EVERY memory
  load or store of a 16, 32 or 64-bit integer, a float, a double or a pointer is byte-reversed. The program then keeps all its
  data in memory in big-endian byte order - exactly the byte image the MIPS program had - while values held in CPU registers
  stay in the PC's normal form. Consequences:
    * data read from the ROM is copied byte-for-byte and is correct as it is, whatever its type;
    * byte accesses (`u8 *`) and mixed accesses (unions, casts) behave as on the console;
    * code that interprets memory by hand (the renderer reading display lists, the audio code reading command lists) reads it
      with an explicit big-endian read, as every N64 emulator does.

WHAT IT DOES, IN FOUR RULES  (the input is clang's UNOPTIMISED IR: `-Xclang -disable-llvm-passes -fno-discard-value-names`)
  1. load/store of i16, i32, i64, float, double, ptr (pointers are 32 bits: i386)  ->  the same access of an integer of that size,
     with `llvm.bswap` applied to the value. Constants being stored are reversed here, at build time.
     i8 and i1 accesses are not touched. Anything else that touches memory by value (long double, vectors, aggregates, atomics,
     unknown `llvm.*` intrinsics, inline assembly) is REFUSED with an error rather than guessed at.
     Copies by `llvm.memcpy`/`memmove`/`memset` move bytes and need no change.
  2. Initialised global data is written by the compiler in little-endian form. It cannot be reversed at build time because
     pointers inside it are only known to the linker. So each object file gets a small function that reverses every multi-byte
     scalar of its initialised globals ONCE when the program starts (host_swap_game_data() in src/platform/host_be.c runs
     them). Globals that are constant are made writable for this reason; all-zero globals are left alone (they stay in .bss).
  3. Variable-argument functions: the arguments are pushed by the CALLER in the machine's own form and are read by `va_arg`
     through the pointer clang names `argp.cur`. That one kind of load must stay native. Everything else about `va_list` is
     consistent as it is, except that the cursor written by `va_start` is reversed once after the call so that the ordinary
     (reversed) cursor loads and stores that follow agree with it.
  4. C bit-fields are laid out from the LEAST significant bit on a PC but from the MOST significant bit on MIPS, so a reversed
     load cannot make them agree. Any bit-field access (`%bf.*` values in the IR) is REFUSED unless the source file is listed
     as a known case; such files then print a warning. The planned fix is accessor macros (docs/architecture.md).

  5. (only when asked for with preempt=True, which compile_decomp.py does for the decomp's own sources, never for this port's
     hardware model) Scheduling points: before every branch that goes BACK to an earlier block of the same function (the end of
     each loop iteration) the code checks a flag that is set while a device event is waiting (`host_preempt_flag`, set by
     host_event_post) and, if set, calls HostPreemptPoll() (src/ultra/os_sched.c), which delivers the event as the console's
     interrupt would be delivered. Without this, a loop that waits for another thread to change a variable and never calls into the
     operating system (the game has such loops, e.g. Audio_WaitForIdle) would run forever, because the console's timer and audio
     interrupts could never reach the other threads. The check is a volatile read of a host-order int plus a branch, so it is cheap.

  6. (always) Missing `return`: the decomp has non-void functions that run off their end ("non-void function does not return a value",
     35 places, clang tag -Wreturn-type). On the console the caller then simply sees whatever the function last left in register v0,
     so some of them are used that way. In clang's C output the result of such a path is an undefined value, which the optimiser
     may turn into anything. Every non-void function therefore starts with a store of zero into its return slot (`%retval`), so a
     path without a `return` yields 0, the same on every run. A function that always returns explicitly is not changed (the
     optimiser drops the dead store). Functions whose callers rely on a better value than 0 are fixed by a port patch instead.

  7. (always) Stack slots of small character arrays are rounded up to a whole 4 bytes. The console's compiler gave every local array a
     whole number of 4-byte words, so code that writes one byte more than the declared length (the usual case is a text buffer
     declared `s8 buf[2]` that receives "10" and its terminating zero) stayed inside the array's own slot. clang places a
     `[2 x i8]` in exactly 2 bytes, and the third byte lands on the neighbouring variable. In this port that neighbour was a pointer
     stored in big-endian order, so the zero byte cleared its most significant byte: the stat-box level text of the battle screen
     ran off into random memory and drew garbage ("10 ~~~ OK"). The rounding gives the array back the room it had on the console.
     It changes nothing for code that stays inside the declared length, and `sizeof` is not affected (clang has already folded it).

WHAT IS NOT COVERED (stated so nobody assumes otherwise)
  * Only 32-bit-pointer targets (i386). A 64-bit build needs a pointer-compression design first; this tool refuses it.
  * Memory touched by the C library or the host (printf's output buffer, file I/O) is not reversed; host code must receive plain
    byte buffers, or use explicit big-endian reads for game structures. See platform.h.
  * The debugger shows reversed values for multi-byte variables (the bytes in memory are big-endian).

Standard library only. Called from compile_decomp.py; can also be run by hand:   be_memory.py in.ll -o out.ll
"""
import argparse
import re
import struct
import sys


class BeError(Exception):
    """The input contains something this tool refuses to translate (the message says what and where)."""


# --------------------------------------------------------------------------------------------------------------------------------
# Small text helpers
# --------------------------------------------------------------------------------------------------------------------------------

_OPEN, _CLOSE = "([{<", ")]}>"


def split_top(s, sep=","):
    """Splits `s` at `sep` characters that are not inside (), [], {}, <> or double quotes. LLVM IR operands can contain commas
    inside constant expressions such as `getelementptr inbounds ([8 x i32], ptr @t, i32 0, i32 1)`."""
    parts, depth, start, i, in_q = [], 0, 0, 0, False
    while i < len(s):
        c = s[i]
        if in_q:
            if c == "\\":
                i += 1
            elif c == '"':
                in_q = False
        elif c == '"':
            in_q = True
        elif c in _OPEN:
            depth += 1
        elif c in _CLOSE:
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(s[start:i])
            start = i + 1
        i += 1
    parts.append(s[start:])
    return parts


def to_signed(v, bits):
    """LLVM prints integer constants as signed decimals; mask first, then fold into the signed range."""
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def bswap_int(v, bits):
    return int.from_bytes((v & ((1 << bits) - 1)).to_bytes(bits // 8, "big"), "little")


def float_bits(token, kind):
    """Bit pattern (unsigned) of an LLVM float/double constant written as `token`.
       LLVM writes a constant either in decimal (`1.500000e+00`, only when that reads back exactly) or as the 16 hex digits of
       the value widened to double (`0x3FF8000000000000`), also for `float`."""
    tok = token.strip()
    if tok.startswith("0x"):
        if tok[2:3] in "HKLM" or len(tok) != 18:
            raise BeError("unsupported floating constant form %r (half/x87/fp128?)" % token)
        d_bits = int(tok[2:], 16)
        if kind == "double":
            return d_bits
        sign, exp, mant = d_bits >> 63, (d_bits >> 52) & 0x7FF, d_bits & ((1 << 52) - 1)
        if exp == 0x7FF:                                  # infinity / NaN: keep the top payload bits
            return (sign << 31) | (0xFF << 23) | (mant >> 29)
        (v,) = struct.unpack("<d", d_bits.to_bytes(8, "little"))
        return int.from_bytes(struct.pack("<f", v), "little")
    try:
        v = float(tok)
    except ValueError:
        raise BeError("cannot read floating constant %r" % token)
    return int.from_bytes(struct.pack("<d" if kind == "double" else "<f", v), "little")


# --------------------------------------------------------------------------------------------------------------------------------
# Types (needed only to find the multi-byte scalars inside initialised globals)
# --------------------------------------------------------------------------------------------------------------------------------
# A type is a tuple: ("int", bits) ("float",) ("double",) ("ptr",) ("other", text) ("named", name)
#                    ("array", count, elem) ("struct", [elems])      (every tuple also carries its source text as last item)

_NAME = r'(?:%[\w.$-]+|%"[^"]*")'


def parse_type(s, i=0):
    """Parses one LLVM type starting at s[i]; returns (type, next index)."""
    while s[i] == " ":
        i += 1
    start = i
    c = s[i]
    if c == "[":                                           # [N x T]
        m = re.compile(r"\[\s*(\d+)\s+x\s+").match(s, i)
        if not m:
            raise BeError("cannot parse array type at %r" % s[i:i + 40])
        elem, j = parse_type(s, m.end())
        while s[j] == " ":
            j += 1
        if s[j] != "]":
            raise BeError("cannot parse array type at %r" % s[i:i + 40])
        return ("array", int(m.group(1)), elem, s[start:j + 1]), j + 1
    if c == "{" or s.startswith("<{", i):                  # { T, T }   or packed   <{ T, T }>
        packed = c == "<"
        j = i + (2 if packed else 1)
        elems = []
        while True:
            while s[j] == " ":
                j += 1
            if s[j] == "}":
                j += 1
                break
            t, j = parse_type(s, j)
            elems.append(t)
            while s[j] == " ":
                j += 1
            if s[j] == ",":
                j += 1
        if packed:
            if s[j] != ">":
                raise BeError("cannot parse packed struct at %r" % s[i:i + 40])
            j += 1
        return ("struct", elems, s[start:j]), j
    if c == "<":                                           # vector <N x T>
        m = re.compile(r"<[^>]*>").match(s, i)
        return ("other", m.group(0), m.group(0)), m.end()
    m = re.compile(_NAME).match(s, i)
    if m:
        return ("named", m.group(0), m.group(0)), m.end()
    m = re.compile(r"i(\d+)|float|double|ptr|half|bfloat|fp128|x86_fp80|ppc_fp128|void|label|metadata|opaque").match(s, i)
    if not m:
        raise BeError("cannot parse type at %r" % s[i:i + 40])
    w = m.group(0)
    if w.startswith("i"):
        return ("int", int(w[1:]), w), m.end()
    if w in ("float", "double", "ptr"):
        return (w, w), m.end()
    return ("other", w, w), m.end()


def type_text(t):
    return t[-1]


class Types:
    """The module's `%name = type ...` definitions, so `%struct.S` can be looked into."""

    def __init__(self, lines):
        self.defs = {}
        rx = re.compile(r"^(" + _NAME + r") = type (.*)$")
        for ln in lines:
            m = rx.match(ln)
            if m and m.group(2).strip() != "opaque":
                self.defs[m.group(1)], _ = parse_type(m.group(2))
        self._memo = {}

    def resolve(self, t):
        while t[0] == "named":
            if t[1] not in self.defs:
                return ("other", "opaque", "opaque")
            t = self.defs[t[1]]
        return t

    def leaf_bits(self, t):
        """Size in bits of a scalar that has to be reversed (16/32/64), 0 for single bytes, None for aggregates."""
        t = self.resolve(t)
        if t[0] == "int":
            if t[1] in (1, 8):
                return 0
            if t[1] in (16, 32, 64):
                return t[1]
            raise BeError("integer type %s in initialised data is not supported" % t[2])
        if t[0] == "float":
            return 32
        if t[0] in ("double",):
            return 64
        if t[0] == "ptr":
            return 32
        if t[0] == "other":
            raise BeError("type %s in initialised data is not supported" % t[1])
        return None

    def has_leaves(self, t):
        key = type_text(t)              # not id(t): ids of short-lived tuples are reused
        if key in self._memo:
            return self._memo[key]
        self._memo[key] = False                     # recursion guard (types cannot contain themselves by value anyway)
        r = self.resolve(t)
        b = self.leaf_bits(r)
        if b is not None:
            res = b > 0
        elif r[0] == "array":
            res = r[1] > 0 and self.has_leaves(r[2])
        else:
            res = any(self.has_leaves(e) for e in r[1])
        self._memo[key] = res
        return res


# --------------------------------------------------------------------------------------------------------------------------------
# Start-up function that reverses initialised data (rule 2)
# --------------------------------------------------------------------------------------------------------------------------------

class FixEmitter:
    """Writes the body of `@__be_fix`: for every swappable global, loops over its arrays and reverses each multi-byte scalar in place."""

    UNROLL = 4                                       # arrays up to this many elements are written out, longer ones become loops

    def __init__(self, types):
        self.types = types
        self.lines = []
        self.n = 0
        self.block = "entry"

    def name(self, hint):
        self.n += 1
        return "%%fix.%s.%d" % (hint, self.n)

    def label(self, hint):
        self.n += 1
        return "fix.%s.%d" % (hint, self.n)

    def start(self, label):
        self.lines.append(label + ":")
        self.block = label

    def swap_at(self, ptr, bits):
        a, b = self.name("v"), self.name("s")
        self.lines.append("  %s = load i%d, ptr %s, align 1" % (a, bits, ptr))
        self.lines.append("  %s = call i%d @llvm.bswap.i%d(i%d %s)" % (b, bits, bits, bits, a))
        self.lines.append("  store i%d %s, ptr %s, align 1" % (bits, b, ptr))

    def walk(self, t, ptr):
        """Emits code that reverses all scalars of type `t` stored at `ptr`."""
        if not self.types.has_leaves(t):
            return
        r = self.types.resolve(t)
        bits = self.types.leaf_bits(r)
        if bits:
            self.swap_at(ptr, bits)
        elif r[0] == "struct":
            for idx, e in enumerate(r[1]):
                if self.types.has_leaves(e):
                    p = self.name("p")
                    self.lines.append("  %s = getelementptr inbounds %s, ptr %s, i32 0, i32 %d" % (p, type_text(t), ptr, idx))
                    self.walk(e, p)
        elif r[0] == "array":
            n, elem = r[1], r[2]
            if n <= self.UNROLL:
                for i in range(n):
                    p = self.name("p")
                    self.lines.append("  %s = getelementptr inbounds %s, ptr %s, i32 0, i32 %d" % (p, type_text(t), ptr, i))
                    self.walk(elem, p)
            else:
                head, body, done = self.label("head"), self.label("body"), self.label("done")
                before = self.block
                i, nxt, cond, p = self.name("i"), self.name("n"), self.name("c"), self.name("p")
                self.lines.append("  br label %%%s" % head)
                self.start(head)
                self.lines.append("  %s = phi i32 [ 0, %%%s ], [ %s, %%%s ]" % (i, before, nxt, "LATCH"))
                latch_phi_index = len(self.lines) - 1
                self.lines.append("  %s = icmp ult i32 %s, %d" % (cond, i, n))
                self.lines.append("  br i1 %s, label %%%s, label %%%s" % (cond, body, done))
                self.start(body)
                self.lines.append("  %s = getelementptr inbounds %s, ptr %s, i32 0, i32 %s" % (p, type_text(t), ptr, i))
                self.walk(elem, p)
                latch = self.block                     # the block the (possibly nested) body ended in
                self.lines.append("  %s = add nuw i32 %s, 1" % (nxt, i))
                self.lines.append("  br label %%%s" % head)
                self.lines[latch_phi_index] = self.lines[latch_phi_index].replace("%LATCH", "%" + latch)
                self.start(done)

    def finish(self):
        return ["define internal void @__be_fix() noinline nounwind {", "entry:"] + self.lines + ["  ret void", "}"]


_GLOBAL_RE = re.compile(r"^(@(?:[\w.$-]+|\"[^\"]*\")) = (?P<attrs>[^=]*?)\b(?P<kind>global|constant)\b (?P<rest>.*)$")
_ZERO_INIT = re.compile(r"^(zeroinitializer|undef|poison|null|false|0|-?0\.0+e\+00)$")


def process_global(line, types, fix):
    """If `line` defines an initialised global with multi-byte scalars: records its fix-up code and returns the line made writable.
       Otherwise returns it unchanged."""
    m = _GLOBAL_RE.match(line)
    if not m or "external " in m.group("attrs") or line.startswith("@llvm."):
        return line
    gtype, j = parse_type(m.group("rest"))
    tail = m.group("rest")[j:]
    init = split_top(tail)[0].strip()
    if _ZERO_INIT.match(init):
        return line                                           # all zero: nothing to reverse, and it can stay in .bss
    if not types.has_leaves(gtype):
        return line                                           # only bytes (strings, byte tables)
    if re.search(r"\b(weak|linkonce|linkonce_odr|weak_odr|common|extern_weak)\b", m.group("attrs")):
        raise BeError("initialised global with weak/common linkage cannot be reversed once: " + line[:100])
    if re.search(r'\bsection "', tail) or re.search(r'\bthread_local\b', m.group("attrs")):
        raise BeError("global with an explicit section or thread-local storage is not supported: " + line[:100])
    name = m.group(1)
    fix.walk(gtype, name)
    if m.group("kind") == "constant":                         # the start-up function writes to it, so it cannot live in read-only memory
        start = m.start("kind")
        line = line[:start] + "global" + line[m.end("kind"):]
    return line


def fragment_section_names(index, windows):
    """(data section, zero-filled section) of code overlay number `index` (see assign_fragment_section).
       ELF: one output section each, named like a C identifier so that the linker provides __start_<name>/__stop_<name>.
       COFF: the PE format allows only 96 sections, so all overlays share two output sections (`.fdat` and `.fzer`) and the part after
       `$` keeps one overlay's pieces together and in order: `<n>a` (begin marker, defined by the generated fragment_data.c), `<n>m`
       (the variables), `<n>z` (end marker). The linker sorts the part after `$` alphabetically (the same mechanism as `.befix`)."""
    if windows:
        return ".fdat$%02dm" % index, ".fzer$%02dm" % index
    return "fd%02d" % index, "fz%02d" % index


def assign_fragment_section(line, index, windows):
    """Puts a defined, writable global variable of a code overlay's source file into the overlay's own section, so that the program can
    restore the overlay's variables to their start-up state whenever the game loads the overlay again (host_fragment.c).
    Why: on the console every load of an overlay copies its data from the ROM and clears its zero-filled part, so the overlay starts from
    scratch each time. Natively the overlay is part of the program and its variables are ordinary statics; without this a second visit
    to a screen starts from the state the first visit left behind (found by backing out of the GB Tower and getting a black Area Select).
    Variables with initial values go to the first section (their start-up values are saved after the big-endian reversal has run),
    zero-initialised ones to the second (restored by clearing). Constants, declarations, thread-locals and variables that already have
    a section are left alone."""
    m = _GLOBAL_RE.match(line)
    if not m or line.startswith("@llvm.") or line.startswith("@__be_"):
        return line
    attrs = m.group("attrs")
    if "external " in attrs or "extern_weak" in attrs or m.group("kind") != "global" or re.search(r"\bthread_local\b", attrs):
        return line
    gtype, j = parse_type(m.group("rest"))
    tail = m.group("rest")[j:]
    parts = split_top(tail)
    if re.search(r'\bsection "', tail) or re.search(r"\b(weak|linkonce|linkonce_odr|weak_odr|common)\b", attrs):
        return line
    data_sec, zero_sec = fragment_section_names(index, windows)
    sec = zero_sec if _ZERO_INIT.match(parts[0].strip()) else data_sec
    new_tail = parts[0] + ', section "%s"' % sec + "".join("," + p for p in parts[1:])
    return line[:m.start("rest")] + m.group("rest")[:j] + new_tail


_CONSOLE_ADDR = re.compile(r'^@"?(?:[\w$-]*\.)?D_([0-9A-Fa-f]{8})\b')
_GLOBAL_NAME = re.compile(r'^@"?([^"\s=]+)"?\s*=')


def console_address(line, symbol_addresses=None):
    """The address the console linked a variable at, or None when it is not known.
    Known from the name when the decomp calls the variable `D_<address>` (or a function-local static `<function>.D_<address>`), and
    otherwise from `symbol_addresses` ({name: address}, the decomp's documented symbols), looked up by the whole name and by the part after
    the last dot (a function-local static `osGbSetNextBuffer.hdwrBugFlag` is documented as `hdwrBugFlag`)."""
    m = _CONSOLE_ADDR.match(line)
    if m:
        return int(m.group(1), 16)
    m = _GLOBAL_NAME.match(line)
    if m and symbol_addresses:
        name = m.group(1)
        for candidate in (name, name.rsplit(".", 1)[-1]):
            if candidate in symbol_addresses:
                return symbol_addresses[candidate]
    return None


def order_fragment_globals(out, indices, symbol_addresses=None):
    """Puts the variables of one code overlay in the order of their console addresses.
    Why: a DMA or a memcpy into a variable may legitimately run past the variable's declared size into the next one: the decomp declares
    `D_86B0EBE8[0xA60]` and `D_86B0F648[0x180]` one after the other, and the game loads 0xBC0 bytes (both) into the first. Clang emits file-level
    statics in the order of their FIRST USE, not of declaration, so natively the next variable in memory was an unrelated one and the load
    overwrote it (the pointer of the Completion scene's model tree: crash in Completion_InitCamera). With the console's order the overrun
    lands on the variable that the console had there.
    `indices` are the positions in `out` of the overlay's variable definitions that went into an overlay section. Each section keeps its own
    order. A variable whose address is not known (see console_address) takes the place of the nearest known variable before it in clang's
    order: it stays right behind that one. (Putting such variables first, as an earlier version did, moved the audio state of the Game Boy
    emulator away from its neighbours and corrupted it: the Game Boy Tower crashed in func_81209374.) The sort is stable."""
    by_section = {}
    for i in indices:
        m = re.search(r'section "([^"]+)"', out[i])
        by_section.setdefault(m.group(1) if m else "", []).append(i)
    for positions in by_section.values():
        keyed = []
        key = -1                                   # before everything, for unknown variables that come before the first known one
        for i in positions:
            addr = console_address(out[i], symbol_addresses)
            if addr is not None:
                key = addr
            keyed.append((key, out[i]))
        keyed.sort(key=lambda kv: kv[0])
        for i, (_, ln) in zip(positions, keyed):
            out[i] = ln


# --------------------------------------------------------------------------------------------------------------------------------
# Function bodies (rules 1, 3, 4)
# --------------------------------------------------------------------------------------------------------------------------------

# Intrinsics that are known not to read or write multi-byte values through memory in a way that matters.
_OK_INTRINSICS = ("lifetime.", "memcpy.", "memmove.", "memset.", "va_start", "va_end", "va_copy", "bswap.", "fabs.", "sqrt.",
                  "floor.", "ceil.", "trunc.", "round.", "rint.", "nearbyint.", "copysign.", "minnum.", "maxnum.", "fma.",
                  "fmuladd.", "ctlz.", "cttz.", "ctpop.", "abs.", "smax.", "smin.", "umax.", "umin.", "expect.", "assume",
                  "trap", "ubsantrap", "debugtrap", "objectsize.", "is.constant.", "stacksave", "stackrestore", "sadd.with.overflow.",
                  "uadd.with.overflow.", "ssub.with.overflow.", "usub.with.overflow.", "smul.with.overflow.",
                  "umul.with.overflow.", "donothing", "bitreverse.", "fshl.", "fshr.", "prefetch.", "ptrmask.")
_BAD_WORDS = re.compile(r"\b(atomicrmw|cmpxchg|va_arg|fence)\b|\bcall\b[^;]*\basm\b")
_INT_SCALARS = {"i16": 16, "i32": 32, "i64": 64}
# Indirect calls. The game calls code through pointers that, on the console, may point into an overlay ("fragment") loaded into the
# memory pool; natively that code is linked into the program and the pool holds only a dead copy. So the target of every indirect call
# goes through this function (defined on the game side, src/game_asm/fragment_native.c), which returns the pointer unchanged unless it
# points into a loaded fragment, in which case it returns the native function that stands for it.
CALL_HOOK = "HostResolveCallTarget"
# `call <return type> [<function type>] %callee(args)`: the callee is a local value directly followed by "(". Direct calls name a
# global (@f) and inline assembly was refused earlier, so any %name( in a call line is the callee (a type such as %struct.S is
# followed by a space or a comma, never directly by "(").
_INDIRECT_CALL = re.compile(r"\bcall\b[^@]*?\s(%[\w.$\-]+)\(")
_NATIVE_LOAD_PTR = re.compile(r"^%argp\.cur")               # clang's name for the pointer that va_arg reads an argument through


class FuncRewriter:
    def __init__(self, fname, source_name, allow_bitfields, stats):
        self.fname, self.source, self.allow_bf, self.stats = fname, source_name, allow_bitfields, stats
        self.n = 0

    def tmp(self):
        self.n += 1
        return "%%be.%d" % self.n

    def err(self, msg, line):
        raise BeError("%s: in function %s: %s\n    %s" % (self.source, self.fname, msg, line.strip()[:200]))

    # --- rule 5: scheduling points before backward branches ---------------------------------------------------------------------
    _LABEL = re.compile(r'^([\w.$-]+|"[^"]+"):')
    _BR = re.compile(r"^\s*br\s")
    _TARGET = re.compile(r"label %([\w.$-]+|\"[^\"]+\")")
    _PHI = re.compile(r"^\s*%[^\s=]+ = phi ")

    def process_function(self, lines, preempt):
        """`lines`: the body of one function (the lines after `define` up to, not including, the closing brace).
           Returns the rewritten lines (rules 1, 3, 4 per instruction, then rule 5 if `preempt`)."""
        out = []
        for ln in lines:
            if ln.startswith("  ") or ln.startswith("\t"):
                out.extend(self.rewrite(ln))
            else:
                out.append(ln)
        self.pad_char_arrays(out)                                # rule 7
        self.zero_return_slot(out)                               # rule 6
        if not preempt:
            return out
        order, cur = {}, "entry"
        for ln in out:                                           # block order as printed; the first block is clang's `entry`
            m = self._LABEL.match(ln)
            if m:
                order[m.group(1)] = len(order)
        order.setdefault("entry", -1)
        res, splits = [], {}                                     # splits: successor block -> [(old label, new label)]
        cur = "entry"
        for ln in out:
            m = self._LABEL.match(ln)
            if m:
                cur = m.group(1)
            elif self._BR.match(ln):
                targets = self._TARGET.findall(ln)
                if any(order.get(t, 1 << 30) <= order.get(cur, -1) for t in targets):
                    self.stats["preempt"] += 1
                    k = self.stats["preempt"]
                    new = "be.cont.%d" % k
                    res += ["  %%be.pf.%d = load volatile i32, ptr @host_preempt_flag, align 4" % k,
                            "  %%be.pc.%d = icmp ne i32 %%be.pf.%d, 0" % (k, k),
                            "  br i1 %%be.pc.%d, label %%be.poll.%d, label %%%s" % (k, k, new),
                            "be.poll.%d:" % k,
                            "  call void @HostPreemptPoll()",
                            "  br label %%%s" % new,
                            "%s:" % new]
                    for t in set(targets):
                        splits.setdefault(t, []).append((cur, new))
            res.append(ln)
        if splits:                                               # phi nodes name the predecessor block: it is now the new block
            cur = "entry"
            for i, ln in enumerate(res):
                m = self._LABEL.match(ln)
                if m:
                    cur = m.group(1)
                elif cur in splits and self._PHI.match(ln):
                    for old, new in splits[cur]:
                        ln = re.sub(r"(,\s*)%" + re.escape(old) + r"(\s*\])", r"\1%" + new + r"\2", ln)
                    res[i] = ln
        return res

    _SMALL_ARRAY = re.compile(r"^(\s*%[\w.]+ = alloca )\[(\d+) x i8\](.*)$")

    def pad_char_arrays(self, out):
        """Rule 7: rounds the size of each local `[N x i8]` stack slot up to a multiple of 4. Only the leading allocas are looked at
           (clang puts all of a function's allocas at its very start)."""
        for i, ln in enumerate(out):
            if self._LABEL.match(ln):                            # clang prints the `entry:` label before the allocas
                continue
            if not self._ALLOCA.match(ln):
                return
            m = self._SMALL_ARRAY.match(ln)
            if m:
                n = int(m.group(2))
                if n % 4:
                    out[i] = "%s[%d x i8]%s" % (m.group(1), (n + 3) // 4 * 4, m.group(3))
                    self.stats["padded_arrays"] += 1

    _RETVAL = re.compile(r"^\s*%retval = alloca (.+), align (\d+)\s*$")
    _ALLOCA = re.compile(r"^\s*%[\w.]+ = alloca ")

    def zero_return_slot(self, out):
        """Rule 6: puts `store <type> zeroinitializer, ptr %retval` after the leading allocas of a function that has a return slot.
           (Zero is the same bit pattern in either byte order, so it needs no reversal.) Only the first, `entry`, block is looked at:
           clang puts all of a function's allocas at its very start."""
        for i, ln in enumerate(out):
            if self._LABEL.match(ln):                            # clang prints the `entry:` label before the allocas
                continue
            m = self._RETVAL.match(ln)
            if m:
                j = i
                while j + 1 < len(out) and self._ALLOCA.match(out[j + 1]):
                    j += 1
                out.insert(j + 1, "  store %s zeroinitializer, ptr %%retval, align %s" % (m.group(1), m.group(2)))
                self.stats["retzero"] += 1
                return
            if not self._ALLOCA.match(ln):                       # past the leading allocas: this function has no return slot
                return

    # --- helpers that emit the instructions for one swapped access --------------------------------------------------------------
    def swap_value(self, out, ty, val):
        """Appends instructions that produce the byte-reversed integer form of `val` (of IR type `ty`); returns (name or literal, int type)."""
        if ty in _INT_SCALARS:
            bits = _INT_SCALARS[ty]
            if re.fullmatch(r"-?\d+", val):
                return str(to_signed(bswap_int(int(val), bits), bits)), ty
            if val in ("undef", "poison"):
                return val, ty
            r = self.tmp()
            out.append("  %s = call %s @llvm.bswap.%s(%s %s)" % (r, ty, ty, ty, val))
            return r, ty
        if ty in ("float", "double"):
            ity, bits = ("i32", 32) if ty == "float" else ("i64", 64)
            if val in ("undef", "poison"):
                return val, ity
            if re.fullmatch(r"-?\d+\.\d+e[+-]\d+|0x[0-9A-Fa-f]+", val):
                return str(to_signed(bswap_int(float_bits(val, ty), bits), bits)), ity
            c, r = self.tmp(), self.tmp()
            out.append("  %s = bitcast %s %s to %s" % (c, ty, val, ity))
            out.append("  %s = call %s @llvm.bswap.%s(%s %s)" % (r, ity, ity, ity, c))
            return r, ity
        if ty == "ptr":
            if val == "null":
                return "0", "i32"
            if val in ("undef", "poison"):
                return val, "i32"
            c, r = self.tmp(), self.tmp()
            out.append("  %s = ptrtoint ptr %s to i32" % (c, val))
            out.append("  %s = call i32 @llvm.bswap.i32(i32 %s)" % (r, c))
            return r, "i32"
        raise BeError("unreachable type " + ty)

    # --- line rewriting ------------------------------------------------------------------------------------------------------------
    def rewrite(self, line):
        """Returns the list of lines that replace `line`."""
        s = line.strip()
        if "%bf." in s:
            self.stats["bitfield"].append(self.fname)
            if self.source not in self.allow_bf:
                self.err("bit-field access: bit-fields are allocated from the other end of the word on MIPS, so a byte-reversed "
                         "access cannot reproduce them. Replace the bit-field by shifts and masks (see docs/architecture.md), or "
                         "list the file in port/bitfield_known.txt until that is done", line)
            return [line]          # a listed file: leave the access native (its bit-field VALUES are wrong, as the warning says)
        if _BAD_WORDS.search(s):
            self.err("construct not supported by the big-endian rewrite (atomic, fence, va_arg instruction or inline assembly)", line)
        for im in re.finditer(r"@llvm\.([\w.]+)", s):
            if not im.group(1).startswith(_OK_INTRINSICS):
                self.err("intrinsic llvm.%s has not been reviewed for byte order; add it to _OK_INTRINSICS if it is safe" % im.group(1), line)

        ind = line[:len(line) - len(line.lstrip())]
        # ---- load -----------------------------------------------------------------------------------------------------------------
        if " = load " in s:
            m = re.match(r"^(%[^\s=]+) = load (volatile )?(.*)$", s)
            if not m:
                self.err("unrecognised load (atomic?)", line)
            res, vol, body = m.group(1), m.group(2) or "", m.group(3)
            parts = split_top(body)
            ty = parts[0].strip()
            if len(parts) < 2 or not parts[1].strip().startswith("ptr "):
                self.err("load that is not of the form `load T, ptr P`", line)
            ptr = parts[1].strip()[4:]
            tail = [p for p in parts[2:] if p.strip().startswith("align")]        # keep alignment, drop value-specific metadata
            tail_s = "".join("," + p for p in tail)
            if ty in ("i8", "i1"):
                return [line]
            if _NATIVE_LOAD_PTR.match(ptr):                       # va_arg reads the caller's pushed argument: native form (rule 3)
                return [line]
            if ty not in _INT_SCALARS and ty not in ("float", "double", "ptr"):
                self.err("load of type %r is not supported (aggregate, vector, long double ...)" % ty, line)
            self.stats["loads"] += 1
            ity = ty if ty in _INT_SCALARS else {"float": "i32", "double": "i64", "ptr": "i32"}[ty]
            raw = self.tmp()
            out = ["%s%s = load %s%s, ptr %s%s" % (ind, raw, vol, ity, ptr, tail_s)]
            if ty in _INT_SCALARS:
                out.append("%s%s = call %s @llvm.bswap.%s(%s %s)" % (ind, res, ity, ity, ity, raw))
            else:
                sw = self.tmp()
                out.append("%s%s = call %s @llvm.bswap.%s(%s %s)" % (ind, sw, ity, ity, ity, raw))
                out.append("%s%s = %s %s %s to %s" % (ind, res, "inttoptr" if ty == "ptr" else "bitcast", ity, sw, ty))
            return out
        # ---- store ----------------------------------------------------------------------------------------------------------------
        if s.startswith("store "):
            m = re.match(r"^store (volatile )?(.*)$", s)
            if not m:
                self.err("unrecognised store (atomic?)", line)
            vol, body = m.group(1) or "", m.group(2)
            parts = split_top(body)
            first = parts[0].strip()
            ty, _, val = first.partition(" ")
            if len(parts) < 2 or not parts[1].strip().startswith("ptr "):
                self.err("store that is not of the form `store T V, ptr P`", line)
            ptr = parts[1].strip()[4:]
            tail = [p for p in parts[2:] if p.strip().startswith("align")]
            tail_s = "".join("," + p for p in tail)
            if ty in ("i8", "i1"):
                return [line]
            if ty not in _INT_SCALARS and ty not in ("float", "double", "ptr"):
                self.err("store of type %r is not supported (aggregate, vector, long double ...)" % ty, line)
            self.stats["stores"] += 1
            out = []
            v, ity = self.swap_value(out, ty, val.strip())
            out = [ind + o.strip() if o.strip() else o for o in out]
            out.append("%sstore %s%s %s, ptr %s%s" % (ind, vol, ity, v, ptr, tail_s))
            return out
        # ---- indirect call: the target goes through HostResolveCallTarget (see CALL_HOOK) ------------------------------------------
        m = _INDIRECT_CALL.search(s)
        if m and not s.startswith(("declare ", "define ")):
            callee = m.group(1)
            t = self.tmp()
            self.stats["indirect_calls"] += 1
            at = len(ind) + m.start(1)                       # position of the callee in `line` (s is line without its indentation)
            return ["%s%s = call ptr @%s(ptr %s)" % (ind, t, CALL_HOOK, callee),
                    line[:at] + t + line[at + len(callee):]]
        # ---- va_start: make the cursor it wrote agree with the (reversed) cursor loads and stores that follow ---------------------
        m = re.search(r"call void @llvm\.va_start(?:\.p\d+)?\(ptr ([^)]+)\)", s)
        if m:
            ap = m.group(1)
            a, b = self.tmp(), self.tmp()
            self.stats["va_start"] += 1
            return [line,
                    "%s%s = load i32, ptr %s, align 1" % (ind, a, ap),
                    "%s%s = call i32 @llvm.bswap.i32(i32 %s)" % (ind, b, a),
                    "%sstore i32 %s, ptr %s, align 1" % (ind, b, ap)]
        return [line]


def transform(text, source_name="<input>", allow_bitfields=(), preempt=False, fragment_index=None, symbol_addresses=None):
    """Rewrites one module of LLVM IR text. Returns (new text, stats dict). Raises BeError for anything it refuses.
       fragment_index: the number of the code overlay this source file belongs to (None for the main program); its writable globals are
       put into the overlay's sections, see assign_fragment_section.
       symbol_addresses: {name: console address} of documented variables, used to order an overlay's variables (order_fragment_globals)."""
    if "%be." in text or "@__be_fix" in text or "@" + CALL_HOOK in text or "@host_preempt_flag" in text:
        raise BeError("input already contains names reserved by this tool (processed twice?)")
    dl = re.search(r'^target datalayout = "([^"]*)"', text, re.M)
    if not dl or "p:32:32" not in dl.group(1):
        raise BeError("%s: only 32-bit-pointer targets are supported (datalayout %s)" % (source_name, dl.group(1) if dl else "missing"))
    if "@llvm.used" in text or "@llvm.global_ctors" in text:
        raise BeError("%s: module already has @llvm.used/@llvm.global_ctors; not supported yet" % source_name)
    lines = text.split("\n")
    triple_m = re.search(r'^target triple = "([^"]*)"', text, re.M)
    windows = bool(triple_m and "windows" in triple_m.group(1))
    types = Types(lines)
    fix = FixEmitter(types)
    stats = {"loads": 0, "stores": 0, "globals": 0, "va_start": 0, "indirect_calls": 0, "preempt": 0, "retzero": 0, "padded_arrays": 0, "bitfield": []}
    out, fname, body_lines = [], None, []
    fragment_positions = []
    for ln in lines:
        if fname is None:
            if ln.startswith("define "):
                fname = (re.search(r"@(?:[\w.$-]+|\"[^\"]*\")", ln) or re.search(r".", ln)).group(0)
                rw = FuncRewriter(fname, source_name, set(allow_bitfields), stats)
                body_lines = []
                out.append(ln)
            elif ln.startswith("@"):
                new = process_global(ln, types, fix)
                if new is not ln:
                    stats["globals"] += 1
                if fragment_index is not None:
                    new2 = assign_fragment_section(new, fragment_index, windows)
                    if new2 is not new:
                        stats["fragment_globals"] = stats.get("fragment_globals", 0) + 1
                        fragment_positions.append(len(out))
                    new = new2
                out.append(new)
            else:
                out.append(ln)
        else:
            if ln.startswith("}"):
                out.extend(rw.process_function(body_lines, preempt))
                fname = None
                out.append(ln)
            else:
                body_lines.append(ln)

    order_fragment_globals(out, fragment_positions, symbol_addresses)

    extra = []
    body = "\n".join(out)
    for bits in (16, 32, 64):
        used = fix.lines or re.search(r"@llvm\.bswap\.i%d\(" % bits, body)
        declared = re.search(r"^declare [^\n]*@llvm\.bswap\.i%d\(" % bits, body, re.M)
        if used and not declared:
            extra.append("declare i%d @llvm.bswap.i%d(i%d)" % (bits, bits, bits))
    if stats["indirect_calls"]:
        extra.append("declare ptr @%s(ptr)" % CALL_HOOK)
    if stats["preempt"]:
        extra.append("@host_preempt_flag = external global i32")
        extra.append("declare void @HostPreemptPoll()")
    if fix.lines:
        triple = re.search(r'^target triple = "([^"]*)"', text, re.M)
        section = ".befix$m" if triple and "windows" in triple.group(1) else "be_fixups"
        extra += fix.finish()
        extra.append('@__be_fixptr = internal constant ptr @__be_fix, section "%s", align 4' % section)
        extra.append('@llvm.used = appending global [1 x ptr] [ptr @__be_fixptr], section "llvm.metadata"')
    return "\n".join(out) + "\n" + "\n".join(extra) + "\n", stats


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--allow-bitfields", action="store_true", help="accept bit-field accesses (they will be wrong; for experiments)")
    args = ap.parse_args()
    text = open(args.input).read()
    try:
        new, stats = transform(text, args.input, {args.input} if args.allow_bitfields else ())
    except BeError as e:
        sys.stderr.write("be_memory: %s\n" % e)
        return 1
    open(args.output, "w").write(new)
    sys.stderr.write("be_memory: %d loads, %d stores, %d globals, %d va_start, %d indirect calls\n" %
                     (stats["loads"], stats["stores"], stats["globals"], stats["va_start"], stats["indirect_calls"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
