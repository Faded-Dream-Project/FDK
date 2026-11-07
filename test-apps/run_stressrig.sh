#!/usr/bin/env bash
# run_stressrig.sh — round 14: the REAL-RATE stress rig. Boots the
# DRIG app under Xvfb at STRESS_W x STRESS_H (default 1920x1080),
# populates beta-dir (STRESS_FILES), then drives the stress rig in
# STRESS_MODE (scroll|hover|sbdrag) at STRESS_RATE_HZ for
# STRESS_SECONDS. Reports CPU % of one core AND the present count +
# median damaged-area ratio from the app's PRESENT-TRACE lines.
#
# The round-13 lesson this rig exists for: a 60Hz-spaced XTEST rig
# measured single-digit CPU that the owner's real hardware flatly
# contradicted — the input rate was the missing variable.
set -u
cd "$(dirname "$0")"
FILES=${STRESS_FILES:-1000}
MODE=${STRESS_MODE:-scroll}
SECONDS_N=${STRESS_SECONDS:-6}
W=${STRESS_W:-1920}
H=${STRESS_H:-1080}
RATE=${STRESS_RATE_HZ:-1000}
BIN=${STRESS_BIN:-./build-rel/fdk-files}

XVFB_NUM=$((300 + RANDOM % 400)); XVFB_DISP=":$XVFB_NUM"
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
Xvfb "$XVFB_DISP" -screen 0 ${W}x${H}x24 -dpi 96 >/tmp/stressrig-xvfb.log 2>&1 &
XP=$!
sleep 1.5
export DISPLAY="$XVFB_DISP"
export ASAN_OPTIONS=detect_leaks=0
export FDK_PRESENT_TRACE=1
[ -n "${STRESS_NOBLIT:-}" ] && export FDK_SCROLL_NOBLIT=1
: > /tmp/stressrig-app.log
FDK_FILES_DRIG=1 FDK_TESTAPP_FRAMES=0 FDK_DRIG_W=$((W - 40)) FDK_DRIG_H=$((H - 80)) "$BIN" > /tmp/stressrig-app.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do grep -q 'DRIG-GEO bookmarks' /tmp/stressrig-app.log && break; sleep 0.5; done
mkdir -p /tmp/fdk-drig/left/beta-dir
for i in $(seq 1 "$FILES"); do printf 'x' > "/tmp/fdk-drig/left/beta-dir/f$i.txt"; done
sleep 1.5
GEO=$(grep -E 'DRIG-GEO pane0 list' /tmp/stressrig-app.log | head -1)
export RIG_PID=$APID RIG_MODE=$MODE RIG_SECONDS=$SECONDS_N RIG_RATE_HZ=$RATE
export RIG_LIST_X=$(echo "$GEO" | awk '{print $4}') RIG_LIST_Y=$(echo "$GEO" | awk '{print $5}')
export RIG_LIST_W=$(echo "$GEO" | awk '{print $6}') RIG_LIST_H=$(echo "$GEO" | awk '{print $7}')

./build/stress-rig | tee /tmp/stressrig-rig.out
RC=$?
sleep 1.0   # the app's final presents + the settle window land in the log
kill $APID 2>/dev/null
sleep 0.3

echo "--- presents during the storm ---"
# cut the PRESENT-TRACE timeline by the rig's STORM-WINDOW markers
# (monotonic ms — the same clock the trace lines carry)
python3 - <<'PYEOF'
import statistics, re
rig = open('/tmp/stressrig-rig.out').read()
m = re.search(r'STORM-WINDOW (\d+) (\d+)', rig)
start_ms, end_ms = (int(m.group(1)), int(m.group(2))) if m else (0, 2**62)
lines = [l for l in open('/tmp/stressrig-app.log').read().splitlines()
         if l.startswith('PRESENT-TRACE') and start_ms <= int(l.split()[1]) <= end_ms]
if not lines:
    print("PRESENTS: 0 (nothing presented during the storm!)")
else:
    ts = []
    ratios = []
    fulls = 0
    for ln in lines:
        p = ln.split()
        # PRESENT-TRACE <ms> <full|rects> <area> <winarea> <count>
        t = int(p[1]); area = int(p[3]); win = int(p[4])
        if p[2] == 'full':
            fulls += 1
        ts.append(t)
        ratios.append(area / win if win else 0)
    span = (ts[-1] - ts[0]) / 1000.0
    fps = (len(ts) - 1) / span if span > 0 else 0
    print(f"PRESENTS: {len(ts)} over {span:.1f}s => {fps:.1f} fps")
    print(f"damage-area/window-area: median {statistics.median(ratios):.3f} "
          f"mean {statistics.mean(ratios):.3f} max {max(ratios):.3f}")
    print(f"full-window presents: {fulls}/{len(ts)}")
PYEOF
kill $XP 2>/dev/null
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
exit $RC
