#!/bin/sh
# fdk-shell/packaging/arch/build-pkg.sh — build the fdk-shell Arch
# package.
#
# Runs INSIDE the archlinux:base container (root) AFTER FDK's own
# build-pkg.sh in the same container run: FDK's pkg is installed
# first, fdk-shell's X11 suite runs against the installed FDK, then
# makepkg builds from a pristine fdk-shell-only tarball (builder
# user; no FDK source in the makepkg tree — the isolation contract
# by construction).

set -eu
. "$(dirname "$0")/../ci/common.sh"
fdk_require_release_posture

VERSION="$(fdkshell_version)"
PACKVER="$(fdk_packver arch "$VERSION")"
MINVER="$(fdkshell_min_fdk_ver arch)"
ARCH="$(uname -m)"

echo "== building fdk-shell $VERSION arch package ($ARCH, FDK floor $MINVER) =="

# --- install FDK's package (built by FDK's build-pkg.sh earlier) -------
FDK_PKG="$(ls dist/fdk-*.pkg.tar.* 2>/dev/null | head -1)"
[ -n "$FDK_PKG" ] || { echo "BUILD FAIL: no FDK package in dist/ (run FDK's build-pkg.sh first)"; exit 1; }
pacman -U --noconfirm "$FDK_PKG"
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

# --- builder workspace (makepkg forbids root) ---------------------------
WORK="/tmp/fdkshell-makepkg"
rm -rf "$WORK"
mkdir -p "$WORK/src"
id builder >/dev/null 2>&1 || useradd -m builder
# The tarball carries ONLY the fdk-shell tree + the project's license
# documents (PKGBUILD's package() installs them); the FDK dependency
# arrives through the installed pkg above.
STAGING="$(mktemp -d)"
mkdir -p "$STAGING"
tar --exclude=build --exclude=build-isolation -C fdk-shell -cf - . | \
    (cd "$STAGING" && tar -xf -)
install -m 644 LICENSE THIRD-PARTY-NOTICES.md "$STAGING/"
tar -czf "$WORK/fdk-shell-$PACKVER.tar.gz" \
    --transform "s|^\./|fdk-shell-$PACKVER/|" \
    -C "$STAGING" ./
rm -rf "$STAGING"
SHA="$(sha256sum "$WORK/fdk-shell-$PACKVER.tar.gz" | awk '{print $1}')"
sed -e "s|@VERSION@|$PACKVER|g" -e "s|@MINVER@|$MINVER|g" -e "s|@SHA256@|$SHA|" \
    fdk-shell/packaging/arch/PKGBUILD > "$WORK/PKGBUILD"
chown -R builder:builder "$WORK"

su builder -s /bin/sh -c "
    set -eu
    cd '$WORK'
    makepkg --force
"
PKG="$(find "$WORK" -maxdepth 1 -name 'fdk-shell-*.pkg.tar.*' -type f ! -name '*debug*')"
[ -n "$PKG" ] || { echo "BUILD FAIL: makepkg produced no package"; exit 1; }
mkdir -p dist
cp "$PKG" dist/
ls -la dist/*fdk-shell*

echo "== package metadata: =="
tar -xOf "$PKG" .PKGINFO | grep -E '^(pkgname|pkgver|pkgdesc|arch|depend|license|url)' || true

# --- payload verification -------------------------------------------------
EXT="$(mktemp -d)"
trap 'rm -rf "$EXT" "$WORK"' EXIT
tar -I zstd -xf dist/"$(basename "$PKG")" -C "$EXT" 2>/dev/null || \
    tar -xf dist/"$(basename "$PKG")" -C "$EXT"
echo "--- extracted payload (excluding .PKGINFO/.MTREE):"
find "$EXT" -type f ! -name '.PKGINFO' ! -name '.MTREE' | sed "s|$EXT||" | sort

# --- the installed-stack smoke --------------------------------------------
pacman -U --noconfirm dist/"$(basename "$PKG")"
if fdk_start_xvfb; then
    fdkshell_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "BUILD FAIL: fdk-shell graphical smoke test"; exit 1; }
    fdk_stop_xvfb
fi

echo "== FDK-SHELL ARCH BUILD OK ($VERSION, $ARCH) =="
