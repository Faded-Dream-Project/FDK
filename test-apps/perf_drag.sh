#!/usr/bin/env bash
# perf_drag.sh — round 10: quantify the drag load. Boots fdk-files
# (DRIG) under Xvfb, runs a 6-second XTEST motion flood (a held
# left button with continuous motion — the rubber-band gesture),
# samples the app's /proc stat every 500ms, and prints per-sample
# utime/stime deltas (USER_HZ=100 → each tick = 1% of one core).
#
# Gotcha already paid for: pgrep -f matches the timeout(1) wrapper
# first — its stat reads 0 0 (it just waits). Use pgrep -x.
set -u
cd /home/z/my-project/work/FDK/test-apps
export CPATH="/tmp/my-project/local-x11/include"
export LD_LIBRARY_PATH="/tmp/my-project/local-x11/lib/x86_64-linux-gnu:/tmp/my-project/work/xdev-root/usr/lib/x86_64-linux-gnu"

XVFB_NUM=$((90 + RANDOM % 400)); XVFB_DISP=":$XVFB_NUM"
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
Xvfb "$XVFB_DISP" -screen 0 1280x800x24 -dpi 96 >/tmp/perf-xvfb.log 2>&1 & XP=$!
sleep 1.5
export DISPLAY="$XVFB_DISP" ASAN_OPTIONS=detect_leaks=0

FDK_FILES_DRIG=1 timeout 60 ./build/fdk-files > /tmp/perf-app.log 2>&1 & APID=$!
for _ in $(seq 1 60); do grep -q 'DRIG-GEO bookmarks' /tmp/perf-app.log && break; sleep 0.5; done
# the pgrep must find fdk-files ITSELF — not the timeout(1) wrapper
# whose cmdline also matches -f (its stat reads 0 0: it just waits)
APID2=$(pgrep -x fdk-files | head -1)
echo "app pid: $APID2"

# sampler in background: 13 samples x 500ms (cumulative utime/stime)
(sleep 2; for i in $(seq 1 13); do
  if [ -r "/proc/$APID2/stat" ]; then
    awk '{print $14, $15}' "/proc/$APID2/stat"
  fi
  sleep 0.5
done) > /tmp/perf-samples.txt & SAMP=$!

# the drag: press on empty space in pane0, sweep for ~6s
DISPLAY="$XVFB_DISP" python3 - <<'EOF'
import ctypes, time
X = ctypes.CDLL('libX11.so.6')
T = ctypes.CDLL('libXtst.so.6')
X.XOpenDisplay.restype = ctypes.c_void_p
d = X.XOpenDisplay(None)
def motion(x, y):
    T.XTestFakeMotionEvent(d, 0, x, y, 0); X.XSync(d, 0)
def button(down):
    T.XTestFakeButtonEvent(d, 1, down, 0); X.XSync(d, 0)
time.sleep(2.5)
motion(600, 320)          # empty space below the rows in pane0
button(True)
t0 = time.time()
i = 0
while time.time() - t0 < 6.0:   # 6 seconds of continuous motion
    y = 260 + (i % 12) * 8      # sweep 260..348
    x = 500 + (i % 7) * 10
    motion(x, y)
    time.sleep(0.004)           # ~250 motions/s
    i += 1
button(False)
X.XCloseDisplay(d)
EOF
wait $SAMP
kill $APID $XP 2>/dev/null; rm -f "/tmp/.X11-unix/X$XVFB_NUM"
echo "cumulative (utime%, stime%) per 500ms sample:"
cat /tmp/perf-samples.txt
python3 - <<'EOF'
rows = [tuple(map(int, l.split())) for l in open('/tmp/perf-samples.txt') if l.strip()]
deltas = [(b[0]-a[0], b[1]-a[1]) for a, b in zip(rows, rows[1:])]
steady = deltas[2:]
if steady:
    au = sum(d[0] for d in steady)/len(steady)
    asys = sum(d[1] for d in steady)/len(steady)
    print(f"steady-state per 500ms: utime {au:.1f}% stime {asys:.1f}% = {au+asys:.1f}% of one core")
EOF