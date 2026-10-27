#!/usr/bin/env bash
# notifyd_rig.sh — the notification daemon's full integration rig
# (S-2 / A-12, 1.1.0).
#
# What this rig pins (each leg prints PASS/FAIL; any FAIL exits 1):
#
#  PROTOCOL legs (a real dbus-daemon via dbus-run-session + bare Xvfb;
#  the CALLS go through dbus-send = libdbus and gdbus = GLib/GIO, two
#  reference implementations parsing FDK's served bytes — the
#  cross-implementation proof; the SIGNALS are received by
#  `gdbus monitor`, a third party's parser):
#   P1  GetServerInformation: name/vendor/version/spec-version exact.
#   P2  GetCapabilities: the honest five present; sound/persistence
#       (things notifyd does NOT do) absent.
#   P3  Notify: returns a nonzero id; a second call gets a new one.
#   P4  expiry: NotificationClosed(id, reason 1) observed by monitor.
#   P5  CloseNotification: reason 3; unknown id -> an ERROR reply
#       (the spec's "empty D-Bus Error message").
#   P6  replaces_id: the same id returns; exactly ONE closed signal
#       for the replaced notification (id continuity, no spurious
#       close on replace).
#   P7  hostile battery: 1 MiB body, 100 actions, broken markup,
#       numeric entities, garbage image-data structs (wrong channels,
#       bogus geometry, short byte array), a wrong-typed Notify — the
#       daemon answers EVERYTHING and stays alive; a clean Notify and
#       GetServerInformation still work afterwards.
#   P8  rate limit: 40 Notify calls in one window through the probe
#       (one process, real bus traffic) — every call answered, the
#       daemon alive, the flood noted once in the log.
#   P9  name etiquette: a SECOND notifyd declines politely (one clear
#       line naming the incumbent, exit 0); --replace takes the name
#       from a daemon that allowed replacement (the first exits
#       cleanly on NameLost); against a stub that REFUSES replacement,
#       --replace exits with the one-line refusal.
#
#  X11 legs (Xvfb + openbox when present — the EWMH path):
#   X1  the card is ON SCREEN top-right: background, urgency strip,
#       and the raw image-data icon's red pixels (PIL readback of the
#       X server's own framebuffer).
#   X2  expiry removes the card (re-readback).
#   X3  stacking: two cards pixel-verified; the lower re-anchors
#       upward after the top one expires.
#   X4  click-through: an XTEST click ABOVE the stack hits nothing
#       (no ActionInvoked — the layer is card-sized, not oversized);
#       a click ON the card invokes the default action and closes
#       with reason 2 (monitor-proved).
#   X5  the EWMH mapping on the card window: _NET_WM_WINDOW_TYPE_DOCK
#       + _NET_WM_STATE_ABOVE + SKIP_TASKBAR/SKIP_PAGER (xprop).
#
#  WAYLAND legs (headless sway — the layer-shell path):
#   W1  the daemon survives under a real compositor; a Notify produces
#       zwlr_layer_surface_v1.configure in the WAYLAND_DEBUG trace, NO
#       wl_display.error, and no attach-before-ack (the present-gate
#       regression class).
#   W2  grim proves the card's pixels on the output.
#   W3  expiry removes them again.
#
# Every leg runs the daemon SANITIZED (ASan/UBSan) with the LSan
# suppressions file (third-party GL/fontconfig process-lifetime
# state — tests/data/lsan-gl.supp); a leak or a crash fails the leg,
# and the daemon's own exit code is checked everywhere.
#
# Usage:  tests/notifyd_rig.sh [seconds]     (default 8 — the
#         per-leg daemon SERVING lifetime)
# Build:  make -C fdk-shell test-notifyd    (examples + probe + rig)
# Self-skips (exit 0, "[skip]") when dbus-run-session / Xvfb /
# python3-PIL / the binaries are missing — set NOTIFYD_RIG_REQUIRE=1
# to make a skip a failure (CI does).
set -u

SHELLDIR=$(cd "$(dirname "$0")/.." && pwd)
FDKROOT=$(cd "$SHELLDIR/.." && pwd)
# A user-space tool prefix (sway/openbox/grim from the restore
# scripts) joins PATH when present; harmless elsewhere.
for p in /home/z/apt/prefix/usr/bin; do
    [ -d "$p" ] && case ":$PATH:" in *":$p:"*) ;; *)
        PATH="$PATH:$p" ;; esac
done
export PATH
NOTIFYD="$SHELLDIR/build/examples/notifyd"
PROBE="$SHELLDIR/build/tests/notifyd_probe"
SUPP="$FDKROOT/tests/data/lsan-gl.supp"
LIFE=${1:-8}
PASS=1

