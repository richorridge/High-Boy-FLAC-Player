#!/usr/bin/env bash
# Copyright (c) 2026 High Boy iPod app contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cross-compile and link the iPod app for the High Boy's ESP32-P4.
#   IDF_PATH=~/esp/v5.5.3/esp-idf ./run_check.sh /path/to/TentacleOS
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export TENTACLEOS="$(cd "$1" && pwd)"
. "$IDF_PATH/export.sh" > /dev/null
work="${WORK_DIR:-$here/.work}"
mkdir -p "$work"
rm -rf "$work/project"
mkdir -p "$work/project"
cp -r "$here/CMakeLists.txt" "$here/sdkconfig.defaults" "$here/main" "$work/project/"
mkdir -p "$work/project/components"
if [ ! -d "$work/lvgl" ]; then
  git clone -q --depth 1 --branch v9.4.0 https://github.com/lvgl/lvgl.git "$work/lvgl"
fi
ln -sfn "$work/lvgl" "$work/project/components/lvgl"
sed -i "s#\${CMAKE_CURRENT_LIST_DIR}/../../../app/ipod#$here/../../app/ipod#" "$work/project/main/CMakeLists.txt"
cd "$work/project"
idf.py set-target esp32p4 > /dev/null
idf.py build
echo "ESP32-P4 build OK: $(ls -1 build/*.elf)"
