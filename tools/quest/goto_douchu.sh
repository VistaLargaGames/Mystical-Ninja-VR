#!/bin/bash
# From Goemon's house (after desk_boot.sh): level select -> DOUCHU (the road) stage.
cd "$(dirname "$0")"
./press.sh "l z dr" 0.5 3
./press.sh a 0.3 2
./press.sh du 0.2 1
./press.sh du 0.2 1
./press.sh a 0.3 2
./press.sh a 0.3 5
