#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
# Kyujo - start script for Batocera's "Ports" menu (also works on other Linux systems).
#
# It lives next to the folder "kyujo" (the program and its files). On a first start it
#   - makes the program executable (files copied over a network share often lose that flag),
#   - writes a settings file that starts the game in full screen,
#   - uses the game ROM it finds in the folder (the first *.z64 / *.n64 / *.v64 file) if the settings do not name one yet.
# Everything the program prints goes to kyujo/kyujo.log (the previous run's log is kept as kyujo.log.old).
# Your ROM is NOT included: copy your own game ROM (.z64 file) into the folder "kyujo".

DIR="$(cd "$(dirname "$0")" && pwd)/kyujo"
cd "$DIR" || { echo "folder $DIR not found"; exit 1; }
chmod +x ./kyujo ./kyujo_probe 2>/dev/null

# Batocera starts games with DISPLAY set; from a plain SSH session it is not, so default to the first display.
[ -n "$DISPLAY" ] || export DISPLAY=:0.0

if [ ! -f kyujo.cfg ]; then
    printf 'fullscreen=1\n' > kyujo.cfg
fi

ARGS=()
if ! grep -q '^game_rom=.\+' kyujo.cfg; then
    for f in *.z64 *.n64 *.v64 *.Z64 *.N64 *.V64; do
        if [ -f "$f" ]; then ARGS+=(--rom "$DIR/$f"); break; fi
    done
fi

[ -f kyujo.log ] && mv -f kyujo.log kyujo.log.old
exec ./kyujo "${ARGS[@]}" "$@" > kyujo.log 2>&1
