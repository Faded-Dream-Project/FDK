#!/usr/bin/env bash
# run_blitcheck.sh — round 14: the blit-scroll PIXEL-IDENTITY check.
#
# The strip path moves already-painted rows with a same-surface blit
# and repaints only the exposed strips. If the blit is faithful and
# the strip math exact, a blit-scrolled frame must be PIXEL-IDENTICAL
# to a full repaint at the same scroll position:
#
#   S1 = the settled frame after a real wheel storm (blit path)
#   S2 = the same position, repainted from scratch (a forced full
#        XExpose — XClearArea with exposures=True — drives the app's
#        full-repaint path without touching any widget state)
#
# Any difference = a strip/blit bug (wrong strip rect, smeared blit
# direction, a widget that changed state without damaging — e.g. the
# CLASSIC scrollbar thumb, the exact bug this check caught).
#
# Env: BLIT_FILES (default 2000), BLIT_SECONDS (default 4).
set -u
cd "$(dirname "$0")"
FILES=${BLIT_FILES:-2000}
SECS=${BLIT_SECONDS:-4}
X11=/tmp/x11dev/usr/lib/x86_64-linux-gnu

XVFB_NUM=$((400 + RANDOM % 300)); D=":$XVFB_NUM"
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
Xvfb "$D" -screen 0 1280x800x24 -dpi 96 >/tmp/blitcheck-xvfb.log 2>&1 &
XP=$!
trap 'kill $APID $XP 2>/dev/null; rm -f "/tmp/.X11-unix/X$XVFB_NUM"' EXIT
sleep 1.5
export DISPLAY="$D"

: > /tmp/blitcheck-app.log
FDK_FILES_DRIG=1 FDK_TESTAPP_FRAMES=0 ./build-rel/fdk-files > /tmp/blitcheck-app.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do grep -q 'DRIG-GEO bookmarks' /tmp/blitcheck-app.log && break; sleep 0.5; done
mkdir -p /tmp/fdk-drig/left/beta-dir
for i in $(seq 1 "$FILES"); do printf 'x' > "/tmp/fdk-drig/left/beta-dir/f$i.txt"; done

GEO=$(grep -E 'DRIG-GEO pane0 list' /tmp/blitcheck-app.log | head -1)
export RIG_PID=$APID RIG_MODE=scroll RIG_SECONDS=$SECS
export RIG_LIST_X=$(echo "$GEO" | awk '{print $4}') RIG_LIST_Y=$(echo "$GEO" | awk '{print $5}')
export RIG_LIST_W=$(echo "$GEO" | awk '{print $6}') RIG_LIST_H=$(echo "$GEO" | awk '{print $7}')
# the rig navigates, settles (CPU-quiet poll), then storms wheels
./build/stress-rig > /tmp/blitcheck-rig.log 2>&1 || true

sleep 1.2   # last flight + present
LD_LIBRARY_PATH=$X11 /tmp/xsnap /tmp/blit-s1.ppm
LD_LIBRARY_PATH=$X11 /tmp/xexpose
sleep 1.2   # full repaint
LD_LIBRARY_PATH=$X11 /tmp/xsnap /tmp/blit-s2.ppm
kill $APID 2>/dev/null

python3 - <<'PYEOF'
import sys
def load(p):
    d = open(p, 'rb').read()
    assert d[:2] == b'P6', p
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) >= w * h * 3, (p, len(px), w, h)
    return w, h, px
w1, h1, a = load('/tmp/blit-s1.ppm')
w2, h2, b = load('/tmp/blit-s2.ppm')
assert (w1, h1) == (w2, h2), f"size mismatch {w1}x{h1} vs {w2}x{h2}"
diff = 0
maxd = 0
first = None
for i in range(0, w1 * h1 * 3, 3):
    for c in range(3):
        d = abs(a[i + c] - b[i + c])
        if d:
            diff += 1
            maxd = max(maxd, d)
            if first is None:
                first = ((i // 3) // w1, (i // 3) % w1, c, a[i + c], b[i + c])
print(f"pixels: {w1 * h1}, differing subpixels: {diff}, max channel delta: {maxd}")
if first:
    print(f"first diff at row {first[0]} col {first[1]} ch{first[2]}: {first[3]} vs {first[4]}")
print("BLIT-CHECK " + ("PASS — blit-scrolled frame is pixel-identical to the full repaint"
                       if diff == 0 else "FAIL"))
sys.exit(0 if diff == 0 else 1)
PYEOF
RC=$?
exit $RC
