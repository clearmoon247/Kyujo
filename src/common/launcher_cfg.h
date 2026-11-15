/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
// launcher.cfg: which Game Boy cartridge (ROM + save file) sits in the Transfer Pak of each controller port.
//
// Format: "pN_rom=<path>" and "pN_save=<path>" for N = 1..4, one per line, '#' comments, other lines are kept as they are when the
// file is rewritten. A path is either
//   * relative: resolved against the folder that holds launcher.cfg, or
//   * rooted ("/roms/red.gb"): resolved against the device root of that folder (for example "sdmc:" on the console).
// A value written with a device prefix ("sdmc:/roms/red.gb") is accepted when reading and rewritten as a rooted path, because a
// prefix would otherwise be read as part of a relative name.
//
// Everything here works on text and strings (no file access) except the two helpers at the end, so it can be tested on any host.
#pragma once

#include <array>
#include <string>

namespace kyujo {

    constexpr int kPorts = 4;

    struct CartSlot {
        std::string rom;   // as written in the file, device prefix removed; empty = no cartridge
        std::string save;
        bool empty() const { return rom.empty(); }
    };
    using CartSlots = std::array<CartSlot, kPorts>;

    // "sdmc:/x" -> "sdmc:"; "/x" or "x" -> "". A device name is letters/digits/underscore followed by ':' and then '/'.
    std::string device_prefix(const std::string& path);

    // Removes a device prefix: "sdmc:/roms/a.gb" -> "/roms/a.gb". Other paths are returned unchanged.
    std::string strip_device(const std::string& path);

    // Resolves a path from the file against the folder of launcher.cfg (`base_dir`, which may start with a device prefix).
    // resolve("gb/a.gb", "sdmc:/switch/app") == "sdmc:/switch/app/gb/a.gb"; resolve("/roms/a.gb", "sdmc:/switch/app") == "sdmc:/roms/a.gb".
    std::string resolve(const std::string& value, const std::string& base_dir);

    CartSlots read_slots(const std::string& text);

    // Returns `existing` with the slot lines replaced: lines for other keys and comments stay in place; a slot's lines are updated where
    // they are, appended at the end if missing, and removed if the slot is empty. Output always ends with a newline.
    std::string write_slots(const std::string& existing, const CartSlots& slots);

    // File helpers. Both return false on failure and never leave a half-written launcher.cfg.
    bool load_text(const std::string& path, std::string& out);
    // Writes `text` to `path` through "<path>.tmp", keeping the previous file as "<path>.bak". FAT cannot rename over an existing
    // file, so the old backup and the old file are removed or moved aside first.
    bool save_text_with_backup(const std::string& path, const std::string& text);

}  // namespace kyujo
