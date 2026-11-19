#!/bin/sh
# packaging/fedora/build-rpm.sh — build the FDK RPMs.
#
# Runs INSIDE the RPM build container (AlmaLinux 10 in CI) with the
# build dependencies already installed by the orchestrator
# (packaging/ci/fedora.sh): gcc, make, pkgconf-pkg-config,
# libX11-devel, libXext-devel, zlib-devel, wayland-devel,
# libxkbcommon-devel (+ dejavu-sans-fonts, dejavu-sans-mono-fonts,
# xorg-x11-server-Xvfb for the tests).
#
# Builds from a pristine source tarball (fdk-<version>.tar.gz, top
# directory fdk-<version>/) through rpmbuild, then verifies the
# payload and runs the extraction-based smoke test.

set -eu
. "$(dirname "$0")/../ci/common.sh"
fdk_require_release_posture

VERSION="$(fdk_expected_version)"
# Package metadata version: rpm reserves '-' for the Version-Release
# separator, so the raw "1.0.0-rc1" is an illegal Version ("Illegal
# char '-'"); the tilde form (1.0.0~rc1) is rpm's native pre-release
# spelling and sorts before the final. The tarball name and its top
# directory follow the spec's Version (%setup -n fdk-%{version}), so
# they use the SAME spelling; the %files wildcards (libfdk.so.1.*)
# match the raw-named real file either way. Plain releases pass
# through unchanged (fdk_packver in common.sh).
PACKVER="$(fdk_packver rpm "$VERSION")"

echo "== building FDK $VERSION RPMs =="

# --- run the test suites on the plain tree first (rpmbuild rebuilds) ----
# FDK_CONFIG is pinned inside the spec's %build/%install as well: the
# Makefile's config stamp would otherwise silently rebuild the payload
# in debug config (sanitizers) between %build and %install.
make
ASAN_OPTIONS=detect_leaks=0 make test
# The X11 integration suite (make test-x11) is deliberately NOT a
# packaging gate (2026-11-19 decision): CI's ubuntu job already runs
# it sanitized with the strict leak gate on a throwaway WM-free
# Xvfb, so a per-distro re-run proved nothing about the PAYLOAD —
# it re-tested the same source tree with a WEAKER posture, doubled
# the container time, and (the triggering report) made package
# builds hostage to the launcher's desktop session: the Makefile
# target prefers $DISPLAY when set, so build-rpm.sh launched from a
# live desktop ran the suite against the real WM instead of Xvfb —
# where a WM-sensitivity bug in the suite (fixed the same day)
# aborted the build. The per-distro graphical proof that packaging
# actually needs still runs: the install smoke test links against
# the extracted payload and opens a real window under Xvfb below.

# --- pristine source tarball ---------------------------------------------
# PACKVER (not VERSION): the tarball name and top directory must match
# the spec's Version field, which %setup -n fdk-%{version} derives.
TARBALL="fdk-$PACKVER.tar.gz"
mkdir -p dist build-rpm/SOURCES
tar --exclude=.git --exclude=build --exclude=build-rpm --exclude=dist \
    -czf "build-rpm/SOURCES/$TARBALL" \
    --transform "s|^\./|fdk-$PACKVER/|" ./
tar -tzf "build-rpm/SOURCES/$TARBALL" | head -3

# --- spec with the version substituted from the header -------------------
sed "s|@VERSION@|$PACKVER|g" packaging/fedora/fdk.spec > build-rpm/fdk.spec

# --- tests inside the rpmbuild too (%check runs the headless suite) ------
rpmbuild -bb \
    --define "_topdir $(pwd)/build-rpm" \
    --define "_sourcedir $(pwd)/build-rpm/SOURCES" \
    --define "dist %{nil}" \
    build-rpm/fdk.spec

# --define "dist %{nil}" drops the .fc42/.el10 suffix on purpose: the
# artifact is built on AlmaLinux 10 and verified on Fedora 42 as well;
# a neutral name states exactly what is true (no per-distro rebuild
# claims). The build container is recorded in the CI job log.

RPMS="$(find build-rpm/RPMS -name '*.rpm' -type f)"
echo "== built: =="
echo "$RPMS"
for r in $RPMS; do
    cp "$r" "dist/"
done
ls -la dist/*.rpm

# --- payload verification -------------------------------------------------
for r in dist/*.rpm; do
    echo "--- $r"
    rpm -qlp "$r"
    rpm -qip "$r" | sed -n '1,10p'
done

EXT="$(mktemp -d)"
trap 'rm -rf "$EXT" build-rpm' EXIT
for r in dist/*.rpm; do
    rpm2cpio "$r" | (cd "$EXT" && cpio -idmu --quiet)
done
echo "--- extracted tree:"
find "$EXT" -type f | sed "s|$EXT||" | sort

if fdk_start_xvfb; then
    FDK_PC_DIR="$EXT/usr/lib64/pkgconfig" \
FDK_INC_DIR="$EXT/usr/include" \
    FDK_LIB_DIR="$EXT/usr/lib64" \
    FDK_DATA_DIR="$EXT/usr/share" \
    fdk_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "BUILD FAIL: graphical smoke test failed"; exit 1; }
    fdk_stop_xvfb
fi

echo "== RPM BUILD OK ($VERSION) =="
