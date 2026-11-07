#!/usr/bin/env bash
# run_deleterig.sh — round 15: drive the DELETE shapes under a
# sanitizer build and report. One Xvfb + one app per mode (a crash
# in one mode must not poison the others).
#
# Env: DEL_MODES (default "del delkey delmulti dellast delscroll delmenu")
#      DEL_FILES (default 30)
set -u
cd "$(dirname "$0")"
MODES=${DEL_MODES:-"del delkey delmulti dellast delscroll delmenu"}
FILES=${DEL_FILES:-30}
X11=/tmp/x11dev/usr/lib/x86_64-linux-gnu

FAIL=0
for MODE in $MODES; do
    D=":$((500 + RANDOM % 300))"
    rm -f "/tmp/.X11-unix/X${D#:}" 2>/dev/null
    Xvfb "$D" -screen 0 1280x800x24 -dpi 96 >/dev/null 2>&1 &
    XP=$!
    sleep 1.5
    export DISPLAY="$D"

    : > /tmp/delrig-app-$MODE.log
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    FDK_FILES_DRIG=1 FDK_TESTAPP_FRAMES=0 ./build/fdk-files \
        > /tmp/delrig-app-$MODE.log 2>&1 &
    APID=$!

    # wait for the DRIG geometry line
    for _ in $(seq 1 60); do
        grep -q 'DRIG-GEO bookmarks' /tmp/delrig-app-$MODE.log && break
        sleep 0.5
    done
    if ! grep -q 'DRIG-GEO bookmarks' /tmp/delrig-app-$MODE.log; then
        echo "[$MODE] APP NEVER BOOTED"
        kill $APID $XP 2>/dev/null
        FAIL=1
        continue
    fi

    # the fixture: created AFTER boot — the DRIG boot wipes and
    # rebuilds /tmp/fdk-drig itself (alpha.txt + empty beta-dir),
    # so anything created before boot is gone by the time the list
    # populates. (Found live: the rig "deleted" in an EMPTY
    # beta-dir, every shape vacuously green.)
    for i in $(seq 1 "$FILES"); do
        printf 'x' > "/tmp/fdk-drig/left/beta-dir/f$i.txt"
    done
    # deldir's target: a nested directory tree (the recursive
    # async-engine delete needs a real tree to walk)
    mkdir -p /tmp/fdk-drig/left/beta-dir/000-sub1/sub2
    for i in $(seq 1 50); do
        printf 'x' > "/tmp/fdk-drig/left/beta-dir/000-sub1/g$i.txt"
    done

    # navigate into beta-dir: double-click row 1 (beta-dir) with a
    # VERIFIED retry loop - the activation needs the app's
    # double-click window (fdk__window_is_double_click), so a
    # too-fast pair selects but never opens, and the rig would then
    # delete in the WRONG directory (the boot dir) and prove
    # nothing. The PHASE: nav telemetry confirms arrival.
    GEO=$(grep -E 'DRIG-GEO pane0 list' /tmp/delrig-app-$MODE.log | head -1)
    LX=$(echo "$GEO" | awk '{print $4}'); LY=$(echo "$GEO" | awk '{print $5}')
    LW=$(echo "$GEO" | awk '{print $6}'); LH=$(echo "$GEO" | awk '{print $7}')
    ROWH=$(echo "$GEO" | awk '{print $9}' | cut -d= -f2)
    [ -z "$ROWH" ] && ROWH=30
    if [ -z "$LW" ]; then
        echo "[$MODE] no pane0 list GEO"
        kill $APID $XP 2>/dev/null; FAIL=1; continue
    fi
    # Navigate by TYPING the path: click pane0's list (window focus),
    # Ctrl+L (the location bar opens, pre-filled + selected, focused),
    # type the absolute path, Enter. Row-order independent: a
    # remembered sort can put alpha.txt at row 0 and a double-click
    # would OPEN the file instead of navigating into the directory.
    PGEO=$(grep -E 'DRIG-GEO path' /tmp/delrig-app-$MODE.log | head -1)
    NAVOK=0
    for ATTEMPT in 1 2 3; do
        DISPLAY="$D" python3 - "$LX" "$LY" <<'NAVNAV'
import ctypes, time, sys
lx, ly = int(sys.argv[1]), int(sys.argv[2])
xt = ctypes.CDLL("/tmp/x11dev/usr/lib/x86_64-linux-gnu/libXtst.so")
x11 = ctypes.CDLL("/tmp/x11dev/usr/lib/x86_64-linux-gnu/libX11.so")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XStringToKeysym.restype = ctypes.c_ulong
x11.XKeysymToKeycode.restype = ctypes.c_uint
dpy = x11.XOpenDisplay(None)
def motion(x, y):
    xt.XTestFakeMotionEvent(dpy, 0, x, y, 0); x11.XFlush(dpy)
