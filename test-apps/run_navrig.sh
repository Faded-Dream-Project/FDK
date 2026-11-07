#!/usr/bin/env bash
# run_navrig.sh — round 12: the navigation-storm CPU rig. Boots
# fdk-files (DRIG) under a private Xvfb, populates beta-dir with
# NAVRIG_FILES files (the inotify reload lands), then storms
# double-click-into-folder / Alt+Left-back NAVRIG_LOOPS times while
# sampling the app's CPU from /proc. Env: NAVRIG_FILES (default
# 500), NAVRIG_LOOPS (default 8).
set -u
cd "$(dirname "$0")"      # test-apps/

FILES=${NAVRIG_FILES:-500}
LOOPS=${NAVRIG_LOOPS:-8}

XVFB_NUM=$((300 + RANDOM % 400)); XVFB_DISP=":$XVFB_NUM"
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
Xvfb "$XVFB_DISP" -screen 0 1280x800x24 -dpi 96 >/tmp/navrig-xvfb.log 2>&1 &
XP=$!
sleep 1.5

export DISPLAY="$XVFB_DISP"
export ASAN_OPTIONS=detect_leaks=0
: > /tmp/navrig-app.log
FDK_FILES_DRIG=1 ./build/fdk-files > /tmp/navrig-app.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do
    grep -q 'DRIG-GEO bookmarks' /tmp/navrig-app.log && break
    sleep 0.5
done

# the big destination: populate beta-dir AFTER boot (inotify reloads
# pane0 once, then the tree is stable)
mkdir -p /tmp/fdk-drig/left/beta-dir
for i in $(seq 1 "$FILES"); do
    printf 'x' > "/tmp/fdk-drig/left/beta-dir/f$i.txt"
done

# the geometry the rig clicks against
GEO=$(grep -E 'DRIG-GEO pane0 list' /tmp/navrig-app.log | head -1)
LIST_X=$(echo "$GEO" | awk '{print $4}')
LIST_Y=$(echo "$GEO" | awk '{print $5}')

export NAVRIG_PID="$APID" NAVRIG_LOOPS="$LOOPS" NAVRIG_FILES="$FILES"
export NAVRIG_LIST_X="$LIST_X" NAVRIG_LIST_Y="$LIST_Y"

./build/nav-rig
RC=$?

kill $APID $XP 2>/dev/null
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
exit $RC
