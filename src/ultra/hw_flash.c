/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * hw_flash.c - the cartridge's flash memory chip (the game's save data), emulated.
 *
 * Sources for every rule below (all in the decomp, nothing from memory):
 *   command codes and the window layout ........ lib/ultralib/include/PR/os_internal_flash.h, os_flash.h
 *   how libultra drives the chip ................ lib/ultralib/src/flash/*.c
 *   how THIS game drives it ..................... src/flash.c, src/save_data.c, src/jpeg_stream.c (Flash_EraseSectorByIndex)
 *
 * The chip as the software sees it:
 *   - 128 KB (FLASH_SIZE) at physical 0x08000000 (FLASH_START_ADDR). A command is a 32-bit write to FLASH_START_ADDR + FLASH_CMD_REG.
 *     The top byte is the command, the lower 24 bits an argument (a page number).
 *   - Page = FLASH_BLOCK_SIZE = 128 bytes. The game erases "sector index << 7" pages (src/jpeg_stream.c) and uses sector indices 0..7
 *     (src/save_data.c: arg0 and arg0 + 4): 8 sectors of 128 pages = 16 KB each, which exactly fills the 128 KB.
 *   - Modes: READ_ARRAY (0xF0): reads/DMA return array bytes. STATUS (0xD2): a read returns the status register. ID (0xE1): a DMA
 *     returns the 8-byte silicon ID. PAGE_PROGRAM (0xB4): the next DMA write of 128 bytes fills the page buffer.
 *     PROGRAM_PAGE (0xA5)|page programs the buffer into that page. SECTOR_ERASE (0x4B)|page or CHIP_ERASE (0x3C) selects what
 *     EXECUTE_ERASE (0x78) erases.
 *   - Status bits as the software tests them: bit0 write busy, bit1 erase busy (os_flash.h), bit2 "program ok" and bit3 "erase ok"
 *     (libultra tests `(status & 4) == 4` after programming and `(status & 8) == 8` after erasing). Operations complete instantly
 *     here, so the busy bits are never set. A STATUS-mode write clears the status (Flash_ClearStatus).
 *
 * Status mode after EXECUTE_ERASE / PROGRAM_PAGE: libultra (flash/flashsectorerase.c, flashallerase.c, flashwritearray.c) and this game
 * (src/flash.c) poll the status with plain reads right after writing those commands, with NO STATUS command in between. So on the
 * real chip those two commands must leave it in status mode; this model does the same (found by running the game: without it
 * the poll read array data 0xFFFFFFFF, the busy bit never cleared and the first save flush hung).
 *
 * Assumptions that the decomp does not state (flash memory in general behaves this way, but it is not verified for this chip):
 *   - an erased byte reads 0xFF, and programming can only turn 1 bits into 0 bits (the page buffer is AND-ed into the array);
 *   - the status register is replicated in all four bytes of the word that is read;
 *   - silicon ID = { 0x11118001, FLASH_VERSION_MX_B_AND_D }: libultra only compares the second word against the FLASH_VERSION_*
 *     list to pick the address scaling (NEW_FLASH for MX_B_AND_D), so this selects the "new" layout (devAddr = page * 128). The first
 *     word is not interpreted by the game.
 *
 * Every change is written to the save file at once (host_save_store).
 * GAME-side file: libultra headers only.
 */
#include "PR/os_internal.h"
#include "PR/os_internal_flash.h"
#include "platform.h"
#include "hw_internal.h"

#define SECTOR_PAGES 128u                                       /* pages per erase sector (see above) */
#define SECTOR_BYTES (SECTOR_PAGES * FLASH_BLOCK_SIZE)          /* 16 KB */
#define WIN_BASE FLASH_START_ADDR

typedef enum { MODE_READ_ARRAY, MODE_STATUS, MODE_ID, MODE_PAGE_PROGRAM } FlashMode;
typedef enum { ERASE_NONE, ERASE_SECTOR, ERASE_CHIP } EraseKind;

static u8        sArray[FLASH_SIZE];
static u8        sPage[FLASH_BLOCK_SIZE];
static int       sLoaded;
static FlashMode sMode = MODE_READ_ARRAY;
static EraseKind sEraseKind = ERASE_NONE;
static u32       sEraseSector;
static u32       sStatus;                                       /* bit2 program ok, bit3 erase ok */

static const u32 sSiliconId[2] = { 0x11118001u, FLASH_VERSION_MX_B_AND_D };

/* Loads the save file on first use; a missing file means a blank (fully erased) chip. */
static void ensure_loaded(void)
{
    u32 i;
    if (sLoaded) return;
    sLoaded = 1;
    if (host_save_load(sArray, FLASH_SIZE) != FLASH_SIZE) {
        for (i = 0; i < FLASH_SIZE; i++) sArray[i] = 0xFF;
    }
}

static void persist(void)
{
    host_save_store(sArray, FLASH_SIZE);
}

