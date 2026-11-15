/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
// Host tests for src/common (settings, stick-to-D-pad, launcher.cfg). No console, no ROM, no third-party test framework.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../src/common/config.h"
#include "../src/common/launcher_cfg.h"
#include "../src/common/stick_dpad.h"

namespace {
    int g_failures = 0;
    int g_checks = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        g_checks++;                                                                      \
        if (!(cond)) {                                                                   \
            g_failures++;                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                                              \
    do {                                                                                                            \
        g_checks++;                                                                                                 \
        const auto va = (a);                                                                                        \
        const auto vb = (b);                                                                                        \
        if (!(va == vb)) {                                                                                          \
            g_failures++;                                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);                             \
        }                                                                                                           \
    } while (0)

    // ---- config -----------------------------------------------------------------------------------------------
    void test_config_defaults() {
        const kyujo::ParseResult r = kyujo::parse_config("");
        CHECK(r.warnings.empty());
        CHECK_EQ(r.config.resolution_factor, 2);
        CHECK(r.config.widescreen == kyujo::Widescreen::Stretch);
        CHECK_EQ(r.config.audio_buffer_frames, 1024);
        CHECK(r.config.nintendo_buttons);
        CHECK(r.config.left_stick_dpad);
        CHECK(r.config.gb_picker == kyujo::PickerMode::Auto);
        CHECK_EQ(r.config.picker_wait_ms, 1500);
        CHECK_EQ(r.config.gb_view, 2);
        CHECK_EQ(r.config.vi_hold, 150);
        CHECK_EQ(r.config.vi_field, 1);
        CHECK(r.config.splash);
        CHECK_EQ(r.config.shader_warm_ms, 45000);
        CHECK_EQ(r.config.shader_prune, 1);
        CHECK_EQ(r.config.shader_opt, 1);
        CHECK(r.config.log_buffered);
        CHECK(!r.config.gbcart_ring);
        CHECK(!r.config.vi_probe);
    }

    void test_config_values() {
        const kyujo::ParseResult r = kyujo::parse_config(
            "# comment\r\n"
            "  Resolution_Factor = 3 \r\n"
            "widescreen=4:3\n"
            "nintendo_buttons=off\n"
            "gb_picker=ALWAYS\n"
            "shader_opt=-1\n"
            "splash=No\n"
            "\n"
            "vi_field=2\n");
        CHECK(r.warnings.empty());
        CHECK_EQ(r.config.resolution_factor, 3);
        CHECK(r.config.widescreen == kyujo::Widescreen::Letterbox);
        CHECK(!r.config.nintendo_buttons);
        CHECK(r.config.gb_picker == kyujo::PickerMode::Always);
        CHECK_EQ(r.config.shader_opt, -1);
        CHECK(!r.config.splash);
        CHECK_EQ(r.config.vi_field, 2);
    }

    void test_config_widescreen_words_and_numbers() {
        CHECK(kyujo::parse_config("widescreen=stretch").config.widescreen == kyujo::Widescreen::Stretch);
        CHECK(kyujo::parse_config("widescreen=adjust").config.widescreen == kyujo::Widescreen::Adjust);
        CHECK(kyujo::parse_config("widescreen=wide").config.widescreen == kyujo::Widescreen::Adjust);
        CHECK(kyujo::parse_config("widescreen=1").config.widescreen == kyujo::Widescreen::Adjust);
        CHECK(kyujo::parse_config("widescreen=0").config.widescreen == kyujo::Widescreen::Letterbox);
    }

    void test_config_bad_values_keep_previous() {
        const kyujo::ParseResult r = kyujo::parse_config(
            "resolution_factor=9\n"       // out of range
            "audio_buffer_frames=abc\n"   // not a number
            "unknown_key=1\n"
            "no equals sign here\n"
            "vi_hold=100\n"
            "vi_hold=1000\n"              // refused: the earlier valid value stays
            "widescreen=sideways\n");
        CHECK_EQ(r.warnings.size(), static_cast<size_t>(6));
        CHECK_EQ(r.config.resolution_factor, 2);
        CHECK_EQ(r.config.audio_buffer_frames, 1024);
        CHECK_EQ(r.config.vi_hold, 100);
        CHECK(r.config.widescreen == kyujo::Widescreen::Stretch);
    }

