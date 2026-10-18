# Copyright (c) 2026 High Boy iPod app contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# hb_sim: the High Boy simulator running the iPod app's unmodified sources on
# LVGL 9.4.0 (the version TentacleOS uses) with SDL2 for the window and audio.

include(FetchContent)
find_package(SDL2 REQUIRED)

set(LV_BUILD_CONF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" CACHE PATH "lv_conf.h directory" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG v9.4.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(lvgl)
target_compile_options(lvgl PRIVATE -w)

add_library(hb_audio_sdl STATIC ${SHIM_DIR}/src/hb_audio.c)
target_compile_definitions(hb_audio_sdl PRIVATE HB_WITH_SDL)
target_link_libraries(hb_audio_sdl PUBLIC hb_rt SDL2::SDL2)

add_executable(hb_sim
  src/main.c
  src/hb_ui.c
  src/hb_port.c
  ${SHIM_DIR}/src/hb_fs.c
  ${APP_DIR}/ui_ipod.c
)
target_include_directories(hb_sim PRIVATE src include ${lvgl_SOURCE_DIR}/src)
target_compile_options(hb_sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties(${APP_DIR}/ui_ipod.c PROPERTIES COMPILE_OPTIONS
                            "${APP_WARNINGS}")
target_link_libraries(hb_sim PRIVATE ipod_engine hb_audio_sdl lvgl SDL2::SDL2 m pthread)
# /sdcard -> host folder, the same technique as TentacleOS's HLE simulator.
target_link_options(hb_sim PRIVATE
  -Wl,--wrap=fopen -Wl,--wrap=stat -Wl,--wrap=opendir -Wl,--wrap=mkdir
  -Wl,--wrap=remove -Wl,--wrap=rename -Wl,--wrap=access -Wl,--wrap=rmdir)
