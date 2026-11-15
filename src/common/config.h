/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
// User settings: a text file of "key=value" lines in the data folder (sdmc:/switch/<data folder>/settings.cfg).
// Portable C++17 with no platform headers, so the host tests drive the same code the console runs.
//
// Rules the parser follows:
//   * one setting per line; blank lines and lines starting with '#' are ignored; spaces around the key and value are trimmed;
//     both LF and CRLF line ends work;
//   * a missing file, a missing key, an unparseable value or a value outside the allowed range leaves the default in place and adds
//     a warning (the caller logs warnings; the program always starts);
//   * a later line for the same key wins;
//   * keys are case-insensitive, values for words (stretch, auto, ...) too.
#pragma once

#include <string>
#include <vector>

namespace kyujo {

    // How the 4:3 picture is shown on a 16:9 screen.
    enum class Widescreen {
        Letterbox = 0,  // 4:3 with black bars left and right, as on the original console
        Adjust = 1,     // 3D scenes widened, 2D screens stay 4:3 in the middle (experimental; menus and transitions can glitch)
        Stretch = 2,    // everything stretched to 16:9, 4:3 art is widened by a third
    };

    // Game Boy cartridge picker at start-up.
    enum class PickerMode {
        Never = 0,
        Auto = 1,    // opens by itself on the first start with ROMs on the card, or when a shoulder button is held
        Always = 2,
    };

    struct Config {
        // Render resolution as a multiple of 320x240: 1 = 320x240, 2 = 640x480, 3 = 960x720.
        int resolution_factor = 2;
        Widescreen widescreen = Widescreen::Stretch;
        // Audio callback size in frames. Buffers under 1024 starve the audio on Horizon with SDL's backend (one buffer is released
        // per ~10 ms service tick).
        int audio_buffer_frames = 1024;
        // true: the game's A (confirm) is the Switch A button on the right and its B (cancel) is the Switch B button below.
        // false: by position (SDL names the buttons by position, the Switch prints them the other way round).
        bool nintendo_buttons = true;
        // The left stick also presses the N64 D-pad. the game's menus read only the D-pad.
        bool left_stick_dpad = true;
        PickerMode gb_picker = PickerMode::Auto;
        // How long (ms) the start-up prompt waits for L/ZL/R/ZR/- to open the picker; 0 = no prompt.
        int picker_wait_ms = 1500;
        // Game Boy Tower screen: 0 as drawn, 1 centred in Game Boy proportions, 2 stretched to fill, 3 in a centred 4:3 box.
        int gb_view = 2;
        // After a video-mode change, pictures that can only come straight from RDRAM (no rendered buffer yet) are garbage; the previous
        // picture is held for at most this many presents (0 = show them). The hold ends at the first rendered picture.
        int vi_hold = 150;
        // Which field of the interlaced 480i menus is shown: 1 upper, 2 lower, 0 alternate (the picture moves one line every frame).
        int vi_field = 1;
        // Show a "please wait" picture with a progress bar while start-up work (shader warm-up) runs.
        bool splash = true;
        // Time budget (ms) for building known shaders before the game starts; 0 = off (shaders then build on demand and stall).
        int shader_warm_ms = 45000;
        // Also build shaders a scan of the game says it may use: 0 none, 1 likely, 2 all.
        int shader_warm_extra = 1;
        // 1: once 40 used shaders are recorded, build only those at start-up; 2: those plus the built-in list; 0: always the full list.
        int shader_prune = 1;
        // Driver shader-compiler optimisation level 0-3, or -1 for the driver default. Lower starts faster and runs shaders slower.
        int shader_opt = 1;
        // Ask the OS for the CPU boost clock during a loading stall.
        bool stall_boost = true;
        // Log files go through memory buffers that a background thread empties; false = one write per line (slow on Horizon).
        bool log_buffered = true;
        // Diagnosis only: write the Transfer Pak access log. Off by default because writing it must not happen under a lock the game uses.
        bool gbcart_ring = false;
        // Diagnosis only: log how the presented picture moves between frames. Slows the game.
        bool vi_probe = false;
    };

    struct ParseResult {
        Config config;
        std::vector<std::string> warnings;
    };

    // Parses settings text. Never fails: problems become warnings.
    ParseResult parse_config(const std::string& text);

    // The effective settings as "key=value" lines, in a fixed order (for the log, and for writing a template).
    std::string describe(const Config& config);

}  // namespace kyujo
