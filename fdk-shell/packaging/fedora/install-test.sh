#!/bin/sh
# fdk-shell/packaging/fedora/install-test.sh — install the built
# fdk-shell RPMs into a CLEAN RPM environment and verify the
# DEPENDENCY CONTRACT (real, versioned, enforced by the package
# manager).
#
#   1. dnf cannot install libfdk-shell0 WITHOUT the FDK packages.
#   2. The full set installs together (dependency resolution).
#   3. pkg-config discovery (fdk-shell + Requires: fdk).
#   4. The graphical smoke.
#   5. Removal.

set -eu
. "$(dirname "$0")/../ci/common.sh"

FDKSHELL_VERSION="$(fdkshell_version)"
FDKSHELL_PKGVER="$(fdk_packver rpm "$FDKSHELL_VERSION")"
MINVER="$(fdkshell_min_fdk_ver rpm)"

echo "== fdk-shell rpm install-test ($FDKSHELL_VERSION, FDK floor $MINVER) =="

SHELL_RT="dist/libfdk-shell0-${FDKSHELL_PKGVER}-1.x86_64.rpm"
SHELL_DV="dist/fdk-shell-devel-${FDKSHELL_PKGVER}-1.x86_64.rpm"
FDK_RT="$(ls dist/libfdk1-*.rpm 2>/dev/null | head -1)"
FDK_DV="$(ls dist/fdk-devel-*.rpm 2>/dev/null | head -1)"
for f in "$SHELL_RT" "$SHELL_DV" "$FDK_RT" "$FDK_DV"; do
    [ -n "$f" ] && [ -f "$f" ] || { echo "INSTALL-TEST FAIL: missing $f"; exit 1; }
done

# Test infrastructure only: the runtime X11/Wayland libraries arrive
# through dnf's own dependency resolution when the packages install
# (rpm's auto-Requires). libX11-devel is here for the graphical smoke —
# it reads server-side truth through a second Xlib connection, and the
# .pc chain deliberately carries no x11 Requires (fdk.pc's documented
# posture) — the same class as gcc/pkg-config themselves.
# Xvfb is xorg-x11-server-Xvfb on Fedora; RHEL-10 clones ship no
# standalone Xorg server at all, so there it is an honest skip and the
# graphical smoke runs on the Fedora leg — the same shape as FDK's own
# rpm install-test.
dnf install -y -q gcc pkgconf-pkg-config libX11-devel \
    dejavu-sans-fonts >/dev/null 2>&1 || \
    dnf install -y -q gcc pkgconfig libX11-devel \
    dejavu-sans-fonts >/dev/null
dnf install -y -q xorg-x11-server-Xvfb 2>/dev/null || \
    echo "XVFB-SKIP: no Xvfb on this image (RHEL 10 family) — graphical smoke skipped, disclosed"

echo "== 1. the dependency is REAL: fdk-shell alone must be refused =="
if dnf install -y -q "./$SHELL_RT" >/tmp/fdkshell-alone.log 2>&1; then
    echo "INSTALL-TEST FAIL: dnf installed libfdk-shell0 without libfdk1 —"
    echo "                     the FDK dependency is NOT declared"
    exit 1
fi
grep -iE "libfdk|nothing provides|no match" /tmp/fdkshell-alone.log | head -3 || true
echo "   refused as expected (unmet dependency on libfdk1)"

echo "== 2. the full set installs together =="
dnf install -y -q "./$FDK_RT" "./$FDK_DV" "./$SHELL_RT" "./$SHELL_DV"
rpm -q libfdk-shell0 fdk-shell-devel

echo "== 3. pkg-config discovery (the Requires: fdk chain) =="
GOT="$(pkg-config --modversion fdk-shell)"
[ "$GOT" = "$FDKSHELL_VERSION" ] || { echo "INSTALL-TEST FAIL: pkg-config fdk-shell = '$GOT'"; exit 1; }
echo "pkg-config: fdk-shell $GOT (requires fdk $(pkg-config --modversion fdk)) OK"
[ -f /usr/include/fdk-shell/fdk-shell.h ] || { echo "INSTALL-TEST FAIL: header missing"; exit 1; }

echo "== 4. graphical smoke test (Xvfb) =="
if fdk_start_xvfb; then
    fdkshell_run_smoke "$FDKSHELL_VERSION" || { fdk_stop_xvfb; echo "INSTALL-TEST FAIL: smoke test"; exit 1; }
    fdk_stop_xvfb
else
    echo "XVFB-SKIP carried through: graphical smoke not run (disclosed above)"
fi

echo "== 5. removal =="
rpm -e fdk-shell-devel libfdk-shell0
[ ! -e /usr/include/fdk-shell ] || { echo "INSTALL-TEST FAIL: header survived removal"; exit 1; }

echo "== FDK-SHELL RPM INSTALL-TEST OK ($FDKSHELL_VERSION on $(. /etc/os-release && echo "$PRETTY_NAME")) =="
