#!/bin/sh
# packaging/debian/build-deb.sh — build the FDK .deb packages.
#
# Runs INSIDE a Debian-family environment (a debian:bookworm container
# in CI, or any Debian/Ubuntu host with the build dependencies
# installed: gcc, make, pkg-config, libx11-dev, libxext-dev,
# zlib1g-dev, optionally libwayland-dev + libxkbcommon-dev, and Xvfb +
# fonts-dejavu-core for the X11 test). Does not require root when
# building: staging is done via make install DESTDIR and
# dpkg-deb --root-owner-group.
#
# Produces in dist/:
#   libfdk1_<ver>-1_<arch>.deb   — runtime: libfdk.so.1 + theme faces
#   fdk-dev_<ver>-1_<arch>.deb   — headers, .so dev symlink, fdk.pc, tools
#
# The static archive is deliberately NOT packaged (FDK Proprietary
# License 4(d): distributing statically-linked FDK requires separate
# written authorization).

set -eu
. "$(dirname "$0")/../ci/common.sh"

VERSION="$(fdk_expected_version)"
PKGVER="${VERSION}-1"
ARCH="$(dpkg --print-architecture 2>/dev/null || \
        uname -m | sed 's/^x86_64$/amd64/; s/^aarch64$/arm64/')"
MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)"
[ -n "$MULTIARCH" ] || MULTIARCH="$(cc -dumpmachine 2>/dev/null || true)"
[ -n "$MULTIARCH" ] || MULTIARCH="x86_64-linux-gnu"
LIBDIR="/usr/lib/$MULTIARCH"

echo "== building FDK $VERSION debs (arch $ARCH, libdir $LIBDIR) =="

# --- build + tests -------------------------------------------------------
# NOTE: the FDK Makefile's config stamp means `make release` followed by
# a plain `make install` silently REBUILDS in debug config (sanitizers
# linked into the .so!). The packaging invocations therefore pin
# FDK_CONFIG=release on the install line too — and the NEEDED scan
# below fails loudly if a sanitizer runtime ever leaks into the payload.
make                                   # debug: the sanitizer-checked build
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
make FDK_CONFIG=release all            # release artifacts for the package

# --- stage the full install ---------------------------------------------
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
make FDK_CONFIG=release install DESTDIR="$STAGE" PREFIX=/usr LIBDIR="$LIBDIR"

SO_REAL="libfdk.so.$VERSION"
SO_MAJOR="libfdk.so.$(sed -n 's/^#define FDK_VERSION_MAJOR \([0-9]*\).*/\1/p' include/fdk/fdk_version.h)"

# --- derive the runtime Depends from what the .so actually needs -------
NEEDED="$(objdump -p "$STAGE$LIBDIR/$SO_REAL" | awk '/NEEDED/ {print $NF}' | sort -u)"
RUNTIME_DEPS="$(for soname in $NEEDED; do
    case "$soname" in
        libc.so.6|libdl.so.2|libpthread.so.0) pkg="libc6" ;;
        ld-linux-x86-64.so.2|ld-linux-aarch64.so.1) pkg="libc6" ;;
        libm.so.6)                            pkg="libm6" ;;
        libgcc_s.so.1)                        pkg="libgcc-s1" ;;
        libX11.so.6)                          pkg="libx11-6" ;;
        libXext.so.6)                         pkg="libxext6" ;;
        libz.so.1)                            pkg="zlib1g" ;;
        libwayland-client.so.0)               pkg="libwayland-client0" ;;
        libxkbcommon.so.0)                    pkg="libxkbcommon0" ;;
        libGL.so.1)                           pkg="libgl1" ;;
        *) echo "BUILD FAIL: unmapped NEEDED soname '$soname' — extend the map in packaging/debian/build-deb.sh" >&2; exit 1 ;;
    esac
    echo "$pkg"
done | sort -u | paste -sd ',' -)"
RUNTIME_DEPS="$(printf '%s' "$RUNTIME_DEPS" | tr ',' ', ')"
echo "== .so NEEDED -> Depends: $RUNTIME_DEPS"

# --- split into the two package trees ------------------------------------
RT="$STAGE/pkg-libfdk1"
DV="$STAGE/pkg-fdk-dev"
mkdir -p "$RT/DEBIAN" "$DV/DEBIAN" \
         "$RT/usr/share/doc/libfdk1" "$DV/usr/share/doc/fdk-dev"

# libfdk1: versioned .so + SONAME symlink + themes + docs
mkdir -p "$RT$LIBDIR"
mv "$STAGE$LIBDIR/$SO_REAL" "$RT$LIBDIR/"
mv "$STAGE$LIBDIR/$SO_MAJOR" "$RT$LIBDIR/"
mkdir -p "$RT/usr/share"
mv "$STAGE/usr/share/fdk" "$RT/usr/share/fdk"

