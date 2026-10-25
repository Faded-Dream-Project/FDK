#!/bin/sh
# fdk-shell/packaging/fedora/build-rpm.sh — build the fdk-shell RPMs.
#
# Runs INSIDE the RPM build container (AlmaLinux 10 in CI) AFTER
# FDK's packaging/fedora/build-rpm.sh, in the same container run:
# dist/ carries FDK's rpms, which this script INSTALLS first — the
# fdk-shell rpmbuild then compiles against the installed public FDK
# (and the rpmbuild chroot carries only the fdk-shell tarball: the
# isolation contract holds by construction).
#
# The fdk-shell X11 test suite runs against the INSTALLED FDK before
# rpmbuild (the chroot itself has no display for %check — disclosed
# in the spec).

set -eu
. "$(dirname "$0")/../ci/common.sh"
fdk_require_release_posture

VERSION="$(fdkshell_version)"
PACKVER="$(fdk_packver rpm "$VERSION")"
MINVER="$(fdkshell_min_fdk_ver rpm)"
FDK_VERSION_RAW="$(fdk_expected_version)"

echo "== building fdk-shell $VERSION RPMs (FDK floor $MINVER) =="

# --- install FDK's rpms (built by FDK's build-rpm.sh earlier) ----------
FDK_RPMS="$(find dist -maxdepth 1 -name 'libfdk1-*.rpm' -o -maxdepth 1 -name 'fdk-devel-*.rpm' -type f 2>/dev/null || true)"
[ -n "$FDK_RPMS" ] || { echo "BUILD FAIL: no FDK rpms in dist/ (run FDK's build-rpm.sh first)"; exit 1; }
# shellcheck disable=SC2086
rpm -Uvh $FDK_RPMS
echo "== FDK installed: pkg-config fdk = $(pkg-config --modversion fdk) =="

# --- build + test against the INSTALLED FDK ----------------------------
make -C fdk-shell clean >/dev/null
make -C fdk-shell static shared \
    FDK_CFLAGS="$(pkg-config --cflags fdk)" \
    FDK_LIBS="$(pkg-config --libs fdk)" \
    FDK_SO_LIBS="$(pkg-config --libs fdk)"
if command -v Xvfb >/dev/null 2>&1; then
    LSAN_OPTIONS=exitcode=0 make -C fdk-shell test-x11 \
        FDK_CFLAGS="$(pkg-config --cflags fdk)" \
        FDK_LIBS="$(pkg-config --libs fdk)" \
        FDK_SO_LIBS="$(pkg-config --libs fdk)"
else
    echo "XVFB-SKIP: Xvfb not present; fdk-shell's X11 suite not run for this build (disclosed)"
fi

# --- pristine fdk-shell-only tarball (the isolation shape: no FDK
# source rides along; the chroot cannot see the sibling checkout) -------
# The tarball carries ONLY the fdk-shell tree + the project's license
# documents (the spec's %license installs them).
TARBALL="fdk-shell-$PACKVER.tar.gz"
mkdir -p dist build-rpm-shell/SOURCES
STAGING="$(mktemp -d)"
mkdir -p "$STAGING"
tar --exclude=build --exclude=build-isolation -C fdk-shell -cf - . | \
    (cd "$STAGING" && tar -xf -)
install -m 644 LICENSE THIRD-PARTY-NOTICES.md "$STAGING/"
tar -czf "build-rpm-shell/SOURCES/$TARBALL" \
    --transform "s|^\./|fdk-shell-$PACKVER/|" \
    -C "$STAGING" ./
tar -tzf "build-rpm-shell/SOURCES/$TARBALL" | head -3
if tar -tzf "build-rpm-shell/SOURCES/$TARBALL" | grep -qE '(^|/)\.\./|include/fdk/'; then
    echo "BUILD FAIL: the fdk-shell tarball carries FDK files"
    exit 1
fi
rm -rf "$STAGING"

# --- spec with the versions substituted --------------------------------
sed -e "s|@VERSION@|$PACKVER|g" -e "s|@MINVER@|$MINVER|g" \
    fdk-shell/packaging/fedora/fdk-shell.spec > build-rpm-shell/fdk-shell.spec

rpmbuild -bb \
    --define "_topdir $(pwd)/build-rpm-shell" \
    --define "_sourcedir $(pwd)/build-rpm-shell/SOURCES" \
    --define "dist %{nil}" \
    build-rpm-shell/fdk-shell.spec

RPMS="$(find build-rpm-shell/RPMS -name '*.rpm' -type f)"
echo "== built: =="
echo "$RPMS"
for r in $RPMS; do
    cp "$r" "dist/"
done
ls -la dist/*fdk-shell*

# --- payload + metadata verification -----------------------------------
for r in dist/fdk-shell-*.rpm dist/libfdk-shell0-*.rpm; do
    [ -f "$r" ] || continue
    echo "--- $r"
    rpm -qlp "$r"
    rpm -qip "$r" | sed -n '1,10p'
    rpm -qp --requires "$r" | grep -E 'libfdk|fdk' || true
done

# --- the installed-stack smoke -----------------------------------------
if fdk_start_xvfb; then
    for r in dist/libfdk-shell0-*.rpm dist/fdk-shell-devel-*.rpm; do
        [ -f "$r" ] || continue
        rpm -Uvh "$r"
    done
    fdkshell_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "BUILD FAIL: fdk-shell graphical smoke test"; exit 1; }
    fdk_stop_xvfb
fi

rm -rf build-rpm-shell
echo "== FDK-SHELL RPM BUILD OK ($VERSION) =="
