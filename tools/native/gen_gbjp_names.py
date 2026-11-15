#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Writes src/platform/gbjp_names.inc: for each Japanese Game Boy character code (Pokemon Red/Green, Japan) the Latin letters that stand for it.

    gen_gbjp_names.py --rom Pokemon_Stadium_2__Japan_.z64 --out src/platform/gbjp_names.inc [--check]

Why: the US Stadium's font has no kana, but it has to show the names stored in a Japanese save (trainer, rival, Pokemon nicknames and
original trainers). host_gbjp.c writes them in Latin letters, as romaji ("ラプラス" -> "RAPURASU").

Where the code -> character mapping comes from: the Japanese Stadium 2 (CP2J) itself. Its ROM holds a table of 256 big-endian 16-bit EUC-JP
codes, one per Game Boy character code (the decomp calls it D_80076FD4_jp and uses it in Text_UntranscodeName, src/gb_data.c). That table
is the authority: this script finds it in the ROM (it starts 0x80 entries before the run of katakana ア イ ウ エ オ) and decodes it.
It was cross-checked against a real Pocket Monsters Midori save: the names there decode to グリーン, ラプラス, フーディン, ウツボット,
ゴローニャ, ライチュウ, リザードン (the player, Lapras, Alakazam, Victreebel, Golem, Raichu, Charizard).

