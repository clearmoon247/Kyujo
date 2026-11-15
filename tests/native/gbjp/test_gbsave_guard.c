/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * test_gbsave_guard.c - checks that a Pokemon Red/Blue/Yellow save caught mid-write never replaces a good save file
 * (src/platform/host_gbcheck.c and flush_one in src/platform/host_gbpak.c).
 *
 *   cc -I src/platform -o test_gbsave_guard tests/native/gbjp/test_gbsave_guard.c src/platform/host_gbcheck.c src/platform/host_gbjp.c
 *   ./test_gbsave_guard [good.sav [crashed.sav]]     (the optional files are the user's own; never stored in this repository)
 *
 * host_gbpak.c is #included so that its static flush_one can be called; the few host functions it needs are replaced by stubs below.
 * The save layout used is the Stadium's own rule (GbSave_VerifyChecksum), written out again in make_valid(), not taken from the code under test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---- stubs for what host_gbpak.c needs from the rest of the program ---- */
unsigned long long host_now_ns(void) { return 0; }
void host_set_exit_hook(void (*f)(void)) { (void)f; }
#include "../../../src/platform/host_gbpak.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   " __VA_ARGS__); printf("\n"); } } while (0)

/* Makes `ram` a valid save by the Stadium's rule: a 0x50 in the name, bytes 0x2598..0x3523 add up to 0xFF. `fill` varies the content. */
static void make_valid(unsigned char *ram, unsigned char fill)
{
    unsigned i, sum = 0;
    memset(ram, 0, 0x8000);
    for (i = 0x2598; i < 0x3523; i++) ram[i] = (unsigned char)(fill + i * 7);
    ram[0x2598 + 5] = 0x50;
    for (i = 0x2598; i < 0x3523; i++) sum += ram[i];
    ram[0x3523] = (unsigned char)(0xFF - (sum & 0xFF));
}

static int same_file(const char *path, const unsigned char *ram)
{
    unsigned char buf[0x8000];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return n == sizeof buf && memcmp(buf, ram, sizeof buf) == 0;
}

static int exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

static void write_file(const char *path, const unsigned char *ram)
{
    FILE *f = fopen(path, "wb");
    fwrite(ram, 1, 0x8000, f);
    fclose(f);
}

static void setup_pak(GbPak *p, unsigned char *ram, unsigned char *rom, const char *title, const char *path)
{
    memset(p, 0, sizeof *p);
    memset(rom, 0, 0x150);
    memcpy(rom + 0x134, title, strlen(title));
    p->present = 1; p->rom = rom; p->rom_size = 0x150; p->ram = ram; p->ram_size = 0x8000;
    snprintf(p->save_path, sizeof p->save_path, "%s", path);
}

static void check_real(const char *good, const char *bad)
{
    unsigned char a[0x8000], b[0x8000];
    FILE *f;
    if (good && (f = fopen(good, "rb"))) {
        CHECK(fread(a, 1, sizeof a, f) == sizeof a, "read %s", good); fclose(f);
        CHECK(gb_gen1_main_save_valid(a), "the real good save passes the check");
    }
    if (bad && (f = fopen(bad, "rb"))) {
        CHECK(fread(b, 1, sizeof b, f) == sizeof b, "read %s", bad); fclose(f);
        CHECK(!gb_gen1_main_save_valid(b), "the real crashed save fails the check");
    }
}

int main(int argc, char **argv)
{
    static unsigned char good[0x8000], half[0x8000], ram[0x8000], rom[0x150];
    char dir[] = "/tmp/gbsaveXXXXXX", path[300], rej[320], bak[320];
    GbPak p;
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(path, sizeof path, "%s/yellow.sav", dir);
    snprintf(rej, sizeof rej, "%s.rejected", path);
    snprintf(bak, sizeof bak, "%s.bak", path);

    make_valid(good, 1);
    CHECK(gb_gen1_main_save_valid(good), "synthetic save is valid");
    memcpy(half, good, sizeof half);
    half[0x2F34] ^= 0x55;                                    /* a byte of the party changed, checksum byte not yet updated */
    CHECK(!gb_gen1_main_save_valid(half), "a changed byte without the new checksum is invalid");
    memcpy(ram, half, sizeof ram); ram[0x2598 + 5] = 0x41;
    make_valid(ram, 9); ram[0x2598 + 5] = 0x41;
    CHECK(!gb_gen1_main_save_valid(ram), "no 0x50 in the name is invalid");
    CHECK(gb_title_is_gen1_international((const unsigned char *)"POKEMON YELLOW\0"), "title YELLOW");
    CHECK(gb_title_is_gen1_international((const unsigned char *)"POKEMON RED\0\0\0\0"), "title RED");
    CHECK(gb_title_is_gen1_international((const unsigned char *)"POKEMON BLUE\0\0\0"), "title BLUE");
    CHECK(!gb_title_is_gen1_international((const unsigned char *)"POKEMON GREEN\0\0"), "title GREEN is not an international layout");
    CHECK(!gb_title_is_gen1_international((const unsigned char *)"POKEMON YELLOWX"), "a longer title is not YELLOW");

    /* 1. no file yet, valid RAM: written */
    setup_pak(&p, good, rom, "POKEMON YELLOW", path); p.dirty = 1;
    flush_one(&p);
    CHECK(same_file(path, good) && !p.dirty, "valid RAM, no file yet: written");

    /* 2. RAM caught mid-write (invalid), file good: the file must stay as it was */
    memcpy(ram, half, sizeof ram);
    setup_pak(&p, ram, rom, "POKEMON YELLOW", path); p.dirty = 1;
    flush_one(&p);
    CHECK(same_file(path, good), "invalid RAM did not replace the good file");
    CHECK(exists(rej) && same_file(rej, half), "the rejected RAM was kept in <save>.rejected");
    CHECK(p.dirty == 1, "RAM is still marked as not written");
    CHECK(!exists(bak), "no backup was made, the file was not touched");

    /* 3. the game finishes the write (valid again): now it replaces the file */
    make_valid(ram, 3);
    p.rejected_written = 0; p.dirty = 1;
    flush_one(&p);
    CHECK(same_file(path, ram) && !p.dirty, "a complete state replaces the file");

    /* 4. the file on disk is already bad: the new RAM is written even if it is bad (nothing to protect) */
    write_file(path, half);
    memcpy(ram, half, sizeof ram); ram[0x2F35] ^= 1;
    setup_pak(&p, ram, rom, "POKEMON YELLOW", path); p.dirty = 1;
    flush_one(&p);
    CHECK(same_file(path, ram), "bad file + bad RAM: written as before");

    /* 5. other cartridges are not checked */
    write_file(path, good);
    setup_pak(&p, ram, rom, "POKEMON GREEN", path); p.dirty = 1;
    flush_one(&p);
    CHECK(same_file(path, ram), "a cartridge without a known layout is always written");
    write_file(path, good);
    setup_pak(&p, ram, rom, "POKEMON YELLOW", path); p.dirty = 1; p.jp = 1;
    flush_one(&p);
    CHECK(same_file(path, ram), "a Japanese Green is always written");

    check_real(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL);

    snprintf(path, sizeof path, "rm -rf %s", dir);
    if (system(path)) { }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
