/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * yay0.c - the Yay0 decompressor, written in C to replace two pieces of the game's hand-written MIPS assembly:
 *     Yay0_Decompress(src, dst)     rom.yaml segment `yay0`            (ROM 0xC3F0..0xC4A0, 176 bytes)
 *     func_80050BA0(src, dst)       rom.yaml segment at ROM 0x517A0    ("yay0 audio decoder", 176 bytes, same size)
 * The decomp does not contain the assembly source (it is assembled from the original bytes), so this is written from the format.
 *
 * Yay0 format (it is MIO0 with the match length extended from 18 to 273; sources: the Paper Mario Yay0 wiki page
 * https://wiki.skelux.root.sx/wiki/paper_mario/yay0 for that statement, and the Yaz0 token coding, which has the same
 * count/distance rules, in libAthena's Compression.cpp). All numbers are big-endian:
 *
 *     0x00  "Yay0"
 *     0x04  u32 size of the decompressed data
 *     0x08  u32 offset (from the start of this header) of the LINK table: one u16 per back-reference
 *     0x0C  u32 offset of the CHUNK stream: literal bytes and long-count bytes, in the order they are needed
 *     0x10  the MASK stream: u32 words; bits are used from the most significant end
 *
 *     Each decoded token takes one mask bit:
 *       1  copy one byte from the CHUNK stream
 *       0  read a u16 from the LINK table:  distance = (low 12 bits) + 1 bytes back from the output position,
 *                                           count    = (high 4 bits) + 2, except that a high nibble of 0 means
 *                                                      "read one byte b from the CHUNK stream, count = b + 0x12" (18..273).
 *          The copy is byte by byte and may overlap its own output (distance 1 repeats the last byte).
 *     Decoding stops when `size` bytes have been produced.
 *
 * This file is GAME-side code (compiled through tools/native/be_memory.py), so the u32/u16 reads below see the big-endian bytes
 * exactly as the console did: no byte swapping is written here on purpose.
 *
 * Validation done (see tools/native/test_yay0.py): round trips of data compressed by an independent Python encoder, and the real
 * Yay0 block of the audio bank inside the player's ROM (decoded size must equal the size the game allocates for it, 0x4120).
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

void host_fatal(const char *message) __attribute__((noreturn));      /* platform.h; not included: the game side has no libc headers */

void Yay0_Decompress(void *src_, void *dst_)
{
    const u8 *src = (const u8 *)src_;
    u8 *dst = (u8 *)dst_;
    u32 size, link_offset, chunk_offset;
    const u32 *mask;
    const u16 *link;
    const u8 *chunk;
    u32 mask_word = 0;
    int bits_left = 0;
    u32 out = 0;

    if (src[0] != 'Y' || src[1] != 'a' || src[2] != 'y' || src[3] != '0') {
        host_fatal("Yay0_Decompress: the data does not start with the Yay0 magic");
    }
    size = *(const u32 *)(src + 4);
    link_offset = *(const u32 *)(src + 8);
    chunk_offset = *(const u32 *)(src + 12);
    mask = (const u32 *)(src + 16);
    link = (const u16 *)(src + link_offset);
    chunk = src + chunk_offset;

    while (out < size) {
        if (bits_left == 0) {
            mask_word = *mask++;
            bits_left = 32;
        }
        if (mask_word & 0x80000000u) {
            dst[out++] = *chunk++;                                   /* literal byte */
        } else {
            u16 entry = *link++;
            u32 distance = (u32)(entry & 0x0FFF) + 1;
            u32 count = (u32)(entry >> 12);
            const u8 *from;
            if (count == 0) {
                count = (u32)(*chunk++) + 0x12;
            } else {
                count += 2;
            }
            if (distance > out) {
                host_fatal("Yay0_Decompress: a back-reference points before the start of the output (corrupt data)");
            }
            from = dst + out - distance;
            while (count-- != 0 && out < size) {                     /* byte by byte: the source may overlap the output */
                dst[out++] = *from++;
            }
        }
        mask_word <<= 1;
        bits_left--;
    }
}

/* The audio code's own copy of the decoder in the original program takes the source as a number (the decomp declares
 * `void func_80050BA0(s32, void *)`). Both decoders implement the same format, as their identical size suggests; this is an
 * assumption that the audio-bank test (test_yay0.py) checks on real data. */
void func_80050BA0(int src, void *dst)
{
    Yay0_Decompress((void *)(unsigned long)src, dst);
}
