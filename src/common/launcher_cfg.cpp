/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
#include "launcher_cfg.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace kyujo {
    namespace {

        std::string trim(const std::string& s) {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
            return s.substr(b, e - b);
        }

        // Splits "pN_rom=value" into the port index (0..3), whether it is the ROM key, and the value. False for any other line.
        bool slot_line(const std::string& line, int& port, bool& is_rom, std::string& value) {
            const std::string t = trim(line);
            if (t.size() < 7 || t[0] != 'p' || t[1] < '1' || t[1] > '4' || t[2] != '_') return false;
            const size_t eq = t.find('=');
            if (eq == std::string::npos) return false;
            const std::string key = trim(t.substr(0, eq));
            if (key.size() == 6 && key.compare(2, 4, "_rom") == 0) is_rom = true;
            else if (key.size() == 7 && key.compare(2, 5, "_save") == 0) is_rom = false;
            else return false;
            port = key[1] - '1';
            value = trim(t.substr(eq + 1));
            return true;
        }

        std::vector<std::string> split_lines(const std::string& text) {
            std::vector<std::string> lines;
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(line);
            }
            return lines;
        }

        std::string join_path(const std::string& a, const std::string& b) {
            if (a.empty()) return b;
            if (a.back() == '/') return a + b;
            return a + "/" + b;
        }

    }  // namespace

    std::string device_prefix(const std::string& path) {
        size_t i = 0;
        while (i < path.size() && (std::isalnum(static_cast<unsigned char>(path[i])) || path[i] == '_')) i++;
        if (i > 0 && i + 1 < path.size() && path[i] == ':' && path[i + 1] == '/') return path.substr(0, i + 1);
        return "";
    }

    std::string strip_device(const std::string& path) {
        const std::string dev = device_prefix(path);
        return dev.empty() ? path : path.substr(dev.size());
    }

    std::string resolve(const std::string& value, const std::string& base_dir) {
        if (value.empty()) return value;
        const std::string dev = device_prefix(value);
        if (!dev.empty()) return value;                       // already absolute with a device
        if (value[0] == '/') return device_prefix(base_dir) + value;  // rooted on the device of the base folder
        return join_path(base_dir, value);
    }

    CartSlots read_slots(const std::string& text) {
        CartSlots slots;
        for (const std::string& line : split_lines(text)) {
            int port = 0;
            bool is_rom = false;
            std::string value;
            if (!slot_line(line, port, is_rom, value)) continue;
            (is_rom ? slots[port].rom : slots[port].save) = strip_device(value);
        }
        return slots;
    }

    std::string write_slots(const std::string& existing, const CartSlots& slots) {
        std::vector<std::string> out;
        std::array<bool, kPorts> rom_done{}, save_done{};
        for (const std::string& line : split_lines(existing)) {
            int port = 0;
            bool is_rom = false;
            std::string value;
            if (!slot_line(line, port, is_rom, value)) {
                out.push_back(line);
                continue;
            }
            const std::string& wanted = is_rom ? slots[port].rom : slots[port].save;
            auto& done = is_rom ? rom_done : save_done;
            if (slots[port].empty() || wanted.empty() || done[port]) continue;  // empty slot, no save wanted, or a duplicate line
            out.push_back("p" + std::to_string(port + 1) + (is_rom ? "_rom=" : "_save=") + wanted);
            done[port] = true;
        }
        for (int port = 0; port < kPorts; port++) {
            if (slots[port].empty()) continue;
            if (!rom_done[port]) out.push_back("p" + std::to_string(port + 1) + "_rom=" + slots[port].rom);
            if (!save_done[port] && !slots[port].save.empty()) out.push_back("p" + std::to_string(port + 1) + "_save=" + slots[port].save);
        }
        std::string text;
        for (const std::string& line : out) text += line + "\n";
        return text;
    }

    bool load_text(const std::string& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    bool save_text_with_backup(const std::string& path, const std::string& text) {
        const std::string tmp = path + ".tmp";
        const std::string bak = path + ".bak";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f << text;
            f.flush();
            if (!f) {
                f.close();
                std::remove(tmp.c_str());
                return false;
            }
        }
        std::ifstream existing(path, std::ios::binary);
        const bool had_old = static_cast<bool>(existing);
        existing.close();
        if (had_old) {
            std::remove(bak.c_str());
            if (std::rename(path.c_str(), bak.c_str()) != 0) {
                std::remove(tmp.c_str());
                return false;
            }
        }
        if (std::rename(tmp.c_str(), path.c_str()) != 0) {
            if (had_old) std::rename(bak.c_str(), path.c_str());  // put the old file back
            std::remove(tmp.c_str());
            return false;
        }
        return true;
    }

}  // namespace kyujo
