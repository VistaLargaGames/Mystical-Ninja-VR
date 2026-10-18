#!/bin/bash
# goto_select.sh <ups> [downs]: level select from gameplay; the cursor starts on SHOP.
# List: MAIN SIRO DOUCHU MACHI SHOP BOMBER 3DBOSS DAIMAP OPENING GOPENING BOSS3D.OP DIARY GOETEAM GAMEOVER
cd "$(dirname "$0")"
./press.sh "l z dr" 0.5 3
./press.sh a 0.3 2
for i in $(seq 1 ${1:-0}); do ./press.sh du 0.2 0.6; done
for i in $(seq 1 ${2:-0}); do ./press.sh dd 0.2 0.6; done
./shot.sh select_menu
./press.sh a 0.3 2
./press.sh a 0.3 6
