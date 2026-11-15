/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * rsp_hle.c - what the RSP (the console's signal processor) does with a task, done at a high level instead of by running its microcode.
 *
 * On the console, osSpTaskLoad copies the task description (struct OSTask, 64 bytes, lib/ultralib/include/PR/sptask.h) to the end of
 * DMEM (offset 0xFC0, hw_sp.c), loads a small boot program into IMEM, and osSpTaskStartGo starts the RSP. The boot program loads the
 * task's microcode and data, which then interprets the task's command list: graphics tasks carry a display list (F3DEX2 microcode: this
 * game's GBI is F3DEX_GBI_2), audio tasks a list of audio commands (the "ABI"). This file reads the same description and, for each
 * task type, does the work directly. It does not execute any microcode (except that audio tasks are not routed here at all, see below).
 *
 * Status of each task type:
 *   graphics  (M_GFXTASK): the display list is handed to the host (host_gfx_task, src/render/): its F3DEX2 interpreter transforms and
 *             lights vertices and feeds a software RDP that draws into the game's frame buffer in RDRAM. A list that ends in a full sync
 *             makes the RDP raise its interrupt (returned as bit 1 of the result).
 *   audio     (M_AUDTASK): NOT handled here. hw_sp.c runs the ROM's own audio microcode on the RSP interpreter (src/rsp/rsp_cpu.c),
 *             which is the authority on how the game's audio command list turns into samples.
 *   JPEG      (type 4, this game's own microcode): the host converts the coefficient blocks to pixels (src/render/jpeg_task.c).
 *   others    (M_NULTASK, video, HVQ): not used by this game as far as known; they stop the program with a message.
 *
 * GAME-side file; it only reads the task description from DMEM (big-endian words) and calls the host.
 */
#include "PR/ultratypes.h"
#include "PR/mbi.h"
#include "platform.h"
#include "hw_internal.h"

/* The game's JPEG microcode task type (not in mbi.h; src/jpeg_stream.c sets `type = 4`). */
#define TASK_TYPE_JPEG 4

/* Offsets inside struct OSTask_t (sptask.h), all 32-bit words on the targets of this port. */
#define TASK_TYPE      0x00
#define TASK_FLAGS     0x04
#define TASK_DATA_PTR  0x30
#define TASK_DATA_SIZE 0x34

static u32 rd32(const u8 *b, u32 off)
{
    return ((u32)b[off] << 24) | ((u32)b[off + 1] << 16) | ((u32)b[off + 2] << 8) | b[off + 3];
}

/* A graphics task is started here and run in slices by hw_sp.c: this only hands over the display list the task names. */
void HostRspGfxBegin(const unsigned char *dmem)
{
    const u8 *task = dmem + 0xFC0;
    host_gfx_task_begin(rd32(task, TASK_DATA_PTR));
}

/* Called by hw_sp.c when the RSP is started. `dmem` is the 4096-byte data memory (bytes as the DMA left them, big-endian words). The
 * return value tells which interrupts the finished task owes: bit 0 = SP (always), bit 1 = DP (a graphics task that ended in a full sync). */
unsigned int HostRspRunTask(const unsigned char *dmem)
{
    const u8 *task = dmem + 0xFC0;
    u32 type = rd32(task, TASK_TYPE);
    unsigned int owed = 1;

    host_trace("rsp task type", type, rd32(task, TASK_FLAGS));
    switch (type) {
    case M_GFXTASK: {
        u32 data = rd32(task, TASK_DATA_PTR);
        u32 syncs = host_gfx_task(data);
        if (syncs) owed |= 2;
        break;
    }
    case M_AUDTASK:
        break;                                              /* not reached: hw_sp.c runs audio tasks on the interpreter (see the file comment) */
    case TASK_TYPE_JPEG: {
        /* The game's own JPEG decoder microcode (src/jpeg_stream.c, Jpeg_InitializeDspTask sets type 4): the CPU does the Huffman
         * decoding, the RSP does the inverse DCT and colour conversion of each 16x16 block into the output buffer the task names.
         * The host does that part (src/render/jpeg_task.c, which documents the data layout and what is assumed). */
        {
            unsigned long long t0 = host_now_ns();
            owed |= host_jpeg_task(rd32(task, TASK_DATA_PTR));
            host_perf_add(PERF_JPEG, host_now_ns() - t0);
        }
        break;
    }
    default:
        HostHwLogHex("kyujo: RSP task of unknown type", type);
        host_fatal("RSP task type that this port does not model");
    }
    return owed;
}
