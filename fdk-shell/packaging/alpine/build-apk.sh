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
    # FDKSHELL_SANITIZE=0: this environment ships no sanitizer runtimes
    # (musl / minimal image) — the same posture as FDK's own alpine
    # build; the sanitizer gates run on the glibc targets.
    make -C fdk-shell FDKSHELL_SANITIZE=0 test-x11 \
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

# The signing key: FDK's own apk build (which ran earlier in this same
# container) already generated one; generate only when missing so a
# standalone run of this script also works. abuild refuses to build
# without a key, and installs use --allow-untrusted either way.
ls /home/builder/.abuild/*.rsa.pub >/dev/null 2>&1 || \
    su builder -s /bin/sh -c "abuild-keygen -a -n" </dev/null
cp /home/builder/.abuild/*.rsa.pub /etc/apk/keys/ 2>/dev/null || true

# Prepare the APKBUILD root-side with version + FDK floor substituted,
# then let abuild finalize the checksum as the builder, then build —
# the exact proven sequence of FDK's own build-apk.sh (abuild checksum
# writes the "<hash>  <filename>" entry; a hand-fed bare hash made
# verify die with "is missing in checksums").
sed -e "s|@VERSION@|$PACKVER|g" -e "s|@MINVER@|$MINVER|g" \
    fdk-shell/packaging/alpine/APKBUILD > "$WORK/APKBUILD"
chown -R builder:builder "$WORK"
su builder -s /bin/sh -c "cd '$WORK' && abuild checksum" </dev/null
chown builder:builder "$WORK/APKBUILD"
su builder -s /bin/sh -c "cd '$WORK' && abuild -r" </dev/null

# --- collect the artifacts ------------------------------------------------
# abuild writes to REPODEST (/home/builder/packages/<repo>/...), never
# to $WORK. FDK's own apks live there too — filter by name.
APKS="$(find /home/builder/packages -name 'fdk-shell*.apk' -type f 2>/dev/null || true)"
[ -n "$APKS" ] || { echo "BUILD FAIL: abuild produced no packages"; find /home/builder/packages -type f 2>/dev/null | head; exit 1; }
mkdir -p dist
for a in $APKS; do
    # Same presentation convention as FDK's apks: the arch suffix makes
    # the release page self-describing (apk installs by content; the
    # filename is presentation only).
    base="$(basename "$a" .apk)"
    case "$base" in
        *-$ARCH) cp "$a" "dist/$base.apk" ;;
        *)       cp "$a" "dist/$base-$ARCH.apk" ;;
    esac
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
