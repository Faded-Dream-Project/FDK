#!/usr/bin/env bash
# labwc_smoke.sh — run the full smoke matrix (all nine apps) against a
# headless labwc compositor, the same way test-apps-smoke runs them
# against Xvfb. Proves the Wayland backend end to end on a SECOND,
# independent wlroots compositor (sway was the first).
#
# Usage: scripts/labwc_smoke.sh [--keep]   (--keep leaves labwc up)
set -u
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT_DIR/scripts/env.sh"

WL_BIN=/home/z/apt/prefix/usr/bin
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/home/z/apt/xdg-run}"
export XDG_RUNTIME_DIR
export PATH="$WL_BIN:$PATH"
export LD_LIBRARY_PATH="/home/z/apt/prefix/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"
export WAYLAND_DISPLAY=wayland-0
export LSAN_OPTIONS="suppressions=$ROOT_DIR/tests/data/lsan-gl.supp"
export ASAN_OPTIONS=detect_leaks=0

LABWC_CONF=/home/z/labwc-conf
mkdir -p "$LABWC_CONF"
if [ ! -f "$LABWC_CONF/rc.xml" ]; then
    cat > "$LABWC_CONF/rc.xml" <<'EOF'
<?xml version="1.0"?>
<labwc_config>
  <core>
    <xwayland>no</xwayland>
  </core>
</labwc_config>
EOF
fi

# labwc up? (the socket in XDG_RUNTIME_DIR is the truth)
if [ ! -S "$XDG_RUNTIME_DIR/wayland-0" ]; then
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
        WLR_HEADLESS_OUTPUTS=1 \
        WLR_XWAYLAND=/home/z/apt/prefix/usr/bin/Xwayland \
        labwc -C "$LABWC_CONF" > /tmp/labwc_smoke.log 2>&1 &
    LABWC_PID=$!
    for i in $(seq 1 30); do
        [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && break
        sleep 0.2
    done
fi

BUILD="$ROOT_DIR/test-apps/build"
SMOKE_APPS="fdk-files fdk-monitor fdk-notes fdk-edit fdk-term \
            fdk-chat fdk-addon-host fdk-stress fdk-workbench"
rc=0
for app in $SMOKE_APPS; do
    if [ ! -x "$BUILD/$app" ] && [ "$app" = fdk-addon-host ]; then
        echo "=== smoke: $app NOT BUILT (make addon first)"; rc=1; continue
    fi
    echo "=== smoke (wayland/labwc): $app"
    extra=""
    [ "$app" = fdk-addon-host ] && extra="FDK_HUD_LIB=$BUILD/libfdk-hud.so"
    # shellcheck disable=SC2086
    env $extra FDK_TESTAPP_FRAMES=40 "$BUILD/$app" --smoke \
        > "/tmp/labwc_smoke_$app.log" 2>&1 || rc=1
    grep -h "receipt\|PASS\|FAIL" "/tmp/labwc_smoke_$app.log" | tail -2
done

if [ $rc -eq 0 ]; then
    echo "== labwc smoke: ALL APPS PASSED"
else
    echo "== labwc smoke: FAILURES (rc=$rc)"
fi
if [ "${1:-}" != "--keep" ]; then
    pkill -f "labwc -C $LABWC_CONF" 2>/dev/null || true
fi
exit $rc