# fdk-dev: headers, dev symlink, pkgconfig, tools, docs
mkdir -p "$DV/usr/include"
mv "$STAGE/usr/include/fdk" "$DV/usr/include/fdk"
mkdir -p "$DV$LIBDIR/pkgconfig"
mv "$STAGE$LIBDIR/libfdk.so" "$DV$LIBDIR/libfdk.so"
mv "$STAGE$LIBDIR/pkgconfig/fdk.pc" "$DV$LIBDIR/pkgconfig/"
mkdir -p "$DV/usr/bin"
mv "$STAGE/usr/bin/fdk-theme" "$STAGE/usr/bin/fdk-set" \
   "$STAGE/usr/bin/fdk-prefs" "$DV/usr/bin/"

# The static archive is NOT shipped (see header comment).
rm -f "$STAGE$LIBDIR/libfdk.a"

# leftovers must be empty — anything else means the split is stale
if find "$STAGE" -path "$STAGE/pkg-*" -prune -o -type f -print | grep -q .; then
    echo "BUILD FAIL: unassigned files in the install tree:"
    find "$STAGE" -path "$STAGE/pkg-*" -prune -o -type f -print
    exit 1
fi

# --- docs ----------------------------------------------------------------
for pkgdir in "$RT/usr/share/doc/libfdk1" "$DV/usr/share/doc/fdk-dev"; do
    install -m 644 LICENSE "$pkgdir/LICENSE"
    install -m 644 THIRD-PARTY-NOTICES.md "$pkgdir/THIRD-PARTY-NOTICES.md"
done
install -m 644 packaging/debian/copyright "$RT/usr/share/doc/libfdk1/copyright"
install -m 644 packaging/debian/copyright "$DV/usr/share/doc/fdk-dev/copyright"
DATE="$(date -R)"
sed -e "s|@PKGVER@|$PKGVER|g" -e "s|@VERSION@|$VERSION|g" -e "s|@DATE@|$DATE|g" \
    packaging/debian/changelog > "$RT/usr/share/doc/libfdk1/changelog"
sed -e "s|@PKGVER@|$PKGVER|g" -e "s|@VERSION@|$VERSION|g" -e "s|@DATE@|$DATE|g" \
    packaging/debian/changelog > "$DV/usr/share/doc/fdk-dev/changelog"

# --- DEBIAN control files ------------------------------------------------
sed -e "s|@PKGVER@|$PKGVER|" -e "s|@RUNTIME_DEPS@|$RUNTIME_DEPS|" \
    packaging/debian/control.libfdk1 > "$RT/DEBIAN/control"
sed -e "s|@PKGVER@|$PKGVER|" \
    packaging/debian/control.fdk-dev > "$DV/DEBIAN/control"
printf 'activate-noawait ldconfig\n' > "$RT/DEBIAN/triggers"
for pkgdir in "$RT" "$DV"; do
    (cd "$pkgdir" && find . -type f ! -path './DEBIAN/*' | sed 's|^\./||' | \
     while IFS= read -r f; do md5sum "$f"; done) > "$pkgdir/DEBIAN/md5sums"
done

# --- build the archives ---------------------------------------------------
mkdir -p dist
dpkg-deb --build --root-owner-group "$RT" "dist/libfdk1_${PKGVER}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$DV" "dist/fdk-dev_${PKGVER}_${ARCH}.deb"

echo "== built: =="
ls -la dist/*.deb

# --- payload verification (works without root: extraction-based) --------
echo "== extraction-based payload verification =="
EXT="$STAGE/extract"
mkdir -p "$EXT"
dpkg-deb -x "dist/libfdk1_${PKGVER}_${ARCH}.deb" "$EXT"
dpkg-deb -x "dist/fdk-dev_${PKGVER}_${ARCH}.deb" "$EXT"
dpkg-deb -I "dist/libfdk1_${PKGVER}_${ARCH}.deb" | sed -n '1,12p'
echo "--- libfdk1 contents:"
dpkg-deb -c "dist/libfdk1_${PKGVER}_${ARCH}.deb" | awk '{print $NF}' | grep -v '/$' | sort
echo "--- fdk-dev contents:"
dpkg-deb -c "dist/fdk-dev_${PKGVER}_${ARCH}.deb" | awk '{print $NF}' | grep -v '/$' | sort

if fdk_start_xvfb; then
    FDK_PC_DIR="$EXT$LIBDIR/pkgconfig" \
FDK_INC_DIR="$EXT/usr/include" \
    FDK_LIB_DIR="$EXT$LIBDIR" \
    FDK_DATA_DIR="$EXT/usr/share" \
    fdk_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "BUILD FAIL: graphical smoke test failed"; exit 1; }
    fdk_stop_xvfb
fi

echo "== DEB BUILD OK ($VERSION, arch $ARCH) =="
