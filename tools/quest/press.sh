#!/bin/bash
# Desk test helper (PC build): press.sh "<keys>" [hold seconds] [wait seconds after]
# Writes the keys to out/pctest/press.txt, holds, lets go, waits.
F="$(dirname "$0")/../../out/pctest/press.txt"
echo "$1" > "$F"
sleep "${2:-0.3}"
: > "$F"
sleep "${3:-1}"
