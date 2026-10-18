#!/bin/bash
# Drives a freshly started PC desk-test game from boot into Goemon's house (about 40 s).
cd "$(dirname "$0")"
P=./press.sh
sleep 6
$P start 0.3 3
$P start 0.3 3
$P dd 0.3 1
$P a 0.3 3
for k in start a a start a; do $P $k 0.3 3; done
