/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_gbemu.c - the three hand-written assembly functions of the GB Tower emulator that the fragment's C code calls.
 *
 * The emulator (code overlay "fragment 1", src/fragments/gb_tower_emulator) is mostly C, which this port compiles for the host. Its Game Boy
 * CPU core is hand-written MIPS assembly with no C source. The C code calls three of its functions (gb_tower_emulator.h):
 *
 *     void GbCpu_InitOpcodeTables(unk_D_8122B2C0 *emu, void *);     link address 0x812096A0
 *     void GbEmu_InitContext(unk_D_8122B2C0 *emu);                  link address 0x81209870
 *     s32  GbCpu_Execute(unk_D_8122B2C0 *emu, s32 cycles);          link address 0x81209EB4
 *
 * (addresses: linker_scripts/us/symbol_addrs_code.txt of the decomp, the US ROM this port supports). Each function here finds the code in the
 * memory pool where the game has loaded and relocated the fragment, and runs it with the MIPS interpreter (host_mips.c).
 * The pointer argument is a host pointer, which is also the console's physical address (docs/architecture.md), so the interpreted code can
 * use it unchanged.
 *
 * HOST-side file. These functions are called by game code compiled by be_memory.py through ordinary C calls with plain integer
 * arguments, and the result is returned by value, as required by the byte-order rule in platform.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

/* The first instruction of each of these functions is `addiu sp, sp, -N` (0x27BDxxxx), as read from the ROM. Checked before running so that
 * a wrong address (another ROM version, or the fragment not loaded) cannot make the interpreter execute data. */
static unsigned int entry_of(unsigned int link_addr, const char *name)
{
    const unsigned char *code;
    unsigned int pool = host_fragment_pool_addr(link_addr);
    if (!pool) {
        fprintf(stderr, "kyujo: %s: the GB Tower emulator fragment is not loaded\n", name);
        host_fatal("a hand-written assembly function was called but its fragment is not in memory");
    }
    code = (const unsigned char *)(unsigned long)pool;
    if (code[0] != 0x27 || code[1] != 0xBD) {
        fprintf(stderr, "kyujo: %s at 0x%08X does not start with addiu sp,sp,-N (bytes %02X %02X %02X %02X)\n", name, pool, code[0], code[1], code[2], code[3]);
        host_fatal("the GB Tower emulator's assembly is not where the US ROM has it");
    }
    return pool;
}

extern unsigned long gHostMipsWatchBase;          /* host_mips.c: development aid, see watch_store() there */

void GbCpu_InitOpcodeTables(void *emu, void *tables)
{
    gHostMipsWatchBase = (unsigned long)emu;
    host_mips_flush();
    host_mips_call(entry_of(0x812096A0u, "GbCpu_InitOpcodeTables"), (unsigned int)(unsigned long)emu, (unsigned int)(unsigned long)tables, 0, 0);
}

/* Development aid: KYUJO_GBEMU_DUMP=<file> writes the loaded fragment's memory (link addresses 0x81200000-0x81230000, as the game
 * relocated it) to <file> the first time the emulator is initialised, so that its MIPS code can be disassembled (llvm-mc --triple=mips). */
static void dump_fragment_once(void)
{
    static int done;
    const char *path = getenv("KYUJO_GBEMU_DUMP");
    unsigned int pool;
    FILE *f;
    if (done || !path) return;
    done = 1;
    pool = host_fragment_pool_addr(0x81200000u);
    f = pool ? fopen(path, "wb") : NULL;
    if (!f) return;
    fwrite((const void *)(unsigned long)pool, 1, 0x30000, f);
    fclose(f);
    fprintf(stderr, "kyujo: fragment memory (0x81200000..0x81230000) written to %s\n", path);
}

static int sQuickLoad;   /* the game is in its known-cartridge quick start: the hand-over to the cartridge is still to come */

void GbEmu_InitContext(void *emu)
{
    dump_fragment_once();
    sQuickLoad = 0;
    gHostMipsWatchBase = (unsigned long)emu;
    host_mips_flush();
    host_mips_call(entry_of(0x81209870u, "GbEmu_InitContext"), (unsigned int)(unsigned long)emu, 0, 0, 0);
}

