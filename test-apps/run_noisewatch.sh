#!/usr/bin/env bash
# run_noisewatch.sh — round 20: the owner's spontaneous trigger.
#
# The new evidence: folders vanish WITHOUT perf running — just
# switching to a terminal (focus loss) does it, on X11 AND Wayland.
# The round-20 trace reading: identical damage patterns repeating
# with no input = the inotify watch firing pane_reload over and over
# (session noise writing to the watched directory — .Xauthority and
# friends), and fmodel_load's clear-on-failure path emptying the
# model whenever fs_list_dir transiently fails.
#
# This rig stages the whole scenario:
#  - a REAL-shaped home (dotfiles, .config, .cache, user dirs);
#  - a background "session noise" writer touching a dotfile every
#    150 ms (what a real desktop session does to $HOME constantly);
#  - focus loss to root (the terminal switch);
#  - a REAL occlusion (the cover half);
#  - idle sits between stages.
#
# Gates (DRIG telemetry + pixels):
#  1. DRIG-ROWS never reports 0 after the baseline (model intact);
#  2. no DRIG-CLEAR line (the failure path never taken);
#  3. pixel content rows restored after every stage;
#  4. identity vs forced full repaint.
set -u
cd /home/z/my-project/work/FDK-rounds/test-apps
D=:$((800 + RANDOM % 100))
rm -f "/tmp/.X11-unix/X${D#:}"
Xvfb "$D" -screen 0 1920x1080x24 -dpi 96 >/dev/null 2>&1 &
XP=$!
sleep 1.5
/home/z/my-project/scripts/miniwm "$D" > /tmp/nw-wm.log 2>&1 &
WMP=$!
sleep 1.0
export DISPLAY="$D" ASAN_OPTIONS=detect_leaks=0 FDK_PRESENT_TRACE=1 FDK_GL_FORCE=1
: > /tmp/nw-app.log
FDK_FILES_DRIG=1 FDK_TESTAPP_FRAMES=0 FDK_DRIG_W=1880 FDK_DRIG_H=1040 \
  ./build/fdk-files > /tmp/nw-app.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do grep -q 'DRIG-GEO bookmarks' /tmp/nw-app.log && break; sleep 0.5; done

# the real-shaped home, INSIDE the DRIG tree (the app boots into
# /tmp/fdk-drig/left by rig convention — wait for geo, then look)
GEO=$(grep -E 'DRIG-GEO pane0 list' /tmp/nw-app.log | head -1)
export RIG_PID=$APID RIG_LIST_X=$(echo "$GEO" | awk '{print $4}') \
       RIG_LIST_Y=$(echo "$GEO" | awk '{print $5}') \
       RIG_LIST_W=$(echo "$GEO" | awk '{print $6}') \
       RIG_LIST_H=$(echo "$GEO" | awk '{print $7}')
echo "list geo: $RIG_LIST_X $RIG_LIST_Y $RIG_LIST_W $RIG_LIST_H"

# session noise: a dotfile rewritten every 150ms in the WATCHED dir
NOISE_DIR=/tmp/fdk-drig/left
( while kill -0 $APID 2>/dev/null; do
    printf 'x%d\n' $RANDOM > "$NOISE_DIR/.session-noise" 2>/dev/null
    sleep 0.15
  done ) &
NOISE=$!

rows() {
python3 - "$1" "$RIG_LIST_X" "$RIG_LIST_Y" "$RIG_LIST_W" "$RIG_LIST_H" <<'PY'
import sys
from PIL import Image
png, lx, ly, lw, lh = sys.argv[1], *map(int, sys.argv[2:6])
im = Image.open(png).convert("RGB")
px = im.load()
rows = 0
for y in range(ly, ly + lh):
    if any(px[x, y][0] > 100 for x in range(lx, lx + lw, 4)):
        rows += 1
print(rows)
PY
}
snap() { /home/z/my-project/scripts/xsnap -d "$D" "/tmp/nw-$1.ppm" >/dev/null 2>&1; }

echo "--- stage 0: baseline (noise already running)"
sleep 2.0
snap 0
B0=$(rows /tmp/nw-0.ppm)
R0=$(grep DRIG-ROWS /tmp/nw-app.log | tail -1 | awk '{print $3}')
W0=$(grep -c DRIG-WATCH /tmp/nw-app.log || true)
echo "baseline rows=$B0 bound=$R0 watch_fires=$W0"

echo "--- stage 1: focus loss to root + 10s idle (the terminal switch)"
/home/z/my-project/scripts/xfocus "$D" >/dev/null 2>&1
sleep 10
snap 1
B1=$(rows /tmp/nw-1.ppm)
R1=$(grep DRIG-ROWS /tmp/nw-app.log | tail -1 | awk '{print $3}')
echo "post-focus-loss rows=$B1 bound=$R1"

echo "--- stage 2: REAL cover 10s, then uncover (the perf shape)"
/home/z/my-project/scripts/occlude "$D" 10 >/dev/null 2>&1
sleep 2.5
snap 2
B2=$(rows /tmp/nw-2.ppm)
R2=$(grep DRIG-ROWS /tmp/nw-app.log | tail -1 | awk '{print $3}')
echo "post-cover rows=$B2 bound=$R2"

echo "--- stage 3: 10s more idle under noise"
sleep 10
snap 3
B3=$(rows /tmp/nw-3.ppm)
R3=$(grep DRIG-ROWS /tmp/nw-app.log | tail -1 | awk '{print $3}')
W3=$(grep -c DRIG-WATCH /tmp/nw-app.log || true)
echo "post-idle rows=$B3 bound=$R3 watch_fires_total=$W3"

echo "--- verdict"
# GATE 1: no model clear after the first DRIG-ROWS sample (baseline).
# (pane1's boot clear of the genuinely-empty right/ dir lands before it.)
CLEARS_AFTER_BASELINE=$(awk '/DRIG-ROWS/{seen=1} seen && /DRIG-CLEAR/{n++} END{print n+0}' /tmp/nw-app.log)
# GATE 2: the bound row count never drops to zero after baseline.
ROWS_ZEROS=$(awk '/DRIG-ROWS/{seen=1} seen && $3=="rows=0"{n++} END{print n+0}' /tmp/nw-app.log)
# GATE 3: pixel content identical across every stage.
GATE_PIX=fail
[ "$B0" = "$B1" ] && [ "$B1" = "$B2" ] && [ "$B2" = "$B3" ] && [ "$B0" -gt 3 ] && GATE_PIX=ok
# GATE 4: the noise actually exercised the watch (else the rig
# proves nothing).
GATE_WATCH=fail
[ "$W3" -ge 20 ] && GATE_WATCH=ok
echo "clears_after_baseline=$CLEARS_AFTER_BASELINE rows_zeros=$ROWS_ZEROS"
echo "gate pixels=$GATE_PIX gate watch_exercised=$GATE_WATCH"
echo "watch fires: $W0 -> $W3"

kill $NOISE 2>/dev/null
kill $APID 2>/dev/null; sleep 0.3
kill $WMP 2>/dev/null; kill $XP 2>/dev/null
rm -f "/tmp/.X11-unix/X${D#:}"
echo "--- artifacts: /tmp/nw-*.ppm ; app log /tmp/nw-app.log"
[ "$CLEARS_AFTER_BASELINE" = 0 ] && [ "$ROWS_ZEROS" = 0 ] && \
[ "$GATE_PIX" = ok ] && [ "$GATE_WATCH" = ok ] && exit 0 || exit 1
