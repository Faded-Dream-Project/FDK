#!/bin/sh
# packaging/debian/install-test.sh — install the built .debs into a
# CLEAN Debian-family environment and verify the result.
#
# Runs INSIDE a fresh debian/ubuntu container (root) with dist/ and
# the repository mounted. Requires nothing beforehand: it installs
# the runtime dependencies and the verification toolchain itself.
#
# Exercises the full native cycle: dpkg -i (ldconfig trigger),
# pkg-config discovery, the fdk-theme tool, a compile+run smoke test
# against the system-installed FDK under Xvfb, and dpkg -r removal.

set -eu
. "$(dirname "$0")/../ci/common.sh"

VERSION="$(fdk_expected_version)"
# The .deb FILENAMES carry the package-metadata version (fdk_packver in
# common.sh: 1.1.0-rc1 -> 1.1.0~rc1); the pkg-config check and the smoke
# test below keep the RAW version — fdk.pc and the library report it.
PKGVER="$(fdk_packver deb "$VERSION")-1"
ARCH="$(dpkg --print-architecture)"

echo "== FDK deb install-test ($VERSION, arch $ARCH) =="

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# Runtime deps of libfdk1 (must match the control file's Depends),
# plus the verification toolchain. libwayland-client0/libxkbcommon0
# are present when the packaged .so carries the Wayland backend; on
# images where they are absent, apt skips them harmlessly via the
# fallback below.
apt-get install -y --no-install-recommends \
    libx11-6 libxext6 zlib1g \
    gcc libc6-dev pkg-config xvfb fonts-dejavu-core
apt-get install -y --no-install-recommends libwayland-client0 libxkbcommon0 \
    2>/dev/null || echo "NOTE: wayland runtime packages unavailable on this image (X11-only payload expected)"

DEB_RT="dist/libfdk1_${PKGVER}_${ARCH}.deb"
DEB_DEV="dist/fdk-dev_${PKGVER}_${ARCH}.deb"
[ -f "$DEB_RT" ] || { echo "INSTALL-TEST FAIL: missing $DEB_RT"; exit 1; }
[ -f "$DEB_DEV" ] || { echo "INSTALL-TEST FAIL: missing $DEB_DEV"; exit 1; }

echo "== dpkg -i =="
dpkg -i "$DEB_RT" "$DEB_DEV"

echo "== ldconfig trigger verification =="
ldconfig -p | grep -q libfdk || { echo "INSTALL-TEST FAIL: libfdk not in ldconfig cache"; exit 1; }

echo "== pkg-config discovery =="
GOT="$(pkg-config --modversion fdk)"
[ "$GOT" = "$VERSION" ] || { echo "INSTALL-TEST FAIL: pkg-config fdk = '$GOT', expected '$VERSION'"; exit 1; }
echo "pkg-config: fdk $GOT OK"

echo "== installed headers =="
ls /usr/include/fdk/ | head -5
[ -f /usr/include/fdk/fdk.h ] || { echo "INSTALL-TEST FAIL: /usr/include/fdk/fdk.h missing"; exit 1; }

echo "== fdk-theme tool + shipped themes =="
fdk-theme --version || true
fdk-theme list | grep -q faded-dream || { echo "INSTALL-TEST FAIL: shipped theme faces not discovered from /usr/share/fdk"; exit 1; }
fdk-theme list | head -12

echo "== graphical smoke test (Xvfb) =="
fdk_start_xvfb || { echo "INSTALL-TEST FAIL: Xvfb unavailable — cannot smoke-test"; exit 1; }
fdk_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "INSTALL-TEST FAIL: smoke test"; exit 1; }
fdk_stop_xvfb

echo "== removal =="
dpkg -r fdk-dev libfdk1
if ldconfig -p 2>/dev/null | grep -q libfdk; then
    echo "INSTALL-TEST FAIL: libfdk still resolvable after dpkg -r"; exit 1
fi
[ ! -e /usr/include/fdk ] || { echo "INSTALL-TEST FAIL: headers survived removal"; exit 1; }
[ ! -e /usr/share/fdk ] || { echo "INSTALL-TEST FAIL: theme dir survived removal"; exit 1; }
dpkg -l | grep -E 'libfdk1|fdk-dev' | grep -v '^un' || true

echo "== DEB INSTALL-TEST OK ($VERSION on $(. /etc/os-release && echo "$PRETTY_NAME" || head -1 /etc/os-release)) =="
