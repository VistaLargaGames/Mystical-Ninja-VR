#!/bin/bash
# desk_start.sh "yaw pitch fovy pull" [extra VAR=value ...]: (re)starts the PC desk test
# in out/pctest, in a small always-on-top window in the bottom-right corner that
# never takes the keyboard focus (someone may be working on this PC).
cd "$(dirname "$0")/../../out/pctest"
taskkill //IM Goemon64Recompiled.exe //F > /dev/null 2>&1
sleep 1
: > press.txt
rm -f vrtest.txt trace_on.txt
(env GOEMON_WINDOW="3820 620 1280 768" SDL_WINDOWS_DPI_AWARENESS=unaware GOEMON_LEVEL_SELECT=1 GOEMON_AUTOSTART=1 GOEMON_VR_TEST="${1:-0 -15 110 0.5}" \
    GOEMON_VR_HUD="0.93 0.231 0.295 0.064" GOEMON_PRESS_FILE="C:/Users/QbAnYtO/gq/out/pctest/press.txt" "${@:2}" \
    ../build/pc/Goemon64Recompiled.exe > run.log 2>&1 &)
sleep 2
