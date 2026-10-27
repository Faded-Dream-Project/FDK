#!/usr/bin/env bash
# trayhost_rig.sh — the StatusNotifier tray host's full integration rig
# (S-1 / A-11, 1.1.0).
#
# What this rig pins (each leg prints PASS/FAIL; any FAIL exits 1):
#
#  PROTOCOL legs (a real dbus-daemon via dbus-run-session + bare Xvfb;
#  the CALLS go through dbus-send = libdbus and gdbus = GLib/GIO, two
#  reference implementations parsing trayhost's served bytes — the
#  cross-implementation proof; the SIGNALS are received by
#  `gdbus monitor`, a third party's parser; the ITEM side is the
#  probe — a second independent FDK-D-Bus client):
#   P1  Properties.GetAll: all four watcher properties, right types.
#   P2  Properties.Get per property (boolean/int32/string/array).
#   P3  the probe registers (KDE dialect): StatusNotifierItemRegistered
#       + PropertiesChanged observed by the monitor; the items array
#       carries the registration; the item's properties fetched; the
#       menu prefetched (the FIRST right-click works).
#   P4  hostile registrations: empty, garbage, overlong, wrong-typed,
#       and a bogus HOST registration — each answered with
#       InvalidArgs; the host alive after.
#   P5  the Ayatana dialect (object-path registration) — same round.
#   P6  the probe exits: NameOwnerChanged drops the item; the
#       Unregistered signal observed; the array empty again.
#   P7  Introspect (the XML tells the truth), Peer.Ping, the
#       read-only Set refusal, the unknown-Get and UnknownMethod
#       answers.
#   P8  name etiquette: a second host declines politely (one line,
#       exit 0); --replace takes over (the first exits 0 on NameLost);
#       against a stub that REFUSES replacement, --replace exits with
#       the one-line refusal.
#
#  X11 legs (Xvfb + openbox when present — the EWMH path; XTEST fake
#  input; PIL pixel readbacks of the X server's own framebuffer):
#   X1  the bar is on screen top-right; the blue 32x32 icon (the
#       best-size pick from the dual-resolution a(iiay)) is in its
#       cell.
#   X2  a left XTEST click: Activate with the click's SCREEN
#       coordinates (the probe logs them).
#   X3  a right XTEST click: the dbusmenu opens (menu-background
#       pixels where there was desktop), the mnemonic-stripped label
#       parsed (the trace's "label 'First item'"), the tree parsed
#       whole (10 nodes), the AboutToShow handshake logged.
#   X4  a click on row one: Event id=1 clicked (the probe logs it);
#       the menu is gone (re-readback).
#   X5  the cascade: hover the submenu row — the child popup FLIPS
#       LEFT (the screen's right edge is close; pixel-proved), its
#       rows present; the parent got Event(7, opened).
#   X6  a click on the nested row: Event id=8 clicked.
#   X7  Passive: the icon fades (alpha 0.45 — pixel-compared), never
#       hides; NeedsAttention: the attention pixmap + the accent
#       strip appear (pixels).
#   X8  the middle click: SecondaryActivate with screen coordinates.
#   X9  FlipIcon (NewIcon -> the GetAll refetch): the icon turns
#       green (pixels); Retitle (NewTitle) refetches.
#   X-last: exit code 0, no sanitizer findings.
#
#  WAYLAND legs (headless sway — the layer-shell path):
#   W0  the host runs under a real compositor and takes the name.
#   W1  the probe registers; a zwlr_layer_surface_v1.configure is in
#       the WAYLAND_DEBUG trace, no protocol error, no attach before
#       the first ack_configure.
#   W2  grim proves the bar's pixels (and the icon's) on the output.
#   W-last: exit code 0, no sanitizer findings.
#
# Every leg runs the host SANITIZED (ASan/UBSan) with the LSan
# suppressions file (third-party GL/fontconfig process-lifetime
# state — tests/data/lsan-gl.supp); a leak or a crash fails the leg,
# not just the exit code.
#
# Requires: dbus-run-session, dbus-send, gdbus, Xvfb, python3-PIL;
# optional (legs self-skip): openbox, sway, grim, the XTEST dev
# files. TRAY_RIG_REQUIRE=1 makes every skip fatal (CI sets it).
set -u

# ---- arguments and environment --------------------------------------------

SHELLDIR=$(cd "$(dirname "$0")/.." && pwd)
FDKROOT=$(cd "$SHELLDIR/.." && pwd)
for p in /home/z/apt/prefix/usr/bin; do
    [ -d "$p" ] && case ":$PATH:" in *":$p:"*) ;; *)
        PATH="$PATH:$p" ;; esac
done
export PATH
TRAYHOST="$SHELLDIR/build/examples/trayhost"
PROBE="$SHELLDIR/build/tests/trayhost_probe"
SUPP="$FDKROOT/tests/data/lsan-gl.supp"
LIFE=${1:-8}
REQUIRE=${TRAY_RIG_REQUIRE:-0}
PASS=1

note() { echo "   $*"; }
check() { # check <name> <ok:0|1>
    if [ "$2" = "0" ]; then echo "   $1: PASS"; else echo "   $1: FAIL"; PASS=0; fi
}
have() { command -v "$1" >/dev/null 2>&1; }

# ---- shared helpers ---------------------------------------------------------

WBUS=org.kde.StatusNotifierWatcher

