#!/usr/bin/env bash
# run_covercheck.sh — round 19: the owner's ACTUAL disappearing trigger,
# reproduced (or not) with a REAL occluder under Xvfb.
#
# The report: "running `sudo perf record -g -p $(pidof fdk-files) --
# sleep 10` makes the folder content disappear" — i.e. cover the
# window (terminal on top) for ~10s, uncover, and the file list is
# gone until some later input forces a repaint.
#
# Round 16's stage D tested XUnmapWindow (wm_hide) — a DIFFERENT
# server path (map regenerates backing) than occlusion. This rig
# drives the real one: an override-redirect occluder stacked above
# the window (the server discards the obscured pixels — backing
# store is off), then unmapped (the server delivers Exposes for
# everything it discarded).
#
# Gates:
#  1. blank check — content rows in the list region after uncover
#     (must equal the baseline's, +-2 for AA jitter);
#  2. pixel identity — the uncovered frame vs a forced full repaint
#     (xexpose) must be byte-identical;
#  3. expose accounting — PRESENT-TRACE lines must INCREASE after
#     uncover (a repaint actually happened with no user input).
set -u
cd /home/z/my-project/work/FDK-rounds/test-apps
D=:$((900 + RANDOM % 100))
rm -f "/tmp/.X11-unix/X${D#:}"
Xvfb "$D" -screen 0 1280x800x24 -dpi 96 >/dev/null 2>&1 &
XP=$!
sleep 1.5
# round 19: a REAL reparenting WM (scripts/miniwm.c) — bare Xvfb
# passed both lanes, but the owner has an EWMH WM and every real WM
# reparents toplevels into frames (different expose geometry,
# synthetic ConfigureNotify traffic). Set FDK_NOWM=1 for the bare
# server shape.
if [ "${FDK_NOWM:-}" != "1" ]; then
  /home/z/my-project/scripts/miniwm "$D" > /tmp/cc${TAG}-wm.log 2>&1 &
  WMP=$!
  sleep 1.0
fi
# FDK_NOGL=1 drops FDK_GL_FORCE (the llvmpipe CPU-lane shape);
# FDK_NOWM=1 drops miniwm (the bare-server shape).
export DISPLAY="$D" ASAN_OPTIONS=detect_leaks=0 FDK_PRESENT_TRACE=1
[ "${FDK_NOGL:-}" != "1" ] && export FDK_GL_FORCE=1
TAG="${FDK_NOWM:+nowm-}${FDK_NOGL:+nogl-}"
: > "/tmp/cc${TAG}-app.log"
FDK_FILES_DRIG=1 FDK_TESTAPP_FRAMES=0 FDK_DRIG_W=1240 FDK_DRIG_H=720 \
  ./build/fdk-files > /tmp/cc${TAG}-app.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do grep -q 'DRIG-GEO bookmarks' /tmp/cc${TAG}-app.log && break; sleep 0.5; done
mkdir -p /tmp/fdk-drig/left/beta-dir
for i in $(seq 1 300); do printf 'x' > "/tmp/fdk-drig/left/beta-dir/f$i.txt"; done
sleep 1.5
GEO=$(grep -E 'DRIG-GEO pane0 list' /tmp/cc${TAG}-app.log | head -1)
export RIG_PID=$APID RIG_LIST_X=$(echo "$GEO" | awk '{print $4}') \
       RIG_LIST_Y=$(echo "$GEO" | awk '{print $5}') \
       RIG_LIST_W=$(echo "$GEO" | awk '{print $6}') \
       RIG_LIST_H=$(echo "$GEO" | awk '{print $7}')
echo "list geo: $RIG_LIST_X $RIG_LIST_Y $RIG_LIST_W $RIG_LIST_H"

echo "--- navigate into beta-dir (300 files)"
RIG_MODE=scroll RIG_SECONDS=0 ./build/stress-rig >/dev/null 2>&1
sleep 1.0
grep -q 'PHASE: sel 1 beta-dir' /tmp/cc${TAG}-app.log || { echo "FAIL: nav"; kill $APID $XP; exit 1; }

rows() {  # $1 = ppm -> count content rows in the list region
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

snap() { /home/z/my-project/scripts/xsnap -d "$D" "/tmp/cc${TAG}-$1.ppm" >/dev/null 2>&1; }

echo "--- stage 0: baseline"
snap 0
B0=$(rows /tmp/cc${TAG}-0.ppm)
P0=$(grep -c PRESENT-TRACE /tmp/cc${TAG}-app.log)
echo "baseline rows=$B0 presents=$P0"

echo "--- stage 1: REAL cover 10s (the perf-record shape), then uncover"
/home/z/my-project/scripts/occlude "$D" 10 >/tmp/cc${TAG}-occ.log 2>&1
sleep 2.5
snap 1
B1=$(rows /tmp/cc${TAG}-1.ppm)
P1=$(grep -c PRESENT-TRACE /tmp/cc${TAG}-app.log)
echo "uncovered rows=$B1 presents=$P0->$P1"
GATE1=fail
[ "$B1" -ge $((B0 - 2)) ] && [ "$B1" -gt 3 ] && GATE1=ok
GATE3=fail
[ "$P1" -gt "$P0" ] && GATE3=ok
echo "gate blank=$GATE1 gate repainted=$GATE3"

echo "--- stage 2: identity vs forced full repaint"
snap 2-blit
/home/z/my-project/scripts/xexpose "$D" >/dev/null 2>&1
sleep 1.5
snap 2-full
python3 - /tmp/cc-2-blit.ppm /tmp/cc-2-full.ppm <<'PY'
import sys
from PIL import Image, ImageChops
b = Image.open(sys.argv[1]).convert('RGB')
r = Image.open(sys.argv[2]).convert('RGB')
d = ImageChops.difference(b, r)
bbox = d.getbbox()
if bbox is None:
    print("identity: PASS (byte-identical)")
else:
    n = sum(1 for px in d.getdata() if px != (0, 0, 0))
    print(f"identity: FAIL {n} px differ, bbox {bbox}")
PY

echo "--- stage 3: nav sanity after everything (the owner's workaround)"
RIG_MODE=scroll RIG_SECONDS=0 RIG_DIR=up ./build/stress-rig >/dev/null 2>&1
sleep 1.0
snap 3
B3=$(rows /tmp/cc${TAG}-3.ppm)
echo "post-nav rows=$B3 (baseline was $B0)"

kill $APID 2>/dev/null; sleep 0.3
[ -n "${WMP:-}" ] && kill $WMP 2>/dev/null
kill $XP 2>/dev/null
rm -f "/tmp/.X11-unix/X${D#:}"
WARN_N=$(grep -cE "ERROR|WARN" /tmp/cc${TAG}-app.log || true)
echo "app warnings: $WARN_N"
echo "--- artifacts: /tmp/cc-*.ppm ; app log /tmp/cc${TAG}-app.log ; occluder log /tmp/cc${TAG}-occ.log"
[ "$GATE1" = ok ] && [ "$GATE3" = ok ] && exit 0 || exit 1
