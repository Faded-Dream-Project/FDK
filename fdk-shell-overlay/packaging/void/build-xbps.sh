#!/bin/sh
# fdk-shell-overlay/packaging/void/build-xbps.sh — build the fdk-shell-overlay Void
# Linux packages.
#
# Runs INSIDE the void-glibc container (root) AFTER FDK's own
# build-xbps.sh in the same container run: FDK's xbps packages are
# installed first (local repository), fdk-shell-overlay's X11 suite runs
# against the installed FDK, then xbps-create builds the packages
# from a staged install.
#
# Uses xbps-create (the native low-level packager) with the metadata
# from fdk-shell-overlay/packaging/void/template: fdk-shell-overlay (runtime) +
# fdk-shell-overlay-devel (development files).

set -eu
. "$(dirname "$0")/../ci/common.sh"
fdk_require_release_posture

VERSION="$(fdkshell_version)"
PACKVER="$(fdk_packver xbps "$VERSION")"
MINVER="$(fdkshell_min_fdk_ver xbps)"
FDK_PACKVER="$(fdk_packver xbps "$(fdk_expected_version)")"
REVISION=1
ARCH="$(uname -m)"

echo "== building fdk-shell-overlay $VERSION XBPS packages ($ARCH, FDK floor $MINVER) =="

# --- install FDK's packages (built by FDK's build-xbps.sh earlier) ------
# A local repository + install: dependency resolution pulls fdk-libs
# in for fdk-devel — the same machinery this package's own dependency
# will ride.
FDK_XBPS_RT="dist/fdk-libs-${FDK_PACKVER}_${REVISION}.x86_64.xbps"
FDK_XBPS_DV="dist/fdk-devel-${FDK_PACKVER}_${REVISION}.x86_64.xbps"
[ -f "$FDK_XBPS_RT" ] && [ -f "$FDK_XBPS_DV" ] || { echo "BUILD FAIL: no FDK xbps packages in dist/ (run FDK's build-xbps.sh first)"; exit 1; }
xbps-rindex -a "$FDK_XBPS_RT" "$FDK_XBPS_DV" 2>/dev/null || \
    xbps-rindex --add "$FDK_XBPS_RT" "$FDK_XBPS_DV"
xbps-install -y --repository "$(pwd)/dist" fdk-devel
echo "== FDK installed: pkg-config fdk = $(pkg-config --modversion fdk) =="

# --- build + test against the INSTALLED FDK ----------------------------
make -C fdk-shell-overlay clean >/dev/null
make -C fdk-shell-overlay FDKSHELL_SANITIZE=0 static shared \
    FDK_CFLAGS="$(pkg-config --cflags fdk)" \
    FDK_LIBS="$(pkg-config --libs fdk)" \
    FDK_SO_LIBS="$(pkg-config --libs fdk)"
if command -v Xvfb >/dev/null 2>&1; then
    # FDKSHELL_SANITIZE=0: this environment ships no sanitizer runtimes
    # (minimal image) — the same posture as FDK's own void build; the
    # sanitizer gates run on the glibc targets.
    make -C fdk-shell-overlay FDKSHELL_SANITIZE=0 test-x11 \
        FDK_CFLAGS="$(pkg-config --cflags fdk)" \
        FDK_LIBS="$(pkg-config --libs fdk)" \
        FDK_SO_LIBS="$(pkg-config --libs fdk)"
else
    echo "XVFB-SKIP: Xvfb not present; fdk-shell-overlay's X11 suite not run for this build (disclosed)"
fi

# --- stage + split ---------------------------------------------------------
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
make -C fdk-shell-overlay CONFIG=release FDKSHELL_SANITIZE=0 install DESTDIR="$STAGE" \
    PREFIX=/usr LIBDIR=/usr/lib \
    FDK_CFLAGS="$(pkg-config --cflags fdk)" \
    FDK_LIBS="$(pkg-config --libs fdk)" \
    FDK_SO_LIBS="$(pkg-config --libs fdk)"
rm -f "$STAGE/usr/lib/libfdk-shell-overlay.a"

RT="$STAGE/stow-fdk-shell-overlay"
DV="$STAGE/stow-fdk-shell-overlay-devel"
mkdir -p "$RT/usr/lib" "$DV/usr/lib/pkgconfig" "$DV/usr/include"

# fdk-shell-overlay: the versioned .so + SONAME symlink
mv "$STAGE/usr/lib/libfdk-shell-overlay.so.$VERSION" "$RT/usr/lib/"
mv "$STAGE/usr/lib/libfdk-shell-overlay.so.0" "$RT/usr/lib/"
mkdir -p "$RT/usr/share/doc/fdk-shell-overlay"
install -m 644 LICENSE "$RT/usr/share/doc/fdk-shell-overlay/LICENSE"
install -m 644 THIRD-PARTY-NOTICES.md "$RT/usr/share/doc/fdk-shell-overlay/THIRD-PARTY-NOTICES.md"

