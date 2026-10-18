#!/bin/bash
# shot.sh name: the desk-test window's picture into out/pctest/<name>.png
cd "$(dirname "$0")/../.."
powershell -NoProfile -ExecutionPolicy Bypass -File tools/quest/shotgame.ps1 -Out "out/pctest/$1.png"
