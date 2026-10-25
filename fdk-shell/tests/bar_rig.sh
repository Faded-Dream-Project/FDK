#!/usr/bin/env bash
# bar_rig.sh — fdk-shell's REAL-COMPOSITOR bar integration rig
# (1.1.0-rc1: the live-desktop findings' permanent tests).
#
# What this rig pins (each leg prints PASS/FAIL and the script exits
# non-zero on any FAIL):
#
#  A3/A4 "alive" leg: the REAL examples/bar binary runs against a
#  headless wlroots compositor (sway, plus labwc when runnable —
#  the compositor the live report came from) for several seconds:
#    - the process is still alive at +2.5s (the live report's bar
#      DIED at startup: it attached+committed a buffer before the
#      layer surface's first configure and the compositor killed
#      the connection);
#    - the layer surface received its configure (the WAYLAND_DEBUG
#      trace must contain zwlr_layer_surface_v1.configure);
#    - NO protocol error occurred (no wl_display.error in the trace,
#      compositor log free of layer-shell errors);
#    - the WAYLAND_DEBUG trace asserts no wl_surface.attach happens
#      before the first zwlr_layer_surface_v1.ack_configure (the
#      protocol-order regression test, compositor-enforcement or
#      not);
#    - when grim is available, the bar's pixels are on the output
#      (the compositor-assigned anchoring actually rendered).
#
#  A2 "fatal dispatch" leg (a SECOND compositor instance so the first
#  leg's state cannot interfere): the bar runs against its own
#  compositor; the compositor is KILLED; the bar must (a) exit by
#  itself within 5s, (b) exit NON-ZERO, (c) log the fatal dispatch
#  exactly once (the live report's loop logged it 2.2 MILLION times
#  while spinning at 100% CPU), and (d) produce a bounded log.
#
# Usage: tests/bar_rig.sh [seconds]        (default 4)
# Self-skips (exit 0, "[skip]") when no runnable compositor exists.
set -u
SHELLDIR=$(cd "$(dirname "$0")/.." && pwd)
SECS=${1:-4}
XDG=${XDG_RUNTIME_DIR:-/tmp/fdkshell-bar-rig}
mkdir -p "$XDG" 2>/dev/null && chmod 700 "$XDG" 2>/dev/null || true
export XDG_RUNTIME_DIR=$XDG
BAR="$SHELLDIR/build/examples/bar"
PASS=1

note() { echo "   $*"; }
check() { # check <name> <ok:0|1>
    if [ "$2" = "0" ]; then echo "   $1: PASS"; else echo "   $1: FAIL"; PASS=0; fi
}

[ -x "$BAR" ] || { echo "[skip] bar example not built (make examples)"; exit 0; }

# ---- compositor helper ------------------------------------------------------
# start_compositor <sway|labwc> <socket-glob-exclude>
start_compositor() {
    local kind=$1
    export WLR_BACKENDS=headless WLR_RENDERER=pixman
    export WLR_LIBINPUT_NO_DEVICES=1
    if [ "$kind" = sway ]; then
        local conf="$XDG/bar-rig-$$.conf"
        printf 'output HEADLESS-1 mode 1024x768\n' > "$conf"
        sway -c "$conf" >/tmp/fdkshell-bar-sway-$$.log 2>&1 &
    else
        # labwc hard-requires an Xwayland path in this build; WLR_XWAYLAND
        # points at one when it exists (searched on PATH), else the leg
        # self-skips.
        local xw=""
        xw=$(command -v Xwayland 2>/dev/null || true)
        if [ -z "$xw" ]; then
            return 2
        fi
        export WLR_XWAYLAND=$xw
        labwc -s true >/tmp/fdkshell-bar-labwc-$$.log 2>&1 &
    fi
    COMP_PID=$!
    SOCK=""
    local i
    for i in $(seq 1 40); do
        SOCK=$(ls "$XDG" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1 || true)
        [ -n "$SOCK" ] && break
        sleep 0.25
    done
    [ -n "$SOCK" ] || return 1
    return 0
}