# fdk-shell-overlay-devel: header, dev symlink, pkgconfig
mv "$STAGE/usr/lib/libfdk-shell-overlay.so" "$DV/usr/lib/"
mv "$STAGE/usr/lib/pkgconfig/fdk-shell-overlay.pc" "$DV/usr/lib/pkgconfig/"
mv "$STAGE/usr/include/fdk-shell-overlay" "$DV/usr/include/fdk-shell-overlay"
mkdir -p "$DV/usr/share/doc/fdk-shell-overlay-devel"
install -m 644 LICENSE "$DV/usr/share/doc/fdk-shell-overlay-devel/LICENSE"
install -m 644 THIRD-PARTY-NOTICES.md "$DV/usr/share/doc/fdk-shell-overlay-devel/THIRD-PARTY-NOTICES.md"

# leftovers must be empty — anything else means the split is stale
if find "$STAGE" -path "$STAGE/stow-*" -prune -o -type f -print | grep -q .; then
    echo "BUILD FAIL: unassigned files in the fdk-shell-overlay install tree:"
    find "$STAGE" -path "$STAGE/stow-*" -prune -o -type f -print
    exit 1
fi

# --- xbps-create: one package per stow dir ---------------------------------
mkdir -p dist
xbps-create \
    -A "$ARCH" \
    -n "fdk-shell-overlay-$PACKVER"_"$REVISION" \
    -s "Layer-shell surfaces for FDK - runtime library" \
    -S "The fdk-shell-overlay runtime: the shared library (libfdk-shell-overlay.so.0), linked against the FDK runtime with a versioned dependency on the FDK that introduced the extension API it needs. Applications built with fdk-shell-overlay remain the property of their authors; the runtimes are installed separately by the end user per the FDK Proprietary License, Section 4(d)." \
    -D "fdk-libs>=$MINVER" \
    -m "FDK maintainers <https://github.com/Faded-Dream-Project/FDK>" \
    -l "Proprietary" \
    -H "https://github.com/Faded-Dream-Project/FDK" \
    "$RT"

xbps-create \
    -A "$ARCH" \
    -n "fdk-shell-overlay-devel-$PACKVER"_"$REVISION" \
    -s "Layer-shell surfaces for FDK - development files" \
    -S "The fdk-shell-overlay public header and pkg-config metadata (Requires: fdk). The static archive is deliberately not shipped: distributing statically-linked FDK requires separate written authorization." \
    -D "fdk-shell-overlay-$PACKVER"_"$REVISION fdk-devel>=$MINVER" \
    -m "FDK maintainers <https://github.com/Faded-Dream-Project/FDK>" \
    -l "Proprietary" \
    -H "https://github.com/Faded-Dream-Project/FDK" \
    "$DV"

# xbps-create writes ./<name>_<rev>.<arch>.xbps into the CURRENT
# directory — the same collection line FDK's own build uses.
mv ./*.xbps dist/ 2>/dev/null || true

SHELL_XBPS_RT="dist/fdk-shell-overlay-$PACKVER"_"$REVISION.$ARCH.xbps"
SHELL_XBPS_DV="dist/fdk-shell-overlay-devel-$PACKVER"_"$REVISION.$ARCH.xbps"
[ -f "$SHELL_XBPS_RT" ] && [ -f "$SHELL_XBPS_DV" ] || { echo "BUILD FAIL: xbps-create produced no packages"; ls dist/; exit 1; }

echo "== built: =="
ls -la dist/*fdk-shell-overlay*

# --- metadata + payload verification ---------------------------------------
for x in "$SHELL_XBPS_RT" "$SHELL_XBPS_DV"; do
    echo "--- $x"
    xbps-query --repository=dist -s fdk-shell-overlay 2>/dev/null | head -4 || true
    tar -xJf "$x" ./props 2>/dev/null || tar -xf "$x" ./props 2>/dev/null || true
    if [ -f props ]; then
        grep -E '^(pkgname|version|depends|short_desc)' props || true
        rm -f props
    fi
done

# --- the installed-stack smoke ---------------------------------------------
xbps-rindex -a "$SHELL_XBPS_RT" "$SHELL_XBPS_DV" 2>/dev/null || \
    xbps-rindex --add "$SHELL_XBPS_RT" "$SHELL_XBPS_DV"
xbps-install -y --repository "$(pwd)/dist" fdk-shell-overlay-devel
if fdk_start_xvfb; then
    fdkshell_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "BUILD FAIL: fdk-shell-overlay graphical smoke test"; exit 1; }
    fdk_stop_xvfb
fi

echo "== FDK-SHELL XBPS BUILD OK ($VERSION, $ARCH) =="
