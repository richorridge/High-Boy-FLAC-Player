#!/usr/bin/env python3
# Copyright (c) 2026 High Boy FLAC iPod contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Install the iPod app into a TentacleOS checkout (the High Boy firmware).

    install_tentacleos.py /path/to/TentacleOS

What it does (idempotent; re-running updates the copied sources):
  1. copies app/ipod/ to firmware_p4/components/Applications/ui/screens/ipod/
  2. ui_manager.h   adds SCREEN_IPOD to screen_id_t
  3. ui_manager.c   routes SCREEN_IPOD to ui_ipod_open / ui_ipod_stop and marks
                    it chrome-free (holding UP must scroll, not open the
                    quick-settings dropdown)
  4. games_menu_ui.c adds "iPod" to the APPS carousel
  5. Applications/CMakeLists.txt adds the include dirs and silences warnings
                    in the vendored single-header codecs

Then build and flash as usual: ./tools/build.sh && ./tools/flash.sh
Put music in /Music on the microSD card (any folder layout).
"""

import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP_SRC = os.path.normpath(os.path.join(HERE, "..", "app", "ipod"))
APPS = os.path.join("firmware_p4", "components", "Applications")
DEST = os.path.join(APPS, "ui", "screens", "ipod")
MARK = "iPod app"


def edit(path, anchor, insert, *, before=False, once_marker=None):
    """Insert text after (or before) an anchor, unless already present."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if (once_marker or insert.strip()) in text:
        return False
    idx = text.find(anchor)
    if idx < 0:
        sys.exit(f"error: anchor not found in {path}:\n{anchor}\n"
                 "TentacleOS changed; apply the edit by hand (see integration/README.md).")
    pos = idx if before else idx + len(anchor)
    text = text[:pos] + insert + text[pos:]
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tentacleos")
    root = os.path.abspath(ap.parse_args().tentacleos)
    if not os.path.isdir(os.path.join(root, APPS)):
        sys.exit(f"error: {root} does not look like a TentacleOS checkout")

    # 1. sources
    dest = os.path.join(root, DEST)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    shutil.copytree(APP_SRC, dest)
    print(f"copied app sources -> {DEST}")

    ui = os.path.join(root, APPS, "ui")
    changed = []

    # 2. screen id
    if edit(os.path.join(ui, "include", "ui_manager.h"),
            "  SCREEN_COUNT\n", f"  SCREEN_IPOD, // {MARK}\n", before=True,
            once_marker="SCREEN_IPOD"):
        changed.append("ui_manager.h")

    # 3. routing, close hook, chrome
    mgr = os.path.join(ui, "ui_manager.c")
    edit(mgr, '#include "mp3_player_ui.h"\n', '#include "ui_ipod.h" // ' + MARK + "\n",
         once_marker='#include "ui_ipod.h"')
    edit(mgr, "    case SCREEN_MP3_PLAYER:\n      return ui_mp3_player_stop;\n",
         "    case SCREEN_IPOD:\n      return ui_ipod_stop;\n",
         once_marker="return ui_ipod_stop;")
    edit(mgr, "    case SCREEN_MP3_PLAYER:\n      return ui_mp3_player_open;\n",
         "    case SCREEN_IPOD:\n      return ui_ipod_open;\n",
         once_marker="return ui_ipod_open;")
    edit(mgr, "    case SCREEN_MP3_PLAYER:\n    case SCREEN_IMAGE_VIEWER:\n",
         "    case SCREEN_IPOD:\n", before=True,
         once_marker="    case SCREEN_IPOD:\n    case SCREEN_MP3_PLAYER:\n    case SCREEN_IMAGE_VIEWER:")
    changed.append("ui_manager.c")

    # 4. APPS carousel entry
    if edit(os.path.join(ui, "screens", "games", "games_menu_ui.c"),
            '    {"Game Boy", "/assets/icons/game_gb.bin", 0x9BBC0F, true, SCREEN_GAME_GB},\n',
            '    {"iPod", "/assets/icons/music_note.bin", 0x2C6BD6, false, SCREEN_IPOD},\n',
            once_marker="SCREEN_IPOD"):
        changed.append("games_menu_ui.c")

    # 5. build
    cmake = os.path.join(root, APPS, "CMakeLists.txt")
    edit(cmake, '  "ui/screens/audio/include"\n',
         '  "ui/screens/ipod/include" # ' + MARK + '\n  "ui/screens/ipod/third_party"\n',
         once_marker='"ui/screens/ipod/include"')
    edit(cmake, "target_link_libraries(${COMPONENT_LIB} -Wl,-zmuldefs)\n",
         "# " + MARK + ": vendored dr_flac / stb_image keep their own warnings.\n"
         "set_source_files_properties(\"${CMAKE_CURRENT_SOURCE_DIR}/ui/screens/ipod/ipod_codec_impl.c\"\n"
         "                            PROPERTIES COMPILE_OPTIONS \"-w\")\n",
         once_marker="ipod_codec_impl.c")
    changed.append("Applications/CMakeLists.txt")

    print("patched: " + ", ".join(changed))
    print("next: ./tools/build.sh && ./tools/flash.sh  (music goes in /Music on the microSD card)")


if __name__ == "__main__":
    main()
