/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_main.c - the program's entry point on the host.
 *
 * On the N64 the boot code (hand-written assembly) sets up a stack and calls Main() in the decomp's src/main.c. Here the host's
 * C start-up code calls main() and this file forwards to the same Main(), so the game starts exactly where it did on hardware.
 *
 * Usage:  kyujo [--rom] PATH-TO-ROM [--save FILE]   (ROM may also come from the environment variable KYUJO_ROM;
 *          a single path without "--rom" is accepted so that dragging the ROM onto the program works; on Windows, with no ROM given at all,
 *          the standard file dialog opens)
 * The ROM is the player's own copy of the supported game (USA); it is checked against the decomp's recorded SHA-1 and never copied.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE                                      /* REG_EIP and friends in <sys/ucontext.h> (Linux crash report below) */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "host_ui.h"
#include "host_crashchain.h"

#ifdef _WIN32
#include <windows.h>

/* The game keeps pointers in 24-bit fields, so the WHOLE program image must lie below 0x01000000 (see tools/native/imagelimit.py). The linker
 * puts it there and switches off ASLR (tools/native/targets.py); this check makes sure Windows really loaded it there, instead of letting the
 * game fail in confusing ways later. NOT run on Windows yet. Returns an error text, or NULL when the image is where it must be. */
static const char *check_image_location(void)
{
    const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt;
    static char msg[256];
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return "cannot read the program's own header";
    nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if ((unsigned long long)(uintptr_t)base + nt->OptionalHeader.SizeOfImage > 0x01000000ULL) {
        snprintf(msg, sizeof msg, "Windows loaded the program at address 0x%08lX, but the game needs the whole program below 0x01000000 "
                 "(its preferred address was 0x%08lX, probably in use). Please report this with your Windows version.",
                 (unsigned long)(uintptr_t)base, (unsigned long)nt->OptionalHeader.ImageBase);
        return msg;
    }
    return NULL;
}

/* ---- no console window --------------------------------------------------------------------------------------------------------------
 * The Windows program is linked as a window program (subsystem WINDOWS, tools/native/targets.py): double-clicking it opens only the game window.
 * Such a program has no stderr, so the program writes its own log: kyujo-log.txt in the program's folder (the log of the run before is
 * kept as kyujo-log-previous.txt). If the program was started with stderr redirected (`kyujo.exe 2> log.txt`, as the old .bat files
 * did) that is left alone. `--console` (or KYUJO_CONSOLE=1), for development, prints to the command prompt that started the program instead.
 * Crashes: without a console nobody would see them, so an unhandled exception is written to the log and shown in a message box.
 * NOT run on Windows yet (compiled only). */
/* Is `n` bytes at `p` readable memory? (VirtualQuery: a bad pointer in a crash report must not crash the report.) */
static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

/* The game's scheduler (decomp src/scheduler.c) keeps its clients in a linked list: the head is D_800A62E0.unk_A10 and each client's `next` is at
 * +0x1D8 (a big-endian pointer: game memory keeps N64 byte order). A crash inside Sched_NotifyClients (the Windows report of a Pikachu/Surf test:
 * a "next" that was garbage, 0x50828B8E) is a damaged list, so the report shows the nodes and the words around each `next`. */
extern unsigned char D_800A62E0[];
static unsigned long be32(const unsigned char *p) { return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3]; }

