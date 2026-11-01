#!/bin/sh
# fdk-shell-overlay/packaging/debian/install-test.sh — install the built
# fdk-shell-overlay debs into a CLEAN Debian-family environment and verify
# the DEPENDENCY CONTRACT: the package declares a real, versioned
# dependency on the FDK runtime, and the native package manager
# enforces it.
#
# Runs INSIDE a fresh debian/ubuntu container (root) with dist/ and
# the repository mounted. Requires nothing beforehand.
#
# The proof, in order:
#   1. apt cannot install fdk-shell-overlay's runtime WITHOUT the FDK
#      packages — the declared dependency is real and enforced.
#   2. apt installs the full set together, resolving the
#      inter-package dependency graph from the local files.
#   3. pkg-config discovery (fdk-shell-overlay + its Requires: fdk chain).
#   4. The graphical smoke (layer create/show/present under Xvfb).
#   5. Removal.

set -eu
. "$(dirname "$0")/../ci/common.sh"

FDKSHELL_VERSION="$(fdkshell_version)"
FDK_VERSION="$(fdk_expected_version)"
FDK_PKGVER="$(fdk_packver deb "$FDK_VERSION")-1"
PKGVER="$(fdk_packver deb "$FDKSHELL_VERSION")-1"
MINVER="$(fdkshell_min_fdk_ver deb)"
ARCH="$(dpkg --print-architecture)"

echo "== fdk-shell-overlay deb install-test ($FDKSHELL_VERSION, arch $ARCH, FDK floor $MINVER) =="

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# libx11-dev is TEST INFRASTRUCTURE, the same class as gcc/pkg-config/
# xvfb on this line: the graphical smoke reads server-side truth (dock
# type, _NET_WM_DESKTOP, the strut) through a second Xlib connection,
# and nothing in the package dependency chain ships those headers — the
# .pc files deliberately carry no x11 Requires (fdk.pc's documented
# posture: `pkg-config --cflags fdk` must work on machines without the
# X11 development packages). The alpine/void/arch install-tests declare
# their X11 dev headers the same way (libx11-dev / libX11-devel / arch's
# unsplit libx11).
apt-get install -y --no-install-recommends \
    libx11-6 libx11-dev libxext6 zlib1g libxkbcommon0 \
    gcc libc6-dev pkg-config xvfb fonts-dejavu-core
apt-get install -y --no-install-recommends libwayland-client0 \
    2>/dev/null || echo "NOTE: libwayland-client0 unavailable on this image (X11-only payload expected)"

SHELL_RT="dist/libfdk-shell-overlay0_${PKGVER}_${ARCH}.deb"
SHELL_DV="dist/fdk-shell-overlay-dev_${PKGVER}_${ARCH}.deb"
FDK_RT="dist/libfdk1_${FDK_PKGVER}_${ARCH}.deb"
FDK_DV="dist/fdk-dev_${FDK_PKGVER}_${ARCH}.deb"
for f in "$SHELL_RT" "$SHELL_DV" "$FDK_RT" "$FDK_DV"; do
    [ -f "$f" ] || { echo "INSTALL-TEST FAIL: missing $f"; exit 1; }
done

echo "== 1. the dependency is REAL: fdk-shell-overlay alone must be refused =="
if apt-get install -y --no-install-recommends "./$SHELL_RT" >/tmp/fdkshell-alone.log 2>&1; then
    echo "INSTALL-TEST FAIL: apt installed libfdk-shell-overlay0 without libfdk1 —"
    echo "                     the FDK dependency is NOT declared (check Depends)"
    exit 1
fi
grep -iE "libfdk1|unmet|unable to locate|depends" /tmp/fdkshell-alone.log | head -3 || true
echo "   refused as expected (unmet dependency on $(fdkshell_fdk_runtime_pkg deb))"

echo "== 2. the full set installs together (dependency resolution) =="
apt-get install -y --no-install-recommends \
    "./$FDK_RT" "./$FDK_DV" "./$SHELL_RT" "./$SHELL_DV"
ldconfig -p | grep -q libfdk-shell-overlay || { echo "INSTALL-TEST FAIL: libfdk-shell-overlay not in ldconfig cache"; exit 1; }

echo "== 3. pkg-config discovery (the Requires: fdk chain) =="
GOT="$(pkg-config --modversion fdk-shell-overlay)"
[ "$GOT" = "$FDKSHELL_VERSION" ] || { echo "INSTALL-TEST FAIL: pkg-config fdk-shell-overlay = '$GOT', expected '$FDKSHELL_VERSION'"; exit 1; }
GOT_FDK="$(pkg-config --modversion fdk)"
[ "$GOT_FDK" = "$FDK_VERSION" ] || { echo "INSTALL-TEST FAIL: pkg-config fdk = '$GOT_FDK', expected '$FDK_VERSION'"; exit 1; }
echo "pkg-config: fdk-shell-overlay $GOT (requires fdk $GOT_FDK) OK"
[ -f /usr/include/fdk-shell-overlay/fdk-shell-overlay.h ] || { echo "INSTALL-TEST FAIL: header missing"; exit 1; }

echo "== 4. graphical smoke test (Xvfb) =="
fdk_start_xvfb || { echo "INSTALL-TEST FAIL: Xvfb unavailable — cannot smoke-test"; exit 1; }
fdkshell_run_smoke "$FDKSHELL_VERSION" || { fdk_stop_xvfb; echo "INSTALL-TEST FAIL: smoke test"; exit 1; }
fdk_stop_xvfb

echo "== 5. removal =="
dpkg -r fdk-shell-overlay-dev libfdk-shell-overlay0
if ldconfig -p 2>/dev/null | grep -q libfdk-shell-overlay; then
    echo "INSTALL-TEST FAIL: libfdk-shell-overlay still resolvable after dpkg -r"; exit 1
fi
[ ! -e /usr/include/fdk-shell-overlay ] || { echo "INSTALL-TEST FAIL: header survived removal"; exit 1; }

echo "== FDK-SHELL DEB INSTALL-TEST OK ($FDKSHELL_VERSION on $(. /etc/os-release && echo "$PRETTY_NAME" || head -1 /etc/os-release)) =="
