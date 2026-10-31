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

# The PACKAGE-METADATA version for one distro family: fdk_packver <family>
# [raw-version]. The raw version defaults to fdk_expected_version; callers
# that only have an explicit version string pass it directly.
#
# WHY THIS EXISTS (found live by CI on 1.1.0-rc1, 2026-10-25): while a
# release candidate is in the tree the raw version carries a hyphenated
# pre-release suffix ("1.1.0-rc1"), and the five package grammars
# disagree about that suffix — four of them reject it outright:
#   rpm   reserves '-' for the Version-Release separator -> "Illegal char"
#   apk   reserves '-' for the -rN pkgrel                 -> "not a valid version"
#   arch  pkgver forbids hyphens entirely
#   xbps  forbids dashes, underscores AND special chars (void-packages
#         Manual: "Must not contain dashes or underscores"), and a '-'
#         makes the pkgname-version split ambiguous so repository
#         lookups fail
#   deb   accepts "1.1.0-rc1" syntactically but ORDERS it above the
#         eventual 1.1.0 final (the hyphen starts the debian_revision,
#         and "rc1" > "1") — the opposite of what a candidate means.
# Each family therefore gets its native pre-release spelling:
#   deb, rpm       1.1.0-rc1 -> 1.1.0~rc1   (tilde sorts before the final)
#   apk            1.1.0-rc1 -> 1.1.0_rc1   (apk's _rc suffix family)
#   arch, xbps     1.1.0-rc1 -> 1.1.0rc1    (drop the hyphen)
# A plain numeric version (1.1.0) passes through EVERY grammar unchanged,
# so at final releases all five spell the same thing and this mapping is
# inert — the 1.0.0 release artifacts are byte-identical under it.
#
# SCOPE: package METADATA only (Version fields, package filenames, and
# the tarball/dir names the metadata-driven builds derive: rpm %setup,
# makepkg $srcdir, abuild builddir). The .so real-file name (numeric
# MAJOR.MINOR.PATCH — see fdk_sover below), the pkg-config Version and
# the smoke test keep the RAW string — they come from fdk_version.h and
# the Makefile and must stay byte-identical to what the library itself
# reports.
fdk_packver() {
    _fp_family="$1"
    _fp_raw="${2:-$(fdk_expected_version)}"
    case "$_fp_family" in
    deb|rpm)   printf '%s\n' "$_fp_raw" | tr '-' '~' ;;
    apk)       printf '%s\n' "$_fp_raw" | tr '-' '_' ;;
    arch|xbps) printf '%s\n' "$_fp_raw" | tr -d '-' ;;
    *)         printf '%s\n' "$_fp_raw" ;;
    esac
}

# The NUMERIC shared-library file version (MAJOR.MINOR.PATCH from the
# header macros — never a pre-release suffix). The Makefile names the
# real .so file with this (FDK_SO_VERSION) and the packaging scripts
# must reference the same name when they split the install tree.
# WHY numeric-only: Alpine's abuild derives the apk so:provides version
# from the .so FILENAME tail (sover=${file##*.so.}); a suffixed tail
# (libfdk.so.1.1.0-rc1) produced 'provides = so:libfdk.so.1=1.1.0-rc1'
# and apk index rejected the whole package ("package file format
# error", proven live on CI 2026-10-25 and reproduced locally with
# apk-tools 2.14.12 — even a hyphenless '1.1.0rc1' tail is rejected;
# only numeric tails are valid). The suffix lives in the version
# string, pkg-config, and package metadata — never in the .so name.
fdk_sover() {
    printf '%s.%s.%s\n' \
        "$(sed -n 's/^#define FDK_VERSION_MAJOR \([0-9]*\).*/\1/p' include/fdk/fdk_version.h)" \
        "$(sed -n 's/^#define FDK_VERSION_MINOR \([0-9]*\).*/\1/p' include/fdk/fdk_version.h)" \
        "$(sed -n 's/^#define FDK_VERSION_PATCH \([0-9]*\).*/\1/p' include/fdk/fdk_version.h)"
}

# RELEASE-POSTURE GUARD. The packaging pipeline is release
# infrastructure: it stages the versioned shared-library layout
# (libfdk.so.<version> real file + SONAME symlink + dev symlink)
# that the Makefile installs only once the public version reaches
# 1.0.0 — see docs/versioning.md, whose 2026-10-11 retraction record
# returned the tree to the unversioned pre-release posture. At
# pre-1.0 versions every packaging script refuses with this
# DISCLOSED skip instead of producing an inconsistent payload.
# exit 0 on purpose: a green CI run must mean "nothing to package
# yet", never "pipeline broken".
fdk_require_release_posture() {
    _major="$(sed -n 's/^#define FDK_VERSION_MAJOR \([0-9]*\).*/\1/p' \
        include/fdk/fdk_version.h)"
    if [ "${_major:-0}" -lt 1 ]; then
        echo "PACKAGING-SKIP: public version $(fdk_expected_version) is pre-1.0"
        echo "PACKAGING-SKIP: (docs/versioning.md — the retraction record). The versioned"
        echo "PACKAGING-SKIP: packaging layout activates with the public 1.0.0 decision."
        echo "PACKAGING-SKIP: nothing was built; this is the honest posture, not a failure."
        exit 0
    fi
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

# Void mirror rotation (2026-10-26: repo-default.voidlinux.org's
# round-robin was unreachable from the GitHub runner AND from two
# other networks at once — a mirror outage, not a code defect; the
# code was green on the five other families while void died one step
# before any package resolution). The single 10s-retry of the same
# dead host could not recover. This rotates the ACTUAL mirrors by
# overriding xbps's repository conf (files in /etc/xbps.d with the
# same name win over /usr/share/xbps.d; the shipped conf writes are
# belt-and-braces since the containers run as root), probing each
# with a bare `xbps-install -S` (repodata only). Memoized per
# container through a stamp so build + install-test containers pay
# the probe once each. POSIX sh; call AFTER sourcing, BEFORE any
# remote xbps-install.
fdk_void_mirror_rotate() {
    if [ -f /tmp/.fdk-void-mirror-ok ]; then
        return 0
    fi
    _vm_mirrors="
https://repo-default.voidlinux.org/current
https://mirror.clarkson.edu/voidlinux/current
https://mirrors.servercentral.com/voidlinux/current
https://repo-eu.voidlinux.org/current
https://repo-us.voidlinux.org/current"
    mkdir -p /etc/xbps.d
    for _vm_m in $_vm_mirrors; do
        printf 'repository=%s\n' "$_vm_m" \
            > /etc/xbps.d/10-repository-main.conf
        printf 'repository=%s\n' "$_vm_m" \
            > /usr/share/xbps.d/10-repository-main.conf 2>/dev/null || true
        if xbps-install -S >/dev/null 2>&1; then
            echo "NOTE: void reposync via $_vm_m"
            : > /tmp/.fdk-void-mirror-ok
            return 0
        fi
        echo "NOTE: void mirror unreachable: $_vm_m (rotating)"
    done
    echo "FATAL: every void mirror failed (network outage?)"
    return 1
}
