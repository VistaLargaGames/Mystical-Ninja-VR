#!/bin/bash
# vr.sh "yaw pitch fovy pull": change the running desk test's view settings live.
echo "$1" > "$(dirname "$0")/../../out/pctest/vrtest.txt"
sleep "${2:-1.5}"