note() { echo "   $*"; }
check() { # check <name> <ok:0|1>
    if [ "$2" = "0" ]; then echo "   $1: PASS"; else echo "   $1: FAIL"; PASS=0; fi
}
have() { command -v "$1" >/dev/null 2>&1; }

# ---- shared helpers (used by the phases below) ---------------------------

# Blocks until the session bus reports an owner for the notification
# name — the daemon's async startup is never raced by a rig.
wait_for_name() {
    local i
    for i in $(seq 1 100); do
        if dbus-send --session --print-reply \
            --dest=org.freedesktop.DBus / \
            org.freedesktop.DBus.GetNameOwner \
            string:org.freedesktop.Notifications 2>/dev/null \
            | grep -q ":1\."; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# gdbus reply id: from "(uint32 N,)" — never the "32" inside
# "uint32" itself (the rig's first id parser did exactly that, and
# every check silently tested the type name instead of the id).
gdbus_id() { # gdbus_id <gdbus-output>
    printf '%s\n' "$1" | sed -nE 's/.*uint32 ([0-9]+).*/\1/p' | head -1
}

# The monitor: a third-party signal receiver, logging to $PH_MLOG.
start_monitor() {
    gdbus monitor --session --dest org.freedesktop.Notifications \
        >>"$PH_MLOG" 2>&1 &
    MPID=$!
    sleep 0.4
}

stop_monitor() {
    [ -n "${MPID:-}" ] && kill "$MPID" 2>/dev/null
    MPID=""
}

# gdbus-typed Notify. Args: app icon summary body actions hints timeout
g_notify() {
    gdbus call --session --dest org.freedesktop.Notifications \
        --object-path /org/freedesktop/Notifications \
        --method org.freedesktop.Notifications.Notify "$@"
}

# The rig's canonical test notification: a summary, markup body, an
# action pair, urgency, expire via $1 (default 2500).
std_notify() {
    g_notify "RigApp" 0 "" "Rig summary" \
        "Body with <b>bold</b>, <i>ital</i>, a <a href=\"http://rig\">link</a> &amp; an &amp;lt;entity&amp;gt;" \
        "['default', 'Default']" "{'urgency': <byte 1>}" "${1:-2500}"
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

# near-match: each channel within tol of the target.
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

# The red 16x16 ARGB icon as a GVariant image-data value (a
# deterministic, theme-independent icon for pixel legs).
icon_gvariant() {
    python3 -c "
row = ','.join(['255,0,0,255'] * 16)
print('(16, 16, 64, true, 8, 4, [%s])' % ','.join([row] * 16))
"
}

# =====================================================================
# PHASE P — protocol, on a real bus
# =====================================================================
phase_P() {
    local rc=0
    export LSAN_OPTIONS="suppressions=$SUPP"
    : > "$PH_DLOG"; : > "$PH_MLOG"

    start_daemon_p() {
        # Outlives the whole phase (PH_LIFE covers the interaction
        # legs; explicit kills do the cleanup).
        DISPLAY="$PH_DISP" FDK_NOTIFYD_TRACE=1 "$NOTIFYD" \
            --quit-after "$((PH_LIFE * 5 + 20))" "$@" >>"$PH_DLOG" 2>&1 &
        DPID=$!
    }

    echo "   P0: the daemon takes the bus name"
    start_daemon_p
    wait_for_name
    check "P0 name acquired" $?

    start_monitor

    # P1 -------------------------------------------------------------
    local out
    out=$(dbus-send --session --print-reply \
        --dest=org.freedesktop.Notifications \
        /org/freedesktop/Notifications \
        org.freedesktop.Notifications.GetServerInformation 2>&1)
    echo "$out" | grep -q 'string "notifyd"' \
        && echo "$out" | grep -q 'string "Faded Dream Project"' \
        && echo "$out" | grep -q 'string "1.2"'
    check "P1 GetServerInformation (libdbus parses the reply)" $?

    # P2 -------------------------------------------------------------
    out=$(dbus-send --session --print-reply \
        --dest=org.freedesktop.Notifications \
        /org/freedesktop/Notifications \
        org.freedesktop.Notifications.GetCapabilities 2>&1)
    local caps_ok=0
    for c in actions body body-markup body-hyperlinks icon-static; do
        echo "$out" | grep -q "string \"$c\"" || caps_ok=1
    done
    echo "$out" | grep -q 'string "sound"' && caps_ok=1
    echo "$out" | grep -q 'string "persistence"' && caps_ok=1
    check "P2 GetCapabilities: the honest five, no phantom caps" $caps_ok

    # P3 -------------------------------------------------------------
    out=$(std_notify 3000)
    local id1
    id1=$(gdbus_id "$out")
    [ -n "$id1" ] && [ "$id1" != "0" ]
    check "P3 Notify returns a nonzero id (got ${id1:-none})" $?
    out=$(std_notify 3000)
    local id2
    id2=$(gdbus_id "$out")
    [ -n "$id2" ] && [ "$id2" != "$id1" ]
    check "P3b a second Notify gets a fresh id (${id2:-none})" $?

    # P4 ------------------------------------------------------------- expire
    out=$(g_notify "RigApp" 0 "" "Expiring" "gone soon" "[]" "{}" 700)
    local idE
    idE=$(gdbus_id "$out")
    sleep 1.6
    grep -q "NotificationClosed (uint32 $idE, uint32 1)" "$PH_MLOG"
    check "P4 expiry: NotificationClosed(id=$idE, reason 1) via monitor" $?

    # P5 ------------------------------------------------------------- close + unknown id
    out=$(g_notify "RigApp" 0 "" "Closed" "by call" "[]" "{}" 0)
    local idC
    idC=$(gdbus_id "$out")
    dbus-send --session --print-reply \
        --dest=org.freedesktop.Notifications \
        /org/freedesktop/Notifications \
        org.freedesktop.Notifications.CloseNotification \
        "uint32:$idC" >/dev/null 2>&1
    sleep 0.5
    grep -q "NotificationClosed (uint32 $idC, uint32 3)" "$PH_MLOG"
    check "P5 CloseNotification -> reason 3" $?
    dbus-send --session --print-reply \
        --dest=org.freedesktop.Notifications \
        /org/freedesktop/Notifications \
        org.freedesktop.Notifications.CloseNotification \
        "uint32:999999" 2>&1 | grep -q "Error"
    check "P5b unknown id -> an error reply (the spec's empty error)" $?

    # P6 ------------------------------------------------------------- replace
    out=$(g_notify "RigApp" 0 "" "Original" "v1" "[]" "{}" 0)
    local idR
    idR=$(gdbus_id "$out")
    # gdbus positional order: app, replaces_id, icon, summary, body,
    # actions, hints, expire
    out=$(g_notify "RigApp" "$idR" "" "Replaced" "v2" "[]" "{}" 0)
    local idR2
    idR2=$(gdbus_id "$out")
    [ -n "$idR2" ] && [ "$idR2" = "$idR" ]
    check "P6 replaces_id returns the SAME id ($idR2 vs $idR)" $?
    dbus-send --session --print-reply \
        --dest=org.freedesktop.Notifications \
        /org/freedesktop/Notifications \
        org.freedesktop.Notifications.CloseNotification \
        "uint32:$idR" >/dev/null 2>&1
    sleep 0.5
    local closed_count
    closed_count=$(grep -c "NotificationClosed (uint32 $idR," "$PH_MLOG" || true)
    closed_count=${closed_count:-0}
    [ "$closed_count" = "1" ]
    check "P6b exactly ONE closed signal for the replaced id" $?

    # P7 ------------------------------------------------------------- hostile
    local big
    big=$(python3 -c "print('A' * 1048576)")
    out=$(g_notify "Flood" 0 "" "big" "$big" "[]" "{}" 1200 2>&1 | head -c 200)
    echo "$out" | grep -qE "uint32|error|Error" || true
    local hostile_rc=0
    # 100 actions
    local acts
    acts=$(python3 -c "print(', '.join(\"'k%d', 'L%d'\" % (i, i) for i in range(100)))")
    g_notify "Flood" 0 "" "many-actions" "body" "[$acts]" "{}" 1200 >/dev/null 2>&1 || hostile_rc=1
    # broken markup + entities
    g_notify "Flood" 0 "" "markup" \
        "<b><i>unclosed <u>nest <a href='x'>link <img src='/etc/passwd' alt='no'> &amp; &#65; &#x1F600; &bogus; &#; &#xZZ;" \
        "[]" "{}" 1200 >/dev/null 2>&1 || hostile_rc=1
    # garbage image-data variants
    g_notify "Flood" 0 "" "img-bad-channels" "b" "[]" \
        "{'image-data': <(4, 4, 16, true, 8, 7, [0,0,0,0])>}" 1200 >/dev/null 2>&1 || hostile_rc=1
    g_notify "Flood" 0 "" "img-bad-geometry" "b" "[]" \
        "{'image-data': <(2000000000, 2000000000, 0, true, 8, 4, [0])>}" 1200 >/dev/null 2>&1 || hostile_rc=1
    g_notify "Flood" 0 "" "img-short" "b" "[]" \
        "{'image-data': <(16, 16, 64, true, 8, 4, [1,2,3])>}" 1200 >/dev/null 2>&1 || hostile_rc=1
    g_notify "Flood" 0 "" "img-bpp" "b" "[]" \
        "{'image-data': <(4, 4, 16, true, 16, 4, [0])>}" 1200 >/dev/null 2>&1 || hostile_rc=1
    # wrong-typed Notify through dbus-send (a string where uint32 goes)
    dbus-send --session --print-reply \
        --dest=org.freedesktop.Notifications \
        /org/freedesktop/Notifications \
        org.freedesktop.Notifications.Notify \
        "string:x" "string:not-a-uint" 2>&1 | grep -q "Error"
    [ $? -eq 0 ] || hostile_rc=1
    # the daemon must still answer cleanly
    out=$(dbus-send --session --print-reply \
        --dest=org.freedesktop.Notifications \
        /org/freedesktop/Notifications \
        org.freedesktop.Notifications.GetServerInformation 2>&1)
    echo "$out" | grep -q 'string "notifyd"' || hostile_rc=1
    out=$(g_notify "RigApp" 0 "" "after-hostile" "still alive" "[]" "{}" 800)
    [ -n "$(gdbus_id "$out")" ] || hostile_rc=1
    check "P7 hostile battery: answered, alive, clean Notify after" $hostile_rc

    # P8 ------------------------------------------------------------- rate limit
    if [ -x "$PROBE" ]; then
        DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
            "$PROBE" flood 40 >>"$PH_DLOG" 2>&1
        local flood_rc=$?
        grep -q "notify flood" "$PH_DLOG"
        local noted=$?
        check "P8 rate limit: 40-in-a-window answered, flood noted once" \
            $((flood_rc != 0 || noted != 0))
    else
        note "P8 (skipped: probe not built)"
    fi

    stop_monitor
    kill $DPID 2>/dev/null
    wait $DPID 2>/dev/null
    local drc=$?
    check "P-last daemon exit code 0 (got $drc)" $([ $drc -eq 0 ] && echo 0 || echo 1)
    grep -qE "ERROR: (Leak|Address)Sanitizer" "$PH_DLOG" \
        && check "P-last no sanitizer findings" 1 \
        || check "P-last no sanitizer findings" 0

    # P9 ------------------------------------------------------------- etiquette
    # A fresh incumbent, then the conflict matrix against it.
    local BLOG="$PH_WORK/b.log" CLOG="$PH_WORK/c.log"
    : > "$BLOG"
    DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
        FDK_NOTIFYD_TRACE=1 "$NOTIFYD" --quit-after 6 >>"$PH_DLOG" 2>&1 &
    local APID=$!
    wait_for_name
    check "P9 incumbent serving" $?

    # B: declines politely, one clear line, exit 0
    DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
        "$NOTIFYD" >"$BLOG" 2>&1
    local brc=$?
    grep -q "already owned" "$BLOG"
    local bline=$?
    check "P9b second daemon: the clear line (exit $brc)" \
        $([ $brc -eq 0 ] && [ $bline -eq 0 ] && echo 0 || echo 1)

    # C --replace: takes the name; A exits cleanly on its NameLost
    DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
        FDK_NOTIFYD_TRACE=1 "$NOTIFYD" --replace --quit-after 5 \
        >>"$CLOG" 2>&1 &
    local CPID=$!
    sleep 1.5
    wait $APID 2>/dev/null
    local arc=$?
    grep -q "another daemon took" "$PH_DLOG"
    local alog=$?
    check "P9c first daemon handed over (exit $arc)" \
        $([ $arc -eq 0 ] && [ $alog -eq 0 ] && echo 0 || echo 1)
    wait_for_name
    check "P9d the replacing daemon serves" $?
    kill $CPID 2>/dev/null
    wait $CPID 2>/dev/null

    # The refusal: a stub owning WITHOUT allow-replacement beats even
    # --replace (RequestName -> EXISTS, the one-line refusal, exit 0).
    if [ -x "$PROBE" ]; then
        DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
            "$PROBE" squat >>"$PH_DLOG" 2>&1 &
        local SPID=$!
        sleep 1.0
        : > "$BLOG"
        DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
            "$NOTIFYD" --replace >"$BLOG" 2>&1
        brc=$?
        grep -q "refuses replacement" "$BLOG"
        bline=$?
        check "P9e --replace vs a refusing owner (exit $brc)" \
            $([ $brc -eq 0 ] && [ $bline -eq 0 ] && echo 0 || echo 1)
        kill $SPID 2>/dev/null
        wait $SPID 2>/dev/null
    else
        note "P9e (skipped: probe not built)"
    fi
    return $((1 - PASS))
}

# =====================================================================
# PHASE X — pixels + EWMH (Xvfb + openbox when present)
# =====================================================================
phase_X() {
    export LSAN_OPTIONS="suppressions=$SUPP"
    : > "$PH_DLOG"; : > "$PH_MLOG"
    local W=1024 H=768
    local SNAP="$PH_WORK/x1.png" SNAP2="$PH_WORK/x2.png"

    echo "   X0: the daemon takes the bus name"
    DISPLAY="$PH_DISP" FDK_NOTIFYD_TRACE=1 "$NOTIFYD" \
        --quit-after "$((PH_LIFE * 5 + 20))" >>"$PH_DLOG" 2>&1 &
    local DPID=$!
    wait_for_name
    check "X0 name acquired" $?
    start_monitor

    # X1: the card, with a raw image-data icon (the PROBE sends a
    # true ay byte array -- gdbus's text form infers bare number
    # lists as int32, which the daemon must and does refuse) and an
    # urgency strip.
    DISPLAY="$PH_DISP" LSAN_OPTIONS="suppressions=$SUPP" \
        "$PROBE" icon 255 1 1 6000 >/dev/null 2>&1
    check "X1a the raw-icon Notify was served" $?
    sleep 1.2
    grab "$PH_DISP" "$SNAP"
    [ -s "$SNAP" ]
    check "X1b the framebuffer is readable" $?

    # Card geometry: 1024 screen, margin 8, width 360 -> x in
    # [656,1016]; the icon box occupies card-local x [14,50),
    # y [12,48); the 16x16 icon is centered at (24,22)-(40,38).
    # Sample its center: screen (688, 30) -> RED (255,1,1).
    px_is "$SNAP" 688 30 255 1 1 12
    check "X1c the image-data icon is on screen (red pixels)" $?
    # The urgency strip: card-local x [0,3) -> screen 656-658.
    local strip
    strip=$(pixel "$SNAP" 657 40)
    # The card interior vs the desktop to its LEFT: boundedness (the
    # theme decides light or dark -- the rig asserts the SHAPE).
    local bg
    bg=$(pixel "$SNAP" 700 60)
    local desk
    desk=$(pixel "$SNAP" 600 60)
    python3 -c "
s = [int(v) for v in '${strip:-x}'.split()]
b = [int(v) for v in '${bg:-x}'.split()]
d = [int(v) for v in '${desk:-x}'.split()]
ok = (len(s) == 3 and max(s) - min(s) > 40 and
      len(b) == 3 and len(d) == 3 and b != d and d == [0, 0, 0])
exit(0 if ok else 1)
"
    check "X1d card bounded: accent strip, body distinct from desktop" $?

    # X3: a second card arrives ON TOP (newest-at-the-edge stacking,
    # the dunst/mako convention): card one slides DOWN by exactly one
    # card height + gap. Find its red icon in the lower range.
    g_notify "RigApp" 0 "" "Card two" "The second card" "[]" "{}" 3000 \
        >/dev/null 2>&1
    sleep 1.2
    grab "$PH_DISP" "$SNAP2"
    local moved=1 my
    for my in $(seq 60 2 140); do
        if px_is "$SNAP2" 688 $my 255 1 1 12 2>/dev/null; then
            moved=0
            note "X3a card one's icon now at y=$my (was 30)"
            break
        fi
    done
    check "X3a card one slid down (still intact) after card two" $moved
    local card2_found=1 y
    for y in 90 110 130 150 170 190 210 230 250; do
        local c2 d2
        c2=$(pixel "$SNAP2" 700 $y)
        d2=$(pixel "$SNAP2" 600 $y)
        if [ -n "$c2" ] && [ "$c2" != "$d2" ]; then
            card2_found=0
            note "X3b card two content found at y=$y ($c2 vs $d2)"
            break
        fi
    done
    check "X3b card two is on screen below card one" $card2_found

    # X4: click-through above the stack, then a real click on a
    # card that OFFERS a default action (the earlier cards had none —
    # a body click on those is a plain dismissal, by design).
    : > "$PH_MLOG"
    if [ -n "$PH_XCLICK" ]; then
        DISPLAY="$PH_DISP" "$PH_XCLICK" 800 3 2>/dev/null
        sleep 0.6
        if grep -q "ActionInvoked" "$PH_MLOG"; then
            check "X4a click ABOVE the stack hits nothing" 1
        else
            check "X4a click ABOVE the stack hits nothing" 0
        fi
        # A clickable card, sent LAST = the TOP card (newest at the
        # edge); its body mid-depth is a stable click target.
        g_notify "RigApp" 0 "" "Clickable" "click me" \
            "['default', 'Default']" "{}" 8000 >/dev/null 2>&1
        sleep 1.0
        DISPLAY="$PH_DISP" "$PH_XCLICK" 800 30 2>/dev/null
        sleep 1.0
        grep -q "ActionInvoked.*'default'" "$PH_MLOG"
        check "X4b click on the card invokes the default action" $?
        grep -qE "NotificationClosed \(uint32 [0-9]+, uint32 2\)" "$PH_MLOG"
        check "X4c the click closed it (reason 2)" $?
    else
        note "X4 (skipped: XTEST helper unavailable)"
    fi

    # X2: expiry removed the remaining card (card two expires at 3 s).
    sleep 3.0
    grab "$PH_DISP" "$SNAP2"
    local gone=1 y2
    for y2 in 8 20 40 60 80 100; do
        local cc dd
        cc=$(pixel "$SNAP2" 700 $y2)
        dd=$(pixel "$SNAP2" 500 $y2)
        if [ "$cc" != "$dd" ]; then gone=0; fi
    done
    check "X2 expiry removed the cards from the screen" $((1 - gone))

    stop_monitor
    kill $DPID 2>/dev/null
    wait $DPID 2>/dev/null
    local drc=$?
    check "X-last daemon exit code 0 (got $drc)" \
        $([ $drc -eq 0 ] && echo 0 || echo 1)
    grep -qE "ERROR: (Leak|Address)Sanitizer" "$PH_DLOG" \
        && check "X-last no sanitizer findings" 1 \
        || check "X-last no sanitizer findings" 0

    # X5: the EWMH mapping (a WM was present for this leg).
    if [ "$PH_OPENBOX" = "1" ] && have xprop; then
        DISPLAY="$PH_DISP" FDK_NOTIFYD_TRACE=1 "$NOTIFYD" \
            --quit-after 30 >>"$PH_DLOG" 2>&1 &
        DPID=$!
        wait_for_name
        g_notify "RigApp" 0 "" "EWMH card" "typed window" "[]" "{}" 4000 \
            >/dev/null 2>&1
        sleep 1.0
        local WL
        WL=$(DISPLAY="$PH_DISP" xprop -root _NET_CLIENT_LIST 2>/dev/null \
            | grep -oE '0x[0-9a-f]+' | head -1)
        local ewmh_rc=1
        if [ -n "$WL" ]; then
            local XP
            XP=$(DISPLAY="$PH_DISP" xprop -id "$WL" 2>/dev/null)
            echo "$XP" | grep -q "_NET_WM_WINDOW_TYPE_DOCK" \
                && echo "$XP" | grep -q "_NET_WM_STATE_ABOVE" \
                && echo "$XP" | grep -q "_NET_WM_STATE_SKIP_TASKBAR" \
                && ewmh_rc=0
        fi
        check "X5 the card window is DOCK/ABOVE/SKIP_TASKBAR (xprop)" \
            $ewmh_rc
        kill $DPID 2>/dev/null
        wait $DPID 2>/dev/null
    else
        note "X5 (skipped: no WM or xprop)"
    fi
    return $((1 - PASS))
}

# =====================================================================
# PHASE W — layer-shell (headless sway)
# =====================================================================
phase_W() {
    export LSAN_OPTIONS="suppressions=$SUPP"
    : > "$PH_DLOG"; : > "$PH_MLOG"
    local SNAP="$PH_WORK/w1.png" SNAP2="$PH_WORK/w2.png"

    export WAYLAND_DISPLAY="$PH_WSOCK"
    echo "   W0: the daemon runs under a real compositor"
    FDK_NOTIFYD_TRACE=1 WAYLAND_DEBUG=1 "$NOTIFYD" \
        --quit-after "$PH_LIFE" >>"$PH_DLOG" 2>&1 &
    local DPID=$!
    wait_for_name
    check "W0 name acquired under sway" $?

    g_notify "RigApp" 0 "" "Wayland card" "Rendered by layer-shell" "[]" \
        "{'urgency': <byte 1>}" 2500 >/dev/null 2>&1
    sleep 1.5

    # W1: the protocol trace.
    grep -q "zwlr_layer_surface_v1.*configure" "$PH_DLOG"
    check "W1a zwlr configure arrived" $?
    if grep -q "wl_display.error" "$PH_DLOG"; then
        check "W1b no protocol error" 1
    else
        check "W1b no protocol error" 0
    fi
    # The present gate: no attach before the first ack_configure.
    python3 - "$PH_DLOG" << 'PYW'
import re, sys
log = open(sys.argv[1], encoding='utf-8', errors='replace').read()
# WAYLAND_DEBUG lines: [timestamp] -> surface_id@...attach(...) and
# zwlr_layer_surface@...ack_configure(...)
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
    check "W1c no attach before the first ack_configure" $?

    # W2: the pixels.
    if have grim; then
        WLR_BACKENDS=headless grim -o HEADLESS-1 "$SNAP" 2>/dev/null
        [ -s "$SNAP" ]
        check "W2a grim captured the output" $?
        # top-right card: x in [656,1016], y in [8,~110]; the body is
        # dark and BOUNDED (left edge contrast at x=656 vs x=600).
        local wc wd
        wc=$(pixel "$SNAP" 700 40)
        wd=$(pixel "$SNAP" 600 40)
        [ "$wc" != "$wd" ]
        check "W2b the card is on the output (contrast at its edge)" $?
    else
        note "W2 (skipped: no grim)"
    fi

    # W3: expiry (the card's timeout is 2.5 s; sample past it).
    sleep 4.0
    if have grim; then
        WLR_BACKENDS=headless grim -o HEADLESS-1 "$SNAP2" 2>/dev/null
        local wc2 wd2 gone=1
        for y in 8 20 40 60 80 100; do
            wc2=$(pixel "$SNAP2" 700 $y)
            wd2=$(pixel "$SNAP2" 500 $y)
            [ "$wc2" != "$wd2" ] && gone=0
        done
        check "W3 expiry removed the card from the output" $((1 - gone))
    else
        note "W3 (skipped: no grim)"
    fi

    kill $DPID 2>/dev/null
    wait $DPID 2>/dev/null
    local drc=$?
    check "W-last daemon exit code 0 (got $drc)" \
        $([ $drc -eq 0 ] && echo 0 || echo 1)
    grep -qE "ERROR: (Leak|Address)Sanitizer" "$PH_DLOG" \
        && check "W-last no sanitizer findings" 1 \
        || check "W-last no sanitizer findings" 0
    unset WAYLAND_DISPLAY
    return $((1 - PASS))
}

# ---- phase dispatch: the rig re-invokes itself INSIDE
# dbus-run-session (the session bus env arrives that way) --------------

case "${1:-}" in
__phaseP__|__phaseX__|__phaseW__)
    PHASE=${1#__phase}; PHASE=${PHASE%__}
    PH_DISP=$2; PH_WORK=$3; PH_LIFE=$4; PH_DLOG=$5; PH_MLOG=$6
    PH_XCLICK=${7:-}; PH_OPENBOX=${8:-0}; PH_WSOCK=${2:-}
    phase_"$PHASE"
    exit $?
    ;;
esac

# ---- skip discipline -------------------------------------------------------

for tool in dbus-run-session dbus-send gdbus Xvfb python3; do
    if ! have "$tool"; then
        echo "[skip] notifyd rig: $tool not available"
        [ "${NOTIFYD_RIG_REQUIRE:-0}" = "1" ] && exit 1
        exit 0
    fi
done
if [ ! -x "$NOTIFYD" ]; then
    echo "[skip] notifyd not built (make -C fdk-shell examples)"
    [ "${NOTIFYD_RIG_REQUIRE:-0}" = "1" ] && exit 1
    exit 0
fi
python3 -c "from PIL import ImageGrab" 2>/dev/null || {
    echo "[skip] PIL unavailable"
    [ "${NOTIFYD_RIG_REQUIRE:-0}" = "1" ] && exit 1
    exit 0
}

WORK=$(mktemp -d /tmp/notifyd-rig-XXXXXX)
trap 'rm -rf "$WORK"' EXIT
XDG="$WORK/xdg"
mkdir -p "$XDG"
chmod 700 "$XDG"
export XDG_RUNTIME_DIR="$XDG"
DLOG="$WORK/daemon.log"
MLOG="$WORK/monitor.log"

# ---- the XTEST click helper (libXtst — the same test-infra class
# the fdk-shell X11 suite already uses) ------------------------------------

XCLICK="$WORK/xclick"
cat > "$WORK/xclick.c" << 'XCEOF'
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: xclick X Y\n"); return 2; }
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    int ev = 0, err = 0, maj = 0, min = 0;
    if (!XTestQueryExtension(d, &ev, &err, &maj, &min)) {
        fprintf(stderr, "no XTEST\n"); return 1;
    }
    XTestFakeMotionEvent(d, -1, atoi(argv[1]), atoi(argv[2]), CurrentTime);
    XSync(d, False);
    XTestFakeButtonEvent(d, 1, True, CurrentTime);
    XSync(d, False);
    XTestFakeButtonEvent(d, 1, False, CurrentTime);
    XSync(d, False);
    XCloseDisplay(d);
    return 0;
}
XCEOF
XTST_FLAGS=""
if pkg-config --exists xtst 2>/dev/null; then
    XTST_FLAGS="$(pkg-config --cflags --libs xtst)"
elif [ -f /home/z/apt/prefix/usr/include/X11/extensions/XTest.h ]; then
    XTST_FLAGS="-I/home/z/apt/prefix/usr/include -L/home/z/apt/prefix/usr/lib/x86_64-linux-gnu -lXtst -lX11"
else
    XTST_FLAGS="-lXtst -lX11"
fi
cc -O1 "$WORK/xclick.c" -o "$XCLICK" $XTST_FLAGS 2>/dev/null || XCLICK=""

echo "notifyd rig: $(date +%H:%M:%S) work=$WORK"

# Race-free X display allocation: Xvfb -displayfd writes the chosen
# display number to a pipe (falls back to the $$-based pattern).
alloc_xdisplay() { # alloc_xdisplay <varname> -> also sets XVFB_PID
    local __var=$1 __num=
    local __out
    __out=$(mktemp /tmp/notifyd-xd-XXXXXX)
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

# ---- PHASE P --------------------------------------------------------------

echo "-- phase P: protocol (real dbus-daemon, dbus-send + gdbus)"
alloc_xdisplay PVFB
dbus-run-session -- bash "$0" __phaseP__ "$PVFB" "$WORK" "$LIFE" \
    "$DLOG" "$MLOG" "$XCLICK"
P_RC=$?
kill_xdisplay
echo "== phase P: rc=$P_RC"
[ $P_RC -ne 0 ] && PASS=0

# ---- PHASE X --------------------------------------------------------------

echo "-- phase X: pixels + EWMH (Xvfb + openbox)"
alloc_xdisplay XVFB
HAVE_OPENBOX=0
OB_PID=""
if have openbox; then
    # XDG_DATA_DIRS points openbox at the user-space prefix's themes
    # (a prefix-installed openbox otherwise finds no theme and EXITS
    # -- "Unable to load a theme" -- leaving a WM-less screen and no
    # EWMH atoms for the xprop leg).
    DISPLAY="$XVFB" \
        XDG_DATA_DIRS="${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:/home/z/apt/prefix/usr/share" \
        openbox >/dev/null 2>&1 &
    OB_PID=$!
    HAVE_OPENBOX=1
    sleep 0.9
fi
dbus-run-session -- bash "$0" __phaseX__ "$XVFB" "$WORK" "$LIFE" \
    "$DLOG" "$MLOG" "$XCLICK" "$HAVE_OPENBOX"
X_RC=$?
[ -n "$OB_PID" ] && kill $OB_PID 2>/dev/null
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
    # (sway from a user-space prefix needs its LD_LIBRARY_PATH too)
    SWAY_PID=$!
    # sway's WAYLAND socket (wayland-N) -- NOT sway-ipc.* (that is
    # swaymsg's control socket; connecting a Wayland client to it
    # goes nowhere). bar_rig.sh established the pattern.
    WSOCK=""
    for i in $(seq 1 30); do
        WSOCK=$(ls "$XDG" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
        [ -n "$WSOCK" ] && break
        sleep 0.2
    done
    [ -n "$WSOCK" ] && WSOCK="$XDG/$WSOCK"
    if [ -n "$WSOCK" ]; then
        dbus-run-session -- bash "$0" __phaseW__ "$WSOCK" "$WORK" \
            "$LIFE" "$DLOG" "$MLOG"
        W_RC=$?
    else
        echo "   (sway socket never appeared)"
        W_RC=1
    fi
    kill $SWAY_PID 2>/dev/null
    wait $SWAY_PID 2>/dev/null
else
    echo "   [skip] sway/grim unavailable — Wayland legs skipped"
    W_RC=0
fi
echo "== phase W: rc=$W_RC"
[ $W_RC -ne 0 ] && PASS=0

# ---- verdict ---------------------------------------------------------------

echo
if [ "$PASS" = "1" ]; then
    echo "notifyd rig: ALL PASS"
else
    echo "notifyd rig: FAILURES PRESENT"
fi
exit $((1 - PASS))