wait_for_name() {
    local i
    for i in $(seq 1 100); do
        if dbus-send --session --print-reply --dest=org.freedesktop.DBus / \
            org.freedesktop.DBus.GetNameOwner \
            "string:$WBUS" 2>/dev/null | grep -q ":1\."; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

start_monitor() {
    gdbus monitor --session --dest "$WBUS" >>"$MLOG" 2>&1 &
    MPID=$!
    sleep 0.4
}

stop_monitor() {
    [ -n "${MPID:-}" ] && kill "$MPID" 2>/dev/null
    MPID=""
}

# PIL reads the X server's own framebuffer.
grab() { # grab <display> <out.png>
    DISPLAY="$1" python3 -c "
from PIL import ImageGrab
ImageGrab.grab(xdisplay='$1').save('$2')
" 2>/dev/null
}

pixel() { # pixel <png> <x> <y> -> "R G B" ("" when unreadable)
    [ -s "$1" ] || return 1
    python3 -c "
from PIL import Image
print('%d %d %d' % Image.open('$1').convert('RGB').getpixel(($2, $3)))
" 2>/dev/null
}

px_is() { # px_is <png> <x> <y> <R> <G> <B> <tol>
    local got
    got=$(pixel "$1" "$2" "$3")
    python3 -c "
g = [int(v) for v in '$got'.split()]
w = [$4, $5, $6]
tol = $7
exit(0 if len(g) == 3 and all(abs(a-b) <= tol for a, b in zip(g, w)) else 1)
"
}

# Race-free X display allocation (Xvfb -displayfd).
alloc_xdisplay() { # alloc_xdisplay <varname> -> also sets XVFB_PID
    local __var=$1 __num=
    local __out
    __out=$(mktemp /tmp/trayrig-xd-XXXXXX)
    Xvfb -displayfd 7 7>"$__out" -screen 0 1024x768x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    local __i
    for __i in $(seq 1 20); do
        if [ -s "$__out" ]; then break; fi
        sleep 0.1
    done
    if [ -s "$__out" ]; then
        __num=$(tr -dc '0-9' <"$__out")
        printf -v "$__var" ":%s" "$__num"
    else
        kill "$XVFB_PID" 2>/dev/null
        local __n=$((91 + ($$ % 380)))
        rm -f "/tmp/.X11-unix/X$__n"
        Xvfb ":$__n" -screen 0 1024x768x24 >/dev/null 2>&1 &
        XVFB_PID=$!
        printf -v "$__var" ":%s" "$__n"
    fi
    rm -f "$__out"
}

kill_xdisplay() {
    [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null
    XVFB_PID=""
}

# The XTEST input helper (libXtst — the same test-infra class the
# fdk-shell X11 suite uses). Modes: click (motion+press+release) and
# move (motion only — the cascade's hover trigger). Built ONCE, in
# the outer invocation, before any phase runs.
build_xtest() {
    XTEST="$WORK/xtest"
    cat > "$WORK/xtest.c" << 'XTEOF'
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: xtest click|move X Y\n"); return 2; }
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    int ev = 0, err = 0, maj = 0, min = 0;
    if (!XTestQueryExtension(d, &ev, &err, &maj, &min)) {
        fprintf(stderr, "no XTEST\n"); return 1;
    }
    int x = atoi(argv[2]), y = atoi(argv[3]);
    XTestFakeMotionEvent(d, -1, x, y, CurrentTime);
    XSync(d, False);
    if (argv[1][0] == 'c') {
        XTestFakeButtonEvent(d, 1, True, CurrentTime);
        XSync(d, False);
        XTestFakeButtonEvent(d, 1, False, CurrentTime);
        XSync(d, False);
    }
    XCloseDisplay(d);
    return 0;
}
XTEOF
    XTST_FLAGS=""
    if pkg-config --exists xtst 2>/dev/null; then
        # xtst.pc keeps x11 in Requires.private (static-only), so the
        # shared link needs it spelled out — the helper calls Xlib.
        XTST_FLAGS="$(pkg-config --cflags --libs xtst) -lX11"
    elif [ -f /home/z/apt/prefix/usr/include/X11/extensions/XTest.h ]; then
        XTST_FLAGS="-I/home/z/apt/prefix/usr/include -L/home/z/apt/prefix/usr/lib/x86_64-linux-gnu -lXtst -lX11"
    else
        XTST_FLAGS="-lXtst -lX11"
    fi
    cc -O1 "$WORK/xtest.c" -o "$XTEST" $XTST_FLAGS 2>/dev/null || XTEST=""
}

# The raw right/middle-click driver (button 3 / 2) — the XTEST helper
# above is button-1-only; menus and SecondaryActivate need the rest.
rclick() { # rclick <display> <button> <x> <y>
    DISPLAY="$1" python3 - "$2" "$3" "$4" << 'PYX' 2>/dev/null
import ctypes, sys
btn, x, y = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
lib = ctypes.CDLL("libXtst.so.6")
lib.XOpenDisplay.restype = ctypes.c_void_p
d = lib.XOpenDisplay(None)
if not d:
    sys.exit(1)
lib.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                     ctypes.c_int, ctypes.c_int,
                                     ctypes.c_ulong]
lib.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                     ctypes.c_int, ctypes.c_ulong]
lib.XTestFakeMotionEvent(d, -1, x, y, 0)
lib.XSync(d, 0)
lib.XTestFakeButtonEvent(d, btn, 1, 0)
lib.XSync(d, 0)
lib.XTestFakeButtonEvent(d, btn, 0, 0)
lib.XSync(d, 0)
PYX
}

