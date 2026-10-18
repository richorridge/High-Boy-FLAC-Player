# Copyright (c) 2026 High Boy FLAC iPod contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host build: the app's core modules, unit tests, and the SDL2 High Boy
# simulator. The device build is TentacleOS's ESP-IDF build (see integration/).

cmake_minimum_required(VERSION 3.16)
project(highboy_flac_ipod C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(HB_BUILD_SIM "Build the SDL2 High Boy simulator" ON)
option(HB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

if(HB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                      -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/app/ipod)
set(SHIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sim/shim)
set(APP_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wshadow -Wpointer-arith
                 -Wstrict-prototypes -Wmissing-prototypes -Werror)

# ── Simulator runtime shims (FreeRTOS on pthreads, ESP log) ──────────────────
add_library(hb_rt STATIC ${SHIM_DIR}/src/hb_freertos.c)
target_include_directories(hb_rt PUBLIC ${SHIM_DIR}/include)
target_link_libraries(hb_rt PUBLIC pthread m)

# ── Vendored codecs (dr_flac, stb_image) ─────────────────────────────────────
add_library(ipod_codecs STATIC ${APP_DIR}/ipod_codec_impl.c)
target_include_directories(ipod_codecs PUBLIC ${APP_DIR}/third_party)
target_compile_options(ipod_codecs PRIVATE -w)

# ── App core: everything below the UI, no LVGL ───────────────────────────────
add_library(ipod_core STATIC
  ${APP_DIR}/ipod_text.c
  ${APP_DIR}/ipod_meta.c
  ${APP_DIR}/ipod_library.c
  ${APP_DIR}/ipod_queue.c
  ${APP_DIR}/ipod_dsp.c
  ${APP_DIR}/ipod_settings.c
  ${APP_DIR}/ipod_menu.c
)
target_include_directories(ipod_core PUBLIC ${APP_DIR}/include)
target_compile_options(ipod_core PRIVATE ${APP_WARNINGS})
target_link_libraries(ipod_core PUBLIC ipod_codecs hb_rt)

# ── App engine: decoder task + art worker (talks to audio_i2s) ───────────────
add_library(ipod_engine STATIC
  ${APP_DIR}/ipod_player.c
  ${APP_DIR}/ipod_art.c
)
target_compile_options(ipod_engine PRIVATE ${APP_WARNINGS})
target_link_libraries(ipod_engine PUBLIC ipod_core)

# Simulator audio_i2s without a sound card (tests); the simulator adds SDL.
add_library(hb_audio_headless STATIC ${SHIM_DIR}/src/hb_audio.c)
target_link_libraries(hb_audio_headless PUBLIC hb_rt)

# ── Tests ────────────────────────────────────────────────────────────────────
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(TEST_LIB ${CMAKE_BINARY_DIR}/testlib)

add_test(NAME make_test_library
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_test_library.py
                 ${TEST_LIB} --seconds 4)
set_tests_properties(make_test_library PROPERTIES FIXTURES_SETUP testlib)

add_executable(test_core ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_core.c)
target_compile_options(test_core PRIVATE ${APP_WARNINGS} -Wno-missing-prototypes -Wno-format-truncation)
target_link_libraries(test_core PRIVATE ipod_core)
add_test(NAME unit_core COMMAND test_core ${TEST_LIB})
set_tests_properties(unit_core PROPERTIES FIXTURES_REQUIRED testlib ENVIRONMENT "HB_LOG=warn")

add_executable(test_player ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_player.c)
target_compile_options(test_player PRIVATE ${APP_WARNINGS} -Wno-missing-prototypes
                       -Wno-format-truncation)
target_link_libraries(test_player PRIVATE ipod_engine hb_audio_headless)
add_test(NAME player_bitexact COMMAND test_player fast ${TEST_LIB})
add_test(NAME player_transport COMMAND test_player realtime ${TEST_LIB})
set_tests_properties(player_bitexact player_transport PROPERTIES
                     FIXTURES_REQUIRED testlib ENVIRONMENT "HB_LOG=warn" TIMEOUT 120)

if(HB_BUILD_SIM)
  add_subdirectory(sim)

  # End-to-end UI tests: scripted button presses against the simulator.
  set(TEST_LIB_BIG ${CMAKE_BINARY_DIR}/testlib_big)
  add_test(NAME make_test_library_big
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_test_library.py
                   ${TEST_LIB_BIG} --seconds 2 --bulk 400)
  set_tests_properties(make_test_library_big PROPERTIES FIXTURES_SETUP testlib_big)
  set(E2E_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e)
  foreach(suite navigation playback settings otg_search extras_hold lifecycle landscape bigscroll)
    if(suite STREQUAL "bigscroll")
      set(card ${TEST_LIB_BIG})
      set(fixture testlib_big)
    else()
      set(card ${TEST_LIB})
      set(fixture testlib)
    endif()
    add_test(NAME e2e_${suite}
             COMMAND ${E2E_DIR}/run_e2e.sh $<TARGET_FILE:hb_sim> ${E2E_DIR}/${suite}.hbs ${card}
                     ${CMAKE_BINARY_DIR}/e2e_shots)
    set_tests_properties(e2e_${suite} PROPERTIES FIXTURES_REQUIRED ${fixture} TIMEOUT 300)
  endforeach()
endif()
