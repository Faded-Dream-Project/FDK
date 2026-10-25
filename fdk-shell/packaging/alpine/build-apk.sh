#!/bin/sh
# fdk-shell/packaging/alpine/build-apk.sh — build the fdk-shell
# Alpine packages.
#
# Runs INSIDE the alpine:3.22 container (root) AFTER FDK's own
# build-apk.sh in the same container run: FDK's apks are installed
# first, fdk-shell's X11 suite runs against the installed FDK, then
# abuild builds from a pristine fdk-shell-only tarball (builder user;
# no FDK source in the abuild tree — the isolation contract by
# construction).

set -eu
. "$(dirname "$0")/../ci/common.sh"
fdk_require_release_posture

VERSION="$(fdkshell_version)"
PACKVER="$(fdk_packver apk "$VERSION")"
MINVER="$(fdkshell_min_fdk_ver apk)"
ARCH="$(uname -m)"

echo "== building fdk-shell $VERSION alpine packages ($ARCH, musl, FDK floor $MINVER) =="

# --- install FDK's packages (built by FDK's build-apk.sh earlier) ------
FDK_APK_RT="$(ls dist/fdk-[0-9]*.apk 2>/dev/null | head -1)"
FDK_APK_DV="$(ls dist/fdk-dev-*.apk 2>/dev/null | head -1)"
[ -n "$FDK_APK_RT" ] && [ -n "$FDK_APK_DV" ] || { echo "BUILD FAIL: no FDK apks in dist/ (run FDK's build-apk.sh first)"; exit 1; }
apk add --allow-untrusted --repository /dev/null "$FDK_APK_RT" "$FDK_APK_DV" 2>/dev/null || \
    apk add --allow-untrusted "$FDK_APK_RT" "$FDK_APK_DV"
echo "== FDK installed: pkg-config fdk = $(pkg-config --modversion fdk) =="

# --- build + test against the INSTALLED FDK ----------------------------
make -C fdk-shell clean >/dev/null
make -C fdk-shell FDKSHELL_SANITIZE=0 static shared \
    FDK_CFLAGS="$(pkg-config --cflags fdk)" \
    FDK_LIBS="$(pkg-config --libs fdk)" \
    FDK_SO_LIBS="$(pkg-config --libs fdk)"
if command -v Xvfb >/dev/null 2>&1; then
    LSAN_OPTIONS=exitcode=0 make -C fdk-shell FDKSHELL_SANITIZE=0 test-x11 \
        FDK_CFLAGS="$(pkg-config --cflags fdk)" \
        FDK_LIBS="$(pkg-config --libs fdk)" \
        FDK_SO_LIBS="$(pkg-config --libs fdk)"
else
    echo "XVFB-SKIP: Xvfb not present; fdk-shell's X11 suite not run for this build (disclosed)"
fi

# --- builder user + key (abuild refuses root) ----------------------------
id builder >/dev/null 2>&1 || adduser -D builder
addgroup builder abuild 2>/dev/null || true
WORK="/home/builder/fdkshell-build"
rm -rf "$WORK"
mkdir -p "$WORK"

# --- pristine fdk-shell-only source (the isolation shape) ---------------
# The tarball carries ONLY the fdk-shell tree + the project's license
# documents (APKBUILD's package() installs them); no FDK source rides
# along — the FDK dependency arrives through the installed packages.
STAGING="$WORK/staging"
mkdir -p "$STAGING"
tar --exclude=build --exclude=build-isolation -C fdk-shell -cf - . | \
    (cd "$STAGING" && tar -xf -)
install -m 644 LICENSE THIRD-PARTY-NOTICES.md "$STAGING/"
tar -czf "$WORK/fdk-shell-$PACKVER.tar.gz" \
    --transform "s|^\./|fdk-shell-$PACKVER/|" \
    -C "$STAGING" ./
SHA="$(sha512sum "$WORK/fdk-shell-$PACKVER.tar.gz" | awk '{print $1}')"

mkdir -p "$WORK/src"
sed -e "s|@VERSION@|$PACKVER|g" -e "s|@MINVER@|$MINVER|g" -e "s|@SHA512@|$SHA|" \
    fdk-shell/packaging/alpine/APKBUILD > "$WORK/APKBUILD"
chown -R builder:builder "$WORK"

# abuild needs the sources under $HOME/<...>/src or $PWD/src keyed by
# pkgname; run from $WORK with src/ present.
su builder -s /bin/sh -c "
    set -eu
    cd '$WORK'
    abuild -r -F || abuild -r
"
APKS="$(find "$WORK" -name 'fdk-shell*.apk' -type f 2>/dev/null || true)"
[ -n "$APKS" ] || { echo "BUILD FAIL: abuild produced no packages"; find "$WORK" -name '*.apk' -o -name 'APKBUILD' | head; exit 1; }
mkdir -p dist
for a in $APKS; do
    cp "$a" dist/
done
ls -la dist/*fdk-shell*

# --- metadata + payload verification --------------------------------------
for a in dist/fdk-shell*.apk; do
    echo "--- $a"
    tar -zxf "$a" .PKGINFO -O 2>/dev/null | grep -E '^(pkgname|pkgver|pkgdesc|depend|license)' || true
done
EXT="$(mktemp -d)"
trap 'rm -rf "$EXT" "$WORK" "$STAGING"' EXIT
for a in dist/fdk-shell*.apk; do
    mkdir -p "$EXT/$(basename "$a")"
    tar -zxf "$a" -C "$EXT/$(basename "$a")"
done
echo "--- extracted payloads:"
find "$EXT" -type f ! -name '.PKGINFO' ! -name '.SIGN*' | sed "s|$EXT||" | sort

# --- the installed-stack smoke ---------------------------------------------
for a in dist/fdk-shell*.apk; do
    apk add --allow-untrusted "$a"
done
if fdk_start_xvfb; then
    fdkshell_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "BUILD FAIL: fdk-shell graphical smoke test"; exit 1; }
    fdk_stop_xvfb
fi

echo "== FDK-SHELL APK BUILD OK ($VERSION, $ARCH) =="
