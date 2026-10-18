# Copyright (c) 2026 High Boy iPod app contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ESP32-P4 build check: compiles and links the iPod app with ESP-IDF 5.5.3,
# real LVGL 9.4.0 (TentacleOS's LVGL settings) and the real TentacleOS
# headers. Only TentacleOS function bodies are stubbed (main/tentacleos_stubs.c).
# Run through ./run_check.sh.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ipod_esp32p4_check)
