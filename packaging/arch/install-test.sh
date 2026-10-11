#!/bin/sh
# packaging/arch/install-test.sh — install the built package into a
# CLEAN Arch environment and verify the result.
#
# Runs INSIDE a fresh archlinux:base container (root) with dist/
# mounted. pacman -U, pkg-config, fdk-theme, smoke under Xvfb,
# pacman -R removal.

set -eu
. "$(dirname "$0")/../ci/common.sh"

VERSION="$(fdk_expected_version)"
ARCH="$(uname -m)"

echo "== FDK arch install-test ($VERSION, $ARCH) =="

pacman -Sy --noconfirm --needed \
    gcc pkgconf libx11 libxext zlib wayland libxkbcommon \
    xvfb ttf-dejavu >/dev/null

PKG="$(ls dist/fdk-*.pkg.tar.* 2>/dev/null | head -1)"
[ -n "$PKG" ] || { echo "INSTALL-TEST FAIL: no arch package in dist/"; exit 1; }

echo "== pacman -U =="
pacman -U --noconfirm "$PKG"

echo "== pkg-config discovery =="
GOT="$(pkg-config --modversion fdk)"
[ "$GOT" = "$VERSION" ] || { echo "INSTALL-TEST FAIL: pkg-config fdk = '$GOT', expected '$VERSION'"; exit 1; }
echo "pkg-config: fdk $GOT OK"

echo "== installed headers =="
ls /usr/include/fdk/ | head -5
[ -f /usr/include/fdk/fdk.h ] || { echo "INSTALL-TEST FAIL: /usr/include/fdk/fdk.h missing"; exit 1; }

echo "== fdk-theme tool + shipped themes =="
fdk-theme --version || true
fdk-theme list | grep -q faded-dream || { echo "INSTALL-TEST FAIL: shipped theme faces not discovered"; exit 1; }
fdk-theme list | head -12

echo "== graphical smoke test (Xvfb) =="
fdk_start_xvfb || { echo "INSTALL-TEST FAIL: Xvfb unavailable — cannot smoke-test"; exit 1; }
fdk_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "INSTALL-TEST FAIL: smoke test"; exit 1; }
fdk_stop_xvfb

echo "== removal =="
pacman -R --noconfirm fdk
[ ! -e /usr/include/fdk ] || { echo "INSTALL-TEST FAIL: headers survived removal"; exit 1; }
[ ! -e /usr/share/fdk ] || { echo "INSTALL-TEST FAIL: theme dir survived removal"; exit 1; }
[ ! -e /usr/lib/libfdk.so ] || { echo "INSTALL-TEST FAIL: library survived removal"; exit 1; }

echo "== ARCH INSTALL-TEST OK ($VERSION) =="
