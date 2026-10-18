# Copyright (c) 2026 High Boy FLAC iPod contributors
# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT DEFINED ENV{TENTACLEOS})
  message(FATAL_ERROR "set TENTACLEOS to a TentacleOS checkout (see run_check.sh)")
endif()
set(APP ${CMAKE_CURRENT_LIST_DIR}/../../../app/ipod)
set(TOS $ENV{TENTACLEOS}/firmware_p4/components)
file(GLOB APP_SRCS ${APP}/*.c)
idf_component_register(
  SRCS main.c tentacleos_stubs.c ${APP_SRCS} ${APP}/port/ipod_port_tentacleos.c
  INCLUDE_DIRS
    ${APP}/include
    ${APP}/third_party
    ${TOS}/Applications/ui/include
    ${TOS}/Drivers/input_manager/include
    ${TOS}/Drivers/audio_i2s/include
    ${TOS}/Drivers/bq25896/include
    ${TOS}/Drivers/sys_prio/include
    ${TOS}/Service/sys_time/include
    ${TOS}/Service/storage_vfs/include
  REQUIRES lvgl freertos esp_timer)
# The same strictness the app is held to on the host, on top of IDF's defaults.
set_source_files_properties(${APP_SRCS} ${APP}/port/ipod_port_tentacleos.c PROPERTIES
  COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter;-Wshadow;-Werror")
set_source_files_properties(${APP}/ipod_codec_impl.c PROPERTIES COMPILE_OPTIONS "-w")