phase_P() {
    local rc=0
    export LSAN_OPTIONS="suppressions=$SUPP"
    : > "$DLOG"; : > "$MLOG"

    echo "   P0: the host takes the Watcher name"
    DISPLAY="$PH_DISP" FDK_TRAYHOST_TRACE=1 "$TRAYHOST" --font-size 14 \
        --quit-after "$((PH_LIFE * 5 + 20))" >>"$DLOG" 2>&1 &
    DPID=$!
    wait_for_name
    check "P0 name acquired" $?

    start_monitor

    # P1 -------------------------------------------------------------
    local out
    out=$(dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher org.freedesktop.DBus.Properties.GetAll \
        string:org.kde.StatusNotifierWatcher 2>&1)
    local p1=0
    echo "$out" | grep -q 'string "ProtocolVersion"' || p1=1
    echo "$out" | grep -q 'string "IconTheme"' || p1=1
    echo "$out" | grep -q 'string "RegisteredStatusNotifierItems"' || p1=1
    echo "$out" | grep -q 'string "IsStatusNotifierHostRegistered"' || p1=1
    echo "$out" | grep -q 'int32 0' || p1=1
    echo "$out" | grep -q 'boolean true' || p1=1
    check "P1 GetAll: four properties, honest values (libdbus parses)" $p1

    # P2 -------------------------------------------------------------
    local p2=0
    # gdbus prints GVariant text form: the int32 variant as (<0>,),
    # the boolean as (<true>,) — NOT dbus-send's "int32 0" spelling.
    out=$(gdbus call --session --dest $WBUS \
        --object-path /StatusNotifierWatcher \
        --method org.freedesktop.DBus.Properties.Get \
        org.kde.StatusNotifierWatcher ProtocolVersion 2>&1)
    echo "$out" | grep -q "(<0>," || p2=1
    out=$(gdbus call --session --dest $WBUS \
        --object-path /StatusNotifierWatcher \
        --method org.freedesktop.DBus.Properties.Get \
        org.kde.StatusNotifierWatcher IsStatusNotifierHostRegistered 2>&1)
    echo "$out" | grep -q "(<true>," || p2=1
    out=$(dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher \
        string:IconTheme 2>&1)
    echo "$out" | grep -q 'variant.*string' || p2=1
    out=$(dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher \
        string:RegisteredStatusNotifierItems 2>&1)
    # dbus-send prints an EMPTY array reply as "array [ ]" with NO
    # "variant" prefix — grep the word that is always there.
    echo "$out" | grep -q 'array' || p2=1
    check "P2 Get per property (GLib parses the variants)" $p2

    # P3 -------------------------------------------------------------
    : > "$PLOG"
    DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
        "$PROBE" kde "$((PH_LIFE + 8))" >>"$PLOG" 2>&1 &
    local PPID_=$!
    sleep 1.5
    grep -q "registered$" "$PLOG"
    check "P3a the probe registered (KDE dialect)" $?
    grep -q "StatusNotifierItemRegistered" "$MLOG"
    check "P3b the monitor saw StatusNotifierItemRegistered" $?
    grep -q "PropertiesChanged" "$MLOG"
    check "P3c PropertiesChanged observed by the monitor" $?
    out=$(dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher \
        string:RegisteredStatusNotifierItems 2>&1)
    # Array ELEMENTS only: dbus-send prints each as an indented,
    # QUOTED, typed line (measured: `      string ":1.2"`); the reply
    # header itself carries "sender=:1.x" — a bare ":1." grep matches
    # the HEADER and never tests the array.
    echo "$out" | grep -qE '^[[:space:]]+string ":1\.[0-9]+"[[:space:]]*$'
    check "P3d the items array carries the registration" $?
    grep -q "GetAll(org.kde.StatusNotifierItem)" "$DLOG"
    check "P3e the host fetched the item's properties" $?
    grep -q "GetLayout(/MenuBar) prefetch" "$DLOG"
    check "P3f the menu was prefetched (first-click works)" $?

    # P4 -------------------------------------------------------------
    local hostile=0
    dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher \
        org.kde.StatusNotifierWatcher.RegisterStatusNotifierItem \
        string:"" 2>&1 | grep -q "InvalidArgs" || hostile=1
    dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher \
        org.kde.StatusNotifierWatcher.RegisterStatusNotifierItem \
        string:"not a name!!" 2>&1 | grep -q "InvalidArgs" || hostile=1
    local big
    big=$(python3 -c "print('a' * 300)")
    dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher \
        org.kde.StatusNotifierWatcher.RegisterStatusNotifierItem \
        "string:$big" 2>&1 | grep -q "InvalidArgs" || hostile=1
    dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher \
        org.kde.StatusNotifierWatcher.RegisterStatusNotifierItem \
        uint32:7 2>&1 | grep -q "InvalidArgs" || hostile=1
    dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher \
        org.kde.StatusNotifierWatcher.RegisterStatusNotifierHost \
        string:"/nope" 2>&1 | grep -q "InvalidArgs" || hostile=1
    wait_for_name
    [ $? -eq 0 ] || hostile=1
    out=$(gdbus call --session --dest $WBUS \
        --object-path /StatusNotifierWatcher \
        --method org.freedesktop.DBus.Properties.Get \
        org.kde.StatusNotifierWatcher ProtocolVersion 2>&1)
    echo "$out" | grep -q "(<0>," || hostile=1
    check "P4 hostile registrations: all refused, host alive" $hostile

    # P5 -------------------------------------------------------------
    : > "$MLOG"
    DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
        "$PROBE" ayatana 4 >>"$PLOG" 2>&1 &
    sleep 1.5
    grep -q "StatusNotifierItemRegistered" "$MLOG"
    check "P5a the Ayatana dialect registered" $?
    out=$(dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher \
        string:RegisteredStatusNotifierItems 2>&1)
    echo "$out" | grep -q "org/fdk/trayprobe/Item1"
    check "P5b the items array carries the object path" $?
    sleep 3.2

    # P6 -------------------------------------------------------------
    kill $PPID_ 2>/dev/null
    wait $PPID_ 2>/dev/null
    sleep 1.0
    grep -q "StatusNotifierItemUnregistered" "$MLOG"
    check "P6a the exit dropped the item (Unregistered observed)" $?
    out=$(dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher \
        string:RegisteredStatusNotifierItems 2>&1)
    # Element-anchored (the header's sender would match a bare grep).
    if echo "$out" | grep -qE '^[[:space:]]+string ":1\.[0-9]+"[[:space:]]*$'; then
        check "P6b the array no longer carries it" 1
    else
        check "P6b the array no longer carries it" 0
    fi

    # P7 -------------------------------------------------------------
    out=$(dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher org.freedesktop.DBus.Introspectable.\
Introspect 2>&1)
    local p7=0
    echo "$out" | grep -q "RegisterStatusNotifierItem" || p7=1
    echo "$out" | grep -q "RegisterStatusNotifierHost" || p7=1
    echo "$out" | grep -q "StatusNotifierItemRegistered" || p7=1
    check "P7a Introspect tells the truth" $p7
    dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher org.freedesktop.DBus.Peer.Ping \
        >/dev/null 2>&1
    check "P7b Peer.Ping answered" $?
    dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher org.freedesktop.DBus.Properties.Set \
        string:org.kde.StatusNotifierWatcher string:IconTheme \
        variant:string:"Adwaita" 2>&1 | grep -qi "error" \
        && check "P7c Set on a read-only property refused" 0 \
        || check "P7c Set on a read-only property refused" 1
    dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher string:NoSuchThing \
        2>&1 | grep -q "InvalidArgs"
    check "P7d unknown property -> InvalidArgs" $?
    dbus-send --session --print-reply --dest=$WBUS \
        /StatusNotifierWatcher org.kde.StatusNotifierWatcher.Bogus \
        2>&1 | grep -q "UnknownMethod"
    check "P7e unknown method -> UnknownMethod" $?

    stop_monitor
    kill $DPID 2>/dev/null
    wait $DPID 2>/dev/null
    local drc=$?
    check "P-last host exit code 0 (got $drc)" \
        $([ $drc -eq 0 ] && echo 0 || echo 1)
    grep -qE "ERROR: (Leak|Address)Sanitizer" "$DLOG" \
        && check "P-last no sanitizer findings" 1 \
        || check "P-last no sanitizer findings" 0

    # P8 -------------------------------------------------------------
    local BLOG="$WORK/b.log" CLOG="$WORK/c.log"
    : > "$BLOG"; : > "$CLOG"
    DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
        FDK_TRAYHOST_TRACE=1 "$TRAYHOST" --font-size 14 --quit-after 6 >>"$DLOG" 2>&1 &
    local APID=$!
    wait_for_name
    check "P8 incumbent serving" $?

    DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
        "$TRAYHOST" --font-size 14 >"$BLOG" 2>&1
    local brc=$?
    grep -q "already owned" "$BLOG"
    local bline=$?
    check "P8b second host: the clear line (exit $brc)" \
        $([ $brc -eq 0 ] && [ $bline -eq 0 ] && echo 0 || echo 1)

    DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
        FDK_TRAYHOST_TRACE=1 "$TRAYHOST" --replace --quit-after 5 \
        >>"$CLOG" 2>&1 &
    local CPID=$!
    sleep 1.5
    wait $APID 2>/dev/null
    local arc=$?
    grep -q "another host took" "$DLOG"
    local alog=$?
    check "P8c first host handed over (exit $arc)" \
        $([ $arc -eq 0 ] && [ $alog -eq 0 ] && echo 0 || echo 1)
    wait_for_name
    check "P8d the replacing host serves" $?
    kill $CPID 2>/dev/null
    wait $CPID 2>/dev/null

    if [ -x "$PROBE" ]; then
        DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
            "$PROBE" squat 8 >>"$PLOG" 2>&1 &
        local SPID=$!
        sleep 1.0
        : > "$BLOG"
        DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
            "$TRAYHOST" --replace >"$BLOG" 2>&1
        brc=$?
        grep -q "refuses replacement" "$BLOG"
        bline=$?
        check "P8e --replace vs a refusing owner (exit $brc)" \
            $([ $brc -eq 0 ] && [ $bline -eq 0 ] && echo 0 || echo 1)
        kill $SPID 2>/dev/null
        wait $SPID 2>/dev/null
    else
        note "P8e (skipped: probe not built)"
        [ "$REQUIRE" = "1" ] && check "P8e probe present" 1
    fi
    return $((1 - PASS))
}

