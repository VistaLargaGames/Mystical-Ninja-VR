#!/bin/bash
# Goemon on the unworn headset (owner OK'd, 2026-10-17): starts the stored ROM without
# the ROM window, presses through the title into the save, swings the head yaw
# (debug.goemon.fakeyaw) and logs the probe (picture shift vs. the shift the head turn
# predicts). Needs our turn on the shared headset lock. Resets every switch afterwards.
#   bash tools/quest/quest_probe_test.sh <out dir> [fakeyaw degrees] [probe pictures]
set -u
OUT=${1:?out dir}; FAKE=${2:-20}; PROBE=${3:-60}
ADB=/c/Users/QbAnYtO/AppData/Local/Android/Sdk/platform-tools/adb.exe
PKG=com.goemon64.recompiled
LOCK=$HOME/.claude/skills/talking-to-other-sessions/scripts/quest_lock.sh
SERIAL=${SERIAL:-$("$ADB" devices | awk 'NR>1 && $2=="device" && $1 !~ /:/ {print $1; exit}')}
SERIAL=${SERIAL:-$("$ADB" devices | awk 'NR>1 && $2=="device" {print $1; exit}')}
A() { MSYS_NO_PATHCONV=1 "$ADB" -s "$SERIAL" "$@"; }
if ! bash "$LOCK" check goemon; then
    echo "No headset turn for goemon"; bash "$LOCK" status; exit 1
fi
mkdir -p "$OUT"
press() { A shell setprop debug.goemon.press "$1"; sleep "${2:-0.3}"; A shell setprop debug.goemon.press 0; sleep "${3:-1}"; }
A shell am force-stop $PKG
DEVNOW=$(A shell date +%s | tr -d '\r')
A shell setprop debug.goemon.testuntil $(( ${DEVNOW:-0} + 360 ))
A shell setprop debug.goemon.autoplay 1
A shell setprop debug.goemon.press 0
A shell setprop debug.oculus.guardian_pause 1
A shell am broadcast -a com.oculus.vrpowermanager.prox_close > /dev/null
A shell input keyevent KEYCODE_WAKEUP
A logcat -c
MARK="GOEMONTEST$$:S"
A logcat -v time "*:V" "$MARK" > "$OUT/logcat.txt" 2>&1 < /dev/null &
LOGPID=$!
A shell am start -n $PKG/org.libsdl.app.SDLActivity > /dev/null
sleep 14
# Title -> file select -> the save (same order as the PC desk_boot.sh).
press start 0.3 3
press start 0.3 3
press a 0.3 3
for k in a a start a; do press $k 0.3 3; done
sleep 4
# Phase 1: standing still, the head swings, the probe compares picture shifts with the turn.
A shell setprop debug.goemon.fakeyaw "$FAKE"
sleep 3
A shell setprop debug.goemon.probe "$PROBE"
sleep 20
A shell setprop debug.goemon.probe 0
A shell setprop debug.goemon.fakeyaw 0
sleep 2
# Phase 2: walking around (the world lock's cuts and follow mode get logged).
press up 2 0.3
press left 1.5 0.3
press down 2 0.3
press right 1.5 0.3
press up 1 1
A shell am force-stop $PKG
kill $LOGPID 2>/dev/null; ps -ef | grep -F "$MARK" | grep -v grep | awk '{print $2}' | xargs -r kill 2>/dev/null
for p in autoplay testuntil press probe fakeyaw; do A shell setprop debug.goemon.$p 0; done
A shell setprop debug.oculus.guardian_pause 0
A shell am broadcast -a com.oculus.vrpowermanager.automation_disable > /dev/null
grep -E " I/OpenXR" "$OUT/logcat.txt" | grep -E "probe:|camera aimed|sky dome:|level camera|fake|world lock" > "$OUT/probe.txt"
echo "done: $(grep -c 'probe: picture' "$OUT/probe.txt") probe lines"
