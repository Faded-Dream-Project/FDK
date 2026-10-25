#!/bin/sh
# fdk-shell/packaging/ci/common.sh — shared helpers for fdk-shell's
# per-distro packaging scripts. POSIX sh, sourced (never executed).
#
# Sources FDK's own packaging/ci/common.sh (version truth, the
# per-family pre-release spellings, the Xvfb/smoke machinery) and
# adds the fdk-shell layer on top: this package's version, the FDK
# runtime package NAME per family, and the minimum FDK version
# fdk-shell's dependency metadata carries.
#
# Environment contract: the working directory is the FDK repository
# root (fdk-shell lives at ./fdk-shell inside it — the same tree,
# its own package; see fdk-shell/README.md's repository note).

# FDK's helpers (fdk_expected_version, fdk_packver, fdk_sover,
# fdk_require_release_posture, fdk_start_xvfb, fdk_run_smoke, ...).
. "$(dirname "$0")/../../../packaging/ci/common.sh"

# fdk-shell's own version — single source of truth is its header.
fdkshell_version() {
    sed -n 's/^#define FDKSHELL_VERSION_STRING "\(.*\)"/\1/p' \
        fdk-shell/include/fdk-shell/fdk-shell.h
}

# The FDK RUNTIME package name per distro family — the package this
# add-on's runtime Depends/Requires/depends points at. These are the
# names FDK's own packaging produces (packaging/<family>/...): the
# shared library lands in libfdk1 / libfdk1 / fdk / fdk / fdk-libs
# for deb / rpm / arch / apk / xbps respectively.
fdkshell_fdk_runtime_pkg() {
    case "$1" in
    deb|rpm) echo "libfdk1" ;;
    arch)    echo "fdk" ;;
    apk)     echo "fdk" ;;
    xbps)    echo "fdk-libs" ;;
    *)       echo "fdk" ;;
    esac
}

# The FDK DEVELOPMENT package name per family (the dev package's own
# dependency target).
fdkshell_fdk_dev_pkg() {
    case "$1" in
    deb|rpm) echo "fdk-dev" ;;
    arch)    echo "fdk" ;;
    apk)     echo "fdk-dev" ;;
    xbps)    echo "fdk-devel" ;;
    *)       echo "fdk-dev" ;;
    esac
}

# The MINIMUM FDK version fdk-shell's dependency metadata declares —
# spelled in the target family's grammar. fdk-shell requires FDK's
# EXTENSION API v1 (fdk_ext_api_version() >= 1, plus the
# FDK_VERSION_ENCODE(1,1,0) runtime gate — include/fdk-shell/
# fdk-shell.h's documented pattern), and extension API v1 first
# shipped in FDK 1.1.0-rc1: THAT is the floor this constraint pins,
# independent of whichever newer FDK the package happens to be built
# against (the API is append-only within the SONAME). fdk_packver
# handles each family's pre-release spelling (deb/rpm 1.1.0~rc1,
# apk 1.1.0_rc1, arch/xbps 1.1.0rc1); a plain numeric FDK floor
# passes through unchanged.
fdkshell_min_fdk_ver() {
    fdk_packver "$1" "1.1.0-rc1"
}

# Compiles and runs fdk-shell's bar smoke against the INSTALLED FDK +
# fdk-shell (both found through pkg-config). $1 = expected fdk-shell
# version. Requires cc + pkg-config + a running X server ($DISPLAY,
# started by the caller through fdk_start_xvfb) + the X11 development
# packages (the smoke's readback connection is a DIRECT Xlib consumer
# and modern ld refuses to resolve a binary's own X symbols through a
# shared library's transitive NEEDED entries - "DSO missing from the
# command line" - so the smoke's link line carries -lX11 itself; the
# X11 dev packages are TEST INFRASTRUCTURE the install-tests
# pre-install, the same class as gcc/pkg-config/xvfb). Prints SMOKE-*
# lines; returns the app's exit status.
fdkshell_run_smoke() {
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
    # Both .pc files must resolve from the SAME environment: fdk-shell's
    # Requires: fdk chain proves the metadata pair works together. A
    # discovery failure prints the REAL pkg-config diagnostic (never
    # swallow it - a bare "not found" has cost a whole CI-debug round).
    if ! pc_err="$(pkg-config --exists fdk-shell 2>&1)"; then
        echo "SMOKE FAIL: pkg-config cannot find fdk-shell"
        echo "  pkg-config diagnostic: $pc_err"
        echo "  PKG_CONFIG_PATH='${PKG_CONFIG_PATH:-}'"
        ls -la /usr/lib/pkgconfig/ 2>/dev/null | \
            grep -i fdk | sed 's/^/  /' || true
        return 1
    fi
    if ! pc_err="$(pkg-config --exists fdk 2>&1)"; then
        echo "SMOKE FAIL: pkg-config cannot find fdk (fdk-shell's Requires)"
        echo "  pkg-config diagnostic: $pc_err"
        ls -la /usr/lib/pkgconfig/ 2>/dev/null | \
            grep -i fdk | sed 's/^/  /' || true
        return 1
    fi
    if ! pkg-config --exists x11 2>/dev/null; then
        echo "SMOKE FAIL: pkg-config cannot find x11 - the X11 development"
        echo "  packages are missing (test infrastructure; see the comment"
        echo "  above and the install-test pre-installs)"
        return 1
    fi
    # shellcheck disable=SC2046
    "$cc" fdk-shell/packaging/common/smoke.c -o /tmp/fdkshell_package_smoke \
        $(pkg-config --cflags fdk-shell fdk) \
        $(pkg-config --libs fdk-shell fdk) $(pkg-config --libs x11) || {
        echo "SMOKE FAIL: compile against installed fdk-shell + fdk"
        return 1
    }
    /tmp/fdkshell_package_smoke "$expected"
}
