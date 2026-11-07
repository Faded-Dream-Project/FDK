#!/usr/bin/env bash
# run_clickrig.sh — round 10: boots fdk-files (DRIG) under a private
# Xvfb, waits for the DRIG-GEO lines, runs the click sweep, prints
# the y -> selection map. The PASS judgment is the reader's (the
# rig prints; the geometry lines let you check every row boundary).
# Env: CLICKRIG_FROM/TO/STEP/X pass through to the rig (window
# coordinates; the app window is at 0,0 under this Xvfb).
set -u
cd "$(dirname "$0")"      # test-apps/ (the rigs + build/ live here)

XVFB_NUM=$((300 + RANDOM % 400)); XVFB_DISP=":$XVFB_NUM"
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
Xvfb "$XVFB_DISP" -screen 0 1280x800x24 -dpi 96 >/tmp/clickrig-xvfb.log 2>&1 &
XP=$!
sleep 1.5

export DISPLAY="$XVFB_DISP"
export ASAN_OPTIONS=detect_leaks=0
: > /tmp/clickrig-app.log
FDK_FILES_DRIG=1 ./build/fdk-files > /tmp/clickrig-app.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do
    grep -q 'DRIG-GEO bookmarks' /tmp/clickrig-app.log && break
    sleep 0.5
done

# report the geometry the sweep is validated against
grep -E 'app window id|DRIG-GEO' /tmp/clickrig-app.log | head -4

./build/click-rig

kill $APID $XP 2>/dev/null
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
