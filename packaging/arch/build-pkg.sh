#!/bin/sh
# packaging/arch/build-pkg.sh — build the FDK Arch package.
#
# Runs INSIDE an archlinux:base container (root) with the build
# dependencies already installed by the orchestrator
# (packaging/ci/arch.sh): base-devel, pkgconf, libx11, libxext, zlib,
# wayland, libxkbcommon, xvfb, ttf-dejavu.
#
# makepkg refuses to run as root, so the flow is: run the test suites
# on the plain tree, prepare a pristine tarball + PKGBUILD in a
# builder-owned workspace, run makepkg as the builder user, copy the
# artifact back, verify it, and smoke-test the extracted payload.

set -eu
. "$(dirname "$0")/../ci/common.sh"
fdk_require_release_posture

VERSION="$(fdk_expected_version)"
# Package metadata version: makepkg's pkgver forbids hyphens ("pkgver is
# not allowed to contain ... hyphens"), so the raw "1.1.0-rc1" drops the
# hyphen (1.1.0rc1, the Arch pre-release spelling). The tarball name and
# its top directory follow pkgver (build()/check()/package() cd
# "$srcdir/fdk-$pkgver"), so they use the SAME spelling. Plain releases
# pass through unchanged (fdk_packver in common.sh).
PACKVER="$(fdk_packver arch "$VERSION")"
ARCH="$(uname -m)"

echo "== building FDK $VERSION arch package ($ARCH) =="

# --- test suites on the plain tree first ---------------------------------
# FDK_CONFIG is pinned on the install line too (config-stamp trap: a
# plain `make install` after a release build silently rebuilds debug
# with sanitizers into the payload).
make
make test
if command -v Xvfb >/dev/null 2>&1; then
# The X11 suite in distro containers runs with LSan exitcode=0: every
# test must still pass, but exit-time leak REPORTS from third-party
# libraries (older distro libxcb leaves ~336 bytes at XCloseDisplay,
# zero FDK frames in the stacks) do not fail the container run. The
# STRICT leak gate (exit-on-any-leak) runs in CI's ubuntu job and on
# every development machine.
    LSAN_OPTIONS=exitcode=0 make test-x11
else
    echo "XVFB-SKIP: Xvfb not present; the X11 integration suite was not run for this build (disclosed)"
fi
make FDK_CONFIG=release all

# --- builder workspace (makepkg forbids root) -----------------------------
WORK="/tmp/fdk-makepkg"
rm -rf "$WORK"
mkdir -p "$WORK/src"
id builder >/dev/null 2>&1 || useradd -m builder
tar --exclude=.git --exclude=build --exclude=dist \
    -czf "$WORK/fdk-$PACKVER.tar.gz" \
    --transform "s|^\./|fdk-$PACKVER/|" ./
SHA="$(sha256sum "$WORK/fdk-$PACKVER.tar.gz" | awk '{print $1}')"
sed -e "s|@VERSION@|$PACKVER|g" -e "s|@SHA256@|$SHA|" \
    packaging/arch/PKGBUILD > "$WORK/PKGBUILD"
chown -R builder:builder "$WORK"

su builder -s /bin/sh -c "
    set -eu
    cd '$WORK'
    makepkg --force
"
PKG="$(find "$WORK" -maxdepth 1 -name 'fdk-*.pkg.tar.*' -type f ! -name '*debug*')"
[ -n "$PKG" ] || { echo "BUILD FAIL: makepkg produced no package"; exit 1; }
mkdir -p dist
cp "$PKG" dist/
ls -la dist/

echo "== package metadata: =="
tar -xOf "$PKG" .PKGINFO | grep -E '^(pkgname|pkgver|pkgdesc|arch|depend|license|url)' || true

# --- payload verification -------------------------------------------------
EXT="$(mktemp -d)"
trap 'rm -rf "$EXT" "$WORK"' EXIT
tar -I zstd -xf dist/"$(basename "$PKG")" -C "$EXT" 2>/dev/null || \
    tar -xf dist/"$(basename "$PKG")" -C "$EXT"
echo "--- extracted payload (excluding .PKGINFO/.MTREE):"
find "$EXT" -type f ! -name '.PKGINFO' ! -name '.MTREE' | sed "s|$EXT||" | sort

if fdk_start_xvfb; then
    FDK_PC_DIR="$EXT/usr/lib/pkgconfig" \
FDK_INC_DIR="$EXT/usr/include" \
    FDK_LIB_DIR="$EXT/usr/lib" \
    FDK_DATA_DIR="$EXT/usr/share" \
    fdk_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "BUILD FAIL: graphical smoke test failed"; exit 1; }
    fdk_stop_xvfb
fi

echo "== ARCH BUILD OK ($VERSION, $ARCH) =="