/* Development trace: KYUJO_GBEMU_TRACE=<n> prints the Game Boy CPU's registers (as the core saves them in the emulator state at the end
 * of a call: emu+0x53E4 AF?, +0x53E6, +0x53E8 = the Game Boy PC, +0x53EA = SP; which of the first three are AF, BC, DE, HL is not yet verified) before
 * and after each of the first n calls of GbCpu_Execute. The state is game memory, so the words are read as big-endian bytes. */
static unsigned int be16_at(const unsigned char *p) { return ((unsigned int)p[0] << 8) | p[1]; }

/* The loader state of the game's C code (src/fragments/gb_tower_emulator): D_8122C4DC (s32) is its state (6 = the Game Boy is running its boot
 * while the Transfer Pak script loads the cartridge, 0 = running), D_8122C4F6 (u8) "already handed over to the cartridge". They are game memory,
 * so big-endian. */
extern unsigned char D_8122C4DC[4];
extern unsigned char D_8122C4F6;
extern unsigned char D_8122C4F7;
static unsigned int loader_state(void) { return ((unsigned int)D_8122C4DC[0] << 24) | ((unsigned int)D_8122C4DC[1] << 16) | ((unsigned int)D_8122C4DC[2] << 8) | D_8122C4DC[3]; }
static void set_loader_state(unsigned int v) { D_8122C4DC[0] = (unsigned char)(v >> 24); D_8122C4DC[1] = (unsigned char)(v >> 16); D_8122C4DC[2] = (unsigned char)(v >> 8); D_8122C4DC[3] = (unsigned char)v; }

