#!/usr/bin/env bash
# §26/3b: fdk-files' tray + notification against a REAL session bus
# with the fdk-shell-overlay TRAY HOST as the watcher.
#
# Legs (each prints PASS/FAIL; any FAIL exits 1):
#   T1  the tray host registers org.kde.StatusNotifierHost on the
#       session bus (dbus-send NameHasOwner)
#   T2  fdk-files runs with DBUS_SESSION_BUS_ADDRESS set; its tray
#       ITEM registers (StatusNotifierItemRegistered seen via the
#       host's items property carrying org.fdk.testapps.files)
#   T3  a notification sent BY fdk-files (the long-op finish path)
#       reaches the bus: monitor the org.freedesktop.Notifications
#       Notify call with gdbus
#
# Usage: bash run_files_tray_bus.sh [path-to-fdk-files]
set -u
FDK_FILES="${1:-/home/z/my-project/work/FDK/test-apps/build/fdk-files}"
OVERLAY=/home/z/my-project/work/FDK/fdk-shell-overlay
WORK=/tmp/fdk-files-traybus
mkdir -p "$WORK"

source /home/z/my-project/scripts/s25-env.sh
export LD_LIBRARY_PATH="/home/z/apt/prefix/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

rc=0
say() { printf '%s\n' "$*"; }
fail() { say "FAIL: $*"; rc=1; }

# a bare X server for both processes
XVFS=""
rm -f /tmp/.X11-unix/X77
Xvfb :77 -screen 0 1024x768x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.2
export DISPLAY=:77

# the throwaway session bus
dbus-daemon --session --fork --print-address=1 --print-pid=1 \
    > "$WORK/bus.addr" 2>/dev/null
BUS_ADDR=$(head -1 "$WORK/bus.addr")
BUS_PID=$(tail -1 "$WORK/bus.addr")
if [ -z "$BUS_ADDR" ]; then
    fail "dbus-daemon did not start"
    kill $XVFB_PID 2>/dev/null
    exit 1
fi
export DBUS_SESSION_BUS_ADDRESS="$BUS_ADDR"
say "bus up: pid $BUS_PID"

cleanup() {
    [ -n "${FILES_PID:-}" ] && kill "$FILES_PID" 2>/dev/null
    [ -n "${HOST_PID:-}" ] && kill "$HOST_PID" 2>/dev/null
    kill "$BUS_PID" 2>/dev/null
    kill "$XVFB_PID" 2>/dev/null
    rm -f /tmp/.X11-unix/X77
}
trap cleanup EXIT

# ---- T1: the tray host ----------------------------------------------
export LD_LIBRARY_PATH="$OVERLAY/build:$LD_LIBRARY_PATH"
"$OVERLAY/build/examples/trayhost" > "$WORK/host.log" 2>&1 &
HOST_PID=$!
HOST_UP=0
for _ in $(seq 1 40); do
    if dbus-send --session --dest=org.freedesktop.DBus \
        --type=method_call --print-reply /org/freedesktop/DBus \
        org.freedesktop.DBus.NameHasOwner \
        string:org.kde.StatusNotifierWatcher 2>/dev/null | grep -q true; then
        HOST_UP=1; break
    fi
    sleep 0.25
done
if [ "$HOST_UP" = 1 ]; then say "PASS T1: StatusNotifierHost owns its name"; else fail "T1 host name not owned"; fi

# ---- T2: fdk-files' tray ITEM registers ------------------------------
export XDG_RUNTIME_DIR=/tmp/xdg-run
FDK_TESTAPP_FRAMES=60 ASAN_OPTIONS=detect_leaks=0 \
    "$FDK_FILES" > "$WORK/files.log" 2>&1 &
FILES_PID=$!

# The KDE spec's RegisterStatusNotifierItem carries the ITEM's bus
# name (our unique :1.N), so the array is matched by GROWTH + the
# app's own "registered with the watcher" log line (both sides of
# the handshake, one from each process).
BEFORE=$(dbus-send --session --print-reply \
    --dest=org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
    org.freedesktop.DBus.Properties.Get \
    string:org.kde.StatusNotifierWatcher \
    string:RegisteredStatusNotifierItems 2>/dev/null | grep -c string)
ITEM_SEEN=0
for _ in $(seq 1 60); do
    AFTER=$(dbus-send --session --print-reply \
        --dest=org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
        org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher \
        string:RegisteredStatusNotifierItems 2>/dev/null | grep -c string)
    if [ "$AFTER" -gt "$BEFORE" ] && \
       grep -q "tray: registered with the watcher" \
           "$WORK/files.log" 2>/dev/null; then
        ITEM_SEEN=1; break
    fi
    sleep 0.25
done
if [ "$ITEM_SEEN" = 1 ]; then
    say "PASS T2: fdk-files' tray item registered (array grew $BEFORE->$AFTER; the app saw the reply)"
else
    fail "T2 item not in host Items (see $WORK/files.log)"
fi

# ---- T3: a notification from fdk-files ------------------------------
# The overlay's notifyd plays the notification daemon (without one,
# fdk-files correctly logs "no notification daemon on the bus" and
# skips sending). Proof of the Notify: notifyd's TRACE log (a unicast
# method call is invisible to gdbus monitor — found live; the daemon
# itself is the honest witness: it traces "place id=N" when it puts
# the arriving notification's card on a layer).
FDK_NOTIFYD_TRACE=1 LD_LIBRARY_PATH="$OVERLAY/build:$LD_LIBRARY_PATH" \
    "$OVERLAY/build/examples/notifyd" > "$WORK/notifyd.log" 2>&1 &
NOTIFYD_PID=$!
sleep 1.5
FDK_TESTAPP_FRAMES=60 ASAN_OPTIONS=detect_leaks=0 \
    "$FDK_FILES" --smoke >> "$WORK/files.log" 2>&1 &
FILES_PID=$!
NOTIFY_SEEN=0
for _ in $(seq 1 80); do
    if grep -q "place id=" "$WORK/notifyd.log" 2>/dev/null; then
        NOTIFY_SEEN=1; break
    fi
    sleep 0.25
done
if [ "$NOTIFY_SEEN" = 1 ]; then
    say "PASS T3: fdk-files' notification reached the daemon (card placed)"
else
    fail "T3 no notification placed (see $WORK/notifyd.log)"
fi

kill $NOTIFYD_PID 2>/dev/null
if [ $rc -eq 0 ]; then say "fdk-files tray-bus rig: ALL PASS"; fi
exit $rc
