// Copyright (c) 2026 High Boy iPod app contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// LVGL configuration for the High Boy simulator. It mirrors the device build
// (firmware_p4/sdkconfig.defaults): 16-bit colour, software rendering and
// only the Montserrat 12/14/16 fonts, so a UI that runs here also fits there.

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

#define LV_USE_STDLIB_MALLOC   LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING   LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF  LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE // the app only touches LVGL from the UI thread

#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF         130

#define LV_USE_DRAW_SW            1
#define LV_DRAW_SW_COMPLEX        1
#define LV_DRAW_BUF_STRIDE_ALIGN  1
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (24 * 1024)

#define LV_USE_LOG      1
#define LV_LOG_LEVEL    LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF   1

#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

// Fonts: exactly what CONFIG_LV_FONT_MONTSERRAT_* enables on the device.
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// Same image cache as the device (CONFIG_LV_CACHE_DEF_SIZE=1048576), so cache
// lifetime bugs show up in the simulator too.
#define LV_CACHE_DEF_SIZE (1024 * 1024)

#define LV_USE_LODEPNG 1
#define LV_USE_TJPGD   1

#define LV_USE_SDL      0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS    0

#endif // LV_CONF_H