int flash_owns(u32 a)
{
    return a >= WIN_BASE && a < WIN_BASE + FLASH_SIZE;
}

u32 flash_read(u32 a)
{
    u32 off = a - WIN_BASE;
    ensure_loaded();
    switch (sMode) {
    case MODE_STATUS: return sStatus * 0x01010101u;                /* replicated into every byte */
    case MODE_READ_ARRAY:
        if (off + 4 > FLASH_SIZE) host_fatal("flash read past the end of the chip");
        return ((u32)sArray[off] << 24) | ((u32)sArray[off + 1] << 16) | ((u32)sArray[off + 2] << 8) | sArray[off + 3];
    case MODE_ID:
        if (off >= 8) host_fatal("flash ID read past the 8 ID bytes");
        return sSiliconId[off >> 2];
    default:
        host_missing_register("flash read in page-program mode of physical address", a);
    }
}

void flash_write(u32 a, u32 v)
{
    u32 off = a - WIN_BASE;
    u32 cmd = v & 0xFF000000u, arg = v & 0x00FFFFFFu;
    ensure_loaded();

    if (off != FLASH_CMD_REG) {
        /* The only non-command write the software makes is Flash_ClearStatus: STATUS mode, data written to the window start. */
        if (sMode == MODE_STATUS && off == 0) { sStatus = 0; return; }
        host_missing_register("flash write outside the command register of physical address", a);
    }

    switch (cmd) {
    case FLASH_CMD_READ_ARRAY:   sMode = MODE_READ_ARRAY; break;
    case FLASH_CMD_STATUS:       sMode = MODE_STATUS; break;
    case FLASH_CMD_ID:           sMode = MODE_ID; break;
    case FLASH_CMD_PAGE_PROGRAM: sMode = MODE_PAGE_PROGRAM; break;
    case FLASH_CMD_SECTOR_ERASE: sEraseKind = ERASE_SECTOR; sEraseSector = arg / SECTOR_PAGES; break;
    case FLASH_CMD_CHIP_ERASE:   sEraseKind = ERASE_CHIP; break;
    case FLASH_CMD_EXECUTE_ERASE: {
        u32 i, from = 0, to = FLASH_SIZE;
        if (sEraseKind == ERASE_NONE) host_missing_register("flash EXECUTE_ERASE without a prior erase selection, physical address", a);
        if (sEraseKind == ERASE_SECTOR) {
            if (sEraseSector >= FLASH_SIZE / SECTOR_BYTES) host_fatal("flash sector erase beyond the chip");
            from = sEraseSector * SECTOR_BYTES;
            to = from + SECTOR_BYTES;
        }
        for (i = from; i < to; i++) sArray[i] = 0xFF;
        sEraseKind = ERASE_NONE;
        sStatus |= 0x08;                                           /* erase ok */
        sMode = MODE_STATUS;                                       /* see the header: the chip answers the status poll */
        persist();
        break;
    }
    case FLASH_CMD_PROGRAM_PAGE: {
        u32 i, base = arg * FLASH_BLOCK_SIZE;
        if (base + FLASH_BLOCK_SIZE > FLASH_SIZE) host_fatal("flash page program beyond the chip");
        for (i = 0; i < FLASH_BLOCK_SIZE; i++) sArray[base + i] &= sPage[i];       /* flash can only clear bits */
        sStatus |= 0x04;                                           /* program ok */
        sMode = MODE_STATUS;                                       /* see the header: the chip answers the status poll */
        persist();
        break;
    }
    default:
        host_missing_register("unknown flash command, written at physical address", a);
    }
}

void flash_dma_to_dram(u32 a, void *dst, u32 len)
{
    u32 off = a - WIN_BASE, i;
    u8 *d = dst;
    ensure_loaded();
    if (sMode == MODE_ID) {
        if (off + len > 8) host_fatal("flash ID DMA longer than the 8 ID bytes");
        for (i = 0; i < len; i++) d[i] = (u8)(sSiliconId[(off + i) >> 2] >> (24 - 8 * ((off + i) & 3)));
    } else if (sMode == MODE_READ_ARRAY) {
        if (off + len > FLASH_SIZE) host_fatal("flash DMA read past the end of the chip");
        for (i = 0; i < len; i++) d[i] = sArray[off + i];
    } else {
        host_missing_register("flash DMA read in this mode, physical address", a);
    }
}

void flash_dma_from_dram(u32 a, const void *src, u32 len)
{
    u32 i;
    const u8 *s = src;
    ensure_loaded();
    if (sMode != MODE_PAGE_PROGRAM || a != WIN_BASE || len != FLASH_BLOCK_SIZE) {
        host_missing_register("flash DMA write other than one page in page-program mode, physical address", a);
    }
    for (i = 0; i < FLASH_BLOCK_SIZE; i++) sPage[i] = s[i];
}
