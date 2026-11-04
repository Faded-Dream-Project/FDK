#!/usr/bin/env bash
# run_scrollcheck.sh — round 16: the owner's real-hardware reports,
# reproduced (or not) under Xvfb + XTEST with the DRIG app.
#
# Reports under test:
#  A. "scrolled aggressively -> the entire folder list disappeared;
#     clicking my username directory brings it back"
#  B. "sitting on a different window -> same effect"
#  C. "scrolling back up glitches the text, things overlapping"
#
# Method: aggressive wheel storms (real rate), screenshots after each
# stage, blank-detection (content-row count in the list region) and
# the blitcheck pixel-identity gate (blit-scrolled frame vs forced
# full repaint must be byte-identical) after EVERY stage.
set -u
cd /home/z/my-project/work/FDK/test-apps
D=:$((800 + RANDOM % 100))
rm -f "/tmp/.X11-unix/X${D#:}"
Xvfb "$D" -screen 0 1280x800x24 -dpi 96 >/dev/null 2>&1 &
XP=$!
sleep 1.5
export DISPLAY="$D" ASAN_OPTIONS=detect_leaks=0 FDK_PRESENT_TRACE=1
: > /tmp/sc-app.log
FDK_FILES_DRIG=1 FDK_TESTAPP_FRAMES=0 FDK_DRIG_W=1240 FDK_DRIG_H=720 \
  ./build/fdk-files > /tmp/sc-app.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do grep -q 'DRIG-GEO bookmarks' /tmp/sc-app.log && break; sleep 0.5; done
mkdir -p /tmp/fdk-drig/left/beta-dir
for i in $(seq 1 300); do printf 'x' > "/tmp/fdk-drig/left/beta-dir/f$i.txt"; done
sleep 1.5
GEO=$(grep -E 'DRIG-GEO pane0 list' /tmp/sc-app.log | head -1)
export RIG_PID=$APID RIG_LIST_X=$(echo "$GEO" | awk '{print $4}') \
       RIG_LIST_Y=$(echo "$GEO" | awk '{print $5}') \
       RIG_LIST_W=$(echo "$GEO" | awk '{print $6}') \
       RIG_LIST_H=$(echo "$GEO" | awk '{print $7}')
echo "list geo: $RIG_LIST_X $RIG_LIST_Y $RIG_LIST_W $RIG_LIST_H"

echo "--- navigate into beta-dir (300 files)"
RIG_MODE=scroll RIG_SECONDS=0 ./build/stress-rig >/dev/null 2>&1
sleep 1.0
grep -q 'PHASE: sel 1 beta-dir' /tmp/sc-app.log || { echo "FAIL: nav"; kill $APID $XP; exit 1; }

# blank check: count content rows inside the list region
check() {  # $1 = stage name, $2 = png
python3 - "$2" "$RIG_LIST_X" "$RIG_LIST_Y" "$RIG_LIST_W" "$RIG_LIST_H" <<'PY'
import sys
from PIL import Image
png, lx, ly, lw, lh = sys.argv[1], *map(int, sys.argv[2:6])
im = Image.open(png).convert("RGB")
px = im.load()
rows = 0
for y in range(ly, ly + lh):
    if any(px[x, y][0] > 100 for x in range(lx, lx + lw, 4)):
        rows += 1
print(f"text_rows={rows} {'BLANK!' if rows < 3 else 'ok'}")
PY
}

snap() { python3 /home/z/my-project/scripts/xsnap.py "$D" "/tmp/sc-$1.png" >/dev/null 2>&1; }

identity() {  # $1 = stage: compare current screen vs forced full repaint
    snap "$1-blit"
    /home/z/my-project/scripts/xexpose "$D" >/dev/null 2>&1
    sleep 1.5
    snap "$1-full"
    python3 - "/tmp/sc-$1-blit.png" "/tmp/sc-$1-full.png" <<'PY'
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
}

echo "--- stage 0: baseline"
snap 0; check 0 /tmp/sc-0.png
P0=$(grep -c PRESENT-TRACE /tmp/sc-app.log)

echo "--- stage A: AGGRESSIVE down-storm (4s @ 30 Hz)"
RIG_MODE=scroll RIG_SECONDS=4 RIG_NONAV=1 RIG_RATE_HZ=30 ./build/stress-rig >/dev/null 2>&1
sleep 2.5
snap A; check A /tmp/sc-A.png
PA=$(grep -c PRESENT-TRACE /tmp/sc-app.log); echo "presents: $P0 -> $PA"
identity A

echo "--- stage B: AGGRESSIVE up-storm (4s @ 30 Hz) — the overlap report"
RIG_MODE=scroll RIG_SECONDS=4 RIG_NONAV=1 RIG_RATE_HZ=30 RIG_DIR=up ./build/stress-rig >/dev/null 2>&1
sleep 2.5
snap B; check B /tmp/sc-B.png
identity B

echo "--- stage C: jitter (8 x 0.4s alternating)"
for i in 1 2 3 4; do
    RIG_MODE=scroll RIG_SECONDS=0.4 RIG_NONAV=1 RIG_RATE_HZ=40 ./build/stress-rig >/dev/null 2>&1
    RIG_MODE=scroll RIG_SECONDS=0.4 RIG_NONAV=1 RIG_RATE_HZ=40 RIG_DIR=up ./build/stress-rig >/dev/null 2>&1
done
sleep 2.5
snap C; check C /tmp/sc-C.png
identity C

echo "--- stage D: unfocused sit (unmap 6s, remap) — the other-window report"
python3 - <<'PY'
import ctypes, os, time
X11 = ctypes.CDLL("/tmp/x11dev/usr/lib/x86_64-linux-gnu/libX11.so")
X11.XOpenDisplay.restype = ctypes.c_void_p
X11.XOpenDisplay.argtypes = [ctypes.c_char_p]
d = X11.XOpenDisplay(os.environ["DISPLAY"].encode())
X11.XDefaultRootWindow.restype = ctypes.c_ulong
root = X11.XDefaultRootWindow(d)
kids = (ctypes.c_ulong * 16)()
n = ctypes.c_uint()
X11.XQueryTree.restype = ctypes.c_int
X11.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                           ctypes.POINTER(ctypes.c_ulong),
                           ctypes.POINTER(ctypes.c_ulong),
                           ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                           ctypes.POINTER(ctypes.c_uint)]
kidsp = (ctypes.c_ulong * 16)()
X11.XQueryTree(d, root, None, None, ctypes.byref(ctypes.cast(kidsp, ctypes.POINTER(ctypes.c_ulong))), ctypes.byref(n))
wins = [kidsp[i] for i in range(min(n.value, 16))]
for w in wins:
    X11.XUnmapWindow(d, w)
X11.XFlush(d)
time.sleep(6)
for w in wins:
    X11.XMapWindow(d, w)
X11.XFlush(d)
PY
sleep 2.5
snap D; check D /tmp/sc-D.png
identity D

echo "--- stage E: post-everything nav sanity (the owner's workaround: click home)"
RIG_MODE=scroll RIG_SECONDS=0 ./build/stress-rig >/dev/null 2>&1
sleep 1.0
snap E; check E /tmp/sc-E.png

kill $APID 2>/dev/null; sleep 0.3; kill $XP 2>/dev/null
rm -f "/tmp/.X11-unix/X${D#:}"
echo "--- artifacts: /tmp/sc-*.png ; app log /tmp/sc-app.log"
grep -cE "ERROR|WARN" /tmp/sc-app.log