    void test_config_last_line_wins_and_describe_round_trips() {
        kyujo::ParseResult r = kyujo::parse_config("gb_view=1\ngb_view=3\nshader_prune=2\nsplash=0\n");
        CHECK_EQ(r.config.gb_view, 3);
        const std::string text = kyujo::describe(r.config);
        const kyujo::ParseResult again = kyujo::parse_config(text);
        CHECK(again.warnings.empty());
        CHECK_EQ(kyujo::describe(again.config), text);
        CHECK_EQ(again.config.shader_prune, 2);
        CHECK(!again.config.splash);
    }

    // ---- stick -> D-pad -----------------------------------------------------------------------------------------
    void test_stick_dpad() {
        kyujo::StickDpad s;
        CHECK_EQ(s.update(0, 0), static_cast<uint16_t>(0));
        CHECK_EQ(s.update(17999, 0), static_cast<uint16_t>(0));                 // under the press threshold
        CHECK_EQ(s.update(18000, 0), kyujo::kN64DpadRight);
        CHECK_EQ(s.update(12000, 0), kyujo::kN64DpadRight);                   // hysteresis: stays pressed
        CHECK_EQ(s.update(11999, 0), static_cast<uint16_t>(0));                 // released
        CHECK_EQ(s.update(-20000, 3000), kyujo::kN64DpadLeft);
        CHECK_EQ(s.update(0, -20000), kyujo::kN64DpadUp);                     // Y points down in SDL units
        CHECK_EQ(s.update(0, 20000), kyujo::kN64DpadDown);
        s.reset();
        // a diagonal presses one direction only: the larger axis
        CHECK_EQ(s.update(25000, 20000), kyujo::kN64DpadRight);
        CHECK_EQ(s.update(20000, 25000), kyujo::kN64DpadDown);
        // switching axis while held: the new dominant axis must clear its own (press) threshold only if not already held
        s.reset();
        CHECK_EQ(s.update(20000, 0), kyujo::kN64DpadRight);
        CHECK_EQ(s.update(13000, 14000), static_cast<uint16_t>(0) /* neither axis is over its threshold in the new dominant direction */);
    }

    // ---- launcher.cfg -------------------------------------------------------------------------------------------
    void test_paths() {
        CHECK_EQ(kyujo::device_prefix("sdmc:/roms/a.gb"), std::string("sdmc:"));
        CHECK_EQ(kyujo::device_prefix("/roms/a.gb"), std::string(""));
        CHECK_EQ(kyujo::device_prefix("gb/a.gb"), std::string(""));
        CHECK_EQ(kyujo::device_prefix("C:foo"), std::string(""));
        CHECK_EQ(kyujo::strip_device("sdmc:/roms/a.gb"), std::string("/roms/a.gb"));
        CHECK_EQ(kyujo::resolve("gb/a.gb", "sdmc:/switch/app"), std::string("sdmc:/switch/app/gb/a.gb"));
        CHECK_EQ(kyujo::resolve("gb/a.gb", "sdmc:/switch/app/"), std::string("sdmc:/switch/app/gb/a.gb"));
        CHECK_EQ(kyujo::resolve("/roms/a.gb", "sdmc:/switch/app"), std::string("sdmc:/roms/a.gb"));
        CHECK_EQ(kyujo::resolve("/roms/a.gb", "/tmp/x"), std::string("/roms/a.gb"));
        CHECK_EQ(kyujo::resolve("sdmc:/roms/a.gb", "sdmc:/switch/app"), std::string("sdmc:/roms/a.gb"));
        CHECK_EQ(kyujo::resolve("", "sdmc:/x"), std::string(""));
    }