phase_X() {
    export LSAN_OPTIONS="suppressions=$SUPP"
    : > "$DLOG"; : > "$MLOG"; : > "$PLOG"
    local W=1024 H=768
    local SNAP="$PH_WORK/x1.png" SNAP2="$PH_WORK/x2.png" SNAP3="$PH_WORK/x3.png"

    echo "   X0: the host and the probe under X"
    DISPLAY="$PH_DISP" FDK_TRAYHOST_TRACE=1 "$TRAYHOST" --font-size 14 \
        --quit-after "$((PH_LIFE * 5 + 30))" >>"$DLOG" 2>&1 &
    local DPID=$!
    wait_for_name
    check "X0 name acquired" $?

    DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
        "$PROBE" kde "$((PH_LIFE + 25))" >>"$PLOG" 2>&1 &
    local PPID_=$!
    sleep 1.5
    grep -q "registered$" "$PLOG"
    check "X0b the probe registered" $?
    sleep 0.6

    # Bar geometry: screen 1024, margin 4, cell 30, pad 3 ->
    # bar 36x36 at (984,4); cell 0 = 987..1017; icon center (1002,22).
    grab "$PH_DISP" "$SNAP"
    [ -s "$SNAP" ]
    check "X1a the framebuffer is readable" $?
    # The built-in default theme's TOOLTIP_BACKGROUND is LIGHT
    # (240,239,245) — measured, not assumed dark.
    px_is "$SNAP" 1002 5 240 239 245 30
    check "X1b the bar background is on screen (top-right)" $?
    local ic
    ic=$(pixel "$SNAP" 1002 22)
    python3 -c "
c = [int(v) for v in '${ic:-x}'.split()]
b = c[2] if len(c) == 3 else -1
exit(0 if b > 120 and c[0] < 120 else 1)
"
    check "X1c the blue 32x32 icon is in its cell ($ic)" $?
    px_is "$SNAP" 900 20 0 0 0 6
    check "X1d the desktop beside the bar is untouched" $?

    if [ -n "$PH_XTEST" ]; then
        # X2: left click -> Activate with SCREEN coordinates.
        DISPLAY="$PH_DISP" "$PH_XTEST" click 1002 22 2>/dev/null
        sleep 0.8
        grep -qE "Activate x=100[0-4] y=2[0-4]" "$PLOG"
        check "X2 left click -> Activate(screen coords) logged" $?

        # X3: right click -> the menu opens.
        # Right-click = button 3; the helper clicks button 1 only, so
        # the rig drives the raw XTEST sequence inline.
        DISPLAY="$PH_DISP" python3 - << 'PYX' 2>/dev/null
import ctypes, ctypes.util, os
lib = ctypes.CDLL("libXtst.so.6")
lib.XOpenDisplay.restype = ctypes.c_void_p
d = lib.XOpenDisplay(None)
lib.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                     ctypes.c_int, ctypes.c_int,
                                     ctypes.c_ulong]
lib.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                     ctypes.c_int, ctypes.c_ulong]
lib.XTestFakeMotionEvent(d, -1, 1002, 22, 0)
lib.XSync(d, 0)
lib.XTestFakeButtonEvent(d, 3, 1, 0)
lib.XSync(d, 0)
lib.XTestFakeButtonEvent(d, 3, 0, 0)
lib.XSync(d, 0)
PYX
        sleep 1.0
        grab "$PH_DISP" "$SNAP2"
        # The menu: right-aligned to the cell (right edge 1017),
        # hanging left of the bar; row one spans screen y 44..72.
        # Sample (1000,58): row one's right side — past the label
        # glyphs (~x 909-965), clear of the hover highlight (the
        # pointer sat at the bar, not the menu).
        local mc
        mc=$(pixel "$SNAP2" 1000 58)
        python3 -c "
