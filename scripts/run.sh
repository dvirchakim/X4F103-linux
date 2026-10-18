#!/bin/bash
# Run the replay and dump frames.bin. Usage: scripts/run.sh [cycles] [device]
set -e
CYCLES=${1:-300}
cd "$(dirname "$0")/.."
[ -x ./replay ] || make
REPLAY_TIMING=1 ST_DELAY_US=0 RD_TRIES=40 RD_DELAY_US=2000 ./replay data/seq.txt "$CYCLES"
python3 scripts/analyze.py frames.bin