alive_leg() {
    local kind=$1
    local label="${kind}-alive"
    echo "== A3/A4: the bar vs a real $kind for ${SECS}s =="
    if ! command -v "$kind" >/dev/null 2>&1; then
        echo "[skip] no $kind on PATH"
        return 0
    fi
    rm -f "$XDG"/wayland-* 2>/dev/null
    start_compositor "$kind"
    local rc=$?
    if [ "$rc" = "2" ]; then
        echo "[skip] $kind present but no Xwayland for this build"
        return 0
    fi
    if [ "$rc" != "0" ]; then
        echo "[skip] $kind failed to start headless"
        return 0
    fi
    note "compositor socket: $SOCK (pid $COMP_PID)"

    WAYLAND_DISPLAY="$SOCK" WAYLAND_DEBUG=1 timeout $(( SECS + 8 )) \
        "$BAR" $SECS >/tmp/fdkshell-bar-out-$$.log 2>/tmp/fdkshell-bar-trace-$$.log &
    APP_PID=$!

    # Alive at +2.5s (startup death happens in the first round trip).
    sleep 2.5
    if kill -0 "$APP_PID" 2>/dev/null; then
        check "$label: alive at +2.5s" 0
    else
        check "$label: alive at +2.5s" 1
        note "early exit; log tail:"
        tail -5 /tmp/fdkshell-bar-out-$$.log | sed 's/^/     /'
    fi

    # Pixel check (grim optional).
    if command -v grim >/dev/null 2>&1; then
        WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR=$XDG \
            grim -o HEADLESS-1 /tmp/fdkshell-bar-shot-$$.png 2>/dev/null || \
            WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR=$XDG \
            grim /tmp/fdkshell-bar-shot-$$.png 2>/dev/null
        if [ -f /tmp/fdkshell-bar-shot-$$.png ]; then
            python3 - "/tmp/fdkshell-bar-shot-$$.png" << 'PYEOF'
import sys
from PIL import Image
from collections import Counter
im = Image.open(sys.argv[1]).convert("RGB")
def hexc(c): return "%06x" % (c[0]<<16 | c[1]<<8 | c[2])
w = im.size[0]
band = im.crop((0, 0, w, 32))  # the bar is exactly 32 logical px tall
cnt = Counter(band.getdata())
dom = cnt.most_common(1)
ok = dom and hexc(dom[0][0]) == "212630"
print("   pixels: bar-region dominant", dom and hexc(dom[0][0]),
      "(want 212630)")
sys.exit(0 if ok else 1)
PYEOF
            check "$label: bar pixels on the output" $?
        fi
    fi

    # Trace assertions: configure arrived, no protocol error, and no
    # attach before the first ack_configure.
    python3 - "/tmp/fdkshell-bar-trace-$$.log" << 'PYEOF'
import re, sys
attaches, acks, configures, errors = [], [], [], []
for i, line in enumerate(open(sys.argv[1], errors='replace')):
    if re.search(r'wl_surface[@#]\d+\.attach\(', line):
        attaches.append(i)
    if re.search(r'zwlr_layer_surface_v1[@#]\d+\.ack_configure\(', line):
        acks.append(i)
    if re.search(r'zwlr_layer_surface_v1[@#]\d+\.configure\(', line):
        configures.append(i)
    if re.search(r'wl_display[@#]\d+\.error\(', line):
        errors.append(line.strip()[:160])
fail = []
if not configures:
    fail.append("no zwlr configure in the trace")
if errors:
    fail.append("wl_display.error in the trace: " + "; ".join(errors[:2]))
if attaches and not acks:
    fail.append("attach with no ack_configure")
elif attaches and acks[0] > attaches[0]:
    fail.append("wl_surface.attach BEFORE the first ack_configure "
                "(line %d vs %d)" % (attaches[0], acks[0]))
for f in fail:
    print("   trace-fail:", f)
sys.exit(1 if fail else 0)
PYEOF
    check "$label: configure arrived, no protocol error, attach-after-ack" $?

    wait "$APP_PID" 2>/dev/null
    local arc=$?
    [ "$arc" = "0" ] || check "$label: clean exit 0" 1

    # Compositor log: no layer-shell protocol errors against us.
    if ! grep -qiE "layer_surface.*never been configured|zwlr.*error" \
        /tmp/fdkshell-bar-$kind-$$.log 2>/dev/null; then
        check "$label: compositor log free of layer-shell errors" 0
    else
        check "$label: compositor log free of layer-shell errors" 1
    fi

    kill "$COMP_PID" 2>/dev/null; wait "$COMP_PID" 2>/dev/null
    rm -f "$XDG/$SOCK" "$XDG/$SOCK.lock" 2>/dev/null
    return 0
}

