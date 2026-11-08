#!/usr/bin/env bash
# §26/3b: fdk-files' tray + notification against a REAL session bus
# with a REAL StatusNotifierWatcher and notification daemon.
#
# The counterpart processes are whatever the environment provides:
#   TRAYHOST_BIN  an executable that registers org.kde.StatusNotifierWatcher
#   NOTIFYD_BIN   an executable that owns org.freedesktop.Notifications
# If unset, the rig probes the bus for a watcher/daemon already there
# (a running desktop, for instance) and honestly SKIPS the legs that
# have no counterpart — FDK's own mock-daemon/watcher legs live in
# `make test-dbus`, which is where the always-available coverage sits.
#
# Legs (each prints PASS/FAIL/SKIP; any FAIL exits 1):
#   T1  a StatusNotifierWatcher owns its bus name
#   T2  fdk-files runs with DBUS_SESSION_BUS_ADDRESS set; its tray
#       ITEM registers (StatusNotifierItemRegistered seen via the
#       watcher's RegisteredStatusNotifierItems property carrying
#       org.fdk.testapps.files)
#   T3  a notification sent BY fdk-files (the long-op finish path)
#       reaches the bus: dbus-monitor witnesses the Notify method
#       call addressed to org.freedesktop.Notifications
#
# Usage: bash run_files_tray_bus.sh [path-to-fdk-files]
set -u
FDK_FILES="${1:-/home/z/my-project/work/FDK/test-apps/build/fdk-files}"
WORK=/tmp/fdk-files-traybus
mkdir -p "$WORK"

source /home/z/my-project/scripts/s25-env.sh
export LD_LIBRARY_PATH="/home/z/apt/prefix/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

rc=0
say() { printf '%s\n' "$*"; }
fail() { say "FAIL: $*"; rc=1; }
skip() { say "SKIP: $*"; }

bus_name_owned() {
    dbus-send --session --dest=org.freedesktop.DBus \
        --type=method_call --print-reply /org/freedesktop/DBus \
        org.freedesktop.DBus.NameHasOwner "string:$1" 2>/dev/null | grep -q true
}

# a bare X server for both processes
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
    [ -n "${NOTIFYD_PID:-}" ] && kill "$NOTIFYD_PID" 2>/dev/null
    [ -n "${MONITOR_PID:-}" ] && kill "$MONITOR_PID" 2>/dev/null
    kill "$BUS_PID" 2>/dev/null
    kill "$XVFB_PID" 2>/dev/null
    rm -f /tmp/.X11-unix/X77
}
trap cleanup EXIT

# ---- T1: the tray watcher --------------------------------------------
if [ -n "${TRAYHOST_BIN:-}" ]; then
    "$TRAYHOST_BIN" > "$WORK/host.log" 2>&1 &
    HOST_PID=$!
fi
HOST_UP=0
for _ in $(seq 1 40); do
    if bus_name_owned org.kde.StatusNotifierWatcher; then HOST_UP=1; break; fi
    sleep 0.25
done
if [ "$HOST_UP" = 1 ]; then
    say "PASS T1: a StatusNotifierWatcher owns its name"
else
    skip "T1/T2: no StatusNotifierWatcher on the bus (set TRAYHOST_BIN or run a desktop)"
fi

# ---- T2: fdk-files' tray ITEM registers ------------------------------
if [ "$HOST_UP" = 1 ]; then
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
        fail "T2 item not in the watcher's Items (see $WORK/files.log)"
    fi
fi

# ---- T3: a notification from fdk-files ------------------------------
# Witness: dbus-monitor (a proper monitor sees directed method calls,
# which gdbus monitor does not — found live) captures the Notify call
# fdk-files addresses to org.freedesktop.Notifications. The daemon is
# any conforming one: NOTIFYD_BIN if set, else one already on the bus.
if [ -n "${NOTIFYD_BIN:-}" ]; then
    "$NOTIFYD_BIN" > "$WORK/notifyd.log" 2>&1 &
    NOTIFYD_PID=$!
fi
if bus_name_owned org.freedesktop.Notifications; then
    dbus-monitor --session \
        "type='method_call',interface='org.freedesktop.Notifications',member='Notify'" \
        > "$WORK/monitor.log" 2>&1 &
    MONITOR_PID=$!
    sleep 1.5
    export XDG_RUNTIME_DIR=/tmp/xdg-run
    FDK_TESTAPP_FRAMES=60 ASAN_OPTIONS=detect_leaks=0 \
        "$FDK_FILES" --smoke >> "$WORK/files.log" 2>&1 &
    FILES_PID=$!
    NOTIFY_SEEN=0
    for _ in $(seq 1 80); do
        if grep -q "member=Notify" "$WORK/monitor.log" 2>/dev/null; then
            NOTIFY_SEEN=1; break
        fi
        sleep 0.25
    done
    if [ "$NOTIFY_SEEN" = 1 ]; then
        say "PASS T3: fdk-files' notification reached the daemon (Notify witnessed on the bus)"
    else
        fail "T3 no Notify call witnessed (see $WORK/monitor.log and $WORK/files.log)"
    fi
else
    skip "T3: no notification daemon on the bus (set NOTIFYD_BIN or run a desktop)"
fi

if [ $rc -eq 0 ]; then say "fdk-files tray-bus rig: ALL PASS"; fi
exit $rc
