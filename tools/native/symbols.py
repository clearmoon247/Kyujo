#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Symbol-table helpers shared by census.py and gen_platform_symbols.py.

Everything here reads object files with LLVM's `llvm-nm` / `llvm-readobj`; nothing is guessed from source text.

Naming: 32-bit Windows (COFF) puts a leading underscore in front of every C name (`_osCreateThread`). All functions here take
`strip_prefix="_"` for such objects and then speak in plain C names, so the same classification works for every target.
"""
import collections
import re
import subprocess

# Relocation types that mean "this is a call or jump to the symbol" (as opposed to taking its address or reading it).
# i386 ELF: R_386_PC32 / R_386_PLT32.  i386 COFF: IMAGE_REL_I386_REL32.  A symbol that is only ever referenced by an absolute
# address (R_386_32, IMAGE_REL_I386_DIR32) could be data or a function whose address is stored; that case is "ambiguous".
# WebAssembly: a call names the function by index; storing a function's address is a table index. Data is always R_WASM_MEMORY_ADDR_*, so nothing is ambiguous.
CALL_RELOCS = {"R_386_PC32", "R_386_PLT32", "IMAGE_REL_I386_REL32",
               "R_WASM_FUNCTION_INDEX_LEB", "R_WASM_FUNCTION_INDEX_I32", "R_WASM_TABLE_INDEX_SLEB", "R_WASM_TABLE_INDEX_I32", "R_WASM_TABLE_INDEX_REL_SLEB"}


# Symbols the linker itself creates for a section whose name is a C identifier (see src/platform/host_be.c): never "missing".
# WebAssembly: wasm-ld defines these itself; the `shell_` functions are imports that the native shell provides (src/platform/host_*shell*.c).
LINKER_SYMBOL = re.compile(r"^(__(start|stop)_[A-Za-z_][A-Za-z0-9_]*|__(stack_pointer|global_base|heap_base|heap_end|data_end|tls_base|memory_base|table_base|"
                           r"stack_low|stack_high|dso_handle)|shell_[a-z0-9_]+|gBeFixups(Begin|End)|gFrag[0-9]{2}[dz](Begin|End))$")


def _clean(name, strip_prefix):
    if not name or name[0] in ".@":                 # compiler bookkeeping such as '.refptr.x', '@feat.00'
        return None
    if strip_prefix and name.startswith(strip_prefix):
        name = name[len(strip_prefix):]
    return name


def nm_names(path, flags, strip_prefix=""):
    """Names printed by `llvm-nm <flags> <path>`; for archives (.a) the names of all members are merged."""
    out = subprocess.run(["llvm-nm", "-j"] + flags + [path], capture_output=True, text=True, check=True).stdout
    names = []
    for line in out.splitlines():
        line = line.strip()
        if line.endswith(":") or line.endswith("):"):     # "member.o:" headers printed for archives
            continue
        n = _clean(line, strip_prefix)
        if n:
            names.append(n)
    return names


def scan(objects, strip_prefix="", provided=()):
    """Returns (defined_by, unresolved) for a set of object files.
       defined_by : {symbol: [object basenames that define it as a global]}
       unresolved : sorted symbols referenced by some object but defined by none of them nor by any archive in `provided`."""
    import os
    defined_by = collections.defaultdict(list)
    referenced = set()
    for o in objects:
        for n in nm_names(o, ["--defined-only", "--extern-only"], strip_prefix):
            defined_by[n].append(os.path.basename(o))
        referenced.update(nm_names(o, ["--undefined-only"], strip_prefix))
    for lib in provided:
        for n in nm_names(lib, ["--defined-only", "--extern-only"], strip_prefix):
            defined_by.setdefault(n, []).append(os.path.basename(lib))
    return defined_by, sorted(s for s in referenced if s not in defined_by)


def archive_index(archive, strip_prefix=""):
    """For a static library: ({symbol: member}, {member: set of symbols it needs}). Read with llvm-nm, one call per query."""
    import re as _re
    line = _re.compile(r"^.*?:([^:]+\.o):\s*(?:[0-9a-fA-F]+)?\s*([A-Za-z])\s+(\S+)$")
    defines, needs = {}, collections.defaultdict(set)
    for flag, bucket in (("--defined-only", "d"), ("--undefined-only", "u")):
        out = subprocess.run(["llvm-nm", "-A"] + ([flag, "--extern-only"] if bucket == "d" else [flag]) + [archive],
                             capture_output=True, text=True, check=True).stdout
        for ln in out.splitlines():
            m = line.match(ln)
            if not m:
                continue
            member, _, name = m.groups()
            name = _clean(name, strip_prefix)
            if not name:
                continue
            if bucket == "d":
                defines.setdefault(name, member)
            else:
                needs[member].add(name)
    return defines, needs


def scan_with_archives(objects, strip_prefix="", system_libs=(), archives=()):
    """Like scan(), but also follows static libraries the way a linker does: an archive member is pulled in only when some
       already-included code needs a symbol it defines, and then ITS references are followed in turn.
       system_libs : archives (C library, compiler runtime) whose symbols are simply 'provided'; the needs of their members are
                     not followed (they are self-contained).
       archives    : our own static libraries (the native libultra); followed fully.
       Returns (defined_by, unresolved, pulled); pulled is {archive path: sorted names of the members that get linked in}."""
    import os
    defined_by, unresolved = scan(objects, strip_prefix, system_libs)
    unresolved = [u for u in unresolved if not LINKER_SYMBOL.match(u)]
    indexes = [(a,) + archive_index(a, strip_prefix) for a in archives]      # (path, {symbol: member}, {member: needs})
    pulled = {a: set() for a, _, _ in indexes}
    pending, missing = set(unresolved), set()
    while pending:
        sym = pending.pop()
        if sym in defined_by:
            continue
        for path, defines, needs in indexes:
            member = defines.get(sym)
            if member is None:
                continue
            if member not in pulled[path]:
                pulled[path].add(member)
                for name, owner in defines.items():                # everything this member defines is now defined
                    if owner == member:
                        defined_by.setdefault(name, []).append(os.path.basename(path) + "(" + member + ")")
                pending.update(needs[member])                       # and everything it needs must be found too
            break
        else:
            missing.add(sym)
    missing = sorted(m for m in missing if m not in defined_by and not LINKER_SYMBOL.match(m))   # a member pulled in later may have defined it
    return defined_by, missing, {path: sorted(members) for path, members in pulled.items()}


def reloc_kinds(objects, wanted, strip_prefix=""):
    """{symbol: set(relocation type names)} for the symbols in `wanted`, read with `llvm-readobj --relocs`."""
    kinds = collections.defaultdict(set)
    line_re = re.compile(r"\b((?:R_386_|IMAGE_REL_I386_|R_WASM_)\w+)\s+(\S+)")
    for o in objects:
        out = subprocess.run(["llvm-readobj", "--relocs", o], capture_output=True, text=True, check=True).stdout
        for m in line_re.finditer(out):
            name = _clean(m.group(2), strip_prefix)
            if name in wanted:
                kinds[name].add(m.group(1))
    return kinds


def classify(sym):
    """Kind of an unresolved symbol (meaning of each kind: docs/native-port-feasibility.md, section 2.2)."""
    if re.fullmatch(r"D_[0-9A-F]{1,8}", sym) and not re.fullmatch(r"D_8[0-9A-F]{7}", sym):
        return "segment_offset"
    if re.search(r"_ROM_(START|END)$", sym) or re.search(r"(_VRAM|_TEXT_START|_TEXT_END|_DATA_START|_BSS_START|_BSS_END)$", sym) \
            or re.match(r"fragment\d+_", sym):
        return "rom_range"
    if re.fullmatch(r"(D|jtbl|func)_8[0-9A-F]{7}", sym):
        return "address_data"
    if re.match(r"_*os[A-Z]", sym) or re.match(r"__OS", sym):
        return "libultra_os"
    if re.match(r"gu[A-Z]", sym):
        return "libultra_gu"
    if re.match(r"(al|AL)[A-Z]", sym):
        return "libultra_al"
    if sym in ("__cosf", "__sinf", "__ll_mul", "__udivdi3", "__umoddi3", "__ull_div", "__ull_rem", "__ull_to_f", "_GLOBAL_OFFSET_TABLE_"):
        return "compiler_rt"
    return "other"
