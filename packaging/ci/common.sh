#!/bin/sh
# packaging/ci/common.sh — shared helpers for the per-distro packaging
# scripts. POSIX sh: runs under bash, dash, busybox ash (Alpine), and
# the Void image's sh alike. Sourced, never executed directly.
#
# Environment contract (set by the caller before sourcing):
#   The current working directory is the FDK repository root
#   (the CI orchestrators docker-run with -w /work).
#
# Optional overrides for extraction-based verification (no root):
#   FDK_PC_DIR    — directory containing fdk.pc (replaces system search)
#   FDK_LIB_DIR   — directory containing libfdk.so.1 (LD_LIBRARY_PATH)
#   FDK_DATA_DIR  — directory containing fdk/.FDKThemes (XDG override)
#   FDK_INC_DIR   — directory containing fdk/*.h (wins over the .pc's
#                   baked /usr/include, which points at the real system)

# The expected FDK version, read from the single source of truth.
fdk_expected_version() {
    sed -n 's/^#define FDK_VERSION_STRING "\(.*\)"/\1/p' \
        include/fdk/fdk_version.h
}

# Starts a private Xvfb and exports DISPLAY. Prints XVFB-SKIP and
# returns 1 (without failing the caller) when no Xvfb exists — the
# callers disclose the skip in their output. The X server is killed
# by an EXIT trap.
fdk_start_xvfb() {
    if ! command -v Xvfb >/dev/null 2>&1; then
        echo "XVFB-SKIP: no Xvfb in this environment; graphical tests skipped (disclosed)"
        return 1
    fi
    FDK_XVFB_N=$((100 + $$ % 800))
    FDK_XVFB_DISP=":$FDK_XVFB_N"
    rm -f "/tmp/.X11-unix/X$FDK_XVFB_N" 2>/dev/null || true
    Xvfb "$FDK_XVFB_DISP" -screen 0 1024x768x24 >/tmp/fdk-smoke-xvfb.log 2>&1 &
    FDK_XVFB_PID=$!
    i=0
    while [ $i -lt 40 ]; do
        if [ -S "/tmp/.X11-unix/X$FDK_XVFB_N" ]; then
            break
        fi
        sleep 0.25
        i=$((i + 1))
    done
    if [ ! -S "/tmp/.X11-unix/X$FDK_XVFB_N" ]; then
        echo "XVFB-FAIL: Xvfb did not come up (see /tmp/fdk-smoke-xvfb.log)"
        kill "$FDK_XVFB_PID" 2>/dev/null || true
        return 1
    fi
    DISPLAY="$FDK_XVFB_DISP"
    export DISPLAY
    echo "XVFB: private display $FDK_XVFB_DISP (pid $FDK_XVFB_PID)"
    return 0
}

fdk_stop_xvfb() {
    if [ -n "${FDK_XVFB_PID:-}" ]; then
        kill "$FDK_XVFB_PID" 2>/dev/null || true
        wait "$FDK_XVFB_PID" 2>/dev/null || true
        rm -f "/tmp/.X11-unix/X$FDK_XVFB_N" 2>/dev/null || true
    fi
}

# Compiles and runs the smoke app against the INSTALLED (or extracted)
# FDK. $1 = expected version. Requires cc + pkg-config. Prints
# SMOKE-* lines from the app; returns its exit status.
fdk_run_smoke() {
    expected="$1"
    cc=""
    for c in cc gcc clang; do
        if command -v "$c" >/dev/null 2>&1; then
            cc="$c"
            break
        fi
    done
    if [ -z "$cc" ]; then
        echo "SMOKE-SKIP: no C compiler available"
        return 1
    fi
    pc_env=""
    ld_env=""
    xdg_env=""
    extra_ld=""
    extra_inc=""
    if [ -n "${FDK_PC_DIR:-}" ]; then
        pc_env="PKG_CONFIG_PATH=$FDK_PC_DIR"
    fi
    if [ -n "${FDK_LIB_DIR:-}" ]; then
        ld_env="LD_LIBRARY_PATH=$FDK_LIB_DIR"
        extra_ld="-L$FDK_LIB_DIR"
    fi
    if [ -n "${FDK_DATA_DIR:-}" ]; then
        xdg_env="XDG_DATA_DIRS=$FDK_DATA_DIR"
    fi
    if [ -n "${FDK_INC_DIR:-}" ]; then
        extra_inc="-I$FDK_INC_DIR"
    fi
    # shellcheck disable=SC2086
    env $pc_env $ld_env $xdg_env "$cc" packaging/common/smoke.c \
        -o /tmp/fdk_package_smoke $extra_inc $extra_ld \
        $(env $pc_env pkg-config --cflags --libs fdk) || {
        echo "SMOKE FAIL: compile against installed FDK (pkg-config: $(env $pc_env pkg-config --modversion fdk 2>&1 || echo unavailable))"
        return 1
    }
    # shellcheck disable=SC2086
    env $ld_env $xdg_env /tmp/fdk_package_smoke "$expected"
}
