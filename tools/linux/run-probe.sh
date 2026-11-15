#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
# Runs the three checks of kyujo_probe (window and keyboard, gamepads, sound) and keeps everything it prints in probe-output.txt.
# Start it from a Batocera SSH session (or any Linux terminal) with the screen and a gamepad in front of you:
#       cd /userdata/roms/ports/kyujo && ./run-probe.sh
cd "$(dirname "$0")" || exit 1
chmod +x ./kyujo_probe 2>/dev/null
[ -n "$DISPLAY" ] || export DISPLAY=:0.0
./kyujo_probe "$@" 2>&1 | tee probe-output.txt
echo
echo "The text above is also saved in $(pwd)/probe-output.txt - please send that file back."