fatal_leg() {
    local kind=${1:-sway}
    echo "== A2: compositor death mid-run (the fatal-dispatch contract) =="
    if ! command -v "$kind" >/dev/null 2>&1; then
        echo "[skip] no $kind on PATH"
        return 0
    fi
    rm -f "$XDG"/wayland-* 2>/dev/null
    start_compositor "$kind"
    if [ $? != "0" ]; then
        echo "[skip] $kind failed to start headless"
        return 0
    fi

    WAYLAND_DISPLAY="$SOCK" "$BAR" 60 \
        >/tmp/fdkshell-bar2-out-$$.log 2>&1 &
    APP_PID=$!
    sleep 2.5
    note "killing the compositor (pid $COMP_PID)..."
    kill "$COMP_PID" 2>/dev/null

    local t0=$(date +%s%N)
    local dead=0
    while [ $(( ($(date +%s%N) - t0) / 1000000000 )) -lt 5 ]; do
        kill -0 "$APP_PID" 2>/dev/null || { dead=1; break; }
        sleep 0.05
    done
    local arc=0
    [ "$dead" = "1" ] && wait "$APP_PID" 2>/dev/null || arc=$?
    kill "$APP_PID" 2>/dev/null; wait "$APP_PID" 2>/dev/null; arc=$?
    local elapsed_ms=$(( ($(date +%s%N) - t0) / 1000000 ))
    local fatal_lines=$(grep -c "dispatch_pending failed" \
        /tmp/fdkshell-bar2-out-$$.log || true)
    local total_lines=$(wc -l < /tmp/fdkshell-bar2-out-$$.log)

    note "exit after ~${elapsed_ms}ms (self-terminated: $dead, rc=$arc)"
    note "log: $total_lines lines, $fatal_lines fatal-report lines"
    [ "$dead" = "1" ] && check "A2: the loop stopped by itself" 0 \
                        || check "A2: the loop stopped by itself" 1
    [ "$arc" != "0" ] && check "A2: exit code non-zero" 0 \
                      || check "A2: exit code non-zero" 1
    if [ "$fatal_lines" -ge 1 ] && [ "$fatal_lines" -le 2 ]; then
        check "A2: the fatal error logged once" 0
    else
        check "A2: the fatal error logged once" 1
    fi
    if [ "$total_lines" -lt 40 ]; then
        check "A2: log bounded (no busy-loop spam)" 0
    else
        check "A2: log bounded (no busy-loop spam)" 1
    fi
    kill "$COMP_PID" 2>/dev/null; wait "$COMP_PID" 2>/dev/null
    rm -f "$XDG/$SOCK" "$XDG/$SOCK.lock" 2>/dev/null
    return 0
}

COMP_PID="" APP_PID=""
cleanup() {
    [ -n "${APP_PID:-}" ] && kill "$APP_PID" 2>/dev/null
    [ -n "${COMP_PID:-}" ] && kill "$COMP_PID" 2>/dev/null
    wait 2>/dev/null
    rm -f "$XDG"/wayland-* /tmp/fdkshell-bar-*$$* 2>/dev/null
    return 0
}
trap cleanup EXIT

alive_leg sway
alive_leg labwc
fatal_leg sway

if [ "$PASS" = "1" ]; then
    echo "bar rig: ALL PASS"
    exit 0
fi
echo "bar rig: FAILURES ABOVE"
exit 1