int GbCpu_Execute(void *emu, int cycles)
{
    static int env_read, left;
    const unsigned char *m = (const unsigned char *)emu;
    int traced = 0, result;
    if (!env_read) {
        const char *e = getenv("KYUJO_GBEMU_TRACE");
        env_read = 1;
        left = e ? atoi(e) : 0;
    }
    if (left > 0) {
        left--;
        traced = 1;
        fprintf(stderr, "gbemu: Execute(cycles %d) in : regs %04X %04X %04X %04X pc %04X\n", cycles, be16_at(m + 0x53E4), be16_at(m + 0x53E6),
                be16_at(m + 0x53E8), be16_at(m + 0x53EA), be16_at(m + 0x53E8));
    }
    {   /* KYUJO_GBEMU_TRACE_CALL=<k>[,<n>]: print the first n (default 3000) interpreted instructions of the k-th call */
        static int calls, tc = -1, tn = 3000;
        if (tc == -1) {
            const char *e = getenv("KYUJO_GBEMU_TRACE_CALL");
            tc = e ? atoi(e) : 0;
            if (e && strchr(e, ',')) tn = atoi(strchr(e, ',') + 1);
        }
        if (++calls == tc) host_mips_trace_next(tn);
    }
    {   /* every 5 s: how fast is the Game Boy running (cycles asked per second against the real 4.194 MHz) and where is its program counter */
        static unsigned long long start;
        static unsigned calls;
        static long long asked;
        unsigned long long now = host_now_ns();
        if (!start) start = now;
        calls++; asked += cycles;
        if (now - start >= 5000000000ULL) {
            double secs = (double)(now - start) / 1e9;
            fprintf(stderr, "kyujo: gbemu: %u calls, avg budget %lld cycles, pc %04X sp %04X\n", calls, calls ? asked / (long long)calls : 0LL, be16_at(m + 0x53E8), be16_at(m + 0x53EA));
            /* loader state (diagnostics for the GB Tower hand-off): game state D_8122C4DC, ROM banks 0/1 and RAM banks 0..3 loaded flags,
             * the Pak service's request/busy/present bytes, "already handed over" D_8122C4F6, run mode D_8122C4F7 */
            fprintf(stderr, "kyujo: gbemu: state %u  rom-flags %02X %02X  ram-flags %02X %02X %02X %02X  game %u/%u  svc req %u busy %u present %u  handed-over %u mode %u\n",
                    loader_state(),
                    m[0x549C], m[0x549D], m[0x559C], m[0x559D], m[0x559E], m[0x559F], m[0x5DC5], m[0x5DC6], m[0x5DC7], m[0x5DC8], m[0x5DCA],
                    (unsigned)D_8122C4F6, (unsigned)D_8122C4F7);
            start = now; calls = 0; asked = 0;
        }
    }
    {   /* log every change of the loader state (state, load flags, service busy/request, hand-over) with the Game Boy pc, to show the order of events */
        static unsigned last_key = 0xFFFFFFFFu;
        const unsigned char *pool = (const unsigned char *)(unsigned long)host_fragment_pool_addr(0x81200000u);
        if (pool) {
            unsigned st = loader_state();
            unsigned key = (st & 0xFF) | ((m[0x549C] != 0) << 8) | ((m[0x549D] != 0) << 9) | ((m[0x559C] != 0) << 10) | ((m[0x559D] != 0) << 11) |
                           ((m[0x559E] != 0) << 12) | ((m[0x559F] != 0) << 13) | ((m[0x5DC7] & 15u) << 16) | ((m[0x5DC8] != 0) << 20) | ((D_8122C4F6 != 0) << 21) |
                           ((be16_at(m + 0x53E8) == 0x100) << 22);
            if (key != last_key) {
                last_key = key;
                fprintf(stderr, "kyujo: gbemu: loader: state %u rom %d%d ram %d%d%d%d svc req %u busy %d handed-over %d pc %04X\n", st, m[0x549C] != 0, m[0x549D] != 0,
                        m[0x559C] != 0, m[0x559D] != 0, m[0x559E] != 0, m[0x559F] != 0, m[0x5DC7], m[0x5DC8] != 0, D_8122C4F6 != 0, be16_at(m + 0x53E8));
            }
        }
    }
    {   /* The game's quick start for a known cartridge (func_8120572C, state 6) lets the Game Boy emulator run its boot logo while the Transfer Pak
         * script loads the cartridge. The hand-over to the cartridge is checked only in that state, in the frame where the Game Boy pc is 0x100
         * and every bank is loaded; the state ends (to 0) as soon as the script is done. On the console the boot logo (a few seconds) is always
         * over long before the 27 s load, but a host that runs the Game Boy slower than real time (the Switch: about half speed) or loads the
         * cartridge faster (pak_real_speed=0) can finish the load first: the state then ends, the hand-over is never checked, and the Game Boy sits at
         * pc 0x100 forever. So while the hand-over is still due, the game is kept in state 6, as it would have been on the console.
         * KYUJO_GBEMU_NOFIX=1 turns this off (development). */
        static int nofix = -1, logged;
        unsigned st = loader_state();
        if (nofix < 0) { const char *e = getenv("KYUJO_GBEMU_NOFIX"); nofix = e && e[0] == '1'; }
        {   /* development: KYUJO_GBEMU_TEST_EARLY0=1 ends the loading state as soon as banks 0 and 1 are loaded and the Game Boy is not yet at pc 0x100,
             * as a service that finishes before a slow boot would */
            static int early = -1;
            if (early < 0) { const char *e = getenv("KYUJO_GBEMU_TEST_EARLY0"); early = e && e[0] == '1'; }
            if (early && st == 6 && m[0x549C] && m[0x549D] && D_8122C4F6 == 0 && be16_at(m + 0x53E8) != 0x100) { sQuickLoad = 1; set_loader_state(0); st = 0; }
        }
        if (st == 6) sQuickLoad = 1;
        else if (D_8122C4F6 != 0) { sQuickLoad = 0; logged = 0; }
        else if (sQuickLoad && st == 0 && !nofix) {
            set_loader_state(6);
            if (!logged) { logged = 1; fprintf(stderr, "kyujo: gbemu: the cartridge load finished before the Game Boy boot (pc %04X): staying in the loading state until the hand-over\n", be16_at(m + 0x53E8)); }
        }
    }
    {   /* development: KYUJO_GBEMU_SLOW_US=<n> makes every call take n microseconds longer, holding the wasm lock as a slow CPU would (the Switch needs ~22 ms) */
        static int slow = -1;
        if (slow < 0) { const char *e = getenv("KYUJO_GBEMU_SLOW_US"); slow = e ? atoi(e) : 0; }
        if (slow > 0) { unsigned long long t0 = host_now_ns(); while (host_now_ns() - t0 < (unsigned long long)slow * 1000ULL) {} }
    }
    result = (int)host_mips_call(entry_of(0x81209EB4u, "GbCpu_Execute"), (unsigned int)(unsigned long)emu, (unsigned int)cycles, 0, 0);
    if (traced) {
        fprintf(stderr, "gbemu: Execute returned 0x%08X; regs %04X %04X %04X %04X, budget field %04X\n", (unsigned int)result, be16_at(m + 0x53E4),
                be16_at(m + 0x53E6), be16_at(m + 0x53E8), be16_at(m + 0x53EA), be16_at(m + 0x53DE));
    }
    return result;
}
