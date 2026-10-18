#!/usr/bin/env bash
# Copyright (c) 2026 High Boy iPod app contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run one simulator UI script against a fresh copy of a test SD card.
#   run_e2e.sh HB_SIM SCRIPT CARD_DIR [SHOT_DIR]
# A line "# args: ..." in the script adds simulator options.
set -euo pipefail
sim="$1"; script="$2"; card="$3"; shots="${4:-}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp -r "$card" "$work/sd"
rm -rf "$work/sd/.ipod"
args="$(sed -n 's/^# args: //p' "$script" | head -n1)"
# Scripts may write screenshots to $SHOTS; default to the scratch folder.
export SHOTS="${shots:-$work/shots}"
mkdir -p "$SHOTS"
sed "s#\$SHOTS#$SHOTS#g" "$script" > "$work/script.hbs"
HB_AUDIO="${HB_AUDIO:-null}" HB_LOG="${HB_LOG:-warn}" HB_TIME="${HB_TIME:-1792000000}" \
  "$sim" --headless --sd "$work/sd" --script "$work/script.hbs" $args
