#!/bin/sh
# packaging/void/build-xbps.sh — build the FDK Void Linux packages.
#
# Runs INSIDE the void-glibc container (root) with the build
# dependencies already installed by the orchestrator
# (packaging/ci/void.sh): base-devel, pkgconf, libX11-devel,
# libXext-devel, zlib-devel, wayland-devel, libxkbcommon-devel
# (+ dejavu-fonts-ttf and xorg-server-xvfb when available — see the
# XVFB-SKIP disclosure otherwise).
#
# Uses xbps-create (the native low-level packager) with the metadata
# from packaging/void/template: fdk-libs (runtime) + fdk-devel
# (development files). glibc Void only — Void/musl would need the
# musl toolchain variant and is not produced by this pipeline.

set -eu
. "$(dirname "$0")/../ci/common.sh"

VERSION="$(fdk_expected_version)"
REVISION=1
ARCH="$(uname -m)"

echo "== building FDK $VERSION XBPS packages ($ARCH, glibc) =="

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

# --- stage + split ---------------------------------------------------------
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
make FDK_SANITIZE=0 FDK_CONFIG=release install DESTDIR="$STAGE" PREFIX=/usr LIBDIR=/usr/lib

SO_REAL="libfdk.so.$VERSION"
SO_MAJOR="libfdk.so.$(sed -n 's/^#define FDK_VERSION_MAJOR \([0-9]*\).*/\1/p' include/fdk/fdk_version.h)"

RT="$STAGE/stow-fdk-libs"
DV="$STAGE/stow-fdk-devel"
mkdir -p "$RT/usr/lib" "$DV/usr/lib/pkgconfig" "$DV/usr/include" "$DV/usr/bin"

# fdk-libs: the versioned .so + SONAME symlink + themes
mv "$STAGE/usr/lib/$SO_REAL" "$STAGE/usr/lib/$SO_MAJOR" "$RT/usr/lib/"
mkdir -p "$RT/usr/share"
mv "$STAGE/usr/share/fdk" "$RT/usr/share/fdk"
mkdir -p "$RT/usr/share/doc/fdk-libs"
install -m 644 LICENSE "$RT/usr/share/doc/fdk-libs/LICENSE"
install -m 644 THIRD-PARTY-NOTICES.md "$RT/usr/share/doc/fdk-libs/THIRD-PARTY-NOTICES.md"

# fdk-devel: headers, dev symlink, pkgconfig, tools
mv "$STAGE/usr/lib/libfdk.so" "$DV/usr/lib/libfdk.so"
mv "$STAGE/usr/lib/pkgconfig/fdk.pc" "$DV/usr/lib/pkgconfig/"
mv "$STAGE/usr/include/fdk" "$DV/usr/include/fdk"
mv "$STAGE/usr/bin/fdk-theme" "$STAGE/usr/bin/fdk-set" \
   "$STAGE/usr/bin/fdk-prefs" "$DV/usr/bin/"
mkdir -p "$DV/usr/share/doc/fdk-devel"
install -m 644 LICENSE "$DV/usr/share/doc/fdk-devel/LICENSE"
install -m 644 THIRD-PARTY-NOTICES.md "$DV/usr/share/doc/fdk-devel/THIRD-PARTY-NOTICES.md"

# The static archive is NOT shipped (license posture, see template).
rm -f "$STAGE/usr/lib/libfdk.a"
if find "$STAGE" -path "$STAGE/stow-*" -prune -o -type f -print | grep -q .; then
    echo "BUILD FAIL: unassigned files in the install tree:"
    find "$STAGE" -path "$STAGE/stow-*" -prune -o -type f -print
    exit 1
fi

# --- xbps-create: one package per stow dir ---------------------------------
mkdir -p dist
# xbps-create flag map (short letters are not mnemonic: -S is the
# long description, -D the dependency list, -H the homepage).
xbps-create \
    -A "$ARCH" \
    -n "fdk-libs-$VERSION"_"$REVISION" \
    -s "Faded Dream ToolKit - runtime library and theme faces" \
    -S "The FDK runtime: the shared library (libfdk.so.1) and the ten shipped theme faces. Applications built with FDK need this package at run time; the FDK runtime is installed separately by the end user per the license's separate-runtime model." \
    -m "FDK maintainers <https://github.com/FemBoyGamerTechGuy/FDK>" \
    -l "Proprietary" \
    -H "https://github.com/FemBoyGamerTechGuy/FDK" \
    "$RT"

xbps-create \
    -A "$ARCH" \
    -n "fdk-devel-$VERSION"_"$REVISION" \
    -s "Faded Dream ToolKit - development files" \
    -S "Headers, pkg-config metadata, the development symlink, and the fdk-theme / fdk-set / fdk-prefs tools. The static archive is deliberately not shipped: distributing statically-linked FDK requires separate written authorization." \
    -m "FDK maintainers <https://github.com/FemBoyGamerTechGuy/FDK>" \
    -l "Proprietary" \
    -H "https://github.com/FemBoyGamerTechGuy/FDK" \
    -D "fdk-libs-$VERSION"_"$REVISION" \
    "$DV"

mv ./*.xbps dist/ 2>/dev/null || true
ls -la dist/*.xbps

echo "== package metadata: =="
for x in dist/*.xbps; do
    echo "--- $x"
    xbps-query --repository="$(pwd)/dist" -S -p pkgver "fdk-libs" >/dev/null 2>&1 || true
    xbps-uhelper fmtmsg "$(basename "$x" .xbps)" 2>/dev/null || true
done

# --- payload verification ---------------------------------------------------
EXT="$STAGE/extract"
mkdir -p "$EXT"
for x in dist/*.xbps; do
    tar -xJf "$x" -C "$EXT" 2>/dev/null || tar -xf "$x" -C "$EXT"
done
echo "--- merged payload (excluding control files):"
find "$EXT" -type f ! -name '.XBPS_*' ! -name 'props.plist' | sed "s|$EXT||" | sort | head -30

if fdk_start_xvfb; then
    FDK_PC_DIR="$EXT/usr/lib/pkgconfig" \
FDK_INC_DIR="$EXT/usr/include" \
    FDK_LIB_DIR="$EXT/usr/lib" \
    FDK_DATA_DIR="$EXT/usr/share" \
    fdk_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "BUILD FAIL: graphical smoke test failed"; exit 1; }
    fdk_stop_xvfb
fi

echo "== XBPS BUILD OK ($VERSION, $ARCH) =="
