#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Generates the pieces that let the natively compiled decomp LINK: constants, and loud stubs for everything not written yet.

    gen_platform_symbols.py --decomp ext/decomp --profile linux32 --sysroot work/sysroot/musl-i386 \
                            --objs build/linux32/obj --out build/linux32/gen

Input: the object files built so far (game code compiled from the decomp, plus any hand-written platform code).
Output (all derived; never committed):
  abs_symbols.S   Absolute-address symbols, assembled into the program. Two kinds, both documented by the decomp itself:
                    * segment offsets  `D_1002480` -> 0x01002480. The original linker defined these as plain numbers equal to
                      their name (checked: all 678 that linker_scripts/us/*.ld lists have value == name). The game stores them in
                      display lists as segmented addresses, so the number itself is what must be there.
                    * ROM ranges       `fragment1_ROM_START` etc. -> byte offsets in the ROM, taken from linker_scripts/us/*
                      when documented there, else derived from yamls/us/rom.yaml (romlayout.py shows the two agree on every value
                      that is documented in both).
  stubs.c         For every other symbol that nothing defines yet:
                    * functions (referenced by a call)  -> a function that prints its own name and stops the program (exit 70);
                    * data (referenced only by address), by where the decomp documents the symbol (linker_scripts/us/*, else
                      the address in its name):
                        - below 0x04000000: a ROM offset or segment-address NUMBER (`gAudioData` = 0x015C0000, textures at
                          0x02xxxxxx). Becomes an absolute constant; the game only passes it to DMA or stores it in display lists.
                        - inside the main program's RAM image (0x80000400 up to the heap): becomes an address inside ONE array,
                          `gMainImage`, so neighbouring symbols stay neighbours and "shifted alias" symbols (undefined_syms.ld:
                          `D_8006FEE8 = &sPokemonNames[150]`) keep their meaning. The part that the ROM holds (the program's
                          initialised data) is copied from the player's ROM at start-up by `host_init_rom_data()`; the rest is the
                          program's .bss and starts as zero, as on the console. EXCEPT a symbol whose address lies inside a variable
                          that the port compiles from C (or that DOCUMENTED_SHIFTS names): it becomes an alias at the same place
                          inside that variable (alias_plan.json; build_native.py compiles the aliases into the object that defines
                          the variable). A raw copy of the ROM's data would hold console addresses where the C variable holds
                          real pointers (found when the species-name table crashed the title screen). What stays in gMainImage:
                          the RSP microcode blobs and a few constants nothing in C defines.
                        - 0x81000000-0x8FFFFFFF: an address in a code overlay ("fragment"). Becomes an absolute constant so that
                          arithmetic on the address (fragment ids) works and any attempt to read through it faults loudly instead
                          of reading zeros.
                        - the cartridge window 0xB0xxxxxx: a variable filled from the ROM at start-up.
                        - anything else: a zero-filled PLACEHOLDER array, labelled as such here and in the report.
  fragment_table.c  The functions that live in code overlays ("fragments"), by the overlay address the console linked them at
                  (0x81000000-0x8FFFFFFF). On the console the game calls into an overlay by jumping to its copy in the memory pool;
                  here that code is part of the program, and host_fragment.c uses this table to turn an address inside a loaded
                  fragment into the native function. Names come from two documented places: the linker script symbol lists
                  (`Name = 0x81200020; // type:func`) and the decomp's own `func_<address>` naming convention.
  fragment_data.c Where each code overlay's variables are (the linker sections that be_memory.py gave them) and the overlay's link base
                  address, so that host_fragment.c can restore the variables to their start-up state each time the game loads the overlay
                  again (the console reloads an overlay's data from the ROM at every load; natively the variables are plain statics).
  report.json     Every symbol and the decision taken for it, plus the lists a human should look at.

A symbol that has been implemented by hand since the last run is simply no longer unresolved, so it disappears from the stubs:
the stub list always shrinks as the platform layer grows. Exit status is 0 if every symbol got a decision.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

# `python3 -I` does not add the script's folder to the import path; do it explicitly, for this repository's own modules only.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compile_decomp  # noqa: E402  (only for fragment_dirs: the numbering of the code overlays' variable sections)
import proc  # noqa: E402
import romlayout  # noqa: E402
import symbols    # noqa: E402
import targets    # noqa: E402

# Functions this script writes itself into stubs.c (so they must not also be stubbed as "missing").
GENERATED_HERE = {"host_init_rom_data", "gHostFragmentFuncs", "gHostFragmentFuncCount", "gHostPoolBytes",
                  "gHostFragmentData", "gHostFragmentDataCount"}   # the last five: fragment_table.c and fragment_data.c

PLACEHOLDER_BYTES = 0x10000     # size given to data stubs whose real size is unknown

# On the N64 the cartridge ROM is also visible to the CPU at 0xB0000000 + offset. A few game functions read it directly through
# a symbol such as `extern s32 D_B0000800;`, i.e. they dereference that address. On a PC such an address is not mapped, so these
# symbols must be real variables whose contents are copied from the ROM at start-up - never absolute constants.
CART_WINDOW = re.compile(r"D_(B[0-9A-F])([0-9A-F]{6})$")


# Aliases whose target is not found by "the address lies inside a variable the port defines": the decomp documents them by hand.
# symbol -> (variable the port defines, the text in the decomp's linker script that says so). The text is checked at run time, so a change
# of the decomp that removes the evidence stops the build instead of silently keeping an unsupported alias.
DOCUMENTED_SHIFTS = {
    "D_8006FEE8": ("D_8006FF00", "D_8006FEE8 = &sPokemonNames[150] = (char*)D_8006FF00 - 0x18."),
}


# Aliases that the decomp defines relative to a variable known by NAME (not by an address-style `D_8xxxxxxx` name):
# symbol -> (variable, byte offset from it, decomp file that documents it, the text in that file that says so). The text is checked at run
# time, like DOCUMENTED_SHIFTS.
#
# gBaseStats: the US linker script places it at 0x80070F84, which lies at the START of the last record (index 151) of D_8006FF00. That is
# only the console's memory layout: the code indexes it one-based, with the stride of SpeciesData (0x17), so the bytes it reads are those of
# gSpeciesData, which on the console follows D_8006FF00 directly. The Japanese script documents the intent ("1-based alias,
# gSpeciesData - 0x1C"), and gb_data.h says the fields address "the same bytes as SpeciesData's stats and growthRate". Checked here
# against the US ROM: the first species record sits at ROM 0x71BA0, i.e. N64 address 0x80070FA0 = 0x80070F84 + 0x1C. Resolving it as
# "D_8006FF00 + 0x1084" instead (what the generic rule above would do) makes the code read whatever the port's linker placed after
# D_8006FF00 - which gave every Pokemon a wrong growth rate and hence wrong levels.
#
# D_800780B3: one byte before D_800780B4 (the 204-byte move table, src/3D140_2.c). The linker script puts it at 0x800780B3, which is the last
# byte of the 12-byte D_800780A8 that precedes it on the console, so the generic rule would pick D_800780A8 + 11; but the code indexes it
# as D_800780B3[n * 3] (3D140_2.c, a 1-based view of the 3-byte move entries, cf. `&D_800780B4[(moveId * 3) - 3]`), which on the console
# reads into D_800780B4 and, with the port's layout, would read into whatever follows D_800780A8.
#
# Symbols in a code overlay ("fragment", 0x81000000..) are handled the same way when they are named here (the generic rule above only looks at
# the main program's image; without an entry they become absolute constants, and a read through such a constant faults):
#
# D_8120EA80, D_8120EA86: interior aliases into sGbApuRegs (linker_scripts/us/undefined_syms.ld says so: "Interior aliases into
# gb_tower_emulator_86CB0.c's sGbApuRegs"). The table starts at 0x8120EA80: in the source it follows D_8120EA70[0x10], which ends there.
#
# D_8120D7F3: INFERRED, not documented. The name is the console address 0x8120D7F3, which lies 0x2D BYTES BEFORE the table D_8120D820[0xEC]
# (gb_tower_emulator_7F9A0.c). func_8120241C reads `((u8*)&D_8120D7F3 - k)[0x116]` for k = 0..10, i.e. addresses 0x8120D909 down to 0x8120D8FF:
# the last bytes of D_8120D820 (0x8120D820 + 0xE9 ... 0xDF). A sibling function (line ~1831) walks the same table backwards the same way: D_8120D820[0xE7 - i].
# So the symbol is "D_8120D820 - 0x2D", a base chosen so that the compiler's 0x116 offset reaches the table. The values read this way decode
# to valid indices of D_8120E580 (checked by hand for the first entry: 0x79 - 0x37 + 0x80 = 0xC2 < 0x100).
NAMED_SHIFTS = {
    "D_8120EA80": ("sGbApuRegs", 0, os.path.join("src", "fragments", "gb_tower_emulator", "gb_tower_emulator_86CB0.c"),
                   "// D_8120EA80 and D_8120EA86 are interior aliases (undefined_syms.ld)."),
    "D_8120EA86": ("sGbApuRegs", 6, os.path.join("src", "fragments", "gb_tower_emulator", "gb_tower_emulator_86CB0.c"),
                   "// D_8120EA80 and D_8120EA86 are interior aliases (undefined_syms.ld)."),
    "D_8120D7F3": ("D_8120D820", -0x2D, os.path.join("src", "fragments", "gb_tower_emulator", "gb_tower_emulator_7F9A0.c"),
                   "u8 D_8120D820[0xEC] = {"),
    "D_800780B3": ("D_800780B4", -1, os.path.join("src", "3D140_2.c"), "u8 D_800780B4[204] = {"),
    "gBaseStats": ("gSpeciesData", -0x1C, os.path.join("linker_scripts", "jp", "symbol_addrs.txt"),
                   "gBaseStats = 0x80071624; // 1-based alias, gSpeciesData - 0x1C"),
}


def alias_names_of(objs):
    """Symbols that compile_decomp.alias_ir put into the objects (listed in a sidecar file next to each object)."""
    names = set()
    for o in objs:
        if os.path.exists(o + ".aliases.json"):
            names.update(json.load(open(o + ".aliases.json")))
    return names


def sized_data_symbols(objs, prefix, skip):
    """Data variables of the port's own objects whose name carries their console address (the decomp's `D_80073230` convention).
    Returns [(vram, size, name, object path)]. `skip`: names of IR aliases (they are not variables of their own).
    Sizes come from the sidecar `<object>.sizes.json` that compile_decomp.py writes from the IR (COFF objects of the Windows target carry
    no sizes, so nm cannot be used for both targets). For ELF objects the numbers are also compared with `llvm-nm -S`: a difference means
    the IR size computation (ir_sizes.py) is wrong, and the build stops."""
    found = []
    pat = re.compile(r"^(.*): ([0-9a-fA-F]+) ([0-9a-fA-F]+) ([dDbBrR]) %sD_(8[0-9A-Fa-f]{7})$" % re.escape(prefix))
    nm_sizes = {}                                        # (object path, name) -> size, ELF only
    if targets_have_nm_sizes(prefix):
        for i in range(0, len(objs), 200):
            r = proc.run(["llvm-nm", "-S", "-A", "--defined-only"] + objs[i:i + 200], capture_output=True, text=True)
            for line in r.stdout.splitlines():
                m = pat.match(line)
                if m:
                    nm_sizes[(m.group(1), "D_" + m.group(5).upper())] = int(m.group(3), 16)
    for o in objs:
        side = o + ".sizes.json"
        if not os.path.exists(side):
            continue                                     # not an object of the game (e.g. a hand-written assembly object)
        for name, size in json.load(open(side)).items():
            if name in skip or size == 0:
                continue
            if nm_sizes and nm_sizes.get((o, name), size) != size:
                sys.exit("gen_platform_symbols: size of %s in %s: IR says %d, nm says %d (tools/native/ir_sizes.py disagrees with the linker)"
                         % (name, o, size, nm_sizes[(o, name)]))
            found.append((int(name[2:], 16), size, name, o))
    return found


def targets_have_nm_sizes(prefix):
    """ELF symbol tables carry sizes (and use no name prefix); COFF ones do not (and the Windows target uses the prefix '_')."""
    return prefix == ""


def main():
    proc.ensure_utf8_mode()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decomp", required=True)
    ap.add_argument("--tree", help="the patched source tree made by srctree.py (its src/fragments folders number the overlays)")
    ap.add_argument("--profile", required=True, choices=sorted(targets.TARGETS))
    ap.add_argument("--sysroot", required=True, help="sysroot made by tools/toolchain/build_sysroots.sh for this profile")
    ap.add_argument("--objs", required=True, nargs="+", help="directories searched for .o files (recursively)")
    ap.add_argument("--archives", nargs="*", default=[], help="static libraries of our own (libultra); members are used only if needed")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    t = targets.TARGETS[args.profile]
    prefix = t["symbol_prefix"]
    objs = sorted(o for d in args.objs for o in glob.glob(os.path.join(d, "**", "*.o"), recursive=True))
    if not objs:
        sys.exit("no object files found under " + " ".join(args.objs))

    # Follow the archives the way the linker does: only the members that some code needs are linked, and their own needs count too.
    defined_by, unresolved, pulled = symbols.scan_with_archives(objs, prefix, targets.provided_libraries(args.profile, args.sysroot),
                                                                args.archives)
    twice = sorted(s for s, f in defined_by.items() if len(f) > 1 and not any(x.endswith(".a") for x in f))
    if twice:
        sys.exit("symbols defined by more than one object (a link error): " + ", ".join(twice[:20]))

    segs, derived, _, _, _ = romlayout.check(args.decomp)
    documented = romlayout.read_documented(args.decomp, derived)
    # The main program: ROM [text_ROM_START, text_ROM_END) is loaded at its virtual address; the .bss follows up to the heap.
    main_vram = next(sg["vram"] for sg in segs if sg["name"] == "text")
    main_rom, main_rom_end = derived["text_ROM_START"], derived["text_ROM_END"]
    main_data_end = main_vram + (main_rom_end - main_rom)                   # end of the part that comes from the ROM
    main_image_end = next(sg["vram"] for sg in segs if sg["name"] == "heap")  # end of .bss = start of the heap
    reloc = symbols.reloc_kinds(objs, set(unresolved), prefix)

    unresolved = [u for u in unresolved if u not in GENERATED_HERE]
    consts, funcs, data, no_value = {}, [], [], []
    odd_segments, cart_reads, image_aliases, number_consts, overlay_consts = [], {}, {}, [], []
    overlay_named = set()                                                  # overlay-address symbols that NAMED_SHIFTS turns into aliases
    for sym in unresolved:
        kind = symbols.classify(sym)
        m = CART_WINDOW.match(sym)
        if m and int(m.group(1), 16) == 0xB0:
            cart_reads[sym] = int(m.group(2), 16)           # ROM offset the game reads through this variable
            data.append((sym, "reads cartridge ROM offset 0x%X: fill from the ROM at start-up" % cart_reads[sym]))
        elif kind == "segment_offset":
            value = int(sym[2:], 16)
            consts[sym] = value
            if value >> 24 > 0x0F:               # a segment number is 0..15; anything else is worth a human look
                odd_segments.append(sym)
        elif kind == "rom_range":
            value = documented.get(sym, derived.get(sym))
            if value is None:
                no_value.append(sym)
                data.append((sym, "ROM-range constant with no documented or derivable value"))
            else:
                consts[sym] = value
        elif reloc.get(sym, set()) & symbols.CALL_RELOCS:
            funcs.append(sym)
        else:
            addr = documented.get(sym)
            how = "documented"
            if addr is None and re.fullmatch(r"D_[0-9A-F]{8}", sym):
                addr, how = int(sym[2:], 16), "from its name"
            if addr is not None and addr < 0x04000000:
                consts[sym] = addr
                number_consts.append(sym)
            elif addr is not None and main_vram <= addr < main_image_end:
                image_aliases[sym] = addr - main_vram
            elif addr is not None and 0x81000000 <= addr < 0x90000000 and sym in NAMED_SHIFTS:
                overlay_named.add(sym)                       # becomes an alias into a variable of the port (NAMED_SHIFTS), not a constant
            elif addr is not None and 0x81000000 <= addr < 0x90000000:
                consts[sym] = addr
                overlay_consts.append(sym)
            else:
                data.append((sym, "N64 address 0x%08X (%s), outside every known region" % (addr, how) if addr is not None else "no documented address"))

    # ---- aliases into variables the port defines ----------------------------------------------------------------------------------
    # A symbol the decomp's linker scripts place INSIDE a variable that the port compiles from C must be at the same place inside that
    # variable (see compile_decomp.alias_ir). Otherwise it would name a raw copy of the ROM's data, whose pointers are console addresses.
    # The plan is derived from the symbol's documented address, so it is the same whether or not the aliases already exist.
    earlier_aliases = alias_names_of(objs)                            # made by an earlier run: not variables of the game's own
    sized = sized_data_symbols(objs, prefix, earlier_aliases)
    sized_names = {n for _, _, n, _ in sized}
    candidates = dict(image_aliases)                                  # symbol -> offset in the main image, still unresolved
    for sym in NAMED_SHIFTS:                                          # overlay-address ones: the offset is not used (NAMED_SHIFTS gives the place)
        if sym in overlay_named or sym in earlier_aliases:
            candidates.setdefault(sym, 0)
    for sym in earlier_aliases:
        if sym in documented and main_vram <= documented[sym] < main_image_end:
            candidates[sym] = documented[sym] - main_vram
    alias_plan = {}                                                   # object path -> [(symbol, target variable, byte offset)]
    gep_map = {}                                                      # wasm32 only: symbol -> [target variable, NEGATIVE byte offset] (see below)

    def plan_add(objpath, sym, target, offset):
        """An alias into a variable of the port. WebAssembly symbols cannot lie before the start of the variable they belong to (the symbol table stores
        an unsigned offset inside the data), so there those few go to gep_map instead and the IR substitution turns each use into target + offset."""
        if args.profile == "wasm32" and offset < 0:
            gep_map[sym] = [target, offset]
        else:
            alias_plan.setdefault(objpath, []).append((sym, target, offset))

    for sym, off in sorted(candidates.items()):
        addr = main_vram + off
        if sym in sized_names:
            continue                                                  # the port defines it itself
        if sym in NAMED_SHIFTS:
            target, delta, rel, evidence = NAMED_SHIFTS[sym]
            if evidence not in open(os.path.join(args.decomp, rel), errors="replace").read():
                sys.exit("%s no longer says: %s" % (rel, evidence))
            if len(defined_by.get(target, [])) != 1:
                sys.exit("%s: expected exactly one object defining %s, found %r" % (sym, target, defined_by.get(target)))
            plan_add(defined_by[target][0], sym, target, delta)
            image_aliases.pop(sym, None)
            continue
        if sym in DOCUMENTED_SHIFTS:
            target, evidence = DOCUMENTED_SHIFTS[sym]
            ld = open(os.path.join(args.decomp, "linker_scripts", "us", "undefined_syms.ld"), errors="replace").read()
            if evidence not in ld:
                sys.exit("linker_scripts/us/undefined_syms.ld no longer says: %s" % evidence)
            hit = [x for x in sized if x[2] == target]
        else:
            hit = sorted((x for x in sized if x[0] <= addr < x[0] + x[1]), key=lambda x: x[1])[:1]      # the smallest variable containing it
        if hit:
            vram, size, target, objpath = hit[0]
            plan_add(objpath, sym, target, addr - vram)
            image_aliases.pop(sym, None)
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "alias_plan.json"), "w") as f:
        json.dump({os.path.basename(k): v for k, v in sorted(alias_plan.items())}, f, indent=1)

    if args.profile == "wasm32":
        # WebAssembly: no absolute symbols in the linker. The constants go into abs_map.json (build_native.py substitutes them in the IR of the game
        # objects, compile_decomp.absolute_ir) and the RAM image with the symbols that name places inside it is IR, see gen_image.ll.
        with open(os.path.join(args.out, "abs_map.json"), "w") as f:
            json.dump(dict(sorted(consts.items())), f, indent=0)
        with open(os.path.join(args.out, "gep_map.json"), "w") as f:
            json.dump(dict(sorted(gep_map.items())), f, indent=0)
        with open(os.path.join(args.out, "gen_image.ll"), "w") as f:
            f.write('; GENERATED by tools/native/gen_platform_symbols.py - do not edit.\n; The main program\'s RAM image: [0x%08X, 0x%08X) from the ROM, then .bss up to 0x%08X.\n'
                    'target triple = "wasm32-unknown-unknown"\n@gMainImage = global [%d x i8] zeroinitializer, align 16\n'
                    % (main_vram, main_data_end, main_image_end, main_image_end - main_vram))
            for sym, off in sorted(image_aliases.items()):
                f.write("@%s = alias i8, getelementptr (i8, ptr @gMainImage, i32 %d)    ; 0x%08X\n" % (sym, off, main_vram + off))
    else:
      with open(os.path.join(args.out, "abs_symbols.S"), "w") as f:
        f.write("/* GENERATED by tools/native/gen_platform_symbols.py - do not edit. Absolute constants, see that script's header. */\n")
        for sym, value in sorted(consts.items()):
            f.write("    .globl %s%s\n    .set   %s%s, 0x%08X\n" % (prefix, sym, prefix, sym, value))
        # The image lives in this file (not in stubs.c) because an assembler can only define "symbol = other symbol + offset"
        # when both are in the same file.
        f.write("\n/* The main program's RAM image: [0x%08X, 0x%08X) from the ROM, then .bss up to 0x%08X. */\n"
                "    .bss\n    .balign 16\n    .globl %sgMainImage\n%sgMainImage:\n    .space 0x%X\n    .text\n"
                % (main_vram, main_data_end, main_image_end, prefix, prefix, main_image_end - main_vram))
        f.write("\n/* Symbols that name places inside that image. */\n")
        for sym, off in sorted(image_aliases.items()):
            f.write("    .globl %s%s\n    .set   %s%s, %sgMainImage + 0x%X    /* 0x%08X */\n" % (prefix, sym, prefix, sym, prefix, off, main_vram + off))
    with open(os.path.join(args.out, "stubs.c"), "w") as f:
        f.write("/* GENERATED by tools/native/gen_platform_symbols.py - do not edit.\n"
                " * Each function below exists only so the program links. Calling one stops the program and names it, so the first\n"
                " * thing the game needs that is not implemented yet is always reported by name. */\n"
                "void host_missing(const char *symbol) __attribute__((noreturn));\n\n")
        for sym in sorted(funcs):
            f.write('void %s(void) { host_missing("%s"); }\n' % (sym, sym))
        f.write("\nextern unsigned char gMainImage[];      /* defined in abs_symbols.S */\n")
        for sym, off in sorted(cart_reads.items()):
            f.write("unsigned char %s[16] __attribute__((aligned(16))); /* the cartridge window: ROM offset 0x%X */\n" % (sym, off))
        f.write("\nvoid host_copy_from_rom(unsigned int offset, void *dst, unsigned int len);\n\n"
                "/* Called once at start-up (host_main.c), after the ROM has been verified: fills the variables that stand for ROM contents. */\n"
                "void host_init_rom_data(void)\n{\n"
                "    host_copy_from_rom(0x%X, gMainImage, 0x%X);\n" % (main_rom, main_rom_end - main_rom))
        for sym, off in sorted(cart_reads.items()):
            f.write("    host_copy_from_rom(0x%X, %s, 16);\n" % (off, sym))
        f.write("}\n")
        f.write("\n/* PLACEHOLDER data: zero-filled, 0x%X bytes each. Real contents come from the ROM; real sizes are not known here. */\n"
                % PLACEHOLDER_BYTES)
        for sym, why in sorted(d for d in data if d[0] not in cart_reads):
            f.write("unsigned char %s[0x%X] __attribute__((aligned(16))); /* PLACEHOLDER: %s */\n" % (sym, PLACEHOLDER_BYTES, why))
    # ---- the table of overlay functions -----------------------------------------------------------------------------------------
    frag_funcs, frag_ambiguous, frag_undefined = {}, {}, []
    candidates = {}                               # name -> overlay address
    for name in defined_by:
        m = re.fullmatch(r"func_(8[1-9A-F][0-9A-F]{6})", name)
        if m:
            candidates[name] = int(m.group(1), 16)
    for path in sorted(glob.glob(os.path.join(args.decomp, "linker_scripts", "us", "symbol_addrs*.txt"))):
        for line in open(path):
            m = re.match(r"^(\w+)\s*=\s*(0x[0-9A-Fa-f]+);\s*//(.*)$", line.strip())
            if m and "type:func" in m.group(3) and 0x81000000 <= int(m.group(2), 16) < 0x90000000:
                if m.group(1) in defined_by:
                    candidates[m.group(1)] = int(m.group(2), 16)
                else:
                    frag_undefined.append(m.group(1))      # documented but the port does not define it (still assembly, or static)
    by_addr = {}
    for name, addr in candidates.items():
        by_addr.setdefault(addr, []).append(name)
    for addr, names in sorted(by_addr.items()):
        frag_funcs[addr] = sorted(names)[0]            # aliases of one address: any name is the same function
    with open(os.path.join(args.out, "fragment_table.c"), "w") as f:
        f.write("/* GENERATED by tools/native/gen_platform_symbols.py - do not edit.\n"
                " * Overlay address -> native function, sorted by address (see host_fragment.c, which searches it). */\n"
                "struct HostFragmentFunc { unsigned int overlay_addr; void (*fn)(void); };\n\n")
        for name in sorted(set(frag_funcs.values())):
            f.write("void %s(void);\n" % name)
        f.write("\nconst struct HostFragmentFunc gHostFragmentFuncs[] = {\n")
        for addr, name in sorted(frag_funcs.items()):
            f.write("    { 0x%08Xu, %s },\n" % (addr, name))
        f.write("};\nconst unsigned int gHostFragmentFuncCount = %d;\n" % len(frag_funcs))
        pool = romlayout.read_pool_bytes(args.decomp)
        f.write("\n/* Size of the game's main memory pool (include/variables.h: POOL_END_6MB - POOL_START). */\n"
                "const unsigned int gHostPoolBytes = 0x%X;\n" % pool)
    # ---- where each overlay's variables are (see be_memory.assign_fragment_section) -------------------------------------------------
    frag_dirs = compile_decomp.fragment_dirs(args.tree or args.decomp)      # the tree holds exactly the manifest's files
    dir_bases = {}                                # overlay number -> the link bases (address & 0xFFF00000) of its functions
    for name, addr in candidates.items():
        for o in defined_by.get(name, []):
            b = os.path.basename(o)
            for idx, d in sorted(enumerate(frag_dirs), key=lambda x: -len(x[1])):
                if b.startswith("src_fragments_%s_" % d):
                    dir_bases.setdefault(idx, set()).add(addr & 0xFFF00000)
                    break
    for idx, bases in sorted(dir_bases.items()):
        if len(bases) != 1:
            sys.exit("fragment directory %s has functions at several overlay bases (%s); the variable reset cannot tell which load is which"
                     % (frag_dirs[idx], ", ".join("0x%08X" % x for x in sorted(bases))))
    # Several directories can share one link base (different overlays loaded one after the other into the same slot: the console never has
    # two of them in memory at once). host_fragment.c resets every entry of the base that is being loaded.
    with open(os.path.join(args.out, "fragment_data.c"), "w") as f:
        f.write("/* GENERATED by tools/native/gen_platform_symbols.py - do not edit.\n"
                " * For each code overlay that has C code: its link base address and the start and end of the two linker sections that hold its\n"
                " * variables (initialised ones, zero-filled ones). Read by host_fragment.c. */\n"
                "struct HostFragmentData { unsigned int base; const char *name; unsigned char *data_start, *data_end, *zero_start, *zero_end; };\n\n"
                "#if defined(_WIN32)\n"
                "/* COFF: the pieces of one overlay are `.fdat$NNm` (variables); our markers `.fdat$NNa` / `.fdat$NNz` surround them in the merged section. */\n")
        for idx in sorted(dir_bases):
            for fam, tag in ((".fdat", "d"), (".fzer", "z")):
                f.write('static unsigned char gFrag%02d%sBegin __attribute__((section("%s$%02da"), used));\n' % (idx, tag, fam, idx))
                f.write('static unsigned char gFrag%02d%sEnd __attribute__((section("%s$%02dz"), used));\n' % (idx, tag, fam, idx))
        f.write("#define FRAG(i, base, name) { base, name, &gFrag##i##dBegin + 1, &gFrag##i##dEnd, &gFrag##i##zBegin + 1, &gFrag##i##zEnd }\n"
                "#elif defined(__wasm__)\n"
                "/* WebAssembly: wasm-ld provides no __start_/__stop_ symbols and no `$` ordering, so the markers are objects of their own (written by\n"
                " * build_native.py) that the link puts in front of and behind the overlay's files; same-named sections are joined in link order. */\n")
        for idx in sorted(dir_bases):
            for tag in ("d", "z"):
                f.write("extern unsigned char gFrag%02d%sBegin, gFrag%02d%sEnd;\n" % (idx, tag, idx, tag))
        f.write("#define FRAG(i, base, name) { base, name, &gFrag##i##dBegin + 1, &gFrag##i##dEnd, &gFrag##i##zBegin + 1, &gFrag##i##zEnd }\n"
                "#else\n"
                "/* ELF: the linker provides __start_/__stop_ symbols for sections whose names are C identifiers; weak, because an overlay without\n"
                " * writable variables has no such section. */\n")
        for idx in sorted(dir_bases):
            f.write("extern unsigned char __start_fd%02d[] __attribute__((weak)), __stop_fd%02d[] __attribute__((weak));\n" % (idx, idx))
            f.write("extern unsigned char __start_fz%02d[] __attribute__((weak)), __stop_fz%02d[] __attribute__((weak));\n" % (idx, idx))
        f.write("#define FRAG(i, base, name) { base, name, __start_fd##i, __stop_fd##i, __start_fz##i, __stop_fz##i }\n"
                "#endif\n\n"
                "const struct HostFragmentData gHostFragmentData[] = {\n")
        for idx in sorted(dir_bases):
            f.write('    FRAG(%02d, 0x%08Xu, "%s"),\n' % (idx, next(iter(dir_bases[idx])), frag_dirs[idx]))
        f.write("};\nconst unsigned int gHostFragmentDataCount = %d;\n" % len(dir_bases))
    with open(os.path.join(args.out, "overlays.json"), "w") as f:       # for build_native.py (wasm): which overlay numbers have variable sections
        json.dump(sorted(dir_bases), f)
    report = {
        "fragment_functions": len(frag_funcs), "fragment_functions_documented_but_not_defined": sorted(frag_undefined),
        "profile": args.profile, "objects": len(objs), "unresolved_total": len(unresolved),
        "constants": len(consts), "function_stubs": sorted(funcs), "data_placeholders": dict(data),
        "number_constants": sorted(number_consts), "main_image_aliases": {k: hex(main_vram + v) for k, v in sorted(image_aliases.items())},
        "overlay_address_constants": sorted(overlay_consts),
        "aliases_into_c_variables": {o: v for o, v in sorted((os.path.basename(k), v) for k, v in alias_plan.items())},
        "rom_range_without_value": no_value, "cart_rom_reads": cart_reads, "segment_symbols_with_unusual_top_byte": odd_segments,
    }
    with open(os.path.join(args.out, "report.json"), "w") as f:
        json.dump(report, f, indent=1)

    for path, members in sorted(pulled.items()):
        print("archive %s: %d members linked in" % (os.path.basename(path), len(members)))
    print("unresolved after C library + compiler runtime + our archives: %d" % len(unresolved))
    print("  absolute constants emitted : %d  (%d segment offsets, %d ROM ranges)"
          % (len(consts), sum(1 for s in consts if symbols.classify(s) == "segment_offset"), sum(1 for s in consts if symbols.classify(s) == "rom_range")))
    print("  function stubs (stop+name) : %d" % len(funcs))
    print("  data: %d ROM/segment numbers, %d inside the main RAM image, %d overlay addresses, %d cartridge-window variables" %
          (len(number_consts), len(image_aliases), len(overlay_consts), len(cart_reads)))
    print("  aliases into C variables   : %d  (%d objects; compiled in by build_native.py)" % (sum(len(v) for v in alias_plan.values()), len(alias_plan)))
    print("  data PLACEHOLDERs (zero)   : %d" % (len(data) - len(cart_reads)))
    print("  overlay functions in table : %d  (%d documented as functions but not defined by the port)" % (len(frag_funcs), len(frag_undefined)))
    if no_value:
        print("  ROM ranges with NO value   : %s" % ", ".join(no_value))
    if cart_reads:
        print("  variables the game reads from the cartridge window (fill from ROM): %s"
              % ", ".join("%s=ROM+0x%X" % kv for kv in sorted(cart_reads.items())))
    if odd_segments:
        print("  segment symbols whose top byte is above 0x0F (check by hand): %s" % ", ".join(odd_segments))
    return 0


if __name__ == "__main__":
    sys.exit(main())