c = [int(v) for v in '${mc:-x}'.split()]
exit(0 if len(c) == 3 and abs(c[0]-240)<30 and abs(c[1]-239)<30 and abs(c[2]-245)<30 else 1)
"
        check "X3a the menu is open (background pixels at its row one)" $?
        grep -q "menu open (root" "$DLOG"
        check "X3b the host opened the root menu" $?
        grep -q "label 'First item'" "$DLOG"
        check "X3c the mnemonic was stripped (Fi_rst -> First)" $?
        grep -q "nodes<=10" "$DLOG"
        check "X3d the whole tree parsed (10 nodes)" $?
        grep -q "AboutToShow id=0" "$PLOG"
        check "X3e the AboutToShow handshake ran" $?

        # X4: click row one -> Event id=1 clicked; the menu closes.
        DISPLAY="$PH_DISP" "$PH_XTEST" click 950 58 2>/dev/null
        sleep 1.0
        grep -q "Event id=1 clicked" "$PLOG"
        check "X4a the row click delivered Event(1, clicked)" $?
        grab "$PH_DISP" "$SNAP2"
        local gone
        gone=$(pixel "$SNAP2" 950 100)
        px_is "$SNAP2" 950 100 0 0 0 6
        check "X4b the menu closed after the choice ($gone)" $?

        # X5: right-click again; hover the submenu row -> the child
        # FLIPS LEFT (the screen edge); its pixels appear.
        DISPLAY="$PH_DISP" python3 - << 'PYX' 2>/dev/null
import ctypes
lib = ctypes.CDLL("libXtst.so.6")
lib.XOpenDisplay.restype = ctypes.c_void_p
d = lib.XOpenDisplay(None)
lib.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                     ctypes.c_int, ctypes.c_int,
                                     ctypes.c_ulong]
lib.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                     ctypes.c_int, ctypes.c_ulong]
lib.XTestFakeMotionEvent(d, -1, 1002, 22, 0)
lib.XSync(d, 0)
lib.XTestFakeButtonEvent(d, 3, 1, 0)
lib.XSync(d, 0)
lib.XTestFakeButtonEvent(d, 3, 0, 0)
lib.XSync(d, 0)
PYX
        sleep 1.0
        # The submenu row: row 7 spans menu-local y 153..181 ->
        # screen y 193..221; x anywhere over the menu.
        DISPLAY="$PH_DISP" "$PH_XTEST" move 950 207 2>/dev/null
        sleep 1.0
        grep -q "submenu flip" "$DLOG"
        check "X5a the cascade flipped left (trace)" $?
        grep -q "Event id=7 opened" "$PLOG"
        check "X5b the submenu parent got Event(7, opened)" $?
        grab "$PH_DISP" "$SNAP3"
        # The child hangs LEFT of the parent (parent left edge ~877,
        # child spans ~767..877): scan a band inside it for the menu
        # background, clear of the nested labels (right half of the
        # child popup, above/below row text works — sample y=210 is
        # row two's vertical center, x past the text).
        local found=1 xf
        for xf in 830 840 850 860 870; do
            if px_is "$SNAP3" $xf 210 240 239 245 30 2>/dev/null; then
                found=0
                note "X5c child popup found at x=$xf"
                break
            fi
        done
        check "X5c the child popup is LEFT of the parent (pixels)" $found

        # X6: click the nested row -> Event id=8 clicked.
        DISPLAY="$PH_DISP" "$PH_XTEST" click 790 211 2>/dev/null
        sleep 1.0
        grep -q "Event id=8 clicked" "$PLOG"
        check "X6 the nested row click delivered Event(8, clicked)" $?

        # X7: Passive fades; NeedsAttention alarms.
        gdbus call --session --dest org.fdk.TrayProbe \
            --object-path /org/fdk/trayprobe/Control \
            --method fdk.trayprobe.Control.SetStatus Passive \
            >/dev/null 2>&1
        sleep 1.2
        grab "$PH_DISP" "$SNAP2"
        local pv
        pv=$(pixel "$SNAP2" 1002 22)
        # The fade: alpha 0.45 over the light bg — the expected
        # pixel is the blend 0.45*icon + 0.55*bg (icon 40,90,210;
        # bg 240,239,245 -> 150,172,229).
        python3 -c "
a = [int(v) for v in '${ic:-x}'.split()]
p = [int(v) for v in '${pv:-x}'.split()]
bg = [240, 239, 245]
exp = [0.45 * a[i] + 0.55 * bg[i] for i in range(3)]
ok = len(p) == 3 and len(a) == 3 and \
     all(abs(p[i] - exp[i]) <= 30 for i in range(3))
exit(0 if ok else 1)
"
        check "X7a Passive: the icon faded (never hidden) ($pv)" $?
        gdbus call --session --dest org.fdk.TrayProbe \
            --object-path /org/fdk/trayprobe/Control \
            --method fdk.trayprobe.Control.SetStatus NeedsAttention \
            >/dev/null 2>&1
        sleep 1.2
        grab "$PH_DISP" "$SNAP2"
        local av sv_
        av=$(pixel "$SNAP2" 1002 22)
        sv_=$(pixel "$SNAP2" 1002 37)
        python3 -c "
a = [int(v) for v in '${av:-x}'.split()]
s = [int(v) for v in '${sv_:-x}'.split()]
# the attention pixmap is amber (230,160,20); the strip is the
# theme accent — it must DIFFER from the light bar bg.
ok = len(a) == 3 and a[0] > 150 and a[1] > 90 and a[1] < 220 and \
     len(s) == 3 and \
     (abs(s[0]-240) > 40 or abs(s[1]-239) > 40 or abs(s[2]-245) > 40)
exit(0 if ok else 1)
"
        check "X7b NeedsAttention: amber icon + the accent strip ($av / $sv_)" $?
        gdbus call --session --dest org.fdk.TrayProbe \
            --object-path /org/fdk/trayprobe/Control \
            --method fdk.trayprobe.Control.SetStatus Active \
            >/dev/null 2>&1
        sleep 1.0

        # X8: the middle click -> SecondaryActivate.
        DISPLAY="$PH_DISP" python3 - << 'PYX' 2>/dev/null
import ctypes
lib = ctypes.CDLL("libXtst.so.6")
lib.XOpenDisplay.restype = ctypes.c_void_p
d = lib.XOpenDisplay(None)
lib.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                     ctypes.c_int, ctypes.c_int,
                                     ctypes.c_ulong]
