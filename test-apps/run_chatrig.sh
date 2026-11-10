#!/usr/bin/env bash
# run_chatrig.sh — the fdk-chat end-to-end rig: the mock ircd, the
# real client under Xvfb (ASan build), and the dump assertions:
# connect, auto-join, NAMES -> members, the styled message, the
# highlight, the PM conversation, and the storm (the streaming
# append stress) — with a wall-clock budget for the storm window.
#
# Usage: run_chatrig.sh [STORM_N]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
STORM="${1:-2000}"
PORT="${FDK_CHATRIG_PORT:-17777}"
DISP=":9$((RANDOM % 80 + 10))"
FAIL=0

say() { printf '%s\n' "$*"; }
check() { # check <desc> <cmd...>
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then
        say "[ok] $desc"
    else
        say "[FAIL] $desc"
        FAIL=1
    fi
}

rm -rf /tmp/fdk-chatrig && mkdir -p /tmp/fdk-chatrig

Xvfb "$DISP" -screen 0 1100x760x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1

python3 test-apps/fdk-chat/ircd_rig.py "$PORT" "$STORM" &
IRCD_PID=$!
sleep 0.6

STORM_START=$(date +%s.%N)
FDK_CHAT_HOST=121.0.0.1 FDK_CHAT_PORT="$PORT" FDK_CHAT_NICK=dreamer \
FDK_CHAT_JOIN="#rig" FDK_TESTAPP_FRAMES=260 DISPLAY="$DISP" \
ASAN_OPTIONS=detect_leaks=1 \
    ./test-apps/build/fdk-chat > /tmp/fdk-chatrig/client.log 2>&1
CLIENT_RC=$?
STORM_END=$(date +%s.%N)
STORM_SECS=$(awk -v a="$STORM_START" -v b="$STORM_END" \
    'BEGIN{printf "%.2f", b - a}')

kill "$IRCD_PID" 2>/dev/null
kill "$XVFB_PID" 2>/dev/null
wait "$IRCD_PID" 2>/dev/null
wait "$XVFB_PID" 2>/dev/null

DUMP=$(sed -n '/=== CHAT DUMP BEGIN ===/,/=== CHAT DUMP END ===/p' \
    /tmp/fdk-chatrig/client.log)
MEMBERS=$(sed -n 's/=== USERS \([0-9]*\) ===/\1/p' \
    /tmp/fdk-chatrig/client.log)
MSGS=$(sed -n 's/=== MSGS \([0-9]*\) ===/\1/p' \
    /tmp/fdk-chatrig/client.log)

say "== fdk-chat rig (storm=$STORM, wall $STORM_SECS s, rc $CLIENT_RC)"
check "client exited cleanly" test "$CLIENT_RC" -eq 0
check "dump produced" test -n "$DUMP"
check "welcome numeric arrived" grep -q "Welcome to the FDK chat rig" /tmp/fdk-chatrig/client.log
check "topic set (332)" grep -q "The FDK chat rig topic" /tmp/fdk-chatrig/client.log
check "styled message body decoded (no 0x03 left)" \
    ! grep -q $'\x03' <<<"$DUMP"
check "url preserved in the log" grep -q "https://example.com/x" /tmp/fdk-chatrig/client.log
check "highlight body present" grep -q "hey dreamer, look at this" /tmp/fdk-chatrig/client.log
check "PM conversation created" grep -q "psst, private hello" /tmp/fdk-chatrig/client.log
check "members from NAMES (5)" test "$MEMBERS" = "5"
check "every storm message landed" \
    test "$(grep -c 'storm message' <<<"$DUMP")" -eq "$STORM"
check "storm within 10 s (streaming append stayed alive)" \
    awk -v s="$STORM_SECS" 'BEGIN{exit !(s < 10.0)}'
if [ "$FAIL" -eq 0 ]; then
    say "VERDICT PASS"
else
    say "VERDICT FAIL — client.log in /tmp/fdk-chatrig/"
fi
exit "$FAIL"
