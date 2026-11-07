#!/usr/bin/env bash
# run_menurig.sh — round 11: boots fdk-files (DRIG) under a private
# Xvfb, waits for the DRIG-GEO lines, right-clicks the folder row,
# clicks the context menu's LAST entry (Properties — the rig finds
# the real popup window and measures it), and checks the app's
# PHASE telemetry: the action that runs must be Properties (the
# round-11 bug fired "Delete permanently" — the delete-confirm
# dialog — from the Properties entry).
set -u
cd "$(dirname "$0")"      # test-apps/

XVFB_NUM=$((300 + RANDOM % 400)); XVFB_DISP=":$XVFB_NUM"
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
Xvfb "$XVFB_DISP" -screen 0 1280x800x24 -dpi 96 >/tmp/menurig-xvfb.log 2>&1 &
XP=$!
sleep 1.5

export DISPLAY="$XVFB_DISP"
export ASAN_OPTIONS=detect_leaks=0
: > /tmp/menurig-app.log
FDK_FILES_DRIG=1 ./build/fdk-files > /tmp/menurig-app.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do
    grep -q 'DRIG-GEO bookmarks' /tmp/menurig-app.log && break
    sleep 0.5
done

# the geometry the rig clicks against (pane0's list, beta-dir=row 1,
# 30px rows — the DRIG layout's measured constants)
GEO=$(grep -E 'DRIG-GEO pane0 list' /tmp/menurig-app.log | head -1)
LIST_X=$(echo "$GEO" | awk '{print $4}')
LIST_Y=$(echo "$GEO" | awk '{print $5}')

export MENURIG_LIST_X="$LIST_X" MENURIG_LIST_Y="$LIST_Y"
export MENURIG_ROW=1 MENURIG_ROW_H=30

RC=1
./build/menu-rig
RC=$?

kill $APID $XP 2>/dev/null
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
exit $RC
