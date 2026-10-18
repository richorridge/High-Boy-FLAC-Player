# TentacleOS integration

## Install

```bash
python3 integration/install_tentacleos.py /path/to/TentacleOS
cd /path/to/TentacleOS && ./tools/build.sh && ./tools/flash.sh
```

Add `--max-files N` to raise the SD card's open-file limit (`VFS_MAX_FILES` in
`Service/storage_vfs/include/vfs_config.h`, 5 by default). Each extra slot
costs about 0.6 KB of internal RAM. The app works with the default limit, but
more slots leave more headroom for other TentacleOS services.

The installer is idempotent: re-run it to update the app sources. It was
written against TentacleOS commit `48e1052` (Aug 2026). If an anchor moves
upstream it stops and tells you which file to edit. The manual equivalent is:

1. Copy `app/ipod/` to
   `firmware_p4/components/Applications/ui/screens/ipod/` (the Applications
   component already globs every `ui/**/*.c`).
2. `ui/include/ui_manager.h`: add `SCREEN_IPOD,` before `SCREEN_COUNT`.
3. `ui/ui_manager.c`:
   * `#include "ui_ipod.h"`
   * `screen_close_fn`: `case SCREEN_IPOD: return ui_ipod_stop;`
   * `screen_open_fn`: `case SCREEN_IPOD: return ui_ipod_open;`
   * `ui_screen_shows_chrome`: add `case SCREEN_IPOD:` to the `false` list.
     The global dropdown opens on a 450 ms UP hold, which would fight with
     scrolling.
4. `ui/screens/games/games_menu_ui.c`: add
   `{"iPod", "/assets/icons/music_note.bin", 0x2C6BD6, false, SCREEN_IPOD},`
   to `GAMES[]` (the APPS carousel).
5. `Applications/CMakeLists.txt`: add `"ui/screens/ipod/include"` and
   `"ui/screens/ipod/third_party"` to `INCLUDE_DIRS`, and compile
   `ui/screens/ipod/ipod_codec_impl.c` with `-w`.

The app only uses public TentacleOS APIs: `ui_manager` (screen loading, input
handler, switching), `input_manager` events, `audio_i2s` streaming, the
`battery_service` and `sys_time` services, and the `/sdcard` VFS.

## ESP32-P4 build check

`esp32p4_check/run_check.sh` compiles and links the app for the ESP32-P4 with
ESP-IDF 5.5.3, LVGL 9.4.0 (with the firmware's `CONFIG_LV_*` settings) and the
real TentacleOS headers. Only the TentacleOS function bodies are stubbed. Use
it when you don't want a full firmware build:

```bash
IDF_PATH=~/esp/v5.5.3/esp-idf integration/esp32p4_check/run_check.sh /path/to/TentacleOS
```
