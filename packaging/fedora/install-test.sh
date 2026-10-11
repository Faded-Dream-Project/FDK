#!/bin/sh
# packaging/fedora/install-test.sh — install the built RPMs into a
# CLEAN RPM-based environment and verify the result.
#
# Runs INSIDE a fresh AlmaLinux/Fedora container (root) with dist/
# mounted. Installs the verification toolchain itself, then exercises
# the full native cycle: dnf install (ldconfig scriptlets),
# pkg-config discovery, fdk-theme, a compile+run smoke test under
# Xvfb, and dnf removal.

set -eu
. "$(dirname "$0")/../ci/common.sh"

VERSION="$(fdk_expected_version)"

echo "== FDK rpm install-test ($VERSION) =="

dnf install -y -q gcc pkgconf-pkg-config xvfb \
    dejavu-sans-fonts dejavu-sans-mono-fonts 2>/dev/null || \
dnf install -y -q gcc pkgconf-pkg-config xorg-x11-server-Xvfb \
    dejavu-sans-fonts dejavu-sans-mono-fonts

RPMS="$(ls dist/*.rpm)"
[ -n "$RPMS" ] || { echo "INSTALL-TEST FAIL: no rpms in dist/"; exit 1; }

echo "== dnf install =="
dnf install -y -q $RPMS

echo "== ldconfig verification =="
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
fdk-theme list | grep -q faded-dream || { echo "INSTALL-TEST FAIL: shipped theme faces not discovered"; exit 1; }
fdk-theme list | head -12

echo "== graphical smoke test (Xvfb) =="
fdk_start_xvfb || { echo "INSTALL-TEST FAIL: Xvfb unavailable — cannot smoke-test"; exit 1; }
fdk_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "INSTALL-TEST FAIL: smoke test"; exit 1; }
fdk_stop_xvfb

echo "== removal =="
rpm -e fdk-devel libfdk1
ldconfig
if ldconfig -p 2>/dev/null | grep -q libfdk; then
    echo "INSTALL-TEST FAIL: libfdk still resolvable after rpm -e"; exit 1
fi
[ ! -e /usr/include/fdk ] || { echo "INSTALL-TEST FAIL: headers survived removal"; exit 1; }
[ ! -e /usr/share/fdk ] || { echo "INSTALL-TEST FAIL: theme dir survived removal"; exit 1; }

echo "== RPM INSTALL-TEST OK ($VERSION on $(. /etc/os-release && echo "$PRETTY_NAME" || head -1 /etc/os-release)) =="
