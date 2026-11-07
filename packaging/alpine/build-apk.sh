#!/bin/sh
# packaging/alpine/build-apk.sh — build the FDK Alpine packages.
#
# Runs INSIDE an alpine:3.22 container (root) with the build
# dependencies already installed by the orchestrator
# (packaging/ci/alpine.sh): build-base, pkgconf, libx11-dev,
# libxext-dev, zlib-dev, wayland-dev, libxkbcommon-dev, alpine-sdk,
# coreutils (+ xvfb and font-dejavu when available, for the X11
# suite — see the XVFB-SKIP disclosure otherwise).
#
# This is the musl target: the produced binaries are linked against
# musl libc and only run on musl systems. abuild runs as an
# unprivileged builder user with a generated signing key.

set -eu
. "$(dirname "$0")/../ci/common.sh"
fdk_require_release_posture

VERSION="$(fdk_expected_version)"
# Package metadata version: apk reserves '-' for the -rN pkgrel, so the
# raw "1.1.0-rc1" is "not a valid version"; the underscore form
# (1.1.0_rc1) is apk's native pre-release suffix family and sorts
# before the final. The tarball name and its top directory follow
# pkgver (builddir="$srcdir/fdk-$pkgver"), so they use the SAME
# spelling. Plain releases pass through unchanged (fdk_packver in
# common.sh).
PACKVER="$(fdk_packver apk "$VERSION")"
ARCH="$(uname -m)"

echo "== building FDK $VERSION alpine packages ($ARCH, musl) =="

# --- test suites on the plain tree first ---------------------------------
# FDK_SANITIZE=0: this environment ships no sanitizer runtimes
# (musl / minimal image) — the same debug config without
# -fsanitize; the sanitizer gates run on the glibc targets. FDK_CONFIG
# is pinned on the install line too (config-stamp trap: a plain
# `make install` after a release build silently rebuilds debug
# with sanitizers into the payload).
make FDK_SANITIZE=0
make FDK_SANITIZE=0 test
if command -v Xvfb >/dev/null 2>&1; then
    make FDK_SANITIZE=0 test-x11
else
    echo "XVFB-SKIP: Xvfb not present; the X11 integration suite was not run for this build (disclosed)"
fi
make FDK_SANITIZE=0 FDK_CONFIG=release all

# --- builder user + key (abuild refuses root) ------------------------------
id builder >/dev/null 2>&1 || adduser -D builder
addgroup builder abuild 2>/dev/null || true
WORK="/home/builder/fdk-build"
rm -rf "$WORK"
mkdir -p "$WORK"
tar --exclude=.git --exclude=build --exclude=dist \
    -czf "$WORK/fdk-$PACKVER.tar.gz" \
    --transform "s|^\./|fdk-$PACKVER/|" ./
chown -R builder:builder "$WORK"

su builder -c "abuild-keygen -a -n" </dev/null
# Install the fresh key so apk inside the build trusts it (root-side).
cp /home/builder/.abuild/*.rsa.pub /etc/apk/keys/ 2>/dev/null || true

# Prepare the APKBUILD root-side with the version substituted, then
# let abuild finalize the checksum as the builder, then build.
sed -e "s|@VERSION@|$PACKVER|g" packaging/alpine/APKBUILD > "$WORK/APKBUILD"
chown builder:builder "$WORK/APKBUILD"
su builder -c "cd '$WORK' && abuild checksum" </dev/null
chown builder:builder "$WORK/APKBUILD"
su builder -c "cd '$WORK' && abuild -r" </dev/null

# --- collect the artifacts -------------------------------------------------
APKS="$(find /home/builder/packages -name '*.apk' -type f 2>/dev/null)"
[ -n "$APKS" ] || { echo "BUILD FAIL: abuild produced no packages"; exit 1; }
mkdir -p dist
for a in $APKS; do
    # Alpine artifacts carry no arch in the filename; add it for a
    # self-describing artifact listing (apk installs by content,
    # the filename is presentation only).
    base="$(basename "$a" .apk)"
    case "$base" in
        *-$ARCH) cp "$a" "dist/$base.apk" ;;
        *)       cp "$a" "dist/$base-$ARCH.apk" ;;
    esac
done
ls -la dist/*.apk

echo "== package metadata: =="
for a in dist/*.apk; do
    echo "--- $a"
    tar -xzOf "$a" .PKGINFO 2>/dev/null | grep -E '^(pkgname|pkgver|pkgdesc|arch|depend|license|url)' || true
done

# --- payload verification ---------------------------------------------------
EXT="$(mktemp -d)"
trap 'rm -rf "$EXT" "$WORK"' EXIT
mkdir -p "$EXT/rt"
for a in dist/*.apk; do
    tar -xzf "$a" -C "$EXT/rt"
done
echo "--- merged payload (excluding control files):"
find "$EXT/rt" -type f ! -name '.PKGINFO' | sed "s|$EXT/rt||" | sort | head -30

if fdk_start_xvfb; then
    FDK_PC_DIR="$EXT/rt/usr/lib/pkgconfig" \
FDK_INC_DIR="$EXT/rt/usr/include" \
    FDK_LIB_DIR="$EXT/rt/usr/lib" \
    FDK_DATA_DIR="$EXT/rt/usr/share" \
    fdk_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "BUILD FAIL: graphical smoke test failed"; exit 1; }
    fdk_stop_xvfb
fi

echo "== ALPINE BUILD OK ($VERSION, $ARCH, musl) =="