def click():
    xt.XTestFakeButtonEvent(dpy, 1, 1, 0); x11.XFlush(dpy); time.sleep(0.06)
    xt.XTestFakeButtonEvent(dpy, 1, 0, 0); x11.XFlush(dpy)
def keycode(sym):
    return x11.XKeysymToKeycode(dpy, sym)
def tap(kc, hold=0.035):
    xt.XTestFakeKeyEvent(dpy, kc, 1, 0); time.sleep(hold)
    xt.XTestFakeKeyEvent(dpy, kc, 0, 0); x11.XFlush(dpy)
def type_char(ch):
    # X11 keysym for printable ASCII; lowercase letters use their
    # own keysym (no shift), uppercase/symbols use the upper slot.
    if ch.islower():
        sym = ord(ch)
    elif ch.isalpha():
        sym = ord(ch)
    elif ch.isdigit() or ch in "/.-_ ":
        sym = ord(ch)
    else:
        sym = ord(ch)
    kc = keycode(sym)
    if kc == 0 and ch.isalpha():
        kc = keycode(ord(ch.upper()))
        if ch.isupper():
            xt.XTestFakeKeyEvent(dpy, 50, 1, 0)  # LShift
            tap(kc)
            xt.XTestFakeKeyEvent(dpy, 50, 0, 0); x11.XFlush(dpy)
            return
    tap(kc)

# 1) window focus: click inside pane0's list area
motion(lx + 40, ly + 40); time.sleep(0.25)
click(); time.sleep(0.3)
# 2) Ctrl+L: open the location bar (entry focused, text selected)
xt.XTestFakeKeyEvent(dpy, 37, 1, 0); time.sleep(0.04)
tap(keycode(ord('l')))
xt.XTestFakeKeyEvent(dpy, 37, 0, 0); x11.XFlush(dpy)
time.sleep(0.35)
# 3) type the path (entry is pre-selected; first char replaces)
for ch in "/tmp/fdk-drig/left/beta-dir":
    type_char(ch)
time.sleep(0.15)
# 4) Enter
tap(keycode(0xff0d), hold=0.05)
time.sleep(1.0)
NAVNAV
        if grep -q "PHASE: nav /tmp/fdk-drig/left/beta-dir" \
            /tmp/delrig-app-$MODE.log; then
            NAVOK=1; break
        fi
        echo "[$MODE] nav attempt $ATTEMPT failed; retrying"
    done
    # settle: wait for the listing to go quiet
    for _ in $(seq 1 30); do
        U1=$(awk '{print $14}' /proc/$APID/stat 2>/dev/null)
        sleep 0.3
        U2=$(awk '{print $14}' /proc/$APID/stat 2>/dev/null)
        [ -n "$U1" ] && [ -n "$U2" ] && [ "$U2" = "$U1" ] && break
    done

    # rows before
    ROWS_BEFORE=$(grep -c 'PHASE: sel' /tmp/delrig-app-$MODE.log 2>/dev/null || echo 0)

    # run the delete shape
    RIG_PID=$APID RIG_MODE=$MODE RIG_ROW_H=$ROWH \
        RIG_LIST_X=$LX RIG_LIST_Y=$LY RIG_LIST_W=$LW RIG_LIST_H=$LH \
        ./build/delete-rig 2>&1 | sed "s/^/[$MODE] /"

    sleep 1.5
    if kill -0 $APID 2>/dev/null; then
        echo "[$MODE] alive at teardown"
        kill $APID 2>/dev/null
    else
        echo "[$MODE] *** APP EXITED (crash or quit) ***"
        FAIL=1
    fi
    kill $XP 2>/dev/null
    rm -f "/tmp/.X11-unix/X${D#:}"

    # sanitizer report?
    if grep -qE "ERROR: AddressSanitizer|runtime error|SEGV|ABORTING" \
        /tmp/delrig-app-$MODE.log; then
        echo "[$MODE] *** SANITIZER HIT ***"
        grep -A12 -E "ERROR: AddressSanitizer|runtime error" \
            /tmp/delrig-app-$MODE.log | head -18 | sed "s/^/[$MODE] /"
        FAIL=1
    fi
    # stale rows: after delete the pane reloads; row telemetry via sel
    echo "[$MODE] log tail:"
    tail -4 /tmp/delrig-app-$MODE.log | sed "s/^/[$MODE]   /"
done
echo "DELETE-RIG overall: $([ $FAIL = 0 ] && echo ALL-ALIVE || echo FAILURES)"
exit $FAIL
