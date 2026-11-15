/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * hw_pif.c - the PIF (the console's controller / security microcontroller) as libultra sees it through the SI registers.
 *
 * How the console works (the part libultra relies on):
 *   - PIF RAM is 64 bytes. libultra builds a "command block" in ordinary memory and copies it into PIF RAM with an SI DMA
 *     (RDRAM -> PIF, started by a write to SI_PIF_ADDR_WR64B_REG). The last byte of the block (offset 0x3F) is a command byte; libultra
 *     sets bit 0 (CONT_CMD_EXE) to ask the PIF to take over the block (the PIF clears the bit again).
 *   - libultra then copies PIF RAM back to memory with a second DMA (PIF -> RDRAM, SI_PIF_ADDR_RD64B_REG) and reads the answers.
 *     THE COMMANDS ARE RUN AT THIS READ DMA, not at the write: ares (ares/n64/pif/hle.cpp, PIF::intA) only clears the bit and parses
 *     the block when it is written, and runs the commands (joyRun) at every read. This matters because libultra often REUSES a block:
 *     __osContRamRead / __osContRamWrite (lib/ultralib/src/io/contramread.c, contramwrite.c) set the EXE bit only when they (re)build
 *     the block, and for the next access of the same kind just change the address and data bytes of the block they got back from the
 *     last read DMA (where the bit is already clear) and DMA it in again. That second write must still make the following read
 *     DMA run the new command. The same idea explains why polling buttons needs only read DMAs (osContStartReadData,
 *     lib/ultralib/src/io/contreaddata.c). My first version ran the commands at the write DMA and failed on the Transfer Pak because of it.
 *   - Both DMAs end with an SI interrupt (MI_INTR_SI).
 *
 * Command block layout. The sizes below are what libultra writes (lib/ultralib/src/io/controller.h, __OSContRequesFormat and
 * __OSContReadFormat) and read back; the walk over the block agrees with an independent PIF parser, lazyusf's pif.c (reached via a search
 * hit; it was only read, not run). Per entry:
 *         [tx size][rx size][ tx bytes: command, arguments... ][ rx bytes: the answer ]
 *   first byte 0xFF  = padding, skipped (does not use up a channel)
 *              0x00  = this channel has no command; go to the next channel
 *              0xFE  = end of the block
 *   rx size bits 6-7 are set by the PIF to report an error (rcp.h: CHNL_ERR_MASK 0xC0); 0x80 = no device answered, which libultra turns
 *   into CONT_NO_RESPONSE_ERROR (os_cont.h).
 *
 * Commands supported (numbers from controller.h; answers from n64brew's Joybus page, https://n64brew.dev/wiki/Joybus_Protocol):
 *     0x00 info, 0xFF reset   3 bytes: device id 0x05 0x00 (standard controller), status byte (0x02 = no pak installed, 0x01 = pak)
 *     0x01 read buttons       4 bytes: buttons (16 bits, high byte first), stick x, stick y (signed bytes)
 *     0x02 read pak           3 bytes sent (command, address high, address low | 5-bit address checksum), 33 answered (32 data, 1 CRC)
 *     0x03 write pak          35 bytes sent (command, address, 32 data), 1 answered (the CRC of the data)
 *   The pak commands are answered only when a Game Boy cartridge was given for the port (host_gbpak.c: a Transfer Pak). Framing as in
 *   n64brew's Joybus page (address checksum table, CRC-8 with polynomial 0x85) and ares (gamepad.cpp: a pak that is not ready answers
 *   with the CRC XOR 0xFF, which libultra reads as "pak changed" and recovers from). Both checksums are tested against libultra's own
 *   routines (lib/ultralib/src/io/crc.c) in tests/native/gbpak.
 * Everything else that a connected controller could be asked (pak commands without a cartridge, EEPROM, voice) is answered "no
 * response": this is an ASSUMPTION about what the hardware does without such an accessory (the sources say only that the info
 * command reports the pak as absent). Commands this file does not know at all stop the program with a message.
 *
 * GAME-side file. PIF RAM is kept as plain bytes, because the DMAs move bytes and the game reads them as big-endian values.
 */
#include "PR/os_internal.h"
#include "PR/rcp.h"
#include "platform.h"
#include "hw_internal.h"

#define PIF_SIZE 64
#define PIF_CHANNELS 4

#define JOYBUS_INFO   0x00
#define JOYBUS_READ_BUTTONS 0x01
#define JOYBUS_RESET  0xFF

#define PIF_ERR_NO_RESPONSE 0x80      /* in the rx size byte */

static u8 sPif[PIF_SIZE];

/* The 5-bit checksum of a pak address (n64brew, Joybus Protocol): XOR of one table value per set address bit, bits 15..5.
 * `addr` is the 16-bit address with the low 5 bits zero. (Not a CRC: n64brew says so explicitly.) */