static void dump_sched_clients(void)
{
    unsigned long node;
    int i;
    if (!readable(D_800A62E0 + 0xA10, 4)) return;
    node = be32(D_800A62E0 + 0xA10);
    fprintf(stderr, "kyujo: CRASH: scheduler client list: head 0x%08lX\n", node);
    for (i = 0; i < 12 && node; i++) {
        const unsigned char *n = (const unsigned char *)(uintptr_t)node;
        if (!readable(n + 0x1C0, 0x24)) { fprintf(stderr, "kyujo: CRASH:   node 0x%08lX is not readable memory\n", node); break; }
        fprintf(stderr, "kyujo: CRASH:   node 0x%08lX  mode(+1DC) 0x%08lX  next(+1D8) 0x%08lX  bytes +1D0..+1DF: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                node, be32(n + 0x1DC), be32(n + 0x1D8), n[0x1D0], n[0x1D1], n[0x1D2], n[0x1D3], n[0x1D4], n[0x1D5], n[0x1D6], n[0x1D7],
                n[0x1D8], n[0x1D9], n[0x1DA], n[0x1DB], n[0x1DC], n[0x1DD], n[0x1DE], n[0x1DF]);
        node = be32(n + 0x1D8);
    }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    const CONTEXT *c = e->ContextRecord;
    char msg[512];
    fprintf(stderr, "kyujo: CRASH: exception 0x%08lX at 0x%08lX\n", (unsigned long)r->ExceptionCode, (unsigned long)(uintptr_t)r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        fprintf(stderr, "kyujo: CRASH: %s address 0x%08lX\n", r->ExceptionInformation[0] == 0 ? "read of" : r->ExceptionInformation[0] == 1 ? "write to" : "execution at",
                (unsigned long)r->ExceptionInformation[1]);
    fprintf(stderr, "kyujo: CRASH: eip %08lX esp %08lX ebp %08lX eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX\n",
            (unsigned long)c->Eip, (unsigned long)c->Esp, (unsigned long)c->Ebp, (unsigned long)c->Eax, (unsigned long)c->Ebx, (unsigned long)c->Ecx,
            (unsigned long)c->Edx, (unsigned long)c->Esi, (unsigned long)c->Edi);
    {   /* who called whom (return addresses; look them up with the symbol file of the same build), then the scheduler's client list */
        char chain[900];
        const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
        if (host_format_frame_chain(chain, sizeof chain, c->Ebp, c->Esp, (unsigned long)(uintptr_t)tib->StackBase, 24))
            fprintf(stderr, "kyujo: CRASH: return addresses, innermost first:\n%s", chain);
        dump_sched_clients();
    }
    fflush(stderr);
    host_gbpak_flush();                                          /* keep what the game wrote to a Game Boy save (a save that fails the game's checksum is not written, host_gbpak.c) */
    snprintf(msg, sizeof msg, "The program crashed (exception 0x%08lX at 0x%08lX).\nPlease send kyujo-log.txt from the program's folder.",
             (unsigned long)r->ExceptionCode, (unsigned long)(uintptr_t)r->ExceptionAddress);
    host_error_box(msg);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void setup_log_and_crash_handler(int argc, char **argv)
{
    int i, want_console = getenv("KYUJO_CONSOLE") != NULL;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    for (i = 1; i < argc; i++) if (strcmp(argv[i], "--console") == 0) want_console = 1;
    if (want_console) {
        if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
            freopen("CONOUT$", "w", stderr);
            freopen("CONOUT$", "w", stdout);
        }
    } else if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN) {   /* no stderr: write the log file */
        char log[1100], prev[1100];
        host_app_path(log, sizeof log, "kyujo-log.txt");
        host_app_path(prev, sizeof prev, "kyujo-log-previous.txt");
        MoveFileExA(log, prev, MOVEFILE_REPLACE_EXISTING);        /* fails harmlessly when there is no old log */
        if (!freopen(log, "w", stderr)) {
            /* the folder is not writable (for example Program Files): nowhere to log, the program still runs */
        }
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    SetUnhandledExceptionFilter(crash_filter);
}
#endif

#if defined(__linux__) && defined(__i386__)
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

/* ---- Linux crash report ---------------------------------------------------------------------------------------------------------------
 * A crash (a bad pointer in the game's code, say) used to end the program with nothing in the log except the last "perf" line, so the
 * user's log said nothing about it. This handler writes the signal, the address that was touched and the registers to the log (stderr),
 * in the same shape as the Windows report, keeps the Game Boy save, and then lets the program die as before (core dump, exit status 139).
 * The program is linked at a fixed address (no PIE), so the instruction address can be looked up with `nm` in the build's program file.
 * Only snprintf (which takes no lock) and write() are used, because the crash may have happened inside stdio. */
static void crash_signal(int sig, siginfo_t *si, void *ctx)
{
    const ucontext_t *uc = (const ucontext_t *)ctx;
    const greg_t *g = uc->uc_mcontext.gregs;
    char msg[600];
    int n;
    n = snprintf(msg, sizeof msg, "\nkyujo: CRASH: signal %d (%s) at eip 0x%08lX, address touched 0x%08lX\n", sig,
                 sig == SIGSEGV ? "segmentation fault" : sig == SIGBUS ? "bus error" : sig == SIGILL ? "illegal instruction" : sig == SIGFPE ? "arithmetic error" : "abort",
                 (unsigned long)g[REG_EIP], (unsigned long)(uintptr_t)si->si_addr);
    n += snprintf(msg + n, sizeof msg - (size_t)n, "kyujo: CRASH: eip %08lX esp %08lX ebp %08lX eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX\n",
                  (unsigned long)g[REG_EIP], (unsigned long)g[REG_ESP], (unsigned long)g[REG_EBP], (unsigned long)g[REG_EAX], (unsigned long)g[REG_EBX],
                  (unsigned long)g[REG_ECX], (unsigned long)g[REG_EDX], (unsigned long)g[REG_ESI], (unsigned long)g[REG_EDI]);
    if (n > 0) { ssize_t w = write(2, msg, (size_t)n); (void)w; }
    host_gbpak_flush();                                          /* keep what the game wrote to a Game Boy save */
    signal(sig, SIG_DFL);                                        /* then die the usual way: the signal is raised again when this handler returns */
}

static void setup_crash_handler(void)
{
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    const size_t altsize = 64 * 1024;                            /* a stack of its own: the crash may be a stack overflow. On the heap: static buffers use up
                                                                  * the room below 0x01000000 (tools/native/imagelimit.py) */
    void *altstack = malloc(altsize);
    stack_t ss;
    struct sigaction sa;
    unsigned i;
    if (altstack) {
        ss.ss_sp = altstack; ss.ss_size = altsize; ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;       /* SA_RESETHAND: the second time it is the default action, so no loop */
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
}
#endif

/* Defined by the decomp (src/main.c). Declared here by hand because HOST-side files do not include the decomp's headers. */
extern void Main(void);

/* An absolute path stays as it is; a relative one (as written in kyujo.cfg) is relative to the program's folder. */
static const char *resolve(char *out, unsigned size, const char *path)
{
    return host_app_path(out, size, path);
}

/* Puts the Game Boy cartridges named by the settings into the Transfer Paks. A cartridge that cannot be loaded is reported (log and a message
 * on the picture) and left out; the game starts anyway, and the menu can fix it. */
static void load_game_paks(void)
{
    HostSettings *cfg = host_settings();
    int i;
#ifdef HOST_SHELL_PICKER
    host_settings_fix_gb_saves();
#endif
    for (i = 0; i < 4; i++) {
        char rom[1100], save[1100], msg[1300];
        const char *err;
        if (!cfg->gb_rom[i][0]) continue;
        resolve(rom, sizeof rom, cfg->gb_rom[i]);
        if (cfg->gb_save[i][0]) resolve(save, sizeof save, cfg->gb_save[i]);
        else host_path_with_ext(rom, ".sav", save, sizeof save);                    /* default: the cartridge file's name with the extension replaced by .sav */
        err = host_gbpak_load(i, rom, save);
        if (err) {
            fprintf(stderr, "kyujo: Game Pak %d: %s\n", i + 1, err);
            snprintf(msg, sizeof msg, "Game Pak %d: %s", i + 1, err);
            host_menu_notice(msg);
        }
    }
}

int main(int argc, char **argv)
{
    HostSettings *cfg = host_settings();
    const char *rom_path = getenv("KYUJO_ROM");
    const char *err;
    int i, rom_path_from_arg = 0, rom_from_settings = 0, rom_from_dialog = 0, cfg_problems;
    char picked[1024], cfg_rom[1100], cfg_save[1100];
    const char *gb_rom = getenv("KYUJO_GB_ROM"), *gb_save = getenv("KYUJO_GB_SAVE");
    const char *save_arg = NULL;

#ifdef _WIN32
    setup_log_and_crash_handler(argc, argv);
    err = check_image_location();
    if (err) {
        fprintf(stderr, "kyujo: %s\n", err);
        host_error_box(err);
        return 2;
    }
#endif
#if defined(__linux__) && defined(__i386__)
    setup_crash_handler();
#endif
    host_set_program_args(argc, argv);
    cfg_problems = host_settings_load(cfg, host_settings_path());       /* -1: no file yet (the defaults are used) */
    fprintf(stderr, "kyujo: settings: %s (%s)\n", host_settings_path(),
            cfg_problems < 0 ? "no file yet, defaults" : (cfg_problems == 0 ? "read" : "read, with problems (see above)"));
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            save_arg = argv[++i];
        } else if (strcmp(argv[i], "--console") == 0) {
            /* handled in setup_log_and_crash_handler (Windows); accepted everywhere so that the same command line works */
        } else if (strcmp(argv[i], "--gb-rom") == 0 && i + 1 < argc) {
            gb_rom = argv[++i];
        } else if (strcmp(argv[i], "--gb-save") == 0 && i + 1 < argc) {
            gb_save = argv[++i];
        } else if (argv[i][0] != '-' && !rom_path_from_arg) {
            rom_path = argv[i];                                  /* a bare path: the ROM (drag and drop onto the program) */
            rom_path_from_arg = 1;
        } else {
            fprintf(stderr, "usage: %s [--rom PATH-TO-game.z64] [--save SAVE-FILE] [--gb-rom GAMEBOY.gb [--gb-save GAMEBOY.sav]]\n"
                            "  Everything is also in the menu (F1 or Esc while the game runs) and in kyujo.cfg next to the program;\n"
                            "  what is given here wins over the settings for this run only.\n"
                            "  (the ROM can also be given in KYUJO_ROM; the save file defaults to kyujo.sav in the program's folder.\n"
                            "   --gb-rom puts a Game Boy cartridge into the Transfer Pak of controller 1, for GB Tower; environment variables\n"
                            "   KYUJO_GB_ROM / KYUJO_GB_SAVE do the same. Without --gb-save the save is the ROM's name with .sav.\n"
                            "   The Game Boy save file is changed when the game writes to it; the original is kept once as <save>.bak.)\n", argv[0]);
            return 2;
        }
    }
    if (save_arg) host_save_set_path(save_arg);
    else {
        char dflt[1024];
        host_app_path(dflt, sizeof dflt, "kyujo.sav");            /* the default: in the program's folder, whatever the current folder is */
        if (cfg->save[0]) {
            const char *want = resolve(cfg_save, sizeof cfg_save, cfg->save);
            if (host_save_can_write(want)) host_save_set_path(want);
            else {                                              /* typically a settings file copied from another computer: its folders do not exist here */
                fprintf(stderr, "kyujo: the save file in the settings cannot be created here ('%s'); using '%s' instead\n", want, dflt);
                host_save_set_path(dflt);
            }
        } else host_save_set_path(dflt);
    }
    if (!rom_path && cfg->rom[0]) {                              /* the ROM chosen the last time */
        rom_path = resolve(cfg_rom, sizeof cfg_rom, cfg->rom);
        rom_from_settings = 1;
    }
    if (!rom_path) {
        rom_path = host_window_pick_rom(picked, sizeof picked);  /* Windows only: the file dialog; NULL elsewhere or if cancelled */
        rom_from_dialog = rom_path != NULL;
    }
    if (!rom_path) {
        fprintf(stderr, "kyujo: no ROM given. This program needs your own copy of Pokemon Stadium (USA).\n"
                        "  run:  %s --rom PATH-TO-ROM     (or set KYUJO_ROM)\n", argv[0]);
        host_error_box("No ROM was chosen. This program needs your own copy of Pokemon Stadium (USA).");
        return 2;
    }
    err = host_rom_load(rom_path);
    if (err && rom_from_settings) {                              /* the remembered ROM is gone or changed: ask again instead of giving up */
        fprintf(stderr, "kyujo: the ROM in the settings cannot be used (%s)\n", err);
        rom_path = host_window_pick_rom(picked, sizeof picked);
        rom_from_dialog = rom_path != NULL;
        rom_from_settings = 0;
        if (rom_path) err = host_rom_load(rom_path);
    }
    if (err) {
        fprintf(stderr, "kyujo: %s\n", err);
        host_error_box(err);
        return 2;
    }
    fprintf(stderr, "kyujo: ROM accepted (%u bytes)\n", host_rom_size());
    if (rom_from_dialog) {                                       /* remember the ROM the player picked, so the next start does not ask again */
        snprintf(cfg->rom, sizeof cfg->rom, "%s", rom_path);
        if (host_settings_save(cfg, host_settings_path()) != 0) fprintf(stderr, "kyujo: could not remember the ROM (settings file not writable)\n");
    }
    if (gb_rom && gb_rom[0]) {                                   /* the command line / environment cartridge is the one in controller 1's Transfer Pak */
        snprintf(cfg->gb_rom[0], sizeof cfg->gb_rom[0], "%s", gb_rom);
        snprintf(cfg->gb_save[0], sizeof cfg->gb_save[0], "%s", gb_save && gb_save[0] ? gb_save : "");
    }
    host_window_prepare();                                       /* Windows: the window is created now (it takes tens of ms; the first picture must not wait for it while the first sound plays) */
    host_audio_prepare();                                        /* Windows: the sound device is opened now, in the background */
    load_game_paks();
    /* Game memory is big-endian (tools/native/be_memory.py); initialised game data must be reversed once before any game code runs. */
    fprintf(stderr, "kyujo: reversed the initialised data of %u game object files; starting the game's Main()\n", host_swap_game_data());
    host_fragments_snapshot();                                   /* the overlays' variables as they start; restored on every load of an overlay */
    host_init_rom_data();
    host_control_start();                                        /* development aid; does nothing unless KYUJO_CONTROL is set */
    host_movetest_init();                                        /* the move tester (host_movetest.c): does nothing unless KYUJO_MOVETEST is set */
    host_trace_init();                                           /* development aid; does nothing unless KYUJO_TRACE is set (host_trace.c) */
    host_perf_start_profiler();                                  /* development aid; does nothing unless KYUJO_PROFILE is set (Linux) */
    host_first_run_setup();                                      /* the shell builds: the menu wallpaper for the start-up picture, the Game Pak Setup page on a first start, then the shaders are prepared */
    Main();
    /* On hardware Main() returns into an idle loop that never ends; reaching this line means the idle thread is not emulated. */
    fprintf(stderr, "kyujo: Main() returned\n");
    return 0;
}
