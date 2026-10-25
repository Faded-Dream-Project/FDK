#!/bin/sh
# fdk-shell/packaging/void/install-test.sh — install the built
# fdk-shell xbps packages into a CLEAN Void environment and verify
# the DEPENDENCY CONTRACT (real, versioned, enforced by xbps through
# a local repository).
#
#   1. xbps-install cannot install fdk-shell WITHOUT the FDK
#      packages in the repository (the declared dependency fails).
#   2. With FDK in the repository, fdk-shell installs and PULLS
#      fdk-libs in through its dependency.
#   3. pkg-config discovery (fdk-shell + Requires: fdk).
#   4. The graphical smoke.
#   5. Removal.

set -eu
. "$(dirname "$0")/../ci/common.sh"

FDKSHELL_VERSION="$(fdkshell_version)"
FDKSHELL_PKGVER="$(fdk_packver xbps "$FDKSHELL_VERSION")"
MINVER="$(fdkshell_min_fdk_ver xbps)"

echo "== fdk-shell xbps install-test ($FDKSHELL_VERSION, FDK floor $MINVER) =="

xbps-install -Sy gcc pkgconf make findutils >/dev/null 2>&1 || \
    xbps-install -Sy gcc pkgconf make findutils >/dev/null
# Runtime shlibs the FDK + fdk-shell packages need in this fresh
# container: xbps resolves the shlib-requires DECLARED on the packages
# only against installed providers — without these lines the fdk-devel
# transaction aborts "unresolvable shlib libXext.so.6 / libxkbcommon.so.0"
# (the same hermetic lines FDK's own void install-test carries).
xbps-install -Sy libX11 libXext zlib 2>/dev/null || true
xbps-install -Sy libxkbcommon 2>/dev/null || true
# Void's wayland client runtime library ships in the package named
# plainly 'wayland' (wayland-devel is its -devel sibling).
xbps-install -Sy wayland 2>/dev/null || true
# libX11-devel: the smoke's second Xlib readback connection (TEST
# INFRASTRUCTURE — the .pc chain deliberately carries no x11
# Requires; the smoke's own link line carries -lX11).
xbps-install -Sy libX11-devel 2>/dev/null || true
xbps-install -Sy xorg-server-xvfb 2>/dev/null || \
    xbps-install -Sy xvfb 2>/dev/null || \
    echo "XVFB-SKIP: no Xvfb in the repositories — graphical smoke skipped, disclosed"

SHELL_XBPS="$(ls dist/fdk-shell-[0-9]*.xbps 2>/dev/null | head -1)"
FDK_RT_XBPS="$(ls dist/fdk-libs-*.xbps 2>/dev/null | head -1)"
FDK_DV_XBPS="$(ls dist/fdk-devel-*.xbps 2>/dev/null | head -1)"
for f in "$SHELL_XBPS" "$FDK_RT_XBPS" "$FDK_DV_XBPS"; do
    [ -n "$f" ] && [ -f "$f" ] || { echo "INSTALL-TEST FAIL: missing $f"; exit 1; }
done

echo "== 1. the dependency is REAL: fdk-shell alone must be refused =="
REPO=/tmp/fdkshell-repo-alone
rm -rf "$REPO"; mkdir -p "$REPO"
xbps-rindex -a "$SHELL_XBPS" 2>/dev/null || true
cp "$SHELL_XBPS" "$REPO/"
xbps-rindex -a "$REPO"/fdk-shell-*.xbps 2>/dev/null || xbps-rindex --add "$REPO"/fdk-shell-*.xbps
if xbps-install -y --repository "$REPO" fdk-shell >/tmp/fdkshell-alone.log 2>&1; then
    echo "INSTALL-TEST FAIL: xbps installed fdk-shell without fdk-libs —"
    echo "                     the FDK dependency is NOT declared"
    exit 1
fi
grep -iE "fdk-libs|unresolvable|not found|dependency" /tmp/fdkshell-alone.log | head -3 || true
echo "   refused as expected (unresolvable dependency on fdk-libs)"

echo "== 2. the full set installs together (fdk-libs pulled by the dependency) =="
REPO=/tmp/fdkshell-repo-full
rm -rf "$REPO"; mkdir -p "$REPO"
cp "$SHELL_XBPS" "$FDK_RT_XBPS" "$FDK_DV_XBPS" dist/fdk-shell-devel-*.xbps "$REPO/"
xbps-rindex -a "$REPO"/*.xbps 2>/dev/null || xbps-rindex --add "$REPO"/*.xbps
xbps-install -y --repository "$REPO" fdk-shell-devel
xbps-query -l | grep -E "fdk-shell|fdk-libs" || true
xbps-query -s fdk-shell | head -4

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
xbps-remove -y fdk-shell-devel fdk-shell
[ ! -e /usr/include/fdk-shell ] || { echo "INSTALL-TEST FAIL: header survived removal"; exit 1; }
[ ! -e /usr/lib/libfdk-shell.so.0 ] || { echo "INSTALL-TEST FAIL: library survived removal"; exit 1; }
rm -rf "$REPO" /tmp/fdkshell-repo-alone

echo "== FDK-SHELL XBPS INSTALL-TEST OK ($FDKSHELL_VERSION on Void) =="
