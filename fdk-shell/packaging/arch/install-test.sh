#!/bin/sh
# fdk-shell/packaging/arch/install-test.sh — install the built
# fdk-shell package into a CLEAN Arch environment and verify the
# DEPENDENCY CONTRACT (real, versioned, enforced by pacman).
#
#   1. pacman -U of fdk-shell alone must be REFUSED (missing fdk).
#   2. The full set installs together.
#   3. pkg-config discovery (fdk-shell + Requires: fdk).
#   4. The graphical smoke.
#   5. Removal.

set -eu
. "$(dirname "$0")/../ci/common.sh"

FDKSHELL_VERSION="$(fdkshell_version)"
FDKSHELL_PKGVER="$(fdk_packver arch "$FDKSHELL_VERSION")"
MINVER="$(fdkshell_min_fdk_ver arch)"

echo "== fdk-shell arch install-test ($FDKSHELL_VERSION, FDK floor $MINVER) =="

pacman -Sy --noconfirm --needed \
    gcc pkgconf libx11 libxext zlib wayland libxkbcommon \
    xorg-server-xvfb ttf-dejavu >/dev/null

SHELL_PKG="$(ls dist/fdk-shell-*.pkg.tar.* 2>/dev/null | head -1)"
FDK_PKG="$(ls dist/fdk-*.pkg.tar.* 2>/dev/null | grep -v fdk-shell | head -1)"
[ -n "$SHELL_PKG" ] || { echo "INSTALL-TEST FAIL: no fdk-shell package in dist/"; exit 1; }
[ -n "$FDK_PKG" ] || { echo "INSTALL-TEST FAIL: no FDK package in dist/"; exit 1; }

echo "== 1. the dependency is REAL: fdk-shell alone must be refused =="
if pacman -U --noconfirm "$SHELL_PKG" >/tmp/fdkshell-alone.log 2>&1; then
    echo "INSTALL-TEST FAIL: pacman installed fdk-shell without fdk —"
    echo "                     the FDK dependency is NOT declared"
    exit 1
fi
grep -iE "missing|fdk" /tmp/fdkshell-alone.log | head -3 || true
echo "   refused as expected (unmet dependency on fdk)"

echo "== 2. the full set installs together =="
pacman -U --noconfirm "$FDK_PKG" "$SHELL_PKG"
pacman -Qi fdk-shell | grep -E "^(Name|Version|Depends)"

echo "== 3. pkg-config discovery (the Requires: fdk chain) =="
GOT="$(pkg-config --modversion fdk-shell)"
[ "$GOT" = "$FDKSHELL_VERSION" ] || { echo "INSTALL-TEST FAIL: pkg-config fdk-shell = '$GOT'"; exit 1; }
echo "pkg-config: fdk-shell $GOT (requires fdk $(pkg-config --modversion fdk)) OK"
[ -f /usr/include/fdk-shell/fdk-shell.h ] || { echo "INSTALL-TEST FAIL: header missing"; exit 1; }

echo "== 4. graphical smoke test (Xvfb) =="
fdk_start_xvfb || { echo "INSTALL-TEST FAIL: Xvfb unavailable"; exit 1; }
fdkshell_run_smoke "$FDKSHELL_VERSION" || { fdk_stop_xvfb; echo "INSTALL-TEST FAIL: smoke test"; exit 1; }
fdk_stop_xvfb

echo "== 5. removal =="
pacman -R --noconfirm fdk-shell
[ ! -e /usr/include/fdk-shell ] || { echo "INSTALL-TEST FAIL: header survived removal"; exit 1; }
[ ! -e /usr/lib/libfdk-shell.so.0 ] || { echo "INSTALL-TEST FAIL: library survived removal"; exit 1; }

echo "== FDK-SHELL ARCH INSTALL-TEST OK ($FDKSHELL_VERSION) =="
