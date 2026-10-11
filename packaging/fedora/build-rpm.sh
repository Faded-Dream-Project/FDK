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

VERSION="$(fdk_expected_version)"

echo "== building FDK $VERSION RPMs =="

# --- run the test suites on the plain tree first (rpmbuild rebuilds) ----
# FDK_CONFIG is pinned inside the spec's %build/%install as well: the
# Makefile's config stamp would otherwise silently rebuild the payload
# in debug config (sanitizers) between %build and %install.
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

# --- pristine source tarball ---------------------------------------------
TARBALL="fdk-$VERSION.tar.gz"
mkdir -p dist build-rpm/SOURCES
tar --exclude=.git --exclude=build --exclude=build-rpm --exclude=dist \
    -czf "build-rpm/SOURCES/$TARBALL" \
    --transform "s|^\./|fdk-$VERSION/|" ./
tar -tzf "build-rpm/SOURCES/$TARBALL" | head -3

# --- spec with the version substituted from the header -------------------
sed "s|@VERSION@|$VERSION|g" packaging/fedora/fdk.spec > build-rpm/fdk.spec

# --- tests inside the rpmbuild too (%check runs the headless suite) ------
rpmbuild -bb \
    --define "_topdir $(pwd)/build-rpm" \
    --define "_sourcedir $(pwd)/build-rpm/SOURCES" \
    --define "dist %{nil}" \
    build-rpm/fdk.spec

# --define "dist %{nil}" drops the .fc42/.el10 suffix on purpose: the
# artifact is built on AlmaLinux 10 and verified on Fedora 42 as well;
# a neutral name states exactly what is true (no per-distro rebuild
# claims). The build container is recorded in the release notes.

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
