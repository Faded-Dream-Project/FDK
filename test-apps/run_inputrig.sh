#!/usr/bin/env bash
# run_inputrig.sh — round 13: scroll/hover CPU rig. Boots the DRIG
# app under Xvfb, populates beta-dir (RIG_FILES files), then runs
# the input rig in RIG_MODE (scroll|hover) for RIG_SECONDS.
set -u
cd "$(dirname "$0")"
FILES=${RIG_FILES:-1000}
MODE=${RIG_MODE:-scroll}
SECONDS_N=${RIG_SECONDS:-4}

XVFB_NUM=$((300 + RANDOM % 400)); XVFB_DISP=":$XVFB_NUM"
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
Xvfb "$XVFB_DISP" -screen 0 1280x800x24 -dpi 96 >/tmp/inputrig-xvfb.log 2>&1 &
XP=$!
sleep 1.5
export DISPLAY="$XVFB_DISP"
export ASAN_OPTIONS=detect_leaks=0
BIN=${RIG_BIN:-./build/fdk-files}
: > /tmp/inputrig-app.log
FDK_FILES_DRIG=1 "$BIN" > /tmp/inputrig-app.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do grep -q 'DRIG-GEO bookmarks' /tmp/inputrig-app.log && break; sleep 0.5; done
mkdir -p /tmp/fdk-drig/left/beta-dir
for i in $(seq 1 "$FILES"); do printf 'x' > "/tmp/fdk-drig/left/beta-dir/f$i.txt"; done
sleep 1.5
GEO=$(grep -E 'DRIG-GEO pane0 list' /tmp/inputrig-app.log | head -1)
export RIG_PID=$APID RIG_MODE=$MODE RIG_SECONDS=$SECONDS_N
export RIG_LIST_X=$(echo "$GEO" | awk '{print $4}') RIG_LIST_Y=$(echo "$GEO" | awk '{print $5}')
./build/input-rig
RC=$?
kill $APID $XP 2>/dev/null
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
exit $RC
