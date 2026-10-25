#!/bin/sh
# fdk-shell/packaging/alpine/install-test.sh — install the built
# fdk-shell apks into a CLEAN Alpine environment and verify the
# DEPENDENCY CONTRACT (real, versioned, enforced by apk).
#
#   1. apk cannot install fdk-shell WITHOUT the FDK packages.
#   2. The full set installs together.
#   3. pkg-config discovery (fdk-shell + Requires: fdk).
#   4. The graphical smoke.
#   5. Removal.

set -eu
. "$(dirname "$0")/../ci/common.sh"

FDKSHELL_VERSION="$(fdkshell_version)"
FDKSHELL_PKGVER="$(fdk_packver apk "$FDKSHELL_VERSION")"
MINVER="$(fdkshell_min_fdk_ver apk)"

echo "== fdk-shell apk install-test ($FDKSHELL_VERSION, FDK floor $MINVER) =="

apk add --no-cache gcc musl-dev pkgconf libx11 libx11-dev wayland \
    wayland-dev xvfb font-dejavu >/dev/null 2>&1 || \
    apk add --no-cache gcc musl-dev pkgconf libx11 wayland xvfb >/dev/null

SHELL_RT="$(ls dist/fdk-shell-[0-9]*.apk 2>/dev/null | head -1)"
SHELL_DV="$(ls dist/fdk-shell-dev-*.apk 2>/dev/null | head -1)"
FDK_RT="$(ls dist/fdk-[0-9]*.apk 2>/dev/null | head -1)"
FDK_DV="$(ls dist/fdk-dev-*.apk 2>/dev/null | head -1)"
for f in "$SHELL_RT" "$SHELL_DV" "$FDK_RT" "$FDK_DV"; do
    [ -n "$f" ] && [ -f "$f" ] || { echo "INSTALL-TEST FAIL: missing $f"; exit 1; }
done

echo "== 1. the dependency is REAL: fdk-shell alone must be refused =="
if apk add --allow-untrusted "$SHELL_RT" >/tmp/fdkshell-alone.log 2>&1; then
    echo "INSTALL-TEST FAIL: apk installed fdk-shell without the FDK runtime —"
    echo "                     the FDK dependency is NOT declared"
    exit 1
fi
grep -iE "ERROR|unsatisfiable|world" /tmp/fdkshell-alone.log | head -3 || true
echo "   refused as expected (unsatisfiable dependency on fdk)"

echo "== 2. the full set installs together =="
apk add --allow-untrusted "$FDK_RT" "$FDK_DV" "$SHELL_RT" "$SHELL_DV"
apk info -e fdk-shell fdk-shell-dev fdk fdk-dev
apk info -R fdk-shell | head -6

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
apk del fdk-shell-dev fdk-shell
[ ! -e /usr/include/fdk-shell ] || { echo "INSTALL-TEST FAIL: header survived removal"; exit 1; }
[ ! -e /usr/lib/libfdk-shell.so.0 ] || { echo "INSTALL-TEST FAIL: library survived removal"; exit 1; }

echo "== FDK-SHELL APK INSTALL-TEST OK ($FDKSHELL_VERSION on $(cat /etc/alpine-release 2>/dev/null || echo alpine)) =="
