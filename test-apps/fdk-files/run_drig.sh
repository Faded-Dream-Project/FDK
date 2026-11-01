#!/usr/bin/env bash
# run_drig.sh — the §27/2b drag-injection rig runner.
# Starts Xvfb + fdk-files with FDK_FILES_DRIG=1, finds the app window,
# runs build/drag-rig against it, reports the verdicts.
set -u
cd /home/z/my-project/work/FDK/test-apps
source /home/z/my-project/scripts/s25-env.sh
export CPATH="/tmp/x11dev/usr/include:/home/z/apt/prefix/usr/include"

XVFB_NUM=$((90 + RANDOM % 400)); XVFB_DISP=":$XVFB_NUM"
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
Xvfb "$XVFB_DISP" -screen 0 1280x800x24 -dpi 96 >/tmp/xdrig.log 2>&1 & XP=$!
sleep 1.5
export DISPLAY="$XVFB_DISP" ASAN_OPTIONS=detect_leaks=0

FDK_FILES_DRIG=1 timeout 180 ./build/fdk-files > /tmp/drig-app.log 2>&1 & APID=$!
for _ in $(seq 1 60); do
    grep -q 'DRIG-READY' /tmp/drig-app.log && break
    sleep 0.5
done
if ! grep -q 'DRIG-READY' /tmp/drig-app.log; then
    echo "FAIL: app never reached DRIG-READY"; tail -5 /tmp/drig-app.log
    kill $APID $XP 2>/dev/null; rm -f "/tmp/.X11-unix/X$XVFB_NUM"
    exit 1
fi

# find the window id: FDK logs it? Use a tiny Xlib scan for the app's
# window via xprop-free python:
WID=$(python3 - <<PYEOF
import subprocess, re
# the app is the only client; use xwininfo if present, else parse
# the X server's client list via xprop, else fall back to scanning
# the root tree with xdotool-less XQueryTree through python-xlib...
# simplest reliable: the app logs its xid? grep it:
log = open('/tmp/drig-app.log').read()
m = re.search(r'xid[=:]([0-9]+|0x[0-9a-f]+)', log)
if m:
    print(m.group(1)); raise SystemExit
# else: query the root tree via the drag-rig's own helper mode
import ctypes
PYEOF
)
# If the app doesn't log its xid, discover it with a helper:
if [ -z "${WID:-}" ]; then
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
r, p, kids, n = (ctypes.c_ulong(), ctypes.c_ulong(), None, ctypes.c_uint())
kpp = ctypes.POINTER(ctypes.c_ulong)()
X.XQueryTree(d, root, ctypes.byref(r), ctypes.byref(p), ctypes.byref(kpp), ctypes.byref(n))
wids = [kpp[i] for i in range(n.value)]
X.XFree.argtypes = [ctypes.c_void_p]
X.XFree(kpp)
for w in wids: print(w)
" | tail -1)
fi
echo "app window id: $WID"

RC=0
DISPLAY="$XVFB_DISP" timeout 120 ./build/drag-rig "$XVFB_DISP" "$WID" || RC=$?

kill $APID 2>/dev/null
kill $XP 2>/dev/null
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
exit $RC
