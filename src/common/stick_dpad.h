/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
// Left stick -> D-pad bits.
//
// the game's menus read only the D-pad (the game's controller code tests the 0x0800/0x0400/0x0200/0x0100 button bits and never the
// analog stick), so to navigate a menu with the stick the stick must also press the D-pad. Rules:
//   * at most one direction at a time: whichever axis is further from centre wins (a diagonal never presses two directions);
//   * hysteresis: pressing needs |axis| >= kOn, but an already-pressed axis stays pressed until |axis| falls under kOff, so a stick
//     resting near the edge does not flicker;
//   * input is in raw SDL axis units (-32768..32767) with Y pointing down; the returned bits use the N64 layout.
//
// The analog value itself is passed to the game separately; this only adds D-pad bits.
#pragma once

#include <cstdint>

namespace kyujo {

    constexpr int32_t kStickDpadOn = 18000;
    constexpr int32_t kStickDpadOff = 12000;

    constexpr uint16_t kN64DpadUp = 0x0800;
    constexpr uint16_t kN64DpadDown = 0x0400;
    constexpr uint16_t kN64DpadLeft = 0x0200;
    constexpr uint16_t kN64DpadRight = 0x0100;

    class StickDpad {
    public:
        // Call once per poll with the current stick position. Returns the D-pad bits to OR into the button word.
        uint16_t update(int32_t x, int32_t y) {
            const int32_t ax = x < 0 ? -x : x;
            const int32_t ay = y < 0 ? -y : y;
            const bool horizontal_held = left_ || right_;
            const bool vertical_held = up_ || down_;
            const bool horizontal = ax >= ay && ax >= (horizontal_held ? kStickDpadOff : kStickDpadOn);
            const bool vertical = ay > ax && ay >= (vertical_held ? kStickDpadOff : kStickDpadOn);
            left_ = horizontal && x < 0;
            right_ = horizontal && x > 0;
            up_ = vertical && y < 0;
            down_ = vertical && y > 0;
            return static_cast<uint16_t>((up_ ? kN64DpadUp : 0) | (down_ ? kN64DpadDown : 0) | (left_ ? kN64DpadLeft : 0) |
                                         (right_ ? kN64DpadRight : 0));
        }

        void reset() { up_ = down_ = left_ = right_ = false; }

    private:
        bool up_ = false, down_ = false, left_ = false, right_ = false;
    };

}  // namespace kyujo
