/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
#include "config.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <sstream>

namespace kyujo {
    namespace {

        std::string trim(const std::string& s) {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
            return s.substr(b, e - b);
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        bool parse_int(const std::string& v, int& out) {
            if (v.empty()) return false;
            errno = 0;
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (errno != 0 || end == v.c_str() || *end != '\0' || n < -1000000 || n > 1000000) return false;
            out = static_cast<int>(n);
            return true;
        }

        bool parse_bool(const std::string& v, bool& out) {
            const std::string w = lower(v);
            if (w == "1" || w == "on" || w == "true" || w == "yes") { out = true; return true; }
            if (w == "0" || w == "off" || w == "false" || w == "no") { out = false; return true; }
            return false;
        }

        struct Setting {
            const char* key;
            // Returns an empty string on success, otherwise the reason the value was refused.
            std::function<std::string(Config&, const std::string&)> set;
            std::function<std::string(const Config&)> get;
        };

        Setting int_setting(const char* key, int Config::*field, int lo, int hi) {
            return {key,
                    [=](Config& c, const std::string& v) -> std::string {
                        int n = 0;
                        if (!parse_int(v, n)) return "not a whole number";
                        if (n < lo || n > hi) return "out of range " + std::to_string(lo) + ".." + std::to_string(hi);
                        c.*field = n;
                        return "";
                    },
                    [=](const Config& c) { return std::to_string(c.*field); }};
        }

        Setting bool_setting(const char* key, bool Config::*field) {
            return {key,
                    [=](Config& c, const std::string& v) -> std::string {
                        bool b = false;
                        if (!parse_bool(v, b)) return "expected 0/1, on/off, true/false or yes/no";
                        c.*field = b;
                        return "";
                    },
                    [=](const Config& c) { return std::string(c.*field ? "1" : "0"); }};
        }

        const std::vector<Setting>& settings() {
            static const std::vector<Setting> table = {
                int_setting("resolution_factor", &Config::resolution_factor, 1, 3),
                {"widescreen",
                 [](Config& c, const std::string& v) -> std::string {
                     const std::string w = lower(v);
                     int n = 0;
                     if (w == "stretch") c.widescreen = Widescreen::Stretch;
                     else if (w == "adjust" || w == "wide") c.widescreen = Widescreen::Adjust;
                     else if (w == "4:3" || w == "letterbox") c.widescreen = Widescreen::Letterbox;
                     else if (parse_int(w, n) && n >= 0 && n <= 2) c.widescreen = static_cast<Widescreen>(n);
                     else return "expected 0, 1, 2, stretch, adjust or 4:3";
                     return "";
                 },
                 [](const Config& c) { return std::to_string(static_cast<int>(c.widescreen)); }},
                int_setting("audio_buffer_frames", &Config::audio_buffer_frames, 256, 4096),
                bool_setting("nintendo_buttons", &Config::nintendo_buttons),
                bool_setting("left_stick_dpad", &Config::left_stick_dpad),
                {"gb_picker",
                 [](Config& c, const std::string& v) -> std::string {
                     const std::string w = lower(v);
                     if (w == "never" || w == "0") c.gb_picker = PickerMode::Never;
                     else if (w == "auto" || w == "1") c.gb_picker = PickerMode::Auto;
                     else if (w == "always" || w == "2") c.gb_picker = PickerMode::Always;
                     else return "expected auto, always or never";
                     return "";
                 },
                 [](const Config& c) {
                     return std::string(c.gb_picker == PickerMode::Never ? "never" : c.gb_picker == PickerMode::Always ? "always" : "auto");
                 }},
                int_setting("picker_wait_ms", &Config::picker_wait_ms, 0, 10000),
                int_setting("gb_view", &Config::gb_view, 0, 3),
                int_setting("vi_hold", &Config::vi_hold, 0, 240),
                int_setting("vi_field", &Config::vi_field, 0, 2),
                bool_setting("splash", &Config::splash),
                int_setting("shader_warm_ms", &Config::shader_warm_ms, 0, 120000),
                int_setting("shader_warm_extra", &Config::shader_warm_extra, 0, 2),
                int_setting("shader_prune", &Config::shader_prune, 0, 2),
                int_setting("shader_opt", &Config::shader_opt, -1, 3),
                bool_setting("stall_boost", &Config::stall_boost),
                bool_setting("log_buffered", &Config::log_buffered),
                bool_setting("gbcart_ring", &Config::gbcart_ring),
                bool_setting("vi_probe", &Config::vi_probe),
            };
            return table;
        }

    }  // namespace

    ParseResult parse_config(const std::string& text) {
        ParseResult result;
        std::istringstream in(text);
        std::string raw;
        int line_no = 0;
        while (std::getline(in, raw)) {
            line_no++;
            const std::string line = trim(raw);  // also removes a trailing CR
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) {
                result.warnings.push_back("line " + std::to_string(line_no) + ": no '=' in \"" + line + "\"");
                continue;
            }
            const std::string key = lower(trim(line.substr(0, eq)));
            const std::string value = trim(line.substr(eq + 1));
            const auto it = std::find_if(settings().begin(), settings().end(), [&](const Setting& s) { return key == s.key; });
            if (it == settings().end()) {
                result.warnings.push_back("line " + std::to_string(line_no) + ": unknown setting \"" + key + "\"");
                continue;
            }
            Config trial = result.config;
            const std::string problem = it->set(trial, value);
            if (!problem.empty()) {
                result.warnings.push_back("line " + std::to_string(line_no) + ": " + key + "=" + value + ": " + problem + "; keeping " +
                                          it->get(result.config));
                continue;
            }
            result.config = trial;
        }
        return result;
    }

    std::string describe(const Config& config) {
        std::string out;
        for (const Setting& s : settings()) out += std::string(s.key) + "=" + s.get(config) + "\n";
        return out;
    }

}  // namespace kyujo