lib.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                     ctypes.c_int, ctypes.c_ulong]
lib.XTestFakeMotionEvent(d, -1, 1002, 22, 0)
lib.XSync(d, 0)
lib.XTestFakeButtonEvent(d, 2, 1, 0)
lib.XSync(d, 0)
lib.XTestFakeButtonEvent(d, 2, 0, 0)
lib.XSync(d, 0)
PYX
        sleep 0.8
        grep -qE "SecondaryActivate x=100[0-4] y=2[0-4]" "$PLOG"
        check "X8 middle click -> SecondaryActivate logged" $?

        # X9: FlipIcon -> NewIcon -> the refetch -> green pixels.
        gdbus call --session --dest org.fdk.TrayProbe \
            --object-path /org/fdk/trayprobe/Control \
            --method fdk.trayprobe.Control.FlipIcon >/dev/null 2>&1
        sleep 1.5
        grab "$PH_DISP" "$SNAP2"
        local gv
        gv=$(pixel "$SNAP2" 1002 22)
        python3 -c "
g = [int(v) for v in '${gv:-x}'.split()]
ok = len(g) == 3 and g[1] > g[0] + 40 and g[1] > g[2] + 40
exit(0 if ok else 1)
"
        check "X9a FlipIcon: the refetched pixmap renders green ($gv)" $?
        gdbus call --session --dest org.fdk.TrayProbe \
            --object-path /org/fdk/trayprobe/Control \
            --method fdk.trayprobe.Control.Retitle string:"Renamed" \
            >/dev/null 2>&1
        sleep 1.2
        grep -q "signal NewTitle" "$PLOG"
        check "X9b Retitle: NewTitle emitted" $?
        local gcount
        gcount=$(grep -c "GetAll(org.kde.StatusNotifierItem)" "$DLOG" || true)
        [ "${gcount:-0}" -ge 2 ]
        check "X9c the host refetched the properties ($gcount GetAlls)" $?
    else
        note "X2-X9 (skipped: XTEST helper unavailable)"
        [ "$REQUIRE" = "1" ] && check "XTEST helper present" 1
    fi

    kill $DPID 2>/dev/null
    wait $DPID 2>/dev/null
    local drc=$?
    check "X-last host exit code 0 (got $drc)" \
        $([ $drc -eq 0 ] && echo 0 || echo 1)
    kill $PPID_ 2>/dev/null
    wait $PPID_ 2>/dev/null
    grep -qE "ERROR: (Leak|Address)Sanitizer" "$DLOG" \
        && check "X-last no sanitizer findings" 1 \
        || check "X-last no sanitizer findings" 0

    # XS: the TASK C size sweep — 12 / 16 / 24 / 32 px through the
    # ENV channel ($FDK_UI_FONT_SIZE -> fdk_ui_font_px -> bar, cell,
    # menu rows), each proving the geometry follows the font, the
    # icon still lands centered in its cell, the menu opens with
    # text inside its rows, and the whole menu fits the screen.
    for S in 12 16 24 32; do
        local SLOG="$PH_WORK/xs$S.log" SSNAP="$PH_WORK/xs$S.png" \
              SPLOG="$PH_WORK/xsp$S.log"
        : > "$SLOG"; : > "$SPLOG"
        DISPLAY="$PH_DISP" FDK_TRAYHOST_TRACE=1 FDK_UI_FONT_SIZE=$S \
            "$TRAYHOST" --quit-after 30 >>"$SLOG" 2>&1 &
        local SPID=$!
        wait_for_name
        check "XS$S host up at font $S px" $?
        sleep 0.5
        local ui font_px bar_h cell row_fl
        ui=$(grep -o \
            'ui: font [0-9]*px (k=[0-9.]*) bar_h=[0-9]* cell=[0-9]* row>=[0-9]*' \
            "$SLOG" | tail -1)
        font_px=$(printf '%s' "$ui" | sed 's/.*font \([0-9]*\)px.*/\1/')
        bar_h=$(printf '%s' "$ui" | sed 's/.*bar_h=\([0-9]*\).*/\1/')
        cell=$(printf '%s' "$ui" | sed 's/.*cell=\([0-9]*\).*/\1/')
        row_fl=$(printf '%s' "$ui" | sed 's/.*row>=\([0-9]*\).*/\1/')
        local want_bar want_cell
        want_bar=$(awk -v s="$S" 'BEGIN{printf "%d", int(36*s/14+0.5)}')
        want_cell=$(awk -v s="$S" 'BEGIN{printf "%d", int(30*s/14+0.5)}')
        check "XS$S trace math: font=$font_px bar=$bar_h/$want_bar cell=$cell/$want_cell" \
            $([ "$font_px" = "$S" ] && [ "$bar_h" = "$want_bar" ] &&
              [ "$cell" = "$want_cell" ] && [ "$row_fl" -ge 20 ] &&
              echo 0 || echo 1)
        # The menu fits the 768-tall screen: bar + 10 rows + pads.
        check "XS$S the 10-row menu fits below the bar" \
            $([ $(( 4 + bar_h + 8 + 10 * row_fl )) -lt 760 ] &&
              echo 0 || echo 1)
        # A fresh probe registers with THIS host instance.
        DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
            "$PROBE" kde 14 >>"$SPLOG" 2>&1 &
        local QPID=$!
        sleep 1.6
        grep -q "registered$" "$SPLOG"
        check "XS$S the probe registered" $?
        sleep 0.6
        grab "$PH_DISP" "$SSNAP"
        [ -s "$SSNAP" ]
        check "XS$S framebuffer readable" $?
        # The icon centered in the computed cell: bar_w = 2*pad + cell,
        # pad = round(3S/14); blue at the center.
        local bar_pad cx cy
        bar_pad=$(awk -v s="$S" 'BEGIN{printf "%d", int(3*s/14+0.5)}')
        cx=$(( 1024 - 4 - 2 * bar_pad - cell + bar_pad + cell / 2 ))
        cy=$(( 4 + bar_h / 2 ))
        local verdicts
        verdicts=$(python3 - "$SSNAP" "$cx" "$cy" << 'PYX'
import sys
from PIL import Image
snap, cx, cy = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
img = Image.open(snap).convert('RGB')
W, H = img.size
def px(x, y):
    if 0 <= x < W and 0 <= y < H:
        return img.getpixel((x, y))
    return (0, 0, 0)
c = px(cx, cy)
icon_ok = len(c) == 3 and c[2] > 120 and c[0] < 120
desk_ok = px(900, 20) == (0, 0, 0)
print('%d %d %s' % (0 if icon_ok else 1, 0 if desk_ok else 1,
                    '%d %d %d' % c))
PYX
)
        local v_icon v_desk v_rgb
        v_icon=$(printf '%s' "$verdicts" | cut -d' ' -f1)
        v_desk=$(printf '%s' "$verdicts" | cut -d' ' -f2)
        v_rgb=$(printf '%s' "$verdicts" | cut -d' ' -f3-)
        check "XS$S the blue icon centered in its cell ($v_rgb)" \
            "${v_icon:-1}"
        check "XS$S the desktop beside the bar untouched" "${v_desk:-1}"
        # Right-click opens the menu; row one carries text.
        if [ -n "$PH_XTEST" ]; then
            # The raw right-click driver (button 3), the X3 leg's
            # proven inline shape — UNQUOTED heredoc so the computed
            # cell coordinates land in the python.
            DISPLAY="$PH_DISP" python3 - << PYX 2>/dev/null