    void test_read_slots() {
        const kyujo::CartSlots s = kyujo::read_slots(
            "# header\r\n"
            "p1_rom=gb/yellow.gbc\r\n"
            "p1_save=gb/yellow.sav\n"
            "p3_rom = sdmc:/roms/red.gb\n"
            "p2_save=orphan.sav\n"
            "p5_rom=ignored.gb\n"
            "p1_extra=ignored\n");
        CHECK_EQ(s[0].rom, std::string("gb/yellow.gbc"));
        CHECK_EQ(s[0].save, std::string("gb/yellow.sav"));
        CHECK(s[1].empty());
        CHECK_EQ(s[1].save, std::string("orphan.sav"));
        CHECK_EQ(s[2].rom, std::string("/roms/red.gb"));   // device prefix removed
        CHECK(s[3].empty());
    }

    void test_write_slots_keeps_other_lines() {
        const std::string existing =
            "# keep me\n"
            "p1_rom=old.gb\n"
            "something=else\n"
            "p1_save=old.sav\n"
            "p2_rom=drop.gb\n"
            "p2_save=drop.sav\n"
            "p4_rom=dup.gb\n"
            "p4_rom=dup2.gb\n";
        kyujo::CartSlots slots;
        slots[0] = {"new.gb", "new.sav"};
        slots[2] = {"/roms/red.gb", ""};
        slots[3] = {"dup.gb", "dup.sav"};
        const std::string out = kyujo::write_slots(existing, slots);
        const std::string expected =
            "# keep me\n"
            "p1_rom=new.gb\n"
            "something=else\n"
            "p1_save=new.sav\n"
            "p4_rom=dup.gb\n"
            "p3_rom=/roms/red.gb\n"
            "p4_save=dup.sav\n";
        CHECK_EQ(out, expected);
        // reading what was written gives the slots back
        const kyujo::CartSlots back = kyujo::read_slots(out);
        CHECK_EQ(back[0].rom, std::string("new.gb"));
        CHECK(back[1].empty());
        CHECK_EQ(back[2].rom, std::string("/roms/red.gb"));
        CHECK(back[2].save.empty());
        CHECK_EQ(back[3].save, std::string("dup.sav"));
        // empty input and no slots
        CHECK_EQ(kyujo::write_slots("", kyujo::CartSlots{}), std::string(""));
    }

    void test_save_with_backup() {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / ("stadium_cfg_test_" + std::to_string(std::rand()));
        fs::create_directories(dir);
        const std::string path = (dir / "launcher.cfg").string();
        std::string got;
        CHECK(kyujo::save_text_with_backup(path, "one\n"));
        CHECK(kyujo::load_text(path, got));
        CHECK_EQ(got, std::string("one\n"));
        CHECK(!fs::exists(path + ".bak"));
        CHECK(kyujo::save_text_with_backup(path, "two\n"));
        CHECK(kyujo::load_text(path, got));
        CHECK_EQ(got, std::string("two\n"));
        CHECK(kyujo::load_text(path + ".bak", got));
        CHECK_EQ(got, std::string("one\n"));
        CHECK(kyujo::save_text_with_backup(path, "three\n"));   // an old backup must not block the next save
        CHECK(kyujo::load_text(path + ".bak", got));
        CHECK_EQ(got, std::string("two\n"));
        CHECK(!fs::exists(path + ".tmp"));
        CHECK(!kyujo::load_text((dir / "missing").string(), got));
        fs::remove_all(dir);
    }
}  // namespace

int main() {
    test_config_defaults();
    test_config_values();
    test_config_widescreen_words_and_numbers();
    test_config_bad_values_keep_previous();
    test_config_last_line_wins_and_describe_round_trips();
    test_stick_dpad();
    test_paths();
    test_read_slots();
    test_write_slots_keeps_other_lines();
    test_save_with_backup();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
