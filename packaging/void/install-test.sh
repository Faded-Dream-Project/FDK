#!/bin/sh
# packaging/void/install-test.sh — install the built .xbps packages
# into a CLEAN Void environment and verify the result.
#
# Runs INSIDE a fresh void-glibc container (root) with dist/ mounted.
# xbps-rindex builds a local repository; xbps-install resolves the
# automatic shared-library dependencies from the system repos.

set -eu
. "$(dirname "$0")/../ci/common.sh"

VERSION="$(fdk_expected_version)"

echo "== FDK xbps install-test ($VERSION) =="

xbps-install -Sy gcc pkgconf 2>/dev/null || xbps-install -Sy gcc pkgconf
# Runtime libraries the packages need in this fresh container. xbps
# resolves the shlib-requires declared on the packages; installing them
# explicitly keeps the test hermetic against repo metadata variance.
xbps-install -Sy libX11 libXext zlib 2>/dev/null || true
xbps-install -Sy libxkbcommon 2>/dev/null || true
# Void's wayland client runtime library ships in the package named
# plainly 'wayland' (wayland-devel is its -devel sibling).
xbps-install -Sy wayland 2>/dev/null || true
xbps-install -Sy dejavu-fonts-ttf 2>/dev/null || \
    echo "NOTE: dejavu-fonts-ttf unavailable; smoke may fail on font discovery (disclosed)"

RT="dist/fdk-libs-$VERSION"_"1.$(uname -m).xbps"
DV="dist/fdk-devel-$VERSION"_"1.$(uname -m).xbps"
ls dist/*.xbps

echo "== local repository + xbps-install =="
xbps-rindex --add dist/*.xbps
xbps-query --repository=dist -S fdk-libs | head -5
xbps-install -Sy --repository="$(pwd)/dist" fdk-libs fdk-devel

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
if fdk_start_xvfb; then
    fdk_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "INSTALL-TEST FAIL: smoke test"; exit 1; }
    fdk_stop_xvfb
else
    echo "XVFB-SKIP carried through: graphical smoke not run (disclosed)"
fi

echo "== removal =="
xbps-remove -Ry fdk-devel fdk-libs
[ ! -e /usr/include/fdk ] || { echo "INSTALL-TEST FAIL: headers survived removal"; exit 1; }
[ ! -e /usr/share/fdk ] || { echo "INSTALL-TEST FAIL: theme dir survived removal"; exit 1; }
[ ! -e /usr/lib/libfdk.so ] || { echo "INSTALL-TEST FAIL: library survived removal"; exit 1; }

echo "== XBPS INSTALL-TEST OK ($VERSION on void $(xbps-query -V 2>/dev/null || true)) =="