import ctypes
lib = ctypes.CDLL("libXtst.so.6")
lib.XOpenDisplay.restype = ctypes.c_void_p
d = lib.XOpenDisplay(None)
lib.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                     ctypes.c_int, ctypes.c_int,
                                     ctypes.c_ulong]
lib.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                     ctypes.c_int, ctypes.c_ulong]
lib.XTestFakeMotionEvent(d, -1, $cx, $cy, 0)
lib.XSync(d, 0)
lib.XTestFakeButtonEvent(d, 3, 1, 0)
lib.XSync(d, 0)
lib.XTestFakeButtonEvent(d, 3, 0, 0)
lib.XSync(d, 0)
PYX
            sleep 1.0
            grab "$PH_DISP" "$SSNAP"
            local menu_y0=$(( 4 + bar_h ))
            local mrow0=$(( menu_y0 + 4 ))
            local mver
            mver=$(python3 - "$SSNAP" "$mrow0" "$row_fl" \
                          "$menu_y0" << 'PYX'
import sys
from PIL import Image
snap, mrow0, row_fl, menu_y0 = sys.argv[1], *(int(v) for v in
                                               sys.argv[2:5])
img = Image.open(snap).convert('RGB')
W, H = img.size
def px(x, y):
    if 0 <= x < W and 0 <= y < H:
        return img.getpixel((x, y))
    return (0, 0, 0)
BG = (240, 239, 245)
def is_bg(x, y, tol=10):
    r, g, b = px(x, y)
    return (abs(r-BG[0]) <= tol and abs(g-BG[1]) <= tol and
            abs(b-BG[2]) <= tol)
# The menu opened: its row-one background is neither the desktop's
# black nor absent.
mc = px(1000, mrow0 + row_fl // 2)
menu_ok = len(mc) == 3 and mc != (0, 0, 0)
# Text inside the row-one band: a glyph core somewhere in the row.
txt = 0
for x in range(720, 991, 3):
    for y in range(mrow0, mrow0 + row_fl + 1, 2):
        if not is_bg(x, y):
            txt = 1
            break
    if txt:
        break
print('%d %d %s' % (0 if menu_ok else 1, 0 if txt else 1,
                    '%d %d %d' % mc))
PYX
)
            local v_menu v_txt v_mrgb
            v_menu=$(printf '%s' "$mver" | cut -d' ' -f1)
            v_txt=$(printf '%s' "$mver" | cut -d' ' -f2)
            v_mrgb=$(printf '%s' "$mver" | cut -d' ' -f3-)
            check "XS$S the menu opened below the bar ($v_mrgb)" \
                "${v_menu:-1}"
            check "XS$S menu row one carries text inside its rows" \
                "${v_txt:-1}"
        else
            note "XS$S menu leg (skipped: no XTEST)"
        fi
        grep -qE "ERROR: (Leak|Address)Sanitizer" "$SLOG" \
            && check "XS$S host sanitizer-clean" 1 \
            || check "XS$S host sanitizer-clean" 0
        kill $QPID $SPID 2>/dev/null
        wait $QPID $SPID 2>/dev/null
        sleep 0.4
    done
    return $((1 - PASS))
}

phase_W() {
    export LSAN_OPTIONS="suppressions=$SUPP"
    : > "$DLOG"; : > "$PLOG"

    export WAYLAND_DISPLAY="$PH_WSOCK"
    echo "   W0: the host runs under a real compositor"
    FDK_TRAYHOST_TRACE=1 WAYLAND_DEBUG=1 "$TRAYHOST" --font-size 14 \
        --quit-after "$PH_LIFE" >>"$DLOG" 2>&1 &
    local DPID=$!
    wait_for_name
    check "W0 name acquired under sway" $?

    WAYLAND_DISPLAY="$PH_WSOCK" LSAN_OPTIONS="suppressions=$SUPP" \
        "$PROBE" kde "$((PH_LIFE - 2))" >>"$PLOG" 2>&1 &
    local PPID_=$!
    sleep 1.5
    grep -q "registered$" "$PLOG"
    check "W1a the probe registered under sway" $?

    grep -q "zwlr_layer_surface_v1.*configure" "$DLOG"
    check "W1b zwlr configure arrived" $?
    if grep -q "wl_display.error" "$DLOG"; then
        check "W1c no protocol error" 1
    else
        check "W1c no protocol error" 0
    fi
    python3 - "$DLOG" << 'PYW'
import re, sys
log = open(sys.argv[1], encoding='utf-8', errors='replace').read()
events = re.findall(
    r'\[[0-9.]+\] +-> +([a-z_0-9@]+)\.(attach|ack_configure)\(', log)
acked = set()
bad = False
for obj, kind in events:
    if kind == 'ack_configure':
        acked.add(obj)
    elif kind == 'attach' and obj not in acked:
        bad = True
sys.exit(1 if bad else 0)
PYW
    check "W1d no attach before the first ack_configure" $?

    if have grim; then
        WLR_BACKENDS=headless grim -o HEADLESS-1 "$PH_WORK/w1.png" \
            2>/dev/null
        [ -s "$PH_WORK/w1.png" ]
        check "W2a grim captured the output" $?
        # The bar top-right: background at (1000,20) vs desktop (700,20).
        local wc wd
        wc=$(pixel "$PH_WORK/w1.png" 1000 20)
        wd=$(pixel "$PH_WORK/w1.png" 700 20)
        [ "$wc" != "$wd" ]
        check "W2b the bar is on the output (contrast at its edge)" $?
        local ic2
        ic2=$(pixel "$PH_WORK/w1.png" 1002 22)
        python3 -c "
c = [int(v) for v in '${ic2:-x}'.split()]
exit(0 if len(c) == 3 and c[2] > 120 and c[0] < 120 else 1)
"
        check "W2c the icon's pixels are on the output ($ic2)" $?
    else
        note "W2 (skipped: no grim)"
        [ "$REQUIRE" = "1" ] && check "W2 grim present" 1
    fi

    kill $PPID_ 2>/dev/null
    wait $PPID_ 2>/dev/null
    kill $DPID 2>/dev/null
    wait $DPID 2>/dev/null
    local drc=$?
    check "W-last host exit code 0 (got $drc)" \
        $([ $drc -eq 0 ] && echo 0 || echo 1)
    grep -qE "ERROR: (Leak|Address)Sanitizer" "$DLOG" \
        && check "W-last no sanitizer findings" 1 \
        || check "W-last no sanitizer findings" 0
    return $((1 - PASS))
}