static u8 pak_addr_checksum(u16 addr)
{
    static const u8 kTable[11] = { 0x01, 0x1A, 0x0D, 0x1C, 0x0E, 0x07, 0x19, 0x16, 0x0B, 0x1F, 0x15 };
    u8 sum = 0;
    int i;
    for (i = 0; i < 11; i++) {
        if (addr & (0x8000 >> i)) sum ^= kTable[i];
    }
    return sum;
}

/* The data CRC: CRC-8, polynomial 0x85, start value 0, most significant bit first (n64brew), over `n` bytes. */
static u8 pak_data_crc(const u8 *data, int n)
{
    u8 crc = 0;
    int i, bit;
    for (i = 0; i < n; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) crc = (crc & 0x80) ? (u8)((crc << 1) ^ 0x85) : (u8)(crc << 1);
    }
    return crc;
}

u32 HostPifRead32(u32 offset)
{
    return ((u32)sPif[offset] << 24) | ((u32)sPif[offset + 1] << 16) | ((u32)sPif[offset + 2] << 8) | sPif[offset + 3];
}

void HostPifWrite32(u32 offset, u32 v)
{
    sPif[offset] = (u8)(v >> 24);
    sPif[offset + 1] = (u8)(v >> 16);
    sPif[offset + 2] = (u8)(v >> 8);
    sPif[offset + 3] = (u8)v;
}

/* Answers one entry. `answer` points at its rx bytes, `rx` is their count, `flag_byte` is the rx size byte (error bits are set there).
 */
static void pif_answer(int channel, int command, const u8 *tx, int txlen, u8 *answer, int rx, u8 *flag_byte)
{
    /* host_input_read returns the state by value (packed), see platform.h for why no pointers are passed for multi-byte results */
    int packed = channel < PIF_CHANNELS ? host_input_read(channel) : -1;
    int present = packed != -1;
    unsigned short buttons = (unsigned short)((unsigned)packed >> 16);
    signed char stick_x = (signed char)(packed >> 8), stick_y = (signed char)packed;

    if (command != JOYBUS_READ_BUTTONS) host_gbpak_trace_cmd(channel, command, txlen > 2 ? tx[1] : 0, txlen > 2 ? tx[2] : 0, rx);
    *flag_byte &= (u8)~0xC0;                                           /* clear the old verdict; it is decided again below */
    if (!present) {
        *flag_byte |= PIF_ERR_NO_RESPONSE;
        return;
    }
    switch (command) {
    case JOYBUS_INFO:
    case JOYBUS_RESET:
        if (rx < 3) host_fatal("PIF: an info command with room for fewer than 3 answer bytes");
        answer[0] = 0x05;                                              /* device id 0x0500: standard controller */
        answer[1] = 0x00;
        {
            int pak = host_gbpak_info_status(channel);                 /* 0x03 / 0x01 with a Transfer Pak, 0 without */
            answer[2] = (u8)(pak ? pak : 0x02);                        /* 0x02 = "no pak installed" */
        }
        break;
    case JOYBUS_READ_BUTTONS:
        if (rx < 4) host_fatal("PIF: a button read with room for fewer than 4 answer bytes");
        answer[0] = (u8)(buttons >> 8);
        answer[1] = (u8)buttons;
        answer[2] = (u8)stick_x;
        answer[3] = (u8)stick_y;
        break;
    case 0x02:                                                         /* read the pak: 32 bytes at a 32-byte aligned address */
    case 0x03:                                                         /* write the pak */
        if (host_gbpak_present(channel)) {
            u16 addr;
            int ok, is_write = command == 0x03;
            if (txlen != (is_write ? 35 : 3) || rx != (is_write ? 1 : 33)) host_fatal("PIF: a pak command with a size libultra never sends");
            addr = (u16)(((tx[1] << 8) | tx[2]) & ~31);
            /* While the "newly inserted" report is still pending, or when the address checksum is wrong, the pak does not act and the
             * CRC is deliberately wrong (XOR 0xFF), the way ares answers; libultra then asks for the status, learns the pak is new and starts over. */
            ok = !host_gbpak_latched(channel) && (tx[2] & 0x1F) == pak_addr_checksum(addr);
            if (!ok) host_gbpak_trace_reject(channel, is_write, addr, host_gbpak_latched(channel), (tx[2] & 0x1F) == pak_addr_checksum(addr));
            if (is_write) {
                if (ok) host_gbpak_write(channel, addr, tx + 3, 32);
                answer[0] = (u8)(pak_data_crc(tx + 3, 32) ^ (ok ? 0x00 : 0xFF));
            } else {
                int i;
                if (ok) host_gbpak_read(channel, addr, answer, 32);
                else for (i = 0; i < 32; i++) answer[i] = 0;
                answer[32] = (u8)(pak_data_crc(answer, 32) ^ (ok ? 0x00 : 0xFF));
            }
        } else {
            *flag_byte |= PIF_ERR_NO_RESPONSE;                         /* ASSUMPTION, see the file comment */
        }
        break;
    case 0x04: case 0x05:                                              /* EEPROM read / write: not used by this game's cartridge */
        *flag_byte |= PIF_ERR_NO_RESPONSE;                             /* ASSUMPTION, see the file comment */
        break;
    default:
        HostHwLogHex("kyujo: PIF: unknown joybus command", (u32)command);
        host_fatal("PIF: a joybus command that is not modelled");
    }
}