The romaji of each kana is written out below (Hepburn: SHI, CHI, TSU, FU, JI). The .inc holds only the result, so building the port does
not need the ROM; running this script with --check against the ROM re-derives the file and fails on any difference.
Standard library only.
"""
import argparse
import sys

# Katakana and hiragana in gojuon order with their romaji. Each pair of strings is (katakana, hiragana) for one syllable.
SYLLABLES = [
    ("ア", "あ", "A"), ("イ", "い", "I"), ("ウ", "う", "U"), ("エ", "え", "E"), ("オ", "お", "O"),
    ("カ", "か", "KA"), ("キ", "き", "KI"), ("ク", "く", "KU"), ("ケ", "け", "KE"), ("コ", "こ", "KO"),
    ("サ", "さ", "SA"), ("シ", "し", "SHI"), ("ス", "す", "SU"), ("セ", "せ", "SE"), ("ソ", "そ", "SO"),
    ("タ", "た", "TA"), ("チ", "ち", "CHI"), ("ツ", "つ", "TSU"), ("テ", "て", "TE"), ("ト", "と", "TO"),
    ("ナ", "な", "NA"), ("ニ", "に", "NI"), ("ヌ", "ぬ", "NU"), ("ネ", "ね", "NE"), ("ノ", "の", "NO"),
    ("ハ", "は", "HA"), ("ヒ", "ひ", "HI"), ("フ", "ふ", "FU"), ("ヘ", "へ", "HE"), ("ホ", "ほ", "HO"),
    ("マ", "ま", "MA"), ("ミ", "み", "MI"), ("ム", "む", "MU"), ("メ", "め", "ME"), ("モ", "も", "MO"),
    ("ヤ", "や", "YA"), ("ユ", "ゆ", "YU"), ("ヨ", "よ", "YO"),
    ("ラ", "ら", "RA"), ("リ", "り", "RI"), ("ル", "る", "RU"), ("レ", "れ", "RE"), ("ロ", "ろ", "RO"),
    ("ワ", "わ", "WA"), ("ヲ", "を", "WO"), ("ン", "ん", "N"),
    # voiced (dakuten) and semi-voiced (handakuten)
    ("ガ", "が", "GA"), ("ギ", "ぎ", "GI"), ("グ", "ぐ", "GU"), ("ゲ", "げ", "GE"), ("ゴ", "ご", "GO"),
    ("ザ", "ざ", "ZA"), ("ジ", "じ", "JI"), ("ズ", "ず", "ZU"), ("ゼ", "ぜ", "ZE"), ("ゾ", "ぞ", "ZO"),
    ("ダ", "だ", "DA"), ("ヂ", "ぢ", "JI"), ("ヅ", "づ", "ZU"), ("デ", "で", "DE"), ("ド", "ど", "DO"),
    ("バ", "ば", "BA"), ("ビ", "び", "BI"), ("ブ", "ぶ", "BU"), ("ベ", "べ", "BE"), ("ボ", "ぼ", "BO"),
    ("パ", "ぱ", "PA"), ("ピ", "ぴ", "PI"), ("プ", "ぷ", "PU"), ("ペ", "ぺ", "PE"), ("ポ", "ぽ", "PO"),
]
# Other characters. Small kana are given as a marker the converter understands: "^TSU" (doubles the next consonant), "^YA", "^YU", "^YO"
# (merge with a preceding -I syllable: キャ -> KYA), and "^A" "^I" "^U" "^E" "^O" (small vowels: the vowel is appended).
OTHERS = {
    "ッ": "^TSU", "っ": "^TSU", "ャ": "^YA", "ゃ": "^YA", "ュ": "^YU", "ゅ": "^YU", "ョ": "^YO", "ょ": "^YO",
    "ァ": "^A", "ぁ": "^A", "ィ": "^I", "ぃ": "^I", "ゥ": "^U", "ぅ": "^U", "ェ": "^E", "ぇ": "^E", "ォ": "^O", "ぉ": "^O",
    "ー": "-", "。": ".", "．": ".", "、": ",", "？": "?", "！": "!", "♂": "", "♀": "",   # the sex signs keep their own codes (0xEF, 0xF5) in the output
    "　": " ",
}
for i, ch in enumerate("０１２３４５６７８９"):
    OTHERS[ch] = str(i)
for i in range(26):
    OTHERS[chr(0xFF21 + i)] = chr(ord("A") + i)            # full-width Latin capitals


def romaji_map():
    m = dict(OTHERS)
    for k, h, r in SYLLABLES:
        m[k] = r
        m[h] = r
    return m


def find_table(rom):
    """The 256-entry EUC-JP table: it has ア イ ウ エ オ (A5A2 A5A4 A5A6 A5A8 A5AA) at entries 0x80..0x84."""
    pat = bytes.fromhex("A5A2A5A4A5A6A5A8A5AA")
    hits = []
    i = rom.find(pat)
    while i >= 0:
        base = i - 0x80 * 2
        if base >= 0 and rom[base:base + 2] == b"\xa1\xa1":          # entry 0 is the full-width space
            hits.append(base)
        i = rom.find(pat, i + 1)
    if len(hits) != 1:
        sys.exit("expected exactly one EUC-JP table in the ROM, found %d" % len(hits))
    return hits[0]


def build(rom):
    base = find_table(rom)
    rm = romaji_map()
    rows = []
    unknown = []
    for code in range(256):
        v = int.from_bytes(rom[base + 2 * code:base + 2 * code + 2], "big")
        if v == 0:
            rows.append((code, None, ""))
            continue
        ch = bytes([v >> 8, v & 0xFF]).decode("euc-jp")
        if ch not in rm:
            unknown.append((code, ch))
            rows.append((code, ch, "?"))
        else:
            rows.append((code, ch, rm[ch]))
    return base, rows, unknown


def render(base, rows):
    out = ["/* GENERATED by tools/native/gen_gbjp_names.py from the Japanese Stadium 2 ROM's character table (at ROM 0x%X). Do not edit.\n"
           " * One entry per Japanese Game Boy character code: the Latin letters that stand for it (\"\" = nothing, e.g. a blank code;\n"
           " * \"^...\" = small kana, see host_gbjp.c). The comment shows the character. */\n" % base]
    for code, ch, r in rows:
        out.append("    /* 0x%02X %s */ \"%s\",\n" % (code, ch if ch else "-", r.replace("\\", "\\\\")))
    return "".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--check", action="store_true", help="do not write; fail if the file differs")
    a = ap.parse_args()
    base, rows, unknown = build(open(a.rom, "rb").read())
    if unknown:
        sys.exit("characters without romaji: " + ", ".join("0x%02X %s" % u for u in unknown))
    text = render(base, rows)
    if a.check:
        if open(a.out, encoding="utf-8").read() != text:
            sys.exit("%s differs from what the ROM gives" % a.out)
        print("ok: %s matches the ROM" % a.out)
    else:
        open(a.out, "w", encoding="utf-8").write(text)
        print("wrote %s (%d entries, table at ROM 0x%X)" % (a.out, len(rows), base))


if __name__ == "__main__":
    main()
