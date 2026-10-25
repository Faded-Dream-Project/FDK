#!/bin/sh
# packaging/alpine/install-test.sh — install the built .apk packages
# into a CLEAN Alpine environment and verify the result.
#
# Runs INSIDE a fresh alpine container (root) with dist/ mounted.
# apk resolves the packages' automatic so: dependencies from the
# enabled repositories; unsigned upstream packages need
# --allow-untrusted (integrity is provided by the release-page
# SHA256SUMS over HTTPS — see docs/packaging.md "Signing").

set -eu
. "$(dirname "$0")/../ci/common.sh"

VERSION="$(fdk_expected_version)"
# The .apk FILENAMES carry the package-metadata version (fdk_packver in
# common.sh: 1.1.0-rc1 -> 1.1.0_rc1, apk's native pre-release spelling);
# the pkg-config check and the smoke test below keep the RAW version —
# fdk.pc and the library report it.
PACKVER="$(fdk_packver apk "$VERSION")"
ARCH="$(uname -m)"

echo "== FDK apk install-test ($VERSION, $ARCH) =="

apk add --no-cache gcc musl-dev pkgconf xvfb 2>/dev/null || \
    apk add --no-cache gcc musl-dev pkgconf
apk add --no-cache font-dejavu 2>/dev/null || \
    echo "NOTE: font-dejavu unavailable; smoke may fail on font discovery (disclosed)"

RT="dist/fdk-$PACKVER-r0-$ARCH.apk"
DV="dist/fdk-dev-$PACKVER-r0-$ARCH.apk"
[ -f "$RT" ] || { echo "INSTALL-TEST FAIL: missing $RT"; ls dist/; exit 1; }
[ -f "$DV" ] || { echo "INSTALL-TEST FAIL: missing $DV"; ls dist/; exit 1; }

echo "== apk add (with automatic so: dependency resolution) =="
apk add --allow-untrusted "$RT" "$DV"

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
apk del fdk-dev fdk
[ ! -e /usr/include/fdk ] || { echo "INSTALL-TEST FAIL: headers survived removal"; exit 1; }
[ ! -e /usr/share/fdk ] || { echo "INSTALL-TEST FAIL: theme dir survived removal"; exit 1; }
[ ! -e /usr/lib/libfdk.so ] || { echo "INSTALL-TEST FAIL: library survived removal"; exit 1; }

echo "== APK INSTALL-TEST OK ($VERSION on $(cat /etc/alpine-release 2>/dev/null || echo alpine)) =="