/* Time the Joybus wire needs for one command: every bit lasts 4 us (n64brew, Joybus Protocol: "Zero and One bits are each 4 us long"),
 * and the command's bytes and the answer's bytes both travel on the same single wire. Stop bits, the PIF's own processing and the gap
 * between the two halves are NOT counted, so a real transfer is a little slower than this (an assumption on the safe side for
 * ordering: nothing here relies on the delay being exact). A 32-byte pak read is 3 + 33 bytes = 1.152 ms by this formula. */
#define JOYBUS_NS_PER_BYTE (8ULL * 4000ULL)

/* Walks the block in PIF RAM and answers every entry (see the layout in the file comment). Returns the time the wire would have been
 * busy, in nanoseconds (summed over the entries a device answered; a missing device costs nothing here). */
static unsigned long long sPakAccesses;                              /* accessory reads and writes answered so far (shown in the speed line of the log) */
unsigned long long HostPifPakAccesses(void) { return sPakAccesses; }

static unsigned long long pif_run(void)
{
    int pos = 0, channel = 0;
    unsigned long long wire_ns = 0;

    while (pos < PIF_SIZE - 1) {                                       /* byte 0x3F is the command byte, never part of an entry */
        u8 t = sPif[pos];
        if (t == 0xFE) break;
        if (t == 0xFF) { pos++; continue; }
        if (t == 0x00) { channel++; pos++; if (channel > 6) break; continue; }
        if (t & 0xC0) {
            HostHwLogHex("kyujo: PIF: unsupported block byte", t);
            host_fatal("PIF: command block entry that is not modelled (a channel reset?)");
        } else {
            int tx = t, rx = sPif[pos + 1] & 0x3F;
            if (pos + 2 + tx + rx > PIF_SIZE - 1) host_fatal("PIF: a command block entry runs past the end of PIF RAM");
            pif_answer(channel, sPif[pos + 2], sPif + pos + 2, tx, sPif + pos + 2 + tx, rx, &sPif[pos + 1]);
            if (!(sPif[pos + 1] & 0xC0)) {                             /* answered (no error bits set) */
                unsigned long long ns = (unsigned long long)(tx + rx) * JOYBUS_NS_PER_BYTE;
                if (sPif[pos + 2] == 0x02 || sPif[pos + 2] == 0x03) {   /* accessory read / write: the Transfer Pak; its time can be shortened (see host_pak_time_percent) */
                    sPakAccesses++;
                    ns = ns * (unsigned long long)host_pak_time_percent() / 100ULL;
                }
                wire_ns += ns;
            }
            pos += 2 + tx + rx;
            channel++;
        }
    }
    return wire_ns;
}

static int sParsed;                                                    /* a block has been handed over (EXE bit seen) and not been taken away */

/* SI_PIF_ADDR_WR64B_REG: memory -> PIF RAM. If libultra asked for it (EXE bit), the PIF takes the block over and clears the bit. The
 * commands themselves run at the next read DMA (see the file comment). */
void HostPifDmaFromDram(const void *dram)
{
    const u8 *src = (const u8 *)dram;
    int i;
    for (i = 0; i < PIF_SIZE; i++) sPif[i] = src[i];
    if (sPif[PIF_SIZE - 1] & CONT_CMD_EXE_BIT) {
        sPif[PIF_SIZE - 1] &= (u8)~CONT_CMD_EXE_BIT;                   /* "the PIF performs the requested function and then turns off the bit" (n64brew) */
        sParsed = 1;
    }
}

/* SI_PIF_ADDR_RD64B_REG: run the block's commands, then PIF RAM -> memory. Returns how long the Joybus was busy (ns) so the caller can
 * delay the completion interrupt; the data is copied at once, which is harmless because libultra does not look at it before that interrupt. */
unsigned long long HostPifDmaToDram(void *dram)
{
    u8 *dst = (u8 *)dram;
    int i;
    unsigned long long wire_ns = 0;
    if (sParsed) wire_ns = pif_run();
    for (i = 0; i < PIF_SIZE; i++) dst[i] = sPif[i];
    return wire_ns;
}
