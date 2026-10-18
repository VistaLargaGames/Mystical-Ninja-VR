#!/bin/bash
# From Goemon's house (after desk_boot.sh): level select -> the stage the menu starts on (a shop).
cd "$(dirname "$0")"
./press.sh "l z dr" 0.5 3
./press.sh a 0.3 2
./press.sh a 0.3 2
./press.sh a 0.3 5
