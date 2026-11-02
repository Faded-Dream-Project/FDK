#!/usr/bin/env bash
# run_drig.sh — the §27/2b drag-injection rig runner.
# Starts Xvfb + the external XDND sink + fdk-files with FDK_FILES_DRIG=1,
# finds the app window, runs build/drag-rig against it, reports verdicts.
set -u
cd /home/z/my-project/work/FDK/test-apps
# (the sandbox's env script changed names across rounds; the rig
# only needs the X11 extraction prefix on its paths)
export CPATH="/tmp/x11dev/usr/include:/home/z/apt/prefix/usr/include"
export LD_LIBRARY_PATH="/tmp/x11dev/usr/lib/x86_64-linux-gnu:/home/z/apt/prefix/usr/lib/x86_64-linux-gnu"
SINK=/home/z/my-project/scripts/xdnd_sink

XVFB_NUM=$((90 + RANDOM % 400)); XVFB_DISP=":$XVFB_NUM"
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
Xvfb "$XVFB_DISP" -screen 0 1280x800x24 -dpi 96 >/tmp/xdrig.log 2>&1 & XP=$!
sleep 1.5
export DISPLAY="$XVFB_DISP" ASAN_OPTIONS=detect_leaks=0

# the external drop target (D4): a raw-Xlib XDND window at
# (1020,100) 200x200 — outside fdk-files' 1000x700 window
rm -f /tmp/drig-sink.log
DISPLAY="$XVFB_DISP" "$SINK" 1020 100 200 200 > /tmp/drig-sink.log 2>&1 & SPID=$!
for _ in $(seq 1 20); do
    grep -q 'SINK: ready' /tmp/drig-sink.log && break
    sleep 0.25
done
grep -q 'SINK: ready' /tmp/drig-sink.log || echo "WARN: sink not ready yet"

FDK_FILES_DRIG=1 timeout 180 ./build/fdk-files > /tmp/drig-app.log 2>&1 & APID=$!
for _ in $(seq 1 60); do
    grep -q 'DRIG-GEO bookmarks' /tmp/drig-app.log && break
    sleep 0.5
done
if ! grep -q 'DRIG-GEO bookmarks' /tmp/drig-app.log; then
    echo "FAIL: app never reached DRIG-READY"; tail -5 /tmp/drig-app.log
    kill $APID $SPID $XP 2>/dev/null; rm -f "/tmp/.X11-unix/X$XVFB_NUM"
    exit 1
fi

# If the app doesn't log its xid, discover it from the root tree.
WID=""
LOG_WID=$(grep -oE 'xid[=:]([0-9]+|0x[0-9a-f]+)' /tmp/drig-app.log | head -1 | sed 's/xid[=:]//')
if [ -n "$LOG_WID" ]; then
    WID="$LOG_WID"
else
    WID=$(DISPLAY="$XVFB_DISP" python3 -c "
import ctypes
X = ctypes.CDLL('libX11.so.6')
X.XOpenDisplay.restype = ctypes.c_void_p
X.XOpenDisplay.argtypes = [ctypes.c_char_p]
X.XDefaultRootWindow.restype = ctypes.c_ulong
X.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
X.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
    ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
    ctypes.POINTER(ctypes.c_uint)]
d = X.XOpenDisplay(None)
root = X.XDefaultRootWindow(d)
r, p, n = (ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_uint())
kpp = ctypes.POINTER(ctypes.c_ulong)()
X.XQueryTree(d, root, ctypes.byref(r), ctypes.byref(p), ctypes.byref(kpp), ctypes.byref(n))
wids = [kpp[i] for i in range(n.value)]
X.XFree.argtypes = [ctypes.c_void_p]
X.XFree(kpp)
for w in wids: print(w)
" | sort -n | tail -1)
fi
echo "app window id: $WID"

RC=0
DISPLAY="$XVFB_DISP" timeout 120 ./build/drag-rig "$XVFB_DISP" "$WID" /tmp/drig-app.log || RC=$?

kill $APID 2>/dev/null
kill $SPID 2>/dev/null
kill $XP 2>/dev/null
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
exit $RC