# ---- phase dispatch: the rig re-invokes itself INSIDE
# dbus-run-session (the session bus env arrives that way) --------------

case "${1:-}" in
__phaseP__|__phaseX__|__phaseW__)
    PHASE=${1#__phase}; PHASE=${PHASE%__}
    PH_DISP=$2; PH_WORK=$3; PH_LIFE=$4
    DLOG=$5; MLOG=$6; WORK=$3
    PH_XTEST=${7:-}; PH_OPENBOX=${8:-0}; PH_WSOCK=${2:-}
    PLOG="$PH_WORK/probe.log"
    phase_"$PHASE"
    exit $((1 - PASS))
    ;;
esac

# ---- skip discipline -------------------------------------------------------

for tool in dbus-run-session dbus-send gdbus Xvfb python3; do
    if ! have "$tool"; then
        echo "[skip] trayhost rig: $tool not available"
        [ "$REQUIRE" = "1" ] && exit 1
        exit 0
    fi
done
if [ ! -x "$TRAYHOST" ]; then
    echo "[skip] trayhost not built (make -C fdk-shell examples)"
    [ "$REQUIRE" = "1" ] && exit 1
    exit 0
fi
if [ ! -x "$PROBE" ]; then
    echo "[skip] probe not built (make -C fdk-shell test-tray)"
    [ "$REQUIRE" = "1" ] && exit 1
    exit 0
fi
python3 -c "from PIL import ImageGrab" 2>/dev/null || {
    echo "[skip] PIL unavailable"
    [ "$REQUIRE" = "1" ] && exit 1
    exit 0
}

WORK=$(mktemp -d /tmp/trayrig-XXXXXX)
trap 'rm -rf "$WORK"' EXIT
XDG="$WORK/xdg"
mkdir -p "$XDG"
chmod 700 "$XDG"
export XDG_RUNTIME_DIR="$XDG"
build_xtest

echo "trayhost rig: $(date +%H:%M:%S) work=$WORK life=${LIFE}s"

# ---- PHASE P --------------------------------------------------------------

echo "-- phase P: protocol (real dbus-daemon, dbus-send + gdbus)"
alloc_xdisplay PVFB
dbus-run-session -- bash "$0" __phaseP__ "$PVFB" "$WORK" "$LIFE" \
    "$WORK/host-p.log" "$WORK/mon-p.log" "$XTEST"
P_RC=$?
kill_xdisplay
echo "== phase P: rc=$P_RC"
[ $P_RC -ne 0 ] && PASS=0

# ---- PHASE X --------------------------------------------------------------

echo "-- phase X: pixels + menus (Xvfb + openbox + XTEST)"
alloc_xdisplay XVFB
HAVE_OPENBOX=0
OB_PID=""
if have openbox; then
    # XDG_DATA_DIRS points openbox at the user-space prefix's themes
    # (a prefix-installed openbox otherwise finds no theme and EXITS
    # -- "Unable to load a theme" -- leaving a WM-less screen).
    DISPLAY="$XVFB" \
        XDG_DATA_DIRS=/home/z/apt/prefix/usr/share:/usr/share \
        openbox >/dev/null 2>&1 &
    OB_PID=$!
    HAVE_OPENBOX=1
    sleep 0.8
fi
dbus-run-session -- bash "$0" __phaseX__ "$XVFB" "$WORK" "$LIFE" \
    "$WORK/host-x.log" "$WORK/mon-x.log" "$XTEST" "$HAVE_OPENBOX"
X_RC=$?
[ -n "$OB_PID" ] && { kill "$OB_PID" 2>/dev/null; wait "$OB_PID" 2>/dev/null; }
kill_xdisplay
echo "== phase X: rc=$X_RC"
[ $X_RC -ne 0 ] && PASS=0

# ---- PHASE W --------------------------------------------------------------

echo "-- phase W: layer-shell (headless sway)"
if have sway && have grim; then
    SWAY_CONF="$WORK/sway.conf"
    printf 'output HEADLESS-1 mode 1024x768\n' > "$SWAY_CONF"
    WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
        XDG_RUNTIME_DIR="$XDG" sway -c "$SWAY_CONF" \
        >"$WORK/sway.log" 2>&1 &
    SWAY_PID=$!
    WSOCK=""
    for i in $(seq 1 30); do
        WSOCK=$(ls "$XDG" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
        [ -n "$WSOCK" ] && break
        sleep 0.2
    done
    [ -n "$WSOCK" ] && WSOCK="$XDG/$WSOCK"
    if [ -n "$WSOCK" ]; then
        dbus-run-session -- bash "$0" __phaseW__ "$WSOCK" "$WORK" \
            "$LIFE" "$WORK/host-w.log" "$WORK/mon-w.log" "$XTEST"
        W_RC=$?
    else
        echo "   (sway socket never appeared)"
        W_RC=1
    fi
    kill $SWAY_PID 2>/dev/null
    wait $SWAY_PID 2>/dev/null
else
    # The notifyd CI pattern: REQUIRE makes TOOL skips fatal (no bus,
    # no X, no PIL), but the Wayland legs SELF-SKIP on runners
    # without sway — the sandbox and TESTING-ON-DESKTOP cover them.
    echo "   [skip] sway/grim unavailable — Wayland legs skipped"
    W_RC=0
fi
echo "== phase W: rc=$W_RC"
[ $W_RC -ne 0 ] && PASS=0

# ---- verdict ---------------------------------------------------------------

echo
if [ "$PASS" = "1" ]; then
    echo "trayhost rig: ALL PASS"
else
    echo "trayhost rig: FAILURES PRESENT"
fi
exit $((1 - PASS))
